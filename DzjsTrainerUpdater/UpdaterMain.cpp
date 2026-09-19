// DzjsTrainerUpdater.exe - standalone update program.
//
// This executable owns the whole update path. It talks to the update endpoint
// itself, downloads and verifies the package, then asks the user to close the
// main program and replaces it once the file is no longer locked. It never
// depends on the main program for downloading, applying or exiting.
//
// Command line:
//   (no arguments)          guided update flow
//   --target <path>         installed program to replace (default: this directory)
//   --source <path>         optional installer copy to refresh as well
//   --force                 install even when the published version is not newer
//   --status                print local file status and exit
//   --manifest <path>       fetch the manifest, write it to <path> and exit
//   --repl                  interactive command loop (status/check/download/update)
//   --no-elevate            do not self-elevate
//   --elevated              internal guard set by the self-elevation relaunch
//   --help                  usage

#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <wininet.h>
#include <bcrypt.h>
#include <io.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <cwchar>
#include <string>
#include <vector>
#include <winnt.h>
#include <iostream>
#include <cwctype>

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "Wininet.lib")
#pragma comment(lib, "Bcrypt.lib")
#pragma comment(lib, "Normaliz.lib")
#pragma comment(lib, "Version.lib")

namespace {

const wchar_t* const kUpdateHostSuffix = L".dzjstrainer.xn--9kq396ceqaq4si9m.cn/update-server/update.php";
const wchar_t* const kPayloadName = L"DzjsTrainerUpdatePayload.exe";
const wchar_t* const kTargetName = L"DzjsTrainer.exe";
const int kReplaceTimeoutSeconds = 600;

bool g_consoleOutput = false;

struct Manifest {
	std::string version;
	std::string notes;
	std::string url;
	std::string sha256;
};

// ---------------------------------------------------------------- text helpers

std::string Utf8FromWide(const std::wstring& text)
{
	if (text.empty()) return std::string();
	const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
		nullptr, 0, nullptr, nullptr);
	if (bytes <= 0) return std::string();
	std::string result(static_cast<size_t>(bytes), '\0');
	WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
		&result[0], bytes, nullptr, nullptr);
	return result;
}

std::wstring WideFromUtf8(const std::string& text)
{
	if (text.empty()) return std::wstring();
	const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
	if (length <= 0) return std::wstring();
	std::wstring result(static_cast<size_t>(length), L'\0');
	if (MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &result[0], length) != length)
		return std::wstring();
	return result;
}

void PrintUtf8(const std::wstring& text)
{
	std::string bytes = Utf8FromWide(text);
	// stdout is in binary mode so UTF-8 bytes survive; a console still needs the
	// carriage return that text mode would have added.
	if (g_consoleOutput) {
		std::string expanded;
		expanded.reserve(bytes.size() + 16);
		for (char ch : bytes) {
			if (ch == '\n') expanded.push_back('\r');
			expanded.push_back(ch);
		}
		bytes.swap(expanded);
	}
	std::cout << bytes << std::flush;
}

// Overwrites the current line so a progress indicator does not scroll.
void PrintProgressLine(const std::wstring& text)
{
	static size_t lastWidth = 0;
	std::wstring line = L"\r" + text;
	if (line.size() < lastWidth) line.append(lastWidth - line.size(), L' ');
	lastWidth = line.size() - 1;
	PrintUtf8(line);
}

void EndProgressLine()
{
	PrintUtf8(L"\n");
}

std::string Trim(std::string text)
{
	size_t first = 0;
	while (first < text.size() && static_cast<unsigned char>(text[first]) <= ' ') ++first;
	size_t last = text.size();
	while (last > first && static_cast<unsigned char>(text[last - 1]) <= ' ') --last;
	return text.substr(first, last - first);
}

std::wstring NormalizeCommand(std::wstring command)
{
	size_t first = 0;
	while (first < command.size() && iswspace(command[first])) ++first;
	size_t last = command.size();
	while (last > first && iswspace(command[last - 1])) --last;
	command = command.substr(first, last - first);
	for (wchar_t& ch : command) ch = static_cast<wchar_t>(towlower(ch));
	return command;
}

