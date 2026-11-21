#pragma once

// 反馈上行的客户端签名。
//
// 与 UpdaterTicket.h 的方向正好相反：
//   票据  服务器签 → 客户端验，公钥在客户端手里
//   反馈  客户端签 → 服务器验，公钥在服务器手里
//
// 为什么客户端必须握私钥
// ----------------------
// 反馈是匿名用户主动送来的东西，服务端没有任何先验凭据能证明"这条请求来自
// DzjsTrainer"。TLS 只证明对端连的是我们的域名，证明不了对端是那个程序 ——
// 一个 curl 脚本同样能完成 TLS 握手，还能原样复制 User-Agent。唯一能自证
// 的方式，是让程序持有一份只有官方构建才有的签名材料。
//
// 这把钥匙防得住什么、防不住什么（必须记住，别高估它）
//   · 防住：随手 curl 一下就往库里灌垃圾；拿脚本跑量刷接口；改包冒充别人。
//   · 防不住：专门针对这个 exe 的逆向。客户端是开源的，私钥必然能被挖出来，
//     挖出来之后他就能伪造"官方客户端"。所以真正的防线是服务端的验签 +
//     时间窗 + nonce 去重 + 限流四道一起，而不是私钥藏得好。
//   · 也证明不了"这台机器没被改过"——只能证明凭据来自官方构建。
//
// 泄漏后的处置：换钥 + 发版。旧版本会因验签失败而无法提交反馈，这是有意的：
// 宁可让旧版反馈失效，也不留一个能继续灌库的口子。
//
// 签名内容
// --------
//   canonical = "DZJS-FEEDBACK/1|<ts>|<nonce>|<metaSha>|<bundleSha>"
//
//   请求头    X-Dzjs-Feedback-Sig: 1|<ts>|<nonce>|<metaSha>|<bundleSha>|<sig>
//
//   ts        客户端 UTC unix 秒，服务端只认 ±300 秒
//   nonce     16 字节随机数的 32 位十六进制，服务端保留 10 分钟防重放
//   metaSha   meta JSON 原始字节的 SHA-256，64 位十六进制
//   bundleSha 反馈包原始字节的 SHA-256
//   sig       256 字节 RSA-2048 PKCS#1 v1.5 SHA-256，大端十六进制
//
// 为什么哈希而不是直接把正文塞进签名：正文最大 8 MB，签名前要先拼一遍、
// 服务端验签前也要再拼一遍，纯属浪费；哈希把被签的内容压到 64 字符，同时
// 把"服务端收到的字节"和"客户端签的字节"绑死 —— 中途改一个字节就验不过。
//
// 为什么这里用 CNG/BCrypt，而票据验签用 CryptoAPI
// ------------------------------------------------
// 票据那边没得选：主程序的 GUI 进程里 CNG 取不到算法提供程序（Schannel 也是
// 同一个病根），只能走 CryptoAPI 那条独立实现。反馈签名只在更新器子进程里跑，
// 那个进程的 CNG 是好的（同一份代码已经在用 BCryptGenRandom 和
// BCryptOpenAlgorithmProvider 算文件哈希）。
//
// 用 CNG 还有一个实际好处：CryptSignHash 只认 dwKeySpec，不接收
// CryptImportKey 出来的句柄，必须先把私钥导入一个临时 key container ——
// 那会在用户 profile 里留文件，还要负责删。BCryptImportKeyPair 直接给句柄，
// 全程不落盘。

#include <windows.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "crypt32.lib")

// 用到 SHA-256、hex 与 SecureZeroMemory 的约定，直接依赖那个头文件，
// 不要求调用方先包含它。
#include "UpdaterTicket.h"

// 有真私钥就用真的，克隆下来的仓库用示例私钥 —— 与 UpdaterTicketKey.h 同一套
// __has_include 回退，保证两种情形都能编过，且失败方向安全（示例钥签的请求
// 正式服务端一定拒收，不会误放行）。
#if defined(__has_include)
#	if __has_include("FeedbackSignKey.h")
#		include "FeedbackSignKey.h"
#	else
#		include "FeedbackSignKey.h.example"
#	endif
#else
#	include "FeedbackSignKey.h"
#endif

namespace jup_feedback_sign
{
	// 协议版本。改签名覆盖面时一起改，服务端据第一段判断走哪套规则。
	inline const char* ProtocolTag()
	{
		return "DZJS-FEEDBACK/1";
	}

	inline const char* HeaderName()
	{
		return "X-Dzjs-Feedback-Sig";
	}

	// 服务端的时间窗。客户端不做校验（本机时间不准的人反而最需要能提交），
	// 只在失败原因里提示他去对时。
	const unsigned long long kServerWindowSeconds = 300;

	namespace detail
	{
		// 错误码一律用八位十六进制报。CryptoAPI/CNG 的失败码全是 0x8xxxxxxx 形状，
		// 十进制对人眼毫无意义 —— 搜都搜不出来。
		inline std::wstring Hex32(unsigned long value)
		{
			wchar_t text[16] = {};
			swprintf_s(text, L"%08x", static_cast<unsigned int>(value));
			return text;
		}

