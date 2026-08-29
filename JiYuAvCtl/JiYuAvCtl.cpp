#include <Windows.h>
#include <winsvc.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <iostream>
#include <string>
#include <vector>

#include "..\JiYuAvShared\AvProtocol.h"

static_assert(sizeof(JIYU_AV_SIGNATURE_RECORD) == 280UL);
static_assert(sizeof(JIYU_AV_SIGNATURE_QUERY_RESPONSE) == 296UL);
static_assert(sizeof(JIYU_AV_SCAN_FILE_REQUEST) == 1056UL);
static_assert(sizeof(JIYU_AV_SCAN_RESULT) == 200UL);

namespace {

constexpr wchar_t kDevicePath[] = L"\\\\.\\JiYuAv";

// Keep the controller's service identity in lockstep with JiYuTrainer's AV
// service generator. The image filename is intentionally independent.
std::wstring DateBasedServiceName()
{
    SYSTEMTIME localTime{};
    GetLocalTime(&localTime);
    std::uint32_t key =
        (static_cast<std::uint32_t>(localTime.wDay) << 24) |
        (static_cast<std::uint32_t>(localTime.wMonth) << 12) |
        static_cast<std::uint32_t>(localTime.wYear & 0x0FFFU);
    key ^= 0xA5A5F00DU;
    key = ~key + (key << 15);
    key ^= key >> 12;
    key += key << 2;
    key ^= key >> 4;
    key *= 2057U;
    key ^= key >> 16;

    constexpr wchar_t alphabet[] = L"abcdefghijklmnopqrstuvwxyz0123456789";
    std::uint64_t state =
        (static_cast<std::uint64_t>(key) << 32) ^ 0x6A09E667F3BCC909ULL;
    std::wstring result;
    result.reserve(11);
    for (size_t index = 0; index < 11; ++index) {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        const std::uint64_t mixed = state * 0x2545F4914F6CDD1DULL;
        result.push_back(alphabet[(mixed >> 32) % (sizeof(alphabet) / sizeof(alphabet[0]) - 1)]);
    }
    return result;
}

void PrintWin32Error(const wchar_t* operation)
{
    std::wcerr << operation << L" failed, Win32=" << GetLastError() << L"\n";
}

HANDLE OpenDriver()
{
    return CreateFileW(
        kDevicePath,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
}

bool SendIoctl(
    HANDLE device,
    DWORD code,
    void* input,
    DWORD inputSize,
    void* output,
    DWORD outputSize,
    DWORD* returned = nullptr)
{
    DWORD localReturned = 0;
    return DeviceIoControl(
        device,
        code,
        input,
        inputSize,
        output,
        outputSize,
        returned != nullptr ? returned : &localReturned,
        nullptr) != FALSE;
}

bool RegisterController(HANDLE device)
{
    if (!SendIoctl(
            device,
            JIYU_AV_IOCTL_REGISTER_CONTROLLER,
            nullptr,
            0,
            nullptr,
            0)) {
        PrintWin32Error(L"register-controller");
        return false;
    }
    return true;
}

bool ParseUnsigned(const wchar_t* text, unsigned long* value)
{
    if (text == nullptr || value == nullptr || *text == L'\0' || *text == L'-') {
        return false;
    }
    wchar_t* end = nullptr;
    unsigned long parsed = wcstoul(text, &end, 0);
    if (text == end || end == nullptr || *end != L'\0') {
        return false;
    }
    *value = parsed;
    return true;
}

int HexNibble(wchar_t value)
{
    if (value >= L'0' && value <= L'9') return value - L'0';
    if (value >= L'a' && value <= L'f') return value - L'a' + 10;
    if (value >= L'A' && value <= L'F') return value - L'A' + 10;
    return -1;
}

bool ParseHash(const std::wstring& text, unsigned char* output)
{
    if (text.size() != JIYU_AV_SHA256_BYTES * 2UL) return false;
    for (size_t index = 0; index < JIYU_AV_SHA256_BYTES; ++index) {
        int high = HexNibble(text[index * 2]);
        int low = HexNibble(text[index * 2 + 1]);
        if (high < 0 || low < 0) return false;
        output[index] = static_cast<unsigned char>((high << 4) | low);
    }
    return true;
}

bool ParsePattern(
    const std::wstring& text,
    unsigned char* data,
    unsigned char* mask,
    unsigned long* length)
{
    std::wstring compact;
    for (wchar_t character : text) {
        if (character != L' ' && character != L'-' && character != L':') {
            compact.push_back(character);
        }
    }
    if ((compact.size() % 2) != 0 || compact.size() < 8 ||
        compact.size() > JIYU_AV_MAX_PATTERN_BYTES * 2UL) {
        return false;
    }

    *length = static_cast<unsigned long>(compact.size() / 2);
    unsigned long significantNibbles = 0UL;
    for (unsigned long index = 0; index < *length; ++index) {
        wchar_t highCharacter = compact[index * 2UL];
        wchar_t lowCharacter = compact[index * 2UL + 1UL];
        if (highCharacter == L'?' && lowCharacter == L'?') {
            data[index] = 0U;
            mask[index] = 0U;
            continue;
        }
        int high = HexNibble(highCharacter);
        int low = HexNibble(lowCharacter);
        if (highCharacter == L'?' && low >= 0) {
            data[index] = static_cast<unsigned char>(low);
            mask[index] = 0x0FU;
            ++significantNibbles;
        }
        else if (high >= 0 && lowCharacter == L'?') {
            data[index] = static_cast<unsigned char>(high << 4);
            mask[index] = 0xF0U;
            ++significantNibbles;
        }
        else if (high >= 0 && low >= 0) {
            data[index] = static_cast<unsigned char>((high << 4) | low);
            mask[index] = 0xFFU;
            significantNibbles += 2UL;
        }
        else {
            return false;
        }
    }
    return significantNibbles >= 8UL;
}

std::wstring BytesToHex(const unsigned char* data, size_t length)
{
    static constexpr wchar_t digits[] = L"0123456789ABCDEF";
    std::wstring result(length * 2, L'0');
    for (size_t index = 0; index < length; ++index) {
        result[index * 2] = digits[data[index] >> 4];
        result[index * 2 + 1] = digits[data[index] & 0x0F];
    }
    return result;
}

std::wstring PatternToText(const JIYU_AV_SIGNATURE_RECORD& record)
{
    static constexpr wchar_t digits[] = L"0123456789ABCDEF";
    std::wstring result(record.dataLength * 2UL, L'?');
    for (unsigned long index = 0; index < record.dataLength; ++index) {
        unsigned char value = record.data[index];
        unsigned char mask = record.mask[index];
        if ((mask & 0xF0U) != 0U) result[index * 2UL] = digits[value >> 4];
        if ((mask & 0x0FU) != 0U) result[index * 2UL + 1UL] = digits[value & 0x0FU];
    }
    return result;
}

std::wstring ToNtPath(const wchar_t* input)
{
    DWORD required = GetFullPathNameW(input, 0, nullptr, nullptr);
    if (required == 0) return {};
    std::vector<wchar_t> buffer(required + 1UL);
    if (GetFullPathNameW(input, static_cast<DWORD>(buffer.size()), buffer.data(), nullptr) == 0) {
        return {};
    }
    std::wstring path(buffer.data());
    if (path.rfind(L"\\\\", 0) == 0) {
        return L"\\??\\UNC\\" + path.substr(2);
    }
    return L"\\??\\" + path;
}

int InstallDriver(const wchar_t* driverPath, bool automatic)
{
    DWORD required = GetFullPathNameW(driverPath, 0, nullptr, nullptr);
    if (required == 0) {
        PrintWin32Error(L"GetFullPathName");
        return 1;
    }
    std::vector<wchar_t> path(required + 1UL);
    if (GetFullPathNameW(driverPath, static_cast<DWORD>(path.size()), path.data(), nullptr) == 0) {
        PrintWin32Error(L"GetFullPathName");
        return 1;
    }

    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (manager == nullptr) {
        PrintWin32Error(L"OpenSCManager");
        return 1;
    }
    const std::wstring serviceName = DateBasedServiceName();
    if (serviceName.empty()) {
        std::wcerr << L"could not derive a service name from the driver path\n";
        CloseServiceHandle(manager);
        return 1;
    }
    DWORD startType = automatic ? SERVICE_AUTO_START : SERVICE_DEMAND_START;
    SC_HANDLE service = CreateServiceW(
        manager,
        serviceName.c_str(),
        L"Kernel Scanner",
        SERVICE_START | SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG,
        SERVICE_KERNEL_DRIVER,
        startType,
        SERVICE_ERROR_NORMAL,
        path.data(),
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr);
    if (service == nullptr && GetLastError() == ERROR_SERVICE_EXISTS) {
        service = OpenServiceW(
            manager,
            serviceName.c_str(),
            SERVICE_START | SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG);
        if (service != nullptr && !ChangeServiceConfigW(
                service,
                SERVICE_KERNEL_DRIVER,
                startType,
                SERVICE_ERROR_NORMAL,
                path.data(),
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                L"Kernel Scanner")) {
            PrintWin32Error(L"ChangeServiceConfig");
            CloseServiceHandle(service);
            CloseServiceHandle(manager);
            return 1;
        }
    }
    if (service == nullptr) {
        PrintWin32Error(L"CreateService/OpenService");
        CloseServiceHandle(manager);
        return 1;
    }

    if (!StartServiceW(service, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        PrintWin32Error(L"StartService");
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        return 1;
    }
    std::wcout << L"installed and started " << serviceName
               << (automatic ? L" (automatic)\n" : L" (demand)\n");
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return 0;
}

int StopAndDeleteService(const wchar_t* serviceName)
{
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) {
        PrintWin32Error(L"OpenSCManager");
        return 1;
    }
    SC_HANDLE service = OpenServiceW(
        manager,
        serviceName,
        SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (service == nullptr) {
        PrintWin32Error(L"OpenService");
        CloseServiceHandle(manager);
        return 1;
    }
    SERVICE_STATUS status{};
    if (!ControlService(service, SERVICE_CONTROL_STOP, &status) &&
        GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
        PrintWin32Error(L"ControlService(STOP)");
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        return 1;
    }
    if (!DeleteService(service) && GetLastError() != ERROR_SERVICE_MARKED_FOR_DELETE) {
        PrintWin32Error(L"DeleteService");
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        return 1;
    }
    std::wcout << L"driver stopped and service deleted\n";
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return 0;
}

int RunSelfTest()
{
    JIYU_AV_SIGNATURE_RECORD record{};
    bool passed = ParsePattern(
        L"48 8B ?? A? ?F 90",
        record.data,
        record.mask,
        &record.dataLength);
    passed = passed && record.dataLength == 6UL;
    passed = passed && record.data[3] == 0xA0U && record.mask[3] == 0xF0U;
    passed = passed && record.data[4] == 0x0FU && record.mask[4] == 0x0FU;
    passed = passed && PatternToText(record) == L"488B??A??F90";

    unsigned char data[JIYU_AV_MAX_PATTERN_BYTES]{};
    unsigned char mask[JIYU_AV_MAX_PATTERN_BYTES]{};
    unsigned long length = 0UL;
    passed = passed && !ParsePattern(L"????????", data, mask, &length);
    passed = passed && !ParsePattern(L"123", data, mask, &length);
    std::wcout << (passed ? L"RESULT parser 7/7 passed\n" : L"RESULT parser failed\n");
    return passed ? 0 : 1;
}

void PrintUsage()
{
    std::wcout
        << L"JiYuAvCtl commands:\n"
        << L"  install <driver.sys> [auto]   (service name = today's AV identity)\n"
        << L"  query\n"
        << L"  stats\n"
        << L"  protect <pid>\n"
        << L"  add-hash <id> <name> <sha256>\n"
        << L"  add-pattern <id> <name> <hex-with-??-A?-?F-wildcards>\n"
        << L"  list\n"
        << L"  remove <id>\n"
        << L"  clear\n"
        << L"  scan <file>\n"
        << L"  selftest\n"
        << L"  unload [service-name]\n";
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) {
        PrintUsage();
        return 2;
    }
    std::wstring command = argv[1];
    std::transform(command.begin(), command.end(), command.begin(), towlower);
    if (command == L"install") {
        if (argc < 3) {
            PrintUsage();
            return 2;
        }
        return InstallDriver(argv[2], argc >= 4 && _wcsicmp(argv[3], L"auto") == 0);
    }
    if (command == L"selftest") {
        return RunSelfTest();
    }

    HANDLE device = OpenDriver();
    if (device == INVALID_HANDLE_VALUE) {
        PrintWin32Error(L"open \\.\\JiYuAv");
        return 1;
    }

    int exitCode = 0;
    if (command == L"query") {
        JIYU_AV_VERSION_RESPONSE response{};
        DWORD returned = 0;
        if (!SendIoctl(device, JIYU_AV_IOCTL_QUERY_VERSION, nullptr, 0,
                &response, sizeof(response), &returned)) {
            PrintWin32Error(L"query");
            exitCode = 1;
        }
        else {
            std::wcout << L"protocol=" << response.version
                       << L" architecture=x" << response.architecture
                       << L" max-signatures=" << response.maxSignatures
                       << L" max-pattern=" << response.maxPatternBytes << L"\n";
        }
    }
    else if (command == L"stats") {
        JIYU_AV_STATS_RESPONSE response{};
        if (!SendIoctl(device, JIYU_AV_IOCTL_QUERY_STATS, nullptr, 0,
                &response, sizeof(response))) {
            PrintWin32Error(L"stats");
            exitCode = 1;
        }
        else {
            std::wcout << L"signatures=" << response.signatureCount
                       << L" protected-pid=" << response.protectedProcessId
                       << L" files=" << response.filesScanned
                       << L" buffers=" << response.buffersScanned
                       << L" detections=" << response.detections
                       << L" bytes=" << response.bytesScanned << L"\n";
        }
    }
    else if (command == L"scan") {
        if (argc < 3) {
            PrintUsage();
            exitCode = 2;
        }
        else {
            std::wstring path = ToNtPath(argv[2]);
            JIYU_AV_SCAN_FILE_REQUEST request{};
            JIYU_AV_SCAN_RESULT result{};
            if (path.empty() || path.size() >= JIYU_AV_MAX_PATH_CHARS) {
                std::wcerr << L"invalid or overlong path\n";
                exitCode = 2;
            }
            else {
                request.size = sizeof(request);
                request.version = JIYU_AV_PROTOCOL_VERSION;
                request.pathLength = static_cast<unsigned long>(path.size());
                wmemcpy_s(request.path, JIYU_AV_MAX_PATH_CHARS, path.data(), path.size());
                if (!SendIoctl(device, JIYU_AV_IOCTL_SCAN_FILE,
                        &request, sizeof(request), &result, sizeof(result))) {
                    PrintWin32Error(L"scan");
                    exitCode = 1;
                }
                else {
                    std::wcout << (((result.flags & JIYU_AV_SCAN_FLAG_MATCHED) != 0)
                            ? L"DETECTED" : L"CLEAN")
                        << L" bytes=" << result.scannedBytes
                        << L" sha256=" << BytesToHex(result.sha256, JIYU_AV_SHA256_BYTES);
                    if ((result.flags & JIYU_AV_SCAN_FLAG_MATCHED) != 0) {
                        std::wcout << L" id=" << result.signatureId
                                   << L" type=" << result.signatureType
                                   << L" offset=" << result.matchOffset
                                   << L" name=" << result.signatureName;
                    }
                    std::wcout << L"\n";
                    exitCode = (result.flags & JIYU_AV_SCAN_FLAG_MATCHED) != 0 ? 10 : 0;
                }
            }
        }
    }
    else if (command == L"list") {
        JIYU_AV_STATS_RESPONSE stats{};
        if (!SendIoctl(device, JIYU_AV_IOCTL_QUERY_STATS, nullptr, 0,
                &stats, sizeof(stats))) {
            PrintWin32Error(L"list/stats");
            exitCode = 1;
        }
        else {
            for (unsigned long index = 0; index < stats.signatureCount; ++index) {
                JIYU_AV_SIGNATURE_QUERY_REQUEST request{};
                JIYU_AV_SIGNATURE_QUERY_RESPONSE response{};
                request.size = sizeof(request);
                request.version = JIYU_AV_PROTOCOL_VERSION;
                request.index = index;
                if (!SendIoctl(device, JIYU_AV_IOCTL_QUERY_SIGNATURE,
                        &request, sizeof(request), &response, sizeof(response))) {
                    PrintWin32Error(L"list/query-signature");
                    exitCode = 1;
                    break;
                }
                const JIYU_AV_SIGNATURE_RECORD& record = response.record;
                std::wcout << L"index=" << response.index
                           << L" id=" << record.signatureId
                           << L" type=" << (record.type == JIYU_AV_SIGNATURE_SHA256
                                ? L"sha256" : L"pattern")
                           << L" name=" << record.name
                           << L" value=" << (record.type == JIYU_AV_SIGNATURE_SHA256
                                ? BytesToHex(record.data, JIYU_AV_SHA256_BYTES)
                                : PatternToText(record)) << L"\n";
            }
            if (stats.signatureCount == 0UL) {
                std::wcout << L"no signatures\n";
            }
        }
    }
    else {
        if (!RegisterController(device)) {
            CloseHandle(device);
            return 1;
        }

        if (command == L"protect") {
            unsigned long pid = 0;
            JIYU_AV_PROCESS_REQUEST request{};
            if (argc < 3 || !ParseUnsigned(argv[2], &pid)) {
                PrintUsage();
                exitCode = 2;
            }
            else {
                request.size = sizeof(request);
                request.version = JIYU_AV_PROTOCOL_VERSION;
                request.processId = pid;
                if (!SendIoctl(device, JIYU_AV_IOCTL_SET_PROTECTED_PROCESS,
                        &request, sizeof(request), nullptr, 0)) {
                    PrintWin32Error(L"protect");
                    exitCode = 1;
                }
                else std::wcout << L"protected pid=" << pid << L"\n";
            }
        }
        else if (command == L"add-hash" || command == L"add-pattern") {
            unsigned long id = 0;
            JIYU_AV_SIGNATURE_RECORD record{};
            if (argc < 5 || !ParseUnsigned(argv[2], &id) || id == 0UL ||
                argv[3][0] == L'\0' || wcslen(argv[3]) >= JIYU_AV_MAX_SIGNATURE_NAME) {
                PrintUsage();
                exitCode = 2;
            }
            else {
                record.size = sizeof(record);
                record.version = JIYU_AV_PROTOCOL_VERSION;
                record.signatureId = id;
                wcsncpy_s(record.name, argv[3], _TRUNCATE);
                bool parsed;
                if (command == L"add-hash") {
                    record.type = JIYU_AV_SIGNATURE_SHA256;
                    record.dataLength = JIYU_AV_SHA256_BYTES;
                    parsed = ParseHash(argv[4], record.data);
                }
                else {
                    record.type = JIYU_AV_SIGNATURE_PATTERN;
                    parsed = ParsePattern(argv[4], record.data, record.mask, &record.dataLength);
                }
                if (!parsed) {
                    std::wcerr << L"invalid signature data\n";
                    exitCode = 2;
                }
                else if (!SendIoctl(device, JIYU_AV_IOCTL_ADD_SIGNATURE,
                        &record, sizeof(record), nullptr, 0)) {
                    PrintWin32Error(L"add-signature");
                    exitCode = 1;
                }
                else std::wcout << L"signature added id=" << id << L"\n";
            }
        }
        else if (command == L"remove") {
            unsigned long id = 0UL;
            JIYU_AV_SIGNATURE_ID_REQUEST request{};
            if (argc < 3 || !ParseUnsigned(argv[2], &id) || id == 0UL) {
                PrintUsage();
                exitCode = 2;
            }
            else {
                request.size = sizeof(request);
                request.version = JIYU_AV_PROTOCOL_VERSION;
                request.signatureId = id;
                if (!SendIoctl(device, JIYU_AV_IOCTL_REMOVE_SIGNATURE,
                        &request, sizeof(request), nullptr, 0)) {
                    PrintWin32Error(L"remove-signature");
                    exitCode = 1;
                }
                else std::wcout << L"signature removed id=" << id << L"\n";
            }
        }
        else if (command == L"clear") {
            if (!SendIoctl(device, JIYU_AV_IOCTL_CLEAR_SIGNATURES,
                    nullptr, 0, nullptr, 0)) {
                PrintWin32Error(L"clear");
                exitCode = 1;
            }
            else std::wcout << L"signatures cleared\n";
        }
        else if (command == L"unload") {
            if (!SendIoctl(device, JIYU_AV_IOCTL_ARM_UNLOAD,
                    nullptr, 0, nullptr, 0)) {
                PrintWin32Error(L"arm-unload");
                exitCode = 1;
            }
            else {
                CloseHandle(device);
                device = INVALID_HANDLE_VALUE;
                exitCode = StopAndDeleteService(
                    argc >= 3 ? argv[2] : DateBasedServiceName().c_str());
            }
        }
        else {
            PrintUsage();
            exitCode = 2;
        }
    }

    if (device != INVALID_HANDLE_VALUE) CloseHandle(device);
    return exitCode;
}