std::wstring FormatBytes(unsigned long long bytes)
{
	wchar_t buffer[64] = {};
	if (bytes >= 1024ull * 1024ull)
		swprintf_s(buffer, L"%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
	else if (bytes >= 1024ull)
		swprintf_s(buffer, L"%.0f KB", static_cast<double>(bytes) / 1024.0);
	else
		swprintf_s(buffer, L"%llu 字节", bytes);
	return buffer;
}

// ------------------------------------------------------------------ path utils

std::wstring ModuleDirectory()
{
	wchar_t path[MAX_PATH] = {};
	const DWORD length = GetModuleFileNameW(nullptr, path, _countof(path));
	if (!length || length >= _countof(path)) return std::wstring();
	std::wstring result(path, length);
	const size_t slash = result.find_last_of(L"\\/");
	return slash == std::wstring::npos ? std::wstring() : result.substr(0, slash);
}

std::wstring DirectoryOf(const std::wstring& path)
{
	const size_t slash = path.find_last_of(L"\\/");
	return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

bool SamePath(const std::wstring& left, const std::wstring& right)
{
	wchar_t a[MAX_PATH] = {}, b[MAX_PATH] = {};
	GetFullPathNameW(left.c_str(), _countof(a), a, nullptr);
	GetFullPathNameW(right.c_str(), _countof(b), b, nullptr);
	return _wcsicmp(a, b) == 0;
}

bool IsPeImage(const std::wstring& path)
{
	HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	IMAGE_DOS_HEADER dos = {};
	DWORD read = 0;
	bool valid = ReadFile(file, &dos, sizeof(dos), &read, nullptr) && read == sizeof(dos) && dos.e_magic == IMAGE_DOS_SIGNATURE;
	if (valid && SetFilePointer(file, dos.e_lfanew, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER) {
		DWORD signature = 0;
		valid = ReadFile(file, &signature, sizeof(signature), &read, nullptr) && read == sizeof(signature) && signature == IMAGE_NT_SIGNATURE;
	} else {
		valid = false;
	}
	CloseHandle(file);
	return valid;
}

bool ReadFileVersion(const std::wstring& path, std::wstring& version)
{
	version.clear();
	DWORD ignored = 0;
	const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
	if (!size) return false;
	std::vector<BYTE> data(size);
	if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return false;
	VS_FIXEDFILEINFO* info = nullptr;
	UINT length = 0;
	if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<LPVOID*>(&info), &length) || !info) return false;
	wchar_t buffer[64] = {};
	swprintf_s(buffer, L"%u.%u.%u.%u",
		HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
		HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
	version = buffer;
	return true;
}

// ---------------------------------------------------------------- URL handling

// The manifest carries the download URL as UTF-8 and the host may use a
// non-ASCII domain (for example 云散皆星河.cn). Decoding that URL with CP_ACP
// corrupts the host, and the ANSI URL APIs cannot resolve a Unicode host, so the
// request would fail no matter how it is retried. Decode as UTF-8 and convert
// the host to its ASCII/punycode form so everything stays ASCII.
std::wstring WideUrlFromUtf8(const std::string& url)
{
	if (url.empty()) return std::wstring();
	std::wstring wide = WideFromUtf8(url);
	if (wide.empty()) return wide;

	const size_t schemeEnd = wide.find(L"://");
	if (schemeEnd == std::wstring::npos) return wide;
	const size_t hostBegin = schemeEnd + 3;
	const size_t hostEnd = wide.find_first_of(L"/?#", hostBegin);
	const std::wstring host = hostEnd == std::wstring::npos
		? wide.substr(hostBegin) : wide.substr(hostBegin, hostEnd - hostBegin);
	if (host.empty()) return wide;

	// Keep an explicit port out of the IDN conversion.
	size_t portBegin = host.rfind(L':');
	if (portBegin != std::wstring::npos) {
		bool numeric = portBegin + 1 < host.size();
		for (size_t i = portBegin + 1; i < host.size() && numeric; ++i)
			if (host[i] < L'0' || host[i] > L'9') numeric = false;
		if (!numeric) portBegin = std::wstring::npos;
	}
	const std::wstring name = portBegin == std::wstring::npos ? host : host.substr(0, portBegin);
	const std::wstring port = portBegin == std::wstring::npos ? std::wstring() : host.substr(portBegin);

	bool asciiOnly = true;
	for (size_t i = 0; i < name.size(); ++i) if (name[i] > 0x7f) { asciiOnly = false; break; }
	if (asciiOnly) return wide;

	const int asciiLength = IdnToAscii(0, name.c_str(), static_cast<int>(name.size()), nullptr, 0);
	if (asciiLength <= 0) return wide;
	std::wstring asciiName(static_cast<size_t>(asciiLength) + 1, L'\0');
	if (IdnToAscii(0, name.c_str(), static_cast<int>(name.size()), &asciiName[0], static_cast<int>(asciiName.size())) <= 0)
		return wide;
	asciiName.resize(wcsnlen_s(asciiName.c_str(), asciiName.size()));

	const size_t tailBegin = hostEnd == std::wstring::npos ? wide.size() : hostEnd;
	return wide.substr(0, hostBegin) + asciiName + port + wide.substr(tailBegin);
}

std::wstring RandomPrefix()
{
	unsigned char bytes[4] = {};
	if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
		const ULONGLONG seed = GetTickCount64() ^ (static_cast<ULONGLONG>(GetCurrentProcessId()) << 32) ^
			static_cast<ULONGLONG>(GetCurrentThreadId());
		memcpy(bytes, &seed, sizeof(bytes));
	}
	static const wchar_t hex[] = L"0123456789abcdef";
	std::wstring prefix;
	prefix.reserve(8);
	for (unsigned char byte : bytes) {
		prefix.push_back(hex[byte >> 4]);
		prefix.push_back(hex[byte & 0x0f]);
	}
	return prefix;
}

// The wildcard host is convenient but not always reachable, so every request
// walks the same candidate list the old client used: two randomized subdomains
// and then the stable apex host as a fallback.
std::wstring ApexEndpoint()
{
	return std::wstring(L"https://") + (kUpdateHostSuffix + 1);
}

std::vector<std::wstring> UpdateEndpoints()
{
	std::vector<std::wstring> endpoints;
	endpoints.reserve(3);
	endpoints.push_back(std::wstring(L"https://") + RandomPrefix() + kUpdateHostSuffix);
	endpoints.push_back(std::wstring(L"https://") + RandomPrefix() + kUpdateHostSuffix);
	endpoints.push_back(ApexEndpoint());
	return endpoints;
}

// Relative manifest URLs resolve against the apex endpoint so the resolved
// address stays stable for the whole update.
std::wstring ResolveDownloadUrl(const std::string& url)
{
	std::string text = Trim(url);
	if (text.empty()) return std::wstring();
	if (text.compare(0, 7, "http://") != 0 && text.compare(0, 8, "https://") != 0) {
		std::wstring base = ApexEndpoint();
		const size_t slash = base.find_last_of(L'/');
		if (slash != std::wstring::npos) base.resize(slash + 1);
		return base + WideFromUtf8(text);
	}
	return WideUrlFromUtf8(text);
}

// ------------------------------------------------------------------- HTTP

bool HttpGetW(const std::wstring& url, std::string& body)
{
	body.clear();
	HINTERNET session = InternetOpenW(L"DzjsTrainerUpdater/1.0", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
	if (!session) return false;
	DWORD timeout = 8000;
	InternetSetOptionW(session, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
	InternetSetOptionW(session, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

	// Certificate validation is intentionally left enabled: the endpoint serves a
	// valid wildcard certificate and the package hash travels over this channel.
	HINTERNET request = InternetOpenUrlW(session, url.c_str(), nullptr, 0,
		INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_SECURE, 0);
	bool ok = false;
	if (request) {
		char buffer[8192];
		DWORD read = 0;
		while (InternetReadFile(request, buffer, sizeof(buffer), &read) && read)
			body.append(buffer, read);
		DWORD status = 0, statusSize = sizeof(status);
		ok = HttpQueryInfoW(request, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &statusSize, nullptr) != FALSE
			&& status >= 200 && status < 300;
		InternetCloseHandle(request);
	}
	InternetCloseHandle(session);
	if (!ok) body.clear();
	return ok;
}

bool DownloadToFileOnce(const std::wstring& url, const std::wstring& path, std::wstring& error)
{
	error.clear();
	HINTERNET session = InternetOpenW(L"DzjsTrainerUpdater/1.0", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
	if (!session) { error = L"无法初始化网络会话"; return false; }
	DWORD timeout = 15000;
	InternetSetOptionW(session, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
	InternetSetOptionW(session, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

	HINTERNET request = InternetOpenUrlW(session, url.c_str(), nullptr, 0,
		INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_SECURE, 0);
	if (!request) {
		error = L"无法连接下载地址（错误码 " + std::to_wstring(GetLastError()) + L"）";
		InternetCloseHandle(session);
		return false;
	}
	DWORD status = 0, statusSize = sizeof(status);
	if (!HttpQueryInfoW(request, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &statusSize, nullptr) ||
		status < 200 || status >= 300) {
		error = L"下载地址返回 HTTP " + std::to_wstring(status);
		InternetCloseHandle(request);
		InternetCloseHandle(session);
		return false;
	}
	unsigned long long total = 0;
	{
		wchar_t header[32] = {};
		DWORD headerSize = sizeof(header);
		if (HttpQueryInfoW(request, HTTP_QUERY_CONTENT_LENGTH, header, &headerSize, nullptr))
			total = _wcstoui64(header, nullptr, 10);
	}

	HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		error = L"无法写入 " + path;
		InternetCloseHandle(request);
		InternetCloseHandle(session);
		return false;
	}

	bool ok = true;
	unsigned long long written = 0;
	std::vector<char> buffer(64 * 1024);
	for (;;) {
		DWORD read = 0;
		if (!InternetReadFile(request, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) {
			error = L"读取下载数据失败（错误码 " + std::to_wstring(GetLastError()) + L"）";
			ok = false;
			break;
		}
		if (read == 0) break;
		DWORD offset = 0;
		while (offset < read) {
			DWORD chunk = 0;
			if (!WriteFile(file, buffer.data() + offset, read - offset, &chunk, nullptr) || chunk == 0) {
				error = L"写入更新包失败（错误码 " + std::to_wstring(GetLastError()) + L"）";
				ok = false;
				break;
			}
			offset += chunk;
		}
		if (!ok) break;
		written += read;
		std::wstring line = L"已下载 " + FormatBytes(written);
		if (total) line += L" / " + FormatBytes(total) + L" (" + std::to_wstring(written * 100 / total) + L"%)";
		PrintProgressLine(line);
	}
	InternetCloseHandle(request);
	InternetCloseHandle(session);
	CloseHandle(file);
	EndProgressLine();
	if (ok) PrintUtf8(L"下载完成：" + FormatBytes(written) + L"\n");
	if (!ok) DeleteFileW(path.c_str());
	return ok;
}

// The package host is a wildcard subdomain, so a single transient failure is
// expected; retry before giving up.
bool DownloadToFile(const std::wstring& url, const std::wstring& path, std::wstring& error)
{
	for (int attempt = 1; attempt <= 3; ++attempt) {
		if (DownloadToFileOnce(url, path, error)) return true;
		if (attempt < 3) {
			PrintUtf8(L"下载中断：" + error + L"，正在重试...\n");
			Sleep(1000);
		}
	}
	return false;
}

// ------------------------------------------------------------------- manifest

bool JsonString(const std::string& json, const char* key, std::string& value)
{
	const std::string needle = std::string("\"") + key + "\"";
	size_t pos = json.find(needle);
	if (pos == std::string::npos) return false;
	pos = json.find(':', pos + needle.size());
	if (pos == std::string::npos) return false;
	++pos;
	while (pos < json.size() && static_cast<unsigned char>(json[pos]) <= ' ') ++pos;
	if (pos >= json.size() || json[pos] != '"') return false;

	std::string result;
	for (++pos; pos < json.size(); ++pos) {
		const char ch = json[pos];
		if (ch == '"') { value = result; return true; }
		if (ch != '\\') { result.push_back(ch); continue; }
		if (pos + 1 >= json.size()) break;
		const char escape = json[++pos];
		switch (escape) {
		case 'n': result.push_back('\n'); break;
		case 't': result.push_back('\t'); break;
		case 'r': result.push_back('\r'); break;
		case 'b': result.push_back('\b'); break;
		case 'f': result.push_back('\f'); break;
		case '/': result.push_back('/'); break;
		case '\\': result.push_back('\\'); break;
		case '"': result.push_back('"'); break;
		case 'u': {
			if (pos + 4 >= json.size()) return false;
			unsigned code = 0;
			for (int i = 1; i <= 4; ++i) {
				const char digit = json[pos + i];
				unsigned value2 = 0;
				if (digit >= '0' && digit <= '9') value2 = static_cast<unsigned>(digit - '0');
				else if (digit >= 'a' && digit <= 'f') value2 = static_cast<unsigned>(digit - 'a' + 10);
				else if (digit >= 'A' && digit <= 'F') value2 = static_cast<unsigned>(digit - 'A' + 10);
				else return false;
				code = (code << 4) | value2;
			}
			pos += 4;
			if (code < 0x80) {
				result.push_back(static_cast<char>(code));
			} else if (code < 0x800) {
				result.push_back(static_cast<char>(0xC0 | (code >> 6)));
				result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
			} else {
				result.push_back(static_cast<char>(0xE0 | (code >> 12)));
				result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
				result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
			}
			break;
		}
		default: result.push_back(escape); break;
		}
	}
	return false;
}

bool HttpGetAny(const std::wstring& query, std::string& body)
{
	for (const std::wstring& endpoint : UpdateEndpoints()) {
		if (HttpGetW(endpoint + query, body)) return true;
	}
	body.clear();
	return false;
}

bool FetchManifest(Manifest& manifest)
{
	std::string body;
	if (HttpGetAny(L"?manifest=1", body)) {
		JsonString(body, "version", manifest.version);
		JsonString(body, "notes", manifest.notes);
		JsonString(body, "url", manifest.url);
		JsonString(body, "sha256", manifest.sha256);
	}
	// The plain-text query parameters remain available on the server, so keep
	// supporting deployments that have not published a manifest yet.
	if (manifest.version.empty() && HttpGetAny(L"?getnewver", body))
		manifest.version = Trim(body);
	if (manifest.notes.empty() && HttpGetAny(L"?getupdateinfo", body))
		manifest.notes = Trim(body);
	if (manifest.url.empty() && HttpGetAny(L"?getupdate", body))
		manifest.url = Trim(body);
	return !manifest.version.empty() && !manifest.url.empty();
}

// 把清单写成 key=value 文本，供主程序读取。
// 说明里的换行转义成 \n，保证一行一个字段，解析端不用处理多行。
static bool WriteManifestFile(const std::wstring& path, const Manifest& manifest)
{
	std::string text;
	text += "version=" + manifest.version + "\n";
	text += "sha256=" + manifest.sha256 + "\n";
	text += "url=" + manifest.url + "\n";
	text += "notes=";
	for (char ch : manifest.notes) {
		if (ch == '\r') continue;
		if (ch == '\n') text += '\\n';
		else text.push_back(ch);
	}
	text += "\n";

	HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	DWORD written = 0;
	const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr)
		&& written == text.size();
	FlushFileBuffers(file);
	CloseHandle(file);
	if (!ok) DeleteFileW(path.c_str());
	return ok;
}

// --------------------------------------------------------------- verification

// Missing components count as zero, so 1.0.5 and 1.0.5.0 compare equal. The
// installed file reports four components while the manifest usually publishes
// three, and treating those as different would re-download on every check.
std::vector<long> VersionParts(const std::wstring& text)
{
	std::vector<long> parts;
	const wchar_t* cursor = text.c_str();
	while (*cursor) {
		wchar_t* end = nullptr;
		const long value = wcstol(cursor, &end, 10);
		if (end == cursor) break;
		parts.push_back(value);
		cursor = end;
		if (*cursor == L'.') ++cursor;
		else if (*cursor != L'\0') break;
	}
	return parts;
}

int CompareVersions(const std::wstring& left, const std::wstring& right)
{
	std::vector<long> a = VersionParts(left);
	std::vector<long> b = VersionParts(right);
	const size_t count = a.size() > b.size() ? a.size() : b.size();
	a.resize(count, 0);
	b.resize(count, 0);
	for (size_t i = 0; i < count; ++i) {
		if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
	}
	return 0;
}

bool FileSha256(const std::wstring& path, std::string& result)
{
	HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	BCRYPT_ALG_HANDLE algorithm = nullptr;
	BCRYPT_HASH_HANDLE hash = nullptr;
	DWORD objectSize = 0, digestSize = 0, bytes = 0;
	bool ok = false;
	if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
		BCRYPT_SUCCESS(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &bytes, 0)) &&
		BCRYPT_SUCCESS(BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&digestSize), sizeof(digestSize), &bytes, 0))) {
		std::vector<BYTE> object(objectSize), digest(digestSize), buffer(64 * 1024);
		if (BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0))) {
			ok = true;
			DWORD read = 0;
			while (ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read) {
				if (!BCRYPT_SUCCESS(BCryptHashData(hash, buffer.data(), read, 0))) { ok = false; break; }
			}
			if (ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest.data(), digestSize, 0))) {
				static const char hex[] = "0123456789abcdef";
				result.clear();
				for (BYTE byte : digest) {
					result.push_back(hex[byte >> 4]);
					result.push_back(hex[byte & 15]);
				}
			} else {
				ok = false;
			}
		}
	}
	if (hash) BCryptDestroyHash(hash);
	if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
	CloseHandle(file);
	return ok;
}

