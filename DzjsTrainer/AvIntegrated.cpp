#include "stdafx.h"
#include "AvIntegrated.h"

#include "DriverLoader.h"
#include "Logger.h"
#include "../JiYuAvShared/AvProtocol.h"
#include "../JiYuAvShared/SharedPatternBuilder.h"
#include <TlHelp32.h>
#include <Softpub.h>
#include <wintrust.h>
#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

extern "C" ULONG NTAPI RtlNtStatusToDosError(LONG status);

extern LoggerInternal* currentLogger;

namespace {
HANDLE g_avDevice = NULL;
// The service name is generated per-day by the app (XGetAvDriverServiceName);
// only the device path stays fixed because it is part of the IOCTL protocol.
const wchar_t kAvDevicePath[] = L"\\\\.\\JiYuAv";
std::mutex g_avSignatureCacheMutex;
std::vector<JIYU_AV_SIGNATURE_RECORD> g_avSignatureCache;

bool SendAvIoctl(DWORD code, void* input, DWORD inputSize, void* output, DWORD outputSize)
{
	if (g_avDevice == NULL || g_avDevice == INVALID_HANDLE_VALUE) return false;
	DWORD returned = 0;
	return DeviceIoControl(g_avDevice, code, input, inputSize, output, outputSize, &returned, NULL) != FALSE;
}

void CacheAvSignature(const JIYU_AV_SIGNATURE_RECORD& record)
{
	std::lock_guard<std::mutex> guard(g_avSignatureCacheMutex);
	for (JIYU_AV_SIGNATURE_RECORD& cached : g_avSignatureCache) {
		if (cached.signatureId == record.signatureId) {
			cached = record;
			return;
		}
	}
	g_avSignatureCache.push_back(record);
}

bool ReplayCachedAvSignatures()
{
	std::lock_guard<std::mutex> guard(g_avSignatureCacheMutex);
	for (JIYU_AV_SIGNATURE_RECORD& record : g_avSignatureCache) {
		if (!SendAvIoctl(
				JIYU_AV_IOCTL_ADD_SIGNATURE,
				&record,
				sizeof(record),
				nullptr,
				0)) {
			return false;
		}
	}
	if (currentLogger && !g_avSignatureCache.empty()) {
		currentLogger->LogInfo(
			L"Replayed %lu cached AV memory signature(s) after driver reconnect",
			static_cast<unsigned long>(g_avSignatureCache.size()));
	}
	return true;
}

int HexNibble(wchar_t value)
{
	if (value >= L'0' && value <= L'9') return value - L'0';
	if (value >= L'a' && value <= L'f') return value - L'a' + 10;
	if (value >= L'A' && value <= L'F') return value - L'A' + 10;
	return -1;
}

bool ParsePatternText(
	LPCWSTR text,
	JIYU_AV_SIGNATURE_RECORD* record,
	DWORD* exactByteCount)
{
	if (!text || !record) return false;
	std::wstring compact;
	for (const wchar_t* cursor = text; *cursor; ++cursor) {
		if (*cursor != L' ' && *cursor != L'\t' && *cursor != L'-' && *cursor != L':') compact.push_back(*cursor);
	}
	if (compact.empty() || (compact.size() & 1U) != 0 ||
		compact.size() > JIYU_AV_MAX_PATTERN_BYTES * 2UL) return false;
	record->dataLength = static_cast<ULONG>(compact.size() / 2U);
	ULONG exact = 0;
	for (ULONG index = 0; index < record->dataLength; ++index) {
		const wchar_t highChar = compact[index * 2U];
		const wchar_t lowChar = compact[index * 2U + 1U];
		const int high = HexNibble(highChar);
		const int low = HexNibble(lowChar);
		if (highChar == L'?' && lowChar == L'?') {
			record->data[index] = 0;
			record->mask[index] = 0;
		}
		else if (highChar == L'?' && low >= 0) {
			record->data[index] = static_cast<unsigned char>(low);
			record->mask[index] = 0x0F;
			++exact;
		}
		else if (high >= 0 && lowChar == L'?') {
			record->data[index] = static_cast<unsigned char>(high << 4);
			record->mask[index] = 0xF0;
			++exact;
		}
		else if (high >= 0 && low >= 0) {
			record->data[index] = static_cast<unsigned char>((high << 4) | low);
			record->mask[index] = 0xFF;
			exact += 2;
		}
		else return false;
	}
	if (exactByteCount) *exactByteCount = exact;
	return exact >= 8;
}

bool AddPatternRecord(JIYU_AV_SIGNATURE_RECORD* record)
{
	if (!record || !SendAvIoctl(
			JIYU_AV_IOCTL_ADD_SIGNATURE,
			record,
			sizeof(*record),
			nullptr,
			0)) return false;
	CacheAvSignature(*record);
	return true;
}

bool IsReadableProcessProtection(DWORD protection)
{
	if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
	const DWORD base = protection & 0xFFU;
	return base == PAGE_READONLY || base == PAGE_READWRITE ||
		base == PAGE_WRITECOPY || base == PAGE_EXECUTE_READ ||
		base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
}

bool IsExecutableProcessProtection(DWORD protection)
{
	if (!IsReadableProcessProtection(protection)) return false;
	const DWORD base = protection & 0xFFU;
	return base == PAGE_EXECUTE || base == PAGE_EXECUTE_READ ||
		base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
}

bool IsPathWithinDirectory(const std::wstring& path, const std::wstring& directory)
{
	if (directory.empty() || path.size() <= directory.size()) return false;
	if (_wcsnicmp(path.c_str(), directory.c_str(), directory.size()) != 0) return false;
	const wchar_t boundary = path[directory.size()];
	return boundary == L'\\' || boundary == L'/';
}

bool IsVerifiedMicrosoftSystemImage(LPCWSTR imagePath)
{
	if (!imagePath || !*imagePath) return false;
	wchar_t windowsDirectory[MAX_PATH] = {};
	if (GetWindowsDirectoryW(windowsDirectory, _countof(windowsDirectory)) == 0) return false;
	const std::wstring path(imagePath);
	const std::wstring windows(windowsDirectory);
	if (!IsPathWithinDirectory(path, windows + L"\\System32") &&
		!IsPathWithinDirectory(path, windows + L"\\SysWOW64")) return false;

	WINTRUST_FILE_INFO fileInfo = {};
	fileInfo.cbStruct = sizeof(fileInfo);
	fileInfo.pcwszFilePath = imagePath;
	WINTRUST_DATA trustData = {};
	trustData.cbStruct = sizeof(trustData);
	trustData.dwUIChoice = WTD_UI_NONE;
	trustData.fdwRevocationChecks = WTD_REVOKE_NONE;
	trustData.dwUnionChoice = WTD_CHOICE_FILE;
	trustData.pFile = &fileInfo;
	trustData.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_DISABLE_MD2_MD4;
	GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
	trustData.dwStateAction = WTD_STATEACTION_VERIFY;
	const LONG trusted = WinVerifyTrust(nullptr, &action, &trustData);
	bool microsoftPublisher = false;
	if (trusted == ERROR_SUCCESS && trustData.hWVTStateData) {
		CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(trustData.hWVTStateData);
		CRYPT_PROVIDER_SGNR* signer = provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
		CRYPT_PROVIDER_CERT* certificate = signer && signer->csCertChain != 0UL
			? WTHelperGetProvCertFromChain(signer, 0) : nullptr;
		if (certificate && certificate->pCert) {
			wchar_t publisher[128] = {};
			if (CertGetNameStringW(certificate->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr,
				publisher, _countof(publisher)) > 1UL) {
				microsoftPublisher = _wcsicmp(publisher, L"Microsoft Windows") == 0 ||
					_wcsicmp(publisher, L"Microsoft Corporation") == 0;
			}
		}
	}
	trustData.dwStateAction = WTD_STATEACTION_CLOSE;
	(void)WinVerifyTrust(nullptr, &action, &trustData);
	return trusted == ERROR_SUCCESS && microsoftPublisher;
}

bool IsVerifiedSystemProcess(DWORD processId, LPCWSTR processName)
{
	if (processId == 0UL || processId == 4UL) return true;
	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
	if (!process) return false;
	wchar_t imagePath[32768] = {};
	DWORD imageLength = _countof(imagePath);
	const bool verified = QueryFullProcessImageNameW(process, 0, imagePath, &imageLength) != FALSE &&
		IsVerifiedMicrosoftSystemImage(imagePath);
	CloseHandle(process);
	if (verified && currentLogger) currentLogger->LogInfo(
		L"AV skipped verified Microsoft system process: PID %lu %s",
		static_cast<unsigned long>(processId), processName ? processName : L"");
	return verified;
}

bool IsDistinctiveProcessWindow(const unsigned char* bytes, size_t length)
{
	if (!bytes || length < 48) return false;
	bool seen[256] = {};
	size_t distinct = 0;
	size_t run = 1;
	size_t longestRun = 1;
	for (size_t index = 0; index < 48; ++index) {
		if (!seen[bytes[index]]) { seen[bytes[index]] = true; ++distinct; }
		if (index != 0 && bytes[index] == bytes[index - 1])
			longestRun = (std::max)(longestRun, ++run);
		else run = 1;
	}
	return distinct >= 8 && longestRun <= 8;
}

bool ReadProcessPatternSample(DWORD processId, std::vector<unsigned char>* sample, size_t* preferredOffset)
{
	if (!sample || !preferredOffset || processId == 0) return false;
	HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
	if (!process) return false;
	HANDLE modules = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId);
	if (modules == INVALID_HANDLE_VALUE) { CloseHandle(process); return false; }
	MODULEENTRY32W module = {};
	module.dwSize = sizeof(module);
	if (!Module32FirstW(modules, &module)) { CloseHandle(modules); CloseHandle(process); return false; }
	const ULONG_PTR moduleBase = reinterpret_cast<ULONG_PTR>(module.modBaseAddr);
	const ULONG_PTR moduleEnd = moduleBase + module.modBaseSize;
	CloseHandle(modules);
	bool found = false;
	for (ULONG_PTR address = moduleBase; !found && address < moduleEnd;) {
		MEMORY_BASIC_INFORMATION memory = {};
		if (VirtualQueryEx(process, reinterpret_cast<LPCVOID>(address), &memory, sizeof(memory)) == 0) break;
		const ULONG_PTR next = reinterpret_cast<ULONG_PTR>(memory.BaseAddress) + memory.RegionSize;
		if (next <= address) break;
		const ULONG_PTR regionStart = (std::max)(address, reinterpret_cast<ULONG_PTR>(memory.BaseAddress));
		const ULONG_PTR regionEnd = (std::min)(next, moduleEnd);
		if (memory.State == MEM_COMMIT && memory.RegionSize >= 48 &&
			regionEnd > regionStart && IsExecutableProcessProtection(memory.Protect)) {
			const SIZE_T readSize = (std::min<SIZE_T>)(regionEnd - regionStart, 16ULL * 1024ULL * 1024ULL);
			std::vector<unsigned char> bytes(static_cast<size_t>(readSize));
			SIZE_T read = 0;
			if (ReadProcessMemory(process, reinterpret_cast<LPCVOID>(regionStart), bytes.data(), readSize, &read) && read >= 48) {
				for (size_t offset = 0; offset + 48 <= read; offset += 16) {
					if (IsDistinctiveProcessWindow(bytes.data() + offset, read - offset)) {
						*sample = std::move(bytes);
						*preferredOffset = offset;
						found = true;
						break;
					}
				}
			}
		}
		address = next;
	}
	CloseHandle(process);
	return found;
}

