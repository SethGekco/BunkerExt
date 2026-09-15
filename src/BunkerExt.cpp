#include "BunkerExt.h"

#include <Phobos.h>
#include <Syringe.h>

HANDLE BunkerExtDLL::hInstance = nullptr;

char BunkerExtDLL::readBuffer[BunkerExtDLL::readLength];
wchar_t BunkerExtDLL::wideBuffer[BunkerExtDLL::readLength];

// No static patches and no INI parsing yet, so no ExeRun/CmdLine init hooks:
// DEFINE_HOOK sites are registered by Syringe from the DLL's export table and
// need no runtime initialisation. This deliberately keeps us off the contested
// init addresses (0x7CD810, 0x52F639) that five other frameworks fight over.
bool __stdcall DllMain(HANDLE hInstance, DWORD dwReason, LPVOID)
{
	if (dwReason == DLL_PROCESS_ATTACH)
	{
		BunkerExtDLL::hInstance = hInstance;
		Phobos::hInstance = hInstance;
	}
	return true;
}

SYRINGE_HANDSHAKE(pInfo)
{
	pInfo->Message = const_cast<char*>("BunkerExt");
	return S_OK;
}
