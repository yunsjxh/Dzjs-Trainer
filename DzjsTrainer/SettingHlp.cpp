#include "stdafx.h"
#include "SettingHlp.h"
#include "StringHlp.h"

namespace
{
	// ini 里可能同时存在两种写法的中文：
	//   · 本程序用 WritePrivateProfileStringW 写 → 文件里是系统 ANSI(GBK) 字节；
	//   · 外部工具、旧版本按 UTF-8 写 → 文件里是 UTF-8 字节。
	// W 版 API 一律按 ANSI 解读，于是第二种会读成 "娴嬭瘯" 这种乱码。
	// 这里做一次可逆判断：把读到的宽字符按 ANSI 转回字节，如果这串字节本身是
	// 合法 UTF-8，就认定原文是 UTF-8，按 UTF-8 重新解码。
	bool IsValidUtf8(const std::string& text)
	{
		const unsigned char* bytes = reinterpret_cast<const unsigned char*>(text.data());
		const size_t size = text.size();
		size_t index = 0;
		while (index < size) {
			const unsigned char lead = bytes[index];
			size_t extra = 0;
			if (lead < 0x80) { ++index; continue; }
			if ((lead & 0xE0) == 0xC0) extra = 1;
			else if ((lead & 0xF0) == 0xE0) extra = 2;
			else if ((lead & 0xF8) == 0xF0) extra = 3;
			else return false;
			if (index + extra >= size) return false;
			for (size_t offset = 1; offset <= extra; ++offset)
				if ((bytes[index + offset] & 0xC0) != 0x80) return false;
			index += extra + 1;
		}
		return true;
	}

	std::wstring DecodeBytes(UINT codePage, const std::string& text)
	{
		if (text.empty()) return std::wstring();
		const int length = MultiByteToWideChar(codePage, 0, text.data(),
			static_cast<int>(text.size()), nullptr, 0);
		if (length <= 0) return std::wstring();
		std::wstring result(static_cast<size_t>(length), L' ');
		MultiByteToWideChar(codePage, 0, text.data(), static_cast<int>(text.size()),
			&result[0], length);
		return result;
	}

	std::wstring RepairUtf8ReadAsAnsi(const std::wstring& text)
	{
		if (text.empty()) return text;
		bool ascii = true;
		for (wchar_t ch : text) {
			if (ch >= 0x80) { ascii = false; break; }
		}
		if (ascii) return text;

		const int bytesLength = WideCharToMultiByte(CP_ACP, 0, text.c_str(),
			static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
		if (bytesLength <= 0) return text;
		std::string bytes(static_cast<size_t>(bytesLength), ' ');
		WideCharToMultiByte(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()),
			&bytes[0], bytesLength, nullptr, nullptr);
		if (!IsValidUtf8(bytes)) return text;

		const std::wstring decoded = DecodeBytes(CP_UTF8, bytes);
		if (decoded.empty() || decoded == text) return text;
		return decoded;
	}
}

SettingHlpInternal::SettingHlpInternal(LPCWSTR setttingFilePath)
{
	wcscpy_s(this->setttingFilePath, setttingFilePath);
}
SettingHlpInternal::~SettingHlpInternal()
{
}

bool SettingHlpInternal::GetSettingBool(LPCWSTR key, bool defaultValue, LPCWSTR appKey)
{
	WCHAR w[16];
	GetPrivateProfileString(appKey, key, defaultValue ? L"TRUE" : L"FALSE", w, 16, setttingFilePath);
	return StringHlp::StrToBoolW(w);
}
int SettingHlpInternal::GetSettingInt(LPCWSTR key, int defaultValue, LPCWSTR appKey)
{
	WCHAR w[32];
	swprintf_s(w, L"%d", defaultValue);
	GetPrivateProfileString(appKey, key, w, w, 32, setttingFilePath);
	return StringHlp::StrToIntW(w);
}
std::wstring SettingHlpInternal::GetSettingStr(LPCWSTR key, LPCWSTR defaultValue, size_t bufferSize, LPCWSTR appKey)
{
	std::wstring tmp;
	tmp.resize(bufferSize + 1);
	GetPrivateProfileString(appKey, key, defaultValue, (wchar_t *)tmp.data(), bufferSize + 1, setttingFilePath);
	// GetPrivateProfileString only fills up to the first NUL; the rest keeps the
	// zeros resize() put there. Without trimming, the returned string carries
	// trailing NULs and never compares equal to a plain literal (e.g. L"image"),
	// which silently breaks every `mode == L"image"` style check.
	const size_t end = tmp.find(L'\0');
	if (end != std::wstring::npos) tmp.resize(end);
	// 读到的是 UTF-8 写进 ini 的中文时，这里顺手还原。
	return RepairUtf8ReadAsAnsi(tmp);
}

bool SettingHlpInternal::SetSettingBool(LPCWSTR key, bool value, LPCWSTR appKey)
{
	return WritePrivateProfileString(appKey, key, value ? L"TRUE" : L"FALSE", setttingFilePath);
}
bool SettingHlpInternal::SetSettingInt(LPCWSTR key, int value, LPCWSTR appKey)
{
	LPCWSTR intStr = StringHlp::IntToStrW(value);
	bool rs =  WritePrivateProfileString(appKey, key, intStr, setttingFilePath);
	delete intStr;
	return rs;
}
bool SettingHlpInternal::SetSettingStr(LPCWSTR key, LPCWSTR value, LPCWSTR appKey)
{
	return WritePrivateProfileString(appKey, key, value, setttingFilePath);
}
bool SettingHlpInternal::SetSettingStr(LPCWSTR key, std::wstring value, LPCWSTR appKey)
{
	return SetSettingStr(key, value.c_str(), appKey);
}
