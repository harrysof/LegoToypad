#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <Xinput.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <gdiplus.h>
#include <SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "GeneratedAssetTable.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "iphlpapi.lib")

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

	// Overlay window sizing. All assets (portraits, logos, background,
	// wordmark, tag .bins) are compiled into the exe as resources at build
	// time by generate_assets.py - nothing is read from disk at runtime.
constexpr int kOverlayWidth = 900;
	constexpr int kOverlayHeight = 610;
	constexpr BYTE kOverlayAlpha = 235; // 0-255, uniform translucency for the whole panel.
	// Color that turns fully transparent when the "No Background" choice is
	// active (LWA_COLORKEY): the frame is painted onto this color instead of
	// a wallpaper, so everything between the UI elements shows the game.
	// Nearly black, so antialiased edges blending toward it stay clean, but
	// dark enough that no UI element ever produces this exact value itself.
	constexpr COLORREF kNoBackgroundColorKey = RGB(8, 0, 8);
	constexpr int kWindowCornerRadius = 6;
	constexpr wchar_t kAppVersion[] = L"1.3.0";

	// Tray icon / menu.
	constexpr UINT kTrayCallbackMessage = WM_APP + 1;
	constexpr UINT kTrayIconId = 1;
	constexpr UINT kMenuIdToggle = 1001;
	constexpr UINT kMenuIdSettings = 1002;
	constexpr UINT kMenuIdExit = 1003;
	constexpr UINT kMenuIdWebAddress = 1004;

	// Global toggle hotkey id (used only when the shortcut type is Keyboard).
	constexpr UINT kToggleHotkeyId = 1;

	struct ToypadSlot
	{
		uint8_t pad;
		uint8_t index;
		const wchar_t* label;
	};

	// pad = 1 is the CENTER pad, pad = 2 is the LEFT pad, pad = 3 is the RIGHT
	// pad (this is the real toypad convention, confirmed by the USB protocol and
	// by Cemu's own built-in toypad window, which renders pad 2 on the left and
	// pad 1 in the middle). The toypad geometry is 3/1/3: the left section owns
	// slots 0/3/4, the center section is the single slot 1, the right section
	// owns slots 2/5/6.
	constexpr std::array<ToypadSlot, 7> kSlots = {{
		{2, 0, L"Left - upper"},
		{1, 1, L"Center"},
		{3, 2, L"Right - upper"},
		{2, 3, L"Left - lower left"},
		{2, 4, L"Left - lower right"},
		{3, 5, L"Right - lower left"},
		{3, 6, L"Right - lower right"},
	}};

	// Controller inputs as a bitmask. XInput's own WORD is full (0x0001 to
	// 0x8000), so the analog triggers - which XInput reports as axes, not
	// buttons - get bits above it and this stays 32-bit wide. Everything
	// that stores or compares a button set uses this type, including the
	// values persisted in LegoToypad.ini.
	using ButtonMask = uint32_t;

	// LT / RT as pressable buttons. A trigger counts as pressed past this
	// much of its travel; XInput's own recommended threshold is 30/255,
	// but a higher one keeps a resting finger from arming a binding.
	constexpr ButtonMask kTriggerLeftButton = 0x00010000u;
	constexpr ButtonMask kTriggerRightButton = 0x00020000u;
	constexpr int16_t kTriggerPressThreshold = 8000; // of SDL's 0..32767

	// Which pad's labels the UI speaks. SDL reads Xbox, PlayStation and
	// Nintendo pads through the same GameController API, so this only
	// changes what the buttons are *called*, never what they do.
	// Order matches the style order of kControllerIconResourceIds, so a
	// style doubles as the row index into the bundled icon table.
	enum class ButtonStyle
	{
		Xbox,
		DualShock4,
		Nintendo,
	};
	constexpr size_t kButtonStyleCount = 3;

	// Settings row values: Auto follows whatever is plugged in, the rest
	// pin one style. Auto is index 0, so the explicit styles are offset
	// by one (see ButtonStyleFromChoice).
	constexpr size_t kButtonStyleChoiceCount = kButtonStyleCount + 1;

	struct ButtonName
	{
		ButtonMask mask;
		const wchar_t* xbox;
		const wchar_t* dualShock4;
		// Nintendo face buttons keep their letters: SDL is asked to report
		// them by label (SDL_GAMECONTROLLER_USE_BUTTON_LABELS), so the
		// button printed "A" on the pad is the one the app calls A, even
		// though it sits where an Xbox pad has B.
		const wchar_t* nintendo;
	};

	// Order matches the button order of kControllerIconResourceIds, so an
	// entry's index here is also its column in the bundled icon table.
	constexpr std::array<ButtonName, 16> kButtonNames = {{
		{XINPUT_GAMEPAD_DPAD_UP, L"D-Pad Up", L"D-Pad Up", L"D-Pad Up"},
		{XINPUT_GAMEPAD_DPAD_DOWN, L"D-Pad Down", L"D-Pad Down", L"D-Pad Down"},
		{XINPUT_GAMEPAD_DPAD_LEFT, L"D-Pad Left", L"D-Pad Left", L"D-Pad Left"},
		{XINPUT_GAMEPAD_DPAD_RIGHT, L"D-Pad Right", L"D-Pad Right", L"D-Pad Right"},
		{XINPUT_GAMEPAD_START, L"Start", L"Options", L"+"},
		{XINPUT_GAMEPAD_BACK, L"Back", L"Share", L"-"},
		{XINPUT_GAMEPAD_LEFT_THUMB, L"Left Stick Click", L"L3", L"Left Stick Click"},
		{XINPUT_GAMEPAD_RIGHT_THUMB, L"Right Stick Click", L"R3", L"Right Stick Click"},
		{XINPUT_GAMEPAD_LEFT_SHOULDER, L"LB", L"L1", L"L"},
		{XINPUT_GAMEPAD_RIGHT_SHOULDER, L"RB", L"R1", L"R"},
		{kTriggerLeftButton, L"LT", L"L2", L"ZL"},
		{kTriggerRightButton, L"RT", L"R2", L"ZR"},
		{XINPUT_GAMEPAD_A, L"A", L"Cross", L"A"},
		{XINPUT_GAMEPAD_B, L"B", L"Circle", L"B"},
		{XINPUT_GAMEPAD_X, L"X", L"Square", L"X"},
		{XINPUT_GAMEPAD_Y, L"Y", L"Triangle", L"Y"},
	}};

	const wchar_t* ButtonNameFor(const ButtonName& entry, ButtonStyle style)
	{
		switch (style)
		{
		case ButtonStyle::DualShock4: return entry.dualShock4;
		case ButtonStyle::Nintendo: return entry.nintendo;
		case ButtonStyle::Xbox: break;
		}
		return entry.xbox;
	}

	// The bundled icon for one button in one style, or 0 when that style's
	// set doesn't include it (the caller then falls back to the name).
	int ButtonIconResourceId(size_t buttonIndex, ButtonStyle style)
	{
		const size_t styleIndex = static_cast<size_t>(style);
		if (styleIndex >= kControllerIconStyleCount || buttonIndex >= kControllerIconButtonCount)
			return 0;
		return kControllerIconResourceIds[styleIndex][buttonIndex];
	}

	// The D-pad is reserved for menu navigation and can never be rebound;
	// the rest of the picker's own buttons live in kBindableActions and are
	// checked dynamically (see ReservedNavigationMask below), since every
	// one of them is remappable from the Settings screen.
	constexpr ButtonMask kDpadButtons = XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN |
		XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT;

	// Reserved chord that always cancels shortcut capture on a controller,
	// regardless of what is being assigned. Keyboard capture is cancelled
	// with Esc instead. This guarantees there is always a way to back out
	// without a mouse or keyboard.
	constexpr ButtonMask kShortcutCancelChord = XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START;

	enum class Screen
	{
		PadViewer,     // 7 gloss pads; initial screen. Choose a pad.
		PadAction,     // bottom action bar inside the selected pad: Load / Move / Clear
		FranchiseList, // grid of the 30 world (franchise) tiles
		RosterList,    // that world's characters + vehicles as circular portraits
		PlusPicker,    // capsule listing a multi-build vehicle's builds by number
		Settings,
	};

	// The 3 actions in the selected pad's bottom action bar.
	enum class PadActionKind
	{
		Load,
		Move,
		Clear,
	};
	constexpr size_t kPadActionCount = 3;

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
	// to actually land. See "Clear all pad" in Settings.
	struct PadSlot
	{
		bool occupied = false;
		std::wstring figureName;
		int binResourceId = 0;
		int portraitResourceId = 0;
		unsigned int ringColor = 0;
	};

	// One flat slot in the roster grid: either a character portrait, a
	// vehicle's build-1 portrait, or the "+" tile of a multi-build vehicle.
	struct RosterSlot
	{
		enum class Kind { Character, Vehicle, Plus } kind = Kind::Character;
		const RosterEntry* entry = nullptr;    // Character / Vehicle (build 1)
		const VehicleGroup* group = nullptr;   // Plus tile
	};

	struct AppState
	{
		Screen screen = Screen::PadViewer;
		size_t slotIndex = 0;
		size_t padActionIndex = 0;
		int hoveredPadActionIndex = -1;
		int pressedPadActionIndex = -1;
		size_t settingsIndex = 0;
		uint16_t port = 9191;
		std::wstring status;

		bool overlayVisible = false;
		HWND previousForegroundWindow = nullptr;

		ShortcutType shortcutType = ShortcutType::Controller;
		ButtonMask shortcutControllerMask = XINPUT_GAMEPAD_BACK;
		UINT shortcutKeyModifiers = 0;
		UINT shortcutKeyCode = 0;
bool swapConfirmBackButtons = false;
		size_t backgroundIndex = 0;
		bool capturingShortcut = false;
		// Story mode: picking a figure skips the series grid entirely and
		// shows only the starter-pack roster (Batman, Gandalf The Grey,
		// Wyldstyle, Batmobile), like the game's own story campaign.
		bool storyMode = false;
		// Controller bindings for the picker's own actions. Single-button
		// masks, remappable from the Settings screen, persisted in [Input].
		ButtonMask buttonConfirm = XINPUT_GAMEPAD_A;
		ButtonMask buttonBack = XINPUT_GAMEPAD_B;
		ButtonMask buttonSettings = XINPUT_GAMEPAD_Y;
		ButtonMask buttonMoveActive = XINPUT_GAMEPAD_X;
		ButtonMask buttonQuickLoad = XINPUT_GAMEPAD_RIGHT_SHOULDER;
		ButtonMask buttonQuickClear = XINPUT_GAMEPAD_LEFT_SHOULDER;
		// Index into kBindableActions while capturing a new button for one
		// of them from Settings; -1 when no binding capture is running.
		int capturingBindingIndex = -1;
		// Which pad's button labels the UI uses. 0 = Auto (follow whatever
		// is connected), 1..4 = pinned to one style.
		size_t buttonStyleChoice = 0;
		// Live controller facts, refreshed by every controller poll: what
		// Auto resolves to, and whether anything is plugged in at all.
		ButtonStyle detectedButtonStyle = ButtonStyle::Xbox;
		bool controllerConnected = false;
		// Web remote: the built-in HTTP server that lets the same tag library
		// be driven from a phone browser on the local network. Runs on its own
		// thread; requests that touch app state are marshaled back to the
		// window's UI thread, so this is fully off by default.
		bool webEnabled = true;
		uint16_t webPort = 8765;
		std::wstring webUrl;
		// Stays false until every controller button has been seen released
		// at least once after entering capture mode, so whatever button was
		// still held down from opening the Settings screen can't be
		// mistaken for the start of the new shortcut.
		bool shortcutCaptureArmed = false;

		std::array<PadSlot, 7> padState{};
		// True while PadViewer is being shown specifically to pick a Move's
		// destination pad, rather than the normal "pick a pad to act on"
		// mode. Reuses the same screen/grid per the original design intent.
		bool selectingMoveDestination = false;
		size_t moveSourceSlotIndex = 0;

		// True while the RosterList screen is showing the story-mode starter
		// roster instead of a franchise's roster, so Back returns straight
		// to the pad viewer and no franchise logo is drawn as the header.
		bool storyRosterActive = false;

		// Franchise / roster browsing state.
		size_t franchiseIndex = 0;
		int franchiseTopRow = 0; // first visible franchise row (scrolled grid)
		std::vector<RosterSlot> rosterSlots;
		size_t rosterIndex = 0;
		int rosterTopRow = 0; // first visible roster row (scrolled grids)
		const VehicleGroup* plusGroup = nullptr;
		size_t plusBuildIndex = 0;
	};

	AppState g_app;
	HANDLE g_inputOwnershipEvent = nullptr;

	// The picker's rebindable actions, in the order they appear as Settings
	// rows. Confirm/Back stay subject to the swap-confirm-back setting on
	// top of whatever buttons they are bound to.
	struct BindableAction
	{
		const wchar_t* label;   // Settings row / status text
		const wchar_t* iniKey;  // [Input] key it persists under
		ButtonMask AppState::*button; // the binding itself
	};

	constexpr std::array<BindableAction, 6> kBindableActions = {{
		{L"Confirm", L"ButtonConfirm", &AppState::buttonConfirm},
		{L"Back", L"ButtonBack", &AppState::buttonBack},
		{L"Settings", L"ButtonSettings", &AppState::buttonSettings},
		{L"Move active pad", L"ButtonMoveActive", &AppState::buttonMoveActive},
		{L"Quick load", L"ButtonQuickLoad", &AppState::buttonQuickLoad},
		{L"Quick clear", L"ButtonQuickClear", &AppState::buttonQuickClear},
	}};

	// Settings rows: shortcut, confirm style, background, story mode,
	// button labels, one rebind row per kBindableActions entry, clear all
	// pads, web remote, reset to defaults.
	constexpr size_t kSettingsBindingFirst = 5;
	constexpr size_t kSettingsItemCount = kSettingsBindingFirst + kBindableActions.size() + 3;

	// Auto resolves to the connected pad's own style; a pinned choice wins
	// over detection entirely.
	ButtonStyle EffectiveButtonStyle()
	{
		if (g_app.buttonStyleChoice == 0)
			return g_app.detectedButtonStyle;
		return static_cast<ButtonStyle>(std::min(g_app.buttonStyleChoice, kButtonStyleCount) - 1);
	}

	std::wstring ButtonStyleName(ButtonStyle style)
	{
		switch (style)
		{
		case ButtonStyle::DualShock4: return L"DualShock 4";
		case ButtonStyle::Nintendo: return L"Switch";
		case ButtonStyle::Xbox: break;
		}
		return L"Xbox";
	}

	// Every button the picker itself currently reacts to while the overlay
	// is open. A toggle shortcut made up of only these would fight normal
	// use of the picker, so shortcut capture requires at least one button
	// outside this set. Computed at call time because the bindings are
	// remappable.
	ButtonMask ReservedNavigationMask()
	{
		ButtonMask mask = kDpadButtons;
		for (const auto& action : kBindableActions)
			mask |= g_app.*(action.button);
		return mask;
	}

	// ---------------------------------------------------------------------
	// Web remote (phone browser control)
	// ---------------------------------------------------------------------
	// A tiny HTTP server embedded in the exe. The sockets library is already
	// linked for the toypad listener, and every image/tag is already compiled
	// in as resources, so the server needs nothing on disk. Requests that read
	// or mutate app state are posted to the window message loop on the UI
	// thread (kWebMessage) so they can never race the controller polling.
	constexpr UINT kWebMessage = WM_APP + 2;
	constexpr UINT kWebDefaultPort = 8765;

	struct WebJob
	{
		enum class Op { State, Catalog, Load, Move, Clear, ClearAll } op = Op::State;
		int a = 0; // slot index for Load/Clear; source slot for Move
		int b = 0; // bin resource id for Load; destination slot for Move
		std::string result; // JSON response body, written on the UI thread
		bool ok = false;
		HANDLE done = nullptr;
	};

	std::atomic<bool> g_webRunning{false};
	SOCKET g_webListenSocket = INVALID_SOCKET;
	HWND g_webWindow = nullptr;
	std::thread g_webThread;
	HWND g_mainWindow = nullptr;

	// The library catalog is static once the app is running, so it is built a
	// single time on the UI thread (it reads live background settings) and
	// then served from cache to every browser tab.
	bool g_catalogBuilt = false;
	std::string g_catalogCache;

	// Forward-declared here; defined later in the file. Confirm()/Back() run
	// before those definitions appear.
void UpdateInputOwnership(HWND window);
	void HideOverlay(HWND window);
	void BeginShortcutCapture();
	void CancelShortcutCapture();
	void BeginBindingCapture(size_t actionIndex);
	void CancelBindingCapture();
	void ResetSettingsToDefaults();
	void ApplyOverlayTransparency(HWND window);
	int CurrentBackgroundResourceId();
	bool StartWebServer(HWND window);
	void StopWebServer();
	void ToggleWebRemote();
	std::wstring DescribeWebRemote();
	std::wstring GetLanAddress();
	void HandleWebJob(WebJob& job);

	// ---------------------------------------------------------------------
	// GDI+ plumbing
	// ---------------------------------------------------------------------

	ULONG_PTR g_gdiplusToken = 0;

	// Compacta Regular, embedded as a resource and loaded in-memory at startup.
	// GDI gets it via AddFontMemResourceEx (usable as a normal face name by the
	// HFONT created in Paint); GDI+ gets its own copy via PrivateFontCollection
	// (used by RenderPlaceholder's Gdiplus::Font). Both stem from the same
	// embedded bytes; the resolved family name below feeds the GDI HFONT.
	Gdiplus::PrivateFontCollection g_privateFonts;
	const Gdiplus::FontFamily* g_uiFontFamily = nullptr;  // current family (points into storage or the GDI+ fallback)
	Gdiplus::FontFamily* g_uiFontFamilyStorage = nullptr; // owns the new[] array of private families, if any
	std::wstring g_uiFontFamilyName = L"Segoe UI";        // resolved name used by CreateFontW
	HANDLE g_gdiFont = nullptr;                            // AddFontMemResourceEx handle

	// Cached off-screen glossy/glowing bitmaps, keyed by (kind, state, ids).
	// Every frame paints by DrawImage-ing these; the expensive gradient and
	// glow construction below only happens once per distinct visual state.
	enum class GlossKind : int
	{
		Pad,
		Portrait,     // circular photo portrait
		PlusTile,     // "+" tile
		Placeholder,  // circular letter plate for entries without a portrait
		FranchiseTile,
		Pill,         // capsule menu background
		PillRow,      // capsule focus highlight row
		Background,   // full-window starfield + dark overlay
		ScaledImage,  // raw asset rescaled (optionally rounded-rect clipped) once
	};

	struct GlossKey
	{
		GlossKind kind;
		int variant;      // per-kind state (pad visual, focused flag, row count...)
		int resId;        // 0 when the kind has no image payload
		unsigned int color;
		int w;
		int h;
		bool operator<(const GlossKey& other) const
		{
			if (kind != other.kind) return static_cast<int>(kind) < static_cast<int>(other.kind);
			if (variant != other.variant) return variant < other.variant;
			if (resId != other.resId) return resId < other.resId;
			if (color != other.color) return color < other.color;
			if (w != other.w) return w < other.w;
			return h < other.h;
		}
	};

	std::map<GlossKey, Gdiplus::Bitmap*> g_glossCache;

	struct AssetImage
	{
		Gdiplus::Bitmap* bitmap = nullptr;
		IStream* stream = nullptr;
	};

	std::map<int, AssetImage> g_assetImages;

	// Raw payload of an embedded resource (tag bytes, png bytes...). Owned
	// by the module; the pointer stays valid for the app's lifetime.
	const uint8_t* GetResourceBytes(int resId, DWORD& sizeOut)
	{
		const HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(resId), RT_RCDATA);
		if (!resource)
			return nullptr;
		const HGLOBAL loaded = LoadResource(nullptr, resource);
		if (!loaded)
			return nullptr;
		sizeOut = SizeofResource(nullptr, resource);
		return static_cast<const uint8_t*>(LockResource(loaded));
	}

	std::vector<uint8_t> LoadResourceBytes(int resId)
	{
		DWORD size = 0;
		const uint8_t* data = GetResourceBytes(resId, size);
		if (!data)
			return {};
		return std::vector<uint8_t>(data, data + size);
	}

	// GDI+ bitmap decoded from an embedded image resource, cached per id.
	// GDI+ needs the IStream it was decoded from to stay alive for the
	// Bitmap's lifetime, so both are cached together and freed at shutdown.
	Gdiplus::Bitmap* GetAssetBitmap(int resId)
	{
		if (resId == 0)
			return nullptr;
		const auto existing = g_assetImages.find(resId);
		if (existing != g_assetImages.end())
			return existing->second.bitmap;

		DWORD size = 0;
		const uint8_t* data = GetResourceBytes(resId, size);
		if (!data || size == 0)
			return nullptr;

		// CreateStreamOnHGlobal requires a GlobalAlloc handle, so the
		// payload is staged into one (transient; freed when the stream is
		// released).
		const HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, size);
		if (!hGlobal)
			return nullptr;
		void* const destination = GlobalLock(hGlobal);
		if (!destination)
		{
			GlobalFree(hGlobal);
			return nullptr;
		}
		memcpy(destination, data, size);
		GlobalUnlock(hGlobal);

		IStream* stream = nullptr;
		if (FAILED(CreateStreamOnHGlobal(hGlobal, TRUE, &stream)) || !stream)
		{
			GlobalFree(hGlobal);
			return nullptr;
		}

		Gdiplus::Bitmap* bitmap = new Gdiplus::Bitmap(stream, FALSE);
		if (bitmap->GetLastStatus() != Gdiplus::Ok)
		{
			delete bitmap;
			stream->Release();
			return nullptr;
		}

		AssetImage entry;
		entry.bitmap = bitmap;
		entry.stream = stream;
		g_assetImages[resId] = entry;
		return bitmap;
	}

	void ReleaseAssetImages()
	{
		for (auto& [id, entry] : g_assetImages)
		{
			delete entry.bitmap;
			entry.stream->Release();
		}
		g_assetImages.clear();
	}

	void ReleaseGlossCache()
	{
		for (auto& [key, bitmap] : g_glossCache)
			delete bitmap;
		g_glossCache.clear();
	}

	// ---------------------------------------------------------------------
	// Glossy / glowing shape renderers (each runs ONCE per cached state)
	// ---------------------------------------------------------------------

	constexpr unsigned int kGlowGold = 0x00FFCC33;    // selection glow everywhere
	constexpr unsigned int kGlowBlue = 0x005A96E0;    // move-source pads
	constexpr unsigned int kPadBorderIdle = 0x00525A6A;
	constexpr unsigned int kPadBorderOccupied = 0x0060B476;
	constexpr unsigned int kPadFocusEdge = 0x003838E8; // selected pad edge (RGB 232,56,56)
	constexpr unsigned int kFranchiseFocusEdge = 0x00FF9D42; // selected franchise edge (RGB 66,157,255)

	void AddRoundedRectPath(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& rect, float radius)
	{
		const float diameter = radius * 2.0f;
		const float right = rect.X + rect.Width;
		const float bottom = rect.Y + rect.Height;
		path.Reset();
		path.AddArc(rect.X, rect.Y, diameter, diameter, 180.0f, 90.0f);
		path.AddArc(right - diameter, rect.Y, diameter, diameter, 270.0f, 90.0f);
		path.AddArc(right - diameter, bottom - diameter, diameter, diameter, 0.0f, 90.0f);
		path.AddArc(rect.X, bottom - diameter, diameter, diameter, 90.0f, 90.0f);
		path.CloseFigure();
	}

	// Pad highlight outline: the round center pad gets a circular outline to
	// match its art, the rest stay rounded rectangles.
	void AddPadPath(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& rect, bool circular)
	{
		if (circular)
		{
			path.Reset();
			path.AddEllipse(rect);
			path.CloseFigure();
		}
		else
		{
			AddRoundedRectPath(path, rect, 14.0f);
		}
	}

	// ---------------------------------------------------------------------
	// Real glow: soft light falloff via repeated separable box blur.
	//
	// GDI+ has no native Gaussian blur, so the glow shape is rasterized as an
	// opaque solid silhouette and that silhouette is blurred outward with a
	// sliding-window box blur (2 horizontal + 2 vertical passes approximate a
	// Gaussian profile). The blurred, premultiplied silhouette is composited
	// under the crisp shape, producing a soft halo instead of the hard
	// concentric strokes the old DrawGlowPath drew. This only ever runs at
	// cached-bitmap generation time, never per frame.
	// ---------------------------------------------------------------------

	// One channel of a 1-D box blur over `count` samples with `stride`
	// bytes between samples (4 for a row, width*4 for a column) at byte
	// offset `offset`. Edge samples are clamped (shrinking the window).
	void BoxBlurChannel1D(const uint8_t* src, uint8_t* dst, int count, int radius, int stride, int offset)
	{
		std::vector<int> prefix(count + 1);
		prefix[0] = 0;
		for (int i = 0; i < count; ++i)
			prefix[i + 1] = prefix[i] + src[i * stride + offset];
		for (int k = 0; k < count; ++k)
		{
			int lo = k - radius;
			if (lo < 0) lo = 0;
			int hi = k + radius;
			if (hi > count - 1) hi = count - 1;
			const int sum = prefix[hi + 1] - prefix[lo];
			const int n = hi - lo + 1;
			dst[k * stride + offset] = static_cast<uint8_t>((sum + n / 2) / n);
		}
	}

	// Separable box blur over a premultiplied BGRA/PARGB buffer, reading from
	// src into dst (either axis). Blurring all four channels together keeps
	// the premultiplied data consistent so the falloff is even and clean.
	void BoxBlurPass(const uint8_t* src, uint8_t* dst, int width, int height, int radius, bool horizontal)
	{
		const int n = horizontal ? width : height;
		const int other = horizontal ? height : width;
		for (int o = 0; o < other; ++o)
		{
			const int base = horizontal ? o * width * 4 : o * 4;
			for (int c = 0; c < 4; ++c)
				BoxBlurChannel1D(src + base, dst + base, n, radius, horizontal ? 4 : width * 4, c);
		}
	}

	// Rasterizes `path` (a closed shape), blurs it soft, and composites the
	// result behind everything else that draws on top. `width`/`height` are
	// the full target bitmap dimensions (the shape is already placed in that
	// bitmap's coordinate space).
	void CompositeGlow(Gdiplus::Graphics& g, Gdiplus::Bitmap* mask, int radius, int width, int height);

	void DrawGlow(Gdiplus::Graphics& g, const Gdiplus::GraphicsPath& path,
		unsigned int color, int radius, int width, int height)
	{
		Gdiplus::Bitmap mask(width, height, PixelFormat32bppPARGB);
		Gdiplus::Graphics mg(&mask);
		mg.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
		Gdiplus::SolidBrush solid(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)));
		mg.FillPath(&solid, &path);
		CompositeGlow(g, &mask, radius, width, height);
	}

	// Blurs the alpha of `mask` (a PARGB bitmap holding the silhouette, RGB
	// already premultiplied) and composites it as a soft colored halo. Shared
	// by the path-based glow and the art-derived glow so both use identical
	// blur quality.
	void CompositeGlow(Gdiplus::Graphics& g, Gdiplus::Bitmap* mask, int radius, int width, int height)
	{
		if (!mask)
			return;

		const Gdiplus::Rect lock(0, 0, width, height);
		Gdiplus::BitmapData data;
		if (mask->LockBits(&lock, Gdiplus::ImageLockModeRead | Gdiplus::ImageLockModeWrite,
				PixelFormat32bppPARGB, &data) != Gdiplus::Ok)
			return;

		const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
		std::vector<uint8_t> a(bytes);
		std::vector<uint8_t> b(bytes);
		const uint8_t* source = static_cast<const uint8_t*>(data.Scan0);
		for (int y = 0; y < height; ++y)
			std::memcpy(a.data() + static_cast<size_t>(y) * width * 4,
				source + static_cast<size_t>(y) * data.Stride, static_cast<size_t>(width) * 4);

		for (int i = 0; i < 2; ++i)
		{
			BoxBlurPass(a.data(), b.data(), width, height, radius, true);
			BoxBlurPass(b.data(), a.data(), width, height, radius, false);
		}

		uint8_t* dest = static_cast<uint8_t*>(data.Scan0);
		for (int y = 0; y < height; ++y)
			std::memcpy(dest + static_cast<size_t>(y) * data.Stride,
				a.data() + static_cast<size_t>(y) * width * 4, static_cast<size_t>(width) * 4);
		mask->UnlockBits(&data);

		g.DrawImage(mask, 0, 0, width, height);
	}

	void DrawTopGloss(Gdiplus::Graphics& g, const Gdiplus::GraphicsPath& clipPath, int width, int height)
	{
		Gdiplus::Region clip(&clipPath);
		g.SetClip(&clip);
		Gdiplus::LinearGradientBrush gloss(
			Gdiplus::RectF(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)),
			Gdiplus::Color(70, 255, 255, 255), Gdiplus::Color(0, 255, 255, 255),
			Gdiplus::LinearGradientModeVertical);
		g.FillRectangle(&gloss, 0, 0, width, height);
		g.ResetClip();
	}

	enum class PadVisual : int
	{
		Idle,
		IdleSelected,
		Occupied,
		OccupiedSelected,
		MoveSource,
	};

	// Glossy rounded-rect pad backed by the per-slot background PNG.
	// `cell` is the real slot rect; the returned bitmap is padded by
	// kPadGlowMargin gives the move-source halo room. Selected pads use only a
	// crisp focus edge so they stay cheap to render.
	// the slot's background resource id, since each of the 7 pads renders a
	// different base image even at identical visuals/size.
	constexpr int kPadGlowMargin = 8;

	Gdiplus::Bitmap* RenderPad(int slotIndex, const RECT& cell, PadVisual visual, unsigned int occupantColor)
	{
		const int w = (cell.right - cell.left) + kPadGlowMargin * 2;
		const int h = (cell.bottom - cell.top) + kPadGlowMargin * 2;
		const GlossKey key{GlossKind::Pad, static_cast<int>(visual), kPadBackgroundResourceIds[slotIndex], occupantColor, w, h};
		const auto cached = g_glossCache.find(key);
		if (cached != g_glossCache.end())
			return cached->second;

		Gdiplus::Bitmap* bitmap = new Gdiplus::Bitmap(w, h, PixelFormat32bppPARGB);
		Gdiplus::Graphics g(bitmap);
		g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
		g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

		const Gdiplus::RectF rect(static_cast<float>(kPadGlowMargin), static_cast<float>(kPadGlowMargin),
			static_cast<float>(cell.right - cell.left), static_cast<float>(cell.bottom - cell.top));
		Gdiplus::GraphicsPath path;
		AddPadPath(path, rect, slotIndex == 1);

		const bool selected = visual == PadVisual::IdleSelected || visual == PadVisual::OccupiedSelected;
		const bool occupied = visual == PadVisual::Occupied || visual == PadVisual::OccupiedSelected;
		const bool moveSource = visual == PadVisual::MoveSource;

		if (moveSource)
			DrawGlow(g, path, kGlowBlue, 8, w, h);

		// The real pad art (already includes the glass/gloss look) fills the
		// cell rect; the procedural gradient and synthetic glossy streak are
		// gone. The rounded-rect clip keeps the shape's outline crisp.
		Gdiplus::Bitmap* padImage = GetAssetBitmap(kPadBackgroundResourceIds[slotIndex]);
		if (padImage)
		{
			Gdiplus::Region clip(&path);
			g.SetClip(&clip);
			g.DrawImage(padImage, rect);
			g.ResetClip();
		}

		const unsigned int occupiedColor = occupantColor != 0 ? occupantColor : kPadBorderOccupied;
		// Occupied has no dedicated art, so the loaded figure's generated
		// color becomes a soft per-pad tint over the same glass texture.
		if (occupied)
		{
			Gdiplus::SolidBrush tint(Gdiplus::Color(
				42, GetRValue(occupiedColor), GetGValue(occupiedColor), GetBValue(occupiedColor)));
			g.FillPath(&tint, &path);
		}

		const unsigned int border = moveSource ? kGlowBlue
			: (selected ? kPadFocusEdge
				: (occupied ? occupiedColor : kPadBorderIdle));
		Gdiplus::Pen borderPen(
			Gdiplus::Color(selected ? 230 : 255, GetRValue(border), GetGValue(border), GetBValue(border)),
			selected ? 3.25f : 2.0f);
		borderPen.SetLineJoin(Gdiplus::LineJoinRound);
		g.DrawPath(&borderPen, &path);

		g_glossCache[key] = bitmap;
		return bitmap;
	}

	// Raw asset rescaled to (w, h) and cached, so the expensive bicubic
	// filter runs once per distinct size instead of on every paint. `radius`
	// > 0 clips the result to a rounded rectangle of that radius (panel art
	// behind grids); 0 keeps the full rectangle. The cache key's variant
	// encodes the radius so same-sized panels with different corners don't
	// share an entry.
	Gdiplus::Bitmap* RenderScaledAsset(int resId, int w, int h, int radius)
	{
		Gdiplus::Bitmap* src = GetAssetBitmap(resId);
		if (!src)
			return nullptr;

		const GlossKey key{GlossKind::ScaledImage, radius, resId, 0, w, h};
		const auto cached = g_glossCache.find(key);
		if (cached != g_glossCache.end())
			return cached->second;

		Gdiplus::Bitmap* bitmap = new Gdiplus::Bitmap(w, h, PixelFormat32bppPARGB);
		Gdiplus::Graphics g(bitmap);
		g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
		g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
		const Gdiplus::RectF rect(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
		if (radius > 0)
		{
			Gdiplus::GraphicsPath path;
			AddRoundedRectPath(path, rect, static_cast<float>(radius));
			Gdiplus::Region clip(&path);
			g.SetClip(&clip);
			g.DrawImage(src, rect);
			g.ResetClip();
		}
		else
		{
			g.DrawImage(src, rect);
		}

		g_glossCache[key] = bitmap;
		return bitmap;
	}
	// brighter ring plus a colored glow. `diameter` is the circle size; the
	// bitmap is padded by kPortraitMargin for the glow.
	constexpr int kPortraitMargin = 10;

	Gdiplus::Rect GetOpaqueContentBounds(Gdiplus::Bitmap* image)
	{
		if (!image)
			return Gdiplus::Rect(0, 0, 1, 1);

		const int w = static_cast<int>(image->GetWidth());
		const int h = static_cast<int>(image->GetHeight());
		if (w <= 0 || h <= 0)
			return Gdiplus::Rect(0, 0, 1, 1);

		Gdiplus::Bitmap probe(w, h, PixelFormat32bppPARGB);
		Gdiplus::Graphics pg(&probe);
		pg.Clear(Gdiplus::Color(0, 0, 0, 0));
		pg.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
		pg.DrawImage(image, 0, 0, w, h);

		Gdiplus::BitmapData data;
		const Gdiplus::Rect lock(0, 0, w, h);
		if (probe.LockBits(&lock, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &data) != Gdiplus::Ok)
			return Gdiplus::Rect(0, 0, w, h);

		int left = w;
		int top = h;
		int right = -1;
		int bottom = -1;
		const uint8_t* pixels = static_cast<const uint8_t*>(data.Scan0);
		for (int y = 0; y < h; ++y)
		{
			const uint8_t* row = pixels + static_cast<size_t>(y) * data.Stride;
			for (int x = 0; x < w; ++x)
			{
				if (row[x * 4 + 3] > 8)
				{
					left = std::min(left, x);
					top = std::min(top, y);
					right = std::max(right, x);
					bottom = std::max(bottom, y);
				}
			}
		}
		probe.UnlockBits(&data);

		if (right < left || bottom < top)
			return Gdiplus::Rect(0, 0, w, h);

		left = std::max(0, left - 2);
		top = std::max(0, top - 2);
		right = std::min(w - 1, right + 2);
		bottom = std::min(h - 1, bottom + 2);
		return Gdiplus::Rect(left, top, right - left + 1, bottom - top + 1);
	}

	void DrawCircularFocusHalo(Gdiplus::Graphics& g, const Gdiplus::RectF& circle, unsigned int color)
	{
		Gdiplus::RectF outer = circle;
		outer.Inflate(3.5f, 3.5f);
		Gdiplus::Pen soft(Gdiplus::Color(80, GetRValue(color), GetGValue(color), GetBValue(color)), 7.0f);
		g.DrawEllipse(&soft, outer);

		Gdiplus::RectF mid = circle;
		mid.Inflate(1.5f, 1.5f);
		Gdiplus::Pen crisp(Gdiplus::Color(130, GetRValue(color), GetGValue(color), GetBValue(color)), 3.0f);
		g.DrawEllipse(&crisp, mid);
	}

	Gdiplus::Bitmap* RenderPortrait(int resId, unsigned int ringColor, bool focused, int diameter)
	{
		const int w = diameter + kPortraitMargin * 2;
		const int h = w;
		const int variant = focused ? 1 : 0;
		const GlossKey key{GlossKind::Portrait, variant, resId, ringColor, w, h};
		const auto cached = g_glossCache.find(key);
		if (cached != g_glossCache.end())
			return cached->second;

		Gdiplus::Bitmap* bitmap = new Gdiplus::Bitmap(w, h, PixelFormat32bppPARGB);
		Gdiplus::Graphics g(bitmap);
		g.Clear(Gdiplus::Color(0, 0, 0, 0));
		g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
		g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

		const float d = static_cast<float>(diameter);
		const Gdiplus::RectF circle(static_cast<float>(kPortraitMargin), static_cast<float>(kPortraitMargin), d, d);
		Gdiplus::GraphicsPath circlePath;
		circlePath.AddEllipse(circle);

		if (focused)
			DrawCircularFocusHalo(g, circle, ringColor);

		Gdiplus::Bitmap* photo = GetAssetBitmap(resId);
		if (photo)
		{
			const Gdiplus::Rect source = GetOpaqueContentBounds(photo);
			const float targetD = d * 0.92f;
			const float scale = std::min(targetD / source.Width, targetD / source.Height);
			const float drawW = source.Width * scale;
			const float drawH = source.Height * scale;
			const Gdiplus::RectF dest(kPortraitMargin + (d - drawW) / 2.0f,
				kPortraitMargin + (d - drawH) / 2.0f, drawW, drawH);
			Gdiplus::Region clip(&circlePath);
			g.SetClip(&clip);
			g.DrawImage(photo, dest, static_cast<float>(source.X), static_cast<float>(source.Y),
				static_cast<float>(source.Width), static_cast<float>(source.Height), Gdiplus::UnitPixel);
			g.ResetClip();
		}
		else
		{
			Gdiplus::LinearGradientBrush fallback(circle,
				Gdiplus::Color(255, 46, 52, 64), Gdiplus::Color(255, 22, 26, 36),
				Gdiplus::LinearGradientModeVertical);
			g.FillEllipse(&fallback, circle);
		}

		// Glossy highlight on the upper-left of the sphere.
		DrawTopGloss(g, circlePath, w, h);

		// Coloring ring: idle is a thin subdued ring, focused is brighter
		// and thicker, plus a dark inner rim for depth.
		const BYTE ringAlpha = focused ? 255 : 170;
		Gdiplus::Pen ringPen(Gdiplus::Color(ringAlpha, GetRValue(ringColor), GetGValue(ringColor), GetBValue(ringColor)),
			focused ? 3.5f : 2.0f);
		g.DrawEllipse(&ringPen, circle);
		Gdiplus::Pen rimPen(Gdiplus::Color(70, 0, 0, 0), 1.0f);
		g.DrawEllipse(&rimPen, circle);

		g_glossCache[key] = bitmap;
		return bitmap;
	}

	// "+" tile for vehicles with more than one build; ring color follows
	// the vehicle group (build 1's color).
	Gdiplus::Bitmap* RenderPlusTile(unsigned int ringColor, bool focused, int diameter)
	{
		const int w = diameter + kPortraitMargin * 2;
		const int h = w;
		const int variant = focused ? 1 : 0;
		const GlossKey key{GlossKind::PlusTile, variant, 0, ringColor, w, h};
		const auto cached = g_glossCache.find(key);
		if (cached != g_glossCache.end())
			return cached->second;

		Gdiplus::Bitmap* bitmap = new Gdiplus::Bitmap(w, h, PixelFormat32bppPARGB);
		Gdiplus::Graphics g(bitmap);
		g.Clear(Gdiplus::Color(0, 0, 0, 0));
		g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

		const float d = static_cast<float>(diameter);
		const Gdiplus::RectF circle(static_cast<float>(kPortraitMargin), static_cast<float>(kPortraitMargin), d, d);
		Gdiplus::GraphicsPath circlePath;
		circlePath.AddEllipse(circle);

		if (focused)
			DrawCircularFocusHalo(g, circle, ringColor);

		Gdiplus::LinearGradientBrush fill(circle,
			Gdiplus::Color(255, 40, 46, 58), Gdiplus::Color(255, 18, 22, 30),
			Gdiplus::LinearGradientModeVertical);
		g.FillEllipse(&fill, circle);
		DrawTopGloss(g, circlePath, w, h);

		const float cx = kPortraitMargin + d / 2.0f;
		const float bar = d * 0.22f;
		Gdiplus::SolidBrush plusBrush(Gdiplus::Color(255, 235, 238, 244));
		g.FillRectangle(&plusBrush, cx - 1.8f, kPortraitMargin + (d - bar) / 2.0f, 3.6f, bar);
		g.FillRectangle(&plusBrush, kPortraitMargin + (d - bar) / 2.0f, cx - 1.8f, bar, 3.6f);

		const BYTE ringAlpha = focused ? 255 : 170;
		Gdiplus::Pen ringPen(Gdiplus::Color(ringAlpha, GetRValue(ringColor), GetGValue(ringColor), GetBValue(ringColor)),
			focused ? 3.5f : 2.0f);
		g.DrawEllipse(&ringPen, circle);

		g_glossCache[key] = bitmap;
		return bitmap;
	}

	// Circular letter plate for entries whose portrait resource is missing.
	Gdiplus::Bitmap* RenderPlaceholder(wchar_t initial, unsigned int ringColor, bool focused, int diameter)
	{
		const int w = diameter + kPortraitMargin * 2;
		const int h = w;
		const int variant = focused ? 1 : 0;
		const GlossKey key{GlossKind::Placeholder, variant, static_cast<int>(initial), ringColor, w, h};
		const auto cached = g_glossCache.find(key);
		if (cached != g_glossCache.end())
			return cached->second;

		Gdiplus::Bitmap* bitmap = new Gdiplus::Bitmap(w, h, PixelFormat32bppPARGB);
		Gdiplus::Graphics g(bitmap);
		g.Clear(Gdiplus::Color(0, 0, 0, 0));
		g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

		const float d = static_cast<float>(diameter);
		const Gdiplus::RectF circle(static_cast<float>(kPortraitMargin), static_cast<float>(kPortraitMargin), d, d);
		Gdiplus::GraphicsPath circlePath;
		circlePath.AddEllipse(circle);

		if (focused)
			DrawCircularFocusHalo(g, circle, ringColor);

		Gdiplus::LinearGradientBrush fill(circle,
			Gdiplus::Color(255, 46, 52, 64), Gdiplus::Color(255, 22, 26, 36),
			Gdiplus::LinearGradientModeVertical);
		g.FillEllipse(&fill, circle);
		DrawTopGloss(g, circlePath, w, h);

		Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 235, 238, 244));
		Gdiplus::Font font(g_uiFontFamily, std::max(12.0f, d * 0.32f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
		Gdiplus::StringFormat format;
		format.SetAlignment(Gdiplus::StringAlignmentCenter);
		format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
		const wchar_t letter[2] = { initial == 0 ? L'?' : initial, L'\0' };
		g.DrawString(letter, 1, &font, circle, &format, &textBrush);

		const BYTE ringAlpha = focused ? 255 : 170;
		Gdiplus::Pen ringPen(Gdiplus::Color(ringAlpha, GetRValue(ringColor), GetGValue(ringColor), GetBValue(ringColor)),
			focused ? 3.5f : 2.0f);
		g.DrawEllipse(&ringPen, circle);

		g_glossCache[key] = bitmap;
		return bitmap;
	}

	// Franchise tile: the world_tile background image with the world logo
	// drawn on top of it, plus a crisp focus outline when selected. The logo
	// alone identifies the world - no text label.
	Gdiplus::Bitmap* RenderFranchiseTile(int logoResourceId, bool focused)
	{
		constexpr int tileW = 190;
		constexpr int tileH = 100;
		constexpr int margin = 6;
		const int w = tileW + margin * 2;
		const int h = tileH + margin * 2;
		const int variant = focused ? 1 : 0;
		const GlossKey key{GlossKind::FranchiseTile, variant, logoResourceId, 0, w, h};
		const auto cached = g_glossCache.find(key);
		if (cached != g_glossCache.end())
			return cached->second;

		Gdiplus::Bitmap* bitmap = new Gdiplus::Bitmap(w, h, PixelFormat32bppPARGB);
		Gdiplus::Graphics g(bitmap);
		g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
		g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

		const Gdiplus::RectF rect(static_cast<float>(margin), static_cast<float>(margin),
			static_cast<float>(tileW), static_cast<float>(tileH));
		Gdiplus::GraphicsPath path;
		AddRoundedRectPath(path, rect, 12.0f);

		// The real tile art (the translucent world-tile panel) fills the
		// tile rect; the procedural gradient/gloss base is gone. The logo is
		// layered over this background below.
		Gdiplus::Bitmap* tileBase = GetAssetBitmap(kWorldTileResourceId);
		if (tileBase)
		{
			// Cover-fit: fill the whole tile area, cropping overflow.
			const float scale = std::max(rect.Width / tileBase->GetWidth(),
				rect.Height / tileBase->GetHeight());
			const float drawW = tileBase->GetWidth() * scale;
			const float drawH = tileBase->GetHeight() * scale;
			const Gdiplus::RectF dest(rect.X + (rect.Width - drawW) / 2.0f,
				rect.Y + (rect.Height - drawH) / 2.0f, drawW, drawH);
			Gdiplus::Region clip(&path);
			g.SetClip(&clip);
			g.DrawImage(tileBase, dest);
			g.ResetClip();
		}

		Gdiplus::Bitmap* logo = GetAssetBitmap(logoResourceId);
		if (logo)
		{
			// Contain-fit the logo across most of the tile.
			constexpr float logoAreaH = 84.0f;
			const Gdiplus::RectF box(margin + 8.0f, margin + 8.0f, tileW - 16.0f, logoAreaH);
			const float scale = std::min(box.Width / logo->GetWidth(), box.Height / logo->GetHeight());
			const float drawW = logo->GetWidth() * scale;
			const float drawH = logo->GetHeight() * scale;
			const Gdiplus::RectF dest(box.X + (box.Width - drawW) / 2.0f, box.Y + (box.Height - drawH) / 2.0f, drawW, drawH);
			Gdiplus::Region clip(&path);
			g.SetClip(&clip);
			g.DrawImage(logo, dest);
			g.ResetClip();
		}

		const unsigned int border = focused ? kFranchiseFocusEdge : kPadBorderIdle;
		Gdiplus::Pen borderPen(
			Gdiplus::Color(focused ? 230 : 200, GetRValue(border), GetGValue(border), GetBValue(border)),
			focused ? 2.75f : 1.5f);
		borderPen.SetLineJoin(Gdiplus::LineJoinRound);
		g.DrawPath(&borderPen, &path);

		g_glossCache[key] = bitmap;
		return bitmap;
	}

	// Capsule (pill) menu background; the focused option row is drawn on
	// top as RenderPillRow. `rows` decides the height.
	Gdiplus::Bitmap* RenderPill(int rows)
	{
		constexpr int rowH = 36;
		constexpr int padV = 7;
		constexpr int width = 340;
		constexpr int glowMargin = 8;
		const int h = rows * rowH + padV * 2;
		const int w = width + glowMargin * 2;
		const int hp = h + glowMargin * 2;
		const GlossKey key{GlossKind::Pill, rows, 0, 0, w, hp};
		const auto cached = g_glossCache.find(key);
		if (cached != g_glossCache.end())
			return cached->second;

		Gdiplus::Bitmap* bitmap = new Gdiplus::Bitmap(w, hp, PixelFormat32bppPARGB);
		Gdiplus::Graphics g(bitmap);
		g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

		const Gdiplus::RectF rect(static_cast<float>(glowMargin), static_cast<float>(glowMargin),
			static_cast<float>(width), static_cast<float>(h));
		Gdiplus::GraphicsPath path;
		AddRoundedRectPath(path, rect, static_cast<float>(h) / 2.0f);

		DrawGlow(g, path, kGlowGold, 5, w, hp);

		Gdiplus::LinearGradientBrush fill(rect,
			Gdiplus::Color(226, 22, 27, 38), Gdiplus::Color(226, 12, 15, 22),
			Gdiplus::LinearGradientModeVertical);
		g.FillPath(&fill, &path);

		Gdiplus::Region clip(&path);
		g.SetClip(&clip);
		Gdiplus::LinearGradientBrush gloss(
			Gdiplus::RectF(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(hp)),
			Gdiplus::Color(56, 255, 255, 255), Gdiplus::Color(0, 255, 255, 255),
			Gdiplus::LinearGradientModeVertical);
		g.FillRectangle(&gloss, 0, 0, w, hp);
		g.ResetClip();

		Gdiplus::Pen borderPen(Gdiplus::Color(190, 104, 114, 132), 1.5f);
		borderPen.SetLineJoin(Gdiplus::LineJoinRound);
		g.DrawPath(&borderPen, &path);

		g_glossCache[key] = bitmap;
		return bitmap;
	}

	Gdiplus::Bitmap* RenderPillRow(bool focused)
	{
		constexpr int rowW = 340 - 16;
		constexpr int rowH = 36;
		const int variant = focused ? 1 : 0;
		const GlossKey key{GlossKind::PillRow, variant, 0, 0, rowW, rowH};
		const auto cached = g_glossCache.find(key);
		if (cached != g_glossCache.end())
			return cached->second;

		Gdiplus::Bitmap* bitmap = new Gdiplus::Bitmap(rowW, rowH, PixelFormat32bppPARGB);
		Gdiplus::Graphics g(bitmap);
		g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

		const Gdiplus::RectF rect(0.0f, 0.0f, static_cast<float>(rowW), static_cast<float>(rowH));
		Gdiplus::GraphicsPath path;
		AddRoundedRectPath(path, rect, 14.0f);
		if (focused)
			DrawGlow(g, path, kGlowGold, 5, rowW, rowH);

		if (focused)
		{
			Gdiplus::LinearGradientBrush fill(rect,
				Gdiplus::Color(255, 58, 110, 176), Gdiplus::Color(255, 32, 74, 130),
				Gdiplus::LinearGradientModeVertical);
			g.FillPath(&fill, &path);
		}
		else
		{
			Gdiplus::SolidBrush fill(Gdiplus::Color(255, 26, 31, 42));
			g.FillPath(&fill, &path);
		}
		DrawTopGloss(g, path, rowW, rowH);

		g_glossCache[key] = bitmap;
		return bitmap;
	}

	// Full-window background: the selected resource scaled to cover the
	// window, with a dark overlay baked in for text legibility.
	Gdiplus::Bitmap* RenderBackground(int width, int height)
	{
		const int backgroundResourceId = CurrentBackgroundResourceId();
		const GlossKey key{GlossKind::Background, 0, backgroundResourceId, 0, width, height};
		const auto cached = g_glossCache.find(key);
		if (cached != g_glossCache.end())
			return cached->second;

		Gdiplus::Bitmap* bitmap = new Gdiplus::Bitmap(width, height, PixelFormat32bppPARGB);
		Gdiplus::Graphics g(bitmap);
		g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

		if (backgroundResourceId == 0)
		{
			// "No Background": the whole frame starts as the transparent
			// color key, so everything the UI doesn't cover shows the game.
			// No legibility overlay either - there is nothing to darken.
			Gdiplus::SolidBrush keyFill(Gdiplus::Color(255,
				GetRValue(kNoBackgroundColorKey), GetGValue(kNoBackgroundColorKey), GetBValue(kNoBackgroundColorKey)));
			g.FillRectangle(&keyFill, 0, 0, width, height);
			g_glossCache[key] = bitmap;
			return bitmap;
		}

		Gdiplus::Bitmap* background = GetAssetBitmap(backgroundResourceId);
		if (background)
		{
			const float scale = std::max(static_cast<float>(width) / background->GetWidth(),
				static_cast<float>(height) / background->GetHeight());
			const float drawW = background->GetWidth() * scale;
			const float drawH = background->GetHeight() * scale;
			const Gdiplus::RectF dest((width - drawW) / 2.0f, (height - drawH) / 2.0f, drawW, drawH);
			g.DrawImage(background, dest);
		}
		Gdiplus::SolidBrush overlay(Gdiplus::Color(96, 8, 10, 16));
		g.FillRectangle(&overlay, 0, 0, width, height);

		g_glossCache[key] = bitmap;
		return bitmap;
	}

	// ---------------------------------------------------------------------
	// Basic GDI+ text helpers
	// ---------------------------------------------------------------------

	Gdiplus::Color ToGdiPlusColor(COLORREF color, BYTE alpha = 255)
	{
		return Gdiplus::Color(alpha, GetRValue(color), GetGValue(color), GetBValue(color));
	}

	Gdiplus::Font MakeUIFont(float px)
	{
		const Gdiplus::FontFamily* family = g_uiFontFamily ? g_uiFontFamily : Gdiplus::FontFamily::GenericSansSerif();
		return Gdiplus::Font(family, px, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
	}

	void DrawTextLine(Gdiplus::Graphics& g, const std::wstring& text, int x, int y, int width, COLORREF color, int height = 30)
	{
		Gdiplus::Font font = MakeUIFont(22.0f);
		Gdiplus::SolidBrush brush(ToGdiPlusColor(color));
		Gdiplus::StringFormat format(Gdiplus::StringFormatFlagsNoWrap);
		format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
		format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
		const Gdiplus::RectF rect(static_cast<float>(x), static_cast<float>(y),
			static_cast<float>(width), static_cast<float>(height));
		g.DrawString(text.c_str(), -1, &font, rect, &format, &brush);
	}

	void DrawTextLineCentered(Gdiplus::Graphics& g, const std::wstring& text, int x, int y, int width, COLORREF color, int height = 30)
	{
		Gdiplus::Font font = MakeUIFont(22.0f);
		Gdiplus::SolidBrush brush(ToGdiPlusColor(color));
		Gdiplus::StringFormat format(Gdiplus::StringFormatFlagsNoWrap);
		format.SetAlignment(Gdiplus::StringAlignmentCenter);
		format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
		format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
		const Gdiplus::RectF rect(static_cast<float>(x), static_cast<float>(y),
			static_cast<float>(width), static_cast<float>(height));
		g.DrawString(text.c_str(), -1, &font, rect, &format, &brush);
	}

	// Centered word-wrap variant: shows the full string, wrapping onto extra
	// lines if it doesn't fit the width. Used for readable figure/build names.
	void DrawTextWrappedCentered(Gdiplus::Graphics& g, const std::wstring& text, int x, int y, int width, int height, COLORREF color)
	{
		Gdiplus::Font font = MakeUIFont(22.0f);
		Gdiplus::SolidBrush brush(ToGdiPlusColor(color));
		Gdiplus::StringFormat format;
		format.SetAlignment(Gdiplus::StringAlignmentCenter);
		format.SetTrimming(Gdiplus::StringTrimmingWord);
		const Gdiplus::RectF rect(static_cast<float>(x), static_cast<float>(y),
			static_cast<float>(width), static_cast<float>(height));
		g.DrawString(text.c_str(), -1, &font, rect, &format, &brush);
	}

	float MeasureTextWidth(Gdiplus::Graphics& g, const std::wstring& text, float px)
	{
		Gdiplus::Font font = MakeUIFont(px);
		Gdiplus::StringFormat format(Gdiplus::StringFormatFlagsNoWrap | Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
		Gdiplus::RectF bounds;
		const Gdiplus::RectF layout(0.0f, 0.0f, 10000.0f, 10000.0f);
		g.MeasureString(text.c_str(), -1, &font, layout, &format, &bounds);
		return bounds.Width;
	}

	float MeasureLongestTokenWidth(Gdiplus::Graphics& g, const std::wstring& text, float px)
	{
		float widest = 0.0f;
		size_t start = std::wstring::npos;
		for (size_t i = 0; i <= text.size(); ++i)
		{
			const bool boundary = i == text.size() || iswspace(text[i]);
			if (!boundary && start == std::wstring::npos)
				start = i;
			if (boundary && start != std::wstring::npos)
			{
				const std::wstring token = text.substr(start, i - start);
				widest = std::max(widest, MeasureTextWidth(g, token, px));
				start = std::wstring::npos;
			}
		}
		return widest;
	}

	float MeasureWrappedTextHeight(Gdiplus::Graphics& g, const std::wstring& text, int width, int maxHeight, float px)
	{
		Gdiplus::Font font = MakeUIFont(px);
		Gdiplus::StringFormat format;
		format.SetAlignment(Gdiplus::StringAlignmentCenter);
		format.SetTrimming(Gdiplus::StringTrimmingWord);
		Gdiplus::RectF bounds;
		const Gdiplus::RectF layout(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(maxHeight * 2));
		g.MeasureString(text.c_str(), -1, &font, layout, &format, &bounds);
		return bounds.Height;
	}

	bool WrappedTextFits(Gdiplus::Graphics& g, const std::wstring& text, int width, int height, float px)
	{
		return MeasureWrappedTextHeight(g, text, width, height, px) <= height + 0.5f &&
			MeasureLongestTokenWidth(g, text, px) <= width + 0.5f;
	}

	void DrawTextWrappedCenteredFit(Gdiplus::Graphics& g, const std::wstring& text, int x, int y, int width, int height, COLORREF color)
	{
		float chosenPx = 17.0f;
		for (int px = 22; px >= 17; --px)
		{
			if (WrappedTextFits(g, text, width, height, static_cast<float>(px)) || px == 17)
			{
				chosenPx = static_cast<float>(px);
				break;
			}
		}

		Gdiplus::Font font = MakeUIFont(chosenPx);
		Gdiplus::SolidBrush brush(ToGdiPlusColor(color));
		Gdiplus::StringFormat format;
		format.SetAlignment(Gdiplus::StringAlignmentCenter);
		format.SetTrimming(Gdiplus::StringTrimmingWord);
		const Gdiplus::RectF rect(static_cast<float>(x), static_cast<float>(y),
			static_cast<float>(width), static_cast<float>(height));
		g.DrawString(text.c_str(), -1, &font, rect, &format, &brush);
	}

	// A pill with a button's name in it, for hints about buttons the
	// bundled Xbox glyph can't stand in for (a PlayStation/Switch label, or
	// a rebound action). The rim follows the pad style so the hint still
	// reads as "this pad's button" at a glance.
	COLORREF ButtonStyleAccent(ButtonStyle style)
	{
		switch (style)
		{
		case ButtonStyle::DualShock4: return RGB(52, 108, 196);
		case ButtonStyle::Nintendo: return RGB(230, 60, 60);
		case ButtonStyle::Xbox: break;
		}
		return RGB(96, 180, 118);
	}

	void DrawButtonBadge(Gdiplus::Graphics& g, const std::wstring& label,
		float x, float y, float w, float h, float px)
	{
		const Gdiplus::RectF rect(x, y, w, h);
		Gdiplus::GraphicsPath path;
		AddRoundedRectPath(path, rect, h / 2.0f);

		Gdiplus::SolidBrush fill(Gdiplus::Color(226, 22, 27, 38));
		g.FillPath(&fill, &path);

		const COLORREF accent = ButtonStyleAccent(EffectiveButtonStyle());
		Gdiplus::Pen rim(ToGdiPlusColor(accent, 220), 2.0f);
		rim.SetLineJoin(Gdiplus::LineJoinRound);
		g.DrawPath(&rim, &path);

		Gdiplus::Font font = MakeUIFont(px);
		Gdiplus::SolidBrush text(Gdiplus::Color(255, 240, 244, 250));
		Gdiplus::StringFormat format(Gdiplus::StringFormatFlagsNoWrap);
		format.SetAlignment(Gdiplus::StringAlignmentCenter);
		format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
		g.DrawString(label.c_str(), -1, &font, rect, &format, &text);
	}

	// One button as the connected pad would print it: the bundled icon when
	// this style ships one, otherwise a badge with the button's name (which
	// is what keeps an incomplete icon set usable). Returns the width used,
	// and with `draw` false only measures, so callers can right-align a row
	// before committing to it.
	float DrawPadButton(Gdiplus::Graphics& g, size_t buttonIndex, float x, float y, float h, bool draw)
	{
		const ButtonStyle style = EffectiveButtonStyle();
		const int resId = ButtonIconResourceId(buttonIndex, style);
		if (Gdiplus::Bitmap* icon = GetAssetBitmap(resId))
		{
			const float w = h * icon->GetWidth() / static_cast<float>(icon->GetHeight());
			if (draw)
			{
				if (Gdiplus::Bitmap* scaled = RenderScaledAsset(
					resId, static_cast<int>(w), static_cast<int>(h), 0))
				{
					g.DrawImage(scaled, x, y);
				}
			}
			return w;
		}

		const float px = h * 0.62f;
		const std::wstring label = ButtonNameFor(kButtonNames[buttonIndex], style);
		const float w = MeasureTextWidth(g, label, px) + 20.0f;
		if (draw)
			DrawButtonBadge(g, label, x, y, w, h, px);
		return w;
	}

	// A whole mask: one button normally, several joined by "+" for a chord
	// like the overlay toggle.
	float DrawPadButtonMask(Gdiplus::Graphics& g, ButtonMask mask, float x, float y, float h, bool draw)
	{
		constexpr float kGap = 5.0f;
		float cursor = 0.0f;
		bool first = true;
		for (size_t index = 0; index < kButtonNames.size(); ++index)
		{
			if ((mask & kButtonNames[index].mask) == 0)
				continue;
			if (!first)
			{
				const float plusW = MeasureTextWidth(g, L"+", h * 0.62f);
				if (draw)
				{
					DrawTextLineCentered(g, L"+", static_cast<int>(x + cursor + kGap),
						static_cast<int>(y), static_cast<int>(plusW), RGB(206, 214, 228), static_cast<int>(h));
				}
				cursor += kGap + plusW + kGap;
			}
			cursor += DrawPadButton(g, index, x + cursor, y, h, draw);
			first = false;
		}
		return cursor;
	}

	std::filesystem::path GetExecutableDirectory()
	{
		std::array<wchar_t, 32768> path{};
		const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
		return length == 0 ? std::filesystem::current_path() : std::filesystem::path(path.data()).parent_path();
	}

	// Loads the embedded UI font (Compacta Regular) into memory - no temp file,
	// no system-wide install. Registers it with GDI so a normal HFONT can use it
	// by the resolved family name, and with GDI+ for DrawString text.
	void LoadUIFont()
	{
		DWORD size = 0;
		const uint8_t* bytes = GetResourceBytes(kUIFontResourceId, size);
		if (!bytes || size == 0)
			return;

		// GDI registration of the read-only resource payload.
		DWORD fontsAdded = 0;
		g_gdiFont = AddFontMemResourceEx(const_cast<uint8_t*>(bytes), size, nullptr, &fontsAdded);
		if (fontsAdded == 0)
			g_gdiFont = nullptr;

		// GDI+ registration of the same bytes.
		g_privateFonts.AddMemoryFont(bytes, static_cast<INT>(size));
		const INT familyCount = g_privateFonts.GetFamilyCount();
		if (familyCount > 0)
		{
			g_uiFontFamilyStorage = new Gdiplus::FontFamily[familyCount];
			INT found = 0;
			g_privateFonts.GetFamilies(familyCount, g_uiFontFamilyStorage, &found);
			if (found > 0)
			{
				g_uiFontFamily = &g_uiFontFamilyStorage[0];
				WCHAR name[LF_FACESIZE];
				g_uiFontFamily->GetFamilyName(name);
				g_uiFontFamilyName = name;
			}
		}

		if (!g_uiFontFamily)
		{
			// No usable family (missing/failed resource) - fall back gracefully.
			g_uiFontFamily = Gdiplus::FontFamily::GenericSansSerif();
			g_uiFontFamilyName = L"Segoe UI";
		}
	}

	void UnloadUIFont()
	{
		if (g_gdiFont)
		{
			RemoveFontMemResourceEx(g_gdiFont);
			g_gdiFont = nullptr;
		}
		if (g_uiFontFamilyStorage)
		{
			delete[] g_uiFontFamilyStorage;
			g_uiFontFamilyStorage = nullptr;
		}
		g_uiFontFamily = nullptr;
	}

	uint16_t ReadPort()
	{
		const auto iniPath = GetExecutableDirectory() / L"LegoToypad.ini";
		const UINT configuredPort = GetPrivateProfileIntW(L"Listener", L"Port", 9191, iniPath.c_str());
		return configuredPort >= 1 && configuredPort <= 65535 ? static_cast<uint16_t>(configuredPort) : 9191;
	}

	void EnsureDefaultIniExists()
	{
		const auto iniPath = GetExecutableDirectory() / L"LegoToypad.ini";
		std::error_code ec;
		if (std::filesystem::exists(iniPath, ec))
			return;

		std::ofstream file(iniPath, std::ios::binary);
		if (!file)
			return;

		file <<
			"[Listener]\n"
			"Port=9191\n"
			"\n"
			"; Optional. The app writes this section itself when you change the\n"
			"; shortcut from the in-app Settings screen, so you normally never need to\n"
			"; edit it by hand. Shown here for reference / manual tweaking.\n"
			"[Shortcut]\n"
			"; Type = Controller | Keyboard\n"
			"Type=Controller\n"
			"; Controller: raw XInput button bitmask (Back = 32), plus LT = 65536 and\n"
			"; RT = 131072 for the analog triggers. Combine buttons for a chord by\n"
			"; adding their values, e.g. LB (256) + RB (512) = 768.\n"
			"; Must include at least one button that isn't the D-pad or one of the\n"
			"; Button* bindings below, since those already drive the picker's menus.\n"
			"ControllerMask=32\n"
			"; Keyboard: modifier bitmask (Alt=1, Ctrl=2, Shift=4, Win=8) and a virtual-key\n"
			"; code. Must include at least one modifier, or the key would be stolen from\n"
			"; every other program while this app is running. Easiest to leave both at 0\n"
			"; and set the keyboard shortcut from the Settings screen instead of\n"
			"; computing VK codes by hand.\n"
			"KeyModifiers=0\n"
			"KeyCode=0\n"
			"\n"
			"[Input]\n"
			"; SwapConfirmBackButtons = 0 uses A to enter and B to go back (RPCS3 style).\n"
			"; SwapConfirmBackButtons = 1 uses B to enter and A to go back (Cemu style).\n"
			"SwapConfirmBackButtons=0\n"
"; BackgroundIndex selects the app background from bundled Assets/Wallpapers images.\n"
			"; The value one past the last wallpaper means No Background: the overlay\n"
			"; is drawn transparent between the UI elements so the game shows through.\n"
			"BackgroundIndex=0\n"
			"; StoryMode = 1 shows only the starter pack (Batman, Gandalf The Grey,\n"
			"; Wyldstyle, Batmobile) when picking a figure, skipping the series grid.\n"
			"StoryMode=0\n"
			"; Controller bindings for the picker's own actions, as raw XInput button\n"
			"; values (A=4096, B=8192, X=16384, Y=32768, LB=256, RB=512, LT=65536,\n"
			"; RT=131072, Start=16, Back=32, stick clicks=64/128). One button per\n"
			"; action; easiest to set them from the in-app Settings screen instead of\n"
			"; editing these by hand.\n"
			"ButtonConfirm=4096\n"
			"ButtonBack=8192\n"
			"ButtonSettings=32768\n"
			"ButtonMoveActive=16384\n"
			"ButtonQuickLoad=512\n"
			"ButtonQuickClear=256\n"
			"; ButtonStyle picks the button icons/names shown in the UI:\n"
			"; Auto | Xbox | DualShock4 | Switch. Auto follows the connected pad\n"
			"; (Xbox wins when several are plugged in). Icons only - every pad works\n"
			"; either way, and the values above stay raw XInput numbers.\n"
			"ButtonStyle=Auto\n"
			"\n"
			"[Web]\n"
			"; Web remote serves the same Toypad UI to a phone browser on your local\n"
			"; network so you never need the desktop overlay. Point the phone at the\n"
			"; address shown in Settings (or the tray's copy menu). Set Enabled=0 to\n"
			"; run fully offline - no sockets are opened then.\n"
			"Enabled=1\n"
			"; Port the phone connects to. Must be allowed inbound by Windows Firewall\n"
			"; (it is NOT the emulator listener port, that one lives under [Listener]).\n"
			"Port=8765\n";
	}

	// ---------------------------------------------------------------------
	// Shortcut description / persistence
	// ---------------------------------------------------------------------

	std::wstring DescribeControllerMask(ButtonMask mask)
	{
		if (mask == 0)
			return L"(none)";
		const ButtonStyle style = EffectiveButtonStyle();
		std::wstring result;
		for (const auto& entry : kButtonNames)
		{
			if (mask & entry.mask)
			{
				if (!result.empty())
					result += L" + ";
				result += ButtonNameFor(entry, style);
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

	// The confirm/back buttons the picker actually reacts to: the bound
	// buttons, swapped as a pair when the RPCS3/Cemu style toggle says so.
	ButtonMask EffectiveConfirmMask()
	{
		return g_app.swapConfirmBackButtons ? g_app.buttonBack : g_app.buttonConfirm;
	}

	ButtonMask EffectiveBackMask()
	{
		return g_app.swapConfirmBackButtons ? g_app.buttonConfirm : g_app.buttonBack;
	}

	std::wstring DescribeConfirmButtonMode()
	{
		const ButtonMask confirm = EffectiveConfirmMask();
		if (confirm == XINPUT_GAMEPAD_A)
			return L"A (RPCS3)";
		if (confirm == XINPUT_GAMEPAD_B)
			return L"B (Cemu)";
		return DescribeControllerMask(confirm);
	}

	std::wstring DescribeStoryMode()
	{
		return g_app.storyMode ? L"Story (starter pack only)" : L"All series";
	}

	std::wstring DescribeButtonStyle()
	{
		if (g_app.buttonStyleChoice != 0)
			return ButtonStyleName(EffectiveButtonStyle());
		if (!g_app.controllerConnected)
			return L"Auto (no controller, showing " + ButtonStyleName(g_app.detectedButtonStyle) + L")";
		return L"Auto (" + ButtonStyleName(g_app.detectedButtonStyle) + L")";
	}

	// Persisted as a name rather than an index so the file stays readable
	// and a new style in the middle of the list can't silently reinterpret
	// an existing setting.
	const wchar_t* ButtonStyleIniValue(size_t choice)
	{
		switch (choice)
		{
		case 1: return L"Xbox";
		case 2: return L"DualShock4";
		case 3: return L"Switch";
		default: break;
		}
		return L"Auto";
	}

	size_t ButtonStyleChoiceFromIni(const wchar_t* value)
	{
		for (size_t choice = 1; choice < kButtonStyleChoiceCount; ++choice)
		{
			if (_wcsicmp(value, ButtonStyleIniValue(choice)) == 0)
				return choice;
		}
		// A DualSense setting written by an earlier build maps onto the
		// PlayStation labels that replaced it.
		if (_wcsicmp(value, L"DualSense") == 0)
			return 2;
		return 0; // Auto, including anything unrecognized
	}

	// backgroundIndex == kBackgroundChoiceCount is the extra "No Background"
	// choice appended after the bundled wallpapers: nothing is drawn behind
	// the UI and those pixels become transparent (see ApplyOverlayTransparency).
	size_t ClampBackgroundIndex(size_t index)
	{
		return kBackgroundChoiceCount == 0 ? 0 : std::min(index, kBackgroundChoiceCount);
	}

	bool IsNoBackground()
	{
		return kBackgroundChoiceCount != 0 && ClampBackgroundIndex(g_app.backgroundIndex) == kBackgroundChoiceCount;
	}

	const BackgroundChoice* CurrentBackgroundChoice()
	{
		if (kBackgroundChoiceCount == 0)
			return nullptr;
		g_app.backgroundIndex = ClampBackgroundIndex(g_app.backgroundIndex);
		if (g_app.backgroundIndex == kBackgroundChoiceCount)
			return nullptr; // No Background
		return &kBackgroundChoices[g_app.backgroundIndex];
	}

	int CurrentBackgroundResourceId()
	{
		if (IsNoBackground())
			return 0;
		const BackgroundChoice* choice = CurrentBackgroundChoice();
		return choice ? choice->resourceId : kBackgroundResourceId;
	}

	std::wstring DescribeBackgroundChoice()
	{
		if (IsNoBackground())
			return L"No Background";
		const BackgroundChoice* choice = CurrentBackgroundChoice();
		return choice ? choice->name : L"(none)";
	}

	// The web remote always renders on a wallpaper - the phone browser has
	// no game behind it to show through - so "No Background" falls back to
	// the first bundled wallpaper there.
	int WebBackgroundResourceId()
	{
		const int resourceId = CurrentBackgroundResourceId();
		if (resourceId != 0)
			return resourceId;
		return kBackgroundChoiceCount != 0 ? kBackgroundChoices[0].resourceId : kBackgroundResourceId;
	}

	void SaveInputSettingsToIni()
	{
		const auto iniPath = GetExecutableDirectory() / L"LegoToypad.ini";
		WritePrivateProfileStringW(L"Input", L"SwapConfirmBackButtons",
			g_app.swapConfirmBackButtons ? L"1" : L"0", iniPath.c_str());
		WritePrivateProfileStringW(L"Input", L"BackgroundIndex",
			std::to_wstring(ClampBackgroundIndex(g_app.backgroundIndex)).c_str(), iniPath.c_str());
		WritePrivateProfileStringW(L"Input", L"StoryMode",
			g_app.storyMode ? L"1" : L"0", iniPath.c_str());
		WritePrivateProfileStringW(L"Input", L"ButtonStyle",
			ButtonStyleIniValue(g_app.buttonStyleChoice), iniPath.c_str());
		for (const auto& action : kBindableActions)
			WritePrivateProfileStringW(L"Input", action.iniKey,
				std::to_wstring(g_app.*(action.button)).c_str(), iniPath.c_str());
	}

	void LoadInputSettingsFromIni()
	{
		const auto iniPath = GetExecutableDirectory() / L"LegoToypad.ini";
		g_app.swapConfirmBackButtons =
			GetPrivateProfileIntW(L"Input", L"SwapConfirmBackButtons", 0, iniPath.c_str()) != 0;
		g_app.backgroundIndex = ClampBackgroundIndex(static_cast<size_t>(
			GetPrivateProfileIntW(L"Input", L"BackgroundIndex", 0, iniPath.c_str())));
		g_app.storyMode =
			GetPrivateProfileIntW(L"Input", L"StoryMode", 0, iniPath.c_str()) != 0;
		std::array<wchar_t, 32> styleBuffer{};
		GetPrivateProfileStringW(L"Input", L"ButtonStyle", L"Auto", styleBuffer.data(),
			static_cast<DWORD>(styleBuffer.size()), iniPath.c_str());
		g_app.buttonStyleChoice = ButtonStyleChoiceFromIni(styleBuffer.data());
		// A hand-edited value falls back to the built-in default when it is
		// unset (0) or a D-pad button, and is dropped entirely (leaving the
		// action unbound, shown as "(none)" in Settings) when it collides
		// with an action earlier in the list - one press firing two actions
		// is worse than one action the user can simply rebind.
		ButtonMask usedButtons = 0;
		for (const auto& action : kBindableActions)
		{
			const ButtonMask stored = static_cast<ButtonMask>(GetPrivateProfileIntW(
				L"Input", action.iniKey, g_app.*(action.button), iniPath.c_str()));
			ButtonMask value = (stored != 0 && (stored & kDpadButtons) == 0) ? stored : g_app.*(action.button);
			if (value & usedButtons)
				value = 0;
			usedButtons |= value;
			g_app.*(action.button) = value;
		}
	}

	void ToggleStoryMode()
	{
		g_app.storyMode = !g_app.storyMode;
		SaveInputSettingsToIni();
		g_app.status = L"Character selection: " + DescribeStoryMode();
	}

	void CycleButtonStyle()
	{
		g_app.buttonStyleChoice = (g_app.buttonStyleChoice + 1) % kButtonStyleChoiceCount;
		SaveInputSettingsToIni();
		g_app.status = L"Button labels: " + DescribeButtonStyle();
	}

	void ToggleConfirmButtonMode()
	{
		g_app.swapConfirmBackButtons = !g_app.swapConfirmBackButtons;
		SaveInputSettingsToIni();
		g_app.status = L"Confirm button: " + DescribeConfirmButtonMode();
	}

	// Uniform-alpha only, or alpha plus the transparent color key when the
	// "No Background" choice is active. Re-applied whenever the background
	// choice changes, so a wallpaper never gets key-color holes punched in
	// it and No Background always shows the game between UI elements.
	void ApplyOverlayTransparency(HWND window)
	{
		if (!window)
			return;
		if (IsNoBackground())
			SetLayeredWindowAttributes(window, kNoBackgroundColorKey, kOverlayAlpha, LWA_ALPHA | LWA_COLORKEY);
		else
			SetLayeredWindowAttributes(window, 0, kOverlayAlpha, LWA_ALPHA);
	}

	void CycleBackgroundChoice()
	{
		if (kBackgroundChoiceCount == 0)
		{
			g_app.status = L"No backgrounds are bundled in Assets/Wallpapers.";
			return;
		}
		// The cycle covers every bundled wallpaper plus the trailing
		// "No Background" choice.
		g_app.backgroundIndex = (ClampBackgroundIndex(g_app.backgroundIndex) + 1) % (kBackgroundChoiceCount + 1);
		SaveInputSettingsToIni();
		ApplyOverlayTransparency(g_mainWindow);
		g_app.status = L"Background: " + DescribeBackgroundChoice();
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
		g_app.shortcutControllerMask = static_cast<ButtonMask>(
			GetPrivateProfileIntW(L"Shortcut", L"ControllerMask", XINPUT_GAMEPAD_BACK, iniPath.c_str()));
		g_app.shortcutKeyModifiers = static_cast<UINT>(
			GetPrivateProfileIntW(L"Shortcut", L"KeyModifiers", 0, iniPath.c_str()));
		g_app.shortcutKeyCode = static_cast<UINT>(
			GetPrivateProfileIntW(L"Shortcut", L"KeyCode", 0, iniPath.c_str()));
	}

	// ---------------------------------------------------------------------
	// Web remote settings
	// ---------------------------------------------------------------------

	void SaveWebSettingsToIni()
	{
		const auto iniPath = GetExecutableDirectory() / L"LegoToypad.ini";
		WritePrivateProfileStringW(L"Web", L"Enabled", g_app.webEnabled ? L"1" : L"0", iniPath.c_str());
		WritePrivateProfileStringW(L"Web", L"Port",
			std::to_wstring(g_app.webPort).c_str(), iniPath.c_str());
	}

	void LoadWebSettingsFromIni()
	{
		const auto iniPath = GetExecutableDirectory() / L"LegoToypad.ini";
		g_app.webEnabled =
			GetPrivateProfileIntW(L"Web", L"Enabled", 1, iniPath.c_str()) != 0;
		const UINT configuredPort = GetPrivateProfileIntW(L"Web", L"Port", kWebDefaultPort, iniPath.c_str());
		g_app.webPort = configuredPort >= 1 && configuredPort <= 65535
			? static_cast<uint16_t>(configuredPort) : static_cast<uint16_t>(kWebDefaultPort);
	}

	std::wstring DescribeWebRemote()
	{
		if (!g_app.webEnabled)
			return L"Off";
		if (!g_app.webUrl.empty())
			return g_app.webUrl;
		return L"On (port " + std::to_wstring(g_app.webPort) + L")";
	}

	void ToggleWebRemote()
	{
		g_app.webEnabled = !g_app.webEnabled;
		if (g_app.webEnabled)
		{
			if (StartWebServer(g_mainWindow))
			{
				g_app.webUrl = GetLanAddress();
				if (g_app.webUrl.empty())
					g_app.webUrl = L"http://<this-pc-lan-ip>:" + std::to_wstring(g_app.webPort) + L"/";
				g_app.status = L"Web remote ON: " + g_app.webUrl;
			}
			else
			{
				g_app.webUrl.clear();
				g_app.status = L"Web remote could not start (port " +
					std::to_wstring(g_app.webPort) + L" busy or unavailable).";
			}
		}
		else
		{
			StopWebServer();
			g_app.webUrl.clear();
			g_app.status = L"Web remote OFF.";
		}
		SaveWebSettingsToIni();
		InvalidateRect(g_mainWindow, nullptr, FALSE);
	}

	// ---------------------------------------------------------------------
	// Wire protocol (cross-repo contract with Cemu's toypad listener).
	//
	// LOAD:   5-byte header (0x01, pad, index, 0x00, 0x00) + 180 tag bytes +
	//         2-byte little-endian path length + UTF-8 path bytes.
	// REMOVE: 5-byte header (0x02, pad, index, 0x00, 0x00).
	// MOVE:   5-byte header (0x03, destPad, destIndex, srcPad, srcIndex).
	//
	// The byte layout is unchanged from before. The one difference: tag data
	// now comes from embedded resources, and since there is no on-disk .bin
	// anymore the LOAD path is empty (length 0), so Cemu can't attach a
	// persistent FileStream back to a source file - game writes stay in
	// Cemu's memory for the session instead.
	// ---------------------------------------------------------------------

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

	std::wstring EntryDisplayName(const RosterEntry& entry)
	{
		if (entry.buildNumber > 1)
			return entry.name + L" \u00b7 Build " + std::to_wstring(entry.buildNumber);
		return entry.name;
	}

	bool SendLoadResourceToSlot(int binResourceId, size_t slotIndex, std::wstring& errorOut)
	{
		if (binResourceId == 0)
		{
			errorOut = L"Missing tag data for this entry.";
			return false;
		}

		const std::vector<uint8_t> tagData = LoadResourceBytes(binResourceId);
		if (tagData.size() != kTagSize)
		{
			errorOut = L"The embedded tag for this entry has the wrong size.";
			return false;
		}

		// Header + tag + 2-byte little-endian path length + path bytes.
		// The path is empty now (see the wire protocol note above).
		std::vector<uint8_t> message(5 + kTagSize + 2);
		message[0] = kLoadCommand;
		message[1] = kSlots[slotIndex].pad;
		message[2] = kSlots[slotIndex].index;
		std::copy(tagData.begin(), tagData.end(), message.begin() + 5);
		message[5 + kTagSize] = 0;
		message[5 + kTagSize + 1] = 0;

		return SendToypadMessage(message.data(), message.size(), errorOut);
	}

	bool SendMoveSlotToSlot(size_t sourceIndex, size_t destIndex, std::wstring& errorOut)
	{
		std::array<uint8_t, 5> message{};
		message[0] = kMoveCommand;
		message[1] = kSlots[destIndex].pad;
		message[2] = kSlots[destIndex].index;
		message[3] = kSlots[sourceIndex].pad;
		message[4] = kSlots[sourceIndex].index;

		return SendToypadMessage(message.data(), message.size(), errorOut);
	}

	bool SendClearSlot(size_t slotIndex, std::wstring& errorOut)
	{
		std::array<uint8_t, 5> message{}; // bytes 3-4 stay zero: reserved for LOAD/REMOVE
		message[0] = kRemoveCommand;
		message[1] = kSlots[slotIndex].pad;
		message[2] = kSlots[slotIndex].index;

		return SendToypadMessage(message.data(), message.size(), errorOut);
	}

	// Loads an entry onto a specific pad slot. With updateUi true the desktop
	// overlay also returns to the pad viewer front screen (controller flow);
	// the web remote passes false so it never yanks the desktop UI around.
	void LoadEntryToSlot(const RosterEntry& entry, size_t slotIndex, bool updateUi)
	{
		std::wstring error;
		if (!SendLoadResourceToSlot(entry.binResourceId, slotIndex, error))
		{
			g_app.status = error;
			return;
		}

		// The listener always clears the destination before loading, so a
		// direct Load action still intentionally overwrites that pad.
		const std::wstring name = EntryDisplayName(entry);
		auto& slot = g_app.padState[slotIndex];
		slot.occupied = true;
		slot.figureName = name;
		slot.binResourceId = entry.binResourceId;
		slot.portraitResourceId = entry.portraitResourceId;
		slot.ringColor = entry.ringColor;

		g_app.status = L"LOAD sent: " + name + L" -> " + kSlots[slotIndex].label;
		if (updateUi)
		{
			g_app.storyRosterActive = false;
			g_app.screen = Screen::PadViewer;
			InvalidateRect(g_mainWindow, nullptr, FALSE);
		}
	}

	// Sources the tag bytes from the embedded resource and sends the same
	// LOAD message as before, to whatever pad slot the user picked.
	void LoadRosterEntryToPad(const RosterEntry& entry)
	{
		LoadEntryToSlot(entry, g_app.slotIndex, true);
	}

	// Clears one pad slot in the emulator and the local bookkeeping. Does not
	// touch the desktop screen when called from the web remote.
	void ClearSlot(size_t slotIndex, bool updateUi)
	{
		std::wstring error;
		if (!SendClearSlot(slotIndex, error))
		{
			g_app.status = error;
			return;
		}

		g_app.padState[slotIndex] = PadSlot{};
		g_app.status = std::wstring(L"CLEAR sent: ") + kSlots[slotIndex].label;
		if (updateUi)
		{
			g_app.screen = Screen::PadViewer;
			InvalidateRect(g_mainWindow, nullptr, FALSE);
		}
	}

	void ClearSelectedPad()
	{
		ClearSlot(g_app.slotIndex, true);
	}

	void ClearAllPads(bool updateUi)
	{
		std::wstring error;
		size_t clearedCount = 0;
		for (size_t index = 0; index < kSlots.size(); ++index)
		{
			if (!SendClearSlot(index, error))
			{
				g_app.status = L"Clear all stopped after " + std::to_wstring(clearedCount) + L" pads: " + error;
				if (updateUi)
					g_app.screen = Screen::Settings;
				return;
			}

			g_app.padState[index] = PadSlot{};
			++clearedCount;
		}

		g_app.status = L"Clear all pad sent: all 7 slots cleared.";
		if (updateUi)
			g_app.screen = Screen::PadViewer;
	}

	// Moves whatever is tracked at moveSourceSlotIndex to destIndex. Called
	// once the user has picked a destination pad in the PadViewer's
	// move-destination mode.
	void MoveSlotToSlot(size_t sourceIndex, size_t destIndex, bool updateUi)
	{
		if (!g_app.padState[sourceIndex].occupied)
		{
			g_app.status = L"There is nothing tracked on that pad to move.";
			if (updateUi)
			{
				g_app.selectingMoveDestination = false;
				g_app.screen = Screen::PadViewer;
			}
			return;
		}

		std::wstring error;
		if (!SendMoveSlotToSlot(sourceIndex, destIndex, error))
		{
			g_app.status = error;
			return;
		}

		const PadSlot sourceSlot = g_app.padState[sourceIndex];
		const PadSlot destSlot = g_app.padState[destIndex];
		const std::wstring movedName = sourceSlot.figureName;
		if (destIndex != sourceIndex)
		{
			if (destSlot.occupied)
			{
				if (!SendLoadResourceToSlot(destSlot.binResourceId, sourceIndex, error))
				{
					g_app.padState[destIndex] = sourceSlot;
					g_app.padState[sourceIndex] = PadSlot{};
					g_app.status = L"MOVE sent, but swap reload failed: " + error;
					if (updateUi)
					{
						g_app.selectingMoveDestination = false;
						g_app.screen = Screen::PadViewer;
					}
					return;
				}

				g_app.padState[destIndex] = sourceSlot;
				g_app.padState[sourceIndex] = destSlot;
				g_app.status = L"SWAP sent: " + sourceSlot.figureName + L" <-> " + destSlot.figureName;
				if (updateUi)
				{
					g_app.selectingMoveDestination = false;
					g_app.screen = Screen::PadViewer;
				}
				return;
			}

			g_app.padState[destIndex] = sourceSlot;
			g_app.padState[sourceIndex] = PadSlot{};
		}

		if (destIndex == sourceIndex)
			g_app.status = L"REFRESH sent: " + movedName + L" on " + kSlots[destIndex].label;
		else
			g_app.status = L"MOVE sent: " + movedName + L" (" + kSlots[sourceIndex].label +
				L" -> " + kSlots[destIndex].label + L")";
		if (updateUi)
		{
			g_app.selectingMoveDestination = false;
			g_app.screen = Screen::PadViewer;
		}
	}

	void MoveToDestination(size_t destIndex)
	{
		MoveSlotToSlot(g_app.moveSourceSlotIndex, destIndex, true);
	}

	// One-press Move (the "Move active pad" binding, X by default): picks
	// up whatever is on the focused pad and goes straight to choosing the
	// destination, skipping the Load/Move/Clear action bar.
	void BeginMoveFromSelectedPad()
	{
		if (!g_app.padState[g_app.slotIndex].occupied)
		{
			g_app.status = L"There is nothing tracked on that pad to move.";
			return;
		}
		g_app.moveSourceSlotIndex = g_app.slotIndex;
		g_app.selectingMoveDestination = true;
		g_app.screen = Screen::PadViewer;
		g_app.status = L"Move: pick a destination pad.";
	}

	// ---------------------------------------------------------------------
	// Navigation
	// ---------------------------------------------------------------------

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

	// Directional (not flat-list) navigation for the pad viewer's 3/1/3
	// grid: 3 slots on the left (one upper, two lower side by side), a single
	// slot in the center, the same 3-slot cluster shape on the right.
	// Matches the real Toypad geometry in kSlots, not a generic row/column
	// grid, since the shape is irregular. -1 in any direction means "nothing
	// that way, stay put".
	struct PadNeighbors { int up, down, left, right; };
	constexpr std::array<PadNeighbors, 7> kPadNeighbors = {{
		/* 0 Left - upper         */ {-1,  3, -1,  1},
		/* 1 Center               */ {-1, -1,  0,  2},
		/* 2 Right - upper        */ {-1,  6,  1, -1},
		/* 3 Left - lower left    */ { 0, -1, -1,  4},
		/* 4 Left - lower right   */ { 0, -1,  3,  5},
		/* 5 Right - lower left   */ { 1, -1,  4,  6},
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

	// Franchise grid: 4 columns of larger tiles, wrapped vertically, with a
	// scrollable viewport (kFranchiseVisibleRows rows shown at a time).
	constexpr size_t kFranchiseCols = 4;
	constexpr size_t kFranchiseVisibleRows = 4;

	void MoveFranchiseSelection(int dx, int dy)
	{
		if (kFranchiseCount == 0)
			return;
		const size_t rows = (kFranchiseCount + kFranchiseCols - 1) / kFranchiseCols;
		size_t row = g_app.franchiseIndex / kFranchiseCols;
		size_t col = g_app.franchiseIndex % kFranchiseCols;

		row = (row + static_cast<size_t>(dy) + rows) % rows;
		const size_t lastCol = std::min(kFranchiseCols, kFranchiseCount - row * kFranchiseCols) - 1;
		col = (col + static_cast<size_t>(dx) + lastCol + 1) % (lastCol + 1);

		g_app.franchiseIndex = row * kFranchiseCols + col;

		// Keep the focused row inside the visible viewport.
		const int focusedRow = static_cast<int>(row);
		while (focusedRow < g_app.franchiseTopRow)
			--g_app.franchiseTopRow;
		while (focusedRow >= g_app.franchiseTopRow + static_cast<int>(kFranchiseVisibleRows))
			++g_app.franchiseTopRow;
		if (g_app.franchiseTopRow < 0)
			g_app.franchiseTopRow = 0;
	}

	// Roster grid: variable row count, wrapped vertically. The viewport shows
	// kRosterVisibleRows rows and follows the selection.
	constexpr size_t kRosterCols = 5;
	constexpr size_t kRosterVisibleRows = 3;
	constexpr int kRosterOriginX = 60;
	constexpr int kRosterOriginY = 120;
	constexpr int kRosterPitchX = 156;
	constexpr int kRosterLabelW = 132;
	constexpr int kRosterSeparatorW = 180;
	constexpr int kRosterSeparatorH = 6;
	constexpr int kRosterGridBottomLimit = kOverlayHeight - 28;

	struct RosterMetrics
	{
		int pitchY;
		int portraitDiameter;
		int labelH;
		int separatorTopGap;
		int vehicleGap;
	};

	constexpr RosterMetrics kRosterNormalMetrics{160, 94, 60, 16, 30};
	constexpr RosterMetrics kRosterCompactMetrics{148, 84, 56, 12, 24};

	size_t GetRosterCharacterCount()
	{
		size_t count = 0;
		while (count < g_app.rosterSlots.size() &&
			g_app.rosterSlots[count].kind == RosterSlot::Kind::Character)
		{
			++count;
		}
		return count;
	}

	size_t GetRosterCharacterRows()
	{
		const size_t characterCount = GetRosterCharacterCount();
		return (characterCount + kRosterCols - 1) / kRosterCols;
	}

	size_t GetRosterVisualRowCount()
	{
		if (g_app.rosterSlots.empty())
			return 0;
		const size_t characterCount = GetRosterCharacterCount();
		const size_t vehicleCount = g_app.rosterSlots.size() - characterCount;
		return ((characterCount + kRosterCols - 1) / kRosterCols) +
			((vehicleCount + kRosterCols - 1) / kRosterCols);
	}

	size_t GetRosterVisualRowItemCount(size_t row)
	{
		const size_t characterCount = GetRosterCharacterCount();
		const size_t characterRows = GetRosterCharacterRows();
		if (row < characterRows)
			return std::min(kRosterCols, characterCount - row * kRosterCols);

		const size_t vehicleCount = g_app.rosterSlots.size() - characterCount;
		const size_t vehicleRow = row - characterRows;
		return std::min(kRosterCols, vehicleCount - vehicleRow * kRosterCols);
	}

	struct RosterVisualPosition { size_t row, col; };

	RosterVisualPosition GetRosterVisualPosition(size_t index)
	{
		const size_t characterCount = GetRosterCharacterCount();
		if (index < characterCount)
			return {index / kRosterCols, index % kRosterCols};

		const size_t vehicleIndex = index - characterCount;
		return {GetRosterCharacterRows() + vehicleIndex / kRosterCols, vehicleIndex % kRosterCols};
	}

	size_t GetRosterIndexAtVisualPosition(size_t row, size_t col)
	{
		const size_t characterCount = GetRosterCharacterCount();
		const size_t characterRows = GetRosterCharacterRows();
		if (row < characterRows)
			return row * kRosterCols + col;

		const size_t vehicleIndex = (row - characterRows) * kRosterCols + col;
		return characterCount + vehicleIndex;
	}

	bool IsRosterSeparatorVisible()
	{
		const size_t characterCount = GetRosterCharacterCount();
		if (characterCount == 0 || characterCount >= g_app.rosterSlots.size())
			return false;

		const size_t vehicleRow = GetRosterCharacterRows();
		return vehicleRow > static_cast<size_t>(g_app.rosterTopRow) &&
			vehicleRow < static_cast<size_t>(g_app.rosterTopRow) + kRosterVisibleRows;
	}

	int MeasureWrappedTextHeight(Gdiplus::Graphics& g, const std::wstring& text, int width, int maxHeight)
	{
		return std::clamp(static_cast<int>(MeasureWrappedTextHeight(g, text, width, maxHeight, 22.0f)), 24, maxHeight);
	}

	int GetRosterSeparatorY(Gdiplus::Graphics& g, const RosterMetrics& metrics)
	{
		const size_t characterCount = GetRosterCharacterCount();
		const size_t lastCharacterRow = GetRosterCharacterRows() - 1;
		const size_t visibleRow = lastCharacterRow - static_cast<size_t>(g_app.rosterTopRow);
		const int rowY = kRosterOriginY + static_cast<int>(visibleRow) * metrics.pitchY;
		const int labelTop = rowY + metrics.portraitDiameter + 8;
		int maxLabelH = 24;

		const size_t rowStart = lastCharacterRow * kRosterCols;
		const size_t rowEnd = std::min(characterCount, rowStart + kRosterCols);
		for (size_t index = rowStart; index < rowEnd; ++index)
		{
			const RosterSlot& slot = g_app.rosterSlots[index];
			if (slot.entry)
				maxLabelH = std::max(maxLabelH, MeasureWrappedTextHeight(g, slot.entry->name, kRosterLabelW, metrics.labelH));
		}

		return labelTop + maxLabelH + metrics.separatorTopGap;
	}

	int GetRosterVehicleSectionOffset(Gdiplus::Graphics& g, const RosterMetrics& metrics)
	{
		if (!IsRosterSeparatorVisible())
			return 0;

		const size_t vehicleRow = GetRosterCharacterRows();
		const size_t visibleVehicleRow = vehicleRow - static_cast<size_t>(g_app.rosterTopRow);
		const int vehicleBaseY = kRosterOriginY + static_cast<int>(visibleVehicleRow) * metrics.pitchY;
		const int minVehicleY = GetRosterSeparatorY(g, metrics) + kRosterSeparatorH + metrics.vehicleGap;
		return std::max(0, minVehicleY - vehicleBaseY);
	}

	size_t GetRosterPaintRowCount()
	{
		const size_t totalRows = GetRosterVisualRowCount();
		const size_t topRow = static_cast<size_t>(std::max(0, g_app.rosterTopRow));
		if (topRow >= totalRows)
			return 0;
		return std::min(kRosterVisibleRows, totalRows - topRow);
	}

	int GetRosterContentBottom(Gdiplus::Graphics& g, const RosterMetrics& metrics)
	{
		const size_t paintRows = GetRosterPaintRowCount();
		if (paintRows == 0)
			return kRosterOriginY;

		const size_t firstRow = static_cast<size_t>(std::max(0, g_app.rosterTopRow));
		const size_t lastRow = firstRow + paintRows - 1;
		const size_t characterRows = GetRosterCharacterRows();
		const int vehicleSectionOffset = (lastRow >= characterRows && characterRows > firstRow)
			? GetRosterVehicleSectionOffset(g, metrics)
			: 0;
		return kRosterOriginY + static_cast<int>(paintRows - 1) * metrics.pitchY +
			vehicleSectionOffset + metrics.portraitDiameter + metrics.labelH;
	}

	RosterMetrics GetRosterMetrics(Gdiplus::Graphics& g)
	{
		const size_t characterCount = GetRosterCharacterCount();
		const bool onePage = GetRosterVisualRowCount() <= kRosterVisibleRows;
		const bool hasSeparator = characterCount > 0 && characterCount < g_app.rosterSlots.size();
		if (onePage && hasSeparator &&
			GetRosterContentBottom(g, kRosterNormalMetrics) > kRosterGridBottomLimit)
		{
			return kRosterCompactMetrics;
		}
		return kRosterNormalMetrics;
	}

	int GetRosterSeparatorY(Gdiplus::Graphics& g)
	{
		return GetRosterSeparatorY(g, GetRosterMetrics(g));
	}

	int GetRosterVehicleSectionOffset(Gdiplus::Graphics& g)
	{
		return GetRosterVehicleSectionOffset(g, GetRosterMetrics(g));
	}

	int GetRosterContentBottom(Gdiplus::Graphics& g)
	{
		return GetRosterContentBottom(g, GetRosterMetrics(g));
	}

	void MoveRosterSelection(int dx, int dy)
	{
		if (g_app.rosterSlots.empty())
			return;
		const size_t rows = GetRosterVisualRowCount();
		RosterVisualPosition pos = GetRosterVisualPosition(g_app.rosterIndex);

		if (dy != 0)
		{
			pos.row = (pos.row + static_cast<size_t>(dy) + rows) % rows;
			pos.col = std::min(pos.col, GetRosterVisualRowItemCount(pos.row) - 1);
		}
		if (dx != 0)
		{
			const size_t count = GetRosterVisualRowItemCount(pos.row);
			pos.col = (pos.col + static_cast<size_t>(dx) + count) % count;
		}

		g_app.rosterIndex = GetRosterIndexAtVisualPosition(pos.row, pos.col);

		// Keep the focused row inside the visible viewport.
		const int focusedRow = static_cast<int>(pos.row);
		while (focusedRow < g_app.rosterTopRow)
			--g_app.rosterTopRow;
		while (focusedRow >= g_app.rosterTopRow + static_cast<int>(kRosterVisibleRows))
			++g_app.rosterTopRow;
		if (g_app.rosterTopRow < 0)
			g_app.rosterTopRow = 0;
	}

	void NavigateGrid(int dx, int dy)
	{
		switch (g_app.screen)
		{
		case Screen::PadViewer:
			NavigatePadGrid(dx, dy);
			break;
		case Screen::FranchiseList:
			MoveFranchiseSelection(dx, dy);
			break;
		case Screen::RosterList:
			MoveRosterSelection(dx, dy);
			break;
		default:
			break;
		}
	}

	void Navigate(int direction)
	{
		switch (g_app.screen)
		{
		case Screen::PadViewer:
		case Screen::FranchiseList:
		case Screen::RosterList:
			NavigateGrid(0, direction);
			break;
		case Screen::PadAction:
			// The bottom bar is horizontal (Load | Move | Clear), so
			// left/right and up/down both cycle through them.
			if (direction < 0)
				SelectPrevious(kPadActionCount, g_app.padActionIndex);
			else
				SelectNext(kPadActionCount, g_app.padActionIndex);
			break;
		case Screen::PlusPicker:
			if (g_app.plusGroup && !g_app.plusGroup->builds.empty())
			{
				if (direction < 0)
					SelectPrevious(g_app.plusGroup->builds.size(), g_app.plusBuildIndex);
				else
					SelectNext(g_app.plusGroup->builds.size(), g_app.plusBuildIndex);
			}
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
	// Screen transitions
	// ---------------------------------------------------------------------

	void OpenFranchiseList()
	{
		g_app.franchiseIndex = 0;
		g_app.screen = Screen::FranchiseList;
	}

	void OpenRosterList()
	{
		g_app.rosterSlots.clear();
		const Franchise& franchise = kFranchises[g_app.franchiseIndex];
		for (const auto& character : franchise.characters)
			g_app.rosterSlots.push_back({RosterSlot::Kind::Character, &character, nullptr});
		for (const auto& vehicle : franchise.vehicles)
		{
			if (vehicle.builds.empty())
				continue;
			const RosterEntry* first = &vehicle.builds.front();
			g_app.rosterSlots.push_back({RosterSlot::Kind::Vehicle, first, &vehicle});
			if (vehicle.builds.size() > 1)
				g_app.rosterSlots.push_back({RosterSlot::Kind::Plus, nullptr, &vehicle});
		}
		g_app.rosterIndex = 0;
		g_app.rosterTopRow = 0;
		g_app.plusGroup = nullptr;
		g_app.storyRosterActive = false;
		g_app.screen = Screen::RosterList;
	}

	const RosterEntry* FindCharacterEntry(const wchar_t* franchiseName, const wchar_t* characterName)
	{
		for (size_t i = 0; i < kFranchiseCount; ++i)
		{
			if (kFranchises[i].name != franchiseName)
				continue;
			for (const auto& character : kFranchises[i].characters)
			{
				if (character.name == characterName)
					return &character;
			}
		}
		return nullptr;
	}

	const VehicleGroup* FindVehicleGroupEntry(const wchar_t* franchiseName, const wchar_t* baseName)
	{
		for (size_t i = 0; i < kFranchiseCount; ++i)
		{
			if (kFranchises[i].name != franchiseName)
				continue;
			for (const auto& vehicle : kFranchises[i].vehicles)
			{
				if (vehicle.baseName == baseName)
					return &vehicle;
			}
		}
		return nullptr;
	}

	// Story mode: the four starter-pack figures shown directly on the
	// roster screen, with no franchise grid in between - the exact cast the
	// game's story campaign starts you with.
	void OpenStoryRoster()
	{
		g_app.rosterSlots.clear();

		constexpr struct { const wchar_t* franchise; const wchar_t* name; } kStoryCharacters[] = {
			{L"DC Comics", L"Batman"},
			{L"Lord of the Rings", L"Gandalf The Grey"},
			{L"The LEGO Movie", L"Wyldstyle"},
		};
		for (const auto& character : kStoryCharacters)
		{
			if (const RosterEntry* entry = FindCharacterEntry(character.franchise, character.name))
				g_app.rosterSlots.push_back({RosterSlot::Kind::Character, entry, nullptr});
		}
		if (const VehicleGroup* batmobile = FindVehicleGroupEntry(L"DC Comics", L"Batmobile"))
		{
			if (!batmobile->builds.empty())
			{
				g_app.rosterSlots.push_back({RosterSlot::Kind::Vehicle, &batmobile->builds.front(), batmobile});
				if (batmobile->builds.size() > 1)
					g_app.rosterSlots.push_back({RosterSlot::Kind::Plus, nullptr, batmobile});
			}
		}

		g_app.rosterIndex = 0;
		g_app.rosterTopRow = 0;
		g_app.plusGroup = nullptr;
		g_app.storyRosterActive = true;
		g_app.screen = Screen::RosterList;
	}

	void OpenPlusPicker(const VehicleGroup& group)
	{
		g_app.plusGroup = &group;
		g_app.plusBuildIndex = 0;
		g_app.screen = Screen::PlusPicker;
	}

	void Confirm()
	{
		switch (g_app.screen)
		{
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
				if (g_app.storyMode)
					OpenStoryRoster();
				else
					OpenFranchiseList();
				break;
			case PadActionKind::Clear:
				ClearSelectedPad();
				break;
			case PadActionKind::Move:
				if (!g_app.padState[g_app.slotIndex].occupied)
				{
					g_app.status = L"There is nothing tracked on that pad to move.";
					g_app.screen = Screen::PadViewer;
					break;
				}
				g_app.moveSourceSlotIndex = g_app.slotIndex;
				g_app.selectingMoveDestination = true;
				g_app.screen = Screen::PadViewer;
				break;
			}
			break;
		case Screen::FranchiseList:
			OpenRosterList();
			break;
		case Screen::RosterList:
			if (g_app.rosterIndex >= g_app.rosterSlots.size())
				break;
			switch (g_app.rosterSlots[g_app.rosterIndex].kind)
			{
			case RosterSlot::Kind::Character:
			case RosterSlot::Kind::Vehicle:
				if (g_app.rosterSlots[g_app.rosterIndex].entry)
					LoadRosterEntryToPad(*g_app.rosterSlots[g_app.rosterIndex].entry);
				break;
			case RosterSlot::Kind::Plus:
				if (g_app.rosterSlots[g_app.rosterIndex].group)
					OpenPlusPicker(*g_app.rosterSlots[g_app.rosterIndex].group);
				break;
			}
			break;
		case Screen::PlusPicker:
			if (g_app.plusGroup && g_app.plusBuildIndex < g_app.plusGroup->builds.size())
				LoadRosterEntryToPad(g_app.plusGroup->builds[g_app.plusBuildIndex]);
			break;
case Screen::Settings:
			if (g_app.settingsIndex == 0)
				BeginShortcutCapture();
			else if (g_app.settingsIndex == 1)
				ToggleConfirmButtonMode();
			else if (g_app.settingsIndex == 2)
				CycleBackgroundChoice();
			else if (g_app.settingsIndex == 3)
				ToggleStoryMode();
			else if (g_app.settingsIndex == 4)
				CycleButtonStyle();
			else if (g_app.settingsIndex >= kSettingsBindingFirst &&
				g_app.settingsIndex < kSettingsBindingFirst + kBindableActions.size())
				BeginBindingCapture(g_app.settingsIndex - kSettingsBindingFirst);
			else if (g_app.settingsIndex == kSettingsItemCount - 3)
				ClearAllPads(true);
			else if (g_app.settingsIndex == kSettingsItemCount - 2)
				ToggleWebRemote();
			else if (g_app.settingsIndex == kSettingsItemCount - 1)
				ResetSettingsToDefaults();
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
		if (g_app.capturingBindingIndex >= 0)
		{
			CancelBindingCapture();
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
				HideOverlay(window);
			}
			break;
		case Screen::PadAction:
			g_app.hoveredPadActionIndex = -1;
			g_app.pressedPadActionIndex = -1;
			g_app.screen = Screen::PadViewer;
			break;
		case Screen::FranchiseList:
			g_app.screen = Screen::PadViewer;
			break;
		case Screen::RosterList:
			if (g_app.storyRosterActive)
			{
				// The story roster was opened straight from the pad viewer,
				// so Back skips the franchise grid on the way out too.
				g_app.storyRosterActive = false;
				g_app.screen = Screen::PadViewer;
			}
			else
			{
				g_app.screen = Screen::FranchiseList;
			}
			break;
		case Screen::PlusPicker:
			g_app.plusGroup = nullptr;
			if (g_app.storyRosterActive)
				OpenStoryRoster();
			else
				OpenRosterList();
			break;
		case Screen::Settings:
			g_app.screen = Screen::PadViewer;
			break;
		}
	}

	// ---------------------------------------------------------------------
	// Overlay show / hide (forward-declared above for the screen transitions)
	// ---------------------------------------------------------------------

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
		g_app.capturingBindingIndex = -1;
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

	void ApplyControllerShortcut(HWND window, ButtonMask mask)
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

	// ---------------------------------------------------------------------
	// Action binding capture (the per-action button rows in Settings)
	// ---------------------------------------------------------------------

	void BeginBindingCapture(size_t actionIndex)
	{
		g_app.capturingBindingIndex = static_cast<int>(actionIndex);
		g_app.shortcutCaptureArmed = false;
		g_app.status = std::wstring(L"Release all controller buttons, then press the new button for \"") +
			kBindableActions[actionIndex].label + L"\".";
	}

	void CancelBindingCapture()
	{
		g_app.capturingBindingIndex = -1;
		g_app.status = L"Button binding unchanged.";
	}

	// Everything the Settings screen can change goes back to the values a
	// fresh install starts with - including character selection back to All
	// series. A default-constructed AppState *is* the defaults, so this
	// can't drift away from the field initialisers.
	//
	// The web remote's port and the listener port are deliberately left
	// alone: they only exist in LegoToypad.ini, never in this screen.
	void ResetSettingsToDefaults()
	{
		const AppState defaults;

		if (g_app.webEnabled != defaults.webEnabled)
			ToggleWebRemote(); // starts/stops the server and saves [Web]

		g_app.shortcutType = defaults.shortcutType;
		g_app.shortcutControllerMask = defaults.shortcutControllerMask;
		g_app.shortcutKeyModifiers = defaults.shortcutKeyModifiers;
		g_app.shortcutKeyCode = defaults.shortcutKeyCode;
		g_app.swapConfirmBackButtons = defaults.swapConfirmBackButtons;
		g_app.backgroundIndex = defaults.backgroundIndex;
		g_app.storyMode = defaults.storyMode;
		g_app.buttonStyleChoice = defaults.buttonStyleChoice;
		for (const auto& action : kBindableActions)
			g_app.*(action.button) = defaults.*(action.button);

		SaveShortcutToIni();
		SaveInputSettingsToIni();
		RegisterToggleHotkeyIfNeeded(g_mainWindow);
		ApplyOverlayTransparency(g_mainWindow);
		g_app.status = L"Settings reset to defaults. Toggle: " + DescribeShortcut() + L".";
	}

	// Assigns a captured single button to the action being rebound. The
	// D-pad, the current toggle shortcut and buttons already used by another
	// action are rejected with an explanation instead of silently creating
	// two actions that fire from one press.
	void ApplyBinding(ButtonMask button)
	{
		if (g_app.capturingBindingIndex < 0 ||
			static_cast<size_t>(g_app.capturingBindingIndex) >= kBindableActions.size())
			return;
		const size_t actionIndex = static_cast<size_t>(g_app.capturingBindingIndex);

		if (button & kDpadButtons)
		{
			g_app.status = L"The D-pad is reserved for menu navigation. Pick another button.";
			return;
		}
		if (g_app.shortcutType == ShortcutType::Controller && button == g_app.shortcutControllerMask)
		{
			g_app.status = L"That button already toggles the overlay. Pick another button.";
			return;
		}
		for (size_t other = 0; other < kBindableActions.size(); ++other)
		{
			if (other != actionIndex && g_app.*(kBindableActions[other].button) == button)
			{
				g_app.status = std::wstring(L"Already used by \"") + kBindableActions[other].label +
					L"\". Pick another button.";
				return;
			}
		}

		g_app.*(kBindableActions[actionIndex].button) = button;
		g_app.capturingBindingIndex = -1;
		SaveInputSettingsToIni();
		g_app.status = std::wstring(kBindableActions[actionIndex].label) + L" is now " +
			DescribeControllerMask(button) + L".";
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
		icon.hIcon = static_cast<HICON>(LoadImageW(
			GetModuleHandleW(nullptr),
			MAKEINTRESOURCEW(IDI_APP_ICON),
			IMAGE_ICON,
			GetSystemMetrics(SM_CXSMICON),
			GetSystemMetrics(SM_CYSMICON),
			LR_DEFAULTCOLOR | LR_SHARED));
		if (!icon.hIcon)
		{
			icon.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON));
		}
		const wchar_t* tip = L"LEGO Dimensions Toypad Picker";
		wcsncpy(icon.szTip, tip, std::size(icon.szTip) - 1);
		icon.szTip[std::size(icon.szTip) - 1] = L'\0';
		Shell_NotifyIconW(NIM_ADD, &icon);

		// One-time launch toast letting the user know the picker is live and
		// how to open it (NIM_MODIFY with NIF_INFO on the same icon id).
		NOTIFYICONDATAW note{};
		note.cbSize = sizeof(note);
		note.hWnd = window;
		note.uID = kTrayIconId;
		note.uFlags = NIF_INFO;
		note.dwInfoFlags = NIIF_INFO;
		const wchar_t* noteTitle = L"LEGO Dimensions Toypad Picker";
		wcsncpy(note.szInfoTitle, noteTitle, std::size(note.szInfoTitle) - 1);
		note.szInfoTitle[std::size(note.szInfoTitle) - 1] = L'\0';
		const wchar_t* noteText = L"LegoToypad is running in the background. Use the toggle shortcut to show the picker.";
		wcsncpy(note.szInfo, noteText, std::size(note.szInfo) - 1);
		note.szInfo[std::size(note.szInfo) - 1] = L'\0';
		Shell_NotifyIconW(NIM_MODIFY, &note);
	}

	void RemoveTrayIcon(HWND window)
	{
		NOTIFYICONDATAW icon{};
		icon.cbSize = sizeof(icon);
		icon.hWnd = window;
		icon.uID = kTrayIconId;
		Shell_NotifyIconW(NIM_DELETE, &icon);
	}

	void CopyTextToClipboard(const std::wstring& text)
	{
		if (!OpenClipboard(nullptr))
			return;
		EmptyClipboard();
		const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
		if (HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, bytes))
		{
			if (void* dest = GlobalLock(handle))
			{
				std::memcpy(dest, text.c_str(), bytes);
				GlobalUnlock(handle);
				SetClipboardData(CF_UNICODETEXT, handle);
			}
		}
		CloseClipboard();
	}

	void ShowTrayMenu(HWND window)
	{
		POINT cursor{};
		GetCursorPos(&cursor);
		HMENU menu = CreatePopupMenu();
		AppendMenuW(menu, MF_STRING, kMenuIdToggle, g_app.overlayVisible ? L"Hide overlay" : L"Show overlay");
		AppendMenuW(menu, MF_STRING, kMenuIdSettings, L"Shortcut settings");
		AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
		AppendMenuW(menu, MF_STRING, kMenuIdWebAddress, g_app.webEnabled ? L"Copy web remote address" : L"Web remote address (disabled)");
		AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
		AppendMenuW(menu, MF_STRING, kMenuIdExit, L"Exit");
		// Required so the popup menu dismisses correctly when clicking away.
		ForceForegroundWindow(window);
		TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, window, nullptr);
		DestroyMenu(menu);
	}

	// ---------------------------------------------------------------------
	// Painting
	// ---------------------------------------------------------------------

	// Cell rectangles for the 7 pad slots, matching the real 3/1/3 (left /
	// center / right) Toypad geometry in kSlots: the left section (pad 2) is
	// the upper slot plus its two lower slots, the center pad (pad 1) is a
	// single slot, and the right section (pad 3) mirrors the left. Sized for
	// the fixed kOverlayWidth x kOverlayHeight window (this app isn't resizable).
	//
	// Positions are measured off the reference layout and mirrored left/right
	// around the window's horizontal center (450) so the two sides line up.
	// The paired lower pads keep a small gutter between them instead of
	// touching, which preserves the separate glass-tile read.
	constexpr std::array<RECT, 7> kPadCells = {{
		{100, 220, 240, 356}, // 0 Left - upper        (140x136)
		{356, 103, 544, 287}, // 1 Center               (188x184)
		{660, 220, 800, 356}, // 2 Right - upper        (140x136)
		{100, 364, 236, 500}, // 3 Left - lower left
		{244, 364, 408, 500}, // 4 Left - lower right
		{492, 364, 656, 500}, // 5 Right - lower left
		{664, 364, 800, 500}, // 6 Right - lower right
	}};

	// Franchise grid layout: 4 columns of large logo tiles, scrolled vertically.
	constexpr int kFranchiseOriginX = 40;
	constexpr int kFranchiseOriginY = 106;
	constexpr int kFranchisePitchX = 210;
	constexpr int kFranchisePitchY = 108;
	constexpr int kFranchiseTileW = 190;
	constexpr int kFranchiseTileH = 100;
	constexpr int kTileGlowMargin = 6;

	// Roster grid layout lives with the roster navigation constants above,
	// because navigation and painting both need the same visual row model.

	// Right-edge vertical scroll bar (Scroll_Bar.png) shown on scrollable
	// lists when their content overflows the visible rows. Kept inside the
	// overlay's right margin, clear of the grid panels.
	constexpr int kScrollBarW = 14;
	constexpr int kScrollBarMarginX = 18;
	constexpr int kScrollBarMinThumbH = 22;

	// Capsule (pill) menus.
	constexpr int kPillWidth = 340;
	constexpr int kPillRowH = 36;
	constexpr int kPillPadV = 7;

	// The glossy pad itself, plus a large centered occupant portrait when
	// loaded. An empty pad is just the bare glass tile - no placeholder
	// dot, no label - matching the reference design. No name text is
	// drawn for occupied pads either; the portrait alone is the label.
	void DrawPad(Gdiplus::Graphics& g, HDC dc, size_t index)
	{
		const RECT& cell = kPadCells[index];
		const bool selected = index == g_app.slotIndex;
		const bool occupied = g_app.padState[index].occupied;
		const bool isMoveSource = g_app.selectingMoveDestination && index == g_app.moveSourceSlotIndex;

		PadVisual visual;
		if (isMoveSource)
			visual = PadVisual::MoveSource;
		else if (selected && occupied)
			visual = PadVisual::OccupiedSelected;
		else if (selected)
			visual = PadVisual::IdleSelected;
		else if (occupied)
			visual = PadVisual::Occupied;
		else
			visual = PadVisual::Idle;

		const unsigned int occupantColor = occupied ? g_app.padState[index].ringColor : 0;
		Gdiplus::Bitmap* pad = RenderPad(static_cast<int>(index), cell, visual, occupantColor);
		if (pad)
			g.DrawImage(pad, static_cast<int>(cell.left) - kPadGlowMargin, static_cast<int>(cell.top) - kPadGlowMargin);

		if (!occupied)
			return; // Empty pad: bare glass tile only, nothing drawn on top of it.

		const int cellWidth = cell.right - cell.left;
		const int cellHeight = cell.bottom - cell.top;
		const PadSlot& slot = g_app.padState[index];

		// Portrait fills most of the pad's face, centered both ways, since
		// there is no name label underneath competing for vertical space
		// anymore.
		const int shorter = std::min(cellWidth, cellHeight);
		const int kOccupantDiameter = std::clamp(static_cast<int>(shorter * 0.80f), 84, 148);
		const int circleX = cell.left + (cellWidth - kOccupantDiameter) / 2;
		const int circleY = cell.top + (cellHeight - kOccupantDiameter) / 2;

		Gdiplus::Bitmap* portrait = RenderPortrait(slot.portraitResourceId, slot.ringColor, false, kOccupantDiameter);
		if (portrait)
			g.DrawImage(portrait, circleX - kPortraitMargin, circleY - kPortraitMargin);
	}

	// The focused occupied pad's occupant name, as muted translucent text
	// just outside the pad's edge, vertically centered on the pad. Upper and
	// center pads have free space on their outer side, so the label lives
	// there; the four flush lower pads leave no room beside them, so their
	// label sits below the pad row, clear of the side-by-side tiles (and of
	// the action bar, which also floats below them).
	void DrawOccupantLabel(Gdiplus::Graphics& g, size_t index)
	{
		const PadSlot& slot = g_app.padState[index];
		if (!slot.occupied || slot.figureName.empty())
			return;

		const RECT& cell = kPadCells[index];
		constexpr int kPadLabelGap = 14;
		constexpr int labelH = 28;
		const int cellHeightPx = cell.bottom - cell.top;
		int labelX = 0;
		int labelW = 0;
		int labelY = 0;
		bool alignFar = false;
		if (index >= 3)
		{
			// Below the pad row: center on the pad, spanning clear of the
			// action bar that floats beneath these pads.
			labelW = 260;
			labelX = cell.left + (cell.right - cell.left - labelW) / 2;
			labelY = cell.bottom + 52;
		}
		else
		{
			// Side placement: right of the left/center pads, left of the
			// rightmost pad, vertically centered on the pad.
			const bool labelOnRight = index != 2;
			labelY = cell.top + (cellHeightPx - labelH) / 2;
			alignFar = !labelOnRight;
			if (labelOnRight)
			{
				labelX = cell.right + kPadLabelGap;
				labelW = kOverlayWidth - labelX - 14;
			}
			else
			{
				labelW = cell.left - kPadLabelGap - 14;
				labelX = 14;
			}
		}

		static Gdiplus::Font s_labelFont(g_uiFontFamily, 22.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
		Gdiplus::StringFormat format(Gdiplus::StringFormatFlagsNoClip | Gdiplus::StringFormatFlagsNoWrap);
		format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
		format.SetAlignment(alignFar ? Gdiplus::StringAlignmentFar : Gdiplus::StringAlignmentNear);
		Gdiplus::SolidBrush labelBrush(Gdiplus::Color(150, 235, 240, 245));
		g.DrawString(slot.figureName.c_str(), -1, &s_labelFont,
			Gdiplus::RectF(static_cast<float>(labelX), static_cast<float>(labelY),
				static_cast<float>(labelW), static_cast<float>(labelH)),
			&format, &labelBrush);
	}

	// Capsule menu floating above the selected pad (PadAction) or centered
	// (PlusPicker). The focused row is drawn in its capsule highlight.
	void DrawPillMenu(Gdiplus::Graphics& g, HDC dc, int x, int y, size_t rows,
		const std::vector<std::wstring>& options, size_t focusedIndex)
	{
		Gdiplus::Bitmap* pill = RenderPill(static_cast<int>(rows));
		if (!pill)
			return;
		constexpr int glowMargin = 8;
		g.DrawImage(pill, x - glowMargin, y - glowMargin);

		Gdiplus::Bitmap* highlight = nullptr;
		for (size_t row = 0; row < rows; ++row)
		{
			if (row == focusedIndex)
			{
				if (!highlight)
					highlight = RenderPillRow(true);
				if (highlight)
					g.DrawImage(highlight, x + 8, y + kPillPadV + static_cast<int>(row) * kPillRowH);
			}
			DrawTextLineCentered(g, options[row], x + 8, y + kPillPadV + static_cast<int>(row) * kPillRowH,
				kPillWidth - 16, row == focusedIndex ? RGB(255, 255, 255) : RGB(196, 204, 216), kPillRowH);
		}
	}

	// Three floating circular image buttons for the selected pad's actions.
	// Upper side pads tuck the row against the pad's lower edge so the buttons
	// sit in the gap before the lower pads; center/lower pads place the row
	// just beneath the selected piece of glass.
	constexpr int kActionButtonSize = 42;
	constexpr int kUpperPadActionButtonSize = 38;
	constexpr int kUpperLeftPadActionButtonSize = 36;
	constexpr int kActionButtonGap = 4;
	constexpr int kUpperLeftPadActionButtonGap = 3;
	constexpr int kActionButtonFocusedGrow = 4;

	int ResourceIdForAction(PadActionKind action)
	{
		switch (action)
		{
		case PadActionKind::Load:
			return kLoadButtonResourceId;
		case PadActionKind::Move:
			return kMoveButtonResourceId;
		case PadActionKind::Clear:
			return kClearButtonResourceId;
		}
		return 0;
	}

	bool IsUpperSidePad(size_t slotIndex)
	{
		return slotIndex == 0 || slotIndex == 2;
	}

	int ActionButtonSizeForSlot(size_t slotIndex)
	{
		if (slotIndex == 0)
			return kUpperLeftPadActionButtonSize;
		return IsUpperSidePad(slotIndex) ? kUpperPadActionButtonSize : kActionButtonSize;
	}

	int ActionButtonGapForSlot(size_t slotIndex)
	{
		return slotIndex == 0 ? kUpperLeftPadActionButtonGap : kActionButtonGap;
	}

	RECT GetActionButtonRect(size_t slotIndex, size_t actionIndex)
	{
		const RECT& cell = kPadCells[slotIndex];
		const int cellW = cell.right - cell.left;
		const int buttonSize = ActionButtonSizeForSlot(slotIndex);
		const int gap = ActionButtonGapForSlot(slotIndex);
		const int rowW = static_cast<int>(kPadActionCount) * buttonSize +
			static_cast<int>(kPadActionCount - 1) * gap;
		const int rowX = cell.left + (cellW - rowW) / 2;
		int rowY = cell.bottom + 4;
		if (IsUpperSidePad(slotIndex))
			rowY = cell.bottom - buttonSize - 8;

		const int x = rowX + static_cast<int>(actionIndex) * (buttonSize + gap);
		return {x, rowY, x + buttonSize, rowY + buttonSize};
	}

	int ActionButtonIndexAt(POINT point)
	{
		if (g_app.screen != Screen::PadAction)
			return -1;

		for (size_t index = 0; index < kPadActionCount; ++index)
		{
			const RECT rect = GetActionButtonRect(g_app.slotIndex, index);
			if (!PtInRect(&rect, point))
				continue;

			const int cx = (rect.left + rect.right) / 2;
			const int cy = (rect.top + rect.bottom) / 2;
			const int dx = point.x - cx;
			const int dy = point.y - cy;
			const int radius = ActionButtonSizeForSlot(g_app.slotIndex) / 2;
			if (dx * dx + dy * dy <= radius * radius)
				return static_cast<int>(index);
		}
		return -1;
	}

	void DrawActionButtons(Gdiplus::Graphics& g)
	{
		for (size_t index = 0; index < kPadActionCount; ++index)
		{
			const PadActionKind action = static_cast<PadActionKind>(index);
			const bool pressed = static_cast<int>(index) == g_app.pressedPadActionIndex;
			const bool hovered = static_cast<int>(index) == g_app.hoveredPadActionIndex;
			const bool focused = index == g_app.padActionIndex;
			const int grow = focused || hovered || pressed ? kActionButtonFocusedGrow : 0;
			const RECT rect = GetActionButtonRect(g_app.slotIndex, index);
			const int x = rect.left - grow / 2;
			const int y = rect.top - grow / 2;
			const int size = ActionButtonSizeForSlot(g_app.slotIndex) + grow;

			if (focused || hovered || pressed)
			{
				const BYTE alpha = pressed ? 105 : (hovered ? 82 : 64);
				Gdiplus::SolidBrush halo(Gdiplus::Color(alpha, 228, 238, 255));
				g.FillEllipse(&halo, x - 2, y - 2, size + 4, size + 4);
			}

			if (Gdiplus::Bitmap* button = RenderScaledAsset(ResourceIdForAction(action), size, size, 0))
				g.DrawImage(button, x, y);
		}
	}

	// Plus-picker: the vehicle's builds as a row of portrait circles (colored
	// ring + name below), plus a trailing "+" glyph - the same visual
	// language as the franchise roster grid, on a shared characters_tile
	// panel. The franchise logo is drawn above this in the screen painter.
	void DrawPlusPickerMenu(Gdiplus::Graphics& g, HDC dc)
	{
		if (!g_app.plusGroup || g_app.plusGroup->builds.empty())
			return;

		const auto& builds = g_app.plusGroup->builds;
		const size_t count = builds.size();

		constexpr int diam = 90;
		constexpr int step = 156;
		constexpr int gapToPlus = 40;
		constexpr int circleY = 160;
		constexpr int labelH = 58;

		const int groupW = static_cast<int>(count - 1) * step + diam + gapToPlus + diam;
		const int groupX = (kOverlayWidth - groupW) / 2;

		// Large translucent panel (characters_tile art) behind the circles.
		constexpr int panelXPad = 24;
		constexpr int panelTopPad = 20;
		const int panelX = groupX - panelXPad;
		const int panelY = circleY - panelTopPad;
		const int panelH = panelTopPad + diam + 8 + labelH + 16;
		const int panelW = groupW + panelXPad * 2;
		if (Gdiplus::Bitmap* panel = RenderScaledAsset(
			kCharactersTileResourceId, panelW, panelH, 16))
		{
			g.DrawImage(panel, panelX, panelY);
		}

		for (size_t i = 0; i < count; ++i)
		{
			const RosterEntry& build = builds[i];
			const int x = groupX + static_cast<int>(i) * step;
			const bool focused = i == g_app.plusBuildIndex;
			Gdiplus::Bitmap* visual = build.portraitResourceId != 0
				? RenderPortrait(build.portraitResourceId, build.ringColor, focused, diam)
				: RenderPlaceholder(build.name.empty() ? L'?' : build.name[0], build.ringColor, focused, diam);
			if (visual)
				g.DrawImage(visual, x - kPortraitMargin, circleY - kPortraitMargin);
			DrawTextWrappedCentered(g, EntryDisplayName(build), x - 16, circleY + diam + 8, step, labelH,
				focused ? RGB(255, 236, 190) : RGB(214, 220, 230));
		}

		// Trailing "+" glyph (non-interactive), matching the roster marker.
		const int plusX = groupX + static_cast<int>(count - 1) * step + diam + gapToPlus;
		Gdiplus::Bitmap* plus = RenderPlusTile(builds[0].ringColor, false, diam);
		if (plus)
			g.DrawImage(plus, plusX - kPortraitMargin, circleY - kPortraitMargin);
	}

	// Right-edge scroll indicator for vertically scrollable grids. The track
	// spans tractTop..trackBottom; the thumb (Scroll_Bar.png) is sized to the
	// visible fraction and slides with the scroll offset. Hidden when the
	// content fits in the viewport.
	void DrawScrollBar(Gdiplus::Graphics& g, int trackTop, int trackBottom,
		size_t totalRows, size_t visibleRows, int topRow)
	{
		if (totalRows <= visibleRows)
			return;
		const int trackH = trackBottom - trackTop;
		int thumbH = static_cast<int>(trackH * static_cast<double>(visibleRows) / totalRows);
		if (thumbH < kScrollBarMinThumbH)
			thumbH = kScrollBarMinThumbH;
		const double frac = topRow / static_cast<double>(totalRows - visibleRows);
		const int thumbY = trackTop + static_cast<int>((trackH - thumbH) * frac);

		Gdiplus::Bitmap* bar = GetAssetBitmap(kScrollBarResourceId);
		if (!bar)
			return;
		const int x = kOverlayWidth - kScrollBarW - kScrollBarMarginX;
		g.DrawImage(bar, static_cast<float>(x), static_cast<float>(thumbY),
			static_cast<float>(kScrollBarW), static_cast<float>(thumbH));
	}

	void DrawFranchiseGrid(Gdiplus::Graphics& g, HDC dc)
	{
		const size_t totalRows = (kFranchiseCount + kFranchiseCols - 1) / kFranchiseCols;
		for (size_t row = 0; row < kFranchiseVisibleRows; ++row)
		{
			for (size_t col = 0; col < kFranchiseCols; ++col)
			{
				const size_t index =
					(static_cast<size_t>(g_app.franchiseTopRow) + row) * kFranchiseCols + col;
				if (index >= kFranchiseCount)
					break;
				const int x = kFranchiseOriginX + static_cast<int>(col) * kFranchisePitchX;
				const int y = kFranchiseOriginY + static_cast<int>(row) * kFranchisePitchY;
				const bool focused = index == g_app.franchiseIndex;
				Gdiplus::Bitmap* tile = RenderFranchiseTile(kFranchises[index].logoResourceId, focused);
				if (tile)
					g.DrawImage(tile, x - kTileGlowMargin, y - kTileGlowMargin);
			}
		}

		const int trackTop = kFranchiseOriginY;
		const int trackBottom = kFranchiseOriginY
			+ static_cast<int>(kFranchiseVisibleRows - 1) * kFranchisePitchY + kFranchiseTileH;
		DrawScrollBar(g, trackTop, trackBottom, totalRows, kFranchiseVisibleRows, g_app.franchiseTopRow);
	}

	void DrawRoundedSeparator(Gdiplus::Graphics& g, int x, int y, int width, int height)
	{
		Gdiplus::GraphicsPath path;
		AddRoundedRectPath(path,
			Gdiplus::RectF(static_cast<float>(x), static_cast<float>(y),
				static_cast<float>(width), static_cast<float>(height)),
			3.0f);
		Gdiplus::LinearGradientBrush brush(
			Gdiplus::RectF(static_cast<float>(x), static_cast<float>(y),
				static_cast<float>(width), static_cast<float>(height)),
			Gdiplus::Color(225, 255, 255, 255), Gdiplus::Color(135, 255, 255, 255),
			Gdiplus::LinearGradientModeVertical);
		g.FillPath(&brush, &path);
	}

	void DrawRosterCategorySeparator(Gdiplus::Graphics& g, HDC dc, const RosterMetrics& metrics)
	{
		if (!IsRosterSeparatorVisible())
			return;

		const int gridW = static_cast<int>(kRosterCols) * kRosterPitchX;
		const int x = kRosterOriginX + (gridW - kRosterSeparatorW) / 2;
		const int y = GetRosterSeparatorY(g, metrics);
		DrawRoundedSeparator(g, x, y, kRosterSeparatorW, kRosterSeparatorH);
	}

	void DrawRosterGrid(Gdiplus::Graphics& g, HDC dc)
	{
		if (g_app.rosterSlots.empty())
		{
			DrawTextLine(g, L"No figures in this world.", 24, 200, 480, RGB(224, 230, 237));
			return;
		}

		const size_t visibleRows = GetRosterPaintRowCount();
		const size_t totalRows = GetRosterVisualRowCount();
		const size_t characterRows = GetRosterCharacterRows();
		const RosterMetrics metrics = GetRosterMetrics(g);
		const int vehicleSectionOffset = GetRosterVehicleSectionOffset(g, metrics);
		DrawRosterCategorySeparator(g, dc, metrics);
		for (size_t row = 0; row < visibleRows; ++row)
		{
			for (size_t col = 0; col < kRosterCols; ++col)
			{
				const size_t visualRow = static_cast<size_t>(g_app.rosterTopRow) + row;
				if (visualRow >= totalRows || col >= GetRosterVisualRowItemCount(visualRow))
					break;
				const size_t slotIndex = GetRosterIndexAtVisualPosition(visualRow, col);
				const RosterSlot& slot = g_app.rosterSlots[slotIndex];
				const int x = kRosterOriginX + static_cast<int>(col) * kRosterPitchX;
				int y = kRosterOriginY + static_cast<int>(row) * metrics.pitchY;
				if (visualRow >= characterRows && characterRows > static_cast<size_t>(g_app.rosterTopRow))
					y += vehicleSectionOffset;
				const bool focused = slotIndex == g_app.rosterIndex;

				Gdiplus::Bitmap* visual = nullptr;
				std::wstring label;
				unsigned int color = 0;
				int resId = 0;
				if (slot.kind == RosterSlot::Kind::Plus)
				{
					label = L"+";
					color = slot.group && !slot.group->builds.empty() ? slot.group->builds[0].ringColor : kPadBorderIdle;
					visual = RenderPlusTile(color, focused, metrics.portraitDiameter);
				}
				else if (slot.entry)
				{
					label = slot.entry->name;
					color = slot.entry->ringColor;
					resId = slot.entry->portraitResourceId;
					visual = resId != 0
						? RenderPortrait(resId, color, focused, metrics.portraitDiameter)
						: RenderPlaceholder(label.empty() ? L'?' : label[0], color, focused, metrics.portraitDiameter);
				}

				const int circleX = x + (kRosterPitchX - metrics.portraitDiameter) / 2;
				const int circleY = y;
				if (visual)
					g.DrawImage(visual, circleX - kPortraitMargin, circleY - kPortraitMargin);

				const int labelX = x + (kRosterPitchX - kRosterLabelW) / 2;
				DrawTextWrappedCenteredFit(g, label, labelX, y + metrics.portraitDiameter + 8, kRosterLabelW, metrics.labelH,
					focused ? RGB(255, 236, 190) : RGB(214, 220, 230));
			}
		}

		const int trackTop = kRosterOriginY - 14;
		const int trackBottom = GetRosterContentBottom(g, metrics);
		DrawScrollBar(g, trackTop, trackBottom, totalRows, kRosterVisibleRows, g_app.rosterTopRow);
	}

	void Paint(HWND window)
	{
		// Top-left wordmark and the "by harrysof" marker (top-right) share a
		// single horizontal band so they sit inline, inset 20px from the edges.
		constexpr float kTopMargin = 20.0f;
		constexpr float kTopBarH = 90.0f;

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
		// off-screen and blitting it atomically avoids that.
		//
		// The buffer (and the Compacta HFONT) are created once and reused
		// across frames; rebuilding them on every paint and font each time
		// was measurable churn on the ~30-60fps animation path.
		static HDC s_bufferDC = nullptr;
		static HBITMAP s_bufferBitmap = nullptr;
		static HGDIOBJ s_bufferOldBitmap = nullptr;
		static int s_bufferW = 0;
		static int s_bufferH = 0;
		if (!s_bufferDC || s_bufferW != width || s_bufferH != height)
		{
			if (s_bufferDC)
			{
				SelectObject(s_bufferDC, s_bufferOldBitmap);
				DeleteObject(s_bufferBitmap);
				DeleteDC(s_bufferDC);
			}
			s_bufferDC = CreateCompatibleDC(windowDC);
			s_bufferBitmap = CreateCompatibleBitmap(windowDC, width, height);
			s_bufferOldBitmap = SelectObject(s_bufferDC, s_bufferBitmap);
			s_bufferW = width;
			s_bufferH = height;
		}
		HDC dc = s_bufferDC;

		Gdiplus::Graphics g(dc);
		g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
		g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

		Gdiplus::Bitmap* background = RenderBackground(width, height);
		if (background)
			g.DrawImage(background, 0, 0);

		SetBkMode(dc, TRANSPARENT);
		static HFONT s_uiFont = nullptr;
		if (!s_uiFont)
			s_uiFont = CreateFontW(-22, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
			OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, g_uiFontFamilyName.c_str());
		SelectObject(dc, s_uiFont);

		// Persistent wordmark in the top-left of every screen — enlarged.
		{
			Gdiplus::Bitmap* wordmark = GetAssetBitmap(kWordmarkResourceId);
			if (wordmark)
			{
				const Gdiplus::RectF box(kTopMargin, kTopMargin, 460.0f, kTopBarH);
				const float scale = std::min(box.Width / wordmark->GetWidth(), box.Height / wordmark->GetHeight());
				const float drawW = wordmark->GetWidth() * scale;
				const float drawH = wordmark->GetHeight() * scale;
				const Gdiplus::RectF dest(box.X, box.Y + (box.Height - drawH) / 2.0f, drawW, drawH);
				g.DrawImage(wordmark, dest);
			}
		}

		switch (g_app.screen)
		{
		case Screen::PadViewer:
			for (size_t index = 0; index < kPadCells.size(); ++index)
				DrawPad(g, dc, index);
			break;
		case Screen::PadAction:
			for (size_t index = 0; index < kPadCells.size(); ++index)
				DrawPad(g, dc, index);
			DrawActionButtons(g);
			break;
		case Screen::FranchiseList:
			DrawFranchiseGrid(g, dc);
			break;
case Screen::RosterList:
		{
			// The current world's logo at top-center, above the roster grid.
			// The story roster spans several worlds, so it has no logo header.
			if (!g_app.storyRosterActive)
			{
				const Gdiplus::RectF box((width - 360.0f) / 2.0f, 24.0f, 360.0f, 62.0f);
				Gdiplus::Bitmap* worldLogo = GetAssetBitmap(kFranchises[g_app.franchiseIndex].logoResourceId);
				if (worldLogo)
				{
					const float scale = std::min(box.Width / worldLogo->GetWidth(), box.Height / worldLogo->GetHeight());
					const int drawW = static_cast<int>(worldLogo->GetWidth() * scale);
					const int drawH = static_cast<int>(worldLogo->GetHeight() * scale);
					if (Gdiplus::Bitmap* logo = RenderScaledAsset(
						kFranchises[g_app.franchiseIndex].logoResourceId, drawW, drawH, 0))
					{
						g.DrawImage(logo, box.X + (box.Width - drawW) / 2.0f,
							box.Y + (box.Height - drawH) / 2.0f);
					}
				}
			}

			// characters_tile.png: one large translucent panel behind the
			// whole portrait grid, spanning the roster grid's bounding box
			// plus a margin on each side. Drawn before the grid so the
			// portraits sit on top of it.
			const int gridRight = kRosterOriginX + static_cast<int>(kRosterCols) * kRosterPitchX;
			const int gridBottom = GetRosterContentBottom(g);
			const int panelX = kRosterOriginX - 16;
			const int panelY = kRosterOriginY - 14;
			const Gdiplus::RectF panelRect(static_cast<float>(panelX), static_cast<float>(panelY),
				static_cast<float>(gridRight - panelX + 16), static_cast<float>(gridBottom - panelY + 14));
			if (Gdiplus::Bitmap* panel = RenderScaledAsset(
				kCharactersTileResourceId, static_cast<int>(panelRect.Width), static_cast<int>(panelRect.Height), 14))
			{
				g.DrawImage(panel, panelX, panelY);
			}
			DrawRosterGrid(g, dc);
			break;
		}
		case Screen::PlusPicker:
		{
			// The current world's logo above the builds panel, the same
			// header the roster screen (this screen's parent) uses. Skipped
			// when the parent is the story roster, which has no single world.
			if (!g_app.storyRosterActive)
			{
				const Gdiplus::RectF box((width - 360.0f) / 2.0f, 8.0f, 360.0f, 62.0f);
				Gdiplus::Bitmap* worldLogo = GetAssetBitmap(kFranchises[g_app.franchiseIndex].logoResourceId);
				if (worldLogo)
				{
					const float scale = std::min(box.Width / worldLogo->GetWidth(), box.Height / worldLogo->GetHeight());
					const int drawW = static_cast<int>(worldLogo->GetWidth() * scale);
					const int drawH = static_cast<int>(worldLogo->GetHeight() * scale);
					if (Gdiplus::Bitmap* logo = RenderScaledAsset(
						kFranchises[g_app.franchiseIndex].logoResourceId, drawW, drawH, 0))
					{
						g.DrawImage(logo, box.X + (box.Width - drawW) / 2.0f,
							box.Y + (box.Height - drawH) / 2.0f);
					}
				}
			}
			DrawPlusPickerMenu(g, dc);
			break;
		}
		case Screen::Settings:
		{
			// A row is text, optionally followed by the pad icons for a
			// button mask - that is how a binding shows the button itself
			// rather than spelling its name out.
			struct SettingsRow
			{
				std::wstring text;
				ButtonMask icons = 0;
			};
			std::vector<SettingsRow> rows;
			rows.reserve(kSettingsItemCount);
			if (g_app.shortcutType == ShortcutType::Controller)
				rows.push_back({L"Toggle shortcut: ", g_app.shortcutControllerMask});
			else
				rows.push_back({L"Toggle shortcut: " + DescribeShortcut(), 0});
			rows.push_back({L"Confirm button: " + DescribeConfirmButtonMode(), 0});
			rows.push_back({L"Background: " + DescribeBackgroundChoice(), 0});
			rows.push_back({L"Character selection: " + DescribeStoryMode(), 0});
			rows.push_back({L"Button labels: " + DescribeButtonStyle(), 0});
			for (const auto& action : kBindableActions)
				rows.push_back({std::wstring(L"Button - ") + action.label + L": ",
					g_app.*(action.button)});
			rows.push_back({L"Clear all pad", 0});
			rows.push_back({L"Web remote: " + DescribeWebRemote(), 0});
			rows.push_back({L"Reset all settings to defaults", 0});

			// 33px pitch (down from the original 40) so the grown list plus
			// the capture hint and status line below still fit the
			// fixed-height overlay.
			constexpr int kSettingsTop = 100;
			constexpr int kSettingsPitch = 33;
			constexpr float kSettingsIconH = 30.0f;
			for (size_t index = 0; index < rows.size(); ++index)
			{
				const int y = kSettingsTop + static_cast<int>(index) * kSettingsPitch;
				const bool selected = index == g_app.settingsIndex;
				if (selected)
				{
					RECT selection{18, y - 2, width - 18, y + 30};
					HBRUSH brush = CreateSolidBrush(RGB(36, 99, 170));
					FillRect(dc, &selection, brush);
					DeleteObject(brush);
				}
				DrawTextLine(g, rows[index].text, 30, y, width - 60,
					selected ? RGB(255, 255, 255) : RGB(228, 232, 238));
				if (rows[index].icons != 0)
				{
					const float iconX = 30.0f + MeasureTextWidth(g, rows[index].text, 22.0f);
					DrawPadButtonMask(g, rows[index].icons, iconX,
						y + (30.0f - kSettingsIconH) / 2.0f, kSettingsIconH, true);
				}
			}

			// While a button is being captured the status line carries the
			// whole conversation - what to press, and why a press was
			// refused - so it is drawn under the list. Outside capture the
			// rows already show every setting's value, and a leftover
			// message there would just be noise.
			if ((g_app.capturingShortcut || g_app.capturingBindingIndex >= 0) && !g_app.status.empty())
			{
				const int hintY = kSettingsTop + static_cast<int>(rows.size()) * kSettingsPitch + 10;
				DrawTextLine(g, g_app.status, 24, hintY, width - 48, RGB(255, 204, 51), 26);
			}
			break;
		}
		}

		// The focused occupied pad's occupant name, drawn after the pads and
		// action bar so it stays readable on all pad screens.
		if (g_app.screen == Screen::PadViewer || g_app.screen == Screen::PadAction)
			DrawOccupantLabel(g, g_app.slotIndex);

		SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));

		// "by harrysof" credit marker in the top-right of every screen, vertically
		// centered on the same band as the wordmark so the two sit inline.
		{
			Gdiplus::Bitmap* mark = GetAssetBitmap(kByHarrysofResourceId);
			if (mark)
			{
				constexpr float kMarkH = 30.0f;
				const float scale = kMarkH / mark->GetHeight();
				const float drawW = mark->GetWidth() * scale;
				const float drawH = kMarkH;
				const float markY = kTopMargin + (kTopBarH - drawH) / 2.0f;
				if (Gdiplus::Bitmap* cached = RenderScaledAsset(
					kByHarrysofResourceId, static_cast<int>(drawW), static_cast<int>(drawH), 0))
				{
					g.DrawImage(cached, width - drawW - kTopMargin, markY);
				}
			}
		}

		// Settings hint in the bottom-right corner: the icon of whatever
		// button currently opens Settings, in the connected pad's own
		// style, next to the "settings" label.
		if (g_app.screen == Screen::PadViewer)
		{
			constexpr float kHintBottomInset = 18.0f;
			constexpr float kHintGap = 8.0f;
			constexpr float kHintYButtonH = 44.0f;
			constexpr float kHintTextH = 40.0f;
			const float hintCenterY = height - kHintBottomInset - kHintTextH / 2.0f;

			Gdiplus::Bitmap* settingsText = GetAssetBitmap(kSettingsTextResourceId);
			if (settingsText)
			{
				const float buttonW = DrawPadButtonMask(g, g_app.buttonSettings, 0.0f, 0.0f,
					kHintYButtonH, false);
				const float textW = settingsText->GetWidth() * (kHintTextH / settingsText->GetHeight());
				const float totalW = buttonW + kHintGap + textW;
				const float originX = width - kTopMargin - totalW;
				const float textX = originX + buttonW + kHintGap;

				DrawPadButtonMask(g, g_app.buttonSettings, originX,
					hintCenterY - kHintYButtonH / 2.0f, kHintYButtonH, true);
				if (Gdiplus::Bitmap* cachedText = RenderScaledAsset(
					kSettingsTextResourceId, static_cast<int>(textW), static_cast<int>(kHintTextH), 0))
				{
					g.DrawImage(cachedText, textX, hintCenterY - kHintTextH / 2.0f);
				}
			}
		}

		// Nothing in the picker can be driven without a pad, so an empty
		// controller list is called out over whatever screen is up. Drawn
		// last so it sits on top of everything, on a plate because "No
		// Background" leaves the game itself behind the text.
		if (!g_app.controllerConnected)
		{
			const std::wstring warning = L"Connect a controller!";
			constexpr float kWarningPx = 38.0f;
			const float warningW = MeasureTextWidth(g, warning, kWarningPx);
			const Gdiplus::RectF plate((width - warningW) / 2.0f - 26.0f,
				height / 2.0f - 32.0f, warningW + 52.0f, 64.0f);
			Gdiplus::GraphicsPath platePath;
			AddRoundedRectPath(platePath, plate, 14.0f);
			Gdiplus::SolidBrush plateFill(Gdiplus::Color(170, 8, 10, 16));
			g.FillPath(&plateFill, &platePath);

			Gdiplus::Font warningFont = MakeUIFont(kWarningPx);
			Gdiplus::SolidBrush warningBrush(ToGdiPlusColor(RGB(236, 64, 64)));
			Gdiplus::StringFormat warningFormat(Gdiplus::StringFormatFlagsNoWrap);
			warningFormat.SetAlignment(Gdiplus::StringAlignmentCenter);
			warningFormat.SetLineAlignment(Gdiplus::StringAlignmentCenter);
			g.DrawString(warning.c_str(), -1, &warningFont, plate, &warningFormat, &warningBrush);
		}

		BitBlt(windowDC, 0, 0, width, height, dc, 0, 0, SRCCOPY);

		EndPaint(window, &paint);
	}

	// ---------------------------------------------------------------------
	// Input polling
	// ---------------------------------------------------------------------

	// SDL knows what each pad actually is, so Auto never has to guess from
	// button counts or names. Anything unrecognized (generic HID pads, the
	// virtual pads some remote-play tools expose) reads as Xbox, which is
	// also what SDL's own default mapping is modelled on.
	ButtonStyle StyleForController(SDL_GameController* controller)
	{
		switch (SDL_GameControllerGetType(controller))
		{
		// A DualSense reads as DualShock 4 here: the two label sets only
		// differ in Create vs Share, and the app ships one PlayStation
		// icon set rather than two nearly identical ones.
		case SDL_CONTROLLER_TYPE_PS5:
		case SDL_CONTROLLER_TYPE_PS4:
		case SDL_CONTROLLER_TYPE_PS3:
			return ButtonStyle::DualShock4;
		case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
#if SDL_VERSION_ATLEAST(2, 24, 0)
		case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
		case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
		case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
#endif
			return ButtonStyle::Nintendo;
		default:
			break;
		}
		return ButtonStyle::Xbox;
	}

	// With several pads plugged in at once the labels have to pick one, so
	// they follow this order. Xbox first because its labels are the ones
	// every other pad's owner can still read (and the app's own art is an
	// Xbox glyph); the rest simply fall in a stable order after it.
	int ButtonStylePriority(ButtonStyle style)
	{
		switch (style)
		{
		case ButtonStyle::Xbox: return 0;
		case ButtonStyle::DualShock4: return 1;
		case ButtonStyle::Nintendo: return 2;
		}
		return 3;
	}

void PollController(HWND window)
	{
		// Controllers are tracked per SDL joystick instance id and opened /
		// closed from SDL_CONTROLLERDEVICEADDED / REMOVED events drained
		// below. SDL delivers hotplug notifications through SDL_PollEvent,
		// which is a cheap no-op when nothing changed - replacing XInput's
		// slow empty-slot enumeration and its 2s probe backoff entirely.
		struct ControllerState
		{
			SDL_GameController* handle = nullptr;
			ButtonMask previousButtons = 0;
			ButtonStyle style = ButtonStyle::Xbox; // resolved once, when opened
		};
		static std::unordered_map<SDL_JoystickID, ControllerState> controllers;
		static ButtonMask previousCombinedButtons = 0;
		static DWORD lastNavigation = 0;

		SDL_Event event;
		while (SDL_PollEvent(&event))
		{
			if (event.type == SDL_CONTROLLERDEVICEADDED)
			{
				// which = joystick device index, only valid this tick.
				SDL_GameController* controller = SDL_GameControllerOpen(event.cdevice.which);
				if (!controller)
					continue;
				// Every connected controller is read, not just the first:
				// Cemu itself may be reading one pad for gameplay, so this
				// app deliberately does not assume it owns any slot.
				const SDL_JoystickID instanceId =
					SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller));
				if (controllers.contains(instanceId))
				{
					SDL_GameControllerClose(controller);
					continue;
				}
				controllers[instanceId] =
					ControllerState{controller, 0, StyleForController(controller)};
			}
			else if (event.type == SDL_CONTROLLERDEVICEREMOVED)
			{
				// which = joystick instance id on removal.
				const auto it = controllers.find(event.cdevice.which);
				if (it != controllers.end())
				{
					SDL_GameControllerClose(it->second.handle);
					controllers.erase(it);
				}
			}
		}

		UpdateInputOwnership(window);

		bool anyConnected = false;
		ButtonStyle bestStyle = ButtonStyle::Xbox;
		int bestStylePriority = ButtonStylePriority(ButtonStyle::Nintendo) + 1;
		ButtonMask combinedButtons = 0;
		ButtonMask combinedPressed = 0;
		bool stickUp = false;
		bool stickDown = false;
		bool stickLeft = false;
		bool stickRight = false;

		for (auto& [deviceId, state] : controllers)
		{
			(void)deviceId;
			SDL_GameController* controller = state.handle;
			ButtonMask buttons = 0;
			if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_UP))
				buttons |= XINPUT_GAMEPAD_DPAD_UP;
			if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN))
				buttons |= XINPUT_GAMEPAD_DPAD_DOWN;
			if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT))
				buttons |= XINPUT_GAMEPAD_DPAD_LEFT;
			if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT))
				buttons |= XINPUT_GAMEPAD_DPAD_RIGHT;
			if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_START))
				buttons |= XINPUT_GAMEPAD_START;
			if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_BACK))
				buttons |= XINPUT_GAMEPAD_BACK;
			if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_LEFTSTICK))
				buttons |= XINPUT_GAMEPAD_LEFT_THUMB;
			if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_RIGHTSTICK))
				buttons |= XINPUT_GAMEPAD_RIGHT_THUMB;
			if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_LEFTSHOULDER))
				buttons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
			if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER))
				buttons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
			// XInput.h exports no constant for the guide bit; keep the
			// documented XINPUT_GAMEPAD_GUIDE value (0x0400) as plain data.
			if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_GUIDE))
				buttons |= 0x0400;
			if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_A))
				buttons |= XINPUT_GAMEPAD_A;
			if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_B))
				buttons |= XINPUT_GAMEPAD_B;
			if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_X))
				buttons |= XINPUT_GAMEPAD_X;
			if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_Y))
				buttons |= XINPUT_GAMEPAD_Y;
			// The triggers are axes, not buttons, so they are thresholded
			// into their own mask bits here - otherwise LT/RT could never be
			// bound to anything, or trigger a shortcut.
			if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > kTriggerPressThreshold)
				buttons |= kTriggerLeftButton;
			if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > kTriggerPressThreshold)
				buttons |= kTriggerRightButton;

			// SDL2 reports stick Y negative = up / positive = down, inverted
			// relative to XInput's sThumbLY; the 7849 deadzone threshold
			// itself matches XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE and is kept.
			const int16_t leftX = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX);
			const int16_t leftY = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY);

			anyConnected = true;
			if (ButtonStylePriority(state.style) < bestStylePriority)
			{
				bestStylePriority = ButtonStylePriority(state.style);
				bestStyle = state.style;
			}
			combinedButtons |= buttons;
			combinedPressed |= buttons & ~state.previousButtons;
			if ((buttons & XINPUT_GAMEPAD_DPAD_UP) || leftY < -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE)
				stickUp = true;
			if ((buttons & XINPUT_GAMEPAD_DPAD_DOWN) || leftY > XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE)
				stickDown = true;
			if ((buttons & XINPUT_GAMEPAD_DPAD_LEFT) || leftX < -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE)
				stickLeft = true;
			if ((buttons & XINPUT_GAMEPAD_DPAD_RIGHT) || leftX > XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE)
				stickRight = true;
			state.previousButtons = buttons;
		}
		const ButtonMask previousCombined = previousCombinedButtons;
		previousCombinedButtons = combinedButtons;

		// Live controller facts drive the "Connect a controller!" warning and
		// Auto button labels, so they are refreshed before any of the early
		// returns below - including the one taken when nothing is connected,
		// which is exactly when the warning has to appear.
		if (g_app.controllerConnected != anyConnected ||
			(anyConnected && g_app.detectedButtonStyle != bestStyle))
		{
			g_app.controllerConnected = anyConnected;
			if (anyConnected)
				g_app.detectedButtonStyle = bestStyle;
			if (g_app.overlayVisible)
				InvalidateRect(window, nullptr, FALSE);
		}

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
				{
					g_app.shortcutCaptureArmed = true;
					g_app.status = L"Listening: press a controller combo, or a keyboard shortcut. " +
						DescribeControllerMask(kShortcutCancelChord) + L" or Esc cancels.";
					InvalidateRect(window, nullptr, FALSE);
				}
				return;
			}

			if (anyConnected && combinedButtons != 0 && combinedPressed != 0)
			{
				if (combinedButtons == kShortcutCancelChord)
				{
					CancelShortcutCapture();
				}
				else if ((combinedButtons & ~ReservedNavigationMask()) == 0)
				{
					g_app.status = L"That combo is only buttons the picker already uses. Add one it "
						L"doesn't (e.g. Start or a stick click). Back+Start cancels.";
				}
				else
				{
					ApplyControllerShortcut(window, combinedButtons);
				}
				InvalidateRect(window, nullptr, FALSE);
			}
			return;
		}

		// Action-binding capture mirrors shortcut capture (arm on full
		// release, Back+Start cancels), but takes a single button: the first
		// newly pressed one this tick.
		if (g_app.capturingBindingIndex >= 0)
		{
			if (!g_app.shortcutCaptureArmed)
			{
				if (combinedButtons == 0)
				{
					g_app.shortcutCaptureArmed = true;
					g_app.status = std::wstring(L"Listening: press the new button for \"") +
						kBindableActions[static_cast<size_t>(g_app.capturingBindingIndex)].label +
						L"\" - any button or trigger except the D-pad. " +
						DescribeControllerMask(kShortcutCancelChord) + L" or Esc cancels.";
					InvalidateRect(window, nullptr, FALSE);
				}
				return;
			}

			if (anyConnected && combinedPressed != 0)
			{
				if (combinedButtons == kShortcutCancelChord)
				{
					CancelBindingCapture();
				}
				else
				{
					// Lowest set bit of the newly pressed buttons: bindings
					// are single buttons, so a simultaneous multi-press just
					// takes the lowest-valued one.
					const ButtonMask firstPressed = static_cast<ButtonMask>(combinedPressed & (0u - static_cast<unsigned>(combinedPressed)));
					ApplyBinding(firstPressed);
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

		// Menu navigation only applies while the overlay is actually visible.
		// Foreground focus is deliberately NOT required here: the overlay is
		// an always-on-top controller picker, Cemu (or the game) almost always
		// holds foreground and Windows games aggressively re-grab focus, so
		// gating on focus was silently swallowing whole button presses. Input
		// exclusivity is handled by UpdateInputOwnership (visibility-based).
		if (!g_app.overlayVisible || IsIconic(window) || !anyConnected)
			return;

		// Track whether anything actually changed this tick so the repaint
		// below only fires when needed, instead of unconditionally on every
		// ~16ms timer tick regardless of input. The changed flag keeps the
		// ~60Hz repaint path exactly as cheap as the old bitmap draws.
		bool changed = false;

		const ButtonMask confirmButton = EffectiveConfirmMask();
		const ButtonMask backButton = EffectiveBackMask();

		if (combinedPressed & confirmButton)
		{
			Confirm();
			changed = true;
		}
		if (combinedPressed & backButton)
		{
			Back(window);
			changed = true;
		}
		if ((combinedPressed & g_app.buttonSettings) && g_app.screen == Screen::PadViewer)
		{
			g_app.screen = Screen::Settings;
			changed = true;
		}

		// One-press pad shortcuts, all acting on the focused (active) pad
		// cell of the pad viewer. Suppressed while a Move destination is
		// being picked so they can't yank that flow out from underneath.
		if (g_app.screen == Screen::PadViewer && !g_app.selectingMoveDestination)
		{
			if (combinedPressed & g_app.buttonMoveActive)
			{
				// X by default: pick the cell up for moving right away.
				BeginMoveFromSelectedPad();
				changed = true;
			}
			if (combinedPressed & g_app.buttonQuickLoad)
			{
				// RB by default: straight into figure selection for this cell.
				if (g_app.storyMode)
					OpenStoryRoster();
				else
					OpenFranchiseList();
				changed = true;
			}
			if (combinedPressed & g_app.buttonQuickClear)
			{
				// LB by default: remove whatever is on this cell.
				if (g_app.padState[g_app.slotIndex].occupied)
					ClearSlot(g_app.slotIndex, true);
				else
					g_app.status = L"There is nothing tracked on that pad to clear.";
				changed = true;
			}
		}

		const bool gridScreen = g_app.screen == Screen::PadViewer ||
			g_app.screen == Screen::FranchiseList || g_app.screen == Screen::RosterList;
		const DWORD now = GetTickCount();

		// D-pad edge presses move immediately instead of waiting out the
		// hold-repeat throttle, so quick taps are never swallowed. Holding a
		// direction (stick or D-pad held down) repeats at a fixed cadence.
		constexpr int kNavRepeatMs = 150;
		const int dxEdge = (combinedPressed & XINPUT_GAMEPAD_DPAD_RIGHT ? 1 : 0)
			- (combinedPressed & XINPUT_GAMEPAD_DPAD_LEFT ? 1 : 0);
		const int dyEdge = (combinedPressed & XINPUT_GAMEPAD_DPAD_UP ? -1 : 0)
			- (combinedPressed & XINPUT_GAMEPAD_DPAD_DOWN ? -1 : 0);

		const bool anyDirection = stickUp || stickDown || stickLeft || stickRight;
		bool navigated = false;
		if (dxEdge != 0 || dyEdge != 0)
		{
			if (gridScreen)
				NavigateGrid(dxEdge, dyEdge);
			else if (dxEdge != 0)
				Navigate(dxEdge);
			else
				Navigate(dyEdge);
			navigated = true;
		}
		else if (anyDirection && now - lastNavigation >= kNavRepeatMs)
		{
			if (gridScreen)
			{
				const int dx = stickLeft ? -1 : (stickRight ? 1 : 0);
				const int dy = stickUp ? -1 : (stickDown ? 1 : 0);
				NavigateGrid(dx, dy);
			}
			else if (stickLeft || stickRight)
			{
				// Horizontal: navigate action buttons and similar horizontal lists.
				Navigate(stickLeft ? -1 : 1);
			}
			else if (stickUp || stickDown)
			{
				Navigate(stickUp ? -1 : 1);
			}
			navigated = true;
		}
		if (navigated)
		{
			lastNavigation = now;
			changed = true;
		}
		if (!anyDirection)
			lastNavigation = 0;

if (changed)
			InvalidateRect(window, nullptr, FALSE);
	}

	// ---------------------------------------------------------------------
	// Web remote: embedded HTTP server
	//
	// Serves the phone UI (index.html/style.css/app.js from embedded
	// resources), every embedded image/tag binary as /img/<resourceId>, a
	// JSON catalog describing the whole tag library, and Load/Move/Clear
	// endpoints that reuse the exact same send paths as the desktop UI.
	// Commands mutate app state on the UI thread via kWebMessage so they can
	// never race the controller poll timer. Socket helper functions (SendAll
	// etc.) are defined earlier in this file.
	// ---------------------------------------------------------------------

	std::string Utf8FromWide(const std::wstring& wide)
	{
		if (wide.empty())
			return {};
		const int length = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
		if (length <= 1)
			return {};
		std::string out(static_cast<size_t>(length - 1), '\0');
		WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, out.data(), length, nullptr, nullptr);
		return out;
	}

	std::wstring WideFromUtf8(const std::string& utf8)
	{
		if (utf8.empty())
			return {};
		const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
		if (length <= 1)
			return {};
		std::wstring out(static_cast<size_t>(length - 1), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, out.data(), length);
		return out;
	}

	std::string JsonEscape(const std::string& in)
	{
		std::string out;
		out.reserve(in.size() + 8);
		for (unsigned char c : in)
		{
			switch (c)
			{
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (c < 0x20)
				{
					char tmp[8];
					::snprintf(tmp, sizeof tmp, "\\u%04x", c);
					out += tmp;
				}
				else
				{
					out += static_cast<char>(c);
				}
			}
		}
		return out;
	}

	std::string WJson(const std::wstring& wide)
	{
		return "\"" + JsonEscape(Utf8FromWide(wide)) + "\"";
	}

	std::string OkJson(const std::wstring& message)
	{
		return "{\"ok\":true,\"status\":" + WJson(message) + "}";
	}

	std::string ErrJson(const std::wstring& message)
	{
		return "{\"ok\":false,\"status\":" + WJson(message) + "}";
	}

	// Sniffs a payload's type from its magic bytes so every embedded resource
	// (png/jpg/ttf/woff2) can be served without a name->mime table.
	const char* MimeForBytes(const uint8_t* data, size_t size)
	{
		if (size >= 8 && std::memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0)
			return "image/png";
		if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
			return "image/jpeg";
		if (size >= 4 && std::memcmp(data, "wOF2", 4) == 0)
			return "font/woff2";
		if (size >= 4 && std::memcmp(data, "wOFF", 4) == 0)
			return "font/woff";
		if (size >= 4 && data[0] == 0x00 && data[1] == 0x01 && data[2] == 0x00 && data[3] == 0x00)
			return "font/ttf";
		if (size >= 1 && data[0] == '<')
			return "text/html; charset=utf-8";
		return "application/octet-stream";
	}

	const char* MimeForWebFile(const char* name)
	{
		const std::string path(name);
		if (path.size() >= 5 && path.compare(path.size() - 5, 5, ".html") == 0)
			return "text/html; charset=utf-8";
		if (path.size() >= 4 && path.compare(path.size() - 4, 4, ".css") == 0)
			return "text/css; charset=utf-8";
		if (path.size() >= 3 && path.compare(path.size() - 3, 3, ".js") == 0)
			return "application/javascript; charset=utf-8";
		if (path.size() >= 5 && path.compare(path.size() - 5, 5, ".json") == 0)
			return "application/json; charset=utf-8";
		if (path.size() >= 4 && path.compare(path.size() - 4, 4, ".svg") == 0)
			return "image/svg+xml";
		if (path.size() >= 6 && path.compare(path.size() - 6, 6, ".woff2") == 0)
			return "font/woff2";
		if (path.size() >= 5 && path.compare(path.size() - 5, 5, ".woff") == 0)
			return "font/woff";
		return "application/octet-stream";
	}

	std::string IdUrl(int resourceId)
	{
		return "/img/" + std::to_string(resourceId);
	}

	std::wstring GetLanAddress()
	{
		ULONG size = 0;
		::GetAdaptersAddresses(AF_INET,
			GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
			nullptr, nullptr, &size);
		if (size == 0)
			return {};
		std::vector<BYTE> buffer(size);
		PIP_ADAPTER_ADDRESSES adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
		if (::GetAdaptersAddresses(AF_INET,
				GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
				nullptr, adapters, &size) != NO_ERROR)
			return {};

		for (PIP_ADAPTER_ADDRESSES adapter = adapters; adapter; adapter = adapter->Next)
		{
			if (adapter->OperStatus != IfOperStatusUp)
				continue;
			if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
				continue;
			for (PIP_ADAPTER_UNICAST_ADDRESS unicast = adapter->FirstUnicastAddress;
				unicast; unicast = unicast->Next)
			{
				const sockaddr_in* address = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
				if (address->sin_family != AF_INET)
					continue;
				const ULONG addr = ntohl(address->sin_addr.s_addr);
				if ((addr >> 24) == 127) continue;      // loopback
				if ((addr >> 16) == 0xA9FE) continue;   // 169.254.x.x APIPA / link-local
				char text[INET_ADDRSTRLEN] = {};
				if (InetNtopA(AF_INET, &address->sin_addr, text, sizeof text))
				{
					return L"http://" + WideFromUtf8(text) + L":" + std::to_wstring(g_app.webPort) + L"/";
				}
			}
		}
		return {};
	}

	void SendHttpResponse(SOCKET socket, int status, const char* statusText,
		const std::string& body, const char* contentType, bool cacheable)
	{
		std::ostringstream head;
		head << "HTTP/1.1 " << status << " " << statusText << "\r\n"
			<< "Content-Type: " << contentType << "\r\n"
			<< "Content-Length: " << body.size() << "\r\n"
			<< "Connection: close\r\n"
			<< "Cache-Control: " << (cacheable ? "public, max-age=3600" : "no-store") << "\r\n"
			<< "Access-Control-Allow-Origin: *\r\n"
			<< "\r\n";
		const std::string header = head.str();
		SendAll(socket, reinterpret_cast<const uint8_t*>(header.data()), header.size());
		SendAll(socket, reinterpret_cast<const uint8_t*>(body.data()), body.size());
	}

	// Minimal HTTP request parser: request line + headers + optional body.
	struct HttpRequest
	{
		std::string method;
		std::string path;
		std::string query;
		std::string version;
		std::map<std::string, std::string> headers;
		std::string body;
	};

	bool ReadHttpRequest(SOCKET socket, HttpRequest& request)
	{
		std::string raw;
		char buffer[4096];
		size_t headerEnd = std::string::npos;
		while (raw.size() < 64 * 1024)
		{
			const int received = recv(socket, buffer, sizeof buffer, 0);
			if (received == SOCKET_ERROR || received == 0)
				return false;
			raw.append(buffer, static_cast<size_t>(received));
			headerEnd = raw.find("\r\n\r\n");
			if (headerEnd != std::string::npos)
				break;
		}
		if (headerEnd == std::string::npos)
			return false;

		const std::string head = raw.substr(0, headerEnd);
		const size_t lineEnd = head.find("\r\n");
		const std::string requestLine = lineEnd == std::string::npos ? head : head.substr(0, lineEnd);

		size_t firstSpace = requestLine.find(' ');
		if (firstSpace == std::string::npos)
			return false;
		const size_t secondSpace = requestLine.find(' ', firstSpace + 1);
		if (secondSpace == std::string::npos)
			return false;
		request.method = requestLine.substr(0, firstSpace);
		std::string target = requestLine.substr(firstSpace + 1, secondSpace - firstSpace - 1);
		request.version = requestLine.substr(secondSpace + 1);

		const size_t queryPos = target.find('?');
		if (queryPos != std::string::npos)
		{
			request.query = target.substr(queryPos + 1);
			target.resize(queryPos);
		}
		request.path = target;

		const size_t headerStart = lineEnd == std::string::npos ? 0 : lineEnd + 2;
		size_t cursor = headerStart;
		while (cursor < head.size())
		{
			const size_t end = head.find("\r\n", cursor);
			if (end == std::string::npos)
				break;
			const std::string line = head.substr(cursor, end - cursor);
			const size_t colon = line.find(':');
			if (colon != std::string::npos)
			{
				std::string key = line.substr(0, colon);
				std::string value = line.substr(colon + 1);
				key.erase(std::remove_if(key.begin(), key.end(),
					[](unsigned char c) { return c == ' ' || c == '\t'; }), key.end());
				std::transform(key.begin(), key.end(), key.begin(),
					[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
					value.erase(value.begin());
				request.headers[key] = value;
			}
			cursor = end + 2;
		}

		const auto contentLength = request.headers.find("content-length");
		if (contentLength != request.headers.end() && !contentLength->second.empty())
		{
			const size_t bodyLength = static_cast<size_t>(std::strtoull(contentLength->second.c_str(), nullptr, 10));
			if (bodyLength > 16 * 1024 * 1024)
				return false;
			const size_t bodyStart = headerEnd + 4;
			while (raw.size() < bodyStart + bodyLength)
			{
				const int received = recv(socket, buffer, sizeof buffer, 0);
				if (received == SOCKET_ERROR || received == 0)
					return false;
				raw.append(buffer, static_cast<size_t>(received));
			}
			request.body = raw.substr(bodyStart, bodyLength);
		}
		return true;
	}

	// Pulls an integer out of {"key": N} JSON minima. Returns -1 when absent.
	int JsonInt(const std::string& json, const char* key)
	{
		const std::string needle = std::string("\"") + key + "\"";
		const size_t keyPos = json.find(needle);
		if (keyPos == std::string::npos)
			return -1;
		const size_t colon = json.find(':', keyPos);
		if (colon == std::string::npos)
			return -1;
		size_t cursor = colon + 1;
		while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == '\t'))
			++cursor;
		if (cursor >= json.size() || (json[cursor] != '-' && (json[cursor] < '0' || json[cursor] > '9')))
			return -1;
		bool negative = json[cursor] == '-';
		if (negative)
			++cursor;
		long long value = 0;
		bool anyDigit = false;
		while (cursor < json.size() && json[cursor] >= '0' && json[cursor] <= '9')
		{
			value = value * 10 + (json[cursor] - '0');
			anyDigit = true;
			++cursor;
		}
		return anyDigit ? static_cast<int>(negative ? -value : value) : -1;
	}

	std::string EntryJson(const RosterEntry& entry)
	{
		const unsigned int color = entry.ringColor;
		char hex[16];
		::snprintf(hex, sizeof hex, "#%02X%02X%02X", color & 0xFF, (color >> 8) & 0xFF, (color >> 16) & 0xFF);
		std::string out = "{";
		out += "\"name\":" + WJson(entry.name) + ",";
		out += "\"build\":" + std::to_string(entry.buildNumber) + ",";
		out += "\"color\":\"" + std::string(hex) + "\",";
		out += "\"portrait\":\"" + IdUrl(entry.portraitResourceId) + "\",";
		out += "\"bin\":" + std::to_string(entry.binResourceId);
		out += "}";
		return out;
	}

	// The full static library description: every franchise, character and
	// vehicle with the asset URLs and bin ids the phone UI needs to render
	// and to request loads. Only reads const data, safe on any thread.
	std::string BuildCatalogJson()
	{
		std::string out = "{";
		out += "\"appName\":" + WJson(L"LEGO Dimensions Toypad") + ",";
		out += "\"version\":" + WJson(kAppVersion) + ",";
		out += "\"listenerPort\":" + std::to_string(g_app.port) + ",";
		out += "\"wordmark\":\"" + IdUrl(kWordmarkResourceId) + "\",";
		out += "\"byMark\":\"" + IdUrl(kByHarrysofResourceId) + "\",";
		out += "\"background\":\"" + IdUrl(WebBackgroundResourceId()) + "\",";
		const int fontResource = kUIFontWebResourceId != 0 ? kUIFontWebResourceId : kUIFontResourceId;
		out += "\"fontUrl\":\"" + IdUrl(fontResource) + "\",";
		out += "\"yButton\":\"" + IdUrl(kYButtonResourceId) + "\",";
		out += "\"settingsText\":\"" + IdUrl(kSettingsTextResourceId) + "\",";
		out += "\"worldTile\":\"" + IdUrl(kWorldTileResourceId) + "\",";
		out += "\"charactersTile\":\"" + IdUrl(kCharactersTileResourceId) + "\",";
		out += "\"loadBtn\":\"" + IdUrl(kLoadButtonResourceId) + "\",";
		out += "\"clearBtn\":\"" + IdUrl(kClearButtonResourceId) + "\",";
		out += "\"moveBtn\":\"" + IdUrl(kMoveButtonResourceId) + "\",";
		out += "\"scrollBar\":\"" + IdUrl(kScrollBarResourceId) + "\",";
		out += "\"pads\":[";
		for (size_t i = 0; i < 7; ++i)
		{
			if (i != 0)
				out += ",";
			out += "\"" + IdUrl(kPadBackgroundResourceIds[i]) + "\"";
		}
		out += "],\"franchises\":[";
		for (size_t franchiseIndex = 0; franchiseIndex < kFranchiseCount; ++franchiseIndex)
		{
			if (franchiseIndex != 0)
				out += ",";
			const Franchise& franchise = kFranchises[franchiseIndex];
			out += "{\"name\":" + WJson(franchise.name) + ",";
			out += "\"logo\":\"" + IdUrl(franchise.logoResourceId) + "\",";
			out += "\"characters\":[";
			for (size_t i = 0; i < franchise.characters.size(); ++i)
			{
				if (i != 0)
					out += ",";
				out += EntryJson(franchise.characters[i]);
			}
			out += "],\"vehicles\":[";
			for (size_t v = 0; v < franchise.vehicles.size(); ++v)
			{
				if (v != 0)
					out += ",";
				const VehicleGroup& group = franchise.vehicles[v];
				out += "{\"base\":" + WJson(group.baseName) + ",\"builds\":[";
				for (size_t b = 0; b < group.builds.size(); ++b)
				{
					if (b != 0)
						out += ",";
					out += EntryJson(group.builds[b]);
				}
				out += "]}";
			}
			out += "]}";
		}
		out += "]}";
		return out;
	}

	// The live pad-state snapshot, read on the UI thread through a WebJob.
	void BuildStateJson(WebJob& job)
	{
		std::string out = "{\"background\":\"" + IdUrl(WebBackgroundResourceId()) + "\",\"pads\":[";
		for (size_t i = 0; i < 7; ++i)
		{
			if (i != 0)
				out += ",";
			const PadSlot& slot = g_app.padState[i];
			const unsigned int color = slot.occupied ? slot.ringColor : 0;
			char hex[16];
			::snprintf(hex, sizeof hex, "#%02X%02X%02X", color & 0xFF, (color >> 8) & 0xFF, (color >> 16) & 0xFF);
			out += "{\"index\":" + std::to_string(i) + ",";
			out += "\"pad\":" + std::to_string(kSlots[i].pad) + ",";
			out += "\"label\":" + WJson(kSlots[i].label) + ",";
			out += std::string("\"occupied\":") + (slot.occupied ? "true" : "false") + ",";
			out += std::string("\"name\":") + (slot.occupied ? WJson(slot.figureName) : std::string("\"\"")) + ",";
			out += "\"color\":\"" + std::string(hex) + "\",";
			out += "\"portrait\":\"" + IdUrl(slot.portraitResourceId) + "\"}";
		}
		out += "],\"status\":" + WJson(g_app.status) + "}";
		job.result = out;
		job.ok = true;
	}

	const RosterEntry* FindEntryByBinResource(int binResourceId)
	{
		for (size_t franchiseIndex = 0; franchiseIndex < kFranchiseCount; ++franchiseIndex)
		{
			for (const auto& character : kFranchises[franchiseIndex].characters)
			{
				if (character.binResourceId == binResourceId)
					return &character;
			}
			for (const auto& vehicle : kFranchises[franchiseIndex].vehicles)
			{
				for (const auto& build : vehicle.builds)
				{
					if (build.binResourceId == binResourceId)
						return &build;
				}
			}
		}
		return nullptr;
	}

	// Runs on the UI thread (dispatched via kWebMessage). Everything it
	// touches belongs to the message-loop thread, so there is no locking.
	void HandleWebJob(WebJob& job)
	{
		switch (job.op)
		{
		case WebJob::Op::State:
			BuildStateJson(job);
			break;
		case WebJob::Op::Catalog:
			if (!g_catalogBuilt)
			{
				g_catalogCache = BuildCatalogJson();
				g_catalogBuilt = true;
			}
			job.result = g_catalogCache;
			job.ok = true;
			break;
		case WebJob::Op::Load:
			if (job.a < 0 || job.a >= static_cast<int>(kSlots.size()))
			{
				job.result = ErrJson(L"Invalid pad slot.");
				break;
			}
			{
				const RosterEntry* entry = FindEntryByBinResource(job.b);
				if (!entry)
				{
					job.result = ErrJson(L"Unknown tag id.");
					break;
				}
				LoadEntryToSlot(*entry, static_cast<size_t>(job.a), false);
				job.ok = g_app.status.rfind(L"LOAD sent", 0) == 0;
				job.result = job.ok ? OkJson(g_app.status) : ErrJson(g_app.status);
			}
			break;
		case WebJob::Op::Move:
			if (job.a < 0 || job.a >= static_cast<int>(kSlots.size()) ||
				job.b < 0 || job.b >= static_cast<int>(kSlots.size()))
			{
				job.result = ErrJson(L"Invalid pad slot.");
				break;
			}
			if (!g_app.padState[job.a].occupied)
			{
				job.result = ErrJson(L"There is nothing tracked on that pad to move.");
				break;
			}
			MoveSlotToSlot(static_cast<size_t>(job.a), static_cast<size_t>(job.b), false);
			job.ok = g_app.status.rfind(L"MOVE sent", 0) == 0 || g_app.status.rfind(L"SWAP sent", 0) == 0 ||
				g_app.status.rfind(L"REFRESH sent", 0) == 0;
			job.result = job.ok ? OkJson(g_app.status) : ErrJson(g_app.status);
			break;
		case WebJob::Op::Clear:
			if (job.a < 0 || job.a >= static_cast<int>(kSlots.size()))
			{
				job.result = ErrJson(L"Invalid pad slot.");
				break;
			}
			ClearSlot(static_cast<size_t>(job.a), false);
			job.ok = g_app.status.rfind(L"CLEAR sent", 0) == 0;
			job.result = job.ok ? OkJson(g_app.status) : ErrJson(g_app.status);
			break;
		case WebJob::Op::ClearAll:
			ClearAllPads(false);
			job.ok = g_app.status.rfind(L"Clear all pad sent", 0) == 0;
			job.result = job.ok ? OkJson(g_app.status) : ErrJson(g_app.status);
			break;
		}
		SetEvent(job.done);
	}

	// Sends an operation to the UI thread and waits (bounded) for its JSON
	// result. Ownership of the job stays in this thread.
	std::string DispatchWebJob(HWND window, WebJob::Op operation, int a, int b)
	{
		WebJob job;
		job.op = operation;
		job.a = a;
		job.b = b;
		job.done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (!job.done)
			return ErrJson(L"Could not create a job event.");
		const BOOL posted = PostMessageW(window, kWebMessage, reinterpret_cast<WPARAM>(&job), 0);
		if (!posted)
		{
			CloseHandle(job.done);
			return ErrJson(L"The desktop app is shutting down.");
		}
		const DWORD waited = WaitForSingleObject(job.done, 8000);
		CloseHandle(job.done);
		if (waited != WAIT_OBJECT_0 || job.result.empty())
			return ErrJson(L"Timed out talking to the desktop app.");
		return job.result;
	}

	void HandleHttpConnection(SOCKET socket, HWND window)
	{
		const DWORD receiveTimeout = 8000;
		setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
			reinterpret_cast<const char*>(&receiveTimeout), sizeof receiveTimeout);

		HttpRequest request;
		if (!ReadHttpRequest(socket, request))
		{
			SendHttpResponse(socket, 400, "Bad Request", "Bad Request", "text/plain; charset=utf-8", false);
			closesocket(socket);
			return;
		}

		// Serves one of the embedded UI files (index.html / style.css / app.js).
		auto serveWebFile = [&](const std::string& filename) -> void
		{
			for (size_t i = 0; i < kWebFileCount; ++i)
			{
				if (filename != kWebFiles[i].name)
					continue;
				std::vector<uint8_t> data = LoadResourceBytes(kWebFiles[i].resourceId);
				if (data.empty())
				{
					SendHttpResponse(socket, 404, "Not Found", "Not Found", "text/plain; charset=utf-8", false);
					return;
				}
				const std::string body(reinterpret_cast<const char*>(data.data()), data.size());
				// The UI payloads change on every rebuild (they are compiled in),
				// so never let a browser heuristically serve a 1-hour-old
				// style.css/app.js. no-store plus a versioned URL (see index.html)
				// guarantees the phone always gets the embedded bytes.
				SendHttpResponse(socket, 200, "OK", body, MimeForWebFile(kWebFiles[i].name), false);
				return;
			}
			SendHttpResponse(socket, 404, "Not Found", "Not Found", "text/plain; charset=utf-8", false);
		};

		// Serves any embedded resource binary by its numeric id (png/jpg/font/tag).
		auto serveResource = [&](int resourceId) -> void
		{
			std::vector<uint8_t> data = LoadResourceBytes(resourceId);
			if (data.empty())
			{
				SendHttpResponse(socket, 404, "Not Found", "Not Found", "text/plain; charset=utf-8", false);
				return;
			}
			const std::string body(reinterpret_cast<const char*>(data.data()), data.size());
			SendHttpResponse(socket, 200, "OK", body, MimeForBytes(data.data(), data.size()), true);
		};

		if (request.method == "GET")
		{
			if (request.path == "/" || request.path == "/index.html")
			{
				serveWebFile("index.html");
			}
			else if (request.path == "/style.css")
			{
				serveWebFile("style.css");
			}
			else if (request.path == "/app.js")
			{
				serveWebFile("app.js");
			}
			else if (request.path == "/InterVariable.woff2")
			{
				serveWebFile("InterVariable.woff2");
			}
			else if (request.path == "/api/catalog")
			{
				const std::string body = DispatchWebJob(window, WebJob::Op::Catalog, 0, 0);
				SendHttpResponse(socket, 200, "OK", body, "application/json; charset=utf-8", false);
			}
			else if (request.path == "/api/state")
			{
				const std::string body = DispatchWebJob(window, WebJob::Op::State, 0, 0);
				SendHttpResponse(socket, 200, "OK", body, "application/json; charset=utf-8", false);
			}
			else if (request.path.rfind("/img/", 0) == 0 && request.path.size() > 5)
			{
				int resourceId = ::atoi(request.path.c_str() + 5);
				if (resourceId == 0)
					SendHttpResponse(socket, 400, "Bad Request", "Bad Request", "text/plain; charset=utf-8", false);
				else
					serveResource(resourceId);
			}
			else if (request.path == "/favicon.ico")
			{
				serveResource(kAppIconResourceId);
			}
			else
			{
				SendHttpResponse(socket, 404, "Not Found", "Not Found", "text/plain; charset=utf-8", false);
			}
		}
		else if (request.method == "POST")
		{
			std::string body;
			if (request.path == "/api/load")
			{
				const int slot = JsonInt(request.body, "slot");
				const int bin = JsonInt(request.body, "bin");
				body = DispatchWebJob(window, WebJob::Op::Load, slot, bin);
			}
			else if (request.path == "/api/move")
			{
				const int source = JsonInt(request.body, "src");
				const int dest = JsonInt(request.body, "dest");
				body = DispatchWebJob(window, WebJob::Op::Move, source, dest);
			}
			else if (request.path == "/api/clear")
			{
				const int slot = JsonInt(request.body, "slot");
				body = DispatchWebJob(window, WebJob::Op::Clear, slot, 0);
			}
			else if (request.path == "/api/clearall")
			{
				body = DispatchWebJob(window, WebJob::Op::ClearAll, 0, 0);
			}
			else
			{
				SendHttpResponse(socket, 404, "Not Found", "Not Found", "text/plain; charset=utf-8", false);
				closesocket(socket);
				return;
			}
			SendHttpResponse(socket, 200, "OK", body, "application/json; charset=utf-8", false);
		}
		else
		{
			SendHttpResponse(socket, 405, "Method Not Allowed", "Method Not Allowed", "text/plain; charset=utf-8", false);
		}

		closesocket(socket);
	}

	void WebServerLoop(SOCKET listenSocket, HWND window)
	{
		while (g_webRunning.load())
		{
			fd_set readSet;
			FD_ZERO(&readSet);
			FD_SET(listenSocket, &readSet);
			timeval timeout{0, 200000}; // 200ms wake-up so shutdown is snappy
			const int ready = select(0, &readSet, nullptr, nullptr, &timeout);
			if (ready == SOCKET_ERROR || ready == 0)
				continue;

			SOCKET client = accept(listenSocket, nullptr, nullptr);
			if (client == INVALID_SOCKET)
				continue;

			// One throwaway thread per connection. The LAN scale is tiny and
			// the phone is the only client, so no pool is warranted; detached
			// workers that are mid-request simply die with the process.
			std::thread worker(HandleHttpConnection, client, window);
			worker.detach();
		}
		closesocket(listenSocket);
	}

	bool StartWebServer(HWND window)
	{
		if (g_webRunning.load())
			return true;

		SOCKET listenSocket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (listenSocket == INVALID_SOCKET)
			return false;

		BOOL reuseAddress = TRUE;
		setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR,
			reinterpret_cast<const char*>(&reuseAddress), sizeof reuseAddress);

		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_port = htons(g_app.webPort);
		address.sin_addr.s_addr = htonl(INADDR_ANY); // listen on all LAN interfaces
		if (bind(listenSocket, reinterpret_cast<const sockaddr*>(&address), sizeof address) == SOCKET_ERROR ||
			listen(listenSocket, 8) == SOCKET_ERROR)
		{
			closesocket(listenSocket);
			return false;
		}

		g_webWindow = window;
		g_webListenSocket = listenSocket;
		g_webRunning.store(true);
		g_webThread = std::thread(WebServerLoop, listenSocket, window);
		return true;
	}

	void StopWebServer()
	{
		if (!g_webRunning.load())
			return;
		g_webRunning.store(false);
		if (g_webListenSocket != INVALID_SOCKET)
		{
			closesocket(g_webListenSocket);
			g_webListenSocket = INVALID_SOCKET;
		}
		if (g_webThread.joinable())
			g_webThread.join();
	}

	void ApplyWindowCornerRadius(HWND window)
	{
		RECT rect{};
		GetClientRect(window, &rect);
		const int width = rect.right - rect.left;
		const int height = rect.bottom - rect.top;
		if (width <= 0 || height <= 0)
			return;

		HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1,
			kWindowCornerRadius * 2, kWindowCornerRadius * 2);
		if (!region)
			return;
		if (SetWindowRgn(window, region, TRUE) == 0)
			DeleteObject(region);
	}

	LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
	{
		switch (message)
		{
		case WM_PAINT:
			Paint(window);
			return 0;
		case WM_KEYDOWN:
			if (g_app.capturingBindingIndex >= 0)
			{
				// Binding capture is controller-only; the keyboard can only
				// bail out of it.
				if (wParam == VK_ESCAPE)
					CancelBindingCapture();
				InvalidateRect(window, nullptr, FALSE);
				return 0;
			}
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
				NavigateGrid(-1, 0);
				break;
			case VK_RIGHT:
				NavigateGrid(1, 0);
				break;
			case VK_RETURN: Confirm(); break;
			case VK_ESCAPE: Back(window); break;
			case 'S':
				if (g_app.screen == Screen::PadViewer)
					g_app.screen = Screen::Settings;
				break;
			// Keyboard equivalents of the one-press pad actions bound to
			// X / RB / LB on a controller.
			case 'M':
				if (g_app.screen == Screen::PadViewer && !g_app.selectingMoveDestination)
					BeginMoveFromSelectedPad();
				break;
			case 'L':
				if (g_app.screen == Screen::PadViewer && !g_app.selectingMoveDestination)
				{
					if (g_app.storyMode)
						OpenStoryRoster();
					else
						OpenFranchiseList();
				}
				break;
			case 'C':
				if (g_app.screen == Screen::PadViewer && !g_app.selectingMoveDestination)
				{
					if (g_app.padState[g_app.slotIndex].occupied)
						ClearSlot(g_app.slotIndex, true);
					else
						g_app.status = L"There is nothing tracked on that pad to clear.";
				}
				break;
			}
			InvalidateRect(window, nullptr, FALSE);
			return 0;
		case WM_MOUSEMOVE:
		{
			const POINT point{static_cast<SHORT>(LOWORD(lParam)), static_cast<SHORT>(HIWORD(lParam))};
			const int hoveredAction = ActionButtonIndexAt(point);
			if (hoveredAction != g_app.hoveredPadActionIndex)
			{
				g_app.hoveredPadActionIndex = hoveredAction;
				InvalidateRect(window, nullptr, FALSE);
			}
			TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
			TrackMouseEvent(&tracking);
			return 0;
		}
		case WM_MOUSELEAVE:
			if (g_app.hoveredPadActionIndex != -1 || g_app.pressedPadActionIndex != -1)
			{
				g_app.hoveredPadActionIndex = -1;
				g_app.pressedPadActionIndex = -1;
				InvalidateRect(window, nullptr, FALSE);
			}
			return 0;
		case WM_LBUTTONDOWN:
		{
			const POINT point{static_cast<SHORT>(LOWORD(lParam)), static_cast<SHORT>(HIWORD(lParam))};
			const int actionIndex = ActionButtonIndexAt(point);
			if (actionIndex >= 0)
			{
				g_app.pressedPadActionIndex = actionIndex;
				g_app.padActionIndex = static_cast<size_t>(actionIndex);
				SetCapture(window);
				InvalidateRect(window, nullptr, FALSE);
			}
			return 0;
		}
		case WM_LBUTTONUP:
		{
			const POINT point{static_cast<SHORT>(LOWORD(lParam)), static_cast<SHORT>(HIWORD(lParam))};
			const int actionIndex = ActionButtonIndexAt(point);
			if (g_app.pressedPadActionIndex >= 0)
			{
				const bool activate = actionIndex == g_app.pressedPadActionIndex;
				g_app.pressedPadActionIndex = -1;
				if (GetCapture() == window)
					ReleaseCapture();
				if (activate)
				{
					g_app.padActionIndex = static_cast<size_t>(actionIndex);
					Confirm();
				}
				InvalidateRect(window, nullptr, FALSE);
				return 0;
			}

			for (size_t index = 0; index < kPadCells.size(); ++index)
			{
				if (!PtInRect(&kPadCells[index], point))
					continue;

				g_app.slotIndex = index;
				g_app.hoveredPadActionIndex = -1;
				if (g_app.screen == Screen::PadViewer && g_app.selectingMoveDestination)
					MoveToDestination(index);
				else if (g_app.screen == Screen::PadViewer || g_app.screen == Screen::PadAction)
				{
					g_app.screen = Screen::PadAction;
					g_app.padActionIndex = 0;
				}
				InvalidateRect(window, nullptr, FALSE);
				return 0;
			}
			return 0;
		}
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
		case kWebMessage:
			if (wParam)
				HandleWebJob(*reinterpret_cast<WebJob*>(wParam));
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
			case kMenuIdWebAddress:
				if (g_app.webEnabled)
				{
					if (g_app.webUrl.empty()) // server running in case URL lookup lost the network
						g_app.webUrl = GetLanAddress();
					if (!g_app.webUrl.empty())
					{
						CopyTextToClipboard(g_app.webUrl);
						g_app.status = L"Web remote address copied: " + g_app.webUrl;
					}
					else
					{
						g_app.status = L"Web remote is on, but no LAN address was found.";
					}
				}
				else
				{
					g_app.status = L"Web remote is disabled. Enable it from Settings.";
				}
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
			UpdateInputOwnership(window);
			return 0;
		case WM_SIZE:
			ApplyWindowCornerRadius(window);
			UpdateInputOwnership(window);
			return 0;
		case WM_DESTROY:
			KillTimer(window, kControllerTimer);
			UnregisterHotKey(window, kToggleHotkeyId);
			RemoveTrayIcon(window);
			StopWebServer();
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

	// Loads gamecontrollerdb.txt from the exe's own directory, if present.
	// The bundled SDL2 mappings already cover Xbox / DualShock / DualSense /
	// Switch Pro; this file is the escape hatch for unrecognized pads
	// (community DB from github.com/gabomdq/SDL_GameControllerDB, or one
	// generated by SDL's mapping tools). Missing file is a silent no-op.
	void LoadExtraControllerMappings()
	{
		wchar_t modulePath[MAX_PATH];
		if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) == 0)
			return;
		std::filesystem::path dbPath(modulePath);
		dbPath.replace_filename(L"gamecontrollerdb.txt");
		if (!std::filesystem::exists(dbPath))
			return;

		char utf8Path[MAX_PATH * 2];
		if (WideCharToMultiByte(CP_UTF8, 0, dbPath.c_str(), -1, utf8Path,
				static_cast<int>(sizeof(utf8Path)), nullptr, nullptr) > 0)
			SDL_GameControllerAddMappingsFromFile(utf8Path);
	}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
	// Single instance: the named mutex survives as long as this process, so a
	// second launch can't grab it and quietly exits instead of running two
	// pickers (which would fight over the socket, controller and toy-pad).
	// Keep the handle alive for the whole lifetime by not closing it here.
	HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\LegoToypadSingleInstance");
	if (!instanceMutex || GetLastError() == ERROR_ALREADY_EXISTS)
		return 0;

	SetProcessDPIAware();

	// Keep the overlay responsive while an emulator saturates the CPU: at
	// normal priority the 8ms poll timer and repaints get starved into
	// visible input lag whenever the game pegs every core.
	SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

	WSADATA wsaData{};
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
		return 1;

	// GDI+ powers the glossy/glowing off-screen rendering; shut down after
	// the message loop, once every cached bitmap is released.
	Gdiplus::GdiplusStartupInput gdiplusStartupInput;
	Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, nullptr);

	// SDL2 provides the unified controller input (Xbox / PS4 / PS5 / Switch
	// Pro) via its GameController API. SDL_INIT_GAMECONTROLLER implies the
	// joystick subsystem. If SDL itself fails to start, clean up what was
	// already initialized and bail non-zero, mirroring the CreateWindowExW
	// failure path below.
	// SDL already speaks Xbox, PlayStation and Nintendo pads through its
	// HIDAPI drivers; these hints just make that explicit rather than
	// relying on the defaults staying put, pair loose Joy-Cons into a
	// single controller, and ask for Nintendo face buttons by their printed
	// label (so the button marked A is the one this app calls A). Hint
	// names are passed as strings: an SDL build without one of them
	// ignores it instead of failing to compile.
	SDL_SetHint("SDL_JOYSTICK_HIDAPI", "1");
	SDL_SetHint("SDL_JOYSTICK_HIDAPI_PS3", "1");
	SDL_SetHint("SDL_JOYSTICK_HIDAPI_PS4", "1");
	SDL_SetHint("SDL_JOYSTICK_HIDAPI_PS5", "1");
	SDL_SetHint("SDL_JOYSTICK_HIDAPI_SWITCH", "1");
	SDL_SetHint("SDL_JOYSTICK_HIDAPI_JOY_CONS", "1");
	SDL_SetHint("SDL_JOYSTICK_HIDAPI_COMBINE_JOY_CONS", "1");
	SDL_SetHint("SDL_GAMECONTROLLER_USE_BUTTON_LABELS", "1");

	if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0)
	{
		Gdiplus::GdiplusShutdown(g_gdiplusToken);
		WSACleanup();
		return 1;
	}
	LoadExtraControllerMappings();

	LoadUIFont();

EnsureDefaultIniExists();
	g_app.port = ReadPort();
	LoadShortcutFromIni();
	LoadInputSettingsFromIni();
	LoadWebSettingsFromIni();

	size_t embeddedTags = 0;
	for (size_t i = 0; i < kFranchiseCount; ++i)
	{
		embeddedTags += kFranchises[i].characters.size();
		for (const auto& vehicle : kFranchises[i].vehicles)
			embeddedTags += vehicle.builds.size();
	}
g_app.status = std::to_wstring(embeddedTags) +
		L" tags embedded. Listener port: " + std::to_wstring(g_app.port) +
		L" | Toggle: " + DescribeShortcut() +
		L" | Confirm: " + DescribeConfirmButtonMode() +
		L" | Background: " + DescribeBackgroundChoice() +
		L" | Buttons: " + DescribeButtonStyle() +
		L" | Web remote: " + (g_app.webEnabled ? L"on" : L"off");

	g_inputOwnershipEvent = CreateEventW(nullptr, TRUE, FALSE, kLegoToypadInputEvent);

	const wchar_t* className = L"LegoToypadWindow";
	WNDCLASSW windowClass{};
	windowClass.hInstance = instance;
	windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
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
		ReleaseGlossCache();
		ReleaseAssetImages();
		UnloadUIFont();
		Gdiplus::GdiplusShutdown(g_gdiplusToken);
		WSACleanup();
		return 1;
	}
	g_mainWindow = window;
	ApplyOverlayTransparency(window);
	ApplyWindowCornerRadius(window);
	AddTrayIcon(window);
	RegisterToggleHotkeyIfNeeded(window);
	SetTimer(window, kControllerTimer, 8, nullptr);

	// Start the web remote if enabled in settings; surface the phone address
	// in the status line and the tray copy menu.
	if (g_app.webEnabled)
	{
		if (StartWebServer(window))
		{
			g_app.webUrl = GetLanAddress();
			if (g_app.webUrl.empty())
				g_app.webUrl = L"http://<this-pc-lan-ip>:" + std::to_wstring(g_app.webPort) + L"/";
			g_app.status = std::wstring(L"Web remote on: ") + g_app.webUrl + L" (Enable from Settings anytime)";
		}
		else
		{
			g_app.status = L"Web remote could not start (port " + std::to_wstring(g_app.webPort) +
				L" busy or unavailable). Checking the firewall may be needed.";
		}
	}
	// The default Windows timer granularity (~15.6ms) would round an 8ms
	// poll back up to ~16ms, keeping the fast-tap miss window untouched, so
	// tighten the system timer resolution to 1ms for the app's lifetime.
	timeBeginPeriod(1);

	MSG message{};
	while (GetMessageW(&message, nullptr, 0, 0) > 0)
	{
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}

	timeEndPeriod(1);

	ReleaseGlossCache();
	ReleaseAssetImages();
	UnloadUIFont();
	SDL_Quit();
	Gdiplus::GdiplusShutdown(g_gdiplusToken);
	WSACleanup();
	return static_cast<int>(message.wParam);
}
