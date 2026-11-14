#include "KernelUtils.h"
#include "KernelUtils.h"
#include "stdafx.h"
#include "DzjsTrainer.h"
#include "AppPublic.h"
#include "resource.h"
#include "DriverLoader.h"
#include "KernelUtils.h"
#include "Logger.h"
#include "NtHlp.h"
#include <winioctl.h>
#include <bcrypt.h>
#include <vector>
#include "../DzjsTrainerDriver/IoCtl.h"
#include "../DzjsTrainerDriver/IoStructs.h"

extern HANDLE hKDrv;
extern JTApp * currentApp;
extern LoggerInternal * currentLogger;

namespace {

constexpr DWORD kEncryptedTokenMagic = 0x314E4554UL; // "TEN1"
constexpr DWORD kEncryptedTokenVersion = 1UL;
constexpr DWORD kEncryptedTokenHeaderSize = 28UL;
constexpr BYTE kTokenKeyPartA[32] = {
    0xD9, 0x13, 0x89, 0x2C, 0x4C, 0x78, 0x7D, 0x94,
    0x3C, 0xCA, 0x79, 0x7B, 0x61, 0x03, 0xA4, 0x21,
    0xCF, 0x35, 0x39, 0x70, 0x81, 0xBA, 0xA9, 0x9A,
    0x6E, 0x45, 0xD1, 0xD8, 0x35, 0xDD, 0x4F, 0x9F
};
constexpr BYTE kTokenKeyPartB[32] = {
    0x72, 0xB0, 0x54, 0x31, 0xC6, 0x5E, 0xA7, 0xE5,
    0x59, 0x69, 0x2C, 0xFB, 0x06, 0x62, 0x0C, 0xFA,
    0x26, 0x5B, 0x30, 0x41, 0x89, 0x05, 0xEF, 0x14,
    0xD0, 0x0F, 0x84, 0x69, 0xA3, 0x0C, 0x64, 0x8C
};

static BOOL DecryptEmbeddedUnloadToken(
    const BYTE* encrypted,
    DWORD encryptedSize,
    JDRV_UNLOAD_TOKEN* token)
{
    if (!encrypted || !token || encryptedSize < kEncryptedTokenHeaderSize) return FALSE;

    DWORD magic = 0;
    DWORD version = 0;
    DWORD cipherSize = 0;
    memcpy(&magic, encrypted, sizeof(magic));
    memcpy(&version, encrypted + sizeof(magic), sizeof(version));
    memcpy(&cipherSize, encrypted + 24, sizeof(cipherSize));
    if (magic != kEncryptedTokenMagic || version != kEncryptedTokenVersion ||
        cipherSize == 0 || encryptedSize != kEncryptedTokenHeaderSize + cipherSize ||
        (cipherSize % 16UL) != 0) return FALSE;

    BYTE key[32] = {};
    for (size_t index = 0; index < _countof(key); ++index)
        key[index] = static_cast<BYTE>(kTokenKeyPartA[index] ^ kTokenKeyPartB[index]);
    BYTE iv[16] = {};
    memcpy(iv, encrypted + 8, sizeof(iv));

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE keyHandle = nullptr;
    DWORD objectLength = 0;
    DWORD resultLength = 0;
    DWORD plainLength = 0;
    std::vector<BYTE> keyObject;
    std::vector<BYTE> plain;
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0);
    if (status == 0) status = BCryptSetProperty(
        algorithm, BCRYPT_CHAINING_MODE,
        reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
        sizeof(BCRYPT_CHAIN_MODE_CBC), 0);
    if (status == 0) status = BCryptGetProperty(
        algorithm, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
        &resultLength, 0);
    if (status == 0) {
        keyObject.resize(objectLength);
        status = BCryptGenerateSymmetricKey(
            algorithm, &keyHandle, keyObject.data(), objectLength,
            key, sizeof(key), 0);
    }
    if (status == 0) status = BCryptDecrypt(
        keyHandle,
        const_cast<PUCHAR>(encrypted + kEncryptedTokenHeaderSize), cipherSize,
        nullptr, iv, sizeof(iv), nullptr, 0, &plainLength,
        BCRYPT_BLOCK_PADDING);
    if (status == 0 && plainLength == sizeof(*token)) {
        plain.resize(plainLength);
        status = BCryptDecrypt(
            keyHandle,
            const_cast<PUCHAR>(encrypted + kEncryptedTokenHeaderSize), cipherSize,
            nullptr, iv, sizeof(iv), plain.data(), plainLength, &plainLength,
            BCRYPT_BLOCK_PADDING);
    }
    if (status == 0 && plainLength == sizeof(*token))
        memcpy(token, plain.data(), sizeof(*token));

