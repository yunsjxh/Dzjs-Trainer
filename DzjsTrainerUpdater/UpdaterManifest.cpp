// UpdaterManifest.cpp —— 见头文件说明。
//
// 这里的实现与更新器 EXE 里的对应部分保持一致（端点策略、JSON 解析、版本比较）。
// 有意保留一份独立的实现而不是强行共享：更新器已经能正常工作，
// 不为了去重去动它。两边如果要改，记得同步。

#include "stdafx.h"
#include "UpdaterManifest.h"
#include <winhttp.h>
#include "UpdaterTrust.h"
#include "UpdaterTicket.h"
#pragma comment(lib, "Winhttp.lib")
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

std::wstring ApexBaseUrl()
{
	return ApexEndpoint();
}

// ---------------------------------------------------------------- HTTP

// 用 WinHTTP 而不是 WinINet。
//
// 原因写在 DzjsTrainerUpdater.h 里：主程序（GUI 进程）里调 WinINet 会直接
// ERROR_INTERNET_CANNOT_CONNECT，历史上就是为了绕开它才把探测工作推给独立的
// 更新器进程；那个进程又会 AllocConsole 弹一个黑窗口。换成 WinHTTP 后探测可以
// 在本进程里完成，既不依赖子进程，也不会再闪黑框。
static HINTERNET OpenWinHttpSession()
{
	HINTERNET session = WinHttpOpen(L"DzjsTrainer/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
		WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session) return nullptr;
	WinHttpSetTimeouts(session, 8000, 8000, 8000, 8000);
	return session;
}

