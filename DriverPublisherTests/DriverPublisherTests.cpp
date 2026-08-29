#include <cwchar>
#include <iostream>

#include "..\JiYuTrainer\DriverPublisherAllowlist.h"

int main()
{
    struct TestCase {
        const wchar_t* publisher;
        bool expected;
    };
    const TestCase tests[] = {
        { L"Microsoft Corporation", true },
        { L"microsoft corporation", true },
        { L"VMware, Inc", true },
        { L"VMware, Inc.", true },
        { L"vMware, iNc.", true },
        { L"JiYuTrainerDriver-Yunsjxh", true },
        { L"JiYuTrainerAvDriver-Yunsjxh", true },
        { L"\u56db\u5ddd\u8fc5\u6e38\u7f51\u7edc\u79d1\u6280\u80a1\u4efd\u6709\u9650\u516c\u53f8", true },
        { L"\u5317\u4eac\u84dd\u53e0\u79d1\u6280\u6709\u9650\u516c\u53f8", true },
        { L"Microsoft", false },
        { L"Microsoft Corporation ", false },
        { L"VMware, Inc..", false },
        { L"VMware, Inc ", false },
        { L"VMware Inc.", false },
        { L"Unknown Publisher", false },
        { L"", false },
        { nullptr, false }
    };

    unsigned long passed = 0UL;
    for (const TestCase& test : tests) {
        bool actual = IsAllowedDriverPublisher(test.publisher);
        if (actual != test.expected) {
            std::wcerr << L"FAIL publisher="
                       << (test.publisher == nullptr ? L"<null>" : test.publisher)
                       << L" expected=" << test.expected << L" actual=" << actual << L"\n";
            continue;
        }
        ++passed;
    }
    std::wcout << L"RESULT publisher " << passed << L"/" << _countof(tests)
               << L" passed\n";
    return passed == _countof(tests) ? 0 : 1;
}