		// 这里曾经有一份手写的 ASN.1 解析 + BCRYPT_RSAKEY_BLOB 拼装。删掉的原因
		// 是 BCryptImportKeyPair 对那份 BLOB 一直回 STATUS_UNSUCCESSFUL
		// (0xC0000001)：magic、位长、e/n/p1/p2 都逐个核对过，密钥的数学关系
		// （p*q==n、d*e≡1、dp/dq/qInv）也验算无误，仍然拒收。CryptoAPI 的
		// CryptDecodeObjectEx(PKCS_RSA_PRIVATE_KEY) 一步就能把 DER 变成它自己
		// 认的 PRIVATEKEYBLOB，省掉这一整段易错的拼装。

		// AES-256-CBC 解出私钥 DER。
		//
		// 密钥与密文在同一个头文件里，作用仅是让私钥不以明文形态躺在 exe 中。
		// 结构上就不可能保密，别把它当成保密措施。
		inline bool DecryptKeyMaterial(std::vector<unsigned char>& der, std::wstring& error)
		{
			der.clear();
			error.clear();

			BCRYPT_ALG_HANDLE algorithm = nullptr;
			BCRYPT_KEY_HANDLE key = nullptr;
			bool ok = false;
			NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM,
				nullptr, 0);
			if (BCRYPT_SUCCESS(status)) {
				status = BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE,
					reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
					static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_CBC)), 0);
			}
			if (BCRYPT_SUCCESS(status)) {
				status = BCryptGenerateSymmetricKey(algorithm, &key, nullptr, 0,
					const_cast<PUCHAR>(kFeedbackKeyAesKey),
					static_cast<ULONG>(sizeof(kFeedbackKeyAesKey)), 0);
			}
			if (BCRYPT_SUCCESS(status)) {
				std::vector<unsigned char> iv(kFeedbackKeyIv,
					kFeedbackKeyIv + sizeof(kFeedbackKeyIv));
				std::vector<unsigned char> buffer(sizeof(kFeedbackKeyCipher));
				ULONG written = 0;
				// BCRYPT_BLOCK_PADDING 让 CNG 顺带去掉 PKCS#7 填充。
				// 参数顺序是 hKey, 输入, 输入长度, pPaddingInfo, pbIV, cbIV,
				// 输出, 输出容量, 实际写出, dwFlags。pPaddingInfo 在 IV 前面，
				// 这两者位置写反了编译器只报类型不匹配，不报"参数含义错"。
				status = BCryptDecrypt(key, const_cast<PUCHAR>(kFeedbackKeyCipher),
					static_cast<ULONG>(sizeof(kFeedbackKeyCipher)),
					nullptr, iv.data(), static_cast<ULONG>(iv.size()),
					buffer.data(), static_cast<ULONG>(buffer.size()), &written,
					BCRYPT_BLOCK_PADDING);
				if (BCRYPT_SUCCESS(status) && written > 0) {
					der.assign(buffer.begin(), buffer.begin() + written);
					ok = true;
				}
			}
			if (key) BCryptDestroyKey(key);
			if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
			if (!ok) {
				error = L"无法解出签名私钥（0x" + std::to_wstring(static_cast<unsigned long>(status)) + L"）";
			}
			return ok;
		}
	}

	// 对 canonical 串签名，输出 512 个字符的小写十六进制；失败时 error 给出原因。
	//
	// 每次调用都重新解密、重新导入：一次反馈只签一次，省下句柄缓存带来的
	// 生命周期问题，也让私钥在内存里的停留时间尽可能短。
	inline bool Sign(const std::string& message, std::string& signatureHex, std::wstring& error)
	{
		signatureHex.clear();
		error.clear();

		if (kFeedbackKeyIsPlaceholder) {
			// 开源构建用示例私钥，正式服务端只信正式公钥。这里说清楚，
			// 否则用户看到的是"签名无效"，会以为是自己机器的问题。
			error = L"当前为开源示例构建，服务端不接受它的反馈签名";
			return false;
		}

		std::vector<unsigned char> der;
		if (!detail::DecryptKeyMaterial(der, error)) return false;

		// 解出来的 DER 长度与首字节只用来报错：这两个值足以区分"解密出来的
		// 不是 DER"和"DER 本身有问题"，又不会泄露密钥内容。
		const size_t derLength = der.size();
		const unsigned int derHead = der.empty() ? 0u : der[0];

		// PKCS#1 DER → CSP 认的 PRIVATEKEYBLOB。这一步由 crypt32 完成，省掉
		// 自己拼 BCRYPT_RSAKEY_BLOB 的一大段代码。
		BYTE* keyMaterial = nullptr;
		DWORD keyMaterialSize = 0;
		const BOOL decoded = CryptDecodeObjectEx(X509_ASN_ENCODING, PKCS_RSA_PRIVATE_KEY,
			der.data(), static_cast<DWORD>(der.size()), CRYPT_DECODE_ALLOC_FLAG, nullptr,
			&keyMaterial, &keyMaterialSize);
		// DER 已经没用了，立刻抹掉 —— 晚一步就可能被写进崩溃转储。
		SecureZeroMemory(der.data(), der.size());
		der.clear();
		if (!decoded || keyMaterial == nullptr) {
			wchar_t message[160] = {};
			swprintf_s(message, L"无法解析签名私钥（%zu 字节，首字节 0x%02x，0x%s）",
				derLength, derHead, detail::Hex32(GetLastError()).c_str());
			error = message;
			return false;
		}

		// 容器名带 PID 与 tick，避免两个实例撞车。
		wchar_t container[64] = {};
		swprintf_s(container, L"DzjsFbSign-%lu-%llu",
			static_cast<unsigned long>(GetCurrentProcessId()),
			static_cast<unsigned long long>(GetTickCount64()));

		HCRYPTPROV provider = 0;
		HCRYPTKEY imported = 0;
		HCRYPTHASH hash = 0;
		bool ok = false;
		std::wstring failure;

		if (!CryptAcquireContextW(&provider, container, MS_ENH_RSA_AES_PROV_W,
			PROV_RSA_AES, CRYPT_NEWKEYSET)) {
			failure = L"无法创建签名容器（0x" + detail::Hex32(GetLastError()) + L"）";
		} else if (!CryptImportKey(provider, keyMaterial, keyMaterialSize, 0, 0, &imported)) {
			failure = L"无法导入签名私钥（0x" + detail::Hex32(GetLastError()) + L"）";
		} else {
			// 导入后密钥落在 AT_SIGNATURE 还是 AT_KEYEXCHANGE，由 BLOB 里的
			// magic（RSA2 / RSA1）决定。不猜，直接问 provider 哪个槽里有 ——
			// 猜错的报应就是 NTE_BAD_KEYSET(0x80090016)，从错误码看不出是
			// 猜错槽位还是压根没导入。
			DWORD keySpec = 0;
			HCRYPTKEY probe = 0;
			if (CryptGetUserKey(provider, AT_SIGNATURE, &probe)) {
				CryptDestroyKey(probe);
				keySpec = AT_SIGNATURE;
			} else if (CryptGetUserKey(provider, AT_KEYEXCHANGE, &probe)) {
				CryptDestroyKey(probe);
				keySpec = AT_KEYEXCHANGE;
			}

			if (keySpec == 0) {
				failure = L"签名容器里没有可用的私钥";
			} else if (!CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
				failure = L"无法初始化摘要（0x" + detail::Hex32(GetLastError()) + L"）";
			} else if (!CryptHashData(hash, reinterpret_cast<const BYTE*>(message.data()),
				static_cast<DWORD>(message.size()), 0)) {
				failure = L"摘要计算失败（0x" + detail::Hex32(GetLastError()) + L"）";
			} else {
				DWORD size = 0;
				if (!CryptSignHashW(hash, keySpec, nullptr, 0, nullptr, &size)) {
					failure = L"签名失败（0x" + detail::Hex32(GetLastError()) + L"）";
				} else {
					std::vector<BYTE> signature(size);
					if (CryptSignHashW(hash, keySpec, nullptr, 0, signature.data(), &size)
						&& size == 256) {
						// CryptoAPI 给的是小端序，OpenSSL 那边要标准大端。
						for (size_t i = 0; i < size / 2; ++i) {
							const BYTE head = signature[i];
							signature[i] = signature[size - 1 - i];
							signature[size - 1 - i] = head;
						}
						signatureHex = jup_ticket::ToHex(signature.data(), size);
						ok = !signatureHex.empty();
					}
					SecureZeroMemory(signature.data(), signature.size());
				}
			}
		}

		if (hash) CryptDestroyHash(hash);
		if (imported) CryptDestroyKey(imported);
		if (provider) {
			CryptReleaseContext(provider, 0);
			// 上面那次 Release 只是放手，私钥文件还在用户 profile 里。
			HCRYPTPROV discard = 0;
			CryptAcquireContextW(&discard, container, MS_ENH_RSA_AES_PROV_W,
				PROV_RSA_AES, CRYPT_DELETEKEYSET);
		}
		if (keyMaterial) {
			SecureZeroMemory(keyMaterial, keyMaterialSize);
			LocalFree(keyMaterial);
		}
		if (!ok) error = failure.empty() ? L"签名失败" : failure;
		return ok;
	}

	// 组装 canonical 串。客户端与服务端必须逐字一致 —— 这里改一个字，那边
	// 就得跟着改，否则全部请求验不过，而现象只是"签名无效"。
	inline std::string Canonical(unsigned long long timestamp, const std::string& nonce,
		const std::string& metaSha, const std::string& bundleSha)
	{
		return std::string(ProtocolTag()) + "|" + std::to_string(timestamp) + "|" + nonce
			+ "|" + metaSha + "|" + bundleSha;
	}
}