typedef LONG (NTAPI* NtWow64QueryInformationProcess64Fn)(
	HANDLE, ULONG, PVOID, ULONG, PULONG);
typedef LONG (NTAPI* NtWow64ReadVirtualMemory64Fn)(
	HANDLE, ULONGLONG, PVOID, ULONGLONG, PULONGLONG);

struct Wow64ProcessBasicInformation64 {
	ULONGLONG exitStatus;
	ULONGLONG pebBaseAddress;
	ULONGLONG affinityMask;
	ULONGLONG basePriority;
	ULONGLONG uniqueProcessId;
	ULONGLONG inheritedFromUniqueProcessId;
};

bool ReadProcessPatternSample64(
	DWORD processId,
	std::vector<unsigned char>* sample,
	size_t* preferredOffset)
{
	if (!sample || !preferredOffset || processId == 0UL) return false;
	HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, processId);
	if (!process) return false;
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	NtWow64QueryInformationProcess64Fn query64 = ntdll
		? reinterpret_cast<NtWow64QueryInformationProcess64Fn>(GetProcAddress(ntdll, "NtWow64QueryInformationProcess64"))
		: nullptr;
	NtWow64ReadVirtualMemory64Fn read64 = ntdll
		? reinterpret_cast<NtWow64ReadVirtualMemory64Fn>(GetProcAddress(ntdll, "NtWow64ReadVirtualMemory64"))
		: nullptr;
	if (!query64 || !read64) {
		CloseHandle(process);
		return false;
	}
	Wow64ProcessBasicInformation64 basic = {};
	if (query64(process, 0UL, &basic, sizeof(basic), nullptr) < 0 || basic.pebBaseAddress == 0) {
		CloseHandle(process);
		return false;
	}
	ULONGLONG imageBase = 0;
	ULONGLONG bytesRead = 0;
	if (read64(process, basic.pebBaseAddress + 0x10ULL, &imageBase, sizeof(imageBase), &bytesRead) < 0 ||
		bytesRead != sizeof(imageBase) || imageBase == 0) {
		CloseHandle(process);
		return false;
	}
	std::vector<unsigned char> headers(0x4000U);
	bytesRead = 0;
	if (read64(process, imageBase, headers.data(), headers.size(), &bytesRead) < 0 || bytesRead < sizeof(IMAGE_DOS_HEADER)) {
		CloseHandle(process);
		return false;
	}
	const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(headers.data());
	if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
		static_cast<size_t>(dos->e_lfanew) + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) > bytesRead) {
		CloseHandle(process);
		return false;
	}
	const BYTE* ntBase = headers.data() + dos->e_lfanew;
	if (*reinterpret_cast<const DWORD*>(ntBase) != IMAGE_NT_SIGNATURE) {
		CloseHandle(process);
		return false;
	}
	const IMAGE_FILE_HEADER* fileHeader = reinterpret_cast<const IMAGE_FILE_HEADER*>(ntBase + sizeof(DWORD));
	const BYTE* optional = ntBase + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
	if (static_cast<size_t>(optional - headers.data()) + fileHeader->SizeOfOptionalHeader > bytesRead ||
		fileHeader->SizeOfOptionalHeader < sizeof(WORD)) {
		CloseHandle(process);
		return false;
	}
	const WORD magic = *reinterpret_cast<const WORD*>(optional);
	DWORD sizeOfImage = 0;
	if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC && fileHeader->SizeOfOptionalHeader >= sizeof(IMAGE_OPTIONAL_HEADER64)) {
		sizeOfImage = reinterpret_cast<const IMAGE_OPTIONAL_HEADER64*>(optional)->SizeOfImage;
	}
	else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC && fileHeader->SizeOfOptionalHeader >= sizeof(IMAGE_OPTIONAL_HEADER32)) {
		sizeOfImage = reinterpret_cast<const IMAGE_OPTIONAL_HEADER32*>(optional)->SizeOfImage;
	}
	if (sizeOfImage == 0) {
		CloseHandle(process);
		return false;
	}
	const size_t sectionOffset = static_cast<size_t>(optional - headers.data()) + fileHeader->SizeOfOptionalHeader;
	const size_t sectionBytes = static_cast<size_t>(fileHeader->NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
	if (sectionOffset + sectionBytes > headers.size()) {
		headers.resize(sectionOffset + sectionBytes);
		bytesRead = 0;
		if (read64(process, imageBase, headers.data(), headers.size(), &bytesRead) < 0 || bytesRead < sectionOffset + sectionBytes) {
			CloseHandle(process);
			return false;
		}
	}
	const IMAGE_SECTION_HEADER* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(headers.data() + sectionOffset);
	for (WORD index = 0; index < fileHeader->NumberOfSections; ++index) {
		const IMAGE_SECTION_HEADER& section = sections[index];
		if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;
		const DWORD sectionSize = (std::max)(section.Misc.VirtualSize, section.SizeOfRawData);
		if (sectionSize < 48U || static_cast<ULONGLONG>(section.VirtualAddress) + sectionSize > sizeOfImage) continue;
		const SIZE_T readSize = (std::min<DWORD>)(sectionSize, 16UL * 1024UL * 1024UL);
		std::vector<unsigned char> bytes(readSize);
		bytesRead = 0;
		if (read64(process, imageBase + section.VirtualAddress, bytes.data(), readSize, &bytesRead) < 0 || bytesRead < 48ULL) continue;
		for (size_t offset = 0; offset + 48 <= static_cast<size_t>(bytesRead); offset += 16) {
			if (IsDistinctiveProcessWindow(bytes.data() + offset, static_cast<size_t>(bytesRead) - offset)) {
				*sample = std::move(bytes);
				*preferredOffset = offset;
				CloseHandle(process);
				return true;
			}
		}
	}
	CloseHandle(process);
	SetLastError(ERROR_NOT_FOUND);
	return false;
}

