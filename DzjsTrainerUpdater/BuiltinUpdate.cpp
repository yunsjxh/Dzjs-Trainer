// 内置更新的落地实现：WinHTTP 下载 + SHA-256 校验 + 自我替换。
//
// 为什么不用 WinINet：本程序 GUI 进程里 WinINet 的 InternetOpen/ InternetOpenUrl
// 实测直接失败并返回 ERROR_INTERNET_CANNOT_CONNECT（同账号控制台进程正常），
// 这也是历史上把更新工作推给独立更新器进程的原因。WinHTTP 走另一套栈，没有这个问题。

#include "stdafx.h"
#include "BuiltinUpdate.h"
#include "UpdaterManifest.h"
#include "UpdaterTrust.h"
#include "../DzjsTrainer/AppPublic.h"
#include "../DzjsTrainer/Logger.h"
#include <winhttp.h>
#include <vector>
#include <string>

#pragma comment(lib, "Winhttp.lib")

namespace
{
	std::wstring OwnExePath()
	{
		WCHAR buffer[MAX_PATH * 2] = {};
		const DWORD length = GetModuleFileNameW(nullptr, buffer, _countof(buffer));
		if (length == 0 || length >= _countof(buffer)) return std::wstring();
		return std::wstring(buffer, length);
	}

	void SetError(LPWSTR errorText, size_t errorChars, const std::wstring& text)
	{
		if (!errorText || errorChars == 0) return;
		wcsncpy_s(errorText, errorChars, text.c_str(), _TRUNCATE);
	}

	// WinHTTP 的地址拆分结果放在这里，host/path 缓冲区必须活得比 request 久。
	struct ParsedUrl {
		std::wstring host;
		std::wstring path;
		INTERNET_PORT port = 0;
		bool secure = false;
	};

	bool ParseUrl(const std::wstring& url, ParsedUrl& out)
	{
		URL_COMPONENTS parts{};
		parts.dwStructSize = sizeof(parts);
		WCHAR host[256] = {};
		WCHAR path[2048] = {};
		parts.lpszHostName = host;
		parts.dwHostNameLength = _countof(host);
		parts.lpszUrlPath = path;
		parts.dwUrlPathLength = _countof(path);
		if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts)) return false;
		out.host.assign(host);
		out.path.assign(path);
		out.port = parts.nPort;
		out.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
		return !out.host.empty();
	}

	bool HttpGetToFile(const std::wstring& url, const std::wstring& destPath,
		JUpdaterProgressFn progress, void* context, std::wstring& error)
	{
		ParsedUrl parsed;
		if (!ParseUrl(url, parsed)) {
			error = L"下载地址无效";
			return false;
		}

		HINTERNET session = WinHttpOpen(L"DzjsTrainer/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
			WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (!session) {
			error = L"无法初始化 WinHTTP";
			return false;
		}
		WinHttpSetTimeouts(session, 15000, 15000, 30000, 30000);

		HINTERNET connection = WinHttpConnect(session, parsed.host.c_str(), parsed.port, 0);
		if (!connection) {
			error = L"无法连接更新服务器";
			WinHttpCloseHandle(session);
			return false;
		}
		HINTERNET request = WinHttpOpenRequest(connection, L"GET", parsed.path.c_str(),
			nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
			parsed.secure ? WINHTTP_FLAG_SECURE : 0);
		bool ok = false;
		if (!request) {
			error = L"无法创建下载请求";
		}
		else if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
			WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request, nullptr)) {
			error = L"下载请求失败（错误码 " + std::to_wstring(GetLastError()) + L"）";
		}
		else {
			DWORD status = 0;
			DWORD statusSize = sizeof(status);
			WinHttpQueryHeaders(request,
				WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
			unsigned long long total = 0;
			{
				WCHAR lengthText[32] = {};
				DWORD lengthSize = sizeof(lengthText);
				if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH,
					WINHTTP_HEADER_NAME_BY_INDEX, lengthText, &lengthSize, WINHTTP_NO_HEADER_INDEX)) {
					total = _wcstoui64(lengthText, nullptr, 10);
				}
			}
			if (status < 200 || status >= 300) {
				error = L"下载地址返回 HTTP " + std::to_wstring(status);
			}
			else {
				HANDLE file = CreateFileW(destPath.c_str(), GENERIC_WRITE, 0, nullptr,
					CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
				if (file == INVALID_HANDLE_VALUE) {
					error = L"无法写入更新包（错误码 " + std::to_wstring(GetLastError()) + L"）";
				}
				else {
					ok = true;
					unsigned long long written = 0;
					std::vector<char> buffer(64 * 1024);
					for (;;) {
						DWORD available = 0;
						if (!WinHttpQueryDataAvailable(request, &available)) {
							error = L"读取下载数据失败（错误码 " + std::to_wstring(GetLastError()) + L"）";
							ok = false;
							break;
						}
						if (available == 0) break;
						const DWORD want = (std::min)(available, static_cast<DWORD>(buffer.size()));
						DWORD read = 0;
						if (!WinHttpReadData(request, buffer.data(), want, &read) || read == 0) {
							error = L"读取下载数据失败（错误码 " + std::to_wstring(GetLastError()) + L"）";
							ok = false;
							break;
						}
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
						if (progress) progress(written, total, context);
					}
					CloseHandle(file);
					if (ok && written == 0) {
						error = L"更新包为空";
						ok = false;
					}
					if (!ok) DeleteFileW(destPath.c_str());
				}
			}
		}
		if (request) WinHttpCloseHandle(request);
		WinHttpCloseHandle(connection);
		WinHttpCloseHandle(session);
		return ok;
	}
}

