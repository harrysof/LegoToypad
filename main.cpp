#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <Xinput.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "xinput9_1_0.lib")
#pragma comment(lib, "shell32.lib")

namespace
{
	constexpr size_t kTagSize = 180;
	constexpr uint8_t kLoadCommand = 0x01;
	constexpr UINT_PTR kControllerTimer = 1;
	// NOTE: this string is a cross-repo contract with the Cemu fork's
	// Controller.cpp, which waits on the same named event to know when to
	// neutralize real controller input. If you change this string, update
	// Controller.cpp to match or the handoff will silently stop working.
	constexpr wchar_t kLegoToypadInputEvent[] = L"Local\\CemuLegoToypadInputActive";

	// Overlay window sizing.
	constexpr int kOverlayWidth = 900;
	constexpr int kOverlayHeight = 610;
	constexpr BYTE kOverlayAlpha = 235; // 0-255, uniform translucency for the whole panel.

	// Tray icon / menu.
	constexpr UINT kTrayCallbackMessage = WM_APP + 1;
	constexpr UINT kTrayIconId = 1;
	constexpr UINT kMenuIdToggle = 1001;
	constexpr UINT kMenuIdSettings = 1002;
	constexpr UINT kMenuIdExit = 1003;

	// Global toggle hotkey id (used only when the shortcut type is Keyboard).
	constexpr UINT kToggleHotkeyId = 1;

	struct Figure
	{
		std::filesystem::path path;
		std::wstring label;
	};

	struct ToypadSlot
	{
		uint8_t pad;
		uint8_t index;
		const wchar_t* label;
	};

	constexpr std::array<ToypadSlot, 7> kSlots = {{
		{2, 0, L"Center"},
		{1, 1, L"Left - upper"},
		{3, 2, L"Right - upper"},
		{2, 3, L"Center - lower left"},
		{2, 4, L"Center - lower right"},
		{3, 5, L"Right - lower left"},
		{3, 6, L"Right - lower right"},
	}};

	struct ButtonName
	{
		WORD mask;
		const wchar_t* name;
	};

	constexpr std::array<ButtonName, 14> kButtonNames = {{
		{XINPUT_GAMEPAD_DPAD_UP, L"D-Pad Up"},
		{XINPUT_GAMEPAD_DPAD_DOWN, L"D-Pad Down"},
		{XINPUT_GAMEPAD_DPAD_LEFT, L"D-Pad Left"},
		{XINPUT_GAMEPAD_DPAD_RIGHT, L"D-Pad Right"},
		{XINPUT_GAMEPAD_START, L"Start"},
		{XINPUT_GAMEPAD_BACK, L"Back"},
		{XINPUT_GAMEPAD_LEFT_THUMB, L"Left Stick Click"},
		{XINPUT_GAMEPAD_RIGHT_THUMB, L"Right Stick Click"},
		{XINPUT_GAMEPAD_LEFT_SHOULDER, L"LB"},
		{XINPUT_GAMEPAD_RIGHT_SHOULDER, L"RB"},
		{XINPUT_GAMEPAD_A, L"A"},
		{XINPUT_GAMEPAD_B, L"B"},
		{XINPUT_GAMEPAD_X, L"X"},
		{XINPUT_GAMEPAD_Y, L"Y"},
	}};

	// Buttons already bound to in-app menu navigation (confirm, back, jump
	// to settings, list scrolling). A captured shortcut made up of only
	// these would immediately fight with normal use of the picker, so at
	// least one other button is always required alongside them.
	constexpr WORD kNavigationButtons = XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_Y |
		XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN;

	// Reserved chord that always cancels shortcut capture on a controller,
	// regardless of what is being assigned. Keyboard capture is cancelled
	// with Esc instead. This guarantees there is always a way to back out
	// without a mouse or keyboard.
	constexpr WORD kShortcutCancelChord = XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START;

	enum class Screen
	{
		FigureList,
		SlotList,
		Settings,
	};

	constexpr size_t kSettingsItemCount = 2; // 0: change shortcut, 1: rescan figures

	enum class ShortcutType
	{
		Controller,
		Keyboard,
	};

	struct AppState
	{
		std::vector<Figure> figures;
		std::filesystem::path binRoot;
		Screen screen = Screen::FigureList;
		size_t figureIndex = 0;
		size_t slotIndex = 0;
		size_t settingsIndex = 0;
		uint16_t port = 9191;
		std::wstring status;

		bool overlayVisible = false;
		HWND previousForegroundWindow = nullptr;