// ------------------------------------------------------------------ replacing

// MoveFileEx cannot replace a running image, so wait for the user to close the
// main program and keep retrying until the file is no longer locked.
bool WaitAndReplace(const std::wstring& payload, const std::wstring& target, int timeoutSeconds)
{
	const DWORD start = GetTickCount();
	for (;;) {
		if (MoveFileExW(payload.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			return true;
		const DWORD error = GetLastError();
		if (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED && error != ERROR_USER_MAPPED_FILE) {
			PrintUtf8(L"\n替换失败：错误码 " + std::to_wstring(error) + L"\n");
			return false;
		}
		const DWORD elapsed = (GetTickCount() - start) / 1000;
		if (timeoutSeconds > 0 && elapsed >= static_cast<DWORD>(timeoutSeconds)) {
			PrintProgressLine(L"");
			PrintUtf8(L"等待超时：主程序仍在运行，未替换。\n");
			return false;
		}
		PrintProgressLine(L"等待主程序退出... " + std::to_wstring(elapsed) + L" 秒（按 Ctrl+C 取消）");
		Sleep(500);
	}
}

bool CopyWithRetry(const std::wstring& payload, const std::wstring& destination, int timeoutSeconds)
{
	const DWORD start = GetTickCount();
	for (;;) {
		if (CopyFileW(payload.c_str(), destination.c_str(), FALSE)) return true;
		const DWORD elapsed = (GetTickCount() - start) / 1000;
		if (timeoutSeconds > 0 && elapsed >= static_cast<DWORD>(timeoutSeconds)) return false;
		Sleep(500);
	}
}

// ------------------------------------------------------------------ elevation

bool IsElevated()
{
	HANDLE token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
	TOKEN_ELEVATION elevation = {};
	DWORD size = sizeof(elevation);
	const bool ok = GetTokenInformation(token, TokenElevation, &elevation, size, &size) != FALSE;
	CloseHandle(token);
	return ok && elevation.TokenIsElevated != 0;
}

bool RelaunchElevated(const std::wstring& parameters)
{
	wchar_t path[MAX_PATH] = {};
	if (!GetModuleFileNameW(nullptr, path, _countof(path))) return false;
	SHELLEXECUTEINFOW info = {};
	info.cbSize = sizeof(info);
	info.fMask = SEE_MASK_NOCLOSEPROCESS;
	info.lpVerb = L"runas";
	info.lpFile = path;
	info.lpParameters = parameters.c_str();
	info.nShow = SW_SHOWNORMAL;
	if (!ShellExecuteExW(&info)) {
		if (GetLastError() == ERROR_CANCELLED)
			PrintUtf8(L"已取消管理员授权，更新程序退出。\n");
		else
			PrintUtf8(L"无法获取管理员权限（错误码 " + std::to_wstring(GetLastError()) + L"）。\n");
		return false;
	}
	if (info.hProcess) CloseHandle(info.hProcess);
	return true;
}

// --------------------------------------------------------------- guided flow

struct Options {
	std::wstring target;
	std::wstring source;
	// --manifest <path>：只取清单写到指定文件后退出。主程序用它来探测新版本 ——
	// 主程序是 GUI 进程，实测在那里面调 WinINet 会 ERROR_INTERNET_CANNOT_CONNECT，
	// 而更新器是控制台进程，同样的调用却正常。
	std::wstring manifestPath;
	bool force = false;
	bool repl = false;
	bool status = false;
	bool help = false;
	bool noElevate = false;
	bool elevated = false;
};

std::wstring DefaultTarget()
{
	const std::wstring directory = ModuleDirectory();
	return directory.empty() ? std::wstring() : directory + L"\\" + kTargetName;
}

std::wstring PayloadPathFor(const std::wstring& target)
{
	const std::wstring directory = DirectoryOf(target);
	return (directory.empty() ? ModuleDirectory() : directory) + L"\\" + kPayloadName;
}

bool FileSize(const std::wstring& path, unsigned long long& size)
{
	WIN32_FILE_ATTRIBUTE_DATA attributes = {};
	if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)) return false;
	ULARGE_INTEGER value = {};
	value.HighPart = attributes.nFileSizeHigh;
	value.LowPart = attributes.nFileSizeLow;
	size = value.QuadPart;
	return true;
}

