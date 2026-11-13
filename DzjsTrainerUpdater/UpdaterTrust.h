#pragma once

#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <string>

#pragma comment(lib, "crypt32.lib")

// 更新通道的第一道闸：主机白名单。
//
// 主程序和更新器 exe 都要用同一套判断，所以做成 header-only 的 inline 函数，
// 免得两边各写一份、日后改漏一边。
//
// 证书公钥指纹（下面那张表）已经从"放行条件"降级为"诊断信息"：
// 身份确认改由 UpdaterTicket.h 的时效票据负责。改动的理由写在那个文件开头，
// 一句话概括 —— 钉公钥会在 Let's Encrypt 续期换钥匙时把旧客户端全部锁死，
// 而客户端唯一的自救手段恰恰就是这条更新通道。

static const wchar_t* const kJupApexHost = L"dzjstrainer.xn--9kq396ceqaq4si9m.cn";

// 服务端证书的 SubjectPublicKeyInfo（DER）十六进制小写。
//
// 现在只用于日志：指纹对不上不再中断请求，只写一行诊断。真正的身份判断
// 走 UpdaterTicket.h 的时效票据（HMAC 签名 + 时间窗 + 正文绑定），
// 那个伪造不了，也不会因为证书续期而失效。
//
// 留着这张表的价值是排查：当更新失败时，日志里能看出"对面换证书了"
// 还是"对面根本不是我们的服务器"。
//   [0] 顶点域，notAfter 2027-02-09
//   [1] 泛域名子域，notAfter 2026-12-11
static const wchar_t* const kJupPinnedSpki[] = {
	L"3059301306072a8648ce3d020106082a8648ce3d0301070342000481a53cb25643ffcdee178be3"
	L"d8965c11d0daae59784a85db91cac5fb66c59ed06d3001222b3af348081550fd9101437a0fab5207"
	L"2387a8e96028132dff216945",
	L"30820122300d06092a864886f70d01010105000382010f003082010a0282010100afb4fad5cf7a"
	L"574b027eb6bd00bb520b1534f129f0f80c9a16798fb876d59c78eebcef35358acc1d60fc95477bb5"
	L"8d20bce5f27f56d41a92d7c549e4e0d1fde4685984d2a953662d17d48b710b9d31498e8cecedbb98"
	L"b9327fcc27ee949dd9263667830ac465c45bd0cb2a18fdf6805c0652fd4cae0b2123080881966fd6"
	L"d4a8388995456a0340043dc409f8ed5dc03cbb5ff1f19605d553ef5b840b3b1b4ceaf805967b8c02"
	L"d85635c177d4f0944ecaf4df18de11be1115fc30d1733b7ce081390e1acecfdd9e52d400cb477e67"
	L"d87a624a094495c680f6075db123e5b36cf702e8b2e9bf08b6e373a8655b36500a884d1871dc15c0"
	L"e84fd255e6cccdddcee10203010001",
};

inline std::wstring JupLowerAscii(std::wstring text)
{
	for (wchar_t& ch : text) {
		if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
	}
	return text;
}

// 只放行顶点域名本身和它的真子域（"xxx." + apex）。
// 不放过 "evil-dzjstrainer.xxx" 这类后缀相近的域名：必须带那个点。
inline bool JupHostAllowed(const std::wstring& host)
{
	const std::wstring lower = JupLowerAscii(host);
	const std::wstring apex = JupLowerAscii(kJupApexHost);
	if (lower == apex) return true;
	const std::wstring suffix = std::wstring(L".") + apex;
	if (lower.size() <= suffix.size()) return false;
	return lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// 协议必须是 https，而且主机在白名单里。
inline bool JupUrlAllowed(const std::wstring& url)
{
	if (url.empty()) return false;
	URL_COMPONENTS parts{};
	parts.dwStructSize = sizeof(parts);
	WCHAR host[256] = {};
	parts.lpszHostName = host;
	parts.dwHostNameLength = _countof(host);
	// 协议只看前缀，别看 parts.nScheme 配 INTERNET_SCHEME_HTTPS：
	// winhttp.h 和 wininet.h 用同一个守卫宏 _INTERNET_SCHEME_ 各定义了一套
	// （2 和枚举值 4），谁先被包含谁生效；而 WinHttpCrackUrl 运行时就返回 2。
	// 同一个二进制里两套头都包含时，这个比较能不能成立纯靠运气。
	if (JupLowerAscii(url).compare(0, 8, L"https://") != 0) return false;
	if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts)) return false;
	return JupHostAllowed(host);
}

// 取证书的 SPKI DER 并转成小写十六进制。失败返回空串。
inline std::string JupSpkiHex(const CERT_CONTEXT* cert)
{
	if (!cert || !cert->pCertInfo) return std::string();
	const CERT_PUBLIC_KEY_INFO& spki = cert->pCertInfo->SubjectPublicKeyInfo;
	if (!spki.Algorithm.pszObjId) return std::string();
	void* encoded = nullptr;
	DWORD size = 0;
	if (!CryptEncodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO,
		&spki, CRYPT_ENCODE_ALLOC_FLAG, nullptr, &encoded, &size) || !encoded) {
		return std::string();
	}
	static const char digits[] = "0123456789abcdef";
	std::string hex;
	hex.reserve(static_cast<size_t>(size) * 2);
	const unsigned char* bytes = static_cast<const unsigned char*>(encoded);
	for (DWORD i = 0; i < size; ++i) {
		hex.push_back(digits[bytes[i] >> 4]);
		hex.push_back(digits[bytes[i] & 0x0f]);
	}
	LocalFree(encoded);
	return hex;
}

inline bool JupPinMatches(const std::string& observed)
{
	if (observed.empty()) return false;
	for (const wchar_t* pinned : kJupPinnedSpki) {
		std::string expected;
		for (const wchar_t* p = pinned; p && *p; ++p) expected.push_back(static_cast<char>(*p));
		if (expected == observed) return true;
	}
	return false;
}

// WinHTTP 请求：握手完成后取服务端证书的 SPKI 并核对是否在已知列表里。
//
// 返回 false 不代表"不可信"，只代表"证书换了或从没见过"，调用方应当记日志
// 后继续。是否需要中断由调用方结合时效票据的结果判断，别在这里做决断。
// observed 回填实际指纹，方便直接对照。
inline bool JupCheckWinHttpCert(HINTERNET request, std::string& observed)
{
	observed.clear();
	if (!request) return false;
	CERT_CONTEXT* cert = nullptr;
	DWORD size = sizeof(cert);
	if (!WinHttpQueryOption(request, WINHTTP_OPTION_SERVER_CERT_CONTEXT, &cert, &size) || !cert) {
		return false;
	}
	observed = JupSpkiHex(cert);
	CertFreeCertificateContext(cert);
	return JupPinMatches(observed);
}