bool ReadProcessPatternSampleFromKernel(
	DWORD processId,
	std::vector<unsigned char>* sample,
	size_t* preferredOffset)
{
	if (!AvIntegratedIsLoaded() || !sample || !preferredOffset || processId == 0UL) return false;
	JIYU_AV_PROCESS_REQUEST request = {};
	request.size = sizeof(request);
	request.version = JIYU_AV_PROTOCOL_VERSION;
	request.processId = processId;
	JIYU_AV_PROCESS_SAMPLE_RESPONSE response = {};
	if (!SendAvIoctl(
			JIYU_AV_IOCTL_SAMPLE_PROCESS_IMAGE,
			&request,
			sizeof(request),
			&response,
			sizeof(response)) ||
		response.status != 0 || response.processId != processId ||
		response.dataLength < 48UL || response.dataLength > JIYU_AV_PROCESS_SAMPLE_BYTES ||
		!IsDistinctiveProcessWindow(response.data, response.dataLength)) {
		if (response.status != 0) SetLastError(RtlNtStatusToDosError(response.status));
		return false;
	}
	sample->assign(response.data, response.data + response.dataLength);
	*preferredOffset = 0;
	return true;
}

bool ScanFileDetailed(LPCWSTR path, JIYU_AV_SCAN_RESULT* result)
{
	if (!AvIntegratedIsLoaded() || !path || !*path || !result) return false;
	const size_t pathLength = wcslen(path);
	if (pathLength == 0 || pathLength >= JIYU_AV_MAX_PATH_CHARS) {
		SetLastError(ERROR_FILENAME_EXCED_RANGE);
		return false;
	}
	JIYU_AV_SCAN_FILE_REQUEST request = {};
	request.size = sizeof(request);
	request.version = JIYU_AV_PROTOCOL_VERSION;
	request.pathLength = static_cast<unsigned long>(pathLength);
	wmemcpy_s(request.path, JIYU_AV_MAX_PATH_CHARS, path, pathLength);
	ZeroMemory(result, sizeof(*result));
	return SendAvIoctl(
		JIYU_AV_IOCTL_SCAN_FILE,
		&request,
		sizeof(request),
		result,
		sizeof(*result));
}

