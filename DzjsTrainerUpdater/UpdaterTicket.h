#pragma once

#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <string>
#include <cstring>
#include <vector>

// 验签走 CryptoAPI（CSP + ASN.1 解码）。header-only 里声明依赖，
// 免得三个包含它的工程各漏一个库。
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "crypt32.lib")

// ============================================================ 时效票据
//
// 服务端用 RSA 私钥签发，客户端用内置公钥验证。
//
// 为什么不是对称密钥：
//   早期版本用 HMAC + 编译进客户端的共享密钥。那个方案的开源即失效 ——
//   密钥和还原算法在同一个头文件里，照着抄二十行就能拿到；而 HMAC 的密钥
//   既能签也能验，拿到的人等于握有签发权，能伪造完整票据去骗别的客户端。
//   私钥签名没有这个对称性：客户端只存公钥，公钥泄漏无所谓，它只能验不能签。
//
// 请求与响应：
//
//   客户端 → 服务端
//     ?manifest=1&n=<nonce>&t=<unix>&v=<客户端版本>
//
//   服务端 → 客户端
//     X-Dzjs-Ticket: 1|<n>|<issued>|<expires>|manifest|<sha256(body)>|<sig>
//     sig = hex(RSA-2048 PKCS#1 v1.5 SHA-256("DZJS-TICKET/1|<n>|...|<sha256(body)>"))
//
//   客户端校验：票据签名、nonce 回显、签发/过期时间、以及正文哈希。
//
// 由此得到三条互相独立的安全性质：
//   · 只有握着私钥的服务器能签出可验的票据 —— 这是「真实性」，与 TLS 证书
//     无关，Let's Encrypt 续期换钥匙完全不影响；中间人哪怕拿到了任意 CA 给
//     这个域名签的合法证书，也伪造不出 sig。
//   · nonce 回显把票据钉死在这一次问答上 —— 重放旧票据、把 A 的响应塞进 B 的
//     请求里，都会因为 nonce 对不上而失败。
//   · issued/expires 给出时效，bodyHash 把正文绑进签名 —— 连"TLS 之后、落盘
//     之前"被代理改一字节都能拦住，比只看证书强一档。
//
// 关于验签为什么不走 BCrypt：
//   本程序的 GUI 进程里 CNG 是坏的（同一进程里 Schannel 报
//   SEC_E_SECPKG_NOT_FOUND，BCrypt 算 SHA-256 都返回失败且不设 Win32 错误码）。
//   但 CryptoAPI（advapi32 的 CSP + crypt32 的 ASN.1 解码）走的是另一条完全
//   独立的实现，实测可用，所以验签走它，SHA-256 仍用下面自带的那份。
//
// 一个必须记住的坑：CryptoAPI 的 CryptVerifySignature 要求签名是**小端序**，
// 而 OpenSSL 的 openssl_sign 输出标准大端序。验签前必须把 256 字节反转，
// 否则永远对不上（错误码 0x80090006 NTE_BAD_SIGNATURE）。
//
// 放行顺序（重要）：任何取安装包的动作都必须先完成一次票据校验。验不过就
// 一个字节都不下载 —— 清单决定"下哪个包、包该是什么哈希"，它是整条链的根，
// 根被换掉之后再怎么比对 SHA-256 都只是在核对攻击者自己填的值。
//
// 换钥：公钥在下面 kPublicKeyDer，私钥在服务端 ticket-sign.pem（不进版本库）。
//       换钥必须发新版客户端，否则旧客户端验不过新票据。

namespace jup_ticket
{
	// 票据自身有效期上限。超过这个值的票直接判废：服务端万一被拿下，
	// 也签不出一张"永不过期"的通行证。
	const unsigned long long kTtlMaxSeconds = 900;

	// 允许的本地时钟偏差。用户机器时间不准是常态，卡太死会误伤；
	// 卡太松则给了重放窗口。10 分钟是够用又不至于离谱的折中。
	const unsigned long long kClockSkewSeconds = 600;