void PrintFileStatus(const std::wstring& path, const wchar_t* label)
{
	unsigned long long size = 0;
	if (!FileSize(path, size)) {
		PrintUtf8(std::wstring(label) + L": 不存在\n");
		return;
	}
	std::wstring line = std::wstring(label) + L": " + FormatBytes(size);
	line += IsPeImage(path) ? L" (PE)\n" : L" (不是 PE)\n";
	PrintUtf8(line);
}

void PrintStatus(const std::wstring& target)
{
	const std::wstring payload = PayloadPathFor(target);
	std::wstring version;
	if (ReadFileVersion(target, version))
		PrintUtf8(L"主程序: " + target + L"（版本 " + version + L"）\n");
	else
		PrintUtf8(L"主程序: " + target + L"（未找到或无法读取版本）\n");
	PrintFileStatus(payload, L"更新包");
}

// A package must be a real executable and match the published hash before it is
// allowed anywhere near the installed program.
bool VerifyPayload(const std::wstring& payload, const Manifest& manifest, bool verbose)
{
	if (!IsPeImage(payload)) {
		PrintUtf8(L"更新包不是有效的可执行文件，已删除。\n");
		DeleteFileW(payload.c_str());
		return false;
	}
	const std::string expected = Trim(manifest.sha256);
	if (expected.empty()) {
		if (verbose) PrintUtf8(L"服务端未提供 SHA-256，跳过校验。\n");
		return true;
	}
	std::string actual;
	if (!FileSha256(payload, actual) || _stricmp(actual.c_str(), expected.c_str()) != 0) {
		PrintUtf8(L"更新包 SHA-256 校验失败，已删除。\n");
		if (verbose) PrintUtf8(L"  期望: " + WideFromUtf8(expected) + L"\n  实际: " + WideFromUtf8(actual) + L"\n");
		DeleteFileW(payload.c_str());
		return false;
	}
	if (verbose) PrintUtf8(L"SHA-256 校验通过。\n");
	return true;
}