bool ScanProcessDetailed(DWORD processId, JIYU_AV_SCAN_RESULT* result)
{
	if (!AvIntegratedIsLoaded() || processId == 0 || !result) return false;
	JIYU_AV_PROCESS_REQUEST request = {};
	request.size = sizeof(request);
	request.version = JIYU_AV_PROTOCOL_VERSION;
	request.processId = processId;
	ZeroMemory(result, sizeof(*result));
	return SendAvIoctl(
		JIYU_AV_IOCTL_SCAN_PROCESS,
		&request,
		sizeof(request),
		result,
		sizeof(*result));
}

struct ExecutableSection
{
    std::vector<unsigned char> bytes;
    std::vector<unsigned char> stableMask;
    size_t entryOffset = 0;
	bool packed = false;
};

bool ReadWholeFile(LPCWSTR path, std::vector<unsigned char>* bytes)
{
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER size = {};
	if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 512LL * 1024LL * 1024LL) {
		CloseHandle(file);
		SetLastError(ERROR_FILE_TOO_LARGE);
		return false;
	}
	bytes->resize(static_cast<size_t>(size.QuadPart));
	size_t offset = 0;
	while (offset < bytes->size()) {
		const DWORD request = static_cast<DWORD>((std::min<size_t>)(bytes->size() - offset, 4UL * 1024UL * 1024UL));
		DWORD read = 0;
		if (!ReadFile(file, bytes->data() + offset, request, &read, nullptr) || read == 0) {
			CloseHandle(file);
			return false;
		}
		offset += read;
	}
	CloseHandle(file);
	return true;
}

