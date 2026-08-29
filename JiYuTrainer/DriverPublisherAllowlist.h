#pragma once

#include <cwchar>

static inline bool IsJiYuDriverPublisher(const wchar_t* publisher)
{
    return publisher != nullptr &&
        (_wcsicmp(publisher, L"JiYuTrainerDriver-Yunsjxh") == 0 ||
         _wcsicmp(publisher, L"JiYuTrainerAvDriver-Yunsjxh") == 0);
}

static inline bool IsAllowedDriverPublisher(const wchar_t* publisher)
{
    static const wchar_t* const publishers[] = {
        L"Qihoo 360 Software (Beijing) Company Limited",
        L"Tencent Technology(Shenzhen) Company Limited",
        L"Microsoft Windows",
        L"KingSoft",
        L"Microsoft Corporation",
        L"Microsoft Root Certificate Authority",
        L"Beijing Baidu Netcom Science and Technology Co.,Ltd",
        L"VMware, Inc",
        L"VMware, Inc.",
        L"Microsoft Windows Hardware Compatibility Publisher",
        L"Intel(R) pGFX",
        L"Logitech Inc",
        L"Sysinternals",
        L"Realtek Semiconductor Corp",
        L"LENOVO (UNITED STATES) INC.",
        L"Beijing Huorong Network Technology Co., Ltd.",
        L"Kaspersky Lab",
        L"Xiaomi Technology Inc",
        L"Cisco Systems, Inc.",
        L"Huawei Technologies Co., Ltd.",
        L"HP Inc.",
        L"Dell Inc",
        L"NVIDIA Corporation",
        L"Beijing Kingsoft Security software Co.,Ltd",
        L"Beijing Rising Network Security Technology Co., Ltd.",
        L"Beijing Jiangmin New Sci.&Tec. Co. Ltd.",
        L"Intel Corporation",
        L"Ralink Technology Corporation",
        L"Qualcomm Atheros, Inc.",
        L"Bigfoot Networks, Inc.",
        L"Mediatek Inc.",
        L"Intel(R) INTELNPG1",
        L"Beijing Qihu Technology Co., Ltd.",
        L"Shanghai 2345 Mobile Technology Co., Ltd.",
        L"Qingdao Ruanmei Network Technology Co.,Ltd.",
        L"China Merchants Bank",
        L"Agricultural Bank of China",
        L"Beijing Sogou Technology Development Co., Ltd.",
        L"BeiJing Eastern Micropoint Info-Tech CO., LTD",
        L"Apple Inc.",
        L"\u56db\u5ddd\u8fc5\u6e38\u7f51\u7edc\u79d1\u6280\u80a1\u4efd\u6709\u9650\u516c\u53f8",
        L"BattlEye Innovations e.K.",
        L"EasyAntiCheat Oy",
        L"Microsoft Windows Component Publisher",
        L"Zhuhai Kingsoft Office Software Co., Ltd.",
        L"SHENZHEN THUNDER NETWORKING TECHNOLOGIES LTD.",
        L"\u5317\u4eac\u84dd\u53e0\u79d1\u6280\u6709\u9650\u516c\u53f8",
        L"Shanghai Changzhi Network Technology Co., Ltd.",
        L"Motorola",
        L"Shenzhen Luyoudashi Technology Co., Ltd.",
        L"ALIBABA (CHINA) NETWORK TECHNOLOGY CO.,LTD.",
        L"JiYuTrainerDriver-Yunsjxh",
        L"JiYuTrainerAvDriver-Yunsjxh"
    };

    if (publisher == nullptr || publisher[0] == L'\0') return false;
    for (const wchar_t* allowed : publishers) {
        if (_wcsicmp(publisher, allowed) == 0) return true;
    }
    return false;
}