		ShortcutType shortcutType = ShortcutType::Controller;
		WORD shortcutControllerMask = XINPUT_GAMEPAD_BACK;
		UINT shortcutKeyModifiers = 0;
		UINT shortcutKeyCode = 0;
		bool capturingShortcut = false;
		// Stays false until every controller button has been seen released
		// at least once after entering capture mode, so whatever button was
		// still held down from opening the Settings screen can't be
		// mistaken for the start of the new shortcut.
		bool shortcutCaptureArmed = false;
	};

	AppState g_app;
	HANDLE g_inputOwnershipEvent = nullptr;

	std::filesystem::path GetExecutableDirectory()
	{
		std::array<wchar_t, 32768> path{};
		const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
		return length == 0 ? std::filesystem::current_path() : std::filesystem::path(path.data()).parent_path();
	}

	std::filesystem::path FindBinRoot()
	{
		std::filesystem::path directory = GetExecutableDirectory();
		for (int i = 0; i != 10; ++i)
		{
			const auto candidate = directory / L"Lego Dimensions Organized bins";
			if (std::filesystem::is_directory(candidate))
				return candidate;
			if (!directory.has_parent_path())
				break;
			directory = directory.parent_path();
		}
		return {};
	}

	uint16_t ReadPort()
	{
		const auto iniPath = GetExecutableDirectory() / L"LegoToypad.ini";
		const UINT configuredPort = GetPrivateProfileIntW(L"Listener", L"Port", 9191, iniPath.c_str());
		return configuredPort >= 1 && configuredPort <= 65535 ? static_cast<uint16_t>(configuredPort) : 9191;
	}

	// ---------------------------------------------------------------------
	// Shortcut description / persistence
	// ---------------------------------------------------------------------

	std::wstring DescribeControllerMask(WORD mask)
	{
		if (mask == 0)
			return L"(none)";
		std::wstring result;
		for (const auto& entry : kButtonNames)
		{
			if (mask & entry.mask)
			{
				if (!result.empty())
					result += L" + ";
				result += entry.name;
			}
		}
		return result.empty() ? L"(unrecognized)" : result;
	}

	std::wstring DescribeKeyboardKey(UINT modifiers, UINT vk)
	{
		std::wstring result;
		if (modifiers & MOD_CONTROL)
			result += L"Ctrl + ";
		if (modifiers & MOD_ALT)
			result += L"Alt + ";
		if (modifiers & MOD_SHIFT)
			result += L"Shift + ";
		if (modifiers & MOD_WIN)
			result += L"Win + ";

		UINT scanCode = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
		LONG lParamValue = static_cast<LONG>(scanCode << 16);
		switch (vk)
		{
		case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
		case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
		case VK_INSERT: case VK_DELETE: case VK_RCONTROL: case VK_RMENU:
			lParamValue |= (1 << 24);
			break;
		default:
			break;
		}

		std::array<wchar_t, 64> buffer{};
		if (vk != 0 && GetKeyNameTextW(lParamValue, buffer.data(), static_cast<int>(buffer.size())) > 0)
			result += buffer.data();
		else
			result += L"(none)";
		return result;
	}

	std::wstring DescribeShortcut()
	{
		return g_app.shortcutType == ShortcutType::Controller
			? L"Controller: " + DescribeControllerMask(g_app.shortcutControllerMask)
			: L"Keyboard: " + DescribeKeyboardKey(g_app.shortcutKeyModifiers, g_app.shortcutKeyCode);
	}

	void SaveShortcutToIni()
	{
		const auto iniPath = GetExecutableDirectory() / L"LegoToypad.ini";
		WritePrivateProfileStringW(L"Shortcut", L"Type",
			g_app.shortcutType == ShortcutType::Controller ? L"Controller" : L"Keyboard", iniPath.c_str());
		WritePrivateProfileStringW(L"Shortcut", L"ControllerMask",
			std::to_wstring(g_app.shortcutControllerMask).c_str(), iniPath.c_str());
		WritePrivateProfileStringW(L"Shortcut", L"KeyModifiers",
			std::to_wstring(g_app.shortcutKeyModifiers).c_str(), iniPath.c_str());
		WritePrivateProfileStringW(L"Shortcut", L"KeyCode",
			std::to_wstring(g_app.shortcutKeyCode).c_str(), iniPath.c_str());
	}

	void LoadShortcutFromIni()
	{
		const auto iniPath = GetExecutableDirectory() / L"LegoToypad.ini";
		std::array<wchar_t, 32> typeBuffer{};
		GetPrivateProfileStringW(L"Shortcut", L"Type", L"Controller", typeBuffer.data(),
			static_cast<DWORD>(typeBuffer.size()), iniPath.c_str());
		g_app.shortcutType = _wcsicmp(typeBuffer.data(), L"Keyboard") == 0 ? ShortcutType::Keyboard : ShortcutType::Controller;
		g_app.shortcutControllerMask = static_cast<WORD>(
			GetPrivateProfileIntW(L"Shortcut", L"ControllerMask", XINPUT_GAMEPAD_BACK, iniPath.c_str()));
		g_app.shortcutKeyModifiers = static_cast<UINT>(
			GetPrivateProfileIntW(L"Shortcut", L"KeyModifiers", 0, iniPath.c_str()));
		g_app.shortcutKeyCode = static_cast<UINT>(
			GetPrivateProfileIntW(L"Shortcut", L"KeyCode", 0, iniPath.c_str()));
	}

