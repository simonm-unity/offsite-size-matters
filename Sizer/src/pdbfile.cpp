// Executable size report utility.
// Aras Pranckevicius, http://aras-p.info/projSizer.html
// Based on code by Fabian "ryg" Giesen, http://farbrausch.com/~fg/
// Public domain.

#include "types.hpp"
#include "debuginfo.hpp"
#include "pdbfile.hpp"

#include <malloc.h>
#include <windows.h>
#include <ole2.h>

/****************************************************************************/
#include <dia2.h>
#include <cvconst.h>
#include <algorithm>

class DECLSPEC_UUID("e60afbee-502d-46ae-858f-8272a09bd707") DiaSource71;
class DECLSPEC_UUID("bce36434-2c24-499e-bf49-8bd99b0eeb68") DiaSource80;
class DECLSPEC_UUID("4C41678E-887B-4365-A09E-925D28DB33C2") DiaSource90;
class DECLSPEC_UUID("B86AE24D-BF2F-4ac9-B5A2-34B14E4CE11D") DiaSource100;
class DECLSPEC_UUID("761D3BCD-1304-41D5-94E8-EAC54E4AC172") DiaSource110;
class DECLSPEC_UUID("3bfcea48-620f-4b6b-81f7-b9af75454c7d") DiaSource120;
class DECLSPEC_UUID("e6756135-1e65-4d17-8576-610761398c3c") DiaSource140;
//class DECLSPEC_UUID("79f1bb5f-b66e-48e5-b6a9-1545c323ca3d") IDiaDataSource;


/****************************************************************************/
IDiaSession* PDBFileReader::Session;
std::vector<std::wstring> PDBFileReader::fileNames;
std::unordered_map<std::wstring, DWORD> PDBFileReader::files;
std::unordered_map<DWORD, DWORD> PDBFileReader::fileMap;


struct PDBFileReader::SectionContrib
{
  DWORD Section;
  DWORD Offset;
  DWORD Length;
  DWORD Compiland;
  sInt Type;
  sInt ObjFile;
};

const PDBFileReader::SectionContrib *PDBFileReader::ContribFromSectionOffset(sU32 sec,sU32 offs)
{
  sInt l,r,x;

  l = 0;
  r = nContribs;
  
  while(l < r)
  {
    x = (l + r) / 2;
    const SectionContrib &cur = Contribs[x];

    if(sec < cur.Section || sec == cur.Section && offs < cur.Offset)
      r = x;
    else if(sec > cur.Section || sec == cur.Section && offs >= cur.Offset + cur.Length)
      l = x+1;
    else if(sec == cur.Section && offs >= cur.Offset && offs < cur.Offset + cur.Length) // we got a winner
      return &cur;
    else
      break; // there's nothing here!
  }

  // normally, this shouldn't happen!
  return 0;
}

// helpers
static sChar *BStrToString( BSTR str, sChar *defString = "", bool stripWhitespace = false )
{
  if(!str)
  {
    sInt len = sGetStringLen(defString);
    sChar *buffer = new sChar[len+1];
    sCopyString(buffer,len+1,defString,len+1);

    return buffer;
  }
  else
  {
    sInt len = SysStringLen(str);
    sChar *buffer = new sChar[len+1];

  sInt j = 0;
    for( sInt i=0;i<len;i++ )
  {
    if( stripWhitespace && iswspace(str[i]) )
      continue;
    buffer[j] = (str[i] >= 32 && str[i] < 128) ? str[i] : '?';
    ++j;
  }

    buffer[j] = 0;

    return buffer;
  }
}

static sInt GetBStr(BSTR str,sChar *defString,DebugInfo &to)
{
  sChar *normalStr = BStrToString(str);
  sInt result = to.MakeString(normalStr);
  delete[] normalStr;

  return result;
}
typedef enum SymTagEnum TSymTagEnum;