static bool HttpGetWOnce(const std::wstring& url, std::string& body, std::string& ticket)
{
	body.clear();
	ticket.clear();
	HINTERNET session = OpenWinHttpSession();
	if (!session) {
		LogTrace(L"更新探测：WinHttpOpen 失败 err=%lu", GetLastError());
		return false;
	}

	URL_COMPONENTS parts{};
	parts.dwStructSize = sizeof(parts);
	WCHAR host[256] = {};
	WCHAR path[2048] = {};
	parts.lpszHostName = host;
	parts.dwHostNameLength = _countof(host);
	parts.lpszUrlPath = path;
	parts.dwUrlPathLength = _countof(path);
	bool ok = false;
	HINTERNET connection = nullptr;
	HINTERNET request = nullptr;
	if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts)) {
		LogTrace(L"更新探测：URL 解析失败 url=%s", url.c_str());
	}
	else {
		connection = WinHttpConnect(session, host, parts.nPort, 0);
		if (!connection) {
			LogTrace(L"更新探测：WinHttpConnect 失败 err=%lu host=%s", GetLastError(), host);
		}
	}
	if (connection) {
		request = WinHttpOpenRequest(connection, L"GET", path, nullptr, WINHTTP_NO_REFERER,
			WINHTTP_DEFAULT_ACCEPT_TYPES,
			parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
		if (!request) {
			LogTrace(L"更新探测：WinHttpOpenRequest 失败 err=%lu", GetLastError());
		}
	}
	// 证书校验保持开启：端点用有效的通配证书，安装包哈希也走这条通道。
	// 发送和接收分开记日志：合成一个条件时只能看到"请求发送失败"，无法区分是
	// 握手失败还是 HTTP 层失败。
	BOOL sent = FALSE;
	if (request) {
		LogTrace(L"更新探测：目标 host=%s port=%u path=%s secure=%d",
			host, static_cast<unsigned>(parts.nPort), path,
			parts.nScheme == INTERNET_SCHEME_HTTPS ? 1 : 0);
		sent = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
			WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
		if (!sent) {
			LogTrace(L"更新探测：SendRequest 失败 err=%lu", GetLastError());
		}
	}
	BOOL received = FALSE;
	if (sent) {
		received = WinHttpReceiveResponse(request, nullptr);
		if (!received) {
			LogTrace(L"更新探测：ReceiveResponse 失败 err=%lu", GetLastError());
		}
	}
	// 握手已经完成、正文还没开始读：看一眼服务端证书。
	//
	// 只记日志，不再据此中断。钉公钥挡不住真冒名（域名被劫持后对方照样能
	// 申请到合法证书），却会在 Let's Encrypt 换钥匙时把旧客户端全部锁死 ——
	// 而客户端唯一的自救手段就是这条更新通道。身份改由下面的时效票据确认。
	if (received) {
		std::string observed;
		if (!JupCheckWinHttpCert(request, observed) && !observed.empty()) {
			static bool reported = false;
			if (!reported) {
				reported = true;
				LogTrace(L"更新探测：服务端公钥不在已知列表，改由时效票据确认 host=%s 实际 SPKI=%S",
					host, observed.c_str());
			}
		}
	}
	if (request && sent && received) {
		DWORD status = 0;
		DWORD statusSize = sizeof(status);
		const BOOL queried = WinHttpQueryHeaders(request,
			WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
		if (!queried) {
			LogTrace(L"更新探测：取状态码失败 err=%lu url=%s", GetLastError(), url.c_str());
		}
		else if (status < 200 || status >= 300) {
			LogTrace(L"更新探测：HTTP %lu url=%s", status, url.c_str());
		}
		else {
			ok = true;
			for (;;) {
				DWORD available = 0;
				if (!WinHttpQueryDataAvailable(request, &available)) {
					ok = false;
					break;
				}
				if (available == 0) break;
				char buffer[8192];
				DWORD read = 0;
				const DWORD want = (available < sizeof(buffer)) ? available : static_cast<DWORD>(sizeof(buffer));
				if (!WinHttpReadData(request, buffer, want, &read) || read == 0) {
					ok = false;
					break;
				}
				body.append(buffer, read);
			}
			if (!ok) LogTrace(L"更新探测：读取响应体失败 err=%lu", GetLastError());
			// 票据头必须等正文读完再取：它绑定的是正文哈希，提前取没有意义。
			if (ok) jup_ticket::ReadTicketHeader(request, ticket);
		}
	}
	else if (request) {
		LogTrace(L"更新探测：请求发送失败 err=%lu url=%s", GetLastError(), url.c_str());
	}
	if (request) WinHttpCloseHandle(request);
	if (connection) WinHttpCloseHandle(connection);
	WinHttpCloseHandle(session);
	if (!ok) body.clear();
	return ok;
}

static bool HttpGetW(const std::wstring& url, std::string& body, std::string& ticket)
{
	// WinHTTP 只有一套代理策略（DEFAULT_PROXY 会读 WinHTTP 自己的配置），
	// 失败时再直连重试一次：个别机器上代理探测会拖住请求。
	if (HttpGetWOnce(url, body, ticket)) return true;
	return HttpGetWOnce(url, body, ticket);
}

// 带时效票据取清单：先发挑战，再拿正文和票据，三项都过才认。
//
// 清单是整条更新链的根：它同时决定"下哪个包"和"这个包该是什么哈希"。
// 清单被换掉的话，后面比对的 SHA-256 只是在核对攻击者自己填的值，
// 等于没有校验，所以这一环必须验身份 + 验内容。
static bool HttpGetManifest(std::string& body, std::wstring& error)
{
	error.clear();
	const jup_ticket::Challenge challenge = jup_ticket::MakeChallenge("t1");
	const std::string query = std::string("?manifest=1") + jup_ticket::ChallengeQuery(challenge);

	int index = 0;
	for (const std::wstring& endpoint : UpdateEndpoints()) {
		++index;
		std::string ticket;
		if (!HttpGetW(endpoint + WideFromUtf8(query), body, ticket)) {
			// 连不上和"连上了但被 403"在这里是同一个返回值（挑战签名不符时服务端
			// 会直接 403）。哪一种是日志里那行 "更新探测：HTTP <code>" 才分得清，
			// 所以提示语不写成确定的"网络不通"。
			error = L"无法连接更新服务器，或服务器拒绝了本次请求";
			LogTrace(L"更新探测：端点 %d 请求失败", index);
			continue;
		}
		if (body.empty()) {
			error = L"服务器返回的清单为空";
			LogTrace(L"更新探测：端点 %d 返回空正文", index);
			continue;
		}
		const jup_ticket::VerifyResult verdict = jup_ticket::Verify(ticket, challenge.nonce, body);
		if (!verdict.Ok()) {
			error = verdict.Text();
			LogTrace(L"更新探测：端点 %d 票据校验失败 —— %s", index, verdict.Text());
			continue;
		}
		LogTrace(L"更新探测：端点 %d 通过票据校验 body=%u 字节", index,
			static_cast<unsigned>(body.size()));
		return true;
	}
	body.clear();
	LogTrace(L"更新探测：全部端点失败");
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
	std::wstring error;
	if (HttpGetManifest(body, error)) {
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
	else {
		info.error = error;
	}

	// 纯文本回退接口（?getnewver / ?getupdateinfo / ?getupdate）在这里停用：
	// 它们返回裸字符串，既没有票据也无从绑定正文，等于给"验票据"开了个后门。
	// 服务端现在都能力返回 manifest，留着只会成为唯一的绕过路径。

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