BOOL JUpdater_ResolveUrl(LPCWSTR manifestUrl, LPWSTR outUrl, size_t outChars)
{
	if (!manifestUrl || !outUrl || outChars == 0) return FALSE;
	std::wstring text = manifestUrl;
	while (!text.empty() && (text.front() == L' ' || text.front() == L'\t')) text.erase(text.begin());
	const bool absolute = text.compare(0, 7, L"http://") == 0 || text.compare(0, 8, L"https://") == 0;
	std::wstring result = text;
	if (!absolute) {
		// 相对路径按清单所在目录补全：https://<host>/update-server/ + name
		std::wstring base = UpdaterManifest::ApexBaseUrl();
		const size_t slash = base.find_last_of(L'/');
		if (slash != std::wstring::npos) base.resize(slash + 1);
		while (!result.empty() && result.front() == L'/') result.erase(result.begin());
		result = base + result;
	}
	// 清单里的地址是外部数据，不能让它把客户端带去任意主机：
	// 只接受 https，且主机必须是自家域名或其子域。
	if (!JupUrlAllowed(result)) return FALSE;
	wcsncpy_s(outUrl, outChars, result.c_str(), _TRUNCATE);
	return TRUE;
}

BOOL JUpdater_DownloadPackage(LPCWSTR url, LPCWSTR destPath, JUpdaterProgressFn progress, void* context)
{
	if (!url || !destPath) return FALSE;

	// 下载前先验服务器身份。
	//
	// 进程内这条通路和独立更新器那条（--download）是两条独立的路，门必须各装
	// 一道：只堵一条，另一条通了就等于没堵。这里复用"取清单"这个动作 —— 它能
	// 返回，就说明这一轮已经验过票据签名、nonce 回显、时间窗和正文哈希。
	// 拿不到清单就不下载，一个字节都不落盘：更新失败可以重试，装错包不可逆。
	UpdaterManifest::Info probe;
	if (!UpdaterManifest::Fetch(probe)) {
		Logger* logger = JTAppGetLoggerDirect();
		if (logger) {
			logger->LogError(L"内置更新：服务器身份校验未通过，已拒绝下载 —— %s",
				probe.error.empty() ? L"原因未知" : probe.error.c_str());
		}
		return FALSE;
	}

	std::wstring error;
	const bool ok = HttpGetToFile(url, destPath, progress, context, error);
	if (!ok) {
		// 让调用方能看到原因：写进日志。
		Logger* logger = JTAppGetLoggerDirect();
		if (logger) logger->LogError(L"内置更新下载失败：%s", error.c_str());
	}
	return ok ? TRUE : FALSE;
}

// ---------------------------------------------------------------- SHA-256
// 不依赖 CNG/BCrypt。主程序进程里 bcrypt 的算法提供程序取不到（同一个进程里
// Schannel 也报 SEC_E_SECPKG_NOT_FOUND，_HKLM 的密码学栈在这进程里是坏的），
// 表现为校验函数直接返回失败、而 GetLastError 还是 0，排查起来完全没线索。
// 更新包只有几 MB，纯软件算一遍几十毫秒，换来的是任何环境都能校验。
namespace
{
	const unsigned int kSha256K[64] = {
		0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
		0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
		0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
		0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
		0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
		0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
		0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
		0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
		0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
		0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
		0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
	};