    if (keyHandle) BCryptDestroyKey(keyHandle);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    SecureZeroMemory(key, sizeof(key));
    SecureZeroMemory(iv, sizeof(iv));
    if (!plain.empty()) SecureZeroMemory(plain.data(), plain.size());
    return status == 0 && plainLength == sizeof(*token);
}

}

static BOOL LoadEmbeddedUnloadToken(JDRV_UNLOAD_TOKEN* token)
{
	if (!token || !currentApp) return FALSE;
	HINSTANCE module = currentApp->GetInstance();
	HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_DRIVER_UNLOCK_TOKEN), L"BIN");
	if (!resource) return FALSE;
	HGLOBAL loaded = LoadResource(module, resource);
	DWORD size = SizeofResource(module, resource);
	if (!loaded || !size) return FALSE;
	BYTE* raw = static_cast<BYTE*>(LockResource(loaded));
	if (!raw) return FALSE;
	return DecryptEmbeddedUnloadToken(raw, size, token);
}

BOOL KFShutdown()
{
	if (XDriverLoaded())
	{
		DWORD ReturnLength = 0;
		BOOL rs = DeviceIoControl(hKDrv, CTL_SHUTDOWN, NULL, 0, NULL, 0, &ReturnLength, NULL);
		currentLogger->LogInfo(L"DeviceIoControl CTL_SHUTDOWN %s", rs ? L"TRUE" : L"FALSE");
		if (!rs) currentLogger->LogError(L"DeviceIoControl CTL_SHUTDOWN %d", GetLastError());

		return rs;
	}
	else currentLogger->LogWarn(L"驱动未加载！");
	return false;
}
BOOL KFReboot()
{
	if (XDriverLoaded())
	{
		DWORD ReturnLength = 0;
		BOOL rs = DeviceIoControl(hKDrv, CTL_REBOOT, NULL, 0, NULL, 0, &ReturnLength, NULL);
		currentLogger->LogInfo(L"DeviceIoControl CTL_REBOOT %s", rs ? L"TRUE" : L"FALSE");
		if(!rs) currentLogger->LogError(L"DeviceIoControl CTL_REBOOT %d",GetLastError());

		return rs;
	}
	else currentLogger->LogWarn(L"驱动未加载！");
	return false;
}
BOOL KForceKill(DWORD pid, NTSTATUS *pStatus)
{
	if (XDriverLoaded())
	{
		JDRV_PROCESS_REQUEST request = { 0 };
		JDRV_OPERATION_RESPONSE response = { 0 };
		DWORD ReturnLength = 0;
		response.status = STATUS_UNSUCCESSFUL;
		request.size = sizeof(request);
		request.version = JDRV_PROTOCOL_VERSION;
		request.processId = pid;
		if (DeviceIoControl(
			hKDrv,
			CTL_KILL_PROCESS,
			&request,
			sizeof(request),
			&response,
			sizeof(response),
			&ReturnLength,
			NULL))
		{
			if (ReturnLength >= sizeof(response) && response.status == STATUS_SUCCESS)
				return TRUE;
			else currentLogger->LogError(L"CTL_KILL_PROCESS 错误：0x%08X", response.status);
		}
		else currentLogger->LogError(L"DeviceIoControl CTL_KILL_PROCESS 错误：%d", GetLastError());
		if (pStatus)*pStatus = (NTSTATUS)response.status;
	}
	else currentLogger->LogWarn(L"驱动未加载！");
	return false;
}
BOOL KFSendDriverinitParam(bool isXp, bool isWin7, ULONG sysBulidVer) {
	if (XDriverLoaded())
	{
		DWORD ReturnLength = 0;

		JDRV_INITPARAM pidb = { 0 };
		pidb.size = sizeof(pidb);
		pidb.version = JDRV_PROTOCOL_VERSION;
		if (isXp) pidb.flags |= JDRV_INIT_FLAG_WINDOWS_XP;
		if (isWin7) pidb.flags |= JDRV_INIT_FLAG_WINDOWS_7_OR_LATER;
		pidb.systemVersion = sysBulidVer;
		if (DeviceIoControl(hKDrv, CTL_INITPARAM, &pidb, sizeof(pidb), NULL, 0, &ReturnLength, NULL))
			return TRUE;
		else currentLogger->LogError(L"DeviceIoControl CTL_INITPARAM 错误：%d", GetLastError());
	}
	return false;
}
BOOL KFBeforeUnInitDriver()
{
	if (XDriverLoaded())
	{
		JDRV_UNLOAD_TOKEN token = {};
		if (!LoadEmbeddedUnloadToken(&token)) {
			// 先写日志再设置错误码：LogError 内部会调 WriteFile 等 API，
			// 会把 GetLastError 覆盖掉，调用方就再也读不到真实原因了。
			currentLogger->LogError(L"Embedded unload token is missing, invalid, or cannot be decrypted");
			SetLastError(ERROR_INVALID_DATA);
			return false;
		}
		DWORD ReturnLength = 0;
		if (DeviceIoControl(
			hKDrv,
			CTL_UNINIT,
			&token,
			sizeof(token),
			NULL,
			0,
			&ReturnLength,
			NULL)) {
			return TRUE;
		}
		const DWORD uninitError = GetLastError();
		currentLogger->LogError(L"DeviceIoControl CTL_UNINIT unlock token rejected: %d", uninitError);
		SetLastError(uninitError);
	}
	else {
		SetLastError(ERROR_INVALID_HANDLE);
	}
	return false;
}
BOOL KFInstallSelfProtect()
{
	if (XDriverLoaded())
	{
		JDRV_PROCESS_REQUEST request = { 0 };
		JDRV_OPERATION_RESPONSE response = { 0 };
		DWORD ReturnLength = 0;
		BOOL transportSucceeded;
		response.status = STATUS_UNSUCCESSFUL;
		request.size = sizeof(request);
		request.version = JDRV_PROTOCOL_VERSION;
		request.processId = GetCurrentProcessId();
		transportSucceeded = DeviceIoControl(
			hKDrv,
			CTL_INITSELFPROTECT,
			&request,
			sizeof(request),
			&response,
			sizeof(response),
			&ReturnLength,
			NULL);
		if (transportSucceeded &&
			ReturnLength >= sizeof(response) &&
			response.status == STATUS_SUCCESS)
			return TRUE;
		if (!transportSucceeded)
			currentLogger->LogError(L"DeviceIoControl CTL_INITSELFPROTECT 错误：%d", GetLastError());
		else
			currentLogger->LogError(L"CTL_INITSELFPROTECT 错误：0x%08X", response.status);
	}
	return false;
}
BOOL KFUnInstallSelfProtect()
{
	if (XDriverLoaded())
	{
		DWORD ReturnLength = 0;
		if (DeviceIoControl(hKDrv, CTL_CLIENT_QUIT, NULL, NULL, NULL, NULL, &ReturnLength, NULL))
			return TRUE;
		else currentLogger->LogError(L"DeviceIoControl CTL_CLIENT_QUIT 错误：%d", GetLastError());
	}
	return false;
}

BOOL KFInjectDll(DWORD pid, LPWSTR dllPath) {
	if (XDriverLoaded())
	{

	}
	else currentLogger->LogWarn(L"驱动未加载！");
	return false;
}