bool CheckForUpdate(const std::wstring& target, Manifest& manifest, std::wstring& installedVersion)
{
	PrintUtf8(L"正在检查更新...\n");
	if (!FetchManifest(manifest)) {
		PrintUtf8(L"无法连接更新服务，请检查网络后重试。\n");
		return false;
	}
	ReadFileVersion(target, installedVersion);
	return true;
}

int RunGuided(const Options& options)
{
	const std::wstring target = options.target.empty() ? DefaultTarget() : options.target;
	if (target.empty()) {
		PrintUtf8(L"无法确定要更新的主程序位置，请用 --target 指定。\n");
		return 3;
	}
	PrintUtf8(L"Dzjs Trainer 更新程序\n目标程序: " + target + L"\n\n");

	if (!InternetGetConnectedState(nullptr, 0)) {
		PrintUtf8(L"当前没有可用的网络连接。\n");
		return 4;
	}

	Manifest manifest;
	if (!FetchManifest(manifest)) {
		PrintUtf8(L"无法连接更新服务，请检查网络后重试。\n");
		return 4;
	}

	std::wstring installed;
	ReadFileVersion(target, installed);
	const std::wstring available = WideFromUtf8(manifest.version);
	PrintUtf8(L"当前版本: " + (installed.empty() ? std::wstring(L"未知") : installed) + L"\n");
	PrintUtf8(L"最新版本: " + available + L"\n");

	if (!options.force && !installed.empty() && CompareVersions(installed, available) >= 0) {
		PrintUtf8(L"当前已是最新版本。\n");
		return 0;
	}

	if (!manifest.notes.empty()) {
		PrintUtf8(L"\n更新说明:\n");
		PrintUtf8(WideFromUtf8(manifest.notes) + L"\n");
	}
	PrintUtf8(L"\n发现新版本 " + available + L"，是否下载并安装？[Y/n] ");
	std::string answer;
	if (!std::getline(std::cin, answer)) return 0;
	const std::wstring choice = NormalizeCommand(WideFromUtf8(answer));
	if (!choice.empty() && choice != L"y" && choice != L"yes") {
		PrintUtf8(L"已取消。\n");
		return 0;
	}

	const std::wstring payload = PayloadPathFor(target);
	const std::wstring downloadUrl = ResolveDownloadUrl(manifest.url);
	if (downloadUrl.empty()) {
		PrintUtf8(L"更新地址无效。\n");
		return 5;
	}
	DeleteFileW(payload.c_str());
	std::wstring error;
	PrintUtf8(L"\n正在下载更新包...\n");
	if (!DownloadToFile(downloadUrl, payload, error)) {
		PrintUtf8(L"下载失败：" + error + L"\n");
		return 6;
	}
	if (!VerifyPayload(payload, manifest, true)) return 6;

	if (!options.source.empty() && !SamePath(options.source, target)) {
		PrintUtf8(L"正在更新来源安装包 " + options.source + L"...\n");
		if (!CopyWithRetry(payload, options.source, kReplaceTimeoutSeconds))
			PrintUtf8(L"来源安装包更新失败，已跳过。\n");
	}

	PrintUtf8(L"\n请关闭 Dzjs Trainer 主程序（托盘图标右键退出）。\n关闭后会自动继续替换。\n\n");
	if (!WaitAndReplace(payload, target, kReplaceTimeoutSeconds)) {
		PrintUtf8(L"更新包保留在: " + payload + L"\n");
		PrintUtf8(L"关闭主程序后可运行 DzjsTrainerUpdater.exe --repl，输入 update 手动完成替换。\n");
		return 7;
	}
	EndProgressLine();
	PrintUtf8(L"更新完成，正在启动 Dzjs Trainer...\n");
	ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	return 0;
}