	// ---------------------------------------------------------------------

	void ScanFigures()
	{
		g_app.figures.clear();
		g_app.binRoot = FindBinRoot();
		if (g_app.binRoot.empty())
		{
			g_app.status = L"Could not find the 'Lego Dimensions Organized bins' folder.";
			return;
		}

		for (const auto& entry : std::filesystem::recursive_directory_iterator(g_app.binRoot))
		{
			if (!entry.is_regular_file() || entry.path().extension() != L".bin")
				continue;
			if (entry.file_size() != kTagSize)
				continue;

			const auto relative = std::filesystem::relative(entry.path(), g_app.binRoot);
			const std::wstring theme = relative.parent_path().wstring();
			g_app.figures.push_back({entry.path(), theme + L"  |  " + entry.path().stem().wstring()});
		}

		std::sort(g_app.figures.begin(), g_app.figures.end(), [](const Figure& left, const Figure& right) {
			return _wcsicmp(left.label.c_str(), right.label.c_str()) < 0;
		});

		if (g_app.figureIndex >= g_app.figures.size())
			g_app.figureIndex = 0;

		g_app.status = (g_app.figures.empty()
			? std::wstring(L"No valid 180-byte .bin files were found.")
			: std::to_wstring(g_app.figures.size()) + L" figures ready. Listener port: " + std::to_wstring(g_app.port))
			+ L" | Toggle: " + DescribeShortcut();
	}

	bool SendAll(SOCKET socket, const uint8_t* data, size_t length)
	{
		while (length != 0)
		{
			const int sent = send(socket, reinterpret_cast<const char*>(data), static_cast<int>(length), 0);
			if (sent == SOCKET_ERROR || sent == 0)
				return false;
			data += sent;
			length -= static_cast<size_t>(sent);
		}
		return true;
	}

