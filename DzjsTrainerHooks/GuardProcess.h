#pragma once
// GuardProcess: 该域的 detour 实现。共享状态见 HooksCommon.h，安装顺序见 HookRegistry.cpp。
#include "HooksCommon.h"

BOOL WINAPI hkDeviceIoControl(HANDLE hDevice, DWORD dwIoControlCode, LPVOID lpInBuffer, DWORD nInBufferSize, LPVOID lpOutBuffer, DWORD nOutBufferSize, LPDWORD lpBytesReturned,  LPOVERLAPPED lpOverlapped);
HANDLE WINAPI hkCreateFileA(LPCSTR lpFileName,  DWORD dwDesiredAccess, DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile );
HANDLE WINAPI hkCreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess,  DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);
BOOL WINAPI hkExitWindowsEx(UINT uFlags, DWORD dwReason);
HINSTANCE WINAPI hkShellExecuteW(HWND hwnd, LPCWSTR lpOperation, LPCWSTR lpFile, LPCWSTR lpParameters, LPCWSTR lpDirectory, INT nShowCmd);
BOOL WINAPI hkShellExecuteExW(SHELLEXECUTEINFOW *pExecInfo);
BOOL VInjectHookIntoProcess(HANDLE process);
BOOL WINAPI hkCreateProcessW(LPCWSTR lpApplicationName, LPWSTR lpCommandLine, LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes, BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory, LPSTARTUPINFOW lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation);
BOOL WINAPI hkCreateProcessA(LPCSTR lpApplicationName, LPSTR lpCommandLine, LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes, BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment, LPCSTR lpCurrentDirectory, LPSTARTUPINFOA lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation);
UINT WINAPI hkWinExec(LPCSTR lpCmdLine, UINT uCmdShow);
BOOL WINAPI hkTerminateProcess(HANDLE hProcess, UINT uExitCode);
HRESULT WINAPI hkFilterConnectCommunicationPort(LPCWSTR lpPortName, DWORD dwOptions, LPCVOID lpContext, WORD wSizeOfContext, LPSECURITY_ATTRIBUTES lpSecurityAttributes, HANDLE * hPort);
BOOL WINAPI hkPostMessageW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
LRESULT WINAPI hkSendMessageW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);

namespace GuardProcess {
    constexpr HookDomain Domain = HookDomain::Process;
    int HookCount();   // 已安装的钩子数，由 HookRegistry.cpp 按域统计
}