// ---------------------------------------------------------------- interactive

bool HasRedirectedStandardInput()
{
	const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
	if (input == nullptr || input == INVALID_HANDLE_VALUE) return false;
	const DWORD type = GetFileType(input);
	return type == FILE_TYPE_PIPE || type == FILE_TYPE_DISK;
}

bool BindInheritedHandleToCrt(HANDLE handle, int fd)
{
	if (handle == nullptr || handle == INVALID_HANDLE_VALUE) return false;
	const intptr_t osHandle = reinterpret_cast<intptr_t>(handle);
	const int crtHandle = _open_osfhandle(osHandle, _O_BINARY);
	if (crtHandle < 0) return false;
	if (_dup2(crtHandle, fd) != 0) {
		_close(crtHandle);
		return false;
	}
	_close(crtHandle);
	return true;
}

void ConfigureInteractiveConsole(bool redirectedInput)
{
	if (redirectedInput) return;
	const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
	if (input == nullptr || input == INVALID_HANDLE_VALUE) return;
	DWORD mode = 0;
	if (!GetConsoleMode(input, &mode)) return;
	// Keep QuickEdit enabled so the console supports the normal mouse selection,
	// copy, and paste operations. Line input still prevents accidental key
	// events from being interpreted as commands until Enter is pressed.
	mode |= ENABLE_EXTENDED_FLAGS | ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_QUICK_EDIT_MODE;
	SetConsoleMode(input, mode);
}