bool LoadExecutableSection(LPCWSTR path, ExecutableSection* output)
{
	std::vector<unsigned char> file;
	if (!ReadWholeFile(path, &file) || file.size() < sizeof(IMAGE_DOS_HEADER)) return false;
	const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());
	if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
	const size_t ntOffset = static_cast<size_t>(dos->e_lfanew);
	if (ntOffset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) > file.size()) return false;
	if (*reinterpret_cast<const DWORD*>(file.data() + ntOffset) != IMAGE_NT_SIGNATURE) return false;
	const IMAGE_FILE_HEADER* fileHeader = reinterpret_cast<const IMAGE_FILE_HEADER*>(file.data() + ntOffset + sizeof(DWORD));
	const size_t optionalOffset = ntOffset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
	if (optionalOffset + fileHeader->SizeOfOptionalHeader > file.size() || fileHeader->SizeOfOptionalHeader < sizeof(WORD)) return false;
	const WORD magic = *reinterpret_cast<const WORD*>(file.data() + optionalOffset);
	DWORD entryRva = 0;
	IMAGE_DATA_DIRECTORY relocationDirectory = {};
	if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC && fileHeader->SizeOfOptionalHeader >= sizeof(IMAGE_OPTIONAL_HEADER32)) {
		const IMAGE_OPTIONAL_HEADER32* optional = reinterpret_cast<const IMAGE_OPTIONAL_HEADER32*>(file.data() + optionalOffset);
		entryRva = optional->AddressOfEntryPoint;
		if (optional->NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_BASERELOC)
			relocationDirectory = optional->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
	}
	else if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC && fileHeader->SizeOfOptionalHeader >= sizeof(IMAGE_OPTIONAL_HEADER64)) {
		const IMAGE_OPTIONAL_HEADER64* optional = reinterpret_cast<const IMAGE_OPTIONAL_HEADER64*>(file.data() + optionalOffset);
		entryRva = optional->AddressOfEntryPoint;
		if (optional->NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_BASERELOC)
			relocationDirectory = optional->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
	}
	else return false;

	const size_t sectionOffset = optionalOffset + fileHeader->SizeOfOptionalHeader;
	if (sectionOffset + static_cast<size_t>(fileHeader->NumberOfSections) * sizeof(IMAGE_SECTION_HEADER) > file.size()) return false;
	const IMAGE_SECTION_HEADER* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(file.data() + sectionOffset);
	const IMAGE_SECTION_HEADER* selected = nullptr;
	for (WORD index = 0; index < fileHeader->NumberOfSections; ++index) {
		const IMAGE_SECTION_HEADER& section = sections[index];
		if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 || section.SizeOfRawData == 0) continue;
		const DWORD virtualSize = (std::max)(section.Misc.VirtualSize, section.SizeOfRawData);
		if (entryRva >= section.VirtualAddress && entryRva < section.VirtualAddress + virtualSize) {
			selected = &section;
			break;
		}
		if (!selected && (section.Characteristics & IMAGE_SCN_CNT_CODE) != 0) selected = &section;
	}
	if (!selected || selected->PointerToRawData >= file.size()) return false;
	const size_t rawSize = (std::min<size_t>)(selected->SizeOfRawData, file.size() - selected->PointerToRawData);
	if (rawSize < 32) return false;
	output->bytes.assign(
		file.begin() + selected->PointerToRawData,
		file.begin() + selected->PointerToRawData + rawSize);
	output->stableMask.assign(rawSize, 0xFFU);
	auto rvaToRawOffset = [&](DWORD rva, size_t* rawOffset) -> bool {
		for (WORD index = 0; index < fileHeader->NumberOfSections; ++index) {
			const IMAGE_SECTION_HEADER& section = sections[index];
			const DWORD span = (std::max)(section.Misc.VirtualSize, section.SizeOfRawData);
			if (rva < section.VirtualAddress || rva >= section.VirtualAddress + span) continue;
			const DWORD delta = rva - section.VirtualAddress;
			if (delta >= section.SizeOfRawData || section.PointerToRawData + delta >= file.size()) return false;
			*rawOffset = static_cast<size_t>(section.PointerToRawData) + delta;
			return true;
		}
		return false;
	};
	if (relocationDirectory.VirtualAddress != 0 && relocationDirectory.Size >= sizeof(IMAGE_BASE_RELOCATION)) {
		size_t relocationRawOffset = 0;
		if (rvaToRawOffset(relocationDirectory.VirtualAddress, &relocationRawOffset)) {
			const size_t available = (std::min<size_t>)(relocationDirectory.Size, file.size() - relocationRawOffset);
			size_t cursor = 0;
			while (cursor + sizeof(IMAGE_BASE_RELOCATION) <= available) {
				const IMAGE_BASE_RELOCATION* block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(
					file.data() + relocationRawOffset + cursor);
				if (block->SizeOfBlock < sizeof(*block) || block->SizeOfBlock > available - cursor) break;
				const WORD* entries = reinterpret_cast<const WORD*>(block + 1);
				const size_t entryCount = (block->SizeOfBlock - sizeof(*block)) / sizeof(WORD);
				for (size_t entryIndex = 0; entryIndex < entryCount; ++entryIndex) {
					const WORD entry = entries[entryIndex];
					const WORD type = entry >> 12;
					size_t width = 0;
					if (type == IMAGE_REL_BASED_HIGHLOW) width = 4;
					else if (type == IMAGE_REL_BASED_DIR64) width = 8;
					else if (type == IMAGE_REL_BASED_HIGH || type == IMAGE_REL_BASED_LOW || type == IMAGE_REL_BASED_HIGHADJ) width = 2;
					if (type == IMAGE_REL_BASED_HIGHADJ && entryIndex + 1 < entryCount) ++entryIndex;
					if (width == 0) continue;
					const DWORD targetRva = block->VirtualAddress + (entry & 0x0FFFU);
					if (targetRva < selected->VirtualAddress) continue;
					const size_t sectionByte = static_cast<size_t>(targetRva - selected->VirtualAddress);
					for (size_t byteIndex = 0; byteIndex < width && sectionByte + byteIndex < output->stableMask.size(); ++byteIndex)
						output->stableMask[sectionByte + byteIndex] = 0U;
				}
				cursor += block->SizeOfBlock;
			}
		}
	}
	output->entryOffset = entryRva >= selected->VirtualAddress
		? static_cast<size_t>(entryRva - selected->VirtualAddress)
		: 0;
	if (output->entryOffset >= output->bytes.size()) output->entryOffset = 0;
	output->packed = memcmp(selected->Name, "UPX", 3) == 0;
	return true;
}

bool BuildSharedPattern(
	const std::vector<ExecutableSection>& samples,
	JIYU_AV_SIGNATURE_RECORD* record,
	DWORD* exactByteCount)
{
	std::vector<JIYU_AV_PATTERN_SAMPLE_VIEW> views;
	views.reserve(samples.size());
	for (const ExecutableSection& sample : samples) {
		views.push_back({ sample.bytes.data(), sample.bytes.size(), sample.entryOffset, sample.stableMask.data() });
	}
	std::size_t patternLength = 0;
	std::size_t exact = 0;
	if (!JiYuAvBuildSharedPattern(
		views.data(), views.size(), record->data, record->mask,
		JIYU_AV_MAX_PATTERN_BYTES, &patternLength, &exact)) {
		return false;
	}
	record->dataLength = static_cast<unsigned long>(patternLength);
	if (exactByteCount) *exactByteCount = static_cast<DWORD>(exact);
	return true;
}

ULONG BuildPatternSignatureId(const JIYU_AV_SIGNATURE_RECORD& record)
{
	ULONG value = 2166136261UL;
	for (ULONG index = 0; index < record.dataLength; ++index) {
		value ^= record.data[index];
		value *= 16777619UL;
		value ^= record.mask[index];
		value *= 16777619UL;
	}
	value |= 0x80000000UL;
	return value == 0UL ? 0x80000001UL : value;
}

void PatternToText(const JIYU_AV_SIGNATURE_RECORD& record, LPWSTR output, DWORD outputCount)
{
	static constexpr wchar_t digits[] = L"0123456789ABCDEF";
	if (!output || outputCount < record.dataLength * 3UL) return;
	DWORD cursor = 0;
	for (ULONG index = 0; index < record.dataLength; ++index) {
		if (index != 0) output[cursor++] = L' ';
		if (record.mask[index] == 0xFFU) {
			output[cursor++] = digits[record.data[index] >> 4];
			output[cursor++] = digits[record.data[index] & 0x0FU];
		}
		else {
			output[cursor++] = L'?';
			output[cursor++] = L'?';
		}
	}
	output[cursor] = L'\0';
}

LPCWSTR FileNameFromPath(LPCWSTR path)
{
	LPCWSTR slash = wcsrchr(path, L'\\');
	LPCWSTR forwardSlash = wcsrchr(path, L'/');
	if (forwardSlash && (!slash || forwardSlash > slash)) slash = forwardSlash;
	return slash ? slash + 1 : path;
}