	void LoadSelectedFigure()
	{
		if (g_app.figures.empty())
			return;

		std::array<uint8_t, kTagSize> tagData{};
		std::ifstream file(g_app.figures[g_app.figureIndex].path, std::ios::binary);
		if (!file.read(reinterpret_cast<char*>(tagData.data()), tagData.size()) || file.gcount() != tagData.size())
		{
			g_app.status = L"Could not read the selected 180-byte tag file.";
			return;
		}

		std::array<uint8_t, 5 + kTagSize> message{};
		message[0] = kLoadCommand;
		message[1] = kSlots[g_app.slotIndex].pad;
		message[2] = kSlots[g_app.slotIndex].index;
		std::copy(tagData.begin(), tagData.end(), message.begin() + 5);

		SOCKET clientSocket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (clientSocket == INVALID_SOCKET)
		{
			g_app.status = L"Could not create a TCP socket.";
			return;
		}

		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_port = htons(g_app.port);
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (connect(clientSocket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
		{
			closesocket(clientSocket);
			g_app.status = L"Could not connect to Cemu. Enable the emulated Toypad and listener first.";
			return;
		}

		const bool sent = SendAll(clientSocket, message.data(), message.size());
		closesocket(clientSocket);
		if (!sent)
		{
			g_app.status = L"Connection to Cemu closed before the LOAD message was sent.";
			return;
		}

		g_app.status = L"LOAD sent: " + g_app.figures[g_app.figureIndex].label + L" -> " + kSlots[g_app.slotIndex].label;
		g_app.screen = Screen::FigureList;
	}

	void SelectPrevious(size_t count, size_t& index)
	{
		if (count != 0)
			index = index == 0 ? count - 1 : index - 1;
	}

	void SelectNext(size_t count, size_t& index)
	{
		if (count != 0)
			index = (index + 1) % count;
	}

	void Navigate(int direction)
	{
		switch (g_app.screen)
		{
		case Screen::FigureList:
			if (direction < 0)
				SelectPrevious(g_app.figures.size(), g_app.figureIndex);
			else
				SelectNext(g_app.figures.size(), g_app.figureIndex);
			break;
		case Screen::SlotList:
			if (direction < 0)
				SelectPrevious(kSlots.size(), g_app.slotIndex);
			else
				SelectNext(kSlots.size(), g_app.slotIndex);
			break;
		case Screen::Settings:
			if (direction < 0)
				SelectPrevious(kSettingsItemCount, g_app.settingsIndex);
			else
				SelectNext(kSettingsItemCount, g_app.settingsIndex);
			break;
		}
	}

	// ---------------------------------------------------------------------
	// Overlay show / hide
	// ---------------------------------------------------------------------

	// Windows normally refuses SetForegroundWindow() calls that don't originate
	// from a genuine input event (e.g. a call made from our background timer
	// thread while Cemu owns focus). Briefly attaching our input queue to the
	// current foreground thread's queue is the standard workaround.
	void ForceForegroundWindow(HWND target)
	{
		if (!target || !IsWindow(target))
			return;

		const HWND currentForeground = GetForegroundWindow();
		const DWORD foregroundThread = currentForeground ? GetWindowThreadProcessId(currentForeground, nullptr) : 0;
		const DWORD thisThread = GetCurrentThreadId();
		const bool attach = foregroundThread != 0 && foregroundThread != thisThread;

		if (attach)
			AttachThreadInput(thisThread, foregroundThread, TRUE);
		SetForegroundWindow(target);
		SetActiveWindow(target);
		if (attach)
			AttachThreadInput(thisThread, foregroundThread, FALSE);
	}

	void PositionOverlayWindow(HWND window)
	{
		HWND reference = (g_app.previousForegroundWindow && IsWindow(g_app.previousForegroundWindow))
			? g_app.previousForegroundWindow
			: window;
		HMONITOR monitor = MonitorFromWindow(reference, MONITOR_DEFAULTTONEAREST);
		MONITORINFO info{};
		info.cbSize = sizeof(info);
		GetMonitorInfoW(monitor, &info);

		const int monitorWidth = info.rcMonitor.right - info.rcMonitor.left;
		const int monitorHeight = info.rcMonitor.bottom - info.rcMonitor.top;
		const int x = info.rcMonitor.left + (monitorWidth - kOverlayWidth) / 2;
		const int y = info.rcMonitor.top + (monitorHeight - kOverlayHeight) / 2;
		SetWindowPos(window, HWND_TOPMOST, x, y, kOverlayWidth, kOverlayHeight, SWP_NOACTIVATE);
	}

	void ShowOverlay(HWND window)
	{
		if (g_app.overlayVisible)
			return;

		const HWND currentForeground = GetForegroundWindow();
		if (currentForeground && currentForeground != window)
			g_app.previousForegroundWindow = currentForeground;

		PositionOverlayWindow(window);
		ShowWindow(window, SW_SHOW);
		ForceForegroundWindow(window);
		g_app.overlayVisible = true;
		InvalidateRect(window, nullptr, FALSE);
	}

	void HideOverlay(HWND window)
	{
		if (!g_app.overlayVisible)
			return;

		ShowWindow(window, SW_HIDE);
		g_app.overlayVisible = false;
		g_app.capturingShortcut = false;

		if (g_app.previousForegroundWindow && IsWindow(g_app.previousForegroundWindow))
			ForceForegroundWindow(g_app.previousForegroundWindow);
	}

	void ToggleOverlay(HWND window)
	{
		if (g_app.overlayVisible)
			HideOverlay(window);
		else
			ShowOverlay(window);
	}

	// ---------------------------------------------------------------------
	// Shortcut capture
	// ---------------------------------------------------------------------

	void RegisterToggleHotkeyIfNeeded(HWND window)
	{
		UnregisterHotKey(window, kToggleHotkeyId);
		if (g_app.shortcutType != ShortcutType::Keyboard || g_app.shortcutKeyCode == 0)
			return;

		// A modifier-less hotkey registers as a truly global key grab, which
		// would swallow every press of that key in every other program. The
		// in-app capture screen never produces one, but LegoToypad.ini can
		// be hand-edited, so this is checked again here as a safety net.
		if (g_app.shortcutKeyModifiers == 0)
		{
			g_app.status = L"Keyboard shortcut in LegoToypad.ini has no modifier (Ctrl/Alt/Shift/Win), "
				L"so it was not registered. Set it from the in-app Settings screen instead.";
			return;
		}

		if (!RegisterHotKey(window, kToggleHotkeyId, g_app.shortcutKeyModifiers | MOD_NOREPEAT, g_app.shortcutKeyCode))
			g_app.status = L"Warning: could not register that keyboard shortcut (it may be in use elsewhere).";
	}

	void BeginShortcutCapture()
	{
		g_app.capturingShortcut = true;
		g_app.shortcutCaptureArmed = false;
		g_app.status = L"Release all controller buttons, then press a combo, or press a keyboard shortcut.";
	}

	void CancelShortcutCapture()
	{
		g_app.capturingShortcut = false;
		g_app.status = L"Shortcut unchanged: " + DescribeShortcut();
	}

	void ApplyControllerShortcut(HWND window, WORD mask)
	{
		g_app.shortcutType = ShortcutType::Controller;
		g_app.shortcutControllerMask = mask;
		g_app.capturingShortcut = false;
		RegisterToggleHotkeyIfNeeded(window);
		SaveShortcutToIni();
		g_app.status = L"Shortcut set to " + DescribeShortcut();
	}

	void ApplyKeyboardShortcut(HWND window, UINT modifiers, UINT vk)
	{
		g_app.shortcutType = ShortcutType::Keyboard;
		g_app.shortcutKeyModifiers = modifiers;
		g_app.shortcutKeyCode = vk;
		g_app.capturingShortcut = false;
		RegisterToggleHotkeyIfNeeded(window);
		SaveShortcutToIni();
		g_app.status = L"Shortcut set to " + DescribeShortcut();
	}

	void Confirm()
	{
		switch (g_app.screen)
		{
		case Screen::FigureList:
			if (!g_app.figures.empty())
				g_app.screen = Screen::SlotList;
			break;
		case Screen::SlotList:
			LoadSelectedFigure();
			break;
		case Screen::Settings:
			if (g_app.settingsIndex == 0)
				BeginShortcutCapture();
			else if (g_app.settingsIndex == 1)
				ScanFigures();
			break;
		}
	}

	void Back(HWND window)
	{
		if (g_app.capturingShortcut)
		{
			CancelShortcutCapture();
			return;
		}
		switch (g_app.screen)
		{
		case Screen::SlotList:
			g_app.screen = Screen::FigureList;
			break;
		case Screen::Settings:
			g_app.screen = Screen::FigureList;
			break;
		case Screen::FigureList:
			HideOverlay(window);
			break;
		}
	}

	// ---------------------------------------------------------------------
	// Tray icon
	// ---------------------------------------------------------------------

	void AddTrayIcon(HWND window)
	{
		NOTIFYICONDATAW icon{};
		icon.cbSize = sizeof(icon);
		icon.hWnd = window;
		icon.uID = kTrayIconId;
		icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
		icon.uCallbackMessage = kTrayCallbackMessage;
		icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
		const wchar_t* tip = L"LEGO Dimensions Toypad Picker";
		wcsncpy(icon.szTip, tip, std::size(icon.szTip) - 1);
		icon.szTip[std::size(icon.szTip) - 1] = L'\0';
		Shell_NotifyIconW(NIM_ADD, &icon);
	}

	void RemoveTrayIcon(HWND window)
	{
		NOTIFYICONDATAW icon{};
		icon.cbSize = sizeof(icon);
		icon.hWnd = window;
		icon.uID = kTrayIconId;
		Shell_NotifyIconW(NIM_DELETE, &icon);
	}

	void ShowTrayMenu(HWND window)
	{
		POINT cursor{};
		GetCursorPos(&cursor);
		HMENU menu = CreatePopupMenu();
		AppendMenuW(menu, MF_STRING, kMenuIdToggle, g_app.overlayVisible ? L"Hide overlay" : L"Show overlay");
		AppendMenuW(menu, MF_STRING, kMenuIdSettings, L"Shortcut settings");
		AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
		AppendMenuW(menu, MF_STRING, kMenuIdExit, L"Exit");
		// Required so the popup menu dismisses correctly when clicking away.
		ForceForegroundWindow(window);
		TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, window, nullptr);
		DestroyMenu(menu);
	}

	// ---------------------------------------------------------------------
	// Drawing
	// ---------------------------------------------------------------------

	void DrawTextLine(HDC dc, const std::wstring& text, int x, int y, int width, COLORREF color, int height = 30)
	{
		SetTextColor(dc, color);
		RECT rect{x, y, x + width, y + height};
		DrawTextW(dc, text.c_str(), -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
	}

	void UpdateInputOwnership(HWND window)
	{
		if (!g_inputOwnershipEvent)
			return;

		const bool ownsInput = g_app.overlayVisible && GetForegroundWindow() == window && !IsIconic(window);
		if (ownsInput)
			SetEvent(g_inputOwnershipEvent);
		else
			ResetEvent(g_inputOwnershipEvent);
	}

	void Paint(HWND window)
	{
		PAINTSTRUCT paint{};
		HDC dc = BeginPaint(window, &paint);
		RECT client{};
		GetClientRect(window, &client);
		HBRUSH background = CreateSolidBrush(RGB(20, 24, 33));
		FillRect(dc, &client, background);
		DeleteObject(background);
		SetBkMode(dc, TRANSPARENT);

		HFONT font = CreateFontW(-22, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
			OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
		HFONT titleFont = CreateFontW(-30, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
			OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
		SelectObject(dc, font);

		const int width = client.right - client.left;
		SelectObject(dc, titleFont);
		DrawTextLine(dc, L"LEGO Dimensions Toypad Picker", 24, 18, width - 48, RGB(255, 204, 51), 40);
		SelectObject(dc, font);

		if (g_app.screen == Screen::FigureList)
		{
			DrawTextLine(dc, L"Choose a figure  |  D-pad/stick: move  |  A/Enter: select  |  Y/S: settings",
				24, 64, width - 48, RGB(224, 230, 237));
			const size_t visibleRows = 12;
			const size_t first = g_app.figureIndex > visibleRows / 2 ? g_app.figureIndex - visibleRows / 2 : 0;
			for (size_t row = 0; row < visibleRows && first + row < g_app.figures.size(); ++row)
			{
				const size_t index = first + row;
				const int y = 108 + static_cast<int>(row) * 34;
				const bool selected = index == g_app.figureIndex;
				if (selected)
				{
					RECT selection{18, y - 2, width - 18, y + 30};
					HBRUSH brush = CreateSolidBrush(RGB(36, 99, 170));
					FillRect(dc, &selection, brush);
					DeleteObject(brush);
				}
				DrawTextLine(dc, g_app.figures[index].label, 30, y, width - 60, selected ? RGB(255, 255, 255) : RGB(228, 232, 238));
			}
		}
		else if (g_app.screen == Screen::SlotList)
		{
			DrawTextLine(dc, L"Choose a Toypad position  |  B or Esc: back  |  A or Enter: send", 24, 64, width - 48, RGB(224, 230, 237));
			DrawTextLine(dc, g_app.figures[g_app.figureIndex].label, 24, 96, width - 48, RGB(255, 204, 51));
			for (size_t index = 0; index < kSlots.size(); ++index)
			{
				const int y = 146 + static_cast<int>(index) * 40;
				const bool selected = index == g_app.slotIndex;
				if (selected)
				{
					RECT selection{18, y - 2, width - 18, y + 34};
					HBRUSH brush = CreateSolidBrush(RGB(36, 99, 170));
					FillRect(dc, &selection, brush);
					DeleteObject(brush);
				}
				DrawTextLine(dc, L"Slot " + std::to_wstring(kSlots[index].index) + L" (pad " + std::to_wstring(kSlots[index].pad) + L"): " + kSlots[index].label,
					30, y, width - 60, selected ? RGB(255, 255, 255) : RGB(228, 232, 238));
			}
		}
		else // Screen::Settings
		{
			DrawTextLine(dc, L"Settings  |  B or Esc: back  |  A or Enter: change", 24, 64, width - 48, RGB(224, 230, 237));

			const std::array<std::wstring, kSettingsItemCount> rows = {
				L"Toggle shortcut: " + DescribeShortcut(),
				L"Rescan figures",
			};
			for (size_t index = 0; index < rows.size(); ++index)
			{
				const int y = 108 + static_cast<int>(index) * 40;
				const bool selected = index == g_app.settingsIndex;
				if (selected)
				{
					RECT selection{18, y - 2, width - 18, y + 34};
					HBRUSH brush = CreateSolidBrush(RGB(36, 99, 170));
					FillRect(dc, &selection, brush);
					DeleteObject(brush);
				}
				DrawTextLine(dc, rows[index], 30, y, width - 60, selected ? RGB(255, 255, 255) : RGB(228, 232, 238));
			}

			if (g_app.capturingShortcut)
			{
				const int y = 108 + static_cast<int>(rows.size()) * 40 + 16;
				if (!g_app.shortcutCaptureArmed)
				{
					DrawTextLine(dc, L"Release every controller button first...",
						24, y, width - 48, RGB(255, 204, 51), 26);
				}
				else
				{
					DrawTextLine(dc, L"Listening: press a controller combo, or a keyboard shortcut.",
						24, y, width - 48, RGB(255, 204, 51), 26);
				}
				DrawTextLine(dc, L"Controller combo needs a non-nav button. Back+Start cancels. Keyboard needs a modifier. Esc cancels.",
					24, y + 26, width - 48, RGB(255, 204, 51), 26);
			}
		}

		DrawTextLine(dc, g_app.status, 24, client.bottom - 44, width - 48, RGB(142, 238, 171));
		SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
		DeleteObject(font);
		DeleteObject(titleFont);
		EndPaint(window, &paint);
	}

	// ---------------------------------------------------------------------
	// Input polling
	// ---------------------------------------------------------------------

	void PollController(HWND window)
	{
		// One slot per XInput user index (0-3), so a controller plugged in
		// as player 2/3/4 works identically to player 1. Cemu itself may be
		// reading controller 0 for gameplay, so this app deliberately does
		// not assume it owns that slot either.
		static std::array<WORD, XUSER_MAX_COUNT> previousButtons{};
		static WORD previousCombinedButtons = 0;
		static DWORD lastNavigation = 0;

		UpdateInputOwnership(window);

		bool anyConnected = false;
		WORD combinedButtons = 0;
		WORD combinedPressed = 0;
		bool stickUp = false;
		bool stickDown = false;

		for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i)
		{
			XINPUT_STATE state{};
			const bool connected = XInputGetState(i, &state) == ERROR_SUCCESS;
			const WORD buttons = connected ? state.Gamepad.wButtons : 0;
			if (connected)
			{
				anyConnected = true;
				combinedButtons |= buttons;
				combinedPressed |= buttons & ~previousButtons[i];
				if ((buttons & XINPUT_GAMEPAD_DPAD_UP) || state.Gamepad.sThumbLY > XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE)
					stickUp = true;
				if ((buttons & XINPUT_GAMEPAD_DPAD_DOWN) || state.Gamepad.sThumbLY < -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE)
					stickDown = true;
			}
			previousButtons[i] = buttons;
		}
		const WORD previousCombined = previousCombinedButtons;
		previousCombinedButtons = combinedButtons;

		// -------------------------------------------------------------
		// Shortcut capture takes over completely while active.
		//
		// - Capture only "arms" once every controller has been seen fully
		//   released, so the button that opened this screen (still held on
		//   the very first tick) is never mistaken for the start of a new
		//   combo.
		// - Back+Start is a reserved chord that always cancels, so there is
		//   always a way out from a controller alone, no keyboard needed.
		// - A combo made entirely of navigation buttons (A/B/Y/D-pad
		//   up-down) is rejected, since assigning one of those would make
		//   the picker fight itself the moment it was used normally.
		// -------------------------------------------------------------
		if (g_app.capturingShortcut)
		{
			if (!g_app.shortcutCaptureArmed)
			{
				if (combinedButtons == 0)
					g_app.shortcutCaptureArmed = true;
				return;
			}

			if (anyConnected && combinedButtons != 0 && combinedPressed != 0)
			{
				if (combinedButtons == kShortcutCancelChord)
				{
					CancelShortcutCapture();
				}
				else if ((combinedButtons & ~kNavigationButtons) == 0)
				{
					g_app.status = L"That combo is only menu-navigation buttons. Add LB, RB, X, "
						L"Back, Start, or a stick click. Back+Start cancels.";
				}
				else
				{
					ApplyControllerShortcut(window, combinedButtons);
				}
				InvalidateRect(window, nullptr, FALSE);
			}
			return;
		}

		// Global toggle: evaluated every tick regardless of focus/visibility so
		// it also works while Cemu (not this app) owns the foreground window.
		// Any connected controller can trigger it.
		if (g_app.shortcutType == ShortcutType::Controller && g_app.shortcutControllerMask != 0)
		{
			const bool matchNow = anyConnected && (combinedButtons & g_app.shortcutControllerMask) == g_app.shortcutControllerMask;
			const bool matchBefore = (previousCombined & g_app.shortcutControllerMask) == g_app.shortcutControllerMask;
			if (matchNow && !matchBefore)
			{
				ToggleOverlay(window);
				return;
			}
		}

		// Menu navigation only applies while the overlay is actually shown and focused.
		if (!g_app.overlayVisible || GetForegroundWindow() != window || IsIconic(window) || !anyConnected)
			return;

		if (combinedPressed & XINPUT_GAMEPAD_A)
			Confirm();
		if (combinedPressed & XINPUT_GAMEPAD_B)
			Back(window);
		if ((combinedPressed & XINPUT_GAMEPAD_Y) && g_app.screen == Screen::FigureList)
			g_app.screen = Screen::Settings;

		const DWORD now = GetTickCount();
		if ((stickUp || stickDown) && now - lastNavigation >= 180)
		{
			Navigate(stickUp ? -1 : 1);
			lastNavigation = now;
		}
		if (!stickUp && !stickDown)
			lastNavigation = 0;

		InvalidateRect(window, nullptr, FALSE);
	}

	LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
	{
		switch (message)
		{
		case WM_PAINT:
			Paint(window);
			return 0;
		case WM_KEYDOWN:
			if (g_app.capturingShortcut)
			{
				if (wParam == VK_ESCAPE)
				{
					CancelShortcutCapture();
				}
				else if (wParam != VK_SHIFT && wParam != VK_CONTROL && wParam != VK_MENU &&
					wParam != VK_LSHIFT && wParam != VK_RSHIFT && wParam != VK_LCONTROL &&
					wParam != VK_RCONTROL && wParam != VK_LMENU && wParam != VK_RMENU &&
					wParam != VK_LWIN && wParam != VK_RWIN)
				{
					UINT modifiers = 0;
					if (GetKeyState(VK_CONTROL) & 0x8000) modifiers |= MOD_CONTROL;
					if (GetKeyState(VK_MENU) & 0x8000) modifiers |= MOD_ALT;
					if (GetKeyState(VK_SHIFT) & 0x8000) modifiers |= MOD_SHIFT;
					if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000)) modifiers |= MOD_WIN;

					// A shortcut with no modifier registers as a truly global
					// hotkey for that bare key, so it would swallow every
					// press of that key in every other program while this
					// app is running. Require at least one modifier instead
					// of allowing that trap.
					if (modifiers == 0)
					{
						g_app.status = L"Add Ctrl, Alt, Shift, or Win. A bare key would take over "
							L"that key everywhere on your PC while this app is running.";
					}
					else
					{
						ApplyKeyboardShortcut(window, modifiers, static_cast<UINT>(wParam));
					}
				}
				InvalidateRect(window, nullptr, FALSE);
				return 0;
			}
			switch (wParam)
			{
			case VK_UP: Navigate(-1); break;
			case VK_DOWN: Navigate(1); break;
			case VK_RETURN: Confirm(); break;
			case VK_ESCAPE: Back(window); break;
			case 'S':
				if (g_app.screen == Screen::FigureList)
					g_app.screen = Screen::Settings;
				break;
			}
			InvalidateRect(window, nullptr, FALSE);
			return 0;
		case WM_HOTKEY:
			if (wParam == kToggleHotkeyId)
				ToggleOverlay(window);
			return 0;
		case kTrayCallbackMessage:
			if (LOWORD(lParam) == WM_LBUTTONUP || LOWORD(lParam) == WM_LBUTTONDBLCLK)
				ToggleOverlay(window);
			else if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU)
				ShowTrayMenu(window);
			return 0;
		case WM_COMMAND:
			switch (LOWORD(wParam))
			{
			case kMenuIdToggle:
				ToggleOverlay(window);
				break;
			case kMenuIdSettings:
				if (!g_app.overlayVisible)
					ShowOverlay(window);
				g_app.screen = Screen::Settings;
				InvalidateRect(window, nullptr, FALSE);
				break;
			case kMenuIdExit:
				DestroyWindow(window);
				break;
			}
			return 0;
		case WM_TIMER:
			if (wParam == kControllerTimer)
				PollController(window);
			return 0;
		case WM_ACTIVATE:
		case WM_SIZE:
			UpdateInputOwnership(window);
			return 0;
		case WM_DESTROY:
			KillTimer(window, kControllerTimer);
			UnregisterHotKey(window, kToggleHotkeyId);
			RemoveTrayIcon(window);
			if (g_inputOwnershipEvent)
			{
				ResetEvent(g_inputOwnershipEvent);
				CloseHandle(g_inputOwnershipEvent);
				g_inputOwnershipEvent = nullptr;
			}
			PostQuitMessage(0);
			return 0;
		default:
			return DefWindowProcW(window, message, wParam, lParam);
		}
	}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
	SetProcessDPIAware();