struct DumpedSymbol
{
    struct LineNumber
    {
        LineNumber(IDiaLineNumber* p)
        {
            length = -1;  p->get_length(&length);
            lineStart = -1; p->get_lineNumber(&lineStart);
            lineEnd = -1; p->get_lineNumberEnd(&lineEnd);
            IDiaSourceFile* pSource = NULL;
            p->get_sourceFile(&pSource);
            if (pSource)
            {
                BSTR fname;
                pSource->get_fileName(&fname);
                fileName = std::wstring(fname);
            }

            rva = section = offset = -1;
            p->get_relativeVirtualAddress(&rva);
            p->get_addressSection(&section);
            p->get_addressOffset(&offset);
        }
        DWORD rva;
        DWORD length;
        std::wstring fileName;
        DWORD lineStart;
        DWORD lineEnd;
        DWORD section;
        DWORD offset;
    };

    DumpedSymbol(IDiaSymbol* pSymbol)
    {
        BSTR n = 0;
        pSymbol->get_name(&n); if (n) name = std::wstring(n);

        DWORD t = 0;
        pSymbol->get_symTag(&t); symtag = (TSymTagEnum)t;

        pSymbol->get_length(&length);

        rva = section = offset = -1;
        pSymbol->get_relativeVirtualAddress(&rva);
        pSymbol->get_addressSection(&section);
        pSymbol->get_addressOffset(&offset);

        {
            IDiaEnumSymbols *enumChildren;
            if (SUCCEEDED(pSymbol->findChildren(SymTagNull, NULL, 0, &enumChildren)))
            {
                IDiaSymbol *pChild;
                ULONG celtChildren = 0;
                while (SUCCEEDED(enumChildren->Next(1, &pChild, &celtChildren)) && (celtChildren == 1))
                {
                    children.emplace_back(pChild);
                    pChild->Release();
                }
            }
        }

        {
            IDiaEnumLineNumbers* pLines = NULL;
            if (SUCCEEDED(pSymbol->findInlineeLines(&pLines)))
            {
                IDiaLineNumber *pLine;
                ULONG celtLines = 0;
                while (SUCCEEDED(pLines->Next(1, &pLine, &celtLines)) && (celtLines == 1))
                {
                    lines.emplace_back(pLine);
                    pLine->Release();
                }
            }
        }
        //{
        //    IDiaEnumSymbols *enumChildren;
        //    for (int i = 0; i < length;++i)
        //    if (SUCCEEDED(pSymbol->findInlineFramesByRVA(rva+i, &enumChildren)))
        //    {
        //        IDiaSymbol *pChild;
        //        ULONG celtChildren = 0;
        //        while (SUCCEEDED(enumChildren->Next(1, &pChild, &celtChildren)) && (celtChildren == 1))
        //        {
        //            inlineFrames.emplace_back(pChild);
        //            pChild->Release();
        //        }
        //    }
        //}
    }

    DWORD rva;
    ULONGLONG length;
    std::wstring name;
    TSymTagEnum symtag;
    DWORD section;
    DWORD offset;
    std::vector<DumpedSymbol> children;
    std::vector<LineNumber> lines;
    //std::vector<DumpedSymbol> inlineFrames;
};


struct FunctionLineInfo
{
	void HandleChildren(IDiaSymbol* pSymbol)
	{
		{
			IDiaEnumSymbols *enumChildren;
			if (SUCCEEDED(pSymbol->findChildren(SymTagInlineSite, NULL, 0, &enumChildren)))
			{
				IDiaSymbol *pChild;
				ULONG celtChildren = 0;
				while (SUCCEEDED(enumChildren->Next(1, &pChild, &celtChildren)) && (celtChildren == 1))
				{
					HandleChild(pChild);
					pChild->Release();
				}
			}
		}
	}

	void HandleChild(IDiaSymbol* pSymbol)
	{
		BSTR pFuncName;
		pSymbol->get_name(&pFuncName);
		int funcNameId = PDBFileReader::getFunctionNameId(pFuncName);

		IDiaEnumLineNumbers* pLines = NULL;
		if (SUCCEEDED(pSymbol->findInlineeLines(&pLines)))
		{
			if (bytes.empty())
			{
				ByteInfo no = { -1, 0, -1 };
				bytes.resize(function_length, no);
			}
			IDiaLineNumber *pLine;
			ULONG celtLines = 0;
			while (SUCCEEDED(pLines->Next(1, &pLine, &celtLines)) && (celtLines == 1))
			{
				DWORD length = -1;  pLine->get_length(&length);
				DWORD rva = -1; pLine->get_relativeVirtualAddress(&rva);
				rva -= function_rva;
				DWORD end_rva = rva + length;

				DWORD lineStart = -1; pLine->get_lineNumber(&lineStart);
				DWORD fileId = -1;  pLine->get_sourceFileId(&fileId);
				fileId = PDBFileReader::getFileNameId(fileId);

				for (int a = rva; a < end_rva; ++a)
				{
					bytes[a].fileId = fileId;
					bytes[a].line = lineStart;
					bytes[a].funcNameId = funcNameId;
				}
				pLine->Release();
			}
			HandleChildren(pSymbol);
		}
	}