bool OpenAndRegister()
{
	g_avDevice = CreateFileW(kAvDevicePath, GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (g_avDevice == INVALID_HANDLE_VALUE) { g_avDevice = NULL; return false; }
	JIYU_AV_VERSION_RESPONSE version = {};
	if (!SendAvIoctl(JIYU_AV_IOCTL_QUERY_VERSION, NULL, 0, &version, sizeof(version)) ||
		version.magic != JIYU_AV_PROTOCOL_MAGIC || version.version != JIYU_AV_PROTOCOL_VERSION || version.architecture != 64UL) {
		CloseHandle(g_avDevice); g_avDevice = NULL; return false;
	}
	if (!SendAvIoctl(JIYU_AV_IOCTL_REGISTER_CONTROLLER, NULL, 0, NULL, 0)) {
		CloseHandle(g_avDevice); g_avDevice = NULL; return false;
	}
	return true;
}

bool ArmUnload(HANDLE device)
{
	if (device == NULL || device == INVALID_HANDLE_VALUE) return false;
	DWORD returned = 0;
	return DeviceIoControl(
		device,
		JIYU_AV_IOCTL_ARM_UNLOAD,
		nullptr,
		0,
		nullptr,
		0,
		&returned,
		nullptr) != FALSE;
}
}

BOOL AvIntegratedEnsureLoaded(LPCWSTR driverPath)
{
	if (AvIntegratedIsLoaded()) return TRUE;
	if (!driverPath || !*driverPath) return FALSE;
	if (!OpenAndRegister()) {
		if (!MLoadKernelDriver(XGetAvDriverServiceName(), driverPath, L"Kernel Scanner")) {
			if (currentLogger) currentLogger->LogWarn(L"AV \u5185\u6838\u670d\u52a1\u542f\u52a8\u5931\u8d25\uff0c\u626b\u63cf\u4e0e\u81ea\u4fdd\u62a4\u672a\u542f\u7528");
			return FALSE;
		}
		if (!OpenAndRegister()) {
			if (currentLogger) currentLogger->LogWarn(L"AV \u5185\u6838\u8bbe\u5907\u8fde\u63a5\u5931\u8d25\uff0c\u626b\u63cf\u4e0e\u81ea\u4fdd\u62a4\u672a\u542f\u7528\uff1a%lu", GetLastError());
			return FALSE;
		}
	}
	if (!ReplayCachedAvSignatures()) {
		const DWORD error = GetLastError();
		if (currentLogger) currentLogger->LogError(
			L"Failed to replay cached AV memory signatures after driver reconnect: %lu",
			static_cast<unsigned long>(error));
		AvIntegratedClose();
		SetLastError(error);
		return FALSE;
	}
	if (!AvIntegratedSetProtectedProcess(GetCurrentProcessId())) {
		if (currentLogger) currentLogger->LogWarn(L"AV \u5185\u6838\u5df2\u52a0\u8f7d\uff0c\u4f46\u4e3b\u7a0b\u5e8f\u4fdd\u62a4\u76ee\u6807\u8bbe\u7f6e\u5931\u8d25\uff1a%lu", GetLastError());
	}
	else if (currentLogger) currentLogger->LogInfo(L"AV \u5185\u6838\u5df2\u52a0\u8f7d\uff0c\u4e3b\u7a0b\u5e8f\u81ea\u4fdd\u62a4\u5df2\u542f\u7528");
	return TRUE;
}

BOOL AvIntegratedPrepareForReplacement()
{
	// The protocol number changed with the scanner, but controller registration
	// and ARM_UNLOAD kept the same IOCTLs so an older v2 service can be retired.
	HANDLE device = g_avDevice;
	bool ownsHandle = false;
	if (device == NULL || device == INVALID_HANDLE_VALUE) {
		device = CreateFileW(
			kAvDevicePath,
			GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			nullptr);
		if (device == INVALID_HANDLE_VALUE) {
			const DWORD error = GetLastError();
			// A stale file without a loaded service is safe to overwrite.
			if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
				error == ERROR_DEVICE_NOT_CONNECTED) {
				return TRUE;
			}
			SetLastError(error);
			return FALSE;
		}
		ownsHandle = true;
	}

	bool registered = g_avDevice != NULL && g_avDevice != INVALID_HANDLE_VALUE;
	if (!registered) {
		DWORD returned = 0;
		registered = DeviceIoControl(
			device,
			JIYU_AV_IOCTL_REGISTER_CONTROLLER,
			nullptr,
			0,
			nullptr,
			0,
			&returned,
			nullptr) != FALSE;
	}
	const bool armed = registered && ArmUnload(device);
	if (ownsHandle) CloseHandle(device);
	if (g_avDevice != NULL && g_avDevice != INVALID_HANDLE_VALUE) {
		CloseHandle(g_avDevice);
		g_avDevice = NULL;
	}
	if (!armed) return FALSE;

	if (!MUnLoadKernelDriver(XGetAvDriverServiceName())) {
		return FALSE;
	}
	return TRUE;
}

void AvIntegratedArmLegacyDriverForUnload()
{
	if (g_avDevice != NULL && g_avDevice != INVALID_HANDLE_VALUE) return;
	// The loaded AV driver only counts as legacy when today's generated
	// service name is not the running instance; never arm the current one.
	SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
	if (manager != nullptr) {
		SC_HANDLE service = OpenServiceW(
			manager, XGetAvDriverServiceName(), SERVICE_QUERY_STATUS);
		if (service != nullptr) {
			SERVICE_STATUS_PROCESS status = {};
			DWORD needed = 0;
			const BOOL queried = QueryServiceStatusEx(
				service,
				SC_STATUS_PROCESS_INFO,
				reinterpret_cast<BYTE*>(&status),
				sizeof(status),
				&needed);
			const bool running = queried != FALSE && status.dwCurrentState == SERVICE_RUNNING;
			CloseServiceHandle(service);
			CloseServiceHandle(manager);
			if (running) return;
		}
		else {
			CloseServiceHandle(manager);
		}
	}
	HANDLE device = CreateFileW(
		kAvDevicePath,
		GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		nullptr);
	if (device == INVALID_HANDLE_VALUE) return;
	DWORD returned = 0;
	DeviceIoControl(
		device,
		JIYU_AV_IOCTL_REGISTER_CONTROLLER,
		nullptr,
		0,
		nullptr,
		0,
		&returned,
		nullptr);
	ArmUnload(device);
	CloseHandle(device);
}

