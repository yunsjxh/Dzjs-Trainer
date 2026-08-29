#include <Windows.h>

#include <iostream>

#include "..\JiYuTrainer\DriverSignaturePolicy.h"

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) {
        std::wcerr << L"usage: DriverSignaturePolicyProbe.exe <driver.sys>\n";
        return 2;
    }
    wchar_t publisher[256] = {};
    LONG trustStatus = E_FAIL;
    BOOL allowed = VerifyAllowedDriverSignature(
        argv[1], publisher, _countof(publisher), &trustStatus);
    std::wcout << L"allowed=" << (allowed ? L"true" : L"false")
               << L" trust=0x" << std::hex << static_cast<unsigned long>(trustStatus)
               << L" publisher=" << (publisher[0] != L'\0' ? publisher : L"<none>")
               << L"\n";
    return allowed ? 0 : 10;
}