	FunctionLineInfo(IDiaSymbol* pSymbol)
	{
		function_length = -1;  pSymbol->get_length(&function_length);
		pSymbol->get_relativeVirtualAddress(&function_rva);
		HandleChildren(pSymbol);
	}


	struct ByteInfo
	{
		int line;
		DWORD fileId;
		int funcNameId;
	};
	std::vector<ByteInfo> bytes;
	std::vector<std::string> files;
	DWORD function_rva;
	ULONGLONG function_length;
};
void PDBFileReader::ProcessSymbol(IDiaSymbol *symbol,DebugInfo &to)
{
  DWORD section,offset,rva;
  enum SymTagEnum tag;
  ULONGLONG length = 0;
  BSTR name = 0, undName = 0, srcFileName = 0;

  symbol->get_symTag((DWORD *) &tag);
  if (tag != SymTagFunction)
	  return;
  symbol->get_relativeVirtualAddress(&rva);
  symbol->get_length(&length);
  symbol->get_addressSection(&section);
  symbol->get_addressOffset(&offset);

  if (addressMap.count(rva))
  {
	  return;
  }
  else
  {
	  addressMap[rva] = length;
  }

  FunctionLineInfo sym(symbol);


  if(!sym.bytes.empty()) for (int i = 0; i < sym.function_length; ++i)
  {
	  const FunctionLineInfo::ByteInfo& b = sym.bytes[i];
	  auto p = std::make_pair(b.line, b.fileId);
	  sourceLocToBytes[p]++;
	  int f = b.funcNameId;
	  if (f != -1)
		  funcToBytes[f]++;
  }

  // get length from type for data
  if( tag == SymTagData )
  {
    IDiaSymbol *type = NULL;
    if( symbol->get_type(&type) == S_OK ) // no SUCCEEDED test as may return S_FALSE!
    {
      if( FAILED(type->get_length(&length)) )
	length = 0;
      type->Release();
    }
    else
      length = 0;
  }

  const SectionContrib *contrib = ContribFromSectionOffset(section,offset);
  sInt objFile = 0;
  sInt sectionType = DIC_UNKNOWN;

  if(contrib)
  {
    objFile = contrib->ObjFile;
    sectionType = contrib->Type;
  }

  symbol->get_name(&name);
  symbol->get_undecoratedName(&undName);

  //if ((name != NULL) && (std::wstring(name).find(L".text") != std::wstring::npos))
  //{
  // DumpedSymbol sym(symbol);
  // symbol->get_name(&name);
  //}  
  //if ((name != NULL) && (std::wstring(name).find(L"foo") != std::wstring::npos))
  //{
	 // DumpedSymbol sym(symbol);
	 // symbol->get_name(&name);
  //}

  if (false)//((name != NULL) && (std::wstring(name).find(L"foo") != std::wstring::npos))
  {
      DumpedSymbol sym(symbol);

      symbol->get_name(&name);

      //IDiaEnumSymbols *enumChildren;
      //if (SUCCEEDED(symbol->findChildren(SymTagNull, NULL, 0, &enumChildren)))
      //{
      //    LONG count = -1;
      //    enumChildren->get_Count(&count);
      //    IDiaSymbol *pChild;
      //    ULONG celtChildren = 0;
      //    while (SUCCEEDED(enumChildren->Next(1, &pChild, &celtChildren)) && (celtChildren == 1) )
      //    {
      //        //ProcessSymbol(pSymbol, to);
      //        BSTR cname = 0;
      //        DWORD ctag;//SymTagEnum
      //        DWORD ckind;//DataKind
      //        pChild->get_name(&cname);
      //        pChild->get_symTag(&ctag);
      //        pChild->get_dataKind(&ckind);
      //        
      //        pChild->Release();
      //    }
      //}

      //IDiaEnumSymbols *enumChildren;
      //if (SUCCEEDED(symbol->findChildren(SymTagInlineSite, NULL, 0, &enumChildren)))
      //{
      //    IDiaSymbol *pChild;
      //    ULONG celtChildren = 0;

      //    while (SUCCEEDED(enumChildren->Next(1, &pChild, &celtChildren)) && (celtChildren == 1))
      //    {
      //        BSTR cname = 0;
      //        ULONGLONG clength;
      //        pChild->get_name(&cname);
      //        pChild->get_length(&clength);
      //        pChild->Release();
      //    }

      //}

  }

  // fill out structure
  sChar *nameStr = BStrToString(name, "<noname>", true);
  sChar *undNameStr = BStrToString(undName, nameStr, false);

  to.Symbols.push_back( DISymbol() );
  DISymbol *outSym = &to.Symbols.back();
  outSym->mangledName = to.MakeString(nameStr);
  outSym->name = to.MakeString(undNameStr);
  outSym->objFileNum = objFile;
  outSym->VA = rva;
  outSym->Size = (sU32) length;
  outSym->Class = sectionType;
  outSym->NameSpNum = to.GetNameSpaceByName(nameStr);

  // clean up
  delete[] nameStr;
  if(name)         SysFreeString(name);
  if(undName)      SysFreeString(undName);
}