BOOL AvIntegratedUnload()
{
	return AvIntegratedPrepareForReplacement();
}

BOOL AvIntegratedIsLoaded() { return g_avDevice != NULL && g_avDevice != INVALID_HANDLE_VALUE; }

BOOL AvIntegratedSetProtectedProcess(DWORD processId)
{
	JIYU_AV_PROCESS_REQUEST request = {};
	request.size = sizeof(request); request.version = JIYU_AV_PROTOCOL_VERSION; request.processId = processId;
	return SendAvIoctl(JIYU_AV_IOCTL_SET_PROTECTED_PROCESS, &request, sizeof(request), NULL, 0) ? TRUE : FALSE;
}

BOOL AvIntegratedScanFile(LPCWSTR path, LPWSTR matchedName, DWORD matchedNameCount, BOOL* detected)
{
	if (detected) *detected = FALSE;
	if (matchedName && matchedNameCount) matchedName[0] = L'\0';
	JIYU_AV_SCAN_RESULT result = {};
	if (!ScanFileDetailed(path, &result) || result.status != 0) return FALSE;
	BOOL isDetected = (result.flags & JIYU_AV_SCAN_FLAG_MATCHED) != 0 ? TRUE : FALSE;
	if (detected) *detected = isDetected;
	if (matchedName && matchedNameCount && isDetected) wcsncpy_s(matchedName, matchedNameCount, result.signatureName, _TRUNCATE);
	return TRUE;
}

BOOL AvIntegratedScanAllProcessMemory(HANDLE cancelEvent, PAV_PROCESS_SCAN_SUMMARY summary)
{
	if (!summary) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	ZeroMemory(summary, sizeof(*summary));
	if (!AvIntegratedIsLoaded()) {
		SetLastError(ERROR_SERVICE_NOT_ACTIVE);
		return FALSE;
	}

	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) return FALSE;
	PROCESSENTRY32W entry = {};
	entry.dwSize = sizeof(entry);
	BOOL enumerated = Process32FirstW(snapshot, &entry);
	while (enumerated) {
		++summary->processCount;
		if (cancelEvent && WaitForSingleObject(cancelEvent, 0) == WAIT_OBJECT_0) {
			summary->cancelled = TRUE;
			break;
		}

		if (IsVerifiedSystemProcess(entry.th32ProcessID, entry.szExeFile)) {
			++summary->failedCount;
		}
		else {
			JIYU_AV_SCAN_RESULT result = {};
			if (!ScanProcessDetailed(entry.th32ProcessID, &result) || result.status != 0) {
				++summary->failedCount;
			}
			else {
				++summary->scannedCount;
				if ((result.flags & JIYU_AV_SCAN_FLAG_MATCHED) != 0UL) {
					++summary->detectionCount;
					if (summary->findingCount < AV_INTEGRATED_MAX_FINDINGS) {
						WCHAR* finding = summary->findings[summary->findingCount++];
						swprintf_s(
							finding,
							AV_INTEGRATED_FINDING_CHARS,
							L"PID %lu  |  %s  |  %s",
							static_cast<unsigned long>(entry.th32ProcessID),
							entry.szExeFile,
							result.signatureName);
					}
				}
			}
		}
		enumerated = Process32NextW(snapshot, &entry);
	}
	CloseHandle(snapshot);
	return TRUE;
}

BOOL AvIntegratedAddExePatternSignature(
	LPCWSTR const* paths,
	DWORD pathCount,
	LPWSTR patternText,
	DWORD patternTextCount,
	ULONG* signatureId,
	DWORD* exactByteCount,
	BOOL* packedSample)
{
	if (patternText && patternTextCount) patternText[0] = L'\0';
	if (signatureId) *signatureId = 0UL;
	if (exactByteCount) *exactByteCount = 0UL;
	if (packedSample) *packedSample = FALSE;
	if (!paths || pathCount == 0 || pathCount > 16) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	std::vector<ExecutableSection> samples(pathCount);
	for (DWORD index = 0; index < pathCount; ++index) {
		if (!paths[index] || !LoadExecutableSection(paths[index], &samples[index])) {
			SetLastError(ERROR_BAD_EXE_FORMAT);
			return FALSE;
		}
		if (packedSample && samples[index].packed) *packedSample = TRUE;
	}
	JIYU_AV_SIGNATURE_RECORD record = {};
	record.size = sizeof(record);
	record.version = JIYU_AV_PROTOCOL_VERSION;
	record.type = JIYU_AV_SIGNATURE_PATTERN;
	if (!BuildSharedPattern(samples, &record, exactByteCount)) {
		SetLastError(ERROR_NOT_FOUND);
		return FALSE;
	}
	record.signatureId = BuildPatternSignatureId(record);
	wcsncpy_s(record.name, FileNameFromPath(paths[0]), _TRUNCATE);
	if (!SendAvIoctl(
		JIYU_AV_IOCTL_ADD_SIGNATURE,
		&record,
		sizeof(record),
		nullptr,
		0)) {
		return FALSE;
	}
	CacheAvSignature(record);
	PatternToText(record, patternText, patternTextCount);
	if (signatureId) *signatureId = record.signatureId;
	return TRUE;
}