void PrepareConsole()
{
	// Capture redirection before AttachConsole: attaching can replace the
	// inherited pipe handles with console handles.
	const bool redirectedInput = HasRedirectedStandardInput();
	const bool attached = redirectedInput ? false : (AttachConsole(ATTACH_PARENT_PROCESS) != FALSE);
	if (!attached && !redirectedInput) {
		AllocConsole();
		FILE* input = nullptr;
		FILE* output = nullptr;
		_wfreopen_s(&input, L"CONIN$", L"r", stdin);
		_wfreopen_s(&output, L"CONOUT$", L"w", stdout);
	} else {
		// GUI-subsystem processes do not initialize CRT stdio from inherited
		// handles. Bind those handles explicitly so pipes and attached consoles
		// both work.
		if (redirectedInput) {
			BindInheritedHandleToCrt(GetStdHandle(STD_INPUT_HANDLE), _fileno(stdin));
		} else {
			FILE* input = nullptr;
			_wfreopen_s(&input, L"CONIN$", L"r", stdin);
		}
		HANDLE outputHandle = GetStdHandle(STD_OUTPUT_HANDLE);
		if (outputHandle && outputHandle != INVALID_HANDLE_VALUE) {
			BindInheritedHandleToCrt(outputHandle, _fileno(stdout));
		} else {
			FILE* output = nullptr;
			_wfreopen_s(&output, L"CONOUT$", L"w", stdout);
		}
	}
	// Emit UTF-8 bytes consistently. A real console is switched to UTF-8 and
	// needs CRLF; redirected output stays plain UTF-8 with LF for scripts.
	_setmode(_fileno(stdout), _O_BINARY);
	g_consoleOutput = !redirectedInput;
	if (!redirectedInput) SetConsoleOutputCP(CP_UTF8);
	ConfigureInteractiveConsole(redirectedInput);
	std::ios::sync_with_stdio(false);
}