	// 重放判定用的容差，单独一个常量，故意比 kClockSkewSeconds 小得多。
	//
	// 这里不能用那个 10 分钟容差：签发时间是服务端给的，同一台服务器上必然
	// 单调递增，客户端见过的最大值不会被正常请求"倒退"。留 60 秒只是为了让
	// 多台服务器之间几秒到几十秒的时钟差、以及并发请求乱序不至于误杀。
	// 用 600 秒的话，把系统时间往回拨 10 分钟就能让旧票复活，等于白做。
	const unsigned long long kReplaySkewSeconds = 60;

	// 票据走响应头而不是正文：三个调用点都已经攥着 request 句柄，
	// 取头比再塞一遍 JSON 解析便宜，也不会跟清单字段互相污染。
	inline const wchar_t* TicketHeaderName()
	{
		return L"X-Dzjs-Ticket";
	}

	// ------------------------------------------------------------ SHA-256
	//
	// 纯软件实现。主程序进程里 BCrypt 的算法提供程序取不到（返回失败且不设
	// Win32 错误码），所以不能指望 CNG。更新包只有几 MB，算一遍几十毫秒。
	namespace detail
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

		inline void Sha256Reset(Sha256Context* context)
		{
			static const unsigned int init[8] = {
				0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
				0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
			};
			for (int i = 0; i < 8; ++i) context->state[i] = init[i];
			context->total = 0;
			context->used = 0;
		}

		inline void Sha256Transform(Sha256Context* context, const unsigned char* chunk)
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
			unsigned int a = context->state[0], b = context->state[1];
			unsigned int c = context->state[2], d = context->state[3];
			unsigned int e = context->state[4], f = context->state[5];
			unsigned int g = context->state[6], h = context->state[7];
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
			context->state[0] += a; context->state[1] += b;
			context->state[2] += c; context->state[3] += d;
			context->state[4] += e; context->state[5] += f;
			context->state[6] += g; context->state[7] += h;
		}

		inline void Sha256Update(Sha256Context* context, const unsigned char* data, size_t length)
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

		inline void Sha256Finish(Sha256Context* context, unsigned char digest[32])
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
			// 长度字段单独补最后一块：上面的 Update 已经把 used 走到 56，
			// 这里不能再累计 total，直接写尾巴。
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

		inline void Sha256(const void* data, size_t length, unsigned char digest[32])
		{
			Sha256Context context;
			Sha256Reset(&context);
			Sha256Update(&context, static_cast<const unsigned char*>(data), length);
			Sha256Finish(&context, digest);
		}
	}

	// ------------------------------------------------------------ 小工具

	inline std::string ToHex(const unsigned char* bytes, size_t count)
	{
		static const char digits[] = "0123456789abcdef";
		std::string hex;
		hex.reserve(count * 2);
		for (size_t i = 0; i < count; ++i) {
			hex.push_back(digits[bytes[i] >> 4]);
			hex.push_back(digits[bytes[i] & 0x0f]);
		}
		return hex;
	}

	inline int HexDigit(char ch)
	{
		if (ch >= '0' && ch <= '9') return ch - '0';
		if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
		if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
		return -1;
	}

	// 定长 hex 解码。长度不对或有非 hex 字符就整体失败，不做半截解析。
	inline bool FromHex(const std::string& hex, unsigned char* out, size_t count)
	{
		if (hex.size() != count * 2) return false;
		for (size_t i = 0; i < count; ++i) {
			const int high = HexDigit(hex[i * 2]);
			const int low = HexDigit(hex[i * 2 + 1]);
			if (high < 0 || low < 0) return false;
			out[i] = static_cast<unsigned char>((high << 4) | low);
		}
		return true;
	}

	// 常数时间比较。签名/摘要一律走这里，别用 operator==：
	// 后者遇到第一个不同字节就返回，理论上能被计时侧信道利用。
	inline bool EqualConst(const std::string& left, const std::string& right)
	{
		if (left.size() != right.size()) return false;
		unsigned char diff = 0;
		for (size_t i = 0; i < left.size(); ++i) {
			diff |= static_cast<unsigned char>(left[i] ^ right[i]);
		}
		return diff == 0;
	}

	inline std::string Sha256Hex(const std::string& text)
	{
		unsigned char digest[32] = {};
		detail::Sha256(text.data(), text.size(), digest);
		return ToHex(digest, sizeof(digest));
	}

	// 一次性材料用完立刻抹掉。
	//
	// 用 SecureZeroMemory 而不是 memset：普通 memset 写进去的值编译器认为是
	// 死存储，优化打开后会被整段删掉，等于没擦。SecureZeroMemory 明确不会被
	// 优化掉，是这类场景的标准做法。
	//
	// 换成非对称签名后，客户端手里已经没有"泄漏即致命"的东西了（公钥只能验签），
	// 这里保留下来的用途是擦 nonce 的原始熵源。
	inline void SecureZero(void* data, size_t length)
	{
		if (data && length) SecureZeroMemory(data, length);
	}

	// ------------------------------------------------------------ 服务端公钥
	//
	// 2048 位 RSA 公钥的 SubjectPublicKeyInfo（DER），294 字节。它由
	// tools/make-ticket-key.py 从 update-server/ticket-sign.pub.der 生成为头文件，
	// 不从源码手写 —— 手抄 294 个十六进制字节只会抄错。
	//
	// 为什么要走构建期注入，尽管公钥泄漏本身无害：
	//   开源出去的这份源码必须能编译，但不该顺手就编出一个"看起来能连官方服务器"
	//   的程序。版本库里只带 UpdaterTicketKey.h.example —— 示例公钥，对应私钥
	//   update-server/ticket-sign.pem.example 是公开的，所以它签不出任何被正式
	//   客户端接受的票据。真公钥生成的 UpdaterTicketKey.h 被 .gitignore 挡住。
	//
	//   下面的 __has_include 让两种情形都能编过：本地有真钥用真钥；克隆下来没真钥
	//   就用示例钥，运行期以"当前为开源占位公钥"明确拒绝。失败方向是安全的。
	//
	// 换钥：换服务端私钥 -> 跑一次 tools/make-ticket-key.py -> 重新编译客户端。
	//       旧客户端只认编进去的那把，所以换钥必须发版。
