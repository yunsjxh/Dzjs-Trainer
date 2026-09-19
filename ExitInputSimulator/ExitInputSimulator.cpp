#include <windows.h>

#include <iostream>
#include <locale>
#include <string>
#include <vector>

namespace {
constexpr wchar_t kWindowClass[] = L"DzjsTrainerExitConfirm";
constexpr int kEditId = 46001;

struct TestResult {
	std::wstring name;
	LRESULT result = 0;
};

BOOL CALLBACK FindExitWindow(HWND window, LPARAM parameter)
{
	auto* result = reinterpret_cast<HWND*>(parameter);
	if (!result || *result) return FALSE;
	wchar_t className[128]{};
	if (GetClassNameW(window, className, _countof(className)) > 0 &&
		lstrcmpW(className, kWindowClass) == 0) {
		*result = window;
		return FALSE;
	}
	return TRUE;
}

HWND FindExitEdit()
{
	HWND window = nullptr;
	EnumWindows(FindExitWindow, reinterpret_cast<LPARAM>(&window));
	if (!window) return nullptr;
	return GetDlgItem(window, kEditId);
}

bool PutClipboardText(const std::wstring& value)
{
	if (!OpenClipboard(nullptr)) return false;
	EmptyClipboard();
	const SIZE_T bytes = (value.size() + 1) * sizeof(wchar_t);
	HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
	if (!memory) {
		CloseClipboard();
		return false;
	}
	void* data = GlobalLock(memory);
	if (!data) {
		GlobalFree(memory);
		CloseClipboard();
		return false;
	}
	memcpy(data, value.c_str(), bytes);
	GlobalUnlock(memory);
	if (!SetClipboardData(CF_UNICODETEXT, memory)) {
		GlobalFree(memory);
		CloseClipboard();
		return false;
	}
	CloseClipboard();
	return true;
}

bool SendUnicodeInput(const std::wstring& value)
{
	std::vector<INPUT> inputs;
	inputs.reserve(value.size() * 2);
	for (wchar_t character : value) {
		INPUT down{};
		down.type = INPUT_KEYBOARD;
		down.ki.wVk = 0;
		down.ki.wScan = character;
		down.ki.dwFlags = KEYEVENTF_UNICODE;
		inputs.push_back(down);
		INPUT up = down;
		up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
		inputs.push_back(up);
	}
	return !inputs.empty() && SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT)) == inputs.size();
}

void PrintResult(const TestResult& result)
{
	std::wcout << L"[" << result.name << L"] SendMessage result=" << result.result << L"\n" << std::flush;
}

bool ResolveTarget(HWND* owner, HWND* edit)
{
	if (!owner || !edit) return false;
	*edit = FindExitEdit();
	*owner = *edit ? GetAncestor(*edit, GA_ROOT) : nullptr;
	if (!*edit || !*owner) {
		std::wcerr << L"未找到 DzjsTrainer 确认退出窗口（class=" << kWindowClass << L"）。\n";
		std::wcerr << L"请先从托盘或关于软件页打开退出确认窗口。\n";
		return false;
	}
	DWORD processId = 0;
	GetWindowThreadProcessId(*owner, &processId);
	std::wcout << L"目标窗口 HWND=" << *owner << L"，编辑框 HWND=" << *edit
		<< L"，进程 PID=" << processId << L"\n";
	return true;
}

void SelectAll(HWND edit)
{
	SendMessageW(edit, EM_SETSEL, 0, -1);
}

void RunMessageTest(int choice, const std::wstring& probe)
{
	HWND owner = nullptr;
	HWND edit = nullptr;
	if (!ResolveTarget(&owner, &edit)) return;
	TestResult result{};
	switch (choice) {
	case 1:
		result = { L"WM_SETTEXT", SendMessageW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(probe.c_str())) };
		PrintResult(result);
		break;
	case 2:
		result.name = L"WM_CHAR";
		result.result = 0;
		SelectAll(edit);
		for (wchar_t character : probe)
			result.result = SendMessageW(edit, WM_CHAR, static_cast<WPARAM>(character), 1);
		PrintResult(result);
		break;
	case 3:
		result.name = L"WM_PASTE";
		if (PutClipboardText(probe)) {
			SelectAll(edit);
			result.result = SendMessageW(edit, WM_PASTE, 0, 0);
		}
		else result.result = -1;
		PrintResult(result);
		break;
	case 4:
		result.name = L"EM_REPLACESEL";
		SelectAll(edit);
		result.result = SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(probe.c_str()));
		PrintResult(result);
		break;
	case 5:
		result.name = L"PostMessage WM_KEYDOWN";
		result.result = PostMessageW(edit, WM_KEYDOWN, static_cast<WPARAM>(probe.front()), 1) ? 1 : 0;
		PrintResult(result);
		break;
	case 6: {
		SetForegroundWindow(owner);
		SetFocus(edit);
		const bool sent = SendUnicodeInput(probe);
		std::wcout << L"[SendInput] 注入字符串长度=" << probe.size() << L"，结果="
			<< (sent ? L"已发送" : L"发送失败") << L"\n";
		break;
	}
	default:
		break;
	}
}

void PrintMenu()
{
	std::wcout << L"\n=== DzjsTrainer 输入验证测试 ===\n"
		<< L"1. WM_SETTEXT\n"
		<< L"2. WM_CHAR\n"
		<< L"3. WM_PASTE（剪贴板）\n"
		<< L"4. EM_REPLACESEL\n"
		<< L"5. PostMessage WM_KEYDOWN\n"
		<< L"6. SendInput（运行时输入字符串）\n"
		<< L"7. 全部消息测试（不含 SendInput）\n"
		<< L"0. 退出测试程序\n"
		<< L"请选择：" << std::flush;
}

int RunInteractive()
{
	const std::wstring probe = L"123456";
	std::wcout << L"测试程序不会点击确认退出按钮，也不会结束 DzjsTrainer。\n"
		<< L"请先打开 DzjsTrainer 的确认退出窗口。\n" << std::flush;
	for (;;) {
		PrintMenu();
		std::wstring command;
		if (!std::getline(std::wcin, command)) return 0;
		if (command == L"0" || command == L"q" || command == L"Q") return 0;
		if (command == L"1" || command == L"2" || command == L"3" || command == L"4" || command == L"5") {
			RunMessageTest(command[0] - L'0', probe);
			continue;
		}
		if (command == L"6") {
			std::wcout << L"请输入要通过 SendInput 注入的字符串（直接回车取消）：";
			std::wstring value;
			if (std::getline(std::wcin, value) && !value.empty()) RunMessageTest(6, value);
			continue;
		}
		if (command == L"7") {
			for (int choice = 1; choice <= 5; ++choice) RunMessageTest(choice, probe);
			continue;
		}
		std::wcout << L"无效选项，请输入 0 到 7。\n";
	}
}
}

int wmain()
{
	SetConsoleOutputCP(936);
	SetConsoleCP(936);
	try {
		std::locale::global(std::locale(""));
		std::wcout.imbue(std::locale(""));
		std::wcerr.imbue(std::locale(""));
	}
	catch (const std::exception&) {
		// Keep the console usable even when the host has no named locale.
	}
	std::wcout.clear();
	std::wcerr.clear();
	std::wcout.setf(std::ios::unitbuf);
	return RunInteractive();
}
