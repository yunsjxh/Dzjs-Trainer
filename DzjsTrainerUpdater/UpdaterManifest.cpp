// UpdaterManifest.cpp —— 见头文件说明。
//
// 这里的实现与更新器 EXE 里的对应部分保持一致（端点策略、JSON 解析、版本比较）。
// 有意保留一份独立的实现而不是强行共享：更新器已经能正常工作，
// 不为了去重去动它。两边如果要改，记得同步。

#include "stdafx.h"
#include "UpdaterManifest.h"
#include "../DzjsTrainer/AppPublic.h"
#include "../DzjsTrainer/DzjsTrainer.h"
#include <vector>
#include <Wininet.h>
#include <bcrypt.h>

#pragma comment(lib, "Wininet.lib")
#pragma comment(lib, "Bcrypt.lib")

namespace UpdaterManifest {

// 探测失败时写进主程序日志，否则用户只能看到一句"无法获取更新信息"，
// 无法判断是网络、端点还是解析的问题。
static void LogTrace(LPCWSTR format, ...)
{
	Logger* logger = JTAppGetLoggerDirect();
	if (!logger) return;
	WCHAR text[512] = {};
	va_list args;
	va_start(args, format);
	_vsnwprintf_s(text, _countof(text), _TRUNCATE, format, args);
	va_end(args);
	logger->Log(text);
}

// 泛解析域名 + 每次进程随机前缀；顶点域名作为回退。
static const wchar_t* const kUpdateHostSuffix =
	L".dzjstrainer.xn--9kq396ceqaq4si9m.cn/update-server/update.php";

// ---------------------------------------------------------------- text helpers

static std::string Trim(std::string text)
{
	size_t begin = 0;
	while (begin < text.size() && static_cast<unsigned char>(text[begin]) <= ' ') ++begin;
	size_t end = text.size();
	while (end > begin && static_cast<unsigned char>(text[end - 1]) <= ' ') --end;
	return text.substr(begin, end - begin);
}

static std::wstring WideFromUtf8(const std::string& text)
{
	if (text.empty()) return std::wstring();
	const int count = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
		static_cast<int>(text.size()), nullptr, 0);
	if (count <= 0) return std::wstring();
	std::wstring wide(static_cast<size_t>(count), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
		&wide[0], count);
	return wide;
}

// ---------------------------------------------------------------- endpoints

static std::wstring RandomPrefix()
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

static std::wstring ApexEndpoint()
{
	return std::wstring(L"https://") + (kUpdateHostSuffix + 1);
}

// 泛域名方便但不总是可达，所以每次都按老客户端那样走候选列表：
// 两个随机子域，再回退到稳定的顶点域名。
static std::vector<std::wstring> UpdateEndpoints()
{
	std::vector<std::wstring> endpoints;
	endpoints.reserve(3);
	endpoints.push_back(std::wstring(L"https://") + RandomPrefix() + kUpdateHostSuffix);
	endpoints.push_back(std::wstring(L"https://") + RandomPrefix() + kUpdateHostSuffix);
	endpoints.push_back(ApexEndpoint());
	return endpoints;
}

// ---------------------------------------------------------------- HTTP