void PDBFileReader::ReadEverything(DebugInfo &to)
{
  ULONG celt;
  
  Contribs = 0;
  nContribs = 0;
  
  // read section table
  IDiaEnumTables *enumTables;
  if(Session->getEnumTables(&enumTables) == S_OK)
  {
    VARIANT vIndex;
    vIndex.vt = VT_BSTR;
    vIndex.bstrVal = SysAllocString(L"Sections");

    IDiaTable *secTable;
    if(enumTables->Item(vIndex,&secTable) == S_OK)
    {
      LONG count;

      secTable->get_Count(&count);
      Contribs = new SectionContrib[count];
      nContribs = 0;

      IDiaSectionContrib *item;
      while(SUCCEEDED(secTable->Next(1,(IUnknown **)&item,&celt)) && celt == 1)
      {
        SectionContrib &contrib = Contribs[nContribs++];

        item->get_addressOffset(&contrib.Offset);
        item->get_addressSection(&contrib.Section);
        item->get_length(&contrib.Length);
        item->get_compilandId(&contrib.Compiland);

	BOOL code=FALSE,initData=FALSE,uninitData=FALSE;
	item->get_code(&code);
	item->get_initializedData(&initData);
	item->get_uninitializedData(&uninitData);

	if(code && !initData && !uninitData)
	  contrib.Type = DIC_CODE;
	else if(!code && initData && !uninitData)
	  contrib.Type = DIC_DATA;
	else if(!code && !initData && uninitData)
	  contrib.Type = DIC_BSS;
	else
	  contrib.Type = DIC_UNKNOWN;

	BSTR objFileName = 0;
	
	IDiaSymbol *compiland = 0;
	item->get_compiland(&compiland);
	if(compiland)
	{
	  compiland->get_name(&objFileName);
	  compiland->Release();
	}

	sChar *objFileStr = BStrToString(objFileName,"<noobjfile>");
	contrib.ObjFile = to.GetFileByName(objFileStr);

	delete[] objFileStr;
	if(objFileName)
	  SysFreeString(objFileName);

        item->Release();
      }

      secTable->Release();
    }

    SysFreeString(vIndex.bstrVal);
    enumTables->Release();
  }

  /*
  // Note: this was the original code; that was however extremely slow especially on larger or 64 bit binaries.
  // New code that replaces it (however it does not produce 100% identical results) is below.

  // enumerate symbols by (virtual) address
  IDiaEnumSymbolsByAddr *enumByAddr;
  if(SUCCEEDED(Session->getSymbolsByAddr(&enumByAddr)))
  {
    IDiaSymbol *symbol;
    // get first symbol to get first RVA (argh)
    if(SUCCEEDED(enumByAddr->symbolByAddr(1,0,&symbol)))
    {
      DWORD rva;
      if(symbol->get_relativeVirtualAddress(&rva) == S_OK)
      {
        symbol->Release();

        // now, enumerate by rva.
        if(SUCCEEDED(enumByAddr->symbolByRVA(rva,&symbol)))
        {
          do
          {
            ProcessSymbol(symbol,to);
            symbol->Release();

            if(FAILED(enumByAddr->Next(1,&symbol,&celt)))
              break;
          }
          while(celt == 1);
        }
      }
      else
        symbol->Release();
    }

    enumByAddr->Release();
  }
  */

  // This is new code that replaces commented out code above. On one not-too-big executable this gets Sizer execution time from
  // 448 seconds down to 1.5 seconds. However it does not list some symbols that are "weird" and are likely due to linker padding
  // or somesuch; I did not dig in. On that particular executable, e.g. 128 kb that is coming from "* Linker *" file is gone.
  IDiaSymbol* globalSymbol = NULL;
  if (SUCCEEDED(Session->get_globalScope(&globalSymbol)))
  {
    // Retrieve the compilands first
    IDiaEnumSymbols *enumSymbols;
    if (SUCCEEDED(globalSymbol->findChildren(SymTagCompiland, NULL, 0, &enumSymbols)))
    {
        LONG compilandCount = 0;
        enumSymbols->get_Count(&compilandCount);
        if (compilandCount == 0)
            compilandCount = 1;
        LONG processedCount = 0;
        IDiaSymbol *compiland;
        fprintf(stderr, "[      ]");
        while (SUCCEEDED(enumSymbols->Next(1, &compiland, &celt)) && (celt == 1))
        {
            ++processedCount;
            fprintf(stderr, "\b\b\b\b\b\b\b\b[%5.1f%%]", processedCount*100.0/compilandCount);
            // Find all the symbols defined in this compiland and treat their info
            IDiaEnumSymbols *enumChildren;
            if (SUCCEEDED(compiland->findChildren(SymTagNull, NULL, 0, &enumChildren)))
            {
                IDiaSymbol *pSymbol;
                ULONG celtChildren = 0;
                while (SUCCEEDED(enumChildren->Next(1, &pSymbol, &celtChildren)) && (celtChildren == 1))
                {
                    ProcessSymbol(pSymbol, to);
                    pSymbol->Release();
                }
                enumChildren->Release();
            }
            compiland->Release();
        }
        enumSymbols->Release();
    }
    globalSymbol->Release();
  }

  // clean up
  delete[] Contribs;
}

