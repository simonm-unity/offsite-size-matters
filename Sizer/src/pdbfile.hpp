// Executable size report utility.
// Aras Pranckevicius, http://aras-p.info/projSizer.html
// Based on code by Fabian "ryg" Giesen, http://farbrausch.com/~fg/
// Public domain.

#ifndef __PDBFILE_HPP_
#define __PDBFILE_HPP_
#include <windows.h>
#include "debuginfo.hpp"
#include <unordered_map>
/****************************************************************************/

struct IDiaSession;

class PDBFileReader : public DebugInfoReader
{
	struct SectionContrib;

	SectionContrib *Contribs;
	sInt nContribs;

	static IDiaSession *Session;

	const SectionContrib *ContribFromSectionOffset(sU32 section, sU32 offset);
	void ProcessSymbol(struct IDiaSymbol *symbol, DebugInfo &to);
	void ReadEverything(DebugInfo &to);

public:
	sBool ReadDebugInfo(sChar *fileName, DebugInfo &to);
	std::unordered_map<size_t, size_t> addressMap;
	std::map<std::pair<int, DWORD>, int> sourceLocToBytes;
	std::unordered_map<int, int> funcToBytes;

	static std::vector<std::wstring> fileNames;
	static std::unordered_map<std::wstring, DWORD> files;
	static std::unordered_map<DWORD, DWORD> fileMap;
	
	static std::vector<std::wstring> funcNames;
	static std::unordered_map<std::wstring, int> funcIndexByName;

	static DWORD getFileNameId(DWORD in);
	static int getFunctionNameId(const std::wstring& funcName);

};

/****************************************************************************/

#endif