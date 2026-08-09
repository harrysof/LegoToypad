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
	constexpr uint8_t kRemoveCommand = 0x02;
	constexpr uint8_t kMoveCommand = 0x03;
	constexpr UINT_PTR kControllerTimer = 1;
	// NOTE: this string is a cross-repo contract with the Cemu fork's
	// Controller.cpp, which opens this same named event and waits on it to
	// know when to neutralize real controller input. It must match the
	// kToypadPickerInputEvent constant in Cemu's src/input/api/Controller.cpp
	// exactly (currently "Local\CemuToypadPickerInputActive") or the handoff
	// will silently stop working.
	constexpr wchar_t kLegoToypadInputEvent[] = L"Local\\CemuToypadPickerInputActive";

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
		{1, 1, L"Left"},
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
		PadViewer,
		PadAction,
		Settings,
	};

	// The 3-option dialog shown after picking a pad in PadViewer.
	enum class PadActionKind
	{
		Load,
		Clear,
		Move,
	};
	constexpr size_t kPadActionCount = 3;

	constexpr size_t kSettingsItemCount = 3; // 0: change shortcut, 1: rescan figures, 2: reset pad view

	enum class ShortcutType
	{
		Controller,
		Keyboard,
	};

	// LegoToypad's own record of what it has sent to each of the 7 pad
	// slots. This is bookkeeping only, not verified truth - the wire
	// protocol is fire-and-forget (no acknowledgement from Cemu), so this
	// can drift out of sync if Cemu's native dialog is also used, if Cemu
	// restarts without LegoToypad restarting, or if a send silently fails
	// to actually land. See "Reset pad view" in Settings.
	struct PadSlot
	{
		bool occupied = false;
		std::wstring figureName;
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

		std::array<PadSlot, 7> padState{};
		size_t padActionIndex = 0;
		// True while PadViewer is being shown specifically to pick a Move's
		// destination pad, rather than the normal "pick a pad to act on"
		// mode. Reuses the same screen/grid per the original design intent.
		bool selectingMoveDestination = false;
		size_t moveSourceSlotIndex = 0;
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

	// Shared connect-send-close for LOAD/REMOVE/MOVE. On failure, errorOut is
	// set to a message suitable for g_app.status and false is returned.
	bool SendToypadMessage(const uint8_t* data, size_t length, std::wstring& errorOut)
	{
		SOCKET clientSocket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (clientSocket == INVALID_SOCKET)
		{
			errorOut = L"Could not create a TCP socket.";
			return false;
		}

		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_port = htons(g_app.port);
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (connect(clientSocket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
		{
			closesocket(clientSocket);
			errorOut = L"Could not connect to Cemu. Enable the emulated Toypad and listener first.";
			return false;
		}

		const bool sent = SendAll(clientSocket, data, length);
		closesocket(clientSocket);
		if (!sent)
		{
			errorOut = L"Connection to Cemu closed before the message was fully sent.";
			return false;
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

		std::wstring error;
		if (!SendToypadMessage(message.data(), message.size(), error))
		{
			g_app.status = error;
			return;
		}

		// The listener always clears the destination before loading (see
		// LISTENER_IMPLEMENTATION.md), so an occupied pad is silently
		// overwritten on the Cemu side too - session state just mirrors that.
		auto& slot = g_app.padState[g_app.slotIndex];
		slot.occupied = true;
		slot.figureName = g_app.figures[g_app.figureIndex].label;

		g_app.status = L"LOAD sent: " + g_app.figures[g_app.figureIndex].label + L" -> " + kSlots[g_app.slotIndex].label;
		g_app.screen = Screen::FigureList;
	}

	void ClearSelectedPad()
	{
		std::array<uint8_t, 5> message{}; // bytes 3-4 stay zero: reserved for LOAD/REMOVE
		message[0] = kRemoveCommand;
		message[1] = kSlots[g_app.slotIndex].pad;
		message[2] = kSlots[g_app.slotIndex].index;

		std::wstring error;
		if (!SendToypadMessage(message.data(), message.size(), error))
		{
			g_app.status = error;
			return;
		}

		g_app.padState[g_app.slotIndex] = PadSlot{};
		g_app.status = std::wstring(L"CLEAR sent: ") + kSlots[g_app.slotIndex].label;
		g_app.screen = Screen::FigureList;
	}

	// Moves whatever is tracked at moveSourceSlotIndex to destIndex. Called
	// once the user has picked a destination pad in the PadViewer's
	// move-destination mode.
	void MoveToDestination(size_t destIndex)
	{
		if (destIndex == g_app.moveSourceSlotIndex)
		{
			// Same pad as source: nothing to send, and updating padState
			// below would otherwise clear the slot (occupy-destination and
			// clear-source would target the same array entry). Stay on this
			// screen so the user can pick a different pad or back out.
			g_app.status = L"That is already where it is. Pick a different pad, or B/Esc to cancel.";
			return;
		}

		std::array<uint8_t, 5> message{};
		message[0] = kMoveCommand;
		message[1] = kSlots[destIndex].pad;
		message[2] = kSlots[destIndex].index;
		message[3] = kSlots[g_app.moveSourceSlotIndex].pad;
		message[4] = kSlots[g_app.moveSourceSlotIndex].index;

		std::wstring error;
		if (!SendToypadMessage(message.data(), message.size(), error))
		{
			g_app.status = error;
			return;
		}

		// Overwrite the destination if it was occupied, same as Load does,
		// and clear the source now that it has moved away.
		const std::wstring movedName = g_app.padState[g_app.moveSourceSlotIndex].figureName;
		g_app.padState[destIndex].occupied = true;
		g_app.padState[destIndex].figureName = movedName;
		g_app.padState[g_app.moveSourceSlotIndex] = PadSlot{};

		g_app.status = L"MOVE sent: " + movedName + L" (" + kSlots[g_app.moveSourceSlotIndex].label +
			L" -> " + kSlots[destIndex].label + L")";
		g_app.selectingMoveDestination = false;
		g_app.screen = Screen::FigureList;
	}

	// Clears LegoToypad's own record of what is loaded where. Does not talk
	// to Cemu at all - see the PadSlot comment for why this can drift and
	// this exists as a manual "start tracking from scratch" reset.
	void ResetPadView()
	{
		g_app.padState = {};
		g_app.status = L"Pad view reset. This only clears LegoToypad's own tracking - nothing was sent to Cemu.";
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

	// Directional (not flat-list) navigation for the pad viewer's 1/3/3
	// grid: 1 slot on the left, a 3-slot cluster in the center (one upper,
	// two lower side by side), the same 3-slot cluster shape on the right.
	// Matches the real Toypad geometry in kSlots/TOYPAD_TECHNICAL.md, not a
	// generic row/column grid, since the shape is irregular. -1 in any
	// direction means "nothing that way, stay put".
	struct PadNeighbors { int up, down, left, right; };
	constexpr std::array<PadNeighbors, 7> kPadNeighbors = {{
		/* 0 Center - upper       */ {-1,  3,  1,  2},
		/* 1 Left                 */ {-1, -1, -1,  0},
		/* 2 Right - upper        */ {-1,  5,  0, -1},
		/* 3 Center - lower left  */ { 0, -1,  1,  4},
		/* 4 Center - lower right */ { 0, -1,  3,  2},
		/* 5 Right - lower left   */ { 2, -1,  4,  6},
		/* 6 Right - lower right  */ { 2, -1,  5, -1},
	}};

	void NavigatePadGrid(int dx, int dy)
	{
		const PadNeighbors& neighbors = kPadNeighbors[g_app.slotIndex];
		int target = -1;
		if (dy < 0) target = neighbors.up;
		else if (dy > 0) target = neighbors.down;
		else if (dx < 0) target = neighbors.left;
		else if (dx > 0) target = neighbors.right;

		if (target >= 0)
			g_app.slotIndex = static_cast<size_t>(target);
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
		case Screen::PadViewer:
			// Up/down through the generic Navigate() entry point (keyboard
			// arrows, controller D-pad/stick up-down); left/right go through
			// NavigatePadGrid directly from the input handlers instead, since
			// this function only carries a single +-1 direction.
			NavigatePadGrid(0, direction);
			break;
		case Screen::PadAction:
			if (direction < 0)
				SelectPrevious(kPadActionCount, g_app.padActionIndex);
			else
				SelectNext(kPadActionCount, g_app.padActionIndex);
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

	// Defined later in this file; declared here because ShowOverlay/HideOverlay
	// assert and release input ownership synchronously around showing/hiding.
	void UpdateInputOwnership(HWND window);

	void ShowOverlay(HWND window)
	{
		if (g_app.overlayVisible)
			return;

		const HWND currentForeground = GetForegroundWindow();
		if (currentForeground && currentForeground != window)
			g_app.previousForegroundWindow = currentForeground;

		PositionOverlayWindow(window);
		// Assert input ownership before the window becomes visible so there
		// is no tick where Cemu can read the controller while the picker is
		// showing (the 16ms poll alone was too slow for that handoff).
		g_app.overlayVisible = true;
		UpdateInputOwnership(window);
		ShowWindow(window, SW_SHOW);
		ForceForegroundWindow(window);
		InvalidateRect(window, nullptr, FALSE);
	}

	void HideOverlay(HWND window)
	{
		if (!g_app.overlayVisible)
			return;

		ShowWindow(window, SW_HIDE);
		g_app.overlayVisible = false;
		g_app.capturingShortcut = false;
		// Release ownership only once the overlay is hidden, so the very
		// button press that closed it never reaches the game either.
		UpdateInputOwnership(window);

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
			{
				g_app.screen = Screen::PadViewer;
				g_app.selectingMoveDestination = false;
			}
			break;
		case Screen::PadViewer:
			if (g_app.selectingMoveDestination)
				MoveToDestination(g_app.slotIndex);
			else
			{
				g_app.screen = Screen::PadAction;
				g_app.padActionIndex = 0; // default to Load
			}
			break;
		case Screen::PadAction:
			switch (static_cast<PadActionKind>(g_app.padActionIndex))
			{
			case PadActionKind::Load:
				LoadSelectedFigure();
				break;
			case PadActionKind::Clear:
				ClearSelectedPad();
				break;
			case PadActionKind::Move:
				g_app.moveSourceSlotIndex = g_app.slotIndex;
				g_app.selectingMoveDestination = true;
				g_app.screen = Screen::PadViewer;
				break;
			}
			break;
		case Screen::Settings:
			if (g_app.settingsIndex == 0)
				BeginShortcutCapture();
			else if (g_app.settingsIndex == 1)
				ScanFigures();
			else if (g_app.settingsIndex == 2)
				ResetPadView();
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
		case Screen::PadViewer:
			if (g_app.selectingMoveDestination)
			{
				// Cancel picking a destination and return to the action
				// dialog for the original pad, still on Move, so the user
				// can retry or pick something else instead.
				g_app.selectingMoveDestination = false;
				g_app.slotIndex = g_app.moveSourceSlotIndex;
				g_app.screen = Screen::PadAction;
				g_app.padActionIndex = static_cast<size_t>(PadActionKind::Move);
			}
			else
			{
				g_app.screen = Screen::FigureList;
			}
			break;
		case Screen::PadAction:
			g_app.screen = Screen::PadViewer;
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

		// Ownership is keyed to the overlay being visible, not to it holding
		// foreground focus. Requiring focus was how input leaked to Cemu:
		// SetForegroundWindow can be refused from a background process (games
		// frequently re-grab focus), so the event silently stayed clear and
		// Cemu kept reading the pad. There is never a reason to let the game
		// receive input while the picker is up, so visibility is the right
		// condition.
		const bool ownsInput = g_app.overlayVisible;
		if (ownsInput)
			SetEvent(g_inputOwnershipEvent);
		else
			ResetEvent(g_inputOwnershipEvent);
	}

	// Cell rectangles for the 7 pad slots, matching the real 1/3/3 (left /
	// center / right) Toypad geometry in kSlots - not the 3/1/3 grouping
	// from the original reference image, which doesn't match how Cemu
	// actually maps pads. Sized for the fixed kOverlayWidth x kOverlayHeight
	// window (this app isn't resizable).
	constexpr std::array<RECT, 7> kPadCells = {{
		{330, 130, 570, 260}, // 0 Center - upper
		{ 60, 220, 230, 350}, // 1 Left
		{630, 130, 860, 260}, // 2 Right - upper
		{330, 280, 445, 410}, // 3 Center - lower left
		{455, 280, 570, 410}, // 4 Center - lower right
		{630, 280, 745, 410}, // 5 Right - lower left
		{745, 280, 860, 410}, // 6 Right - lower right
	}};

	void DrawPadGrid(HDC dc)
	{
		for (size_t i = 0; i < kPadCells.size(); ++i)
		{
			const RECT& cell = kPadCells[i];
			const bool selected = i == g_app.slotIndex;
			const bool occupied = g_app.padState[i].occupied;
			// While picking a Move destination, the pad being moved FROM is
			// shown in its own color so it reads as "the source", not just
			// another occupied cell you might overwrite by mistake.
			const bool isMoveSource = g_app.selectingMoveDestination && i == g_app.moveSourceSlotIndex;

			const COLORREF fill = occupied ? RGB(40, 66, 48) : RGB(32, 37, 48);
			COLORREF border = selected ? RGB(255, 204, 51) : (occupied ? RGB(88, 168, 108) : RGB(70, 76, 90));
			if (isMoveSource)
				border = RGB(90, 150, 230);

			HBRUSH fillBrush = CreateSolidBrush(fill);
			HPEN borderPen = CreatePen(PS_SOLID, selected ? 3 : 2, border);
			HGDIOBJ oldBrush = SelectObject(dc, fillBrush);
			HGDIOBJ oldPen = SelectObject(dc, borderPen);
			RoundRect(dc, cell.left, cell.top, cell.right, cell.bottom, 14, 14);
			SelectObject(dc, oldBrush);
			SelectObject(dc, oldPen);
			DeleteObject(fillBrush);
			DeleteObject(borderPen);

			const int cellWidth = cell.right - cell.left;
			DrawTextLine(dc, kSlots[i].label, cell.left + 10, cell.top + 6, cellWidth - 20, RGB(170, 178, 190), 20);
			const int nameY = cell.top + 30;
			const int nameHeight = (cell.bottom - cell.top) - 36;
			DrawTextLine(dc, occupied ? g_app.padState[i].figureName : L"(empty)",
				cell.left + 10, nameY, cellWidth - 20,
				occupied ? RGB(226, 240, 230) : RGB(110, 116, 128), nameHeight);
		}
	}

	void Paint(HWND window)
	{
		PAINTSTRUCT paint{};
		HDC windowDC = BeginPaint(window, &paint);
		RECT client{};
		GetClientRect(window, &client);
		const int width = client.right - client.left;
		const int height = client.bottom - client.top;

		// Everything below is drawn into an off-screen buffer first, then
		// presented in one BitBlt at the end. Drawing the background fill
		// and then content directly on the window's own surface (the
		// original approach) is visibly flickery on a layered window, since
		// DWM can composite a half-drawn frame; building the whole frame
		// off-screen and blitting it atomically avoids that. The local name
		// `dc` is kept for the memory DC so none of the drawing calls below
		// need to change - only this setup/teardown differs from before.
		HDC dc = CreateCompatibleDC(windowDC);
		HBITMAP bitmap = CreateCompatibleBitmap(windowDC, width, height);
		HGDIOBJ oldBitmap = SelectObject(dc, bitmap);

		HBRUSH background = CreateSolidBrush(RGB(20, 24, 33));
		FillRect(dc, &client, background);
		DeleteObject(background);
		SetBkMode(dc, TRANSPARENT);

		HFONT font = CreateFontW(-22, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
			OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
		HFONT titleFont = CreateFontW(-30, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
			OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
		SelectObject(dc, font);

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
		else if (g_app.screen == Screen::PadViewer)
		{
			if (g_app.selectingMoveDestination)
			{
				DrawTextLine(dc, L"Choose where to move it  |  D-pad/stick: move  |  A/Enter: confirm  |  B/Esc: cancel",
					24, 64, width - 48, RGB(224, 230, 237));
				DrawTextLine(dc, L"Moving " + g_app.padState[g_app.moveSourceSlotIndex].figureName +
					L" from " + kSlots[g_app.moveSourceSlotIndex].label, 24, 96, width - 48, RGB(255, 204, 51));
			}
			else
			{
				DrawTextLine(dc, L"Choose a Toypad position  |  D-pad/stick: move  |  A/Enter: select  |  B/Esc: back",
					24, 64, width - 48, RGB(224, 230, 237));
				DrawTextLine(dc, g_app.figures[g_app.figureIndex].label, 24, 96, width - 48, RGB(255, 204, 51));
			}
			DrawPadGrid(dc);
		}
		else if (g_app.screen == Screen::PadAction)
		{
			const PadSlot& target = g_app.padState[g_app.slotIndex];
			DrawTextLine(dc, L"What do you want to do?  |  D-pad/stick: move  |  A/Enter: confirm  |  B/Esc: back",
				24, 64, width - 48, RGB(224, 230, 237));
			DrawTextLine(dc, g_app.figures[g_app.figureIndex].label + L"  ->  " + kSlots[g_app.slotIndex].label,
				24, 96, width - 48, RGB(255, 204, 51));

			const std::array<std::wstring, kPadActionCount> options = {
				L"Load " + g_app.figures[g_app.figureIndex].label + L" here" +
					(target.occupied ? (L" (overwrites " + target.figureName + L")") : L""),
				target.occupied ? (L"Clear this pad (removes " + target.figureName + L")")
								 : L"Clear this pad (already empty)",
				target.occupied ? (L"Move " + target.figureName + L" from here to another pad")
								 : L"Move (this pad is empty, nothing to move)",
			};
			for (size_t index = 0; index < options.size(); ++index)
			{
				const int y = 150 + static_cast<int>(index) * 44;
				const bool selected = index == g_app.padActionIndex;
				if (selected)
				{
					RECT selection{18, y - 2, width - 18, y + 36};
					HBRUSH brush = CreateSolidBrush(RGB(36, 99, 170));
					FillRect(dc, &selection, brush);
					DeleteObject(brush);
				}
				DrawTextLine(dc, options[index], 30, y, width - 60, selected ? RGB(255, 255, 255) : RGB(228, 232, 238), 34);
			}
		}
		else // Screen::Settings
		{
			DrawTextLine(dc, L"Settings  |  B or Esc: back  |  A or Enter: change", 24, 64, width - 48, RGB(224, 230, 237));

			const std::array<std::wstring, kSettingsItemCount> rows = {
				L"Toggle shortcut: " + DescribeShortcut(),
				L"Rescan figures",
				L"Reset pad view (local tracking only, does not affect Cemu)",
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

		BitBlt(windowDC, 0, 0, width, height, dc, 0, 0, SRCCOPY);

		SelectObject(dc, oldBitmap);
		DeleteObject(bitmap);
		DeleteDC(dc);
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
		bool stickLeft = false;
		bool stickRight = false;

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
				if ((buttons & XINPUT_GAMEPAD_DPAD_LEFT) || state.Gamepad.sThumbLX < -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE)
					stickLeft = true;
				if ((buttons & XINPUT_GAMEPAD_DPAD_RIGHT) || state.Gamepad.sThumbLX > XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE)
					stickRight = true;
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

		// Track whether anything actually changed this tick so the repaint
		// below only fires when needed, instead of unconditionally on every
		// ~16ms timer tick regardless of input. The old unconditional
		// InvalidateRect here was a real contributor to the overlay's
		// flickering: since Paint() runs a fresh BeginPaint/EndPaint (now
		// also a fresh off-screen buffer, see Paint()) each time, invoking
		// it ~60 times a second while sitting idle multiplied how often a
		// partially-composited frame could be exposed.
		bool changed = false;

		if (combinedPressed & XINPUT_GAMEPAD_A)
		{
			Confirm();
			changed = true;
		}
		if (combinedPressed & XINPUT_GAMEPAD_B)
		{
			Back(window);
			changed = true;
		}
		if ((combinedPressed & XINPUT_GAMEPAD_Y) && g_app.screen == Screen::FigureList)
		{
			g_app.screen = Screen::Settings;
			changed = true;
		}

		const DWORD now = GetTickCount();
		const bool anyDirection = stickUp || stickDown || stickLeft || stickRight;
		if (anyDirection && now - lastNavigation >= 180)
		{
			if (g_app.screen == Screen::PadViewer)
			{
				const int dx = stickLeft ? -1 : (stickRight ? 1 : 0);
				const int dy = stickUp ? -1 : (stickDown ? 1 : 0);
				NavigatePadGrid(dx, dy);
			}
			else if (stickUp || stickDown)
			{
				Navigate(stickUp ? -1 : 1);
			}
			lastNavigation = now;
			changed = true;
		}
		if (!anyDirection)
			lastNavigation = 0;

		if (changed)
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
			case VK_LEFT:
				if (g_app.screen == Screen::PadViewer)
					NavigatePadGrid(-1, 0);
				break;
			case VK_RIGHT:
				if (g_app.screen == Screen::PadViewer)
					NavigatePadGrid(1, 0);
				break;
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