// 打开一个 WinINet 会话。accessType 先试 PRECONFIG（沿用系统代理设置），
// 失败再试 DIRECT。
//
// 为什么要两档：主程序是提权进程，实测在部分机器上 PRECONFIG 会直接
// ERROR_INTERNET_CANNOT_CONNECT(0x800C0005) —— 读不到当前用户的 Internet 配置。
// 同时显式启用 TLS 1.2：默认协议列表来自注册表，提权/新用户配置下可能拿不到。
static HINTERNET OpenSession(DWORD accessType)
{
	HINTERNET session = InternetOpenW(L"DzjsTrainer/1.0", accessType, nullptr, nullptr, 0);
	if (!session) return nullptr;
	DWORD timeout = 8000;
	InternetSetOptionW(session, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
	InternetSetOptionW(session, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
	// 显式启用 TLS 1.2。默认协议列表来自注册表，提权进程或新用户配置下可能拿不到。
	// 84 = INTERNET_OPTION_SECURE_PROTOCOLS；0x800 = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2
	// （WinINet 与 WinHTTP 共用这个位值）。这两个常量在部分 SDK 里没有声明，直接写数值。
	const DWORD kOptionSecureProtocols = 84;
	const DWORD kProtocolTls12 = 0x00000800;
	DWORD protocols = kProtocolTls12;
	InternetSetOptionW(session, kOptionSecureProtocols, &protocols, sizeof(protocols));
	return session;
}

static bool HttpGetWOnce(const std::wstring& url, DWORD accessType, std::string& body)
{
	body.clear();
	HINTERNET session = OpenSession(accessType);
	if (!session) return false;

	// 证书校验保持开启：端点用有效的通配证书，安装包哈希也走这条通道。
	HINTERNET request = InternetOpenUrlW(session, url.c_str(), nullptr, 0,
		INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_SECURE, 0);
	bool ok = false;
	if (request) {
		char buffer[8192];
		DWORD read = 0;
		while (InternetReadFile(request, buffer, sizeof(buffer), &read) && read)
			body.append(buffer, read);
		DWORD status = 0, statusSize = sizeof(status);
		const BOOL queried = HttpQueryInfoW(request, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
			&status, &statusSize, nullptr);
		if (!queried)
			LogTrace(L"更新探测：取状态码失败 err=%lu url=%s", GetLastError(), url.c_str());
		else if (status < 200 || status >= 300)
			LogTrace(L"更新探测：HTTP %lu url=%s", status, url.c_str());
		ok = queried && status >= 200 && status < 300;
		InternetCloseHandle(request);
	} else {
		LogTrace(L"更新探测：InternetOpenUrl 失败 err=%lu accessType=%lu url=%s",
			GetLastError(), accessType, url.c_str());
	}
	InternetCloseHandle(session);
	if (!ok) body.clear();
	return ok;
}

static bool HttpGetW(const std::wstring& url, std::string& body)
{
	if (HttpGetWOnce(url, INTERNET_OPEN_TYPE_PRECONFIG, body)) return true;
	// PRECONFIG 失败时改直连重试一次：提权进程读不到用户代理配置时会出现这种情况。
	return HttpGetWOnce(url, INTERNET_OPEN_TYPE_DIRECT, body);
}

static bool HttpGetAny(const std::wstring& query, std::string& body)
{
	int index = 0;
	for (const std::wstring& endpoint : UpdateEndpoints()) {
		++index;
		const std::wstring url = endpoint + query;
		if (HttpGetW(url, body)) {
			LogTrace(L"更新探测：端点 %d 成功 query=%s body=%u 字节", index, query.c_str(),
				static_cast<unsigned>(body.size()));
			return true;
		}
		LogTrace(L"更新探测：端点 %d 失败 query=%s url=%s", index, query.c_str(), url.c_str());
	}
	body.clear();
	LogTrace(L"更新探测：全部端点失败 query=%s", query.c_str());
	return false;
}

// ---------------------------------------------------------------- JSON

// 极简的 JSON 字符串取值：只处理 "key": "value" 这一种形态，够清单用。
static bool JsonString(const std::string& json, const char* key, std::string& value)
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
				unsigned part = 0;
				if (digit >= '0' && digit <= '9') part = static_cast<unsigned>(digit - '0');
				else if (digit >= 'a' && digit <= 'f') part = static_cast<unsigned>(digit - 'a' + 10);
				else if (digit >= 'A' && digit <= 'F') part = static_cast<unsigned>(digit - 'A' + 10);
				else return false;
				code = (code << 4) | part;
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

// ---------------------------------------------------------------- public

bool Fetch(Info& info)
{
	info = Info();

	// 记录执行环境。排查"改了没生效"时先看这行：
	// 它能证明跑的是哪个构建，并暴露账号 / 位数 / 是否提权 ——
	// 这几项都可能是"同一个请求有的进程通、有的进程不通"的原因。
	{
		WCHAR user[128] = {};
		DWORD userSize = _countof(user);
		GetUserNameW(user, &userSize);
		WCHAR module[MAX_PATH] = {};
		GetModuleFileNameW(nullptr, module, _countof(module));
		LogTrace(L"更新探测[build=20260919-worker]: user=%s pid=%lu x86=%d module=%s",
			user[0] ? user : L"(未知)", GetCurrentProcessId(),
#ifdef _WIN64
			0,
#else
			1,
#endif
			module);
	}

	std::string body;
	if (HttpGetAny(L"?manifest=1", body)) {
		std::string version, notes, url, sha256;
		JsonString(body, "version", version);
		JsonString(body, "notes", notes);
		JsonString(body, "url", url);
		JsonString(body, "sha256", sha256);
		info.version = WideFromUtf8(version);
		info.notes = WideFromUtf8(notes);
		info.url = WideFromUtf8(url);
		info.sha256 = WideFromUtf8(sha256);
	}

	// 服务端仍保留纯文本接口，兼容还没发布清单的部署。
	if (info.version.empty() && HttpGetAny(L"?getnewver", body))
		info.version = WideFromUtf8(Trim(body));
	if (info.notes.empty() && HttpGetAny(L"?getupdateinfo", body))
		info.notes = WideFromUtf8(Trim(body));
	if (info.url.empty() && HttpGetAny(L"?getupdate", body))
		info.url = WideFromUtf8(Trim(body));

	LogTrace(L"更新探测：解析结果 version=\"%s\" url=%s sha256=%s", info.version.c_str(),
		info.url.empty() ? L"(空)" : L"(有)", info.sha256.empty() ? L"(空)" : L"(有)");
	return !info.version.empty() && !info.url.empty();
}

static std::vector<long> VersionParts(const std::wstring& text)
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

} // namespace UpdaterManifest
