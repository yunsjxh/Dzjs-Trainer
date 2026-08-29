#pragma once
#include "stdafx.h"

//加载驱动
//    lpszDriverName：驱动的服务名
//    driverPath：驱动的完整路径
//    lpszDisplayName：nullptr
BOOL MLoadKernelDriver(const wchar_t* lpszDriverName, const wchar_t* driverPath, const wchar_t* lpszDisplayName);
//卸载驱动
//    szSvrName：服务名
BOOL MUnLoadKernelDriver(const wchar_t* szSvrName);
BOOL MUnLoadDriverServiceWithMessage(const wchar_t * szSvrName);
//打开驱动
BOOL XOpenDriver(BOOL startGuard);
//返回驱动是否加载
BOOL XDriverLoaded();
BOOL XTryReuseInstalledDriver();

BOOL XTestDriverCanUse();

// Randomized main-driver service name (date-based, generated independently
// from the driver file name; see GenerateDateBasedServiceName in App.cpp)
LPCWSTR XGetDriverServiceName();
// Randomized AV kernel-driver service name (date-based, algorithm differs
// from the main driver's; see GenerateDateBasedAvServiceName in App.cpp)
LPCWSTR XGetAvDriverServiceName();

BOOL XInitSelfProtect();

BOOL XLoadDriver();

BOOL XCloseDriverHandle();

BOOL XUnLoadDriver();