BOOL AvIntegratedAddProcessPatternSignature(
	DWORD processId,
	LPWSTR patternText,
	DWORD patternTextCount,
	ULONG* signatureId,
	DWORD* exactByteCount)
{
	if (patternText && patternTextCount) patternText[0] = L'\0';
	if (signatureId) *signatureId = 0UL;
	if (exactByteCount) *exactByteCount = 0UL;
	std::vector<unsigned char> bytes;
	size_t offset = 0;
	if (!ReadProcessPatternSample(processId, &bytes, &offset)) {
		const DWORD userModeError = GetLastError();
		if (!ReadProcessPatternSample64(processId, &bytes, &offset) &&
			!ReadProcessPatternSampleFromKernel(processId, &bytes, &offset)) {
			SetLastError(GetLastError() ? GetLastError() : userModeError ? userModeError : ERROR_NOT_FOUND);
			return FALSE;
		}
		if (currentLogger) currentLogger->LogInfo(
			L"AV used kernel image sampling for PID %lu after user-mode read failed: %lu",
			static_cast<unsigned long>(processId),
			static_cast<unsigned long>(userModeError));
	}
	if (offset + 48 > bytes.size()) {
		SetLastError(ERROR_NOT_FOUND);
		return FALSE;
	}
	JIYU_AV_SIGNATURE_RECORD record = {};
	record.size = sizeof(record);
	record.version = JIYU_AV_PROTOCOL_VERSION;
	record.type = JIYU_AV_SIGNATURE_PATTERN;
	record.dataLength = 48;
	memcpy(record.data, bytes.data() + offset, record.dataLength);
	memset(record.mask, 0xFF, record.dataLength);
	record.signatureId = BuildPatternSignatureId(record);
	WCHAR name[MAX_PATH] = L"PID";
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot != INVALID_HANDLE_VALUE) {
		PROCESSENTRY32W entry = {};
		entry.dwSize = sizeof(entry);
		if (Process32FirstW(snapshot, &entry)) do {
			if (entry.th32ProcessID == processId) {
				swprintf_s(name, L"%s (PID %lu)", entry.szExeFile, static_cast<unsigned long>(processId));
				break;
			}
		} while (Process32NextW(snapshot, &entry));
		CloseHandle(snapshot);
	}
	wcsncpy_s(record.name, name, _TRUNCATE);
	if (!AddPatternRecord(&record)) return FALSE;
	PatternToText(record, patternText, patternTextCount);
	if (signatureId) *signatureId = record.signatureId;
	if (exactByteCount) *exactByteCount = record.dataLength;
	return TRUE;
}

BOOL AvIntegratedAddPatternSignatureText(
	LPCWSTR patternText,
	LPCWSTR signatureName,
	ULONG* signatureId,
	DWORD* exactByteCount)
{
	if (signatureId) *signatureId = 0UL;
	if (exactByteCount) *exactByteCount = 0UL;
	JIYU_AV_SIGNATURE_RECORD record = {};
	DWORD exact = 0;
	if (!ParsePatternText(patternText, &record, &exact)) {
		SetLastError(ERROR_INVALID_DATA);
		return FALSE;
	}
	record.size = sizeof(record);
	record.version = JIYU_AV_PROTOCOL_VERSION;
	record.type = JIYU_AV_SIGNATURE_PATTERN;
	record.signatureId = BuildPatternSignatureId(record);
	wcsncpy_s(record.name, signatureName && *signatureName ? signatureName : L"手工输入特征", _TRUNCATE);
	if (!AddPatternRecord(&record)) return FALSE;
	if (signatureId) *signatureId = record.signatureId;
	if (exactByteCount) *exactByteCount = exact / 2U;
	return TRUE;
}

BOOL AvIntegratedAddPatternSignatureTextBatch(
	LPCWSTR patternText,
	LPCWSTR signatureName,
	ULONG* lastSignatureId,
	DWORD* registeredCount,
	DWORD* exactByteCount)
{
	if (lastSignatureId) *lastSignatureId = 0UL;
	if (registeredCount) *registeredCount = 0UL;
	if (exactByteCount) *exactByteCount = 0UL;
	if (!patternText || !*patternText) {
		SetLastError(ERROR_INVALID_DATA);
		return FALSE;
	}
	std::vector<JIYU_AV_SIGNATURE_RECORD> records;
	std::wstring line;
	auto parseLine = [&](const std::wstring& value) -> bool {
		if (value.empty()) return true;
		JIYU_AV_SIGNATURE_RECORD record = {};
		DWORD exact = 0;
		if (!ParsePatternText(value.c_str(), &record, &exact)) return false;
		record.size = sizeof(record);
		record.version = JIYU_AV_PROTOCOL_VERSION;
		record.type = JIYU_AV_SIGNATURE_PATTERN;
		record.signatureId = BuildPatternSignatureId(record);
		records.push_back(record);
		return records.size() <= JIYU_AV_MAX_SIGNATURES;
	};
	for (const wchar_t* cursor = patternText;; ++cursor) {
		if (*cursor == L'\r' || *cursor == L'\n' || *cursor == L'\0') {
			if (!parseLine(line)) {
				SetLastError(ERROR_INVALID_DATA);
				return FALSE;
			}
			line.clear();
			if (*cursor == L'\r' && cursor[1] == L'\n') ++cursor;
			if (*cursor == L'\0') break;
		}
		else line.push_back(*cursor);
	}
	if (records.empty()) {
		SetLastError(ERROR_INVALID_DATA);
		return FALSE;
	}
	const LPCWSTR baseName = signatureName && *signatureName ? signatureName : L"手工输入特征";
	DWORD exactTotal = 0;
	ULONG lastId = 0;
	for (size_t index = 0; index < records.size(); ++index) {
		WCHAR name[JIYU_AV_MAX_SIGNATURE_NAME] = {};
		swprintf_s(name, L"%s #%lu", baseName, static_cast<unsigned long>(index + 1));
		wcsncpy_s(records[index].name, name, _TRUNCATE);
		if (!AddPatternRecord(&records[index])) return FALSE;
		lastId = records[index].signatureId;
		for (ULONG byteIndex = 0; byteIndex < records[index].dataLength; ++byteIndex)
			if (records[index].mask[byteIndex] == 0xFFU) ++exactTotal;
	}
	if (lastSignatureId) *lastSignatureId = lastId;
	if (registeredCount) *registeredCount = static_cast<DWORD>(records.size());
	if (exactByteCount) *exactByteCount = exactTotal;
	return TRUE;
}

void AvIntegratedClose()
{
	if (g_avDevice != NULL && g_avDevice != INVALID_HANDLE_VALUE) CloseHandle(g_avDevice);
	g_avDevice = NULL;
}
