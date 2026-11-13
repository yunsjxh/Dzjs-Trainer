#pragma once

#include <windows.h>

// 主程序与更新器子进程之间的进度通道。
//
// 下载只能由子进程做（主程序进程里 Schannel/CNG 起不来），但进度条在主程序界面里。
// 跨进程 PostMessage 不能带指针，所以 total / done 直接塞 wParam / lParam：
//   wParam = total 字节数（0 表示长度未知，界面画不确定态）
//   lParam = done  字节数
// 两个进程必须用同一个消息号，常量只放这里，别在别处复制一份。
#define WM_JUP_DOWNLOAD_PROGRESS (WM_APP + 46)