int RunInteractive(const Options& options)
{
	const std::wstring target = options.target.empty() ? DefaultTarget() : options.target;
	const std::wstring payload = PayloadPathFor(target);
	PrintUtf8(L"Dzjs Trainer 更新程序\n");
	PrintUtf8(L"命令: status 查看文件, check 检查更新, download 下载更新包, update 执行替换, help 帮助, q 退出\n");
	std::string commandBytes;
	for (;;) {
		PrintUtf8(L"updater> ");
		if (!std::getline(std::cin, commandBytes)) break;
		const std::wstring command = NormalizeCommand(WideFromUtf8(commandBytes));
		if (command == L"q" || command == L"quit" || command == L"exit") break;
		if (command == L"help" || command == L"?") {
			PrintUtf8(L"status | check | download | update | help | q\n");
			continue;
		}
		if (command == L"status") {
			PrintStatus(target);
			continue;
		}
		if (command == L"check") {
			Manifest manifest;
			std::wstring installed;
			if (!CheckForUpdate(target, manifest, installed)) continue;
			PrintUtf8(L"当前版本: " + (installed.empty() ? std::wstring(L"未知") : installed) + L"\n");
			PrintUtf8(L"最新版本: " + WideFromUtf8(manifest.version) + L"\n");
			if (!installed.empty() && CompareVersions(installed, WideFromUtf8(manifest.version)) >= 0)
				PrintUtf8(L"当前已是最新版本。\n");
			continue;
		}
		if (command == L"download") {
			Manifest manifest;
			std::wstring installed;
			if (!CheckForUpdate(target, manifest, installed)) continue;
			const std::wstring url = ResolveDownloadUrl(manifest.url);
			if (url.empty()) { PrintUtf8(L"更新地址无效。\n"); continue; }
			DeleteFileW(payload.c_str());
			std::wstring error;
			if (!DownloadToFile(url, payload, error)) { PrintUtf8(L"下载失败：" + error + L"\n"); continue; }
			if (VerifyPayload(payload, manifest, true)) PrintUtf8(L"更新包已保存到 " + payload + L"\n");
			continue;
		}
		if (command == L"update") {
			if (!PathFileExistsW(payload.c_str()) || !IsPeImage(payload.c_str())) {
				PrintUtf8(L"本地没有待替换的更新包。请先执行 download，或由主程序启动更新。\n");
				continue;
			}
			if (!PathFileExistsW(target.c_str()) || !IsPeImage(target.c_str())) {
				PrintUtf8(L"未找到有效的目标程序：" + target + L"\n");
				continue;
			}
			PrintUtf8(L"请关闭 Dzjs Trainer 主程序，关闭后会自动继续替换。\n");
			if (WaitAndReplace(payload, target, kReplaceTimeoutSeconds)) {
				EndProgressLine();
				PrintUtf8(L"更新完成，正在启动 Dzjs Trainer...\n");
				ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
			}
			continue;
		}
		if (!command.empty()) PrintUtf8(L"未知命令，输入 help 查看帮助。\n");
	}
	return 0;
}

void PrintUsage()
{
	PrintUtf8(L"Dzjs Trainer 更新程序\n"
		L"  (无参数)        检查并安装更新\n"
		L"  --target <路径> 指定要更新的主程序（默认本目录）\n"
		L"  --source <路径> 同时刷新来源安装包\n"
		L"  --force         即使版本不更新也重新安装\n"
		L"  --status        只显示本地文件状态\n"
		L"  --repl          进入交互命令模式\n"
		L"  --no-elevate    不申请管理员权限\n"
		L"  --help          显示本帮助\n");
}

Options ParseOptions(int argc, wchar_t** argv)
{
	Options options;
	for (int i = 1; i < argc; ++i) {
		const std::wstring arg = argv[i];
		auto value = [&](std::wstring& out) {
			if (i + 1 < argc) out = argv[++i];
		};
		if (arg == L"--target") value(options.target);
		else if (arg == L"--source") value(options.source);
		else if (arg == L"--force") options.force = true;
		else if (arg == L"--repl") options.repl = true;
		else if (arg == L"--status") options.status = true;
		else if (arg == L"--manifest") value(options.manifestPath);
		else if (arg == L"--help" || arg == L"-h" || arg == L"/?") options.help = true;
		else if (arg == L"--no-elevate") options.noElevate = true;
		else if (arg == L"--elevated") options.elevated = true;
	}
	return options;
}

} // namespace

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
	int argc = 0;
	wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (!argv) return 2;
	const Options options = ParseOptions(argc, argv);
	// Keep the original arguments so the elevated relaunch receives them again.
	std::wstring forwarded;
	for (int i = 1; i < argc; ++i) {
		if (!forwarded.empty()) forwarded.push_back(L' ');
		forwarded.push_back(L'"');
		forwarded += argv[i];
		forwarded.push_back(L'"');
	}
	LocalFree(argv);
	if (!forwarded.empty()) forwarded.push_back(L' ');
	forwarded += L"--elevated";

	if (options.help) {
		PrepareConsole();
		PrintUsage();
		return 0;
	}
	if (options.status) {
		PrepareConsole();
		PrintStatus(options.target.empty() ? DefaultTarget() : options.target);
		return 0;
	}
	// 只取清单：写完文件就退出。放在提权判断之前 —— 这个模式不改任何文件，不需要 UAC。
	if (!options.manifestPath.empty()) {
		PrepareConsole();
		Manifest manifest;
		if (!FetchManifest(manifest)) {
			PrintUtf8(L"无法获取更新信息\n");
			return 3;
		}
		if (!WriteManifestFile(options.manifestPath, manifest)) {
			PrintUtf8(L"无法写入清单文件\n");
			return 4;
		}
		return 0;
	}

	// The updater replaces files inside the install directory, so run elevated.
	// The main program launches it without requesting elevation and this process
	// owns the decision, which also lets a declined prompt be reported properly.
	if (!options.elevated && !options.noElevate && !IsElevated()) {
		PrepareConsole();
		if (RelaunchElevated(forwarded)) return 0;
		return 8;
	}

	PrepareConsole();
	if (options.repl) return RunInteractive(options);
	return RunGuided(options);
}