	WSADATA wsaData{};
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
		return 1;

	g_app.port = ReadPort();
	LoadShortcutFromIni();
	ScanFigures();
	g_inputOwnershipEvent = CreateEventW(nullptr, TRUE, FALSE, kLegoToypadInputEvent);

	const wchar_t* className = L"LegoToypadWindow";
	WNDCLASSW windowClass{};
	windowClass.hInstance = instance;
	windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	windowClass.lpfnWndProc = WindowProcedure;
	windowClass.lpszClassName = className;
	RegisterClassW(&windowClass);

	// WS_POPUP (no caption/border) + layered/topmost/toolwindow gives us a
	// borderless, translucent, always-on-top panel that stays out of the
	// taskbar and alt-tab list, similar to Xbox Game Bar. It is created
	// hidden and only shown when the configured shortcut is triggered.
	HWND window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
		className, L"LEGO Dimensions Toypad Picker", WS_POPUP,
		CW_USEDEFAULT, CW_USEDEFAULT, kOverlayWidth, kOverlayHeight, nullptr, nullptr, instance, nullptr);
	if (!window)
	{
		WSACleanup();
		return 1;
	}

	SetLayeredWindowAttributes(window, 0, kOverlayAlpha, LWA_ALPHA);
	AddTrayIcon(window);
	RegisterToggleHotkeyIfNeeded(window);
	SetTimer(window, kControllerTimer, 16, nullptr);

	MSG message{};
	while (GetMessageW(&message, nullptr, 0, 0) > 0)
	{
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}

	WSACleanup();
	return static_cast<int>(message.wParam);
}