	inline unsigned int RotateRight(unsigned int value, int bits)
	{
		return (value >> bits) | (value << (32 - bits));
	}

	struct Sha256Context {
		unsigned int state[8];
		unsigned long long total;
		unsigned char block[64];
		size_t used;
	};

	void Sha256Reset(Sha256Context* context)
	{
		static const unsigned int init[8] = {
			0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
			0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
		};
		for (int i = 0; i < 8; ++i) context->state[i] = init[i];
		context->total = 0;
		context->used = 0;
	}

	void Sha256Transform(Sha256Context* context, const unsigned char* chunk)
	{
		unsigned int w[64] = {};
		for (int i = 0; i < 16; ++i) {
			w[i] = (static_cast<unsigned int>(chunk[i * 4]) << 24) |
				(static_cast<unsigned int>(chunk[i * 4 + 1]) << 16) |
				(static_cast<unsigned int>(chunk[i * 4 + 2]) << 8) |
				static_cast<unsigned int>(chunk[i * 4 + 3]);
		}
		for (int i = 16; i < 64; ++i) {
			const unsigned int s0 = RotateRight(w[i - 15], 7) ^ RotateRight(w[i - 15], 18) ^ (w[i - 15] >> 3);
			const unsigned int s1 = RotateRight(w[i - 2], 17) ^ RotateRight(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		unsigned int a = context->state[0], b = context->state[1], c = context->state[2], d = context->state[3];
		unsigned int e = context->state[4], f = context->state[5], g = context->state[6], h = context->state[7];
		for (int i = 0; i < 64; ++i) {
			const unsigned int s1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
			const unsigned int choice = (e & f) ^ ((~e) & g);
			const unsigned int temp1 = h + s1 + choice + kSha256K[i] + w[i];
			const unsigned int s0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
			const unsigned int majority = (a & b) ^ (a & c) ^ (b & c);
			const unsigned int temp2 = s0 + majority;
			h = g; g = f; f = e; e = d + temp1;
			d = c; c = b; b = a; a = temp1 + temp2;
		}
		context->state[0] += a; context->state[1] += b; context->state[2] += c; context->state[3] += d;
		context->state[4] += e; context->state[5] += f; context->state[6] += g; context->state[7] += h;
	}

	void Sha256Update(Sha256Context* context, const unsigned char* data, size_t length)
	{
		context->total += length;
		while (length > 0) {
			const size_t take = (64 - context->used) < length ? (64 - context->used) : length;
			for (size_t i = 0; i < take; ++i) context->block[context->used + i] = data[i];
			context->used += take;
			data += take;
			length -= take;
			if (context->used == 64) {
				Sha256Transform(context, context->block);
				context->used = 0;
			}
		}
	}

	void Sha256Finish(Sha256Context* context, unsigned char digest[32])
	{
		const unsigned long long bits = context->total * 8ull;
		unsigned char padding[72] = {};
		padding[0] = 0x80;
		const size_t padLength = (context->used < 56) ? (56 - context->used) : (120 - context->used);
		Sha256Update(context, padding, padLength);
		unsigned char lengthBytes[8] = {};
		for (int i = 0; i < 8; ++i) {
			lengthBytes[7 - i] = static_cast<unsigned char>((bits >> (i * 8)) & 0xffu);
		}
		// 这一步不能再累加长度（上面已经多算了 padding），直接补最后一块。
		for (int i = 0; i < 8; ++i) context->block[context->used + i] = lengthBytes[i];
		Sha256Transform(context, context->block);
		context->used = 0;
		for (int i = 0; i < 8; ++i) {
			digest[i * 4] = static_cast<unsigned char>((context->state[i] >> 24) & 0xffu);
			digest[i * 4 + 1] = static_cast<unsigned char>((context->state[i] >> 16) & 0xffu);
			digest[i * 4 + 2] = static_cast<unsigned char>((context->state[i] >> 8) & 0xffu);
			digest[i * 4 + 3] = static_cast<unsigned char>(context->state[i] & 0xffu);
		}
	}
}

BOOL JUpdater_FileSha256(LPCWSTR path, LPWSTR outHex, size_t outChars)
{
	if (!path || !outHex || outChars < 65) return FALSE;
	// 刚写完的 exe 经常被杀软/索引器短暂独占（ERROR_SHARING_VIOLATION）。
	// 共享位放全 + 最多重试 1.5 秒，别让"算不出校验值"变成偶发失败。
	HANDLE file = INVALID_HANDLE_VALUE;
	for (int attempt = 0; attempt < 15; ++attempt) {
		file = CreateFileW(path, GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file != INVALID_HANDLE_VALUE) break;
		const DWORD error = GetLastError();
		if (error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION &&
			error != ERROR_ACCESS_DENIED) return FALSE;
		Sleep(100);
	}
	if (file == INVALID_HANDLE_VALUE) return FALSE;

	Sha256Context context;
	Sha256Reset(&context);
	std::vector<unsigned char> buffer(64 * 1024);
	bool ok = true;
	for (;;) {
		DWORD read = 0;
		if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
			ok = false;
			break;
		}
		if (read == 0) break;
		Sha256Update(&context, buffer.data(), read);
	}
	CloseHandle(file);
	if (!ok) return FALSE;

	unsigned char digest[32] = {};
	Sha256Finish(&context, digest);
	static const wchar_t hex[] = L"0123456789abcdef";
	for (size_t i = 0; i < sizeof(digest); ++i) {
		outHex[i * 2] = hex[digest[i] >> 4];
		outHex[i * 2 + 1] = hex[digest[i] & 0x0F];
	}
	outHex[sizeof(digest) * 2] = L'\0';
	return TRUE;
}

BOOL JUpdater_ApplyPackageInPlace(LPCWSTR packagePath, LPCWSTR sha256Hex,
	LPWSTR errorText, size_t errorChars)
{
	if (errorText && errorChars) errorText[0] = 0;
	if (!packagePath || packagePath[0] == 0) {
		SetError(errorText, errorChars, L"没有可用的更新包");
		return FALSE;
	}
	const std::wstring exePath = OwnExePath();
	if (exePath.empty()) {
		SetError(errorText, errorChars, L"无法定位当前程序的位置");
		return FALSE;
	}
	const WCHAR kSep = static_cast<WCHAR>(92);
	const WCHAR kQuote = static_cast<WCHAR>(34);
	const size_t slash = exePath.find_last_of(kSep);
	if (slash == std::wstring::npos) {
		SetError(errorText, errorChars, L"当前程序路径异常");
		return FALSE;
	}
	const std::wstring directory = exePath.substr(0, slash + 1);
	const std::wstring stagedPath = exePath + L".new";
	const std::wstring backupPath = exePath + L".old";

	// 没有校验值就不替换。这是整条更新链最后一步写入，比下载更不该妥协：
	// 少一个判断，被换过的包就能顶替主程序，而且换完就再也救不回来。
	if (!sha256Hex || sha256Hex[0] == 0) {
		SetError(errorText, errorChars, L"没有校验值，拒绝替换当前程序");
		return FALSE;
	}

	// 1) 先把包落到程序目录下的 .new。同卷改名是原子的，避免出现"原名腾出来了
	//    但新文件还没到"的窗口期。
	DeleteFileW(stagedPath.c_str());
	if (!CopyFileW(packagePath, stagedPath.c_str(), FALSE)) {
		const DWORD code = GetLastError();
		SetError(errorText, errorChars,
			(code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
			? L"更新包不存在，可能已被清理"
			: L"无法把更新包写入程序目录，错误码 " + std::to_wstring(code));
		return FALSE;
	}

	// 2) 校验搬过来的这一份，而不是临时目录里的原件 —— 中间被换掉也拦得住。
	WIN32_FILE_ATTRIBUTE_DATA packageInfo{};
	WIN32_FILE_ATTRIBUTE_DATA stagedInfo{};
	if (!GetFileAttributesExW(packagePath, GetFileExInfoStandard, &packageInfo) ||
		!GetFileAttributesExW(stagedPath.c_str(), GetFileExInfoStandard, &stagedInfo) ||
		packageInfo.nFileSizeHigh != stagedInfo.nFileSizeHigh ||
		packageInfo.nFileSizeLow != stagedInfo.nFileSizeLow) {
		DeleteFileW(stagedPath.c_str());
		SetError(errorText, errorChars, L"更新包复制后大小不一致，已丢弃");
		return FALSE;
	}
	// 校验值是必填的（上面已经拦过空值），这里不再有"跳过校验"的分支。
	{
		WCHAR actual[65] = {};
		if (!JUpdater_FileSha256(stagedPath.c_str(), actual, _countof(actual))) {
			DeleteFileW(stagedPath.c_str());
			SetError(errorText, errorChars, L"无法计算新版本校验值");
			return FALSE;
		}
		if (_wcsicmp(actual, sha256Hex) != 0) {
			DeleteFileW(stagedPath.c_str());
			SetError(errorText, errorChars, L"新版本校验失败（SHA-256 不匹配），已丢弃");
			return FALSE;
		}
	}

	// 3) 自己改名让位。改不了说明文件被别的东西占着（杀软在扫、调试器附着、
	//    放在网络盘上），这一步失败就交给独立更新器等本进程退出后再换。
	DeleteFileW(backupPath.c_str());
	if (!MoveFileExW(exePath.c_str(), backupPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
		const DWORD code = GetLastError();
		DeleteFileW(stagedPath.c_str());
		SetError(errorText, errorChars,
			L"当前程序文件被占用，无法让位，错误码 " + std::to_wstring(code));
		return FALSE;
	}

	// 4) 新版本占回原名。
	if (!MoveFileExW(stagedPath.c_str(), exePath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
		const DWORD code = GetLastError();
		MoveFileExW(backupPath.c_str(), exePath.c_str(), MOVEFILE_REPLACE_EXISTING);
		DeleteFileW(stagedPath.c_str());
		SetError(errorText, errorChars,
			L"写入新版本失败，已回滚，错误码 " + std::to_wstring(code));
		return FALSE;
	}

	// 5) 拉起新版本。它带 --after-update，会等本进程放开单实例互斥体再继续，
	//    顺手把 .old 删掉。
	std::wstring commandLine;
	commandLine.push_back(kQuote);
	commandLine += exePath;
	commandLine.push_back(kQuote);
	commandLine += L" --after-update";
	std::vector<WCHAR> mutableCommand(commandLine.begin(), commandLine.end());
	mutableCommand.push_back(static_cast<WCHAR>(0));

	STARTUPINFOW startup{};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process{};
	const BOOL started = CreateProcessW(exePath.c_str(), mutableCommand.data(), nullptr, nullptr,
		FALSE, CREATE_UNICODE_ENVIRONMENT, nullptr, directory.c_str(), &startup, &process);
	if (!started) {
		const DWORD code = GetLastError();
		// 回滚：删掉刚搬进来的新文件，把 .old 改回原名，用户还能继续用旧版本。
		const BOOL reverted = DeleteFileW(exePath.c_str()) &&
			MoveFileExW(backupPath.c_str(), exePath.c_str(), MOVEFILE_REPLACE_EXISTING);
		if (reverted) {
			SetError(errorText, errorChars,
				L"新版本无法启动，已回滚到原版本，错误码 " + std::to_wstring(code));
		}
		else {
			SetError(errorText, errorChars,
				L"新版本无法启动，且回滚失败。请把 " + backupPath + L" 改名为 " + exePath);
		}
		return FALSE;
	}
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	return TRUE;
}

void JUpdater_CleanupOldImages()
{
	const std::wstring exePath = OwnExePath();
	if (exePath.empty()) return;
	const size_t slash = exePath.find_last_of(L'\\');
	if (slash == std::wstring::npos) return;
	const std::wstring pattern = exePath.substr(0, slash + 1) + L"*.old";
	WIN32_FIND_DATAW data{};
	HANDLE find = FindFirstFileW(pattern.c_str(), &data);
	if (find == INVALID_HANDLE_VALUE) return;
	std::vector<std::wstring> victims;
	do {
		if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
			victims.push_back(exePath.substr(0, slash + 1) + data.cFileName);
		}
	} while (FindNextFileW(find, &data));
	FindClose(find);
	// 进程内换文件之后，新实例起来时旧进程可能还没完全退干净，.old 仍然被映像
	// 占着，删除会报拒绝访问。少量重试就够 —— 进程一退映像随即释放。
	for (const std::wstring& victim : victims) {
		for (int attempt = 0; attempt < 6; ++attempt) {
			if (DeleteFileW(victim.c_str())) break;
			Sleep(100);
		}
	}
}
