// dllmain.cpp : 定义 DLL 应用程序的入口点。
#include "stdafx.h"
#include "DzjsTrainerHooks.h"

HINSTANCE hInst;
DWORD WINAPI VLoadBootstrapThread(LPVOID lpThreadParameter);

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
		hInst = hModule;
		DisableThreadLibraryCalls(hModule);
		{
			HANDLE bootstrapThread = CreateThread(NULL, 0, VLoadBootstrapThread, NULL, 0, NULL);
			if (bootstrapThread)
				CloseHandle(bootstrapThread);
			else
				return FALSE;
		}
		break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
		break;
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