/****************************************************************************/

sBool PDBFileReader::ReadDebugInfo(sChar *fileName,DebugInfo &to)
{
  static const struct DLLDesc
  {
    const char *Filename;
    IID UseCLSID;
  } DLLs[] = {
      "msdia140.dll", __uuidof(DiaSource140), // VS 2015
      "msdia120.dll", __uuidof(DiaSource120), // VS 2013
      "msdia71.dll", __uuidof(DiaSource71),
    "msdia80.dll", __uuidof(DiaSource80),
    "msdia90.dll", __uuidof(DiaSource90),
    "msdia100.dll", __uuidof(DiaSource100), // VS 2010
    "msdia110.dll", __uuidof(DiaSource110), // VS 2012
    // add more here as new versions appear (as long as they're backwards-compatible)
    0
  };

  sBool readOk = false;

  if(FAILED(CoInitialize(0)))
  {
    fprintf(stderr, "  failed to initialize COM\n");
    return false;
  }

  IDiaDataSource *source = 0;
  HRESULT hr = E_FAIL;

  // Try creating things "the official way"
  for(sInt i=0;DLLs[i].Filename;i++)
  {
    hr = CoCreateInstance(DLLs[i].UseCLSID,0,CLSCTX_INPROC_SERVER,
      __uuidof(IDiaDataSource),(void**) &source);

    if(SUCCEEDED(hr))
      break;
  }

  if(FAILED(hr))
  {
    // None of the classes are registered, but most programmers will have the
    // DLLs on their system anyway and can copy it over; try loading it directly.

    for(sInt i=0;DLLs[i].Filename;i++)
    {
      HMODULE hDIADll = LoadLibrary(DLLs[i].Filename);
      if(hDIADll)
      {
	typedef HRESULT (__stdcall *PDllGetClassObject)(REFCLSID rclsid,REFIID riid,void** ppvObj);
	PDllGetClassObject DllGetClassObject = (PDllGetClassObject) GetProcAddress(hDIADll,"DllGetClassObject");
	if(DllGetClassObject)
	{
	  // first create a class factory
          IClassFactory *classFactory;
	  hr = DllGetClassObject(DLLs[i].UseCLSID,IID_IClassFactory,(void**) &classFactory);
	  if(SUCCEEDED(hr))
	  {
	    hr = classFactory->CreateInstance(0,__uuidof(IDiaDataSource),(void**) &source);
	    classFactory->Release();
	  }
	}

	if(SUCCEEDED(hr))
	  break;
	else
	  FreeLibrary(hDIADll);
      }
    }
  }

  if(source)
  {
    wchar_t wideFileName[260];
    mbstowcs(wideFileName,fileName,260);
    if(SUCCEEDED(source->loadDataForExe(wideFileName,0,0)))
    {
      if(SUCCEEDED(source->openSession(&Session)))
      {
        ReadEverything(to);

		////////////////////////////////////////////////////////////////////////////////////////////
		typedef std::pair<std::pair<int, DWORD>, int> ptype;
		std::vector<ptype> v;
		v.assign(sourceLocToBytes.begin(), sourceLocToBytes.end());
		std::sort(v.begin(), v.end(), [](auto a, auto b) {return a.second > b.second; });


		////////////////////////////////////////////////////////////////////////////////////////////
		std::vector<std::pair<int, int>> funcs;
		funcs.assign(funcToBytes.begin(), funcToBytes.end());
		std::sort(funcs.begin(), funcs.end(), [](auto a, auto b) {return a.second > b.second; });

		////////////////////////////////////////////////////////////////////////////////////////////
		if(false)
		{
			int ndump = v.size();
			for (int i = 0; i < ndump; ++i)
			{
				auto& e = v[i];
				//if (e.first.first == 274)
				//{
				//	printf("wtf\n");
				//}

				if (e.first.first == -1) continue;
				printf("%S(%d): %d\n", fileNames[e.first.second].c_str(), e.first.first, e.second);
			}
		}
		else
		{
			int ndump = funcs.size();
			for (int i = 0; i < ndump; ++i)
			{
				auto& e = funcs[i];
				printf("%S : %d\n", funcNames[e.first].c_str(), e.second);
			}
		}

		exit(0);
        readOk = true;
        Session->Release();
      }
      else
	fprintf(stderr,"  failed to open DIA session\n");
    }
    else
      fprintf(stderr,"  failed to load debug symbols (PDB not found)\n");

    source->Release();
  }
  else
    fprintf(stderr,"  couldn't find (or properly initialize) any DIA dll, copying msdia*.dll to app dir might help.\n");

  CoUninitialize();

  return readOk;
}

DWORD PDBFileReader::getFileNameId(DWORD in)
{
	if (fileMap.count(in))
	{
		return fileMap[in];
	}

	IDiaSourceFile* pFile;
	Session->findFileById(in, &pFile);
	BSTR pFileName;
	pFile->get_fileName(&pFileName);
	pFile->Release();
	std::wstring fileName(pFileName);

	auto it = files.find(fileName);
	if (it != files.end())
	{
		fileMap[in] = it->second;
		return it->second;
	}

	fileNames.push_back(fileName);
	return fileMap[in] = files[fileName] = fileNames.size() - 1;
}

std::vector<std::wstring> PDBFileReader::funcNames;
std::unordered_map<std::wstring, int> PDBFileReader::funcIndexByName;

int PDBFileReader::getFunctionNameId(const std::wstring& funcName)
{
	auto p = funcIndexByName.insert(std::make_pair(funcName, funcNames.size()));
	if (p.second)
	{
		funcNames.push_back(funcName);
	}
	return p.first->second;
}

/****************************************************************************/