#if defined(__has_include)
#	if __has_include("UpdaterTicketKey.h")
#		include "UpdaterTicketKey.h"
#	else
#		include "UpdaterTicketKey.h.example"
#	endif
#else
#	include "UpdaterTicketKey.h"
#endif

	// 反转一段字节：OpenSSL 的大端签名 -> CryptoAPI 要的小端签名。
	inline void ReverseBytes(std::vector<unsigned char>& data)
	{
		for (size_t i = 0; i < data.size() / 2; ++i) {
			const unsigned char head = data[i];
			data[i] = data[data.size() - 1 - i];
			data[data.size() - 1 - i] = head;
		}
	}

	// ------------------------------------------------------------ RSA 验签
	//
	// message      被签的原文（票据前六段拼成的 "DZJS-TICKET/1|..." 串）
	// signatureHex 签名十六进制，2048 位密钥下固定 512 个字符
	//
	// 走 CryptoAPI（CSP）而不是 CNG/BCrypt：本程序的 GUI 进程里 CNG 取不到
	// 算法提供程序，但 CryptoAPI 是另一套独立实现，实测可用。
	//
	// 字节序是必须记住的坑：CryptVerifySignature 要求签名小端序，而 OpenSSL 的
	// openssl_sign 给的是标准大端序。不反转永远验不过，而错误码只有
	// 0x80090006 NTE_BAD_SIGNATURE，看不出是顺序问题。
	inline bool VerifyRsaSignature(const std::string& message, const std::string& signatureHex)
	{
		// 示例公钥不是任何真实服务器的公钥，拿它验签的结果没有意义。
		// 这里提前返回，具体原因由 Verify() 统一报给上层。
		if (kJupTicketKeyIsPlaceholder) return false;

		// 长度卡死：既排除格式垃圾，也避免把超大 hex 解进内存。
		if (signatureHex.size() != 512) return false;

		std::vector<unsigned char> signature(256);
		if (!FromHex(signatureHex, signature.data(), signature.size())) return false;
		ReverseBytes(signature);

		// 解出 SubjectPublicKeyInfo。两步调用：先问长度再填，省一次 LocalFree。
		DWORD infoSize = 0;
		if (!CryptDecodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO, kPublicKeyDer,
			sizeof(kPublicKeyDer), 0, nullptr, nullptr, &infoSize) || infoSize == 0) {
			return false;
		}
		std::vector<unsigned char> infoBuffer(infoSize);
		if (!CryptDecodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO, kPublicKeyDer,
			sizeof(kPublicKeyDer), 0, nullptr, infoBuffer.data(), &infoSize)) {
			return false;
		}
		// 这里不能用 const：CryptImportPublicKeyInfo 的形参是 PCERT_PUBLIC_KEY_INFO。
		CERT_PUBLIC_KEY_INFO* publicInfo =
			reinterpret_cast<CERT_PUBLIC_KEY_INFO*>(infoBuffer.data());

		// 必须是 PROV_RSA_AES：PROV_RSA_FULL 不支持 CALG_SHA_256。
		HCRYPTPROV provider = 0;
		bool ok = false;
		if (CryptAcquireContextW(&provider, nullptr, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES,
			CRYPT_VERIFYCONTEXT)) {
			HCRYPTKEY key = 0;
			if (CryptImportPublicKeyInfo(provider, X509_ASN_ENCODING, publicInfo, &key)) {
				HCRYPTHASH hash = 0;
				if (CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
					if (CryptHashData(hash, reinterpret_cast<const BYTE*>(message.data()),
						static_cast<DWORD>(message.size()), 0)) {
						ok = CryptVerifySignatureW(hash, signature.data(),
							static_cast<DWORD>(signature.size()), key, nullptr, 0) != FALSE;
					}
					CryptDestroyHash(hash);
				}
				CryptDestroyKey(key);
			}
			CryptReleaseContext(provider, 0);
		}
		return ok;
	}


	// ------------------------------------------------------------ 时间

	// 取 UTC unix 秒。不用 CRT 的 time()：更新器 exe 里 _CRT_SECURE_NO_WARNINGS
	// 之类的开关不一定一致，而且 FILETIME 直接给出绝对时间，省一层时区换算。
	inline unsigned long long UnixNow()
	{
		FILETIME fileTime{};
		GetSystemTimeAsFileTime(&fileTime);
		ULARGE_INTEGER value;
		value.LowPart = fileTime.dwLowDateTime;
		value.HighPart = fileTime.dwHighDateTime;
		// 1601-01-01 到 1970-01-01 是 11644473600 秒，单位为 100ns。
		return (value.QuadPart - 116444736000000000ull) / 10000000ull;
	}

	// ------------------------------------------------------------ 随机

	inline void RandomBytes(unsigned char* out, size_t count)
	{
		// RtlGenRandom 走 advapi32，跟 CNG 的算法提供程序没有关系，
		// 在 BCrypt 坏掉的进程里照样能用（这正是选它的原因）。
		typedef BOOLEAN(WINAPI* RtlGenRandomFn)(PVOID, ULONG);
		// FARPROC 到函数指针要走 memcpy：直接 reinterpret_cast 会触发 C4191，
		// 而工程的 /W3 会把它当警告报出来。
		static RtlGenRandomFn generator = []() -> RtlGenRandomFn {
			FARPROC proc = GetProcAddress(GetModuleHandleW(L"advapi32.dll"), "SystemFunction036");
			RtlGenRandomFn fn = nullptr;
			if (proc) memcpy(&fn, &proc, sizeof(fn));
			return fn;
		}();
		if (generator && generator(out, static_cast<ULONG>(count))) return;

		// 兜底：混时间、进程、线程和一个自增计数。
		// nonce 只用于抗重放，不是密钥；但也不能退化成可预测序列。
		static unsigned long long counter = 0;
		unsigned long long seed = GetTickCount64() ^
			(static_cast<unsigned long long>(GetCurrentProcessId()) << 32) ^
			(static_cast<unsigned long long>(GetCurrentThreadId()) << 16) ^
			(++counter * 0x9e3779b97f4a7c15ull);
		for (size_t i = 0; i < count; ++i) {
			seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
			out[i] = static_cast<unsigned char>((seed >> 24) & 0xffu);
		}
	}

	// ------------------------------------------------------------ 挑战

	// 一次请求的挑战材料。nonce 必须回显，所以生命周期要跟着请求走，不能跨请求复用。
	struct Challenge {
		std::string nonce;
		unsigned long long timestamp = 0;
		std::string clientVersion;
	};

	inline Challenge MakeChallenge(const std::string& clientVersion)
	{
		Challenge challenge;
		unsigned char raw[16] = {};
		RandomBytes(raw, sizeof(raw));
		challenge.nonce = ToHex(raw, sizeof(raw));
		// 原始随机字节不要留着：nonce 本身要发出去、不算秘密，但既然只是
		// 一次性的熵源，用完就该消失，免得给"从栈上捞随机数"留机会。
		SecureZero(raw, sizeof(raw));
		challenge.timestamp = UnixNow();
		challenge.clientVersion = clientVersion;
		return challenge;
	}

	// 拼进查询串的部分（以 & 开头，直接接在 ?manifest=1 后面）。
	//
	// 只有 nonce 和时间戳，没有签名：客户端不持有任何可用于签名的材料，这是
	// 刻意的。请求侧不需要证明"我是自家客户端" —— 清单本来就是给所有人看的，
	// 需要证明的是反方向：**响应**确实出自我们的服务器。那由票据签名承担。
	inline std::string ChallengeQuery(const Challenge& challenge)
	{
		return std::string("&n=") + challenge.nonce +
			"&t=" + std::to_string(challenge.timestamp) +
			"&v=" + challenge.clientVersion;
	}

	// ------------------------------------------------------------ 票据校验

	enum VerifyCode {
		kVerifyOk = 0,
		kVerifyMissing,       // 响应里没有票据头
		kVerifyMalformed,     // 段数/格式/数字解析不对
		kVerifyBadSignature,  // 签名验不过 —— 对面不是我们的服务器
		kVerifyNonceMismatch, // nonce 不是这次请求的
		kVerifyExpired,       // 已过期
		kVerifyNotYetValid,   // 签发时间在未来，超出可容忍偏差
		kVerifyTtlTooLong,    // 票据自称有效期太长
		kVerifyBodyMismatch,  // 正文哈希不匹配
		kVerifyReplayed,      // 签发时间比上次见过的还早（时钟被回拨）
		kVerifyPlaceholder,   // 编译用的是开源示例公钥，不是正式公钥
	};

	struct VerifyResult {
		VerifyCode code = kVerifyOk;
		bool Ok() const { return code == kVerifyOk; }
		bool IsAuthFailure() const
		{
			// 占位公钥也算身份校验失败：两者的处理一致（拒绝、不下载任何东西），
			// 只是给用户看的措辞不同。
			return code == kVerifyBadSignature || code == kVerifyPlaceholder;
		}
		const wchar_t* Text() const
		{
			switch (code) {
			case kVerifyOk: return L"通过";
			case kVerifyMissing: return L"服务器未提供时效票据";
			case kVerifyMalformed: return L"时效票据格式错误";
			case kVerifyBadSignature: return L"服务器身份校验失败（票据签名不符）";
			case kVerifyNonceMismatch: return L"时效票据与本次请求不匹配";
			case kVerifyExpired: return L"时效票据已过期";
			case kVerifyNotYetValid: return L"本机时间与服务器相差过大";
			case kVerifyTtlTooLong: return L"时效票据有效期超出上限";
			case kVerifyBodyMismatch: return L"清单内容与票据绑定值不符";
			case kVerifyReplayed: return L"检测到时间被回拨，拒绝旧票据";
			case kVerifyPlaceholder:
				return L"此版本内置的是开源示例公钥，无法校验正式服务器签名，请使用官方发行版";
			default: return L"时效票据校验失败";
			}
		}
	};

	// 票据的签名覆盖面：不带 sig 本身的那一段。
	inline std::string TicketMessage(const std::string& nonce, unsigned long long issued,
		unsigned long long expires, const std::string& scope, const std::string& bodyHash)
	{
		return std::string("DZJS-TICKET/1|") + nonce + "|" + std::to_string(issued) + "|" +
			std::to_string(expires) + "|" + scope + "|" + bodyHash;
	}

	inline bool ParseU64(const std::string& text, unsigned long long& value)
	{
		if (text.empty() || text.size() > 20) return false;
		unsigned long long result = 0;
		for (char ch : text) {
			if (ch < '0' || ch > '9') return false;
			const unsigned long long digit = static_cast<unsigned long long>(ch - '0');
			if (result > (~0ull - digit) / 10ull) return false;
			result = result * 10ull + digit;
		}
		value = result;
		return true;
	}

	// 单调性记录：把见过的最晚签发时间存进 HKCU。
	//
	// 为什么需要它：票据的有效性最终要落到"本地时间"上，而本地时间是用户能改的。
	// 把系统时间调回票据有效期内，一张旧票就能二次使用。记下见过的最晚签发时间，
	// 后到的票据如果比它还早（超过容差），直接判重放 —— 时间回拨就绕不过去了。
	//
	// 不用 HKLM：主程序虽然提权运行，但 HKCU 才是"这个用户"的稳定身份；
	// 写成 REG_SZ 十进制，跨 32/64 位进程读同一份，不会有 WOW64 重定向问题。
	inline const wchar_t* StateKeyPath()
	{
		return L"Software\\DzjsTrainer\\UpdateTicket";
	}

	inline unsigned long long LoadLastIssued()
	{
		HKEY key = nullptr;
		if (RegOpenKeyExW(HKEY_CURRENT_USER, StateKeyPath(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
			return 0;
		}
		wchar_t text[32] = {};
		DWORD type = 0;
		DWORD size = sizeof(text) - sizeof(wchar_t);
		unsigned long long value = 0;
		if (RegQueryValueExW(key, L"LastIssued", nullptr, &type,
			reinterpret_cast<LPBYTE>(text), &size) == ERROR_SUCCESS && type == REG_SZ) {
			text[_countof(text) - 1] = 0;
			value = _wcstoui64(text, nullptr, 10);
		}
		RegCloseKey(key);
		return value;
	}

	inline void SaveLastIssued(unsigned long long value)
	{
		HKEY key = nullptr;
		if (RegCreateKeyExW(HKEY_CURRENT_USER, StateKeyPath(), 0, nullptr, 0,
			KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
			return;
		}
		const std::wstring text = std::to_wstring(value);
		RegSetValueExW(key, L"LastIssued", 0, REG_SZ,
			reinterpret_cast<const BYTE*>(text.c_str()),
			static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)));
		RegCloseKey(key);
	}

	// 校验一张票据。
	//
	// ticket      响应头原值（"1|nonce|issued|expires|scope|bodyHash|sig"）
	// expectNonce 本次请求发出的 nonce
	// body        响应正文；票据里的 bodyHash 必须等于它的 SHA-256
	//
	// 顺序是有讲究的：先验签再信任何字段。否则等于拿攻击者可控的数据去做判断。
	inline VerifyResult Verify(const std::string& ticket, const std::string& expectNonce,
		const std::string& body)
	{
		VerifyResult result;

		// 编译时用的是示例公钥：这个版本无论连谁都不可能验过，与其让上层看到
		// 笼统的"签名不符"、以为是自己网络或服务器的问题，不如直接说明。
		// 判定来自编译期常量，不比对 DER 内容 —— 后者能被伪造，前者不能。
		if (kJupTicketKeyIsPlaceholder) {
			result.code = kVerifyPlaceholder;
			return result;
		}

		if (ticket.empty()) {
			result.code = kVerifyMissing;
			return result;
		}

		// 切 7 段。用 vector 收，段数不对直接判格式错。
		std::vector<std::string> parts;
		size_t start = 0;
		for (;;) {
			const size_t bar = ticket.find('|', start);
			if (bar == std::string::npos) {
				parts.push_back(ticket.substr(start));
				break;
			}
			parts.push_back(ticket.substr(start, bar - start));
			start = bar + 1;
		}
		if (parts.size() != 7 || parts[0] != "1") {
			result.code = kVerifyMalformed;
			return result;
		}

		const std::string& nonce = parts[1];
		const std::string& scope = parts[4];
		const std::string& bodyHash = parts[5];
		unsigned long long issued = 0;
		unsigned long long expires = 0;
		if (!ParseU64(parts[2], issued) || !ParseU64(parts[3], expires) ||
			expires <= issued || nonce.size() != 32 || bodyHash.size() != 64 || scope.empty()) {
			result.code = kVerifyMalformed;
			return result;
		}

		// 1) 签名。这一步过了才说明对面握着私钥，后面几项才有意义。
		//
		// 注意这里不能"先比 expected 再用 EqualConst"：签名是非对称的，
		// 客户端算不出正确的签名值，只能把原文和签名一起交给公钥去验。
		if (!VerifyRsaSignature(TicketMessage(nonce, issued, expires, scope, bodyHash), parts[6])) {
			result.code = kVerifyBadSignature;
			return result;
		}

		// 2) nonce 回显。防"把别处的合法响应塞进来"。
		if (!EqualConst(nonce, expectNonce)) {
			result.code = kVerifyNonceMismatch;
			return result;
		}

		// 3) 时效。
		if (expires - issued > kTtlMaxSeconds) {
			result.code = kVerifyTtlTooLong;
			return result;
		}
		const unsigned long long now = UnixNow();
		if (now > expires + kClockSkewSeconds) {
			result.code = kVerifyExpired;
			return result;
		}
		if (issued > now + kClockSkewSeconds) {
			result.code = kVerifyNotYetValid;
			return result;
		}

		// 4) 单调性。容差比时钟偏差小得多，理由见 kReplaySkewSeconds。
		const unsigned long long lastIssued = LoadLastIssued();
		if (lastIssued > kReplaySkewSeconds && issued + kReplaySkewSeconds < lastIssued) {
			result.code = kVerifyReplayed;
			return result;
		}

		// 5) 正文绑定。到这里 TLS、票据都验过了，仍要确认拿到的正文就是被签的那一段。
		if (!EqualConst(bodyHash, Sha256Hex(body))) {
			result.code = kVerifyBodyMismatch;
			return result;
		}

		if (issued > lastIssued) SaveLastIssued(issued);
		return result;
	}

	// 把响应头里的宽字符串转成窄串。票据只含 ASCII，直接取低字节。
	inline std::string FromWideHeader(const std::wstring& text)
	{
		std::string result;
		result.reserve(text.size());
		for (wchar_t ch : text) {
			if (ch == 0) continue;
			result.push_back(static_cast<char>(ch & 0x7f));
		}
		return result;
	}

	// 从 WinHTTP 请求里取票据头。取不到返回 false，调用方按"没给凭证"处理。
	//
	// 放在这里而不是各调用点：清单请求有三个入口（独立更新器、主程序探测、
	// 以及将来可能加的命令行模式），取头的边界条件（头不存在 vs 缓冲区不够）
	// 复制三遍必然有一遍写错。
	inline bool ReadTicketHeader(HINTERNET request, std::string& ticket)
	{
		ticket.clear();
		if (!request) return false;
		DWORD size = 0;
		if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, TicketHeaderName(),
			WINHTTP_NO_OUTPUT_BUFFER, &size, WINHTTP_NO_HEADER_INDEX)) {
			// 头存在但没给缓冲区时是 ERROR_INSUFFICIENT_BUFFER；头根本不存在时
			// 是 ERROR_WINHTTP_HEADER_NOT_FOUND。只有前者值得再查一次。
			if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return false;
		}
		if (size < sizeof(wchar_t)) return false;
		std::wstring buffer(size / sizeof(wchar_t) + 1, L'\0');
		if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, TicketHeaderName(),
			&buffer[0], &size, WINHTTP_NO_HEADER_INDEX)) {
			return false;
		}
		buffer.resize(size / sizeof(wchar_t));
		ticket = FromWideHeader(buffer);
		return !ticket.empty();
	}
}
