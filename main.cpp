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
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
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
	// The controller poll used to run on a WM_TIMER. Windows only synthesises
	// WM_TIMER when the message queue is otherwise empty, and it shares that
	// lowest priority tier with WM_PAINT - so every millisecond spent painting
	// was a millisecond the next controller poll was delayed by, and a slow
	// frame turned directly into a dropped or late button press. A posted
	// message is an ordinary queued message and is delivered ahead of both, so
	// input now always wins over drawing. A small thread does the pacing; the
	// handler still runs on the UI thread, so nothing about the app's
	// single-threaded state model changes.
	constexpr UINT kTickMessage = WM_APP + 4;
	// 8ms while the picker is up, so a quick tap of a button is never missed.
	// Twice that while it is hidden, where the only thing the poll is looking
	// for is the toggle shortcut - which nobody presses for less than 16ms -
	// and where this app otherwise sits in the tray all day burning CPU for
	// nothing.
	constexpr DWORD kTickIntervalVisibleMs = 8;
	constexpr DWORD kTickIntervalHiddenMs = 16;
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
	// Uniform translucency for the whole panel, as a percentage of opaque.
	// 92% is the old fixed 235/255. The Settings row steps this (and sound
	// volume) by kSettingsPercentStep in either direction.
	constexpr int kDefaultOpacityPercent = 92;
	// Toypad sneak peek: a click-through, never-activated HUD drawn straight
	// over the running game while a button is held. It is a second view of
	// the same seven pads - it reads g_app.padState / g_app.ledRegions and
	// nothing else, so it is always in lockstep with the overlay's own pads.
	//
	// The point of it is that the game keeps running. The picker overlay
	// takes the foreground and asserts input ownership (see
	// UpdateInputOwnership), which is exactly what stops the emulator dead
	// while it is up; the peek window does neither - WS_EX_TRANSPARENT plus
	// WS_EX_NOACTIVATE means every button, including the one being held to
	// summon it, goes to the game untouched.
	constexpr int kPeekSizeChoiceCount = 4; // Off / Small / Medium / Large
	constexpr size_t kDefaultPeekSizeChoice = 2; // Medium
	// Pad height as a fraction of the monitor's height at Medium, so the HUD
	// is the same physical size at 1080p and at 4K. The other sizes scale
	// this by kPeekSizeScales.
	constexpr float kPeekPadHeightFraction = 0.145f;
	constexpr std::array<float, kPeekSizeChoiceCount> kPeekSizeScales = {0.0f, 0.78f, 1.0f, 1.26f};
	// Screen-edge insets, as fractions of the monitor.
	constexpr float kPeekMarginXFraction = 0.022f;
	constexpr float kPeekMarginYFraction = 0.030f;
	constexpr DWORD kPeekFadeInMs = 90;
	constexpr DWORD kPeekFadeOutMs = 130;
	constexpr int kOpacityFloorPercent = 20; // below this the window becomes hard to see
	constexpr int kSettingsPercentStep = 15;
	constexpr int kWindowCornerRadius = 20;
	// kAppVersion itself now comes from GeneratedAssetTable (generated from
	// generate_assets.py's APP_VERSION) so the web UI's version string can
	// never drift from the exe's own FILEVERSION/ProductVersion again.

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

	// The real Toypad only writes tags through its centre portal - the two
	// side portals are read-only in hardware. That is a fixed fact about the
	// physical device, not something this app or the picker's own pad
	// selection has any say in: when a game prompts for a blank tag to
	// create a custom one, it always means the centre pad, regardless of
	// which of the 7 slots the user had focused when they opened the "+"
	// tile.
	constexpr size_t kCenterPadSlotIndex = 1;

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

	// How a physical pad region's LED is behaving, as commanded by the game's
	// HID LED commands (0xC0..0xC8). The app mirrors this so the overlay glows
	// like the real toypad during keystone puzzles. ApplyLedCommand is the seam
	// the wire poll (and the mock demo) drives.
	//
	// Durations arrive as toypad *ticks*, not milliseconds - the wire fields are
	// single bytes, so a millisecond reading would cap every effect at 255ms and
	// make finite ones expire before the first repaint. See kLedTickMs.
	enum class LedMode : uint8_t { Off, Solid, Flash, Fade };

	struct LedRegion
	{
		LedMode mode = LedMode::Off;
		uint8_t r = 0, g = 0, b = 0; // Fade: the target colour being faded to
		uint8_t fromR = 0, fromG = 0, fromB = 0; // Fade: the colour being faded from
		int onTicks = 0;    // Flash: lit duration, in toypad ticks
		int offTicks = 0;   // Flash: dark duration, in toypad ticks
		int count = 0;      // Flash/Fade: cycle count, 0 = repeat until next command
		int speedTicks = 0; // Fade: ticks per fade step
		DWORD cycleStart = 0;
		float intensity = 0.0f; // 0..1 overall alpha, computed by the animation tick
		// The colour actually drawn this frame. Equals r/g/b for every mode
		// except Fade, where the real toypad alternates between fromR/G/B and
		// r/g/b rather than ramping one colour's brightness - see
		// ComputeLedFrame.
		uint8_t curR = 0, curG = 0, curB = 0;
	};

	// One flat slot in the roster grid: a character portrait, a vehicle's
	// build-1 portrait, or the "+" tile of a multi-build vehicle.
	//
	// Character makes up the grid's first (character) section; Vehicle / Plus
	// make up the second one, below the separator.
	struct RosterSlot
	{
		enum class Kind { Character, Vehicle, Plus } kind = Kind::Character;
		const RosterEntry* entry = nullptr;    // Character / Vehicle (build 1)
		const VehicleGroup* group = nullptr;   // Plus tile
	};

	// A favorited character or vehicle, identified by (franchise, name)
	// rather than a resource id: ids are reassigned whenever the asset
	// generator's symbol table changes, but names are stable and are what
	// gets persisted to the ini. "name" is the character name for a
	// character, or the vehicle group's baseName (family) for a vehicle -
	// favoriting a multi-build vehicle favorites the whole group, same as
	// what the roster tile itself represents.
	struct FavoriteEntry
	{
		std::wstring franchise;
		std::wstring name;
		bool isVehicle = false;
		// Which build of a multi-build vehicle this favorite pins - 0 means
		// "unspecified": either a character (irrelevant), or a favorite
		// saved before per-variant favoriting existed, which resolves to
		// build 1 for backward compatibility. A specific vehicle favorited
		// today always stores its real RosterEntry::buildNumber, so two
		// builds of the same family can be favorited independently.
		int buildNumber = 0;
	};

	struct AppState
	{
		Screen screen = Screen::PadViewer;
		size_t slotIndex = 0;
		size_t padActionIndex = 0;
		int hoveredPadActionIndex = -1;
		int pressedPadActionIndex = -1;
		size_t settingsIndex = 0;
		int settingsTopRow = 0; // first visible Settings row (scrolled list)
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
		// Controller bindings for the picker's own actions. Single-button
		// masks, remappable from the Settings screen, persisted in [Input].
		ButtonMask buttonConfirm = XINPUT_GAMEPAD_A;
		ButtonMask buttonBack = XINPUT_GAMEPAD_B;
		ButtonMask buttonSettings = XINPUT_GAMEPAD_Y;
		ButtonMask buttonMoveActive = XINPUT_GAMEPAD_X;
		ButtonMask buttonQuickLoad = XINPUT_GAMEPAD_RIGHT_SHOULDER;
		ButtonMask buttonQuickClear = XINPUT_GAMEPAD_LEFT_SHOULDER;
		// Same physical default as buttonSettings (Y / Triangle / Y) - no
		// conflict, since this only fires on the RosterList/PlusPicker
		// screens and buttonSettings only fires on PadViewer.
		ButtonMask buttonFavorite = XINPUT_GAMEPAD_Y;
		// Reorganize: picks a favorite/franchise tile up so the next
		// navigation + confirm drops it in a new spot. Same physical default
		// as buttonMoveActive (X / Square / X) - no conflict, disjoint
		// screens from everything else already on X.
		ButtonMask buttonReorganizeRoster = XINPUT_GAMEPAD_X;
		ButtonMask buttonReorganizeFranchise = XINPUT_GAMEPAD_X;
		// Held (not tapped) to summon the sneak-peek HUD over the game. The
		// left trigger by default: nothing else in the picker is bound to it,
		// and LEGO Dimensions itself only uses it for the shoulder-swap, so
		// holding it is cheap in-game.
		ButtonMask buttonSneakPeek = kTriggerLeftButton;
		// Sneak peek size, 0 = off. Index into kPeekSizeScales.
		size_t peekSizeChoice = kDefaultPeekSizeChoice;
		// Index into kBindableActions while capturing a new button for one
		// of them from Settings; -1 when no binding capture is running.
		int capturingBindingIndex = -1;
		// Which pad's button labels the UI uses. 0 = Auto (follow whatever
		// is connected), 1..4 = pinned to one style.
		size_t buttonStyleChoice = 0;
		// Mirrors the running game's toypad LEDs onto the overlay's pads.
		// OFF by default: the mirror repaints the pads as bare outlines and
		// polls the listener 30x a second, which is not what a first run
		// should look like or cost. Turning it on in Settings starts the poll
		// thread; turning it off stops it entirely, so nothing talks to the
		// listener and the pads stay their printed glass art.
		bool ledMirrorEnabled = false;
		// UI sound effects (Assets/SFX): a blip when the selection moves and
		// a different one when something is confirmed.
		bool soundEffects = true;
		// Playback level, 0-100. PlaySound has no volume control of its own,
		// so this is applied by scaling the WAV's samples (see GetSoundBytes).
		int soundVolume = 70;
		// Whole-window translucency, as a percentage of opaque. Applied as
		// UpdateLayeredWindow's constant alpha on top of the frame's own
		// per-pixel alpha, so it dims glows and art alike.
		int opacityPercent = kDefaultOpacityPercent;
		// Which folder under Assets/Pads supplies the seven pad images.
		// Stored by name in the ini so adding or removing a skin folder never
		// silently repoints this at a different one.
		size_t padSkinIndex = 0;
		// Window placement. Fixed re-centres the overlay on the active
		// monitor every time it is shown; draggable lets the mouse pick it
		// up anywhere that isn't a clickable control and remembers where it
		// was dropped, across hide/show and across restarts.
		bool windowDraggable = false;
		bool hasSavedWindowPos = false;
		int savedWindowX = 0;
		int savedWindowY = 0;
		// Live drag state; only meaningful while the left button is held
		// down on a draggable overlay. Both points are screen coordinates,
		// so the delta stays right even as the window moves under the mouse.
		bool draggingWindow = false;
		POINT dragStartCursor{};
		POINT dragStartWindow{};
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
		std::array<LedRegion, 3> ledRegions{};
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
		// The roster slot that was focused when the build picker was opened,
		// so backing out of it re-selects that vehicle instead of resetting
		// to the top of the roster.
		size_t rosterIndexBeforePlus = 0;

		// Favorites: characters/vehicles marked from the roster screen,
		// persisted in the ini under [Favorites] and browsed through the
		// Favorites tile prepended to the franchise grid.
		std::vector<FavoriteEntry> favorites;
		// True when the franchise grid's logical index 0 (the Favorites
		// tile) is the focused/open tile, instead of kFranchises[franchiseIndex].
		bool favoritesTileSelected = false;
		// Reordering the Favorites roster: pick a tile up (source index into
		// rosterSlots), navigate to a new spot, drop it there. Only ever
		// active while browsing the Favorites roster itself.
		bool reorganizingRoster = false;
		size_t reorganizeRosterSourceIndex = 0;

		// Custom franchise/world display order, indices into kFranchises.
		// Defaults to [0, 1, ..., kFranchiseCount-1] (the catalog's own
		// order); persisted by name under [FranchiseOrder], resolved back to
		// indices on load - see LoadFranchiseOrderFromIni.
		std::vector<size_t> franchiseDisplayOrder;
		// Reordering the franchise grid: same pick-up/move/drop shape as
		// reorganizingRoster above, but reorganizeFranchiseSourceIndex is a
		// display slot (0-based over real franchises, the synthetic
		// Favorites tile excluded).
		bool reorganizingFranchise = false;
		size_t reorganizeFranchiseSourceIndex = 0;

		// How the franchise grid is sorted/filtered on the franchise page.
		// Cycled with the shoulder buttons (RB/LB, R1/L1, R/L) so the user can
		// browse all-series / custom / story in real time without touching
		// Settings. Favorites is reserved for a future mode.
		enum class FranchiseSort { Default, User, Story, Favorites };
		FranchiseSort franchiseSort = FranchiseSort::Default;
		// Effective grid content for the current sort: franchise indices in
		// display order, excluding the optional Favorites tile. Rebuilt by
		// RebuildFranchiseDisplay(); franchiseDisplayOrder stays the user's
		// persisted custom order (used only when sort == User).
		std::vector<size_t> franchiseDisplayList;
		bool showFavoritesTile = true;
	};

	AppState g_app;
	HANDLE g_inputOwnershipEvent = nullptr;

	// ---------------------------------------------------------------------
	// Transitions
	// ---------------------------------------------------------------------
	// Two independent fades:
	//
	//   - the whole window fades in when the overlay is summoned and back out
	//     when it is dismissed, so it arrives and leaves instead of snapping.
	//     This one rides UpdateLayeredWindow's constant alpha, which costs
	//     nothing extra: the frame is already presented that way, only the
	//     blend value changes.
	//   - the screen content cross-fades (and settles up a few pixels) every
	//     time the picker moves between screens. That one renders the content
	//     into a scratch layer for the ~150ms it lasts and composites the
	//     layer at a scaled alpha; outside a transition it is drawn straight
	//     into the frame with no layer at all, so the steady state is exactly
	//     as cheap as before.
	//
	// Both are driven off the existing 8ms controller timer, which already
	// owned the "does anything still need repainting" decision for the LEDs.
	constexpr DWORD kWindowFadeInMs = 140;
	constexpr DWORD kWindowFadeOutMs = 110;
	constexpr DWORD kScreenFadeMs = 150;
	constexpr int kScreenFadeRisePx = 10;

	DWORD g_windowFadeStart = 0;
	bool g_windowFadingOut = false;
	// True from the moment a hide is asked for until the fade-out finishes
	// and the window is really hidden. The overlay still counts as visible
	// (and still paints) throughout, but stops accepting navigation.
	bool g_overlayHiding = false;
	DWORD g_screenFadeStart = 0;
	Screen g_lastTransitionScreen = Screen::PadViewer;

	// A small banner that flashes g_app.status whenever it changes, then
	// fades - e.g. "Added to favorites: X". g_app.status has dozens of call
	// sites and no dedicated setter, so rather than touching every one of
	// them, this just diffs the string once a tick (SyncStatusToast) and
	// times the fade from whenever it last changed.
	constexpr DWORD kStatusToastHoldMs = 1400;
	constexpr DWORD kStatusToastFadeMs = 450;
	constexpr DWORD kStatusToastTotalMs = kStatusToastHoldMs + kStatusToastFadeMs;
	std::wstring g_lastSeenStatus;
	DWORD g_statusToastStart = 0;

	void SyncStatusToast()
	{
		if (g_app.status == g_lastSeenStatus)
			return;
		g_lastSeenStatus = g_app.status;
		if (g_app.status.empty())
		{
			g_statusToastStart = 0;
			return;
		}
		g_statusToastStart = GetTickCount();
		if (g_statusToastStart == 0)
			g_statusToastStart = 1; // never let 0 mean "armed" collide with "off"
	}

	bool StatusToastActive()
	{
		return g_statusToastStart != 0 && GetTickCount() - g_statusToastStart < kStatusToastTotalMs;
	}

	// 1 through the hold phase, easing down to 0 over the fade phase.
	float StatusToastAlpha()
	{
		if (!StatusToastActive())
			return 0.0f;
		const DWORD elapsed = GetTickCount() - g_statusToastStart;
		if (elapsed < kStatusToastHoldMs)
			return 1.0f;
		const float t = static_cast<float>(elapsed - kStatusToastHoldMs) / static_cast<float>(kStatusToastFadeMs);
		return 1.0f - std::clamp(t, 0.0f, 1.0f);
	}

	// The focused tile's landing animation. The selection glow itself is
	// steady; what moves is a single small spring the moment the selection
	// arrives somewhere new - the tile swells ~5% and settles back over
	// ~220ms, and then nothing animates at all.
	//
	// This replaced a glow that breathed continuously. That version had two
	// problems: every frame of it was a full-window repaint forever, and
	// because the whole 900x610 layered surface was being re-presented at a
	// slightly different alpha several times a second, the entire panel
	// appeared to shimmer rather than the selected tile appearing to glow.
	// A one-shot animation is both calmer to look at and free when idle.
	constexpr DWORD kSelectionTapMs = 220;
	constexpr float kSelectionTapScale = 0.05f;  // 5% at the peak
	constexpr float kSelectionGlowRest = 0.80f;  // steady glow alpha between taps

	DWORD g_selectionTapStart = 0;
	uint64_t g_lastSelectionSignature = 0;
	bool g_hasSelectionSignature = false;

	bool SelectionTapActive()
	{
		return g_selectionTapStart != 0 && GetTickCount() - g_selectionTapStart < kSelectionTapMs;
	}

	// 0 -> 1 -> 0 over the tap, rising quickly and settling slowly, with zero
	// velocity at both ends so it neither jerks on nor stops dead. The time
	// warp (t^0.6) is what puts the peak at ~31% of the duration instead of
	// halfway, which is the difference between a tap and a throb.
	float SelectionTapAmount()
	{
		if (!SelectionTapActive())
			return 0.0f;
		const float t = std::clamp(
			static_cast<float>(GetTickCount() - g_selectionTapStart) / static_cast<float>(kSelectionTapMs),
			0.0f, 1.0f);
		return std::pow(std::sin(3.14159265f * std::pow(t, 0.6f)), 1.5f);
	}

	// How much the focused tile is scaled up right now, about its own centre.
	float SelectionTapScale()
	{
		return 1.0f + kSelectionTapScale * SelectionTapAmount();
	}

	// The focused tile's halo: steady, with a small lift while the tap runs.
	float SelectionGlowAlpha()
	{
		return kSelectionGlowRest + (1.0f - kSelectionGlowRest) * SelectionTapAmount();
	}

	// Poll pacing. kTickPending keeps at most one tick in flight, so a slow
	// frame can never let the queue fill with a backlog of stale polls that
	// would then all run at once (which is what "it registers something I
	// didn't press, then starts accepting input again" looks like).
	std::atomic<bool> g_tickRunning{false};
	std::atomic<bool> g_tickPending{false};
	// Read by the pacing thread, written by the UI thread when the overlay is
	// shown or hidden - hence the atomic rather than reading overlayVisible
	// across threads.
	std::atomic<DWORD> g_tickIntervalMs{kTickIntervalHiddenMs};
	std::thread g_tickThread;

	// Cubic ease-out: fast at the start, gentle at the end. Applied to both
	// fades so they feel like they are settling rather than stopping dead.
	float EaseOutCubic(float t)
	{
		t = std::clamp(t, 0.0f, 1.0f);
		const float inv = 1.0f - t;
		return 1.0f - inv * inv * inv;
	}

	float ElapsedFraction(DWORD start, DWORD durationMs)
	{
		if (start == 0 || durationMs == 0)
			return 1.0f;
		const DWORD elapsed = GetTickCount() - start;
		return elapsed >= durationMs ? 1.0f : static_cast<float>(elapsed) / static_cast<float>(durationMs);
	}

	BYTE TargetOverlayAlpha()
	{
		const int percent = std::clamp(g_app.opacityPercent, 20, 100);
		return static_cast<BYTE>(std::clamp(percent * 255 / 100, 1, 255));
	}

	bool WindowFadeActive()
	{
		return g_windowFadeStart != 0 &&
			ElapsedFraction(g_windowFadeStart, g_windowFadingOut ? kWindowFadeOutMs : kWindowFadeInMs) < 1.0f;
	}

	// The constant alpha handed to UpdateLayeredWindow this frame.
	BYTE CurrentOverlayAlpha()
	{
		const BYTE target = TargetOverlayAlpha();
		if (g_windowFadeStart == 0)
			return target;
		const float t = EaseOutCubic(
			ElapsedFraction(g_windowFadeStart, g_windowFadingOut ? kWindowFadeOutMs : kWindowFadeInMs));
		const float scale = g_windowFadingOut ? 1.0f - t : t;
		return static_cast<BYTE>(std::clamp(static_cast<int>(target * scale + 0.5f), 0, 255));
	}

	// Starts a window fade that is already `fromShownFraction` of the way to
	// being visible (0 = invisible, 1 = fully shown). Reversing mid-fade
	// passes the fraction it had reached, so a quick double-tap of the
	// shortcut looks like the window changed its mind rather than teleporting
	// to one end and starting over. The start time is back-dated through the
	// inverse of the ease so the motion stays continuous across the reversal.
	void BeginWindowFade(bool fadingOut, float fromShownFraction)
	{
		g_windowFadingOut = fadingOut;
		const float eased = std::clamp(fadingOut ? 1.0f - fromShownFraction : fromShownFraction, 0.0f, 1.0f);
		const float linear = 1.0f - std::cbrt(1.0f - eased); // inverse of EaseOutCubic
		const DWORD duration = fadingOut ? kWindowFadeOutMs : kWindowFadeInMs;
		g_windowFadeStart = GetTickCount() - static_cast<DWORD>(linear * duration);
		if (g_windowFadeStart == 0)
			g_windowFadeStart = 1; // 0 is the "no fade running" sentinel
	}

	// How far along the current fade the window is, as a shown-ness fraction.
	float CurrentShownFraction()
	{
		const BYTE target = TargetOverlayAlpha();
		if (target == 0)
			return 0.0f;
		return std::clamp(static_cast<float>(CurrentOverlayAlpha()) / static_cast<float>(target), 0.0f, 1.0f);
	}

	bool ScreenTransitionActive()
	{
		return g_screenFadeStart != 0 && ElapsedFraction(g_screenFadeStart, kScreenFadeMs) < 1.0f;
	}

	float ScreenFadeAlpha()
	{
		if (g_screenFadeStart == 0)
			return 1.0f;
		return EaseOutCubic(ElapsedFraction(g_screenFadeStart, kScreenFadeMs));
	}

	// Detects a screen change and starts the content fade. Called from the
	// timer (so it is noticed within a tick of the change) and from Paint (so
	// a repaint that beats the timer still fades rather than popping).
	// PadViewer and PadAction are the same picture - the seven pads - with an
	// action bar added under the focused one. Cross-fading between them made
	// the whole pad grid blink every time a pad was opened or backed out of,
	// which reads as a glitch rather than a transition. They are treated as
	// one screen here, so only real screen changes fade.
	bool IsSamePadScene(Screen a, Screen b)
	{
		const auto padScene = [](Screen screen) {
			return screen == Screen::PadViewer || screen == Screen::PadAction;
		};
		return padScene(a) && padScene(b);
	}

	void SyncScreenTransition()
	{
		if (g_app.screen == g_lastTransitionScreen)
			return;
		const Screen previous = g_lastTransitionScreen;
		g_lastTransitionScreen = g_app.screen;
		if (IsSamePadScene(previous, g_app.screen))
			return;
		g_screenFadeStart = GetTickCount();
		if (g_screenFadeStart == 0)
			g_screenFadeStart = 1;
	}

	// The picker's rebindable actions, in the order they appear as Settings
	// rows. Confirm/Back stay subject to the swap-confirm-back setting on
	// top of whatever buttons they are bound to.
	struct BindableAction
	{
		const wchar_t* label;   // Settings row / status text
		const wchar_t* iniKey;  // [Input] key it persists under
		ButtonMask AppState::*button; // the binding itself
		// True for Confirm/Back, which are read on every screen and so can
		// never share a button with anything else. False for the rest,
		// which each only fire on one specific screen (scope) - two of
		// those are free to share the same default/bound button as long as
		// their screens are never both "live" at once (e.g. Favorite only
		// fires on RosterList, Move-active-pad only fires on PadViewer).
		bool global;
		Screen scope; // ignored when global is true
		// True for actions that only fire while the picker is HIDDEN (the
		// sneak-peek hold). Those can never collide with a picker action,
		// whatever screen it belongs to, because the two are live at
		// mutually exclusive times.
		bool whileHidden = false;
	};

	// Two actions need distinct buttons only if either fires on every screen,
	// or they fire on the very same screen.
	bool ActionsCanConflict(const BindableAction& a, const BindableAction& b)
	{
		if (a.whileHidden != b.whileHidden)
			return false;
		if (a.global || b.global)
			return true;
		return a.scope == b.scope;
	}

	constexpr std::array<BindableAction, 10> kBindableActions = {{
		{L"Confirm", L"ButtonConfirm", &AppState::buttonConfirm, true, Screen::PadViewer},
		{L"Back", L"ButtonBack", &AppState::buttonBack, true, Screen::PadViewer},
		{L"Settings", L"ButtonSettings", &AppState::buttonSettings, false, Screen::PadViewer},
		{L"Move active pad", L"ButtonMoveActive", &AppState::buttonMoveActive, false, Screen::PadViewer},
		{L"Quick load", L"ButtonQuickLoad", &AppState::buttonQuickLoad, false, Screen::PadViewer},
		{L"Quick clear", L"ButtonQuickClear", &AppState::buttonQuickClear, false, Screen::PadViewer},
		{L"Add to favorites", L"ButtonFavorite", &AppState::buttonFavorite, false, Screen::RosterList},
		{L"Reorganize favorites", L"ButtonReorganizeRoster", &AppState::buttonReorganizeRoster, false,
			Screen::RosterList},
		{L"Reorganize worlds", L"ButtonReorganizeFranchise", &AppState::buttonReorganizeFranchise, false,
			Screen::FranchiseList},
		{L"Sneak peek (hold)", L"ButtonSneakPeek", &AppState::buttonSneakPeek, false,
			Screen::PadViewer, true},
	}};

	// ---------------------------------------------------------------------
	// Settings model
	// ---------------------------------------------------------------------
	// The Settings screen is a scrolled list grouped into categories, laid
	// out like the franchise grid: a translucent panel, a right-edge scroll
	// bar and a viewport that follows the focus. A row is either a category
	// heading (not focusable, not activatable) or one setting.
	//
	// The same table drives painting and activation, so a row can never end
	// up wired to the wrong action - which is exactly what the old
	// "settingsIndex == 6" chain made easy to get wrong every time a row was
	// inserted.
	enum class SettingAction
	{
		Heading,
		Shortcut,
		ConfirmStyle,
		ButtonStyle,
		Binding,        // uses bindingIndex
		Background,
		PadSkin,
		Opacity,
		WindowPlacement,
		SneakPeek,
		LedMirror,
		SoundEffects,
		SoundVolume,
		ClearAllPads,
		WebRemote,
		ResetDefaults,
	};

	// Whether a row's value is an on/off state, and which way. On is drawn
	// green and Off red, so the state of every switch in the list is readable
	// without reading a single word. Settings whose value is a choice rather
	// than a switch (a wallpaper, a window mode) stay Neutral - colouring
	// "All series" green would be asserting something meaningless.
	enum class SettingValueTone { Neutral, On, Off };

	struct SettingsEntry
	{
		SettingAction action = SettingAction::Heading;
		std::wstring label;     // "Toypad LEDs", or the category name on a heading
		std::wstring value;     // "Off" - drawn separately so it can be coloured
		SettingValueTone tone = SettingValueTone::Neutral;
		ButtonMask icons = 0;   // trailing pad-button icons, 0 for none
		size_t bindingIndex = 0;
	};

	bool IsSettingsHeading(const SettingsEntry& entry)
	{
		return entry.action == SettingAction::Heading;
	}

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
		enum class Op { State, Leds, Catalog, Load, Move, Clear, ClearAll, FavoritesGet, FavoriteToggle } op = Op::State;
		int a = 0; // slot index for Load/Clear; source slot for Move
		int b = 0; // bin resource id for Load and FavoriteToggle; destination slot for Move
		std::string result; // JSON response body, written on the UI thread
		bool ok = false;
		HANDLE done = nullptr;
	};

	std::atomic<bool> g_webRunning{false};
	SOCKET g_webListenSocket = INVALID_SOCKET;
	HWND g_webWindow = nullptr;
	std::thread g_webThread;
	HWND g_mainWindow = nullptr;
	// Serialises all loopback connections to the emulator's Toypad listener:
	// the LED poll thread and the LOAD/REMOVE/MOVE sends run concurrently, and
	// the listener serves one connection at a time, so only one is ever open.
	std::mutex g_socketMutex;

	// The library catalog is static once the app is running, so it is built a
	// single time on the UI thread (it reads live background settings) and
	// then served from cache to every browser tab.
	bool g_catalogBuilt = false;
	std::string g_catalogCache;

	// Forward-declared here; defined later in the file. Confirm()/Back() run
	// before those definitions appear.
void UpdateInputOwnership(HWND window);
	void Paint(HWND window);
	void HideOverlay(HWND window);
	void OpenBrowseScreen();
	void OpenStoryRoster();
	void OpenFavoritesRoster();
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
	void ToggleLedMirror();
	void SetLedMirrorEnabled(bool enabled);
	void PositionOverlayWindow(HWND window);
	std::wstring DescribeLedMirror();
	std::wstring DescribeWebRemote();
	std::wstring GetLanAddress();
	void HandleWebJob(WebJob& job);
	void MoveSlotToSlot(size_t sourceIndex, size_t destIndex, bool updateUi);
	void SyncSelectionTap();
	void StartTickThread(HWND window);
	void StopTickThread();
	// Settings list (built from the categorised table further down).
	std::vector<SettingsEntry> BuildSettingsEntries();
	void MoveSettingsSelection(int direction);
	void ActivateSettingsEntry();
	void AdjustSettingsValue(int direction);
	void ClampSettingsSelection();
	void BuildPadSkinList();
	void CyclePadSkin(int direction);
	void CycleOpacity(int direction);
	void CycleSoundVolume(int direction);
	void CycleSneakPeek(int direction);
	std::wstring DescribeSneakPeek();
	void HidePeekWindow(bool immediate);
	void ToggleSoundEffects();
	std::wstring DescribePadSkin();
	std::wstring DescribeOpacity();
	std::wstring DescribeSoundEffects();
	std::wstring DescribeSoundVolume();

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
		LedHalo,      // per-region LED glow (tinted soft halo, cached per color+level)
		FocusGlow,    // soft pulsing halo behind the focused pad / franchise tile
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
	// UI sound effects
	// ---------------------------------------------------------------------
	// Two short WAVs embedded like every other asset (Assets/SFX): one blip
	// when the highlight moves, one when something is confirmed. Played
	// straight out of the resource bytes with PlaySound(SND_MEMORY), which
	// needs nothing on disk and no audio library - winmm is already linked
	// for the timer resolution. SND_ASYNC means a new blip cuts off the
	// previous one, which is what fast scrolling should sound like anyway.
	//
	// The resource memory belongs to the module and outlives playback, so
	// handing PlaySound a pointer into it is safe for the async case.
	// Volume-adjusted copies of the embedded WAVs, keyed by (resource, level).
	// PlaySound offers no volume control and the process-wide mixer volume is
	// not ours to touch, so the level is baked into the samples instead. The
	// copies are cached and never freed, which is what lets us hand PlaySound
	// a pointer and return immediately: an async playback keeps reading the
	// buffer after this function is done with it. There are only two sounds
	// and a handful of levels, so the cache is a few hundred KB at worst.
	std::map<std::pair<int, int>, std::vector<uint8_t>> g_scaledSounds;

	// Applies gain in place to the PCM samples of a RIFF/WAVE buffer. Walks
	// the chunk list properly rather than assuming the canonical 44-byte
	// header, because a WAV exported by any real tool tends to carry LIST or
	// fact chunks before the data. Anything that isn't 8- or 16-bit PCM is
	// left alone and simply plays at full volume.
	void ApplyWavGain(std::vector<uint8_t>& wav, int volumePercent)
	{
		if (wav.size() < 44 || std::memcmp(wav.data(), "RIFF", 4) != 0 ||
			std::memcmp(wav.data() + 8, "WAVE", 4) != 0)
			return;

		const auto readU16 = [&wav](size_t at) {
			return static_cast<uint16_t>(wav[at] | (wav[at + 1] << 8));
		};
		const auto readU32 = [&wav](size_t at) {
			return static_cast<uint32_t>(wav[at] | (wav[at + 1] << 8) |
				(wav[at + 2] << 16) | (static_cast<uint32_t>(wav[at + 3]) << 24));
		};

		uint16_t format = 0;
		uint16_t bits = 0;
		size_t dataOffset = 0;
		size_t dataSize = 0;
		size_t cursor = 12;
		while (cursor + 8 <= wav.size())
		{
			const uint32_t chunkSize = readU32(cursor + 4);
			const size_t body = cursor + 8;
			if (body > wav.size())
				break;
			const size_t available = std::min(static_cast<size_t>(chunkSize), wav.size() - body);
			if (std::memcmp(wav.data() + cursor, "fmt ", 4) == 0 && available >= 16)
			{
				format = readU16(body);
				bits = readU16(body + 14);
			}
			else if (std::memcmp(wav.data() + cursor, "data", 4) == 0)
			{
				dataOffset = body;
				dataSize = available;
				break;
			}
			// Chunks are word-aligned: an odd size is followed by a pad byte.
			cursor = body + available + (available & 1);
		}

		if (dataSize == 0 || format != 1)
			return;

		const int gain = std::clamp(volumePercent, 0, 100);
		if (bits == 16)
		{
			for (size_t i = 0; i + 1 < dataSize; i += 2)
			{
				int16_t sample = static_cast<int16_t>(
					wav[dataOffset + i] | (wav[dataOffset + i + 1] << 8));
				sample = static_cast<int16_t>(std::clamp(sample * gain / 100, -32768, 32767));
				wav[dataOffset + i] = static_cast<uint8_t>(sample & 0xFF);
				wav[dataOffset + i + 1] = static_cast<uint8_t>((sample >> 8) & 0xFF);
			}
		}
		else if (bits == 8)
		{
			// 8-bit WAV samples are unsigned with silence at 128, so the gain
			// has to be applied around that midpoint, not around zero.
			for (size_t i = 0; i < dataSize; ++i)
			{
				const int centred = static_cast<int>(wav[dataOffset + i]) - 128;
				wav[dataOffset + i] =
					static_cast<uint8_t>(std::clamp(128 + centred * gain / 100, 0, 255));
			}
		}
	}

	const std::vector<uint8_t>* GetSoundBytes(int resourceId, int volumePercent)
	{
		const auto key = std::make_pair(resourceId, volumePercent);
		const auto cached = g_scaledSounds.find(key);
		if (cached != g_scaledSounds.end())
			return &cached->second;

		std::vector<uint8_t> wav = LoadResourceBytes(resourceId);
		if (wav.empty())
			return nullptr;
		if (volumePercent < 100)
			ApplyWavGain(wav, volumePercent);
		return &g_scaledSounds.emplace(key, std::move(wav)).first->second;
	}

	void PlayUiSound(int resourceId)
	{
		if (!g_app.soundEffects || resourceId == 0 || g_app.soundVolume <= 0)
			return;
		const std::vector<uint8_t>* wav = GetSoundBytes(resourceId, g_app.soundVolume);
		if (!wav || wav->empty())
			return;
		PlaySoundW(reinterpret_cast<LPCWSTR>(wav->data()), nullptr,
			SND_MEMORY | SND_ASYNC | SND_NODEFAULT | SND_NOWAIT);
	}

	void PlayNavigateSound()
	{
		PlayUiSound(kSfxNavigateResourceId);
	}

	void PlaySelectSound()
	{
		PlayUiSound(kSfxSelectResourceId);
	}

	// Plays once a Move or Clear has actually gone through - after the wire
	// send succeeds, not on the button press that requested it - so the
	// sound confirms the pad genuinely changed rather than just that a
	// button was pressed (Select already covers that).
	void PlayMoveSound()
	{
		PlayUiSound(kSfxMoveResourceId);
	}

	void PlayRemoveSound()
	{
		PlayUiSound(kSfxRemoveResourceId);
	}

	void StopUiSounds()
	{
		PlaySoundW(nullptr, nullptr, SND_PURGE);
	}

	// ---------------------------------------------------------------------
	// Pad skins
	// ---------------------------------------------------------------------
	// The seven pad images come from a folder under Assets/Pads: "default" is
	// the built-in look, and any sibling folder holding the same seven
	// filenames is another selectable skin. Two sources feed the list:
	//
	//   - folders that existed at build time, compiled into the exe as
	//     resources (kPadSkins), and
	//   - folders dropped into "Assets/Pads/<name>" next to the .exe after
	//     the fact, loaded from disk at startup.
	//
	// Disk art can't have a resource id, so its slots get synthetic negative
	// ids (-1, -2, ...) that index g_padDiskArtPaths. Everything downstream -
	// including the gloss cache key - just carries the id, so switching skins
	// stays a repaint with no cache invalidation: each skin's pads are simply
	// different cache entries.
	struct PadSkinOption
	{
		std::wstring name;
		std::array<int, 7> artIds{};
	};

	std::vector<PadSkinOption> g_padSkins;
	std::vector<std::filesystem::path> g_padDiskArtPaths;
	std::map<int, Gdiplus::Bitmap*> g_padDiskBitmaps;

	// Filenames a skin folder must contain, in kPadCells order.
	constexpr std::array<const wchar_t*, 7> kPadSlotArtNames = {
		L"left_upper", L"center", L"right_upper",
		L"left_lower_left", L"left_lower_right",
		L"right_lower_left", L"right_lower_right",
	};

	Gdiplus::Bitmap* GetPadArtBitmap(int artId)
	{
		if (artId >= 0)
			return GetAssetBitmap(artId);

		const auto cached = g_padDiskBitmaps.find(artId);
		if (cached != g_padDiskBitmaps.end())
			return cached->second;

		const size_t pathIndex = static_cast<size_t>(-artId - 1);
		Gdiplus::Bitmap* bitmap = nullptr;
		if (pathIndex < g_padDiskArtPaths.size())
		{
			bitmap = new Gdiplus::Bitmap(g_padDiskArtPaths[pathIndex].c_str());
			if (bitmap->GetLastStatus() != Gdiplus::Ok)
			{
				delete bitmap;
				bitmap = nullptr;
			}
		}
		// A failed load is cached as null too, so a broken PNG is decoded
		// once instead of on every frame.
		g_padDiskBitmaps[artId] = bitmap;
		return bitmap;
	}

	void ReleasePadDiskBitmaps()
	{
		for (auto& [id, bitmap] : g_padDiskBitmaps)
			delete bitmap;
		g_padDiskBitmaps.clear();
	}

	// The art id for one pad slot under the active skin. Falls back to the
	// compiled-in default table if the skin list is somehow empty, so the
	// pads are never blank.
	int PadArtIdForSlot(size_t slotIndex)
	{
		if (slotIndex >= 7)
			return 0;
		if (g_padSkins.empty())
			return kPadBackgroundResourceIds[slotIndex];
		const size_t skin = std::min(g_app.padSkinIndex, g_padSkins.size() - 1);
		return g_padSkins[skin].artIds[slotIndex];
	}

	// ---------------------------------------------------------------------
	// Glossy / glowing shape renderers (each runs ONCE per cached state)
	// ---------------------------------------------------------------------

	constexpr unsigned int kGlowGold = 0x00FFCC33;    // selection glow everywhere
	// Move-source highlight: the pad you are picking a new home for. This was
	// previously named kGlowBlue but its packed value (0x00BBGGRR) actually
	// unpacked to R=224,G=150,B=90 - a dull orange, not blue at all, and with
	// only a single-pass 8px glow it read as barely-there. Reusing the vivid
	// blue this project already used for tile selection before that unified
	// on red/coral (RGB 66,157,255), and giving it the same two-pass
	// bloom+core halo as the pad/tile selection glow (see RenderFocusGlow) so
	// "you are about to move this" is unmistakable at a glance.
	constexpr unsigned int kMoveSourceGlow = 0x00FF9D42; // RGB(66, 157, 255)
	// Reorganize-source highlight: the favorite/franchise tile picked up to
	// be dropped in a new spot. Yellow, matching the "Moved: ..." status
	// toast's glow (see DrawStatusToast) so the picked-up tile and the
	// message explaining it read as the same action.
	constexpr unsigned int kReorganizeGlow = RGB(255, 210, 60);
	constexpr unsigned int kPadBorderIdle = 0x00525A6A;
	constexpr unsigned int kPadBorderOccupied = 0x0060B476;
	// One selection colour for the pad grid and the franchise grid alike. The
	// old look was a 1.5-3.25px hairline - red on the pads, blue on the
	// tiles - which disappeared against busy art and read as a defect rather
	// than a highlight. Both now use this colour as a soft halo that breathes
	// (see RenderFocusGlow / FocusPulse) plus a wide-then-crisp double stroke,
	// so the focus is obvious at a glance without a hard line anywhere.
	constexpr unsigned int kSelectionGlow = 0x004848FF; // RGB 255,72,72

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
	//
	// `outlineOnly` strips the pad back to an empty outlined box at exactly the
	// same size, position and corner shape: the printed glass art and the
	// occupant tint are both skipped, so DrawLedRegionTint's colour lands on
	// bare transparency. A colour over dark printed art reads as a muddy wash
	// and its fade is almost impossible to follow; over an empty box the hue is
	// true and every step of the ramp is visible. It follows the Toypad LEDs
	// setting rather than whether a region happens to be lit right now, so pads
	// never flip between art and outline mid-flash. It is a separate cache
	// variant, so the switch costs one extra cached bitmap per pad.
	constexpr int kPadGlowMargin = 8;
	constexpr int kPadOutlineVariantBit = 0x100;

	Gdiplus::Bitmap* RenderPad(int slotIndex, const RECT& cell, PadVisual visual, unsigned int occupantColor,
		bool outlineOnly)
	{
		const int w = (cell.right - cell.left) + kPadGlowMargin * 2;
		const int h = (cell.bottom - cell.top) + kPadGlowMargin * 2;
		const int variant = static_cast<int>(visual) | (outlineOnly ? kPadOutlineVariantBit : 0);
		const GlossKey key{GlossKind::Pad, variant, PadArtIdForSlot(static_cast<size_t>(slotIndex)),
			occupantColor, w, h};
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
		(void)moveSource; // the halo itself is now drawn externally, see DrawPad

		// The real pad art (already includes the glass/gloss look) fills the
		// cell rect; the procedural gradient and synthetic glossy streak are
		// gone. The rounded-rect clip keeps the shape's outline crisp.
		Gdiplus::Bitmap* padImage = outlineOnly
			? nullptr
			: GetPadArtBitmap(PadArtIdForSlot(static_cast<size_t>(slotIndex)));
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
		if (occupied && !outlineOnly)
		{
			Gdiplus::SolidBrush tint(Gdiplus::Color(
				42, GetRValue(occupiedColor), GetGValue(occupiedColor), GetBValue(occupiedColor)));
			g.FillPath(&tint, &path);
		}

		if (selected)
		{
			// Wide translucent stroke first, crisp thin one over it: the edge
			// keeps a definite position while still fading into the halo
			// drawn behind the pad, instead of being a single hard line.
			Gdiplus::Pen wide(Gdiplus::Color(80, GetRValue(kSelectionGlow),
				GetGValue(kSelectionGlow), GetBValue(kSelectionGlow)), 7.5f);
			wide.SetLineJoin(Gdiplus::LineJoinRound);
			g.DrawPath(&wide, &path);
			Gdiplus::Pen crisp(Gdiplus::Color(225, GetRValue(kSelectionGlow),
				GetGValue(kSelectionGlow), GetBValue(kSelectionGlow)), 2.4f);
			crisp.SetLineJoin(Gdiplus::LineJoinRound);
			g.DrawPath(&crisp, &path);
		}
		else
		{
			const unsigned int border = moveSource ? kMoveSourceGlow
				: (occupied ? occupiedColor : kPadBorderIdle);
			// A bit heavier than the plain idle/occupied edge, so the crisp
			// line itself - not just the halo behind it - reads as
			// deliberately emphasized rather than incidental.
			Gdiplus::Pen borderPen(
				Gdiplus::Color(255, GetRValue(border), GetGValue(border), GetBValue(border)),
				moveSource ? 2.75f : 2.0f);
			borderPen.SetLineJoin(Gdiplus::LineJoinRound);
			g.DrawPath(&borderPen, &path);
		}

		g_glossCache[key] = bitmap;
		return bitmap;
	}

	// ---------------------------------------------------------------------
	// Focus glow: the soft, slowly breathing halo behind whatever is selected
	// ---------------------------------------------------------------------
	// The halo itself is expensive (two box-blurred silhouettes) but it is
	// built ONCE per shape+size+colour and cached like every other surface.
	// The pulse is not baked into it: the cached bitmap is drawn at full
	// strength and the breathing comes from scaling its alpha at blit time
	// with a colour matrix, which is one DrawImage. So a pulsing selection
	// costs the same per frame as the old static outline did - no per-frame
	// blur, no cache entry per animation step, and nothing to invalidate when
	// the phase moves.
	constexpr int kFocusGlowMargin = 20;
	constexpr float kFocusPulsePeriodMs = 2400.0f; // one slow breath
	// The breath is quantised to this many brightness steps. Nothing about the
	// drawing needs it - the alpha is continuous - but the repaint decision
	// does: a continuously varying alpha means "repaint forever at whatever
	// frame rate we pick", while a stepped one means "repaint only when the
	// picture would actually differ". At 24 steps over 2.4s that is ~19
	// repaints a second on average and none at all when the breath is near
	// its turning points, and the steps are ~2.4% of alpha apart, which is
	// far below what the eye resolves in a soft glow.
	constexpr int kFocusPulseSteps = 24;

	enum class FocusShape : int
	{
		RoundedPad,      // pad tiles (14px corners)
		Circle,          // the round centre pad
		RoundedTile,     // franchise tiles (12px corners)
		Pill,            // fully rounded ends (radius = height / 2) - the status toast
	};

	// Which step of the breath the wall clock is on. Every focused element
	// shares the phase, so the whole screen breathes together instead of
	// drifting apart element by element, and the repaint gate in the tick
	// handler watches this same number.
	int FocusPulseStep()
	{
		const float phase =
			static_cast<float>(GetTickCount() % static_cast<DWORD>(kFocusPulsePeriodMs)) / kFocusPulsePeriodMs;
		const float raw = 0.5f - 0.5f * std::cos(phase * 6.28318530718f);
		return std::clamp(static_cast<int>(raw * (kFocusPulseSteps - 1) + 0.5f), 0, kFocusPulseSteps - 1);
	}

	// 0..1, stepped. Drawing uses the same quantised value the repaint gate
	// tests, so a frame is never skipped while the painted alpha keeps
	// drifting underneath it.
	float FocusPulse()
	{
		return static_cast<float>(FocusPulseStep()) / static_cast<float>(kFocusPulseSteps - 1);
	}

	// Screens that animate continuously and therefore need repainting while
	// they are up. The pad and franchise grids are deliberately NOT in this
	// list: their selection is a steady glow plus a one-shot tap, so they are
	// completely idle between keypresses.
	bool ScreenHasPulsingFocus()
	{
		return false;
	}

	// DrawImage with the whole bitmap's alpha scaled by `alpha` (0..1), into
	// an arbitrary destination rectangle. Used by the selection tap (scaled
	// about a centre) and by the screen-transition fade (moved, not scaled).
	void DrawImageWithAlpha(Gdiplus::Graphics& g, Gdiplus::Bitmap* bitmap,
		const Gdiplus::RectF& dest, float alpha)
	{
		if (!bitmap || alpha <= 0.004f)
			return;
		const int w = static_cast<int>(bitmap->GetWidth());
		const int h = static_cast<int>(bitmap->GetHeight());
		const bool native = std::fabs(dest.Width - w) < 0.5f && std::fabs(dest.Height - h) < 0.5f;
		if (alpha >= 0.999f && native)
		{
			// The (x, y) overload draws at the bitmap's own size with no
			// scaler in the path at all; a rect overload can still go through
			// the interpolator even at 1:1.
			g.DrawImage(bitmap, static_cast<int>(dest.X + 0.5f), static_cast<int>(dest.Y + 0.5f));
			return;
		}
		if (alpha >= 0.999f)
		{
			g.DrawImage(bitmap, dest);
			return;
		}
		Gdiplus::ColorMatrix matrix = {
			{{1.0f, 0.0f, 0.0f, 0.0f, 0.0f},
			 {0.0f, 1.0f, 0.0f, 0.0f, 0.0f},
			 {0.0f, 0.0f, 1.0f, 0.0f, 0.0f},
			 {0.0f, 0.0f, 0.0f, alpha, 0.0f},
			 {0.0f, 0.0f, 0.0f, 0.0f, 1.0f}}};
		Gdiplus::ImageAttributes attributes;
		attributes.SetColorMatrix(&matrix, Gdiplus::ColorMatrixFlagsDefault, Gdiplus::ColorAdjustTypeBitmap);
		g.DrawImage(bitmap, dest, 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h),
			Gdiplus::UnitPixel, &attributes);
	}

	void DrawImageWithAlpha(Gdiplus::Graphics& g, Gdiplus::Bitmap* bitmap, int x, int y, float alpha)
	{
		if (!bitmap)
			return;
		DrawImageWithAlpha(g, bitmap, Gdiplus::RectF(static_cast<float>(x), static_cast<float>(y),
			static_cast<float>(bitmap->GetWidth()), static_cast<float>(bitmap->GetHeight())), alpha);
	}

	// Draws `bitmap` at (x, y) scaled by `scale` about the point (cx, cy) - so
	// a tile grows into its own centre instead of towards the top-left. At
	// scale 1 this collapses to a plain blit.
	void DrawImageScaledAbout(Gdiplus::Graphics& g, Gdiplus::Bitmap* bitmap,
		float x, float y, float cx, float cy, float scale, float alpha = 1.0f)
	{
		if (!bitmap)
			return;
		const float w = static_cast<float>(bitmap->GetWidth());
		const float h = static_cast<float>(bitmap->GetHeight());
		const Gdiplus::RectF dest(cx - (cx - x) * scale, cy - (cy - y) * scale, w * scale, h * scale);
		DrawImageWithAlpha(g, bitmap, dest, alpha);
	}

	// Blurred halo of one shape, padded by kFocusGlowMargin on every side.
	// Two passes at different radii: a tight bright core right at the edge
	// and a wide faint bloom around it, which is what makes it read as light
	// rather than as a fat second outline.
	Gdiplus::Bitmap* RenderFocusGlow(int shapeW, int shapeH, FocusShape shape, unsigned int color)
	{
		const int w = shapeW + kFocusGlowMargin * 2;
		const int h = shapeH + kFocusGlowMargin * 2;
		const GlossKey key{GlossKind::FocusGlow, static_cast<int>(shape), 0, color, w, h};
		const auto cached = g_glossCache.find(key);
		if (cached != g_glossCache.end())
			return cached->second;

		Gdiplus::Bitmap* bitmap = new Gdiplus::Bitmap(w, h, PixelFormat32bppPARGB);
		Gdiplus::Graphics g(bitmap);
		g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

		const Gdiplus::RectF rect(static_cast<float>(kFocusGlowMargin), static_cast<float>(kFocusGlowMargin),
			static_cast<float>(shapeW), static_cast<float>(shapeH));
		Gdiplus::GraphicsPath path;
		switch (shape)
		{
		case FocusShape::Circle:
			path.AddEllipse(rect);
			path.CloseFigure();
			break;
		case FocusShape::RoundedTile:
			AddRoundedRectPath(path, rect, 12.0f);
			break;
		case FocusShape::RoundedPad:
			AddRoundedRectPath(path, rect, 14.0f);
			break;
		case FocusShape::Pill:
			AddRoundedRectPath(path, rect, std::min(shapeW, shapeH) / 2.0f);
			break;
		}

		DrawGlow(g, path, color, 13, w, h); // wide bloom
		DrawGlow(g, path, color, 5, w, h);  // tight core

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

	// Trimmed art bounds per resource id. The scan below is a full-size probe
	// bitmap plus a per-pixel alpha sweep, and the answer only depends on the
	// image - so it is computed once per asset rather than once per (asset,
	// size, focus state) portrait variant.
	std::map<int, Gdiplus::Rect> g_opaqueBoundsCache;

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

	// Cached front end for the above, keyed by resource id.
	Gdiplus::Rect GetOpaqueContentBoundsCached(int resId, Gdiplus::Bitmap* image)
	{
		const auto cached = g_opaqueBoundsCache.find(resId);
		if (cached != g_opaqueBoundsCache.end())
			return cached->second;
		const Gdiplus::Rect bounds = GetOpaqueContentBounds(image);
		g_opaqueBoundsCache[resId] = bounds;
		return bounds;
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
			const Gdiplus::Rect source = GetOpaqueContentBoundsCached(resId, photo);
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

		if (focused)
		{
			// Same double stroke as a focused pad, in the same colour: the
			// two grids are the same kind of "pick one of these", so they now
			// look the same when you do.
			Gdiplus::Pen wide(Gdiplus::Color(80, GetRValue(kSelectionGlow),
				GetGValue(kSelectionGlow), GetBValue(kSelectionGlow)), 6.5f);
			wide.SetLineJoin(Gdiplus::LineJoinRound);
			g.DrawPath(&wide, &path);
			Gdiplus::Pen crisp(Gdiplus::Color(225, GetRValue(kSelectionGlow),
				GetGValue(kSelectionGlow), GetBValue(kSelectionGlow)), 2.2f);
			crisp.SetLineJoin(Gdiplus::LineJoinRound);
			g.DrawPath(&crisp, &path);
		}
		else
		{
			Gdiplus::Pen borderPen(Gdiplus::Color(200, GetRValue(kPadBorderIdle),
				GetGValue(kPadBorderIdle), GetBValue(kPadBorderIdle)), 1.5f);
			borderPen.SetLineJoin(Gdiplus::LineJoinRound);
			g.DrawPath(&borderPen, &path);
		}

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
			// "No Background": nothing is drawn at all, so the frame stays
			// fully transparent (alpha 0) and everything the UI doesn't
			// cover shows the game. No legibility overlay either - there is
			// nothing to darken.
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
		Gdiplus::SolidBrush overlay(Gdiplus::Color(40, 8, 10, 16));
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

	// Small filled 5-point star marking an already-favorited tile - drawn as
	// a vector path (no bundled asset needed) in the top-right corner of a
	// roster/build-picker portrait.
	void DrawFavoriteStar(Gdiplus::Graphics& g, float centerX, float centerY, float outerRadius)
	{
		constexpr float kInnerRatio = 0.42f;
		Gdiplus::PointF points[10];
		for (int i = 0; i < 10; ++i)
		{
			const float radius = (i % 2 == 0) ? outerRadius : outerRadius * kInnerRatio;
			const float angle = -1.57079632679f + i * 3.14159265358979323846f / 5.0f;
			points[i] = Gdiplus::PointF(centerX + radius * std::cos(angle), centerY + radius * std::sin(angle));
		}
		Gdiplus::GraphicsPath path;
		path.AddPolygon(points, 10);
		Gdiplus::SolidBrush outline(Gdiplus::Color(220, 20, 22, 28));
		g.FillPath(&outline, &path); // slightly larger dark base for contrast, see scale below
		Gdiplus::Matrix shrink;
		shrink.Translate(centerX, centerY);
		shrink.Scale(0.78f, 0.78f);
		shrink.Translate(-centerX, -centerY);
		Gdiplus::GraphicsPath innerPath;
		innerPath.AddPolygon(points, 10);
		innerPath.Transform(&shrink);
		Gdiplus::SolidBrush star(Gdiplus::Color(255, 255, 205, 60));
		g.FillPath(&star, &innerPath);
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

	// One "<icon> Label" hint, right-aligned so its right edge lands at
	// rightX - mirrors the built-in Settings hint's layout (see Paint), but
	// draws the label with the UI font directly since there's no bundled
	// text-image asset for these. Returns the total width used, so several
	// hints can stack leftward from the same right edge.
	float DrawButtonHint(Gdiplus::Graphics& g, ButtonMask mask, const std::wstring& label,
		float rightX, float centerY, float h)
	{
		constexpr float kGap = 8.0f;
		constexpr float kTextPx = 22.0f;
		const float buttonW = DrawPadButtonMask(g, mask, 0.0f, 0.0f, h, false);
		const float textW = MeasureTextWidth(g, label, kTextPx);
		const float totalW = buttonW + kGap + textW;
		const float originX = rightX - totalW;
		DrawPadButtonMask(g, mask, originX, centerY - h / 2.0f, h, true);
		DrawTextLineCentered(g, label, static_cast<int>(originX + buttonW + kGap),
			static_cast<int>(centerY - h / 2.0f), static_cast<int>(textW) + 4, RGB(226, 232, 240),
			static_cast<int>(h));
		return totalW;
	}

	// "<LB> <RB> Sort" hint on the left side of the screen, so it stays clear
	// of the right-aligned Favorite/Organize hints. Both shoulder buttons are
	// drawn side by side and share a single label, left-aligned at leftX.
	float DrawButtonHintLeft(Gdiplus::Graphics& g, ButtonMask mask1, ButtonMask mask2,
		const std::wstring& label, float leftX, float centerY, float h)
	{
		constexpr float kIconGap = 5.0f;
		constexpr float kGap = 8.0f;
		constexpr float kTextPx = 22.0f;
		float cursor = leftX;
		cursor += DrawPadButtonMask(g, mask1, cursor, centerY - h / 2.0f, h, true);
		cursor += kIconGap;
		cursor += DrawPadButtonMask(g, mask2, cursor, centerY - h / 2.0f, h, true);
		cursor += kGap;
		const float textW = MeasureTextWidth(g, label, kTextPx);
		DrawTextLineCentered(g, label, static_cast<int>(cursor),
			static_cast<int>(centerY - h / 2.0f), static_cast<int>(textW) + 4, RGB(226, 232, 240),
			static_cast<int>(h));
		return cursor - leftX;
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
"; BackgroundIndex walks the Background row's own order, which is NOT the\n"
			"; raw wallpaper order: 0 is the default wallpaper, 1 is Clear (the overlay\n"
			"; is drawn transparent between the UI elements so the game shows through),\n"
			"; and 2 upwards are the remaining bundled Assets/Wallpapers images.\n"
			"BackgroundIndex=0\n"
			"; FranchiseSort = 0 Default (all worlds, alphabetical), 1 User (custom\n"
			"; order), 2 Story (starter pack characters/vehicles), 3 Favorites.\n"
			"; Browsed with the shoulder buttons (RB/LB) on the world screen.\n"
			"FranchiseSort=0\n"
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
			"ButtonFavorite=32768\n"
			"; Reorganize picks a Favorites-roster or franchise-grid tile up so the\n"
			"; next navigation + Confirm drops it in a new spot.\n"
			"ButtonReorganizeRoster=16384\n"
			"ButtonReorganizeFranchise=16384\n"
			"; Held, not tapped: the sneak-peek HUD is up for exactly as long as\n"
			"; this button is down. 65536 is LT.\n"
			"ButtonSneakPeek=65536\n"
			"; ButtonStyle picks the button icons/names shown in the UI:\n"
			"; Auto | Xbox | DualShock4 | Switch. Auto follows the connected pad\n"
			"; (Xbox wins when several are plugged in). Icons only - every pad works\n"
			"; either way, and the values above stay raw XInput numbers.\n"
			"ButtonStyle=Auto\n"
			"; ToypadLeds = 1 mirrors the running game's toypad LEDs onto the pads.\n"
			"; It is off by default: mirroring redraws the pads as bare colour boxes\n"
			"; and polls the emulator's listener ~30 times a second.\n"
			"ToypadLeds=0\n"
			"; SoundEffects = 1 plays Assets/SFX/Navigate.wav when the highlight moves\n"
			"; and Select.wav when something is confirmed.\n"
			"SoundEffects=1\n"
			"; SoundVolume is the playback level, 0-100. PlaySound has no volume\n"
			"; control of its own, so the level is baked into a cached copy of the\n"
			"; samples. The Settings row offers 100/85/70/55/40.\n"
			"SoundVolume=70\n"
			"; SneakPeek is the click-through HUD you get by HOLDING ButtonSneakPeek\n"
			"; while the game runs: the same seven pads, drawn over the game, with\n"
			"; the game still playing (unlike the picker, which pauses input for the\n"
			"; emulator while it is open). 0 = off, 1 = small, 2 = medium, 3 = large.\n"
			"SneakPeek=2\n"
			"; PadSkin names the folder under Assets/Pads whose seven images are drawn\n"
			"; as the toypad. \"Default\" is the built-in art. Any folder holding the\n"
			"; same seven filenames (left_upper, center, right_upper, left_lower_left,\n"
			"; left_lower_right, right_lower_left, right_lower_right) is another skin -\n"
			"; including ones you drop into Assets/Pads next to this exe after install,\n"
			"; which are picked up at startup and switchable from Settings.\n"
			"PadSkin=Default\n"
			"\n"
			"; Written by the app when you change the Window row in Settings.\n"
			"[Window]\n"
			"; Draggable = 0 keeps the overlay fixed: it is centred on the monitor\n"
			"; the game is on every time it is shown. Draggable = 1 lets you drag it\n"
			"; anywhere with the left mouse button and remembers where you dropped it.\n"
			"Draggable=0\n"
			"; RememberedPosition = 1 means PositionX/Y below hold a real dragged\n"
			"; spot; 0 means the overlay has never been moved and is still centred.\n"
			"; The remembered spot is ignored (and the overlay re-centres) if it no\n"
			"; longer lands on any connected monitor.\n"
			"RememberedPosition=0\n"
			"PositionX=0\n"
			"PositionY=0\n"
			"; Opacity is the whole panel's translucency, as a percentage of solid.\n"
			"; The Settings row cycles 100/92/85/75/65/55/45; hand-edited values are\n"
			"; clamped to 20-100 so the window can never become impossible to see.\n"
			"Opacity=92\n"
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

	// backgroundIndex walks a display order that is NOT the bundled wallpaper
	// order: position 0 is the default wallpaper, position 1 is the clear
	// (transparent) backdrop, and positions 2..N are the remaining
	// wallpapers. Clear sits second because it is the one people reach for -
	// it is what turns the overlay into a HUD over the running game - and
	// burying it past a dozen wallpapers made it feel like an afterthought.
	// Nothing is drawn behind the UI for it and those pixels stay at alpha 0
	// (see ApplyOverlayTransparency).
	constexpr size_t kClearBackgroundPosition = 1;

	size_t BackgroundOptionCount()
	{
		return kBackgroundChoiceCount + 1; // wallpapers + the clear backdrop
	}

	size_t ClampBackgroundIndex(size_t index)
	{
		return std::min(index, BackgroundOptionCount() - 1);
	}

	bool IsNoBackground()
	{
		if (kBackgroundChoiceCount == 0)
			return true; // nothing bundled: clear is the only thing left
		return ClampBackgroundIndex(g_app.backgroundIndex) == kClearBackgroundPosition;
	}

	const BackgroundChoice* CurrentBackgroundChoice()
	{
		if (kBackgroundChoiceCount == 0)
			return nullptr;
		g_app.backgroundIndex = ClampBackgroundIndex(g_app.backgroundIndex);
		if (g_app.backgroundIndex == kClearBackgroundPosition)
			return nullptr; // Clear
		// Everything past the clear slot is shifted back onto the wallpaper
		// table it came from.
		const size_t wallpaper = g_app.backgroundIndex < kClearBackgroundPosition
			? g_app.backgroundIndex
			: g_app.backgroundIndex - 1;
		return &kBackgroundChoices[std::min(wallpaper, kBackgroundChoiceCount - 1)];
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
			return L"Clear (see the game through it)";
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
		WritePrivateProfileStringW(L"Input", L"FranchiseSort",
			std::to_wstring(static_cast<int>(g_app.franchiseSort)).c_str(), iniPath.c_str());
		WritePrivateProfileStringW(L"Input", L"ButtonStyle",
			ButtonStyleIniValue(g_app.buttonStyleChoice), iniPath.c_str());
		WritePrivateProfileStringW(L"Input", L"ToypadLeds",
			g_app.ledMirrorEnabled ? L"1" : L"0", iniPath.c_str());
		WritePrivateProfileStringW(L"Input", L"SoundEffects",
			g_app.soundEffects ? L"1" : L"0", iniPath.c_str());
		WritePrivateProfileStringW(L"Input", L"SoundVolume",
			std::to_wstring(g_app.soundVolume).c_str(), iniPath.c_str());
		WritePrivateProfileStringW(L"Input", L"SneakPeek",
			std::to_wstring(g_app.peekSizeChoice).c_str(), iniPath.c_str());
		// Stored by folder name, not by index: adding or removing a skin
		// folder must never silently repoint this at a different one.
		WritePrivateProfileStringW(L"Input", L"PadSkin",
			g_app.padSkinIndex < g_padSkins.size() ? g_padSkins[g_app.padSkinIndex].name.c_str() : L"Default",
			iniPath.c_str());
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
		// Franchise grid sort. Clamp into the valid range so a stale/bogus ini
		// value can never index out of the sort enum.
		{
			const int sortVal = GetPrivateProfileIntW(
				L"Input", L"FranchiseSort", 0, iniPath.c_str());
			const int maxSort = static_cast<int>(AppState::FranchiseSort::Favorites);
			g_app.franchiseSort = static_cast<AppState::FranchiseSort>(
				std::clamp(sortVal, 0, maxSort));
		}
		std::array<wchar_t, 32> styleBuffer{};
		GetPrivateProfileStringW(L"Input", L"ButtonStyle", L"Auto", styleBuffer.data(),
			static_cast<DWORD>(styleBuffer.size()), iniPath.c_str());
		g_app.buttonStyleChoice = ButtonStyleChoiceFromIni(styleBuffer.data());
		g_app.ledMirrorEnabled =
			GetPrivateProfileIntW(L"Input", L"ToypadLeds", 0, iniPath.c_str()) != 0;
		g_app.soundEffects =
			GetPrivateProfileIntW(L"Input", L"SoundEffects", 1, iniPath.c_str()) != 0;
		g_app.soundVolume = std::clamp(static_cast<int>(GetPrivateProfileIntW(
			L"Input", L"SoundVolume", 70, iniPath.c_str())), 0, 100);
		g_app.peekSizeChoice = static_cast<size_t>(std::clamp(static_cast<int>(
			GetPrivateProfileIntW(L"Input", L"SneakPeek",
				static_cast<INT>(kDefaultPeekSizeChoice), iniPath.c_str())),
			0, kPeekSizeChoiceCount - 1));
		std::array<wchar_t, 96> padSkinBuffer{};
		GetPrivateProfileStringW(L"Input", L"PadSkin", L"Default", padSkinBuffer.data(),
			static_cast<DWORD>(padSkinBuffer.size()), iniPath.c_str());
		g_app.padSkinIndex = 0;
		for (size_t i = 0; i < g_padSkins.size(); ++i)
		{
			if (_wcsicmp(g_padSkins[i].name.c_str(), padSkinBuffer.data()) == 0)
			{
				g_app.padSkinIndex = i;
				break;
			}
		}
		// A hand-edited value falls back to the built-in default when it is
		// unset (0) or a D-pad button, and is dropped entirely (leaving the
		// action unbound, shown as "(none)" in Settings) when it collides
		// with an earlier action it can actually conflict with (see
		// ActionsCanConflict) - one press firing two actions on the same
		// screen is worse than one action the user can simply rebind.
		std::array<ButtonMask, kBindableActions.size()> assigned{};
		for (size_t i = 0; i < kBindableActions.size(); ++i)
		{
			const auto& action = kBindableActions[i];
			const ButtonMask stored = static_cast<ButtonMask>(GetPrivateProfileIntW(
				L"Input", action.iniKey, g_app.*(action.button), iniPath.c_str()));
			ButtonMask value = (stored != 0 && (stored & kDpadButtons) == 0) ? stored : g_app.*(action.button);
			for (size_t j = 0; j < i; ++j)
			{
				if (value != 0 && assigned[j] == value && ActionsCanConflict(action, kBindableActions[j]))
				{
					value = 0;
					break;
				}
			}
			assigned[i] = value;
			g_app.*(action.button) = value;
		}
	}

	// Favorites persist as an indexed list under [Favorites]: Count, then
	// Item0..ItemN-1 each "<0/1 isVehicle>|<franchise>|<name>|<buildNumber>".
	// Indexed rather than one key per entry keyed by name, since favorites
	// are added/removed at runtime and the section needs to shrink cleanly
	// too. buildNumber pins a specific build of a multi-build vehicle so two
	// builds of the same family can be favorited independently; it's 0 for
	// characters.
	void SaveFavoritesToIni()
	{
		const auto iniPath = GetExecutableDirectory() / L"LegoToypad.ini";
		// Clear the whole section first so a shorter list doesn't leave
		// stale ItemN rows behind from a previous, longer one.
		WritePrivateProfileStringW(L"Favorites", nullptr, nullptr, iniPath.c_str());
		WritePrivateProfileStringW(L"Favorites", L"Count",
			std::to_wstring(g_app.favorites.size()).c_str(), iniPath.c_str());
		for (size_t i = 0; i < g_app.favorites.size(); ++i)
		{
			const auto& fav = g_app.favorites[i];
			const std::wstring key = L"Item" + std::to_wstring(i);
			const std::wstring value =
				std::wstring(fav.isVehicle ? L"1|" : L"0|") + fav.franchise + L"|" + fav.name +
				L"|" + std::to_wstring(fav.buildNumber);
			WritePrivateProfileStringW(L"Favorites", key.c_str(), value.c_str(), iniPath.c_str());
		}
	}

	void LoadFavoritesFromIni()
	{
		const auto iniPath = GetExecutableDirectory() / L"LegoToypad.ini";
		g_app.favorites.clear();
		const int count = static_cast<int>(GetPrivateProfileIntW(L"Favorites", L"Count", 0, iniPath.c_str()));
		std::array<wchar_t, 512> buffer{};
		for (int i = 0; i < count; ++i)
		{
			const std::wstring key = L"Item" + std::to_wstring(i);
			GetPrivateProfileStringW(L"Favorites", key.c_str(), L"", buffer.data(),
				static_cast<DWORD>(buffer.size()), iniPath.c_str());
			const std::wstring value = buffer.data();
			if (value.empty())
				continue;
			const size_t firstBar = value.find(L'|');
			if (firstBar == std::wstring::npos)
				continue;
			const size_t secondBar = value.find(L'|', firstBar + 1);
			if (secondBar == std::wstring::npos)
				continue;
			// The buildNumber segment is optional - an ini written before
			// per-variant favoriting existed simply won't have it, and that
			// entry resolves to build 1 (see FavoriteEntry::buildNumber).
			const size_t thirdBar = value.find(L'|', secondBar + 1);
			FavoriteEntry entry;
			entry.isVehicle = value.substr(0, firstBar) == L"1";
			entry.franchise = value.substr(firstBar + 1, secondBar - firstBar - 1);
			entry.name = thirdBar == std::wstring::npos
				? value.substr(secondBar + 1)
				: value.substr(secondBar + 1, thirdBar - secondBar - 1);
			if (thirdBar != std::wstring::npos)
			{
				try { entry.buildNumber = std::stoi(value.substr(thirdBar + 1)); }
				catch (...) { entry.buildNumber = 0; }
			}
			if (!entry.franchise.empty() && !entry.name.empty())
				g_app.favorites.push_back(std::move(entry));
		}
	}

	// Custom franchise/world display order, persisted the same shape as
	// Favorites but under [FranchiseOrder], each value a franchise name (not
	// an index - the generated catalog's own indices can shift when it's
	// regenerated, names are stable). See AppState::franchiseDisplayOrder.
	void SaveFranchiseOrderToIni()
	{
		const auto iniPath = GetExecutableDirectory() / L"LegoToypad.ini";
		WritePrivateProfileStringW(L"FranchiseOrder", nullptr, nullptr, iniPath.c_str());
		WritePrivateProfileStringW(L"FranchiseOrder", L"Count",
			std::to_wstring(g_app.franchiseDisplayOrder.size()).c_str(), iniPath.c_str());
		for (size_t i = 0; i < g_app.franchiseDisplayOrder.size(); ++i)
		{
			const size_t franchiseIndex = g_app.franchiseDisplayOrder[i];
			if (franchiseIndex >= kFranchiseCount)
				continue;
			const std::wstring key = L"Item" + std::to_wstring(i);
			WritePrivateProfileStringW(L"FranchiseOrder", key.c_str(),
				kFranchises[franchiseIndex].name.c_str(), iniPath.c_str());
		}
	}

	void LoadFranchiseOrderFromIni()
	{
		const auto iniPath = GetExecutableDirectory() / L"LegoToypad.ini";
		g_app.franchiseDisplayOrder.clear();
		std::vector<bool> used(kFranchiseCount, false);
		const int count = static_cast<int>(
			GetPrivateProfileIntW(L"FranchiseOrder", L"Count", 0, iniPath.c_str()));
		std::array<wchar_t, 256> buffer{};
		for (int i = 0; i < count; ++i)
		{
			const std::wstring key = L"Item" + std::to_wstring(i);
			GetPrivateProfileStringW(L"FranchiseOrder", key.c_str(), L"", buffer.data(),
				static_cast<DWORD>(buffer.size()), iniPath.c_str());
			const std::wstring name = buffer.data();
			if (name.empty())
				continue;
			for (size_t f = 0; f < kFranchiseCount; ++f)
			{
				if (used[f] || kFranchises[f].name != name)
					continue;
				used[f] = true;
				g_app.franchiseDisplayOrder.push_back(f);
				break;
			}
		}
		// Anything not mentioned in the ini (a fresh install, or a franchise
		// added since the order was saved) is appended in catalog order.
		for (size_t f = 0; f < kFranchiseCount; ++f)
		{
			if (!used[f])
				g_app.franchiseDisplayOrder.push_back(f);
		}
	}

	void CycleButtonStyle(int direction)
	{
		const size_t step = direction < 0 ? kButtonStyleChoiceCount - 1 : 1;
		g_app.buttonStyleChoice = (g_app.buttonStyleChoice + step) % kButtonStyleChoiceCount;
		SaveInputSettingsToIni();
		g_app.status = L"Button labels: " + DescribeButtonStyle();
	}

	void ToggleConfirmButtonMode()
	{
		g_app.swapConfirmBackButtons = !g_app.swapConfirmBackButtons;
		SaveInputSettingsToIni();
		g_app.status = L"Confirm button: " + DescribeConfirmButtonMode();
	}

	// The frame carries its own alpha channel and reaches the screen through
	// UpdateLayeredWindow (see Paint), so switching backgrounds needs nothing
	// more than a repaint: "No Background" simply leaves those pixels at
	// alpha 0. SetLayeredWindowAttributes must never be called on this window
	// - it would put it back into whole-pixel (color-key) transparency and
	// make every later UpdateLayeredWindow fail.
	void ApplyOverlayTransparency(HWND window)
	{
		if (!window)
			return;
		InvalidateRect(window, nullptr, FALSE);
	}

	void CycleBackgroundChoice(int direction)
	{
		if (kBackgroundChoiceCount == 0)
		{
			g_app.status = L"No backgrounds are bundled in Assets/Wallpapers.";
			return;
		}
		// The cycle covers every bundled wallpaper plus the clear backdrop
		// wedged in at position 1. `direction` lets the Settings row be walked
		// backwards with Left as well as forwards with Right / Confirm.
		const size_t count = BackgroundOptionCount();
		const size_t step = direction < 0 ? count - 1 : 1;
		g_app.backgroundIndex = (ClampBackgroundIndex(g_app.backgroundIndex) + step) % count;
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
	// Window placement settings
	// ---------------------------------------------------------------------

	void SaveWindowSettingsToIni()
	{
		const auto iniPath = GetExecutableDirectory() / L"LegoToypad.ini";
		WritePrivateProfileStringW(L"Window", L"Draggable",
			g_app.windowDraggable ? L"1" : L"0", iniPath.c_str());
		// A window that has never been dragged has no saved spot, and the
		// difference matters: an unset X/Y must fall back to centring rather
		// than pin the overlay to the top-left corner of the desktop.
		WritePrivateProfileStringW(L"Window", L"RememberedPosition",
			g_app.hasSavedWindowPos ? L"1" : L"0", iniPath.c_str());
		WritePrivateProfileStringW(L"Window", L"PositionX",
			std::to_wstring(g_app.savedWindowX).c_str(), iniPath.c_str());
		WritePrivateProfileStringW(L"Window", L"PositionY",
			std::to_wstring(g_app.savedWindowY).c_str(), iniPath.c_str());
		WritePrivateProfileStringW(L"Window", L"Opacity",
			std::to_wstring(g_app.opacityPercent).c_str(), iniPath.c_str());
	}

	void LoadWindowSettingsFromIni()
	{
		const auto iniPath = GetExecutableDirectory() / L"LegoToypad.ini";
		g_app.windowDraggable =
			GetPrivateProfileIntW(L"Window", L"Draggable", 0, iniPath.c_str()) != 0;
		g_app.hasSavedWindowPos =
			GetPrivateProfileIntW(L"Window", L"RememberedPosition", 0, iniPath.c_str()) != 0;
		// GetPrivateProfileIntW is unsigned and refuses negatives, so the
		// coordinates are read as text: a window parked on a monitor left of
		// or above the primary one has a negative origin, and that is a
		// perfectly ordinary place to have left it.
		std::array<wchar_t, 32> buffer{};
		GetPrivateProfileStringW(L"Window", L"PositionX", L"0", buffer.data(),
			static_cast<DWORD>(buffer.size()), iniPath.c_str());
		g_app.savedWindowX = _wtoi(buffer.data());
		GetPrivateProfileStringW(L"Window", L"PositionY", L"0", buffer.data(),
			static_cast<DWORD>(buffer.size()), iniPath.c_str());
		g_app.savedWindowY = _wtoi(buffer.data());
		// Clamped rather than rejected: a hand-edited 5% would leave a window
		// nobody can see well enough to fix the setting from.
		g_app.opacityPercent = std::clamp(static_cast<int>(GetPrivateProfileIntW(
			L"Window", L"Opacity", kDefaultOpacityPercent, iniPath.c_str())), kOpacityFloorPercent, 100);
	}

	std::wstring DescribeWindowPlacement()
	{
		if (!g_app.windowDraggable)
			return L"Fixed (centred)";
		return g_app.hasSavedWindowPos ? L"Draggable (position remembered)" : L"Draggable";
	}

	void ToggleWindowDraggable()
	{
		g_app.windowDraggable = !g_app.windowDraggable;
		if (!g_app.windowDraggable)
		{
			// Going back to Fixed forgets the dragged spot and re-centres
			// right away, so the row's new value is something you can see
			// rather than something that only takes effect on the next show.
			if (g_app.draggingWindow)
			{
				g_app.draggingWindow = false;
				if (GetCapture() == g_mainWindow)
					ReleaseCapture();
			}
			g_app.hasSavedWindowPos = false;
			if (g_mainWindow && g_app.overlayVisible)
				PositionOverlayWindow(g_mainWindow);
		}
		SaveWindowSettingsToIni();
		g_app.status = L"Window: " + DescribeWindowPlacement();
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

	// Milliseconds a connect() is allowed before this app gives up on it.
	// Every caller of this helper runs on the UI thread (SendToypadMessage),
	// so a hang here is a hang of the whole window - no repaint, no input,
	// nothing - for however long it lasts. A plain blocking connect() has no
	// such ceiling: Windows' own default TCP connect timeout runs into many
	// seconds, and that is exactly what a closed or unresponsive listener
	// port (no emulator running, or one that silently drops the connection)
	// was hitting, over and over. 250ms is generous for a real loopback
	// listener - those accept in well under a millisecond - and still short
	// enough that a missing one is a brief hitch, not a freeze.
	constexpr int kSocketConnectTimeoutMs = 250;

	// connect() with a bounded wait instead of an unbounded one. The socket is
	// switched to non-blocking only for the handshake and always restored to
	// blocking before returning (success or failure), so callers keep using
	// ordinary blocking send()/recv() exactly as they did before.
	bool ConnectWithTimeout(SOCKET socket, const sockaddr_in& address, int timeoutMs)
	{
		u_long nonBlocking = 1;
		ioctlsocket(socket, FIONBIO, &nonBlocking);

		bool connected = connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
		if (!connected && WSAGetLastError() == WSAEWOULDBLOCK)
		{
			fd_set writeSet;
			FD_ZERO(&writeSet);
			FD_SET(socket, &writeSet);
			fd_set errorSet = writeSet;
			timeval timeout{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
			if (select(0, nullptr, &writeSet, &errorSet, &timeout) > 0 && FD_ISSET(socket, &writeSet))
			{
				int socketError = 0;
				int socketErrorLen = sizeof(socketError);
				connected = getsockopt(socket, SOL_SOCKET, SO_ERROR,
					reinterpret_cast<char*>(&socketError), &socketErrorLen) == 0 && socketError == 0;
			}
		}

		u_long blocking = 0;
		ioctlsocket(socket, FIONBIO, &blocking);
		return connected;
	}

	// Shared connect-send-close for LOAD/REMOVE/MOVE. On failure, errorOut is
	// set to a message suitable for g_app.status and false is returned.
	bool SendToypadMessage(const uint8_t* data, size_t length, std::wstring& errorOut)
	{
		// One listener connection at a time: the LED poll uses the same port, so
		// hold the shared mutex for the whole connect/send/close transaction.
		std::lock_guard lock(g_socketMutex);
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
		if (!ConnectWithTimeout(clientSocket, address, kSocketConnectTimeoutMs))
		{
			closesocket(clientSocket);
			errorOut = L"Could not connect to the emulator. Enable the Toypad listener in Cemu, RPCS3, shadPS4, or Xenia first.";
			return false;
		}

		const bool sent = SendAll(clientSocket, data, length);
		closesocket(clientSocket);
		if (!sent)
		{
			errorOut = L"Connection to the emulator closed before the message was fully sent.";
			return false;
		}
		return true;
	}

	std::wstring EntryDisplayName(const RosterEntry& entry)
	{
		// Build number not shown: a multi-build vehicle's alternate forms are
		// picked from the build picker by their own names (e.g. "Snail Dude
		// Jake"), so the number only clutters the label.
		return entry.name;
	}

	// Sends one tag's 180 bytes to a pad slot, whatever they came from - an
	// embedded resource for the shipped library, a file on disk for a custom
	// tag. The wire message is identical either way.
	bool SendLoadBytesToSlot(const std::vector<uint8_t>& tagData, size_t slotIndex, std::wstring& errorOut)
	{
		if (tagData.size() != kTagSize)
		{
			errorOut = L"That tag is not " + std::to_wstring(kTagSize) + L" bytes long.";
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
		return SendLoadBytesToSlot(tagData, slotIndex, errorOut);
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
		// Always send a plain LOAD, even if the same tag is already tracked on
		// another pad. Some tags can occupy multiple pads at once, so loading
		// an already-placed tag duplicates it onto the chosen pad rather than
		// relocating it there. The listener clears the destination before a
		// LOAD anyway, so this still overwrites whatever is on that pad.

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
		PlayRemoveSound();
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
		// Covers all three outcomes below (plain move, swap, and the
		// same-pad refresh) - each is a MOVE wire message that just
		// succeeded, which is exactly what this sound confirms.
		PlayMoveSound();

		const PadSlot sourceSlot = g_app.padState[sourceIndex];
		const PadSlot destSlot = g_app.padState[destIndex];
		const std::wstring movedName = sourceSlot.figureName;
		if (destIndex != sourceIndex)
		{
			if (destSlot.occupied)
			{
				const bool reloaded = SendLoadResourceToSlot(destSlot.binResourceId, sourceIndex, error);
				if (!reloaded)
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

	// Franchise grid sorting --------------------------------------------------
	// The grid shows an *effective* display list (franchiseDisplayList) that
	// depends on the current FranchiseSort, plus a synthetic Favorites tile in
	// the modes where it makes sense. The user's persisted custom order is kept
	// separately in franchiseDisplayOrder and only used for the "User" sort.
	bool IsStarterPackFranchiseName(const std::wstring& name)
	{
		return name == L"DC Comics" || name == L"The Lord of the Rings" ||
			name == L"The LEGO Movie";
	}

	void RebuildFranchiseDisplay()
	{
		g_app.franchiseDisplayList.clear();
		switch (g_app.franchiseSort)
		{
		case AppState::FranchiseSort::User:
			// The user's own reordered list.
			g_app.franchiseDisplayList = g_app.franchiseDisplayOrder;
			g_app.showFavoritesTile = true;
			break;
		case AppState::FranchiseSort::Story:
			// Starter pack only (Batman, Gandalf, Wyldstyle); no Favorites tile.
			for (size_t i = 0; i < kFranchiseCount; ++i)
				if (IsStarterPackFranchiseName(kFranchises[i].name))
					g_app.franchiseDisplayList.push_back(i);
			g_app.showFavoritesTile = false;
			break;
		case AppState::FranchiseSort::Favorites:
			// Reserved for a later "favorite franchises only" mode; falls back
			// to the catalog order until that is added.
			for (size_t i = 0; i < kFranchiseCount; ++i)
				g_app.franchiseDisplayList.push_back(i);
			g_app.showFavoritesTile = true;
			break;
		case AppState::FranchiseSort::Default:
		default:
			// All series, alphabetical (the catalog's own order).
			for (size_t i = 0; i < kFranchiseCount; ++i)
				g_app.franchiseDisplayList.push_back(i);
			g_app.showFavoritesTile = true;
			break;
		}
	}

	std::wstring DescribeFranchiseSort()
	{
		switch (g_app.franchiseSort)
		{
		case AppState::FranchiseSort::User: return L"User";
		case AppState::FranchiseSort::Story: return L"Story";
		case AppState::FranchiseSort::Favorites: return L"Favorites";
		case AppState::FranchiseSort::Default:
		default: return L"Default";
		}
	}

	// True while the screen is one of the "browse" views that the shoulder
	// buttons sort: the franchise tile grid, or the Story/Favorites roster.
	bool IsFranchiseSortBrowseActive()
	{
		if (g_app.screen == Screen::FranchiseList)
			return true;
		if (g_app.screen == Screen::RosterList)
			return g_app.storyRosterActive ||
				(g_app.favoritesTileSelected && g_app.franchiseSort == AppState::FranchiseSort::Favorites);
		return false;
	}

	// Cycle the franchise-grid sort. Rebuilds the browse view (grid or the
	// story/favorites roster) and keeps the focused selection valid.
	void CycleFranchiseSort(int direction)
	{
		const auto modes = {
			AppState::FranchiseSort::Default,
			AppState::FranchiseSort::User,
			AppState::FranchiseSort::Story,
			AppState::FranchiseSort::Favorites,
		};
		const int count = static_cast<int>(modes.size());
		int idx = 0;
		for (int i = 0; i < count; ++i)
			if (*(modes.begin() + i) == g_app.franchiseSort)
				idx = i;
		idx = (idx + direction + count) % count;
		g_app.franchiseSort = *(modes.begin() + idx);
		SaveInputSettingsToIni();
		OpenBrowseScreen();
		g_app.status = L"Sort: " + DescribeFranchiseSort();
	}

	// Display slot (0-based, real franchises only) that franchiseIndex
	// currently occupies in the effective display list. Linear search over
	// ~30 entries, called only on input.
	size_t FindFranchiseDisplaySlot(size_t franchiseIndex)
	{
		for (size_t i = 0; i < g_app.franchiseDisplayList.size(); ++i)
		{
			if (g_app.franchiseDisplayList[i] == franchiseIndex)
				return i;
		}
		return 0;
	}

	void MoveFranchiseSelection(int dx, int dy)
	{
		// The grid shows the effective display list (franchiseDisplayList) plus
		// an optional Favorites tile at logical index 0. Keeping the existing
		// ragged-last-row wrap math untouched, just over the extra item when the
		// tile is present.
		const size_t realCount = g_app.franchiseDisplayList.size();
		if (realCount == 0)
			return;
		const size_t logicalCount = realCount + (g_app.showFavoritesTile ? 1 : 0);
		const size_t rows = (logicalCount + kFranchiseCols - 1) / kFranchiseCols;
		size_t logicalIndex = g_app.showFavoritesTile
			? (g_app.favoritesTileSelected ? 0 : 1 + FindFranchiseDisplaySlot(g_app.franchiseIndex))
			: FindFranchiseDisplaySlot(g_app.franchiseIndex);
		size_t row = logicalIndex / kFranchiseCols;
		size_t col = logicalIndex % kFranchiseCols;

		row = (row + static_cast<size_t>(dy) + rows) % rows;
		const size_t lastCol = std::min(kFranchiseCols, logicalCount - row * kFranchiseCols) - 1;
		col = (col + static_cast<size_t>(dx) + lastCol + 1) % (lastCol + 1);

		logicalIndex = row * kFranchiseCols + col;
		if (g_app.showFavoritesTile)
		{
			g_app.favoritesTileSelected = (logicalIndex == 0);
			if (!g_app.favoritesTileSelected)
			{
				const size_t slot = logicalIndex - 1;
				if (slot < g_app.franchiseDisplayList.size())
					g_app.franchiseIndex = g_app.franchiseDisplayList[slot];
			}
		}
		else
		{
			g_app.favoritesTileSelected = false;
			if (logicalIndex < g_app.franchiseDisplayList.size())
				g_app.franchiseIndex = g_app.franchiseDisplayList[logicalIndex];
		}

		// Keep the focused row inside the visible viewport.
		const int focusedRow = static_cast<int>(row);
		while (focusedRow < g_app.franchiseTopRow)
			--g_app.franchiseTopRow;
		while (focusedRow >= g_app.franchiseTopRow + static_cast<int>(kFranchiseVisibleRows))
			++g_app.franchiseTopRow;
		if (g_app.franchiseTopRow < 0)
			g_app.franchiseTopRow = 0;
	}

	// Picks up the focused franchise tile so the next navigation + Confirm
	// drops it in a new spot. The synthetic Favorites tile can't be moved.
	void BeginFranchiseReorganize()
	{
		if (g_app.favoritesTileSelected)
			return;
		// Reordering only makes sense in the custom-order view; in the other
		// sorts the grid order is fixed, so guide the user instead.
		if (g_app.franchiseSort != AppState::FranchiseSort::User)
		{
			g_app.status = L"Switch to \"Custom order\" (RB/LB) to reorganize worlds.";
			return;
		}
		g_app.reorganizingFranchise = true;
		g_app.reorganizeFranchiseSourceIndex = FindFranchiseDisplaySlot(g_app.franchiseIndex);
		g_app.status = L"Reorganizing: " + kFranchises[g_app.franchiseIndex].name + L" - pick a new spot";
	}

	void DropFranchiseReorder()
	{
		const size_t from = g_app.reorganizeFranchiseSourceIndex;
		const size_t to = g_app.favoritesTileSelected
			? from : FindFranchiseDisplaySlot(g_app.franchiseIndex);
		g_app.reorganizingFranchise = false;
		if (from >= g_app.franchiseDisplayOrder.size() || to >= g_app.franchiseDisplayOrder.size() ||
			from == to)
			return;

		const size_t movedFranchise = g_app.franchiseDisplayOrder[from];
		const std::wstring name = kFranchises[movedFranchise].name;
		g_app.franchiseDisplayOrder.erase(g_app.franchiseDisplayOrder.begin() + from);
		g_app.franchiseDisplayOrder.insert(g_app.franchiseDisplayOrder.begin() + to, movedFranchise);
		SaveFranchiseOrderToIni();
		// The active display list is a copy of the user order; refresh it so the
		// grid reflects the drop immediately.
		RebuildFranchiseDisplay();
		g_app.status = L"Moved: " + name;
	}

	void CancelFranchiseReorder()
	{
		g_app.reorganizingFranchise = false;
		if (g_app.reorganizeFranchiseSourceIndex < g_app.franchiseDisplayOrder.size())
			g_app.franchiseIndex = g_app.franchiseDisplayOrder[g_app.reorganizeFranchiseSourceIndex];
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
	// Vertical panel/scrollbar margin above the first row and below the
	// last one. Must clear kFocusGlowMargin (20px, defined further down)
	// so a focused top- or bottom-row tile's halo stays inside the panel.
	constexpr int kRosterPanelPadY = 26;

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
	// A world with two full character rows plus a vehicle row (10+ characters
	// and at least one vehicle) still overflowed the window even at compact
	// metrics - e.g. DC's roster cut its vehicle row's labels off at the
	// bottom edge. This tier trims further so that worst case always fits.
	constexpr RosterMetrics kRosterExtraCompactMetrics{134, 72, 46, 8, 16};

	// Size of the grid's first section - the one above the separator. It holds
	// the world's characters, then that world's captured custom tags, then the
	// tile that captures a new one; the vehicles start after it.
	size_t GetRosterCharacterCount()
	{
		size_t count = 0;
		while (count < g_app.rosterSlots.size())
		{
			const RosterSlot::Kind kind = g_app.rosterSlots[count].kind;
			if (kind == RosterSlot::Kind::Vehicle || kind == RosterSlot::Kind::Plus)
				break;
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

	// Horizontal centering offset for one visual row: a row with fewer than
	// kRosterCols items is centered under the (always full-width) panel
	// rather than left-anchored - each row independently, since the
	// character and vehicle sections can each have a different item count
	// (e.g. 1 character above 2 vehicles), and left-anchoring both under a
	// single shared offset left the narrower one looking off-center under
	// the wider one instead of both centered on the panel.
	int GetRosterRowColumnOffset(size_t row)
	{
		const size_t items = GetRosterVisualRowItemCount(row);
		return static_cast<int>(kRosterCols - items) * kRosterPitchX / 2;
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

	// Whether the current page needs the tighter metrics: the separator gap
	// between the character and vehicle sections adds extra vertical space
	// wherever it lands, and that gap can push the last visible row past the
	// window's bottom edge on ANY page it appears on - not only when the
	// whole roster fits without scrolling (the only case this used to check).
	// A franchise with enough vehicles to need scrolling could land the
	// character/vehicle boundary in the middle of a scrolled page and hit the
	// exact same overflow, with nothing to catch it: the last row's portraits
	// and labels simply ran past the window edge and were clipped.
	// IsRosterSeparatorVisible() already implies a mixed roster (it checks
	// characterCount is neither zero nor the whole list), so no separate
	// "hasSeparator" check is needed on top of it.
	RosterMetrics GetRosterMetrics(Gdiplus::Graphics& g)
	{
		if (IsRosterSeparatorVisible() &&
			GetRosterContentBottom(g, kRosterNormalMetrics) > kRosterGridBottomLimit)
		{
			if (GetRosterContentBottom(g, kRosterCompactMetrics) > kRosterGridBottomLimit)
				return kRosterExtraCompactMetrics;
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

	// Extra top margin added on top of kRosterOriginY when the current
	// roster's content doesn't need the full budget that constant was tuned
	// for (a maxed-out 3-row page). Without this, a world with only a
	// couple of figures still opens with its grid pinned right under the
	// header - unused space just piles up at the bottom instead. Capped,
	// and never negative, so a page that already fills the budget is
	// completely unaffected.
	constexpr int kRosterMaxOriginShift = 60;

	int GetRosterOriginShift(Gdiplus::Graphics& g, const RosterMetrics& metrics)
	{
		const int contentHeight = GetRosterContentBottom(g, metrics) - kRosterOriginY;
		const int available = kRosterGridBottomLimit - kRosterOriginY;
		const int slack = available - contentHeight;
		return std::min(kRosterMaxOriginShift, std::max(0, slack) / 2);
	}

	int GetRosterOriginShift(Gdiplus::Graphics& g)
	{
		return GetRosterOriginShift(g, GetRosterMetrics(g));
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

	// Re-selects a specific roster slot and scrolls its row into view, without
	// otherwise touching the roster grid. Used to restore the previously
	// focused vehicle after backing out of the build picker.
	void SelectRosterIndexAndScroll(size_t index)
	{
		if (g_app.rosterSlots.empty())
			return;
		index = std::min(index, g_app.rosterSlots.size() - 1);
		g_app.rosterIndex = index;
		const RosterVisualPosition pos = GetRosterVisualPosition(index);
		const int focusedRow = static_cast<int>(pos.row);
		while (focusedRow < g_app.rosterTopRow)
			--g_app.rosterTopRow;
		while (focusedRow >= g_app.rosterTopRow + static_cast<int>(kRosterVisibleRows))
			++g_app.rosterTopRow;
		if (g_app.rosterTopRow < 0)
			g_app.rosterTopRow = 0;
	}

	// A cheap fingerprint of "what is highlighted right now". Navigation
	// sounds are gated on this changing, so pushing a direction into the edge
	// of a grid - which deliberately does nothing - stays silent instead of
	// blipping at every frame of a held stick.
	uint64_t SelectionSignature()
	{
		uint64_t signature = static_cast<uint64_t>(g_app.screen);
		signature = signature * 131 + g_app.slotIndex;
		signature = signature * 131 + g_app.padActionIndex;
		signature = signature * 131 + g_app.franchiseIndex;
		signature = signature * 131 + (g_app.favoritesTileSelected ? 1 : 0);
		signature = signature * 131 + g_app.rosterIndex;
		signature = signature * 131 + g_app.plusBuildIndex;
		signature = signature * 131 + g_app.settingsIndex;
		return signature;
	}

	// Starts the landing animation when the highlight has moved since the last
	// tick. Watching the signature rather than hooking every navigation site
	// means mouse clicks, screen changes and the web remote all get the same
	// treatment for free, and none of them can forget to.
	void SyncSelectionTap()
	{
		const uint64_t signature = SelectionSignature();
		if (!g_hasSelectionSignature)
		{
			g_hasSelectionSignature = true;
			g_lastSelectionSignature = signature;
			return;
		}
		if (signature == g_lastSelectionSignature)
			return;
		g_lastSelectionSignature = signature;
		g_selectionTapStart = GetTickCount();
		if (g_selectionTapStart == 0)
			g_selectionTapStart = 1;
	}

	void NavigateGridInternal(int dx, int dy)
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
		case Screen::Settings:
			// Horizontal is handled by the NavigateGrid wrapper (it edits the
			// value); only the vertical axis reaches here.
			if (dy != 0)
				MoveSettingsSelection(dy);
			break;
		default:
			break;
		}
	}

	void NavigateInternal(int direction)
	{
		switch (g_app.screen)
		{
		case Screen::PadViewer:
		case Screen::FranchiseList:
		case Screen::RosterList:
			NavigateGridInternal(0, direction);
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
			MoveSettingsSelection(direction);
			break;
		}
	}

	// Public entry points: navigate, then blip if the highlight actually
	// moved.
	void NavigateGrid(int dx, int dy)
	{
		// On the Settings screen the horizontal axis edits the focused row's
		// value rather than moving the highlight, so a switch can be flipped
		// with Left/Right as well as with Confirm.
		if (g_app.screen == Screen::Settings && dx != 0)
		{
			AdjustSettingsValue(dx);
			return;
		}
		const uint64_t before = SelectionSignature();
		NavigateGridInternal(dx, dy);
		if (SelectionSignature() != before)
			PlayNavigateSound();
	}

	void Navigate(int direction)
	{
		const uint64_t before = SelectionSignature();
		NavigateInternal(direction);
		if (SelectionSignature() != before)
			PlayNavigateSound();
	}

	// ---------------------------------------------------------------------
	// Screen transitions
	// ---------------------------------------------------------------------

	void OpenFranchiseList()
	{
		RebuildFranchiseDisplay();
		g_app.franchiseTopRow = 0;
		if (!g_app.franchiseDisplayList.empty())
			g_app.franchiseIndex = g_app.franchiseDisplayList[0];
		// The Favorites tile is logical index 0 in the grid when shown (see
		// MoveFranchiseSelection/DrawFranchiseGrid) - it's the first tile
		// shown, so it should also be the one initially focused.
		g_app.favoritesTileSelected = g_app.showFavoritesTile;
		g_app.storyRosterActive = false;
		g_app.screen = Screen::FranchiseList;
	}

	// Opens whatever the current franchise sort should show. Default/User show
	// the franchise tile grid; Story and Favorites show their roster of
	// characters/vehicles directly instead of tiles.
	void OpenBrowseScreen()
	{
		switch (g_app.franchiseSort)
		{
		case AppState::FranchiseSort::Story:
			OpenStoryRoster();
			return;
		case AppState::FranchiseSort::Favorites:
			g_app.storyRosterActive = false;
			g_app.favoritesTileSelected = true;
			OpenFavoritesRoster();
			g_app.rosterIndex = 0;
			g_app.rosterTopRow = 0;
			g_app.plusGroup = nullptr;
			g_app.screen = Screen::RosterList;
			return;
		case AppState::FranchiseSort::Default:
		case AppState::FranchiseSort::User:
		default:
			OpenFranchiseList();
			return;
		}
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

	// Builds the roster grid from the current favorites list instead of a
	// single franchise. Stale entries (e.g. left over from an ini edited by
	// hand) are silently skipped rather than shown as broken tiles.
	void OpenFavoritesRoster()
	{
		g_app.rosterSlots.clear();
		// Two passes, not one filtering pass: every other roster-building
		// path (OpenRosterList, GetRosterCharacterCount, the grid layout/
		// navigation) assumes ALL Character slots come before ALL Vehicle
		// slots with no interleaving. Favorites are added in whatever order
		// the user favorited them, which can freely mix the two - a single
		// pass over g_app.favorites would carry that interleaving straight
		// into rosterSlots and split the character section around a vehicle,
		// which is exactly the "character got mixed into the vehicle row"
		// bug this avoids.
		for (const auto& fav : g_app.favorites)
		{
			if (fav.isVehicle)
				continue;
			const RosterEntry* character = FindCharacterEntry(fav.franchise.c_str(), fav.name.c_str());
			if (!character)
				continue;
			g_app.rosterSlots.push_back({RosterSlot::Kind::Character, character, nullptr});
		}
		for (const auto& fav : g_app.favorites)
		{
			if (!fav.isVehicle)
				continue;
			const VehicleGroup* group = FindVehicleGroupEntry(fav.franchise.c_str(), fav.name.c_str());
			if (!group || group->builds.empty())
				continue;
			// buildNumber 0 is a legacy entry saved before per-variant
			// favoriting existed - resolves to build 1, same as it always
			// displayed before. Otherwise show the exact build favorited, so
			// two builds of the same family can appear as separate tiles.
			const RosterEntry* build = &group->builds.front();
			if (fav.buildNumber != 0)
			{
				for (const auto& candidate : group->builds)
				{
					if (candidate.buildNumber == fav.buildNumber)
					{
						build = &candidate;
						break;
					}
				}
			}
			g_app.rosterSlots.push_back({RosterSlot::Kind::Vehicle, build, group});
		}
	}

	void OpenRosterList()
	{
		if (g_app.favoritesTileSelected)
			OpenFavoritesRoster();
		else
		{
			g_app.rosterSlots.clear();
			const Franchise& franchise = kFranchises[g_app.franchiseIndex];
			for (const auto& character : franchise.characters)
				g_app.rosterSlots.push_back({RosterSlot::Kind::Character, &character, nullptr});
			for (const auto& vehicle : franchise.vehicles)
			{
				if (vehicle.builds.empty())
					continue;
				// Only the default (build 1) tile is shown; its alternates are
				// revealed through the build picker when the tile is confirmed.
				const RosterEntry* first = &vehicle.builds.front();
				g_app.rosterSlots.push_back({RosterSlot::Kind::Vehicle, first, &vehicle});
			}
		}
		g_app.rosterIndex = 0;
		g_app.rosterTopRow = 0;
		g_app.plusGroup = nullptr;
		g_app.storyRosterActive = false;
		g_app.screen = Screen::RosterList;
	}

	// Finds which franchise actually owns a roster entry/vehicle group, by
	// object identity rather than by whichever "world" is currently being
	// browsed - the Favorites roster aggregates entries from many franchises,
	// so the browsing context alone can't tell you where one came from.
	bool FindFranchiseForCharacter(const RosterEntry* entry, std::wstring& franchiseOut)
	{
		for (size_t i = 0; i < kFranchiseCount; ++i)
		{
			for (const auto& character : kFranchises[i].characters)
			{
				if (&character == entry)
				{
					franchiseOut = kFranchises[i].name;
					return true;
				}
			}
		}
		return false;
	}

	bool FindFranchiseForVehicleGroup(const VehicleGroup* group, std::wstring& franchiseOut)
	{
		for (size_t i = 0; i < kFranchiseCount; ++i)
		{
			for (const auto& vehicle : kFranchises[i].vehicles)
			{
				if (&vehicle == group)
				{
					franchiseOut = kFranchises[i].name;
					return true;
				}
			}
		}
		return false;
	}

	// Whether (franchise, name, isVehicle, buildNumber) is currently
	// favorited. Used both by ToggleFavorite's own lookup and by the roster/
	// build-picker tiles to draw their favorited-star badge.
	bool IsFavorited(const std::wstring& franchise, const std::wstring& name, bool isVehicle,
		int buildNumber)
	{
		return std::find_if(g_app.favorites.begin(), g_app.favorites.end(), [&](const FavoriteEntry& f) {
			return f.isVehicle == isVehicle && f.franchise == franchise && f.name == name &&
				f.buildNumber == buildNumber;
		}) != g_app.favorites.end();
	}

	// Adds or removes one (franchise, name, buildNumber) favorite, validating
	// it against the real library first so a bad request (stale ini entry, or
	// a bogus web request) can never inject a favorite that doesn't resolve
	// to anything. Returns false (favorites untouched) when validation fails;
	// otherwise toggles the entry, persists to the ini, and reports the new
	// state via favoritedOut. Shared by the roster-screen button binding and
	// the web remote's /api/favorite endpoint, so both stay in sync.
	// buildNumber is ignored for characters (always stored as 0).
	bool ToggleFavorite(const std::wstring& franchise, const std::wstring& name, bool isVehicle,
		int buildNumber, bool& favoritedOut)
	{
		if (isVehicle)
		{
			if (!FindVehicleGroupEntry(franchise.c_str(), name.c_str()))
				return false;
		}
		else
		{
			buildNumber = 0;
			if (!FindCharacterEntry(franchise.c_str(), name.c_str()))
				return false;
		}

		auto& favorites = g_app.favorites;
		const auto it = std::find_if(favorites.begin(), favorites.end(), [&](const FavoriteEntry& f) {
			return f.isVehicle == isVehicle && f.franchise == franchise && f.name == name &&
				f.buildNumber == buildNumber;
		});
		if (it != favorites.end())
		{
			favorites.erase(it);
			favoritedOut = false;
		}
		else
		{
			favorites.push_back({franchise, name, isVehicle, buildNumber});
			favoritedOut = true;
		}
		SaveFavoritesToIni();
		return true;
	}

	// Toggles favorite status for whatever roster slot is currently focused.
	// Characters favorite by name. A single-build vehicle favorites that one
	// build. A multi-build vehicle's roster tile ("+") is a no-op here - it
	// represents the whole family, not one variant, so favoriting a specific
	// build only happens from inside the build picker (see
	// ToggleFavoriteForFocusedPlusBuild).
	void ToggleFavoriteForFocusedRoster()
	{
		if (g_app.rosterIndex >= g_app.rosterSlots.size())
			return;
		const RosterSlot& slot = g_app.rosterSlots[g_app.rosterIndex];

		std::wstring franchise;
		std::wstring name;
		bool isVehicle = false;
		int buildNumber = 0;
		if (slot.kind == RosterSlot::Kind::Character && slot.entry)
		{
			if (!FindFranchiseForCharacter(slot.entry, franchise))
				return;
			name = slot.entry->name;
		}
		else if (slot.kind == RosterSlot::Kind::Vehicle && slot.group)
		{
			// While browsing the Favorites roster this tile already stands
			// for one specific favorited build (see OpenFavoritesRoster), so
			// it toggles that exact build even if the family has others.
			// Anywhere else, a tile with more than one build is the "+"
			// family tile and isn't favoritable directly.
			if (slot.group->builds.size() > 1 && !g_app.favoritesTileSelected)
			{
				g_app.status = L"Open it to favorite a specific build.";
				return;
			}
			if (!FindFranchiseForVehicleGroup(slot.group, franchise))
				return;
			name = slot.group->baseName;
			isVehicle = true;
			buildNumber = slot.entry ? slot.entry->buildNumber : 0;
		}
		else
		{
			return;
		}

		bool favorited = false;
		if (!ToggleFavorite(franchise, name, isVehicle, buildNumber, favorited))
			return;
		g_app.status = (favorited ? L"Added to favorites: " : L"Removed from favorites: ") + name;
	}

	// Toggles favorite status for the build currently highlighted in the
	// build picker (Screen::PlusPicker) - the only way to favorite one
	// specific variant of a multi-build vehicle.
	void ToggleFavoriteForFocusedPlusBuild()
	{
		if (!g_app.plusGroup || g_app.plusBuildIndex >= g_app.plusGroup->builds.size())
			return;
		std::wstring franchise;
		if (!FindFranchiseForVehicleGroup(g_app.plusGroup, franchise))
			return;
		const RosterEntry& build = g_app.plusGroup->builds[g_app.plusBuildIndex];

		bool favorited = false;
		if (!ToggleFavorite(franchise, g_app.plusGroup->baseName, true, build.buildNumber, favorited))
			return;
		g_app.status = (favorited ? L"Added to favorites: " : L"Removed from favorites: ") +
			EntryDisplayName(build);
	}

	// Identifies which FavoriteEntry a Favorites-roster slot came from, so
	// reordering can find and move the underlying entry regardless of how
	// characters/vehicles are interleaved in storage (see ReorderFavorite).
	bool FavoriteIdentityForRosterSlot(size_t index, std::wstring& franchiseOut, std::wstring& nameOut,
		bool& isVehicleOut, int& buildNumberOut)
	{
		if (index >= g_app.rosterSlots.size())
			return false;
		const RosterSlot& slot = g_app.rosterSlots[index];
		if (slot.kind == RosterSlot::Kind::Character && slot.entry)
		{
			if (!FindFranchiseForCharacter(slot.entry, franchiseOut))
				return false;
			nameOut = slot.entry->name;
			isVehicleOut = false;
			buildNumberOut = 0;
			return true;
		}
		if (slot.kind == RosterSlot::Kind::Vehicle && slot.group && slot.entry)
		{
			if (!FindFranchiseForVehicleGroup(slot.group, franchiseOut))
				return false;
			nameOut = slot.group->baseName;
			isVehicleOut = true;
			buildNumberOut = slot.entry->buildNumber;
			return true;
		}
		return false;
	}

	// Moves the favorite shown at fromDisplayIndex to land where
	// toDisplayIndex currently is, shifting the rest - by editing
	// g_app.favorites directly via the entries' own identity (not by
	// recomputed index), so it works no matter how characters/vehicles are
	// interleaved in storage. The Favorites roster is rebuilt afterward.
	void ReorderFavorite(size_t fromDisplayIndex, size_t toDisplayIndex)
	{
		if (fromDisplayIndex == toDisplayIndex)
			return;

		std::wstring fromFranchise, fromName, toFranchise, toName;
		bool fromIsVehicle = false, toIsVehicle = false;
		int fromBuild = 0, toBuild = 0;
		if (!FavoriteIdentityForRosterSlot(fromDisplayIndex, fromFranchise, fromName, fromIsVehicle, fromBuild) ||
			!FavoriteIdentityForRosterSlot(toDisplayIndex, toFranchise, toName, toIsVehicle, toBuild))
			return;

		auto matches = [](const FavoriteEntry& f, const std::wstring& franchise, const std::wstring& name,
			bool isVehicle, int buildNumber) {
			return f.isVehicle == isVehicle && f.franchise == franchise && f.name == name &&
				f.buildNumber == buildNumber;
		};

		const auto fromIt = std::find_if(g_app.favorites.begin(), g_app.favorites.end(),
			[&](const FavoriteEntry& f) { return matches(f, fromFranchise, fromName, fromIsVehicle, fromBuild); });
		if (fromIt == g_app.favorites.end())
			return;
		const FavoriteEntry moved = *fromIt;
		g_app.favorites.erase(fromIt);

		const auto toIt = std::find_if(g_app.favorites.begin(), g_app.favorites.end(),
			[&](const FavoriteEntry& f) { return matches(f, toFranchise, toName, toIsVehicle, toBuild); });
		// Moving right/down lands the item just after its target, moving
		// left/up lands it just before - either way it ends up occupying
		// the target's display slot and pushes the rest along by one.
		if (toIt == g_app.favorites.end())
			g_app.favorites.push_back(moved);
		else if (fromDisplayIndex < toDisplayIndex)
			g_app.favorites.insert(toIt + 1, moved);
		else
			g_app.favorites.insert(toIt, moved);

		OpenFavoritesRoster();
		SaveFavoritesToIni();
	}

	// Picks up whatever roster tile is focused so the next navigation +
	// Confirm drops it in a new spot. Only meaningful in the Favorites
	// roster - a real franchise's roster order comes from the game data.
	void BeginRosterReorganize()
	{
		if (!g_app.favoritesTileSelected || g_app.rosterIndex >= g_app.rosterSlots.size())
			return;
		g_app.reorganizingRoster = true;
		g_app.reorganizeRosterSourceIndex = g_app.rosterIndex;
		const RosterSlot& slot = g_app.rosterSlots[g_app.rosterIndex];
		const std::wstring name = slot.entry ? slot.entry->name : L"item";
		g_app.status = L"Reorganizing: " + name + L" - pick a new spot";
	}

	void DropRosterReorder()
	{
		const size_t from = g_app.reorganizeRosterSourceIndex;
		const size_t to = g_app.rosterIndex;
		g_app.reorganizingRoster = false;
		if (from >= g_app.rosterSlots.size() || to >= g_app.rosterSlots.size())
			return;
		const RosterSlot& movedSlot = g_app.rosterSlots[from];
		const std::wstring name = movedSlot.entry ? movedSlot.entry->name : L"item";
		ReorderFavorite(from, to);
		// The moved item now sits at the drop target's old display slot.
		g_app.rosterIndex = std::min(to, g_app.rosterSlots.empty() ? 0 : g_app.rosterSlots.size() - 1);
		g_app.status = L"Moved: " + name;
	}

	void CancelRosterReorder()
	{
		g_app.reorganizingRoster = false;
		g_app.rosterIndex = g_app.reorganizeRosterSourceIndex;
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
		g_app.rosterIndexBeforePlus = g_app.rosterIndex;
		g_app.plusGroup = &group;
		g_app.plusBuildIndex = 0;
		g_app.screen = Screen::PlusPicker;
	}

	void Confirm()
	{
		// One blip for every confirm, wherever it lands: picking a pad, a
		// figure, a settings row or a letter on the naming keyboard.
		PlaySelectSound();
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
				OpenBrowseScreen();
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
			if (g_app.reorganizingFranchise)
				DropFranchiseReorder();
			else
				OpenRosterList();
			break;
		case Screen::RosterList:
			if (g_app.reorganizingRoster)
			{
				DropRosterReorder();
				break;
			}
			if (g_app.rosterIndex >= g_app.rosterSlots.size())
				break;
			switch (g_app.rosterSlots[g_app.rosterIndex].kind)
			{
			case RosterSlot::Kind::Character:
				if (g_app.rosterSlots[g_app.rosterIndex].entry)
					LoadRosterEntryToPad(*g_app.rosterSlots[g_app.rosterIndex].entry);
				break;
			case RosterSlot::Kind::Vehicle:
				// A multi-build vehicle shows only its default tile here; the
				// alternates are revealed in the build picker, which loads the
				// chosen build. A single-build vehicle loads straight away.
				// While browsing the Favorites roster the tile already stands
				// for one specific favorited build (see OpenFavoritesRoster),
				// so it always loads directly instead of reopening the picker.
				if (!g_app.favoritesTileSelected && g_app.rosterSlots[g_app.rosterIndex].group &&
					g_app.rosterSlots[g_app.rosterIndex].group->builds.size() > 1)
					OpenPlusPicker(*g_app.rosterSlots[g_app.rosterIndex].group);
				else if (g_app.rosterSlots[g_app.rosterIndex].entry)
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
			ActivateSettingsEntry();
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
			if (g_app.reorganizingFranchise)
				CancelFranchiseReorder();
			else
				g_app.screen = Screen::PadViewer;
			break;
		case Screen::RosterList:
			if (g_app.reorganizingRoster)
			{
				CancelRosterReorder();
				break;
			}
			if (g_app.storyRosterActive ||
				(g_app.favoritesTileSelected && g_app.franchiseSort == AppState::FranchiseSort::Favorites))
			{
				// A browse roster (Story / Favorites sort) was opened straight
				// from the pad viewer, so Back skips the franchise grid.
				g_app.storyRosterActive = false;
				g_app.favoritesTileSelected = false;
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
			// OpenRosterList/OpenStoryRoster reset the selection to the top
			// of the roster; put it back on the vehicle that was opened.
			SelectRosterIndexAndScroll(g_app.rosterIndexBeforePlus);
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
		int x = info.rcMonitor.left + (monitorWidth - kOverlayWidth) / 2;
		int y = info.rcMonitor.top + (monitorHeight - kOverlayHeight) / 2;

		// A draggable overlay comes back exactly where it was dropped, but
		// only if enough of it would still be reachable there. A monitor
		// that has been unplugged since, a resolution change, or a drag that
		// pushed it nearly off the edge would otherwise leave it somewhere
		// the mouse can't grab it again - and the Settings row that turns
		// dragging back off lives inside that same window. So the remembered
		// spot is nudged back onto its monitor when too little of it shows,
		// and dropped entirely (falling through to centred) when it lands on
		// no monitor at all.
		if (g_app.windowDraggable && g_app.hasSavedWindowPos)
		{
			const RECT remembered{g_app.savedWindowX, g_app.savedWindowY,
				g_app.savedWindowX + kOverlayWidth, g_app.savedWindowY + kOverlayHeight};
			if (HMONITOR savedMonitor = MonitorFromRect(&remembered, MONITOR_DEFAULTTONULL))
			{
				MONITORINFO savedInfo{};
				savedInfo.cbSize = sizeof(savedInfo);
				if (GetMonitorInfoW(savedMonitor, &savedInfo))
				{
					// Grab-handle margin: this much of the window has to stay
					// on that monitor in both axes.
					constexpr int kMinVisible = 160;
					x = std::clamp(g_app.savedWindowX,
						static_cast<int>(savedInfo.rcMonitor.left) - (kOverlayWidth - kMinVisible),
						static_cast<int>(savedInfo.rcMonitor.right) - kMinVisible);
					y = std::clamp(g_app.savedWindowY,
						static_cast<int>(savedInfo.rcMonitor.top) - (kOverlayHeight - kMinVisible),
						static_cast<int>(savedInfo.rcMonitor.bottom) - kMinVisible);
				}
				else
				{
					x = g_app.savedWindowX;
					y = g_app.savedWindowY;
				}
			}
		}
		SetWindowPos(window, HWND_TOPMOST, x, y, kOverlayWidth, kOverlayHeight, SWP_NOACTIVATE);
	}

	void ShowOverlay(HWND window)
	{
		if (g_app.overlayVisible)
		{
			// Caught mid-dismiss: turn the fade around instead of ignoring
			// the press, which would otherwise look like a dropped input.
			if (g_overlayHiding)
			{
				g_overlayHiding = false;
				BeginWindowFade(false, CurrentShownFraction());
				UpdateInputOwnership(window);
				ForceForegroundWindow(window);
				InvalidateRect(window, nullptr, FALSE);
			}
			return;
		}

		const HWND currentForeground = GetForegroundWindow();
		if (currentForeground && currentForeground != window)
			g_app.previousForegroundWindow = currentForeground;

		// Two views of the same seven pads on screen at once reads as a bug,
		// and the picker's own pads are the richer one, so the HUD goes now
		// rather than fading out behind the overlay.
		HidePeekWindow(true);

		PositionOverlayWindow(window);
		// Assert input ownership before the window becomes visible so there
		// is no tick where Cemu can read the controller while the picker is
		// showing (the 16ms poll alone was too slow for that handoff).
		g_app.overlayVisible = true;
		g_overlayHiding = false;
		g_tickIntervalMs.store(kTickIntervalVisibleMs, std::memory_order_relaxed);
		// The very first frame has to be the faded-in one, so the fade is
		// armed before the window is shown - otherwise ShowWindow presents
		// the previous frame at full alpha for a beat.
		BeginWindowFade(false, 0.0f);
		// A fresh show is always the front screen's entrance, so the content
		// fade runs too rather than only the window's.
		g_screenFadeStart = GetTickCount();
		if (g_screenFadeStart == 0)
			g_screenFadeStart = 1;
		UpdateInputOwnership(window);
		Paint(window);
		ShowWindow(window, SW_SHOW);
		ForceForegroundWindow(window);
		InvalidateRect(window, nullptr, FALSE);
	}

	// Asks the overlay to go away. It does not vanish here: the window fades
	// out over kWindowFadeOutMs and FinishOverlayHide (driven by the timer)
	// does the actual hiding once the fade lands. Input ownership is held for
	// the whole fade, so the button press that dismissed the picker still
	// never reaches the game.
	void HideOverlay(HWND window)
	{
		if (!g_app.overlayVisible || g_overlayHiding)
			return;

		g_overlayHiding = true;
		g_app.capturingShortcut = false;
		g_app.capturingBindingIndex = -1;
		BeginWindowFade(true, CurrentShownFraction());
		InvalidateRect(window, nullptr, FALSE);
	}

	void FinishOverlayHide(HWND window)
	{
		if (!g_overlayHiding)
			return;
		g_overlayHiding = false;
		ShowWindow(window, SW_HIDE);
		g_app.overlayVisible = false;
		g_tickIntervalMs.store(kTickIntervalHiddenMs, std::memory_order_relaxed);
		g_windowFadeStart = 0;
		// Release ownership only once the overlay is really hidden, so the
		// very button press that closed it never reaches the game either.
		UpdateInputOwnership(window);

		if (g_app.previousForegroundWindow && IsWindow(g_app.previousForegroundWindow))
			ForceForegroundWindow(g_app.previousForegroundWindow);
	}

	void ToggleOverlay(HWND window)
	{
		if (g_app.overlayVisible && !g_overlayHiding)
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
		g_app.franchiseSort = defaults.franchiseSort;
		RebuildFranchiseDisplay();
		g_app.buttonStyleChoice = defaults.buttonStyleChoice;
		g_app.soundEffects = defaults.soundEffects;
		g_app.padSkinIndex = defaults.padSkinIndex;
		g_app.peekSizeChoice = defaults.peekSizeChoice;
		HidePeekWindow(true); // the HUD's size setting just changed under it
		g_app.opacityPercent = defaults.opacityPercent;
		if (g_app.ledMirrorEnabled != defaults.ledMirrorEnabled)
			SetLedMirrorEnabled(defaults.ledMirrorEnabled); // starts/stops the poll
		for (const auto& action : kBindableActions)
			g_app.*(action.button) = defaults.*(action.button);

		// Back to a fixed, centred window, and the remembered spot goes with
		// it - a later switch back to draggable should start from centre
		// rather than from wherever it happened to sit before the reset.
		if (g_app.draggingWindow)
		{
			g_app.draggingWindow = false;
			if (GetCapture() == g_mainWindow)
				ReleaseCapture();
		}
		g_app.windowDraggable = defaults.windowDraggable;
		g_app.hasSavedWindowPos = defaults.hasSavedWindowPos;
		g_app.savedWindowX = defaults.savedWindowX;
		g_app.savedWindowY = defaults.savedWindowY;
		SaveWindowSettingsToIni();
		if (g_mainWindow && g_app.overlayVisible)
			PositionOverlayWindow(g_mainWindow);

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
			if (other != actionIndex && g_app.*(kBindableActions[other].button) == button &&
				ActionsCanConflict(kBindableActions[actionIndex], kBindableActions[other]))
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

	// ---------------------------------------------------------------------
	// Toypad LED mirror: the physical pad's three RGB LED regions (left,
	// center, right) glow during keystone puzzles, driven by the game's HID
	// LED commands (0xC0..0xC8). The exact wire byte layout is still
	// unconfirmed, so ApplyLedCommand is the seam that a future parser (and
	// the mock demo below) calls; it needs only a pad region, a mode and a
	// color. The overlay renders a soft tinted halo per region using the same
	// box-blur glow as every other cached surface, quantized so a fade reuses
	// a bounded set of cached halos instead of re-blurring every frame.
	// ---------------------------------------------------------------------

	// Slot indices covered by each region (0=left, 1=center, 2=right), one
	// entry per physical pad: pad 2 = left, pad 1 = center, pad 3 = right.
	constexpr std::array<std::array<int, 3>, 3> kLedRegionSlots = {{
		{0, 3, 4}, // left
		{1, 1, 1}, // center (single circular slot)
		{2, 5, 6}, // right
	}};
	constexpr std::array<int, 3> kLedRegionSlotCount = {3, 1, 3};

	constexpr int kLedGlowRadius = 14;
	constexpr int kLedGlowMargin = kLedGlowRadius + 8;
	constexpr int kLedIntensityLevels = 9;

	// The wire carries raw LED drive levels, not display colour: the pad's
	// three channels have very different luminous efficiency, and the game
	// compensates by driving them against a white point of (255, 110, 24).
	// Painted straight to a screen everything skews orange - green sits at
	// 43% and blue at 9% of where sRGB would put them. Confirmed against
	// captures across several scenes (including a mid-fade frame, which
	// scaled all three channels by the same factor, so the drive is linear)
	// - see toypad-led-color-fix.md. Applied once, here, so every consumer
	// (halo glow, pad tint) gets true colour without touching the raw wire
	// state the fade/flash math above operates on.
	constexpr int kLedWhiteR = 255, kLedWhiteG = 110, kLedWhiteB = 24;
	void CalibrateLedColor(uint8_t r, uint8_t g, uint8_t b, BYTE& outR, BYTE& outG, BYTE& outB)
	{
		outR = static_cast<BYTE>(std::clamp(static_cast<int>(r) * 255 / kLedWhiteR, 0, 255));
		outG = static_cast<BYTE>(std::clamp(static_cast<int>(g) * 255 / kLedWhiteG, 0, 255));
		outB = static_cast<BYTE>(std::clamp(static_cast<int>(b) * 255 / kLedWhiteB, 0, 255));
	}

	// Milliseconds per toypad duration tick. The wire carries single-byte tick
	// counts, so this is what turns them into wall-clock time. Calibrated from a
	// LEGO Dimensions trace: the game's idle rainbow issues a Fade All with
	// tickTime = 0x1E (30) and re-issues the next colour ~1.2s later, so one
	// tick lands at ~40ms and a fade fills exactly the interval before it.
	constexpr int kLedTickMs = 40;

	// The LED geometry helpers take the pad layout they are drawing against
	// rather than reading kPadCells directly, so the sneak-peek HUD (which
	// spreads the same seven pads across the whole screen) mirrors the LEDs
	// through exactly this code instead of a parallel copy of it. Everything
	// on the overlay's own path keeps passing kPadCells by default.
	RECT LedRegionBounds(int region, const std::array<RECT, 7>& cells = kPadCells)
	{
		RECT bbox{INT_MAX, INT_MAX, INT_MIN, INT_MIN};
		for (int i = 0; i < kLedRegionSlotCount[region]; ++i)
		{
			const RECT& cell = cells[static_cast<size_t>(kLedRegionSlots[region][i])];
			bbox.left = std::min(bbox.left, cell.left);
			bbox.top = std::min(bbox.top, cell.top);
			bbox.right = std::max(bbox.right, cell.right);
			bbox.bottom = std::max(bbox.bottom, cell.bottom);
		}
		return bbox;
	}

	// Adds one region's pad silhouettes to `path`, translated by (dx, dy) so
	// the same shapes can be drawn either in window space or in a small
	// cached halo bitmap's local space.
	void AppendLedRegionShape(Gdiplus::GraphicsPath& path, int region, float dx, float dy,
		const std::array<RECT, 7>& cells = kPadCells)
	{
		for (int i = 0; i < kLedRegionSlotCount[region]; ++i)
		{
			const RECT& cell = cells[static_cast<size_t>(kLedRegionSlots[region][i])];
			const Gdiplus::RectF rect(
				static_cast<float>(cell.left) + dx,
				static_cast<float>(cell.top) + dy,
				static_cast<float>(cell.right - cell.left),
				static_cast<float>(cell.bottom - cell.top));
			if (kLedRegionSlots[region][i] == 1)
			{
				path.AddEllipse(rect);
			}
			else
			{
				const float radius = 14.0f;
				const float diameter = radius * 2.0f;
				const float right = rect.X + rect.Width;
				const float bottom = rect.Y + rect.Height;
				path.StartFigure();
				path.AddArc(rect.X, rect.Y, diameter, diameter, 180.0f, 90.0f);
				path.AddArc(right - diameter, rect.Y, diameter, diameter, 270.0f, 90.0f);
				path.AddArc(right - diameter, bottom - diameter, diameter, diameter, 0.0f, 90.0f);
				path.AddArc(rect.X, bottom - diameter, diameter, diameter, 90.0f, 90.0f);
				path.CloseFigure();
			}
		}
	}

	// Map a wire-protocol pad (1=center, 2=left, 3=right) to a region index.
	int LedRegionForPad(uint8_t pad)
	{
		switch (pad)
		{
		case 2: return 0; // left
		case 1: return 1; // center
		case 3: return 2; // right
		}
		return -1;
	}

	// Entry point for LED state: mirrors one pad region's glow. This is the
	// seam the future HID parser (and the mock demo) drives.
	//
	// fromR/G/B is the colour the real toypad fades *from* - the wire's
	// GET_LED snapshot carries this as the pad's pre-command colour (see
	// LedPollFrame below). It is ignored outside of LedMode::Fade.
	void ApplyLedCommand(uint8_t pad, LedMode mode, uint8_t r, uint8_t g, uint8_t b,
		int onTicks = 0, int offTicks = 0, int count = 0, int speedTicks = 0,
		uint8_t fromR = 0, uint8_t fromG = 0, uint8_t fromB = 0)
	{
		const int region = LedRegionForPad(pad);
		if (region < 0)
			return;
		LedRegion& led = g_app.ledRegions[static_cast<size_t>(region)];
		led.mode = mode;
		led.r = r; led.g = g; led.b = b;
		// Stored verbatim, never second-guessed: only ComputeLedFrame's Fade
		// branch reads these, so there is nothing to normalise for the other
		// modes - and rewriting them here once desynced this state from the
		// wire snapshot it came from.
		led.fromR = fromR; led.fromG = fromG; led.fromB = fromB;
		led.onTicks = onTicks; led.offTicks = offTicks;
		led.count = count; led.speedTicks = speedTicks;
		led.cycleStart = GetTickCount();
	}

	// Computes a region's current on-screen appearance (intensity + colour)
	// from the clock, converting the wire's tick durations to milliseconds,
	// and writes the result into led.intensity / led.curR,G,B.
	//
	// Flash blinks led.r/g/b on and off against dark, and finite flashes
	// expire dark once their cycles run out - that part matches the real
	// toypad, which is documented to do exactly this.
	//
	// Fade is different from what a "fade" name suggests: the real toypad
	// does NOT ramp one colour's brightness up or down. It cross-fades
	// between the pad's pre-command colour (fromR/G/B) and the new target
	// (r/g/b), alternating direction every `speedTicks` step: step 0 goes
	// from->to, step 1 to->from, step 2 from->to, and so on. `count` is the
	// number of steps to run (0 = repeat forever); this is also why the
	// protocol's documented behaviour is "odd count lands on the new colour,
	// even count lands back on the old one" - see TOYPAD_LED_PROTOCOL.md.
	// A fade is therefore always fully lit (intensity 1) - only its colour
	// moves, it never dims to black the way Flash does.
	//
	// Settling a finished fade to LedMode::Solid (rather than leaving it in
	// Fade forever at t=1) matters beyond looks: the emulator suppresses a
	// repeated identical command, so a region that never reports itself
	// "done" would never be told to light up again by the next command.
	void ComputeLedFrame(LedRegion& led, DWORD now)
	{
		led.curR = led.r; led.curG = led.g; led.curB = led.b;

		if (led.mode == LedMode::Off)
		{
			led.intensity = 0.0f;
			return;
		}
		if (led.mode == LedMode::Solid)
		{
			led.intensity = 1.0f;
			return;
		}

		const int elapsed = static_cast<int>(now - led.cycleStart);

		if (led.mode == LedMode::Flash)
		{
			const int period = (led.onTicks + led.offTicks) * kLedTickMs;
			if (period <= 0)
			{
				led.intensity = 1.0f; // Degenerate flash: treat as steady rather than strobing.
				return;
			}
			if (led.count > 0 && elapsed >= period * led.count)
			{
				led.mode = LedMode::Off;
				led.intensity = 0.0f;
				return;
			}
			const DWORD phase = static_cast<DWORD>(elapsed) % static_cast<DWORD>(period);
			led.intensity = phase < static_cast<DWORD>(led.onTicks * kLedTickMs) ? 1.0f : 0.0f;
			return;
		}

		// Fade.
		led.intensity = 1.0f;
		const int step = (led.speedTicks > 0 ? led.speedTicks : 25) * kLedTickMs;
		int stepIndex = elapsed / step;
		float localT = static_cast<float>(elapsed % step) / static_cast<float>(step);
		if (led.count > 0 && stepIndex >= led.count)
		{
			// Settle exactly on the endpoint the real hardware would land on:
			// the last step run was (count - 1), so its parity decides
			// whether that's the "to" or "from" colour.
			stepIndex = led.count - 1;
			localT = 1.0f;
		}
		// Ease each leg (matches the smoothed "breathing" feel of the
		// previous single-colour implementation) instead of a hard linear
		// ramp between the two colours.
		const float eased = static_cast<float>(0.5 - 0.5 * std::cos(localT * 3.14159265358979323846));
		// Quantise the blend fraction to the same discrete levels used for
		// Flash/Solid glow caching (kLedIntensityLevels) so a long or
		// infinite fade produces a bounded set of colours - and therefore a
		// bounded glow-bitmap cache - instead of one unique RGB per frame.
		const float quantised = std::round(eased * (kLedIntensityLevels - 1)) / static_cast<float>(kLedIntensityLevels - 1);
		const bool forward = (stepIndex % 2) == 0; // even step: from -> to
		const uint8_t startR = forward ? led.fromR : led.r;
		const uint8_t startG = forward ? led.fromG : led.g;
		const uint8_t startB = forward ? led.fromB : led.b;
		const uint8_t endR = forward ? led.r : led.fromR;
		const uint8_t endG = forward ? led.g : led.fromG;
		const uint8_t endB = forward ? led.b : led.fromB;
		led.curR = static_cast<uint8_t>(startR + (endR - startR) * quantised + 0.5f);
		led.curG = static_cast<uint8_t>(startG + (endG - startG) * quantised + 0.5f);
		led.curB = static_cast<uint8_t>(startB + (endB - startB) * quantised + 0.5f);

		if (led.count > 0 && stepIndex == led.count - 1 && localT >= 1.0f)
		{
			// Landed for good: fold the settled colour into r/g/b so
			// LedMode::Solid keeps drawing it correctly afterwards, even
			// when the count was even and the settled colour is fromR/G/B.
			led.r = led.curR; led.g = led.curG; led.b = led.curB;
			led.mode = LedMode::Solid;
		}
	}

	// Advances the animation clock and reports whether any region is still
	// animating (flash/fade) and therefore needs continuous repaints.
	bool AdvanceLedAnimation()
	{
		const DWORD now = GetTickCount();
		bool animating = false;
		for (auto& led : g_app.ledRegions)
		{
			if (led.mode == LedMode::Flash || led.mode == LedMode::Fade)
				animating = true;
			ComputeLedFrame(led, now);
		}
		return animating;
	}

	// Cached soft tinted halo for one region at one brightness level. The
	// expensive blur runs once per (region, color, level); steady-state fades
	// then just DrawImage a cached bitmap.
	// `layoutTag` separates one layout's cached halos from another's: the
	// overlay (0) and the sneak-peek HUD (1) can land on the same bitmap
	// size by coincidence, and their region shapes are not the same.
	Gdiplus::Bitmap* RenderLedHalo(int region, uint8_t r, uint8_t g, uint8_t b, int level,
		const std::array<RECT, 7>& cells = kPadCells, int layoutTag = 0)
	{
		if (level <= 0)
			return nullptr;

		const RECT bbox = LedRegionBounds(region, cells);
		if (bbox.left == INT_MAX)
			return nullptr;

		const int w = (bbox.right - bbox.left) + kLedGlowMargin * 2;
		const int h = (bbox.bottom - bbox.top) + kLedGlowMargin * 2;
		const unsigned int color = (static_cast<unsigned int>(level) << 24) |
			(static_cast<unsigned int>(r) << 16) | (static_cast<unsigned int>(g) << 8) | b;
		const GlossKey key{GlossKind::LedHalo, region, layoutTag, color, w, h};
		const auto cached = g_glossCache.find(key);
		if (cached != g_glossCache.end())
			return cached->second;

		Gdiplus::Bitmap* bitmap = new Gdiplus::Bitmap(w, h, PixelFormat32bppPARGB);
		Gdiplus::Graphics gfx(bitmap);
		gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

		Gdiplus::GraphicsPath path;
		AppendLedRegionShape(path, region,
			static_cast<float>(kLedGlowMargin - bbox.left),
			static_cast<float>(kLedGlowMargin - bbox.top), cells);

		BYTE calR, calG, calB;
		CalibrateLedColor(r, g, b, calR, calG, calB);
		const float intensity = static_cast<float>(level) / static_cast<float>(kLedIntensityLevels - 1);
		const BYTE rr = static_cast<BYTE>(calR * intensity);
		const BYTE gg = static_cast<BYTE>(calG * intensity);
		const BYTE bb = static_cast<BYTE>(calB * intensity);
		DrawGlow(gfx, path, RGB(rr, gg, bb), kLedGlowRadius, w, h);

		g_glossCache[key] = bitmap;
		return bitmap;
	}

	// Draws the glow for any lit LED region, behind the pads. Called from
	// Paint's pad screens before the pad loop.
	void DrawLedRegionGlows(Gdiplus::Graphics& g, const std::array<RECT, 7>& cells = kPadCells,
		int layoutTag = 0)
	{
		for (int region = 0; region < 3; ++region)
		{
			const LedRegion& led = g_app.ledRegions[static_cast<size_t>(region)];
			if (led.mode == LedMode::Off || led.intensity <= 0.004f)
				continue;
			const int level = std::clamp(
				static_cast<int>(led.intensity * (kLedIntensityLevels - 1) + 0.5f),
				1, kLedIntensityLevels - 1);
			Gdiplus::Bitmap* halo = RenderLedHalo(region, led.curR, led.curG, led.curB, level,
				cells, layoutTag);
			if (!halo)
				continue;
			const RECT bbox = LedRegionBounds(region, cells);
			g.DrawImage(halo, static_cast<int>(bbox.left) - kLedGlowMargin,
				static_cast<int>(bbox.top) - kLedGlowMargin);
		}
	}

	// Tints the actual pad surfaces with the LED colour, drawn on top of the
	// pad art so the glow is unmistakable (the soft halo in DrawLedRegionGlows
	// is behind the pads and would otherwise be hidden by the art). Alpha is
	// scaled by the region's live intensity so solid pads stay lit and flash
	// blinks read clearly; a fade is always fully lit and instead moves
	// between curR/G/B's two endpoint colours (see ComputeLedFrame).
	void DrawLedRegionTint(Gdiplus::Graphics& g, const std::array<RECT, 7>& cells = kPadCells)
	{
		// A flash's dark beat darkens the tile itself rather than merely
		// omitting the colour tint: a colour-tinted opacity toggle is
		// invisible whenever the flash colour matches the background
		// (black-on-dark, white-on-white both animate nothing) - see
		// toypad-led-color-fix.md. Brightness is colour-independent, so this
		// reads regardless of what colour the flash actually is.
		constexpr float kFlashDarkBrightness = 0.34f;
		constexpr BYTE kFlashDarkAlpha =
			static_cast<BYTE>((1.0f - kFlashDarkBrightness) * 255.0f + 0.5f);

		for (int region = 0; region < 3; ++region)
		{
			const LedRegion& led = g_app.ledRegions[static_cast<size_t>(region)];
			if (led.mode == LedMode::Off)
				continue;

			Gdiplus::GraphicsPath path;
			AppendLedRegionShape(path, region, 0.0f, 0.0f, cells); // window coordinates

			if (led.mode == LedMode::Flash && led.intensity <= 0.004f)
			{
				Gdiplus::SolidBrush dark(Gdiplus::Color(kFlashDarkAlpha, 0, 0, 0));
				g.FillPath(&dark, &path);
				continue;
			}
			if (led.intensity <= 0.004f)
				continue;

			BYTE calR, calG, calB;
			CalibrateLedColor(led.curR, led.curG, led.curB, calR, calG, calB);
			// 210 rather than the old 140: with the LEDs on the pads are drawn
			// as empty boxes, so this fill is the LED itself, not a stain on
			// top of artwork. Scaling by intensity keeps a flash's dark phase
			// legible as actually dark.
			const BYTE alpha = static_cast<BYTE>(std::clamp(210.0f * led.intensity, 0.0f, 255.0f));
			Gdiplus::SolidBrush brush(Gdiplus::Color(alpha, calR, calG, calB));
			g.FillPath(&brush, &path);
		}
	}

	// Mock feed: toggles a keystone-like scene so the LED glow can be seen
	// before the real wire parser lands. Key 'G' from the pad viewer.
	void ToggleLedDemo()
	{
		if (!g_app.ledMirrorEnabled)
		{
			g_app.status = L"Toypad LEDs are off - turn them on in Settings first.";
			return;
		}
		static bool demoOn = false;
		demoOn = !demoOn;
		if (demoOn)
		{
			// Durations are toypad ticks (kLedTickMs each), matching the wire:
			// 8 ticks ~ 320ms of flash, 35 ticks ~ 1.4s per fade leg.
			ApplyLedCommand(1, LedMode::Solid, 40, 120, 255);     // center: solid blue
			ApplyLedCommand(2, LedMode::Flash, 0, 220, 90, 8, 8); // left: flashing green
			// right: cross-fading red <-> blue forever (count 0), demonstrating
			// the real toypad's two-colour fade rather than a single-hue pulse.
			ApplyLedCommand(3, LedMode::Fade, 230, 40, 40, 0, 0, 0, 35, 40, 40, 230);
			g_app.status = L"LED demo ON - press G to stop.";
		}
		else
		{
			ApplyLedCommand(1, LedMode::Off, 0, 0, 0);
			ApplyLedCommand(2, LedMode::Off, 0, 0, 0);
			ApplyLedCommand(3, LedMode::Off, 0, 0, 0);
			g_app.status = L"LED demo OFF.";
		}
	}

	// ---------------------------------------------------------------------
	// LED polling transport to the emulator's Toypad listener. A 5-byte GET_LED
	// request elicits a fixed LED snapshot, so the running game's keystone-
	// puzzle pad glows can be mirrored without any push channel. The poll runs
	// on its own thread and is serialised with LOAD/REMOVE/MOVE via
	// g_socketMutex (the listener serves one connection at a time); parsed
	// snapshots are marshalled back to the UI thread via kLedMessage so the
	// glow never races the renderer.
	//
	// Wire format v2 (see TOYPAD_LED_PROTOCOL.md): { 'L', serial, version,
	// region count, then 3 regions x 12 bytes (pad, mode, r, g, b, fromR,
	// fromG, fromB, onMs, offMs, count, speedMs) } = 40 bytes. fromR/G/B is
	// the colour the pad was showing before this command, which real toypad
	// hardware fades *from* - see ComputeLedFrame for why that matters.
	// ---------------------------------------------------------------------
	constexpr UINT kLedMessage = WM_APP + 3;
	constexpr uint8_t kGetLedCommand = 0x04;
	constexpr uint8_t kLedPollHeaderSize = 5;
	constexpr uint8_t kLedProtocolVersion = 2;
	constexpr uint8_t kLedResponseSize = 4 + 3 * 12;
	constexpr int kLedPollIntervalMs = 33;

	std::atomic<bool> g_ledPollRunning{false};
	std::thread g_ledPollThread;
	uint8_t g_lastLedSerial = 0xFF;

	struct LedPollFrame
	{
		uint8_t serial;
		std::array<uint8_t, 3> pad{};
		std::array<uint8_t, 3> mode{};
		std::array<uint8_t, 3> r{};
		std::array<uint8_t, 3> g{};
		std::array<uint8_t, 3> b{};
		std::array<uint8_t, 3> fromR{};
		std::array<uint8_t, 3> fromG{};
		std::array<uint8_t, 3> fromB{};
		std::array<uint8_t, 3> onTicks{};
		std::array<uint8_t, 3> offTicks{};
		std::array<uint8_t, 3> count{};
		std::array<uint8_t, 3> speedTicks{};
	};

	// The last wire snapshot actually applied to each region, kept verbatim so
	// change-detection compares wire-against-wire.
	//
	// It deliberately does NOT compare against the live LedRegion: the
	// animation mutates that state as it runs (a finished fade settles to
	// Solid, a finished flash settles to Off, and a settled fade folds its
	// final colour into r/g/b), while the emulator keeps reporting the
	// original command forever - it stores commands, not animation progress.
	// Comparing the two therefore mismatches permanently once anything
	// completes, and because the serial is global, the next command to *any*
	// pad would re-apply and restart every already-finished animation.
	struct LedWireState
	{
		bool applied = false;
		uint8_t mode = 0;
		uint8_t r = 0, g = 0, b = 0;
		uint8_t fromR = 0, fromG = 0, fromB = 0;
		uint8_t onTicks = 0, offTicks = 0, count = 0, speedTicks = 0;
	};
	std::array<LedWireState, 3> g_lastAppliedLed{};

	// Applies one polled snapshot, re-issuing a command only when a region's
	// state actually changed (so an unchanged flash isn't restarted every poll).
	// Runs on the UI thread via kLedMessage.
	void ApplyLedFrame(const LedPollFrame& frame)
	{
		// The poll thread is stopped when the mirror is off, but a snapshot
		// posted just before that can still arrive afterwards.
		if (!g_app.ledMirrorEnabled)
			return;
		if (frame.serial == g_lastLedSerial)
			return;
		g_lastLedSerial = frame.serial;
		bool changed = false;
		for (size_t i = 0; i < 3; ++i)
		{
			const int region = LedRegionForPad(frame.pad[i]);
			if (region < 0)
				continue;
			LedWireState& last = g_lastAppliedLed[static_cast<size_t>(region)];
			const auto mode = static_cast<LedMode>(frame.mode[i]);
			if (last.applied && last.mode == frame.mode[i] && last.r == frame.r[i] &&
				last.g == frame.g[i] && last.b == frame.b[i] && last.fromR == frame.fromR[i] &&
				last.fromG == frame.fromG[i] && last.fromB == frame.fromB[i] &&
				last.onTicks == frame.onTicks[i] && last.offTicks == frame.offTicks[i] &&
				last.count == frame.count[i] && last.speedTicks == frame.speedTicks[i])
				continue;
			ApplyLedCommand(frame.pad[i], mode, frame.r[i], frame.g[i], frame.b[i],
				frame.onTicks[i], frame.offTicks[i], frame.count[i], frame.speedTicks[i],
				frame.fromR[i], frame.fromG[i], frame.fromB[i]);
			last = LedWireState{true, frame.mode[i], frame.r[i], frame.g[i], frame.b[i],
				frame.fromR[i], frame.fromG[i], frame.fromB[i],
				frame.onTicks[i], frame.offTicks[i], frame.count[i], frame.speedTicks[i]};
			changed = true;
		}
		// Diagnostic: report the LED serial so it's obvious whether the poll is
		// receiving data. The number advances only when the game drives the pads.
		g_app.status = L"LED: serial=" + std::to_wstring(frame.serial) + (changed ? L" (lit)" : L" (idle)");
		if (g_app.overlayVisible)
			InvalidateRect(g_mainWindow, nullptr, FALSE);
	}

	void LedPollThread(HWND window)
	{
		while (g_ledPollRunning)
		{
			{
				std::lock_guard lock(g_socketMutex);
				const SOCKET clientSocket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
				if (clientSocket != INVALID_SOCKET)
				{
					// A dead listener must never pin the shared mutex for long.
					DWORD recvTimeout = 250;
					setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO,
						reinterpret_cast<const char*>(&recvTimeout), sizeof(recvTimeout));

					sockaddr_in address{};
					address.sin_family = AF_INET;
					address.sin_port = htons(g_app.port);
					address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
					// This runs on its own thread, so a slow connect() can't
					// freeze the window the way it could on the UI-thread
					// callers - but it would still hold g_socketMutex for
					// however long it took, starving every Load/Move/Clear
					// and custom-tag request behind it. Same bounded wait.
					if (!ConnectWithTimeout(clientSocket, address, kSocketConnectTimeoutMs))
					{
						closesocket(clientSocket);
					}
					else
					{
						const uint8_t request[kLedPollHeaderSize] = {kGetLedCommand, 0, 0, 0, 0};
						bool ok = SendAll(clientSocket, request, sizeof(request));
						std::array<uint8_t, kLedResponseSize> response{};
						if (ok)
						{
							size_t received = 0;
							while (received < response.size())
							{
								const int got = recv(clientSocket,
									reinterpret_cast<char*>(response.data() + received),
									static_cast<int>(response.size() - received), 0);
								if (got == SOCKET_ERROR || got == 0)
								{
									ok = false;
									break;
								}
								received += static_cast<size_t>(got);
							}
						}
						closesocket(clientSocket);
						// The version byte guards against a stale build on either end
						// silently misreading a different-sized snapshot (v1 was 30
						// bytes with no fromR/G/B; see TOYPAD_LED_PROTOCOL.md).
						if (ok && response[0] == 0x4C && response[2] == kLedProtocolVersion && response[3] == 0x03)
						{
							LedPollFrame* frame = new LedPollFrame();
							frame->serial = response[1];
							for (size_t i = 0; i < 3; ++i)
							{
								const size_t off = 4 + i * 12;
								frame->pad[i] = response[off + 0];
								frame->mode[i] = response[off + 1];
								frame->r[i] = response[off + 2];
								frame->g[i] = response[off + 3];
								frame->b[i] = response[off + 4];
								frame->fromR[i] = response[off + 5];
								frame->fromG[i] = response[off + 6];
								frame->fromB[i] = response[off + 7];
								frame->onTicks[i] = response[off + 8];
								frame->offTicks[i] = response[off + 9];
								frame->count[i] = response[off + 10];
								frame->speedTicks[i] = response[off + 11];
							}
							if (!PostMessageW(window, kLedMessage, reinterpret_cast<WPARAM>(frame), 0))
								delete frame;
						}
					}
				}
			}
			if (!g_ledPollRunning)
				break;
			Sleep(kLedPollIntervalMs);
		}
	}

	void StartLedPoll(HWND window)
	{
		bool expected = false;
		if (!g_ledPollRunning.compare_exchange_strong(expected, true))
			return;
		g_ledPollThread = std::thread(LedPollThread, window);
	}

	void StopLedPoll()
	{
		if (!g_ledPollRunning.exchange(false))
			return;
		if (g_ledPollThread.joinable())
			g_ledPollThread.join();
	}

	std::wstring DescribeLedMirror()
	{
		return g_app.ledMirrorEnabled ? L"On (mirror the game)" : L"Off";
	}

	// Turning the mirror off stops the poll thread (no more traffic to the
	// listener) and darkens the three regions, so the pads go back to their
	// plain colours instead of freezing on whatever the game last sent.
	// Turning it on clears the remembered serial, so the very next snapshot
	// counts as a change and lights the pads again immediately.
	void SetLedMirrorEnabled(bool enabled)
	{
		g_app.ledMirrorEnabled = enabled;
		if (enabled)
		{
			g_lastLedSerial = 0xFF;
			// The regions were darkened when the mirror went off, so forget
			// what was last applied - otherwise an unchanged snapshot would
			// be suppressed as a no-op and the pads would stay dark.
			for (auto& last : g_lastAppliedLed)
				last.applied = false;
			StartLedPoll(g_mainWindow);
		}
		else
		{
			StopLedPoll();
			for (auto& led : g_app.ledRegions)
			{
				led.mode = LedMode::Off;
				led.intensity = 0.0f;
			}
		}
		if (g_mainWindow)
			InvalidateRect(g_mainWindow, nullptr, FALSE);
	}

	void ToggleLedMirror()
	{
		SetLedMirrorEnabled(!g_app.ledMirrorEnabled);
		SaveInputSettingsToIni();
		g_app.status = L"Toypad LEDs: " + DescribeLedMirror();
	}

	// ---------------------------------------------------------------------
	// Sneak peek (the Settings row; the HUD itself lives further down)
	// ---------------------------------------------------------------------

	std::wstring DescribeSneakPeek()
	{
		const wchar_t* size = nullptr;
		switch (g_app.peekSizeChoice)
		{
		case 1: size = L"Small"; break;
		case 2: size = L"Medium"; break;
		case 3: size = L"Large"; break;
		default: break;
		}
		if (!size)
			return L"Off";
		// The hold button rides along in the value, because a size on its own
		// says nothing about how the HUD is summoned - and the row for the
		// binding itself sits several categories further down the list.
		return std::wstring(size) + L" (hold " +
			DescribeControllerMask(g_app.buttonSneakPeek) + L")";
	}

	void CycleSneakPeek(int direction)
	{
		const size_t count = static_cast<size_t>(kPeekSizeChoiceCount);
		const size_t step = direction < 0 ? count - 1 : 1;
		g_app.peekSizeChoice = (g_app.peekSizeChoice + step) % count;
		// A size change while the HUD is up would leave the old geometry on
		// screen until the button is released, so drop it and let the next
		// hold rebuild it.
		HidePeekWindow(true);
		SaveInputSettingsToIni();
		g_app.status = g_app.peekSizeChoice == 0
			? L"Sneak peek: Off"
			: L"Sneak peek: " + DescribeSneakPeek() + L" while the game runs.";
	}

	// ---------------------------------------------------------------------
	// Pad skins (list building + the Settings row)
	// ---------------------------------------------------------------------

	// Builds the selectable skin list: every complete folder that existed
	// under Assets/Pads at build time (compiled in), followed by every
	// complete folder found under "Assets/Pads" next to the .exe at startup.
	// The on-disk half is what makes "add a folder of your own pad art" work
	// without rebuilding: drop Assets/Pads/MySkin/{left_upper,center,...}.png
	// beside LegoToypad.exe and it shows up as another choice.
	void BuildPadSkinList()
	{
		g_padSkins.clear();
		g_padDiskArtPaths.clear();
		ReleasePadDiskBitmaps();

		for (size_t i = 0; i < kPadSkinCount; ++i)
		{
			PadSkinOption option;
			option.name = kPadSkins[i].name;
			for (size_t slot = 0; slot < 7; ++slot)
				option.artIds[slot] = kPadSkins[i].slotResourceIds[slot];
			g_padSkins.push_back(std::move(option));
		}

		std::error_code ec;
		const auto padsRoot = GetExecutableDirectory() / L"Assets" / L"Pads";
		if (!std::filesystem::is_directory(padsRoot, ec))
			return;

		std::vector<std::filesystem::path> folders;
		for (const auto& entry : std::filesystem::directory_iterator(padsRoot, ec))
		{
			if (entry.is_directory(ec))
				folders.push_back(entry.path());
		}
		std::sort(folders.begin(), folders.end(), [](const std::filesystem::path& a, const std::filesystem::path& b) {
			return _wcsicmp(a.filename().c_str(), b.filename().c_str()) < 0;
		});

		for (const auto& folder : folders)
		{
			const std::wstring folderName = folder.filename().wstring();
			// A folder that shipped with the build is already in the list
			// under its own name; the disk copy would just be a duplicate.
			const bool alreadyBuiltIn = std::any_of(g_padSkins.begin(), g_padSkins.end(),
				[&](const PadSkinOption& skin) { return _wcsicmp(skin.name.c_str(), folderName.c_str()) == 0; });
			if (alreadyBuiltIn || _wcsicmp(folderName.c_str(), L"default") == 0)
				continue;

			// All seven or none: a half-skin would draw holes where the
			// missing pads are, which looks like a bug rather than a choice.
			std::array<std::filesystem::path, 7> files;
			bool complete = true;
			for (size_t slot = 0; slot < 7 && complete; ++slot)
			{
				complete = false;
				for (const wchar_t* extension : {L".png", L".jpg", L".jpeg"})
				{
					const auto candidate = folder / (std::wstring(kPadSlotArtNames[slot]) + extension);
					if (std::filesystem::is_regular_file(candidate, ec))
					{
						files[slot] = candidate;
						complete = true;
						break;
					}
				}
			}
			if (!complete)
				continue;

			PadSkinOption option;
			option.name = folderName;
			for (size_t slot = 0; slot < 7; ++slot)
			{
				g_padDiskArtPaths.push_back(files[slot]);
				// Synthetic negative ids: -1 is g_padDiskArtPaths[0], and so on.
				option.artIds[slot] = -static_cast<int>(g_padDiskArtPaths.size());
			}
			g_padSkins.push_back(std::move(option));
		}
	}

	std::wstring DescribePadSkin()
	{
		if (g_padSkins.empty())
			return L"(none bundled)";
		const size_t index = std::min(g_app.padSkinIndex, g_padSkins.size() - 1);
		return g_padSkins[index].name + L" (" + std::to_wstring(index + 1) + L"/" +
			std::to_wstring(g_padSkins.size()) + L")";
	}

	void CyclePadSkin(int direction)
	{
		if (g_padSkins.size() <= 1)
		{
			g_app.status = L"Only one pad skin is available. Add a folder of pad art to "
				L"Assets/Pads next to the app to get more.";
			return;
		}
		// Every skin is a separate set of cache entries keyed by its own art
		// ids, so switching is a plain repaint - nothing to invalidate, and
		// flipping back to a skin you already used is instant.
		const size_t step = direction < 0 ? g_padSkins.size() - 1 : 1;
		g_app.padSkinIndex = (g_app.padSkinIndex + step) % g_padSkins.size();
		SaveInputSettingsToIni();
		g_app.status = L"Pad skin: " + DescribePadSkin();
		if (g_mainWindow)
			InvalidateRect(g_mainWindow, nullptr, FALSE);
	}

	// ---------------------------------------------------------------------
	// Window transparency
	// ---------------------------------------------------------------------

	std::wstring DescribeOpacity()
	{
		return std::to_wstring(g_app.opacityPercent) + L"% opaque" +
			(g_app.opacityPercent >= 100 ? L" (solid)" : L"");
	}

	void CycleOpacity(int direction)
	{
		// A plain +/-15 clamped to [floor, 100] rather than a step through a
		// fixed list: Right always makes the panel more opaque, Left always
		// less, and it simply stops at either end instead of wrapping around
		// (wrapping a magnitude like this - "keep going right and you're
		// suddenly back at ghostly" - reads as a bug, not a feature).
		g_app.opacityPercent = std::clamp(
			g_app.opacityPercent + direction * kSettingsPercentStep, kOpacityFloorPercent, 100);
		SaveWindowSettingsToIni();
		g_app.status = L"Window transparency: " + DescribeOpacity();
		if (g_mainWindow)
			InvalidateRect(g_mainWindow, nullptr, FALSE);
	}

	// ---------------------------------------------------------------------
	// Sound effects
	// ---------------------------------------------------------------------

	std::wstring DescribeSoundVolume()
	{
		return std::to_wstring(g_app.soundVolume) + L"%";
	}

	void CycleSoundVolume(int direction)
	{
		// Same plain +/-15, clamped rather than wrapped - see CycleOpacity.
		// 0 is a reachable, valid value here (not guarded the way opacity's
		// floor is): a silent volume is just the effects being inaudible,
		// with the separate Sound effects switch above it still doing the
		// on/off job.
		g_app.soundVolume = std::clamp(
			g_app.soundVolume + direction * kSettingsPercentStep, 0, 100);
		SaveInputSettingsToIni();
		g_app.status = L"Sound volume: " + DescribeSoundVolume();
		// Play a blip at the level just chosen, so the setting demonstrates
		// itself instead of being a number you have to go and test. Alternate
		// between the two UI sounds rather than always previewing Select, so
		// adjusting the slider actually demonstrates the Navigate sound too.
		static bool previewNavigate = false;
		previewNavigate ? PlayNavigateSound() : PlaySelectSound();
		previewNavigate = !previewNavigate;
	}
	std::wstring DescribeSoundEffects()
	{
		if (kSfxNavigateResourceId == 0 && kSfxSelectResourceId == 0)
			return L"Off (no sounds bundled)";
		return g_app.soundEffects ? L"On" : L"Off";
	}

	void ToggleSoundEffects()
	{
		g_app.soundEffects = !g_app.soundEffects;
		if (!g_app.soundEffects)
			StopUiSounds();
		SaveInputSettingsToIni();
		g_app.status = L"Sound effects: " + DescribeSoundEffects();
		if (g_app.soundEffects)
			PlaySelectSound(); // let the user hear what they just turned on
	}

	// ---------------------------------------------------------------------
	// The Settings list
	// ---------------------------------------------------------------------

	SettingValueTone ToneForSwitch(bool on)
	{
		return on ? SettingValueTone::On : SettingValueTone::Off;
	}

	std::vector<SettingsEntry> BuildSettingsEntries()
	{
		std::vector<SettingsEntry> entries;
		const auto heading = [&entries](const wchar_t* title) {
			entries.push_back({SettingAction::Heading, title, {}, SettingValueTone::Neutral, 0, 0});
		};
		const auto row = [&entries](SettingAction action, std::wstring label, std::wstring value,
			SettingValueTone tone = SettingValueTone::Neutral, ButtonMask icons = 0,
			size_t bindingIndex = 0) {
			entries.push_back({action, std::move(label), std::move(value), tone, icons, bindingIndex});
		};

		heading(L"Appearance");
		row(SettingAction::Background, L"Background", DescribeBackgroundChoice());
		row(SettingAction::PadSkin, L"Pad skin", DescribePadSkin());
		row(SettingAction::Opacity, L"Window transparency", DescribeOpacity());
		row(SettingAction::WindowPlacement, L"Window", DescribeWindowPlacement());
		row(SettingAction::SneakPeek, L"Sneak peek", DescribeSneakPeek(),
			ToneForSwitch(g_app.peekSizeChoice != 0));
		row(SettingAction::LedMirror, L"Toypad LEDs", DescribeLedMirror(),
			ToneForSwitch(g_app.ledMirrorEnabled));

		heading(L"Audio");
		// Nothing bundled means the switch is effectively off however it is
		// set, and the row should not claim otherwise.
		const bool soundsAvailable = kSfxNavigateResourceId != 0 || kSfxSelectResourceId != 0;
		row(SettingAction::SoundEffects, L"Sound effects", DescribeSoundEffects(),
			ToneForSwitch(g_app.soundEffects && soundsAvailable));
		row(SettingAction::SoundVolume, L"Sound volume", DescribeSoundVolume());

		heading(L"Controls");
		if (g_app.shortcutType == ShortcutType::Controller)
			row(SettingAction::Shortcut, L"Toggle shortcut", {}, SettingValueTone::Neutral,
				g_app.shortcutControllerMask);
		else
			row(SettingAction::Shortcut, L"Toggle shortcut", DescribeShortcut());
		row(SettingAction::ConfirmStyle, L"Confirm button", DescribeConfirmButtonMode());
		row(SettingAction::ButtonStyle, L"Button labels", DescribeButtonStyle());

		heading(L"Button bindings");
		for (size_t i = 0; i < kBindableActions.size(); ++i)
		{
			row(SettingAction::Binding, std::wstring(L"Button - ") + kBindableActions[i].label, {},
				SettingValueTone::Neutral, g_app.*(kBindableActions[i].button), i);
		}

		heading(L"System");
		row(SettingAction::ClearAllPads, L"Clear all pads", {});
		row(SettingAction::WebRemote, L"Web remote", DescribeWebRemote(),
			ToneForSwitch(g_app.webEnabled));
		row(SettingAction::ResetDefaults, L"Reset all settings to defaults", {});
		return entries;
	}

	// settingsIndex points at a row in the full (headings included) list, so
	// painting and navigation never have to translate between two numbering
	// schemes. Headings are simply skipped when moving and refused when
	// activating.
	void ClampSettingsSelection()
	{
		const std::vector<SettingsEntry> entries = BuildSettingsEntries();
		if (entries.empty())
		{
			g_app.settingsIndex = 0;
			return;
		}
		if (g_app.settingsIndex >= entries.size())
			g_app.settingsIndex = entries.size() - 1;
		// Land on a real setting, never on a heading (which is where index 0
		// starts out).
		while (g_app.settingsIndex < entries.size() && IsSettingsHeading(entries[g_app.settingsIndex]))
			++g_app.settingsIndex;
		if (g_app.settingsIndex >= entries.size())
		{
			g_app.settingsIndex = entries.size() - 1;
			while (g_app.settingsIndex > 0 && IsSettingsHeading(entries[g_app.settingsIndex]))
				--g_app.settingsIndex;
		}
	}

	// Rows visible at once in the Settings viewport, and the geometry the
	// paint code and the scroll follow both use.
	// The panel used to start at y=92, which put its top edge underneath the
	// wordmark band (kTopMargin 20 + kTopBarH 90 = 110). It now begins below
	// that, and shows one row fewer so the list plus the capture hint still
	// clear the bottom of the window.
	constexpr int kSettingsTop = 130;
	constexpr int kSettingsPitch = 32;
	constexpr size_t kSettingsVisibleRows = 12;

	void ScrollSettingsIntoView()
	{
		const int focused = static_cast<int>(g_app.settingsIndex);
		// A setting directly under a heading brings its heading along, so you
		// can always see which category you are in.
		if (focused - 1 < g_app.settingsTopRow)
			g_app.settingsTopRow = std::max(0, focused - 1);
		while (focused >= g_app.settingsTopRow + static_cast<int>(kSettingsVisibleRows))
			++g_app.settingsTopRow;
		const int maxTop = std::max(0,
			static_cast<int>(BuildSettingsEntries().size()) - static_cast<int>(kSettingsVisibleRows));
		g_app.settingsTopRow = std::clamp(g_app.settingsTopRow, 0, maxTop);
	}

	void MoveSettingsSelection(int direction)
	{
		const std::vector<SettingsEntry> entries = BuildSettingsEntries();
		if (entries.empty())
			return;
		const int step = direction < 0 ? -1 : 1;
		int index = static_cast<int>(g_app.settingsIndex);
		const int count = static_cast<int>(entries.size());
		// Walk past headings; the wrap keeps the list circular like every
		// other menu in the picker.
		for (int guard = 0; guard < count; ++guard)
		{
			index = (index + step + count) % count;
			if (!IsSettingsHeading(entries[static_cast<size_t>(index)]))
				break;
		}
		g_app.settingsIndex = static_cast<size_t>(index);
		ScrollSettingsIntoView();
	}

	// Changes the focused row's value. `direction` is +1 for Confirm and
	// Right, -1 for Left, so a list of choices can be walked either way
	// instead of only ever forwards. Rows that are not a value at all - the
	// capture rows, the one-shot actions - only respond to Confirm, which is
	// what `allowAction` marks.
	void AdjustSettingsEntry(int direction, bool allowAction)
	{
		ClampSettingsSelection();
		const std::vector<SettingsEntry> entries = BuildSettingsEntries();
		if (g_app.settingsIndex >= entries.size())
			return;
		const SettingsEntry& entry = entries[g_app.settingsIndex];
		switch (entry.action)
		{
		case SettingAction::Heading: break;
		case SettingAction::Shortcut: if (allowAction) BeginShortcutCapture(); break;
		case SettingAction::Binding: if (allowAction) BeginBindingCapture(entry.bindingIndex); break;
		case SettingAction::ClearAllPads: if (allowAction) ClearAllPads(true); break;
		case SettingAction::ResetDefaults: if (allowAction) ResetSettingsToDefaults(); break;
		case SettingAction::ConfirmStyle: ToggleConfirmButtonMode(); break;
		case SettingAction::ButtonStyle: CycleButtonStyle(direction); break;
		case SettingAction::Background: CycleBackgroundChoice(direction); break;
		case SettingAction::PadSkin: CyclePadSkin(direction); break;
		case SettingAction::Opacity: CycleOpacity(direction); break;
		case SettingAction::WindowPlacement: ToggleWindowDraggable(); break;
		case SettingAction::SneakPeek: CycleSneakPeek(direction); break;
		case SettingAction::LedMirror: ToggleLedMirror(); break;
		case SettingAction::SoundEffects: ToggleSoundEffects(); break;
		case SettingAction::SoundVolume: CycleSoundVolume(direction); break;
		case SettingAction::WebRemote: ToggleWebRemote(); break;
		}
	}

	void ActivateSettingsEntry()
	{
		AdjustSettingsEntry(1, true);
	}

	// Left/Right on a settings row. Only ever changes a value, never triggers
	// a capture or a destructive one-shot - reaching "Reset all settings to
	// defaults" with the stick and having it fire sideways would be a nasty
	// surprise.
	void AdjustSettingsValue(int direction)
	{
		AdjustSettingsEntry(direction, false);
	}

	// Franchise grid layout: 4 columns of large logo tiles, scrolled vertically.
	// Origin Y sits well clear of the persistent wordmark bar above it
	// (kTopMargin + kTopBarH = 110) - it used to start at 106, before the
	// header even ended, crowding the first row against the logo.
	constexpr int kFranchiseOriginX = 40;
	constexpr int kFranchiseOriginY = 128;
	constexpr int kFranchisePitchX = 210;
	constexpr int kFranchisePitchY = 108;
	constexpr int kFranchiseTileW = 190;
	constexpr int kFranchiseTileH = 100;
	constexpr int kTileGlowMargin = 6;

	// Roster grid layout lives with the roster navigation constants above,
	// because navigation and painting both need the same visual row model.

	// Right-edge vertical scroll bar (Scroll_Bar.png) shown on scrollable
	// lists when their content overflows the visible rows. Kept inside the
	// overlay's right margin, clear of the grid panels. Slim enough, and
	// with enough margin, that it never overlaps the Settings panel's
	// right edge (kSettingsPanelX mirrored, at kOverlayWidth - 30).
	constexpr int kScrollBarW = 10;
	constexpr int kScrollBarMarginX = 16;
	constexpr int kScrollBarMinThumbH = 22;

	// Capsule (pill) menus.
	constexpr int kPillWidth = 340;
	constexpr int kPillRowH = 36;
	constexpr int kPillPadV = 7;

	// The pad surface only - the glossy tile (or, with the LEDs on, its bare
	// outline). The occupant portrait is deliberately NOT drawn here: it goes
	// on in DrawPadOccupant after DrawLedRegionTint, so the LED colour passes
	// behind the figure instead of washing over it. An empty pad is just the
	// bare tile - no placeholder dot, no label - matching the reference design.
	void DrawPad(Gdiplus::Graphics& g, size_t index)
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

		// The focused pad springs about its own centre as the selection lands
		// on it, then sits still. Its halo and its art are scaled by the same
		// factor about the same point, so the two never separate.
		const float scale = selected ? SelectionTapScale() : 1.0f;
		const float cx = (cell.left + cell.right) / 2.0f;
		const float cy = (cell.top + cell.bottom) / 2.0f;

		// The halo goes down first, so the pad's own art sits on top of it and
		// only the light spilling past the edge is visible.
		if (selected)
		{
			Gdiplus::Bitmap* glow = RenderFocusGlow(
				static_cast<int>(cell.right - cell.left), static_cast<int>(cell.bottom - cell.top),
				index == 1 ? FocusShape::Circle : FocusShape::RoundedPad, kSelectionGlow);
			DrawImageScaledAbout(g, glow, static_cast<float>(cell.left - kFocusGlowMargin),
				static_cast<float>(cell.top - kFocusGlowMargin), cx, cy, scale, SelectionGlowAlpha());
		}
		// The pad you are moving FROM gets the same rich two-pass halo, in
		// blue, so it reads as "this one's on the move" at a glance instead
		// of the old single-pass glow that (on top of being the wrong
		// colour) was too faint to notice. Static, not tapped/animated - it
		// sits on this pad for as long as the destination pick lasts, not
		// just the instant it was landed on.
		if (isMoveSource)
		{
			Gdiplus::Bitmap* glow = RenderFocusGlow(
				static_cast<int>(cell.right - cell.left), static_cast<int>(cell.bottom - cell.top),
				index == 1 ? FocusShape::Circle : FocusShape::RoundedPad, kMoveSourceGlow);
			g.DrawImage(glow, static_cast<int>(cell.left) - kFocusGlowMargin,
				static_cast<int>(cell.top) - kFocusGlowMargin);
		}

		// Toypad LEDs on: the pads are bare outlines so the LED colour reads
		// directly instead of staining the printed glass art.
		Gdiplus::Bitmap* pad = RenderPad(static_cast<int>(index), cell, visual, occupantColor,
			g_app.ledMirrorEnabled);
		if (pad)
		{
			DrawImageScaledAbout(g, pad, static_cast<float>(cell.left - kPadGlowMargin),
				static_cast<float>(cell.top - kPadGlowMargin), cx, cy, scale);
		}
	}

	// The loaded figure's portrait, drawn on top of the LED tint so a lit pad
	// never hides who is standing on it. No name text is drawn here either;
	// the portrait alone is the label.
	void DrawPadOccupant(Gdiplus::Graphics& g, size_t index)
	{
		if (!g_app.padState[index].occupied)
			return;

		const RECT& cell = kPadCells[index];
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
		{
			// Rides the focused pad's spring, about the same centre, so the
			// figure stays planted on its tile instead of sliding across it.
			const float scale = index == g_app.slotIndex ? SelectionTapScale() : 1.0f;
			DrawImageScaledAbout(g, portrait, static_cast<float>(circleX - kPortraitMargin),
				static_cast<float>(circleY - kPortraitMargin),
				(cell.left + cell.right) / 2.0f, (cell.top + cell.bottom) / 2.0f, scale);
		}
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

	// ---------------------------------------------------------------------
	// Toypad sneak peek
	// ---------------------------------------------------------------------
	// A second, read-only view of the very same seven pads, drawn straight
	// over the running game for as long as the peek button is held.
	//
	// Why it is a separate window rather than another screen of the picker:
	// the picker overlay takes the foreground and holds the input-ownership
	// event, which is exactly what makes the emulator go still while it is
	// open. This window is created WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, is
	// never activated, never touches g_inputOwnershipEvent and has no hit
	// area at all - mouse clicks fall through it to whatever is underneath,
	// and the held trigger reaches the game like any other button. The game
	// keeps playing; you just get to see the pads.
	//
	// It owns no state of its own. Everything it draws comes from
	// g_app.padState and g_app.ledRegions, so a load / move / clear (from
	// the picker, the web remote, or anywhere else) and every toypad LED
	// change land on it at the same moment they land on the overlay.

	HWND g_peekWindow = nullptr;
	bool g_peekShown = false;   // window is up (possibly mid fade-out)
	bool g_peekHiding = false;
	DWORD g_peekFadeStart = 0;
	// Screen-space geometry of the seven pads for the monitor the HUD is
	// currently spread across; rebuilt on every show, so a resolution change
	// or a move to another monitor is picked up automatically.
	std::array<RECT, 7> g_peekCells{};

	// The peek's layout is NOT a scaled copy of kPadCells: the overlay packs
	// the toypad into a 900x610 panel, while the HUD pushes the two side
	// sections down into the bottom corners and the centre pad up to the top
	// edge, leaving the middle of the screen - where the game actually is -
	// clear. Individual pad proportions still come from kPadCells so the
	// shapes stay the real toypad's.
	std::array<RECT, 7> ComputePeekCells(int width, int height)
	{
		const float scale = kPeekSizeScales[std::min(g_app.peekSizeChoice,
			static_cast<size_t>(kPeekSizeChoiceCount - 1))];
		// kPadCells' own pad height is the unit everything else is measured
		// in, so a pad keeps its aspect ratio at any HUD size.
		const float s = (height * kPeekPadHeightFraction * scale) / 136.0f;
		const auto S = [s](float v) { return v * s; };
		const float marginX = width * kPeekMarginXFraction;
		const float marginY = height * kPeekMarginYFraction;
		const float gutter = S(10.0f); // between the two lower pads of a section
		const float vgap = S(24.0f);   // between a section's upper and lower row
		const auto R = [](float x, float y, float w, float h) {
			return RECT{static_cast<LONG>(std::lround(x)), static_cast<LONG>(std::lround(y)),
				static_cast<LONG>(std::lround(x + w)), static_cast<LONG>(std::lround(y + h))};
		};

		std::array<RECT, 7> cells{};
		const float lowerTop = height - marginY - S(136.0f);
		const float upperTop = lowerTop - vgap - S(136.0f);

		// Left section (pad 2): lower-left corner, upper pad stacked above
		// the outer of the two lower ones.
		cells[3] = R(marginX, lowerTop, S(136.0f), S(136.0f));
		cells[4] = R(marginX + S(136.0f) + gutter, lowerTop, S(164.0f), S(136.0f));
		cells[0] = R(marginX, upperTop, S(140.0f), S(136.0f));

		// Right section (pad 3): the mirror image of the left one.
		cells[6] = R(width - marginX - S(136.0f), lowerTop, S(136.0f), S(136.0f));
		cells[5] = R(width - marginX - S(136.0f) - gutter - S(164.0f), lowerTop,
			S(164.0f), S(136.0f));
		cells[2] = R(width - marginX - S(140.0f), upperTop, S(140.0f), S(136.0f));

		// Centre section (pad 1): the single circular pad, top-centre.
		cells[1] = R((width - S(188.0f)) / 2.0f, marginY, S(188.0f), S(184.0f));
		return cells;
	}

	bool PeekFadeActive()
	{
		return g_peekFadeStart != 0 &&
			ElapsedFraction(g_peekFadeStart, g_peekHiding ? kPeekFadeOutMs : kPeekFadeInMs) < 1.0f;
	}

	// 0 = fully invisible, 1 = fully shown, following whichever fade is running.
	float PeekShownFraction()
	{
		if (!g_peekShown)
			return 0.0f;
		if (g_peekFadeStart == 0)
			return 1.0f;
		const float t = EaseOutCubic(
			ElapsedFraction(g_peekFadeStart, g_peekHiding ? kPeekFadeOutMs : kPeekFadeInMs));
		return g_peekHiding ? 1.0f - t : t;
	}

	BYTE PeekAlpha()
	{
		// The HUD rides the same transparency setting as the overlay panel,
		// so one slider controls how much of the game shows through both.
		return static_cast<BYTE>(std::clamp(
			static_cast<int>(TargetOverlayAlpha() * PeekShownFraction() + 0.5f), 0, 255));
	}

	// Starts a fade that is already `fromShownFraction` of the way there, so
	// tapping the peek button on and off reverses the fade instead of
	// restarting it from the far end.
	void BeginPeekFade(bool out, float fromShownFraction)
	{
		g_peekHiding = out;
		const DWORD duration = out ? kPeekFadeOutMs : kPeekFadeInMs;
		const float done = out ? 1.0f - fromShownFraction : fromShownFraction;
		// The fraction is post-easing; invert the ease so the fade resumes at
		// the brightness it was actually showing rather than jumping.
		const float linear = 1.0f - std::cbrt(std::clamp(1.0f - done, 0.0f, 1.0f));
		g_peekFadeStart = GetTickCount() - static_cast<DWORD>(linear * duration);
		if (g_peekFadeStart == 0)
			g_peekFadeStart = 1;
	}

	void PaintPeekWindow(HWND window);

	void ShowPeekWindow()
	{
		if (!g_peekWindow || g_app.peekSizeChoice == 0)
			return;
		if (g_peekShown && !g_peekHiding)
			return;
		if (g_peekShown && g_peekHiding)
		{
			// Caught mid-dismiss - turn the fade around in place; the window
			// is already positioned and its cells are already built.
			BeginPeekFade(false, PeekShownFraction());
			PaintPeekWindow(g_peekWindow);
			return;
		}

		// Spread across whatever monitor the game is on. The foreground
		// window is the game in every case that matters here (the picker is
		// hidden whenever the HUD is up), with the last known one and then
		// the primary monitor as fallbacks.
		HWND reference = GetForegroundWindow();
		if (!reference || reference == g_mainWindow || reference == g_peekWindow)
		{
			reference = (g_app.previousForegroundWindow && IsWindow(g_app.previousForegroundWindow))
				? g_app.previousForegroundWindow
				: nullptr;
		}
		HMONITOR monitor = MonitorFromWindow(reference ? reference : g_peekWindow,
			MONITOR_DEFAULTTOPRIMARY);
		MONITORINFO info{};
		info.cbSize = sizeof(info);
		if (!GetMonitorInfoW(monitor, &info))
			return;

		const int width = info.rcMonitor.right - info.rcMonitor.left;
		const int height = info.rcMonitor.bottom - info.rcMonitor.top;
		if (width <= 0 || height <= 0)
			return;

		g_peekCells = ComputePeekCells(width, height);
		SetWindowPos(g_peekWindow, HWND_TOPMOST, info.rcMonitor.left, info.rcMonitor.top,
			width, height, SWP_NOACTIVATE);

		g_peekShown = true;
		BeginPeekFade(false, 0.0f);
		// The first presented frame has to already be the faded-in one, so
		// paint before showing rather than after.
		PaintPeekWindow(g_peekWindow);
		// SW_SHOWNA, never SW_SHOW: showing this window must not take
		// activation away from the game.
		ShowWindow(g_peekWindow, SW_SHOWNA);
	}

	// `immediate` skips the fade entirely - used when the picker overlay is
	// coming up (two views of the same pads on screen at once would just look
	// like a bug) and when the size setting changes underneath the HUD.
	void HidePeekWindow(bool immediate)
	{
		if (!g_peekShown)
			return;
		if (immediate)
		{
			if (g_peekWindow)
				ShowWindow(g_peekWindow, SW_HIDE);
			g_peekShown = false;
			g_peekHiding = false;
			g_peekFadeStart = 0;
			return;
		}
		if (g_peekHiding)
			return;
		BeginPeekFade(true, PeekShownFraction());
		PaintPeekWindow(g_peekWindow);
	}

	// Driven from the tick, so the last frame of the fade is actually
	// presented before the window disappears.
	void FinishPeekHide()
	{
		if (!g_peekShown || !g_peekHiding || PeekFadeActive())
			return;
		HidePeekWindow(true);
	}

	// The HUD's whole frame: LED halos, the pad surfaces, the LED tint over
	// them and the occupants on top - the same order, and the same helpers,
	// the overlay's own pad screens use.
	void PaintPeekWindow(HWND window)
	{
		if (!window)
			return;
		RECT client{};
		GetClientRect(window, &client);
		const int width = client.right - client.left;
		const int height = client.bottom - client.top;
		if (width <= 0 || height <= 0)
			return;

		// Same off-screen 32bpp top-down DIB + UpdateLayeredWindow present as
		// Paint uses, and for the same reason: per-pixel alpha is what lets
		// the pads sit on the game with soft edges and nothing else drawn.
		// The buffer is kept across frames; at monitor size, rebuilding it
		// every frame would be several megabytes of churn per second.
		static HDC s_peekDC = nullptr;
		static HBITMAP s_peekBitmap = nullptr;
		static HGDIOBJ s_peekOldBitmap = nullptr;
		static void* s_peekBits = nullptr;
		static int s_peekW = 0;
		static int s_peekH = 0;
		if (!s_peekDC || s_peekW != width || s_peekH != height)
		{
			if (s_peekDC)
			{
				SelectObject(s_peekDC, s_peekOldBitmap);
				DeleteObject(s_peekBitmap);
				DeleteDC(s_peekDC);
				s_peekDC = nullptr;
			}
			HDC screenDC = GetDC(nullptr);
			BITMAPINFO info{};
			info.bmiHeader.biSize = sizeof(info.bmiHeader);
			info.bmiHeader.biWidth = width;
			info.bmiHeader.biHeight = -height; // top-down, so row 0 is the top row
			info.bmiHeader.biPlanes = 1;
			info.bmiHeader.biBitCount = 32;
			info.bmiHeader.biCompression = BI_RGB;
			s_peekBits = nullptr;
			s_peekDC = CreateCompatibleDC(screenDC);
			s_peekBitmap = CreateDIBSection(screenDC, &info, DIB_RGB_COLORS, &s_peekBits, nullptr, 0);
			ReleaseDC(nullptr, screenDC);
			if (!s_peekDC || !s_peekBitmap)
			{
				if (s_peekBitmap)
					DeleteObject(s_peekBitmap);
				if (s_peekDC)
					DeleteDC(s_peekDC);
				s_peekDC = nullptr;
				s_peekBitmap = nullptr;
				s_peekBits = nullptr;
				return;
			}
			s_peekOldBitmap = SelectObject(s_peekDC, s_peekBitmap);
			s_peekW = width;
			s_peekH = height;
		}
		if (!s_peekBits)
			return;

		std::memset(s_peekBits, 0, static_cast<size_t>(width) * static_cast<size_t>(height) * 4);

		Gdiplus::Bitmap frame(width, height, width * 4, PixelFormat32bppPARGB,
			static_cast<BYTE*>(s_peekBits));
		Gdiplus::Graphics g(&frame);
		g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
		g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

		if (g_app.ledMirrorEnabled)
			DrawLedRegionGlows(g, g_peekCells, 1);

		for (size_t index = 0; index < g_peekCells.size(); ++index)
		{
			const RECT& cell = g_peekCells[index];
			const PadSlot& slot = g_app.padState[index];
			// No selection or move-source states here: nothing is focused on
			// a HUD nobody can navigate, so a pad is simply empty or loaded.
			Gdiplus::Bitmap* pad = RenderPad(static_cast<int>(index), cell,
				slot.occupied ? PadVisual::Occupied : PadVisual::Idle,
				slot.occupied ? slot.ringColor : 0, g_app.ledMirrorEnabled);
			if (pad)
			{
				g.DrawImage(pad, static_cast<int>(cell.left) - kPadGlowMargin,
					static_cast<int>(cell.top) - kPadGlowMargin);
			}
		}

		if (g_app.ledMirrorEnabled)
			DrawLedRegionTint(g, g_peekCells);

		for (size_t index = 0; index < g_peekCells.size(); ++index)
		{
			const PadSlot& slot = g_app.padState[index];
			if (!slot.occupied)
				continue;
			const RECT& cell = g_peekCells[index];
			const int cellWidth = cell.right - cell.left;
			const int cellHeight = cell.bottom - cell.top;
			// Same 80%-of-the-shorter-side portrait as the overlay's pads;
			// the bounds are wider here because the HUD is sized off the
			// monitor rather than a fixed 900x610 panel.
			const int diameter = std::clamp(
				static_cast<int>(std::min(cellWidth, cellHeight) * 0.80f), 32, 512);
			Gdiplus::Bitmap* portrait = RenderPortrait(slot.portraitResourceId, slot.ringColor,
				false, diameter);
			if (portrait)
			{
				g.DrawImage(portrait,
					static_cast<int>(cell.left) + (cellWidth - diameter) / 2 - kPortraitMargin,
					static_cast<int>(cell.top) + (cellHeight - diameter) / 2 - kPortraitMargin);
			}
		}

		POINT source{0, 0};
		SIZE size{width, height};
		BLENDFUNCTION blend{AC_SRC_OVER, 0, PeekAlpha(), AC_SRC_ALPHA};
		UpdateLayeredWindow(window, nullptr, nullptr, &size, s_peekDC, &source,
			0, &blend, ULW_ALPHA);
	}

	// Everything the HUD draws comes from pad + LED state, so one cheap
	// signature over that state says whether a repaint would even look
	// different. LED intensity is quantized to the same levels the cached
	// halos use, so a fade only asks for a frame when it actually steps.
	uint64_t PeekContentSignature()
	{
		uint64_t signature = 1469598103934665603ull;
		const auto mix = [&signature](uint64_t value) {
			signature = (signature ^ value) * 1099511628211ull;
		};
		for (const auto& slot : g_app.padState)
		{
			mix(slot.occupied ? 1u : 0u);
			mix(static_cast<uint64_t>(slot.portraitResourceId));
			mix(slot.ringColor);
		}
		for (const auto& led : g_app.ledRegions)
		{
			mix(static_cast<uint64_t>(led.mode));
			mix((static_cast<uint64_t>(led.curR) << 16) |
				(static_cast<uint64_t>(led.curG) << 8) | led.curB);
			mix(static_cast<uint64_t>(std::clamp(
				static_cast<int>(led.intensity * (kLedIntensityLevels - 1) + 0.5f),
				0, kLedIntensityLevels - 1)));
		}
		mix(g_app.ledMirrorEnabled ? 1u : 0u);
		return signature;
	}

	// Called from every controller poll with the buttons currently held down
	// across all pads. The HUD is strictly a hold: it comes up while the
	// binding is down and goes away when it is released, and it never
	// competes with the picker - the overlay being up suppresses it entirely.
	void UpdatePeekHold(bool anyConnected, ButtonMask heldButtons)
	{
		const bool wants = g_app.peekSizeChoice != 0 &&
			g_app.buttonSneakPeek != 0 &&
			anyConnected &&
			!g_app.overlayVisible &&
			!g_app.capturingShortcut &&
			g_app.capturingBindingIndex < 0 &&
			(heldButtons & g_app.buttonSneakPeek) == g_app.buttonSneakPeek;
		if (wants)
			ShowPeekWindow();
		else
			HidePeekWindow(false);
	}

	LRESULT CALLBACK PeekWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
	{
		// The frame is presented by UpdateLayeredWindow from PaintPeekWindow,
		// not from a WM_PAINT device context, so painting here only has to
		// validate the window and re-present the current state.
		if (message == WM_PAINT)
		{
			PAINTSTRUCT paint{};
			BeginPaint(window, &paint);
			EndPaint(window, &paint);
			if (g_peekShown)
				PaintPeekWindow(window);
			return 0;
		}
		// WS_EX_TRANSPARENT already makes the window click-through; answering
		// HTTRANSPARENT as well means even a hit test that reaches here
		// refuses to claim the cursor.
		if (message == WM_NCHITTEST)
			return HTTRANSPARENT;
		return DefWindowProcW(window, message, wParam, lParam);
	}

	// Capsule menu floating above the selected pad (PadAction) or centered
	// (PlusPicker). The focused row is drawn in its capsule highlight.
	void DrawPillMenu(Gdiplus::Graphics& g, int x, int y, size_t rows,
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
	// ring + name below), on a shared characters_tile panel. The franchise logo
	// is drawn above this in the screen painter. Panel width and label height
	// are sized from the longest build name so nothing gets cropped, and the
	// whole row is anchored to the panel so it stays centred.
	void DrawPlusPickerMenu(Gdiplus::Graphics& g)
	{
		if (!g_app.plusGroup || g_app.plusGroup->builds.empty())
			return;

		const auto& builds = g_app.plusGroup->builds;
		const size_t count = builds.size();

		constexpr int diam = 90;
		constexpr int step = 156;            // cell width / centre-to-centre
		constexpr int circleY = 160;
		constexpr int kLabelGap = 8;         // circle bottom -> label top
		constexpr int kPanelPadX = 20;
		constexpr int kPanelPadTop = 20;

		// Fit the label block to the longest name: measure its wrapped height at
		// the shrink floor (17px) and give it that much room, so the auto-fit
		// text renderer never has to truncate a long vehicle name.
		int labelH = 34;
		for (const auto& build : builds)
		{
			const float textH = MeasureWrappedTextHeight(
				g, EntryDisplayName(build), step, 400, 17.0f);
			labelH = std::max(labelH, static_cast<int>(std::ceil(textH)) + 6);
		}
		if (labelH > 100)
			labelH = 100;

		// Panel covers the label row (count cells of `step`) plus padding, and
		// is centred on the overlay. The circles sit at each cell's centre, so
		// the group is symmetric within the panel.
		const int rowW = static_cast<int>(count) * step;
		const int panelW = rowW + kPanelPadX * 2;
		const int panelX = (kOverlayWidth - panelW) / 2;
		const int panelY = circleY - kPanelPadTop;
		const int panelH = kPanelPadTop + diam + kLabelGap + labelH + 16;
		if (Gdiplus::Bitmap* panel = RenderScaledAsset(
			kCharactersTileResourceId, panelW, panelH, 16))
		{
			g.DrawImage(panel, panelX, panelY);
		}

		// One lookup for the whole picker, not per build - every build here
		// belongs to the same VehicleGroup/franchise.
		std::wstring franchise;
		FindFranchiseForVehicleGroup(g_app.plusGroup, franchise);

		const int cell0X = panelX + kPanelPadX;   // left edge of the first cell
		for (size_t i = 0; i < count; ++i)
		{
			const RosterEntry& build = builds[i];
			const int cellLeft = cell0X + static_cast<int>(i) * step;
			const int centerX = cellLeft + step / 2;
			const bool focused = i == g_app.plusBuildIndex;
			Gdiplus::Bitmap* visual = build.portraitResourceId != 0
				? RenderPortrait(build.portraitResourceId, build.ringColor, focused, diam)
				: RenderPlaceholder(build.name.empty() ? L'?' : build.name[0], build.ringColor, focused, diam);
			// RenderPortrait's bitmap is (diam + 2*margin) with the ring centred
			// in it, so offset by the full half-extent or the ring drifts right
			// of centre by diam/2 and no longer lines up with its label.
			if (visual)
				g.DrawImage(visual, centerX - kPortraitMargin - diam / 2, circleY - kPortraitMargin);
			if (!franchise.empty() && IsFavorited(franchise, g_app.plusGroup->baseName, true, build.buildNumber))
			{
				DrawFavoriteStar(g, static_cast<float>(centerX + diam / 2 - 6),
					static_cast<float>(circleY + 6), 11.0f);
			}
			// The label is centred in its cell (same width the height was
			// measured at), so it hugs the circle and stays inside the panel.
			DrawTextWrappedCenteredFit(g, EntryDisplayName(build),
				cellLeft, circleY + diam + kLabelGap, step, labelH,
				focused ? RGB(255, 236, 190) : RGB(214, 220, 230));
		}
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

		// Cached, for the same reason as the wordmark and even more so: the
		// scroll bar asset is 512x9984, so drawing it scaled straight to a
		// ~14px-wide thumb is a 5-megapixel resample. There are only a handful
		// of distinct thumb heights per screen, so the cache stays tiny.
		Gdiplus::Bitmap* bar = RenderScaledAsset(kScrollBarResourceId, kScrollBarW, thumbH, 0);
		if (!bar)
			return;
		const int x = kOverlayWidth - kScrollBarW - kScrollBarMarginX;
		g.DrawImage(bar, x, thumbY);
	}

	void DrawFranchiseGrid(Gdiplus::Graphics& g)
	{
		// Logical index 0 is the Favorites tile (custom_bin.png) when shown;
		// logical i>=1 (or i>=0 without the tile) maps to the franchise at
		// display slot of the current sort's franchiseDisplayList.
		const size_t realCount = g_app.franchiseDisplayList.size();
		const size_t logicalCount = realCount + (g_app.showFavoritesTile ? 1 : 0);
		if (logicalCount == 0)
			return;
		const size_t totalRows = (logicalCount + kFranchiseCols - 1) / kFranchiseCols;
		for (size_t row = 0; row < kFranchiseVisibleRows; ++row)
		{
			for (size_t col = 0; col < kFranchiseCols; ++col)
			{
				const size_t index =
					(static_cast<size_t>(g_app.franchiseTopRow) + row) * kFranchiseCols + col;
				if (index >= logicalCount)
					break;
				const int x = kFranchiseOriginX + static_cast<int>(col) * kFranchisePitchX;
				const int y = kFranchiseOriginY + static_cast<int>(row) * kFranchisePitchY;
				const bool isFavoritesTile = g_app.showFavoritesTile && index == 0;
				const size_t slot = g_app.showFavoritesTile ? (index - 1) : index;
				const bool inRange = slot < g_app.franchiseDisplayList.size();
				const bool focused = isFavoritesTile
					? g_app.favoritesTileSelected
					: (!g_app.favoritesTileSelected && inRange &&
						g_app.franchiseDisplayList[slot] == g_app.franchiseIndex);
				const bool isReorganizeSource = g_app.reorganizingFranchise && !isFavoritesTile &&
					inRange && slot == g_app.reorganizeFranchiseSourceIndex;
				const int logoResourceId = isFavoritesTile
					? kCustomBinIconResourceId
					: kFranchises[g_app.franchiseDisplayList[slot]].logoResourceId;
				const float scale = focused ? SelectionTapScale() : 1.0f;
				const float cx = x + kFranchiseTileW / 2.0f;
				const float cy = y + kFranchiseTileH / 2.0f;
				if (focused)
				{
					Gdiplus::Bitmap* glow = RenderFocusGlow(
						kFranchiseTileW, kFranchiseTileH, FocusShape::RoundedTile, kSelectionGlow);
					DrawImageScaledAbout(g, glow, static_cast<float>(x - kFocusGlowMargin),
						static_cast<float>(y - kFocusGlowMargin), cx, cy, scale, SelectionGlowAlpha());
				}
				// The tile picked up to be reorganized keeps its own yellow
				// halo at its original spot, same idea as the pad-move
				// source highlight, while the gold cursor glow above shows
				// where it would land if dropped now.
				if (isReorganizeSource)
				{
					Gdiplus::Bitmap* glow = RenderFocusGlow(
						kFranchiseTileW, kFranchiseTileH, FocusShape::RoundedTile, kReorganizeGlow);
					g.DrawImage(glow, x - kFocusGlowMargin, y - kFocusGlowMargin);
				}
				Gdiplus::Bitmap* tile = RenderFranchiseTile(logoResourceId, focused);
				if (tile)
				{
					DrawImageScaledAbout(g, tile, static_cast<float>(x - kTileGlowMargin),
						static_cast<float>(y - kTileGlowMargin), cx, cy, scale);
				}
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

	void DrawRosterCategorySeparator(Gdiplus::Graphics& g, const RosterMetrics& metrics)
	{
		if (!IsRosterSeparatorVisible())
			return;

		const int gridW = static_cast<int>(kRosterCols) * kRosterPitchX;
		const int x = kRosterOriginX + (gridW - kRosterSeparatorW) / 2;
		const int y = GetRosterSeparatorY(g, metrics) + GetRosterOriginShift(g, metrics);
		DrawRoundedSeparator(g, x, y, kRosterSeparatorW, kRosterSeparatorH);
	}

	void DrawRosterGrid(Gdiplus::Graphics& g)
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
		// Shift it down when this roster's content is shorter than the
		// full 3-row budget kRosterOriginY assumes, so a short roster isn't
		// left pinned right under the header with all the slack dumped at
		// the bottom.
		const int originShift = GetRosterOriginShift(g, metrics);
		DrawRosterCategorySeparator(g, metrics);
		for (size_t row = 0; row < visibleRows; ++row)
		{
			for (size_t col = 0; col < kRosterCols; ++col)
			{
				const size_t visualRow = static_cast<size_t>(g_app.rosterTopRow) + row;
				if (visualRow >= totalRows || col >= GetRosterVisualRowItemCount(visualRow))
					break;
				const size_t slotIndex = GetRosterIndexAtVisualPosition(visualRow, col);
				const RosterSlot& slot = g_app.rosterSlots[slotIndex];
				const int x = kRosterOriginX + GetRosterRowColumnOffset(visualRow)
					+ static_cast<int>(col) * kRosterPitchX;
				int y = kRosterOriginY + originShift + static_cast<int>(row) * metrics.pitchY;
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
				// The tile picked up to be reorganized keeps its own yellow
				// halo at its original spot while the cursor (gold, drawn as
				// part of RenderPortrait/RenderPlusTile's own focus state)
				// shows where it would land if dropped now.
				if (g_app.reorganizingRoster && slotIndex == g_app.reorganizeRosterSourceIndex)
				{
					Gdiplus::Bitmap* glow = RenderFocusGlow(
						metrics.portraitDiameter, metrics.portraitDiameter, FocusShape::Circle, kReorganizeGlow);
					g.DrawImage(glow, circleX - kFocusGlowMargin, circleY - kFocusGlowMargin);
				}
				if (visual)
					g.DrawImage(visual, circleX - kPortraitMargin, circleY - kPortraitMargin);

				// Favorited-star badge: only useful while browsing a real
				// franchise's roster (every tile in the Favorites roster is
				// trivially favorited already, so it's skipped there).
				if (!g_app.favoritesTileSelected && slot.kind != RosterSlot::Kind::Plus && slot.entry &&
					IsFavorited(kFranchises[g_app.franchiseIndex].name,
						slot.kind == RosterSlot::Kind::Vehicle && slot.group ? slot.group->baseName : slot.entry->name,
						slot.kind == RosterSlot::Kind::Vehicle, slot.entry->buildNumber))
				{
					DrawFavoriteStar(g, static_cast<float>(circleX + metrics.portraitDiameter - 6),
						static_cast<float>(circleY + 6), 11.0f);
				}

				const int labelX = x + (kRosterPitchX - kRosterLabelW) / 2;
				DrawTextWrappedCenteredFit(g, label, labelX, y + metrics.portraitDiameter + 8, kRosterLabelW, metrics.labelH,
					focused ? RGB(255, 236, 190) : RGB(214, 220, 230));
			}
		}

		const int trackTop = kRosterOriginY + originShift - kRosterPanelPadY;
		const int trackBottom = GetRosterContentBottom(g, metrics) + originShift;
		DrawScrollBar(g, trackTop, trackBottom, totalRows, kRosterVisibleRows, g_app.rosterTopRow);
	}

	// The logo drawn above the roster/build-picker grid: the focused
	// franchise's logo normally, or the Favorites tile's custom_bin.png icon
	// when the roster being browsed is the aggregated favorites list.
	int CurrentRosterWorldLogoResourceId()
	{
		if (g_app.favoritesTileSelected)
			return kCustomBinIconResourceId;
		return kFranchises[g_app.franchiseIndex].logoResourceId;
	}

	// What kind of thing g_app.status just reported, classified from the
	// message text itself rather than a separate field - every message this
	// applies to is a literal string set right alongside it (ToggleFavorite/
	// ToggleFavoriteForFocused*, DropRosterReorder/DropFranchiseReorder), so
	// this deliberately doesn't touch the dozens of other g_app.status
	// call sites - they keep rendering as plain Neutral toasts, unchanged.
	enum class StatusTone { Neutral, Added, Removed, Moved };
	StatusTone ClassifyStatusTone(const std::wstring& status)
	{
		if (status.rfind(L"Added to favorites:", 0) == 0) return StatusTone::Added;
		if (status.rfind(L"Removed from favorites:", 0) == 0) return StatusTone::Removed;
		if (status.rfind(L"Moved:", 0) == 0) return StatusTone::Moved;
		if (status.rfind(L"Reorganizing:", 0) == 0) return StatusTone::Moved;
		return StatusTone::Neutral;
	}

	// The transient "Added to favorites: X" style banner - see
	// SyncStatusToast/StatusToastAlpha. Settings has its own status line
	// under the row list (only shown while capturing a shortcut/binding), so
	// this is skipped there to avoid showing the same text twice.
	void DrawStatusToast(Gdiplus::Graphics& g, int width, int height)
	{
		if (g_app.screen == Screen::Settings || g_app.status.empty())
			return;
		const float alpha = StatusToastAlpha();
		if (alpha <= 0.0f)
			return;

		constexpr float kToastH = 44.0f;
		constexpr float kToastPadX = 22.0f;
		constexpr float kToastMaxW = 620.0f;
		constexpr float kToastBottomInset = 20.0f;
		constexpr float kToastFontPx = 20.0f;

		const float textW = MeasureTextWidth(g, g_app.status, kToastFontPx);
		const float boxW = std::min(kToastMaxW, textW + kToastPadX * 2.0f);
		const float boxX = (width - boxW) / 2.0f;
		const float boxY = height - kToastBottomInset - kToastH;
		const Gdiplus::RectF box(boxX, boxY, boxW, kToastH);

		const StatusTone tone = ClassifyStatusTone(g_app.status);
		unsigned int borderColor = RGB(96, 200, 255); // today's plain blue, unchanged for Neutral/Added
		switch (tone)
		{
		case StatusTone::Removed: borderColor = RGB(230, 70, 70); break;
		case StatusTone::Moved: borderColor = RGB(230, 190, 60); break;
		default: break;
		}

		// A genuine soft glow (not just a colored border) for the three
		// "something happened" tones, same two-pass bloom+core halo as
		// every other focus glow in the app. Neutral toasts keep today's
		// plain border-only look - zero visual change for the other status
		// messages this doesn't apply to.
		if (tone != StatusTone::Neutral)
		{
			// Quantized so a session's worth of differently-sized toasts
			// doesn't grow the glow bitmap cache one entry per pixel width.
			constexpr float kWidthBucket = 20.0f;
			const int glowW = static_cast<int>(std::ceil(boxW / kWidthBucket)) * static_cast<int>(kWidthBucket);
			Gdiplus::Bitmap* glow = RenderFocusGlow(glowW, static_cast<int>(kToastH), FocusShape::Pill, borderColor);
			DrawImageWithAlpha(g, glow,
				Gdiplus::RectF(boxX + (boxW - glowW) / 2.0f - kFocusGlowMargin, boxY - kFocusGlowMargin,
					static_cast<float>(glow->GetWidth()), static_cast<float>(glow->GetHeight())),
				alpha);
		}

		Gdiplus::GraphicsPath path;
		AddRoundedRectPath(path, box, kToastH / 2.0f);

		Gdiplus::SolidBrush fill(Gdiplus::Color(static_cast<BYTE>(214.0f * alpha), 14, 20, 28));
		g.FillPath(&fill, &path);
		Gdiplus::Pen border(Gdiplus::Color(static_cast<BYTE>(150.0f * alpha),
			GetRValue(borderColor), GetGValue(borderColor), GetBValue(borderColor)), 1.5f);
		g.DrawPath(&border, &path);

		Gdiplus::Font font = MakeUIFont(kToastFontPx);
		Gdiplus::SolidBrush textBrush(Gdiplus::Color(static_cast<BYTE>(255.0f * alpha), 255, 255, 255));
		Gdiplus::StringFormat format(Gdiplus::StringFormatFlagsNoWrap);
		format.SetAlignment(Gdiplus::StringAlignmentCenter);
		format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
		format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
		g.DrawString(g_app.status.c_str(), -1, &font, box, &format, &textBrush);
	}

	// The franchise-sort name badge at top-centre of the browse screens, sitting
	// inline with the LEGO TOYPAD wordmark (top-left) and the "by harrysof"
	// credit (top-right). Default/User/Story draw their sort name as word-art
	// inside a high-radius pill with a coloured glow and a matching outline -
	// white for Default, red for User, blue for the Story (starter) roster.
	// Favorites has no name art, so that roster shows the custom_bin icon
	// (custom_bin.png) enlarged instead of a name plate.
	void DrawSortBadge(Gdiplus::Graphics& g, int width)
	{
		constexpr float kBadgeH = 62.0f;
		constexpr float kBadgePadX = 26.0f;
		constexpr float kBadgePadY = 8.0f;
		constexpr float kMaxBadgeW = 300.0f;
		// Top-aligned in the header band (same y as the world logo it replaces),
		// with the pill bottom clear of the roster panel that starts at y=94.
		constexpr float kBadgeTop = 24.0f;

		// Favorites: no name plate - show the custom_bin icon enlarged, centred.
		// This roster can be reached either by cycling to the Favorites sort or
		// by confirming the Favorites tile on the grid, so the check is on the
		// roster screen state rather than the sort value. The story roster
		// never shows it (its name plate is "Starter"), and `favoritesTileSelected`
		// can be stale there, so the story guard wins.
		if (g_app.screen == Screen::RosterList && g_app.favoritesTileSelected && !g_app.storyRosterActive)
		{
			Gdiplus::Bitmap* favLogo = GetAssetBitmap(kCustomBinIconResourceId);
			if (!favLogo)
				return;
			const Gdiplus::RectF box((width - 90.0f) / 2.0f, kBadgeTop, 90.0f, 66.0f);
			const float scale = std::min(
				box.Width / favLogo->GetWidth(), box.Height / favLogo->GetHeight());
			const int drawW = static_cast<int>(favLogo->GetWidth() * scale);
			const int drawH = static_cast<int>(favLogo->GetHeight() * scale);
			if (Gdiplus::Bitmap* cached = RenderScaledAsset(
				kCustomBinIconResourceId, drawW, drawH, 0))
				g.DrawImage(cached, box.X + (box.Width - drawW) / 2.0f,
					box.Y + (box.Height - drawH) / 2.0f);
			return;
		}

		int nameResId = 0;
		unsigned int glowColor = 0;
		switch (g_app.franchiseSort)
		{
		case AppState::FranchiseSort::User:
			nameResId = kSortUserResourceId;
			glowColor = RGB(255, 64, 64);   // red
			break;
		case AppState::FranchiseSort::Story:
			nameResId = kSortStarterResourceId;
			glowColor = RGB(66, 157, 255);  // blue
			break;
		case AppState::FranchiseSort::Default:
		default:
			nameResId = kSortDefaultResourceId;
			glowColor = RGB(240, 244, 250); // white
			break;
		}

		Gdiplus::Bitmap* nameArt = GetAssetBitmap(nameResId);
		if (!nameArt)
			return;

		const float scale = std::min(
			(kMaxBadgeW - kBadgePadX * 2.0f) / nameArt->GetWidth(),
			(kBadgeH - kBadgePadY * 2.0f) / nameArt->GetHeight());
		const float artW = nameArt->GetWidth() * scale;
		const float artH = nameArt->GetHeight() * scale;
		const float badgeW = artW + kBadgePadX * 2.0f;
		const float badgeX = (width - badgeW) / 2.0f;
		const float badgeY = kBadgeTop;
		const Gdiplus::RectF badge(badgeX, badgeY, badgeW, kBadgeH);

		// Soft halo of the pill outline in the sort's own colour.
		if (Gdiplus::Bitmap* glow = RenderFocusGlow(
			static_cast<int>(badgeW), static_cast<int>(kBadgeH), FocusShape::Pill, glowColor))
		{
			g.DrawImage(glow, badgeX - kFocusGlowMargin, badgeY - kFocusGlowMargin);
		}

		// Translucent pill body.
		Gdiplus::GraphicsPath path;
		AddRoundedRectPath(path, badge, kBadgeH / 2.0f);
		Gdiplus::SolidBrush fill(Gdiplus::Color(214, 14, 20, 28));
		g.FillPath(&fill, &path);

		// Crisp outline in the sort's colour.
		Gdiplus::Pen border(Gdiplus::Color(235, GetRValue(glowColor),
			GetGValue(glowColor), GetBValue(glowColor)), 2.5f);
		g.DrawPath(&border, &path);

		// The sort name word-art, centred inside the pill.
		if (Gdiplus::Bitmap* cachedArt = RenderScaledAsset(
			nameResId, static_cast<int>(artW), static_cast<int>(artH), 0))
		{
			g.DrawImage(cachedArt, badgeX + kBadgePadX, badgeY + (kBadgeH - artH) / 2.0f);
		}
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

		// Everything below is drawn into an off-screen 32-bit top-down DIB
		// and the finished frame is handed to UpdateLayeredWindow in one
		// call. Drawing the background fill and then content directly on the
		// window's own surface (the original approach) is visibly flickery
		// on a layered window, since DWM can composite a half-drawn frame;
		// building the whole frame off-screen and presenting it atomically
		// avoids that.
		//
		// The DIB has a real alpha channel, which is what makes soft glows
		// work over a transparent background. The old buffer was an opaque
		// compatible bitmap and "No Background" was faked with LWA_COLORKEY:
		// only pixels exactly equal to one near-black key color vanished, so
		// a glow's outer falloff - which blends *toward* that color without
		// ever reaching it - stayed opaque and dark, drawing a thick black
		// frame around every glowing element. With per-pixel alpha the
		// falloff simply fades out over whatever is behind the window.
		//
		// The buffer is created once and reused across frames; rebuilding it
		// on every paint was measurable churn on the ~30-60fps animation path.
		static HDC s_bufferDC = nullptr;
		static HBITMAP s_bufferBitmap = nullptr;
		static HGDIOBJ s_bufferOldBitmap = nullptr;
		static void* s_bufferBits = nullptr;
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
			BITMAPINFO info{};
			info.bmiHeader.biSize = sizeof(info.bmiHeader);
			info.bmiHeader.biWidth = width;
			info.bmiHeader.biHeight = -height; // top-down, so row 0 is the top row
			info.bmiHeader.biPlanes = 1;
			info.bmiHeader.biBitCount = 32;
			info.bmiHeader.biCompression = BI_RGB;
			s_bufferBits = nullptr;
			s_bufferDC = CreateCompatibleDC(windowDC);
			s_bufferBitmap = CreateDIBSection(windowDC, &info, DIB_RGB_COLORS, &s_bufferBits, nullptr, 0);
			s_bufferOldBitmap = SelectObject(s_bufferDC, s_bufferBitmap);
			s_bufferW = width;
			s_bufferH = height;
		}
		if (!s_bufferBits)
		{
			EndPaint(window, &paint);
			return;
		}

		// Start every frame fully transparent; whatever the UI draws is all
		// that ends up visible.
		std::memset(s_bufferBits, 0, static_cast<size_t>(width) * static_cast<size_t>(height) * 4);

		// GDI+ writes premultiplied BGRA straight into the DIB's pixels,
		// which is exactly the layout UpdateLayeredWindow expects. Every
		// draw below goes through GDI+ for that reason - a plain GDI call
		// here would leave alpha at 0 and punch a hole in the frame.
		Gdiplus::Bitmap frame(width, height, width * 4, PixelFormat32bppPARGB,
			static_cast<BYTE*>(s_bufferBits));
		Gdiplus::Graphics frameGraphics(&frame);
		frameGraphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
		frameGraphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

		Gdiplus::Bitmap* background = RenderBackground(width, height);
		if (background)
			frameGraphics.DrawImage(background, 0, 0);

		// Persistent wordmark in the top-left of every screen — enlarged.
		// Drawn straight onto the frame, outside the transition layer: the
		// backdrop and the branding are the parts that do NOT change when the
		// picker moves between screens, so fading them would just make the
		// whole window flicker on every keypress.
		//
		// It goes through RenderScaledAsset rather than DrawImage-ing the raw
		// asset: the source PNG is 3294x1853 and the target is ~160x90, so a
		// direct draw is a 6-megapixel high-quality bicubic resample - per
		// frame, on every screen. That was invisible while repaints only
		// happened on a keypress and became the dominant cost the moment
		// anything animated continuously. Cached, it is one blit.
		{
			Gdiplus::Bitmap* wordmark = GetAssetBitmap(kWordmarkResourceId);
			if (wordmark)
			{
				const Gdiplus::RectF box(kTopMargin, kTopMargin, 460.0f, kTopBarH);
				const float scale = std::min(box.Width / wordmark->GetWidth(), box.Height / wordmark->GetHeight());
				const int drawW = static_cast<int>(wordmark->GetWidth() * scale);
				const int drawH = static_cast<int>(wordmark->GetHeight() * scale);
				if (Gdiplus::Bitmap* cached = RenderScaledAsset(kWordmarkResourceId, drawW, drawH, 0))
				{
					frameGraphics.DrawImage(cached, static_cast<int>(box.X),
						static_cast<int>(box.Y + (box.Height - drawH) / 2.0f));
				}
			}
		}

		// Everything from here down is this screen's own content. During a
		// screen transition it goes into a scratch layer that is composited
		// at a rising alpha and a shrinking upward offset, so the new screen
		// fades up into place. Outside a transition `g` IS the frame, so the
		// steady state allocates nothing and costs exactly what it used to.
		SyncScreenTransition();
		const float screenAlpha = ScreenFadeAlpha();
		const bool transitioning = screenAlpha < 0.999f;
		std::unique_ptr<Gdiplus::Bitmap> layerBitmap;
		std::unique_ptr<Gdiplus::Graphics> layerGraphics;
		if (transitioning)
		{
			layerBitmap = std::make_unique<Gdiplus::Bitmap>(width, height, PixelFormat32bppPARGB);
			layerGraphics = std::make_unique<Gdiplus::Graphics>(layerBitmap.get());
			layerGraphics->Clear(Gdiplus::Color(0, 0, 0, 0));
			layerGraphics->SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
			layerGraphics->SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
		}
		Gdiplus::Graphics& g = transitioning ? *layerGraphics : frameGraphics;

		switch (g_app.screen)
		{
		case Screen::PadViewer:
			DrawLedRegionGlows(g);
			for (size_t index = 0; index < kPadCells.size(); ++index)
				DrawPad(g, index);
			// Tint the pad faces, then put the figures on top of it.
			DrawLedRegionTint(g);
			for (size_t index = 0; index < kPadCells.size(); ++index)
				DrawPadOccupant(g, index);
			break;
		case Screen::PadAction:
			DrawLedRegionGlows(g);
			for (size_t index = 0; index < kPadCells.size(); ++index)
				DrawPad(g, index);
			// Tint the pad faces, then put the figures on top of it.
			DrawLedRegionTint(g);
			for (size_t index = 0; index < kPadCells.size(); ++index)
				DrawPadOccupant(g, index);
			DrawActionButtons(g);
			break;
		case Screen::FranchiseList:
			DrawFranchiseGrid(g);
			DrawSortBadge(g, width);
			break;
		case Screen::RosterList:
		{
			// The current world's logo at top-center, above the roster grid.
			// The story roster spans several worlds, so it shows the "Starter"
			// sort badge instead; the Favorites roster shows the app wordmark
			// enlarged (both via DrawSortBadge).
			if (g_app.storyRosterActive)
			{
				DrawSortBadge(g, width);
			}
			else if (g_app.favoritesTileSelected)
			{
				DrawSortBadge(g, width);
			}
			else
			{
				const Gdiplus::RectF box((width - 360.0f) / 2.0f, 24.0f, 360.0f, 62.0f);
				Gdiplus::Bitmap* worldLogo = GetAssetBitmap(CurrentRosterWorldLogoResourceId());
				if (worldLogo)
				{
					const float scale = std::min(box.Width / worldLogo->GetWidth(), box.Height / worldLogo->GetHeight());
					const int drawW = static_cast<int>(worldLogo->GetWidth() * scale);
					const int drawH = static_cast<int>(worldLogo->GetHeight() * scale);
					if (Gdiplus::Bitmap* logo = RenderScaledAsset(
						CurrentRosterWorldLogoResourceId(), drawW, drawH, 0))
					{
						g.DrawImage(logo, box.X + (box.Width - drawW) / 2.0f,
							box.Y + (box.Height - drawH) / 2.0f);
					}
				}
			}

			// characters_tile.png: one large translucent panel behind the
			// whole portrait grid, spanning the roster grid's bounding box
			// plus a margin on each side. Drawn before the grid so the
			// portraits sit on top of it. The vertical margin (kRosterPanelPadY)
			// has to clear kFocusGlowMargin (20px) on both edges, or a focused
			// tile in the first/last row spills its halo past the panel border.
			// originShift pushes the whole panel down when this roster is
			// shorter than the full 3-row budget, so it doesn't stay pinned
			// right under the header (LEGO TOYPAD wordmark) with the leftover
			// space dumped entirely at the bottom.
			const int originShift = GetRosterOriginShift(g);
			const int gridRight = kRosterOriginX + static_cast<int>(kRosterCols) * kRosterPitchX;
			const int gridBottom = GetRosterContentBottom(g) + originShift;
			const int panelX = kRosterOriginX - 16;
			const int panelY = kRosterOriginY + originShift - kRosterPanelPadY;
			const Gdiplus::RectF panelRect(static_cast<float>(panelX), static_cast<float>(panelY),
				static_cast<float>(gridRight - panelX + 16), static_cast<float>(gridBottom - panelY + kRosterPanelPadY));
			if (Gdiplus::Bitmap* panel = RenderScaledAsset(
				kCharactersTileResourceId, static_cast<int>(panelRect.Width), static_cast<int>(panelRect.Height), 14))
			{
				g.DrawImage(panel, panelX, panelY);
			}
			DrawRosterGrid(g);
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
				Gdiplus::Bitmap* worldLogo = GetAssetBitmap(CurrentRosterWorldLogoResourceId());
				if (worldLogo)
				{
					const float scale = std::min(box.Width / worldLogo->GetWidth(), box.Height / worldLogo->GetHeight());
					const int drawW = static_cast<int>(worldLogo->GetWidth() * scale);
					const int drawH = static_cast<int>(worldLogo->GetHeight() * scale);
					if (Gdiplus::Bitmap* logo = RenderScaledAsset(
						CurrentRosterWorldLogoResourceId(), drawW, drawH, 0))
					{
						g.DrawImage(logo, box.X + (box.Width - drawW) / 2.0f,
							box.Y + (box.Height - drawH) / 2.0f);
					}
				}
			}
			DrawPlusPickerMenu(g);
			break;
		}
		case Screen::Settings:
		{
			// Same furniture as the franchise and roster grids: one
			// translucent panel behind the whole list, a viewport that
			// follows the focus, and the shared right-edge scroll bar. The
			// list itself is the categorised table from BuildSettingsEntries,
			// so painting and activation can never disagree about which row
			// is which.
			ClampSettingsSelection();
			const std::vector<SettingsEntry> rows = BuildSettingsEntries();
			ScrollSettingsIntoView();

			constexpr int kSettingsPanelX = 30;
			constexpr float kSettingsIconH = 28.0f;
			const int panelW = width - kSettingsPanelX * 2;
			const size_t visibleRows =
				std::min(kSettingsVisibleRows, rows.size() - static_cast<size_t>(g_app.settingsTopRow));
			// 18px of breathing room above the first row and below the last
			// one (symmetric), rather than the cramped 12px that used to let
			// the last row's text sit almost on top of the panel's bottom edge.
			constexpr int kSettingsPanelPad = 18;
			const int panelH = static_cast<int>(visibleRows) * kSettingsPitch + kSettingsPanelPad * 2;
			// settings_tile.png if it was bundled; the roster panel art
			// otherwise, so a build without the new asset still has a panel.
			const int settingsPanelResource =
				kSettingsTileResourceId != 0 ? kSettingsTileResourceId : kCharactersTileResourceId;
			if (Gdiplus::Bitmap* panel = RenderScaledAsset(settingsPanelResource, panelW, panelH, 14))
			{
				g.DrawImage(panel, kSettingsPanelX, kSettingsTop - kSettingsPanelPad);
			}

			for (size_t visible = 0; visible < visibleRows; ++visible)
			{
				const size_t index = static_cast<size_t>(g_app.settingsTopRow) + visible;
				const SettingsEntry& row = rows[index];
				const int y = kSettingsTop + static_cast<int>(visible) * kSettingsPitch;

				if (IsSettingsHeading(row))
				{
					// A category heading: centred, with a rule running out to
					// both edges, so the groups read as dividers rather than
					// as more settings.
					DrawTextLineCentered(g, row.label, kSettingsPanelX, y, panelW,
						RGB(255, 204, 51), kSettingsPitch);
					const float labelW = MeasureTextWidth(g, row.label, 22.0f);
					const float centre = kSettingsPanelX + panelW / 2.0f;
					const float ruleY = y + kSettingsPitch / 2.0f - 1.0f;
					const float innerGap = labelW / 2.0f + 14.0f;
					const float leftEdge = static_cast<float>(kSettingsPanelX + 22);
					const float rightEdge = static_cast<float>(kSettingsPanelX + panelW - 22);
					Gdiplus::SolidBrush rule(Gdiplus::Color(70, 255, 204, 51));
					if (centre - innerGap > leftEdge)
						g.FillRectangle(&rule, leftEdge, ruleY, centre - innerGap - leftEdge, 2.0f);
					if (rightEdge > centre + innerGap)
						g.FillRectangle(&rule, centre + innerGap, ruleY, rightEdge - centre - innerGap, 2.0f);
					continue;
				}

				const bool selected = index == g_app.settingsIndex;
				if (selected)
				{
					Gdiplus::GraphicsPath highlight;
					AddRoundedRectPath(highlight,
						Gdiplus::RectF(static_cast<float>(kSettingsPanelX + 8), static_cast<float>(y),
							static_cast<float>(panelW - 16), static_cast<float>(kSettingsPitch - 2)),
						8.0f);
					Gdiplus::LinearGradientBrush fill(
						Gdiplus::RectF(static_cast<float>(kSettingsPanelX + 8), static_cast<float>(y),
							static_cast<float>(panelW - 16), static_cast<float>(kSettingsPitch - 2)),
						Gdiplus::Color(255, 58, 110, 176), Gdiplus::Color(255, 32, 74, 130),
						Gdiplus::LinearGradientModeVertical);
					g.FillPath(&fill, &highlight);
				}

				// Label first, then the value in its own colour immediately
				// after it. Splitting the two is what lets an On read green
				// and an Off read red without tinting the whole row.
				const bool hasTrailer = !row.value.empty() || row.icons != 0;
				const std::wstring label = hasTrailer ? row.label + L": " : row.label;
				const int labelX = kSettingsPanelX + 24;
				DrawTextLine(g, label, labelX, y, panelW - 60,
					selected ? RGB(255, 255, 255) : RGB(228, 232, 238), kSettingsPitch);

				const float trailerX = labelX + MeasureTextWidth(g, label, 22.0f);
				if (!row.value.empty())
				{
					COLORREF valueColor = RGB(255, 214, 140); // a choice, not a switch
					if (row.tone == SettingValueTone::On)
						valueColor = RGB(126, 226, 142);
					else if (row.tone == SettingValueTone::Off)
						valueColor = RGB(255, 108, 108);
					DrawTextLine(g, row.value, static_cast<int>(trailerX), y,
						kSettingsPanelX + panelW - 24 - static_cast<int>(trailerX),
						valueColor, kSettingsPitch);
				}
				else if (row.icons != 0)
				{
					DrawPadButtonMask(g, row.icons, trailerX,
						y + (kSettingsPitch - kSettingsIconH) / 2.0f, kSettingsIconH, true);
				}
			}

			DrawScrollBar(g, kSettingsTop, kSettingsTop + static_cast<int>(visibleRows) * kSettingsPitch,
				rows.size(), kSettingsVisibleRows, g_app.settingsTopRow);

			// While a button is being captured the status line carries the
			// whole conversation - what to press, and why a press was
			// refused - so it is drawn under the list. Outside capture the
			// rows already show every setting's value, and a leftover
			// message there would just be noise.
			if ((g_app.capturingShortcut || g_app.capturingBindingIndex >= 0) && !g_app.status.empty())
			{
				const int hintY = kSettingsTop + static_cast<int>(visibleRows) * kSettingsPitch + 16;
				DrawTextLine(g, g_app.status, 24, hintY, width - 48, RGB(255, 204, 51), 26);
			}
			break;
		}
		}

		// The focused occupied pad's occupant name, drawn after the pads and
		// action bar so it stays readable on all pad screens.
		if (g_app.screen == Screen::PadViewer || g_app.screen == Screen::PadAction)
			DrawOccupantLabel(g, g_app.slotIndex);

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

		// Roster: "Y  Favorite" hint, same bottom-right corner as the
		// Settings hint above, with "X  Organize" stacked to its left while
		// browsing the Favorites roster - the only roster whose order can
		// be changed (a real franchise's order comes from the game data).
		if (g_app.screen == Screen::RosterList)
		{
			constexpr float kHintBottomInset = 18.0f;
			constexpr float kHintButtonH = 40.0f;
			constexpr float kHintStackGap = 20.0f;
			const float hintCenterY = height - kHintBottomInset - kHintButtonH / 2.0f;
			float rightEdge = width - kTopMargin;
			rightEdge -= DrawButtonHint(g, g_app.buttonFavorite, L"Favorite", rightEdge, hintCenterY, kHintButtonH);
			if (g_app.favoritesTileSelected)
			{
				rightEdge -= kHintStackGap;
				DrawButtonHint(g, g_app.buttonReorganizeRoster, L"Organize", rightEdge, hintCenterY, kHintButtonH);
			}
			// Story/Favorites browse rosters can also change the sort; the sort
			// hint sits on the left so it stays clear of Favorite/Organize.
			if (IsFranchiseSortBrowseActive())
				DrawButtonHintLeft(g, XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER,
					L"Sort", kTopMargin, hintCenterY, kHintButtonH);
		}
		// Franchise grid: "X  Organize" hint on the right (only in the custom
		// order view; the synthetic Favorites tile has nothing to reorganize
		// from), with the "LB/RB  Sort" hint on the left.
		else if (g_app.screen == Screen::FranchiseList)
		{
			constexpr float kHintBottomInset = 18.0f;
			constexpr float kHintButtonH = 40.0f;
			const float hintCenterY = height - kHintBottomInset - kHintButtonH / 2.0f;
			DrawButtonHintLeft(g, XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER,
				L"Sort", kTopMargin, hintCenterY, kHintButtonH);
			if (g_app.franchiseSort == AppState::FranchiseSort::User && !g_app.favoritesTileSelected)
			{
				const float rightEdge = width - kTopMargin;
				DrawButtonHint(g, g_app.buttonReorganizeFranchise, L"Organize",
					rightEdge, hintCenterY, kHintButtonH);
			}
		}

		DrawStatusToast(g, width, height);

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

		// Composite the transition layer, if there is one. The few pixels of
		// upward travel are applied as a destination offset rather than a
		// transform, so nothing inside the layer had to know it was being
		// animated.
		if (transitioning)
		{
			layerGraphics.reset(); // flush the layer's drawing before reading it
			const int rise = static_cast<int>(kScreenFadeRisePx * (1.0f - screenAlpha) + 0.5f);
			DrawImageWithAlpha(frameGraphics, layerBitmap.get(), 0, rise, screenAlpha);
		}

		// Per-pixel alpha from the DIB, plus the panel-wide translucency that
		// SetLayeredWindowAttributes used to apply, in a single present. The
		// constant alpha is the user's opacity setting, scaled by whatever
		// show/dismiss fade is running.
		POINT source{0, 0};
		SIZE size{width, height};
		BLENDFUNCTION blend{AC_SRC_OVER, 0, CurrentOverlayAlpha(), AC_SRC_ALPHA};
		UpdateLayeredWindow(window, nullptr, nullptr, &size, s_bufferDC, &source,
			0, &blend, ULW_ALPHA);

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
		static int navHoldMoves = 0;

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

		// The sneak-peek HUD is a pure function of what is held down right
		// now, so it is settled before any of the early returns below - in
		// particular before the capture branches, which would otherwise
		// leave it stuck on screen for as long as a capture lasts.
		UpdatePeekHold(anyConnected, combinedButtons);

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

		// A dismissed overlay keeps painting (and keeps input ownership) for
		// the length of its fade-out, but must not act on anything: the
		// picker is on its way out, and a stray press during those ~110ms
		// would land on a screen the user has already left.
		if (g_overlayHiding)
			return;

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

		// True for the rest of this tick once Quick load has just opened the
		// browse screen. Quick load uses the same shoulder button that cycles
		// the sort on the browse screen (RB by default), so without this guard
		// a single press would both open the browse view AND jump the sort
		// forward in the same tick. Quick load should only open the browse
		// view; sorting stays a separate press while already browsing.
		bool quickLoadJustOpened = false;

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
				OpenBrowseScreen();
				quickLoadJustOpened = true;
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

		// Y / Triangle / Y by default: favorite the focused character or
		// vehicle while browsing a roster (including the Favorites roster
		// itself, so it also doubles as the way to unfavorite something), or
		// the highlighted build while inside the build picker - the only
		// place a specific variant of a multi-build vehicle can be favorited
		// (see ToggleFavoriteForFocusedPlusBuild).
		if (combinedPressed & g_app.buttonFavorite)
		{
			if (g_app.screen == Screen::RosterList && !g_app.reorganizingRoster)
			{
				ToggleFavoriteForFocusedRoster();
				changed = true;
			}
			else if (g_app.screen == Screen::PlusPicker)
			{
				ToggleFavoriteForFocusedPlusBuild();
				changed = true;
			}
		}

		// X / Square / X by default: pick the focused tile up so the next
		// navigation + Confirm drops it in a new spot. Only meaningful for
		// the Favorites roster (a real franchise's roster order comes from
		// the game data) and for real franchise tiles in the world grid.
		if ((combinedPressed & g_app.buttonReorganizeRoster) && g_app.screen == Screen::RosterList &&
			g_app.favoritesTileSelected && !g_app.reorganizingRoster)
		{
			BeginRosterReorganize();
			changed = true;
		}
		if ((combinedPressed & g_app.buttonReorganizeFranchise) && g_app.screen == Screen::FranchiseList &&
			!g_app.favoritesTileSelected && !g_app.reorganizingFranchise)
		{
			BeginFranchiseReorganize();
			changed = true;
		}

		// Shoulder buttons on the browse screen cycle the sort mode
		// (RB/LB on Xbox, R1/L1 on PS, R/L on Switch). Quick load/clear only
		// run on the pad viewer, so both shoulders are free here. Works on the
		// franchise grid AND the Story/Favorites browse rosters. Skipped in
		// the same tick a Quick load just opened the browse screen, so that
		// press only navigates and never also advances the sort.
		if (IsFranchiseSortBrowseActive() && !g_app.reorganizingFranchise && !quickLoadJustOpened)
		{
			if (combinedPressed & XINPUT_GAMEPAD_RIGHT_SHOULDER)
			{
				CycleFranchiseSort(+1);
				changed = true;
			}
			else if (combinedPressed & XINPUT_GAMEPAD_LEFT_SHOULDER)
			{
				CycleFranchiseSort(-1);
				changed = true;
			}
		}

		// Screens navigated in two dimensions.
		const bool gridScreen = g_app.screen == Screen::PadViewer ||
			g_app.screen == Screen::FranchiseList || g_app.screen == Screen::RosterList ||
			g_app.screen == Screen::Settings;
		const DWORD now = GetTickCount();

		// D-pad edge presses move immediately instead of waiting out the
		// hold-repeat throttle, so quick taps are never swallowed. Holding a
		// direction (stick or D-pad held down) repeats at a fixed cadence,
		// but the first repeat waits out a longer initial delay - a stick
		// flick stays past the deadzone longer than a D-pad click's travel
		// + spring-back, so without this a single flick could still land
		// inside the fast-repeat window and move two tiles instead of one.
		constexpr int kNavRepeatMs = 150;
		constexpr int kNavInitialRepeatMs = 350;
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
		else if (anyDirection && now - lastNavigation >=
			(navHoldMoves <= 1 ? kNavInitialRepeatMs : kNavRepeatMs))
		{
			if (gridScreen)
			{
				int dx = stickLeft ? -1 : (stickRight ? 1 : 0);
				const int dy = stickUp ? -1 : (stickDown ? 1 : 0);
				// On the Settings screen the horizontal axis edits values, and
				// values must not auto-repeat: holding Right on "Web remote"
				// would stop and restart the HTTP server six times a second,
				// and on "Toypad LEDs" it would thrash the poll thread. A
				// value changes once per deliberate press; only the vertical
				// axis (moving between rows) repeats while held.
				if (g_app.screen == Screen::Settings)
					dx = 0;
				if (dx != 0 || dy != 0)
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
			navHoldMoves++;
			changed = true;
		}
		if (!anyDirection)
		{
			lastNavigation = 0;
			navHoldMoves = 0;
		}

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

	// Case-insensitive scan for common virtual / VPN / tunnel-adapter markers in
	// an adapter's FriendlyName / Description. IfType alone won't catch every
	// kind (Hyper-V and Docker report as Ethernet), so this is the safety net
	// that keeps the "real LAN" picker from latching onto a reachable-looking
	// private IP that another device on the LAN can never actually reach.
	bool HasVirtualAdapterMarker(const std::wstring& text)
	{
		static constexpr const wchar_t* kMarkers[] = {
			L"vpn", L"virtual", L"hyper-v", L"hyperv", L"vmware", L"virtualbox",
			L"tailscale", L"zerotier", L"wsl", L"loopback", L"loop", L"tap",
			L"tun", L"docker", L"vethernet", L"hamachi", L"wireguard", L"openvpn",
			L"nordvpn", L"surfshark", L"proton", L"tunnel", L"ppp", L"bluetooth",
			L"isatap", L"teredo", L"6to4", L"wan miniport", L"mirrored",
		};
		std::wstring lower;
		lower.reserve(text.size());
		for (wchar_t c : text)
			lower.push_back(static_cast<wchar_t>(towlower(c)));
		for (const wchar_t* marker : kMarkers)
		{
			if (lower.find(marker) != std::wstring::npos)
				return true;
		}
		return false;
	}

	// Picks the IPv4 address a phone/tablet on the same LAN should use to reach
	// the web remote. Instead of returning the first non-loopback / non-APIPA
	// adapter Windows happens to enumerate (which is often a VPN / WSL / Docker
	// / Hyper-V virtual NIC), this ranks every candidate and prefers a real
	// physical Ethernet or Wi-Fi adapter with a default gateway and a private
	// RFC1918 address, heavily penalising known virtual-adapter names.
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

		std::wstring best;
		int bestScore = -1000000;
		for (PIP_ADAPTER_ADDRESSES adapter = adapters; adapter; adapter = adapter->Next)
		{
			if (adapter->OperStatus != IfOperStatusUp)
				continue;
			if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
				continue;

			// Adapter-level properties, applied equally to each of its addresses.
			int baseScore = 0;
			switch (adapter->IfType)
			{
			case IF_TYPE_ETHERNET_CSMACD: baseScore += 100; break; // real wired LAN
			case IF_TYPE_IEEE80211:       baseScore += 90;  break; // real Wi-Fi
			default:                      baseScore += 10;  break; // PPP / tunnel / other
			}
			if (adapter->FirstGatewayAddress != nullptr)
				baseScore += 20; // has a default route: it's the "real" connectivity
			if ((adapter->FriendlyName && HasVirtualAdapterMarker(adapter->FriendlyName)) ||
				(adapter->Description && HasVirtualAdapterMarker(adapter->Description)))
				baseScore -= 60; // virtual NIC: reachable-looking but not on the real LAN

			for (PIP_ADAPTER_UNICAST_ADDRESS unicast = adapter->FirstUnicastAddress;
				unicast; unicast = unicast->Next)
			{
				const sockaddr_in* address = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
				if (!address || address->sin_family != AF_INET)
					continue;
				const ULONG addr = ntohl(address->sin_addr.s_addr);
				if ((addr >> 24) == 127) continue;      // loopback
				if ((addr >> 16) == 0xA9FE) continue;   // 169.254.x.x APIPA / link-local
				if (addr == 0 || addr == 0xFFFFFFFF) continue; // unspecified / broadcast

				// Prefer the private ranges a LAN device can actually reach.
				const uint8_t b1 = (addr >> 24) & 0xFF;
				const uint8_t b2 = (addr >> 16) & 0xFF;
				const bool rfc1918 = (b1 == 10) || (b1 == 192 && b2 == 168) ||
					(b1 == 172 && b2 >= 16 && b2 <= 31);
				const int score = baseScore + (rfc1918 ? 15 : 0);

				char text[INET_ADDRSTRLEN] = {};
				if (!InetNtopA(AF_INET, &address->sin_addr, text, sizeof text))
					continue;
				if (score > bestScore)
				{
					bestScore = score;
					best = L"http://" + WideFromUtf8(text) + L":" + std::to_wstring(g_app.webPort) + L"/";
				}
			}
		}
		return best;
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

	// franchise is the entry's real owning franchise - always the containing
	// world when called from the normal per-franchise catalog loop, but
	// distinct from whatever world is being browsed when called while
	// building the aggregated Favorites response. The web client needs it on
	// every entry to know what to send back to /api/favorite when toggling
	// favorite status from inside the Favorites roster itself.
	std::string EntryJson(const RosterEntry& entry, const std::wstring& franchise)
	{
		const unsigned int color = entry.ringColor;
		char hex[16];
		::snprintf(hex, sizeof hex, "#%02X%02X%02X", color & 0xFF, (color >> 8) & 0xFF, (color >> 16) & 0xFF);
		std::string out = "{";
		out += "\"name\":" + WJson(entry.name) + ",";
		out += "\"franchise\":" + WJson(franchise) + ",";
		out += "\"build\":" + std::to_string(entry.buildNumber) + ",";
		out += "\"color\":\"" + std::string(hex) + "\",";
		out += "\"portrait\":\"" + IdUrl(entry.portraitResourceId) + "\",";
		out += "\"bin\":" + std::to_string(entry.binResourceId);
		out += "}";
		return out;
	}

	std::string VehicleGroupJson(const VehicleGroup& group, const std::wstring& franchise)
	{
		std::string out = "{\"base\":" + WJson(group.baseName) + ",\"franchise\":" + WJson(franchise) + ",\"builds\":[";
		for (size_t b = 0; b < group.builds.size(); ++b)
		{
			if (b != 0)
				out += ",";
			out += EntryJson(group.builds[b], franchise);
		}
		out += "]}";
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
		// Logo for the client-built Favorites tile at the front of the Pick-
		// a-World grid; same custom_bin.png icon the desktop overlay uses.
		out += "\"favoritesIcon\":\"" + IdUrl(kCustomBinIconResourceId) + "\",";
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
				out += EntryJson(franchise.characters[i], franchise.name);
			}
			out += "],\"vehicles\":[";
			for (size_t v = 0; v < franchise.vehicles.size(); ++v)
			{
				if (v != 0)
					out += ",";
				out += VehicleGroupJson(franchise.vehicles[v], franchise.name);
			}
			out += "]}";
		}
		out += "]}";
		return out;
	}

	// A synthetic "world" shaped exactly like a franchise entry in the
	// catalog (same {name, logo, characters, vehicles} shape), built from
	// the current favorites instead of one static franchise, so the web
	// client can render it through the exact same roster-grid code path.
	// Not cached: favorites change at runtime, unlike the rest of the
	// catalog. Stale entries are silently skipped, same as OpenFavoritesRoster
	// on the desktop side.
	std::string BuildFavoritesJson()
	{
		std::string out = "{\"name\":" + WJson(L"Favorites") + ",";
		out += "\"logo\":\"" + IdUrl(kCustomBinIconResourceId) + "\",";
		out += "\"characters\":[";
		bool firstChar = true;
		for (const auto& fav : g_app.favorites)
		{
			if (fav.isVehicle)
				continue;
			const RosterEntry* character = FindCharacterEntry(fav.franchise.c_str(), fav.name.c_str());
			if (!character)
				continue;
			if (!firstChar)
				out += ",";
			firstChar = false;
			out += EntryJson(*character, fav.franchise);
		}
		out += "],\"vehicles\":[";
		bool firstVeh = true;
		for (const auto& fav : g_app.favorites)
		{
			if (!fav.isVehicle)
				continue;
			const VehicleGroup* group = FindVehicleGroupEntry(fav.franchise.c_str(), fav.name.c_str());
			if (!group || group->builds.empty())
				continue;
			if (!firstVeh)
				out += ",";
			firstVeh = false;
			out += VehicleGroupJson(*group, fav.franchise);
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
			out += "\"bin\":" + std::to_string(slot.occupied ? slot.binResourceId : 0) + ",";
			out += "\"portrait\":\"" + IdUrl(slot.portraitResourceId) + "\"}";
		}
		out += "],\"status\":" + WJson(g_app.status) + "}";
		job.result = out;
		job.ok = true;
	}

	// The live LED glow for the three pad regions (left/center/right), read
	// straight off g_app.ledRegions - AdvanceLedAnimation keeps curR/G/B and
	// intensity current every tick regardless of whether the overlay window
	// is visible, so this is never stale. Polled far more often than
	// /api/state (see Web/app.js): Flash and Fade need to read as smooth
	// motion, not a slideshow.
	void BuildLedsJson(WebJob& job)
	{
		static const char* kLedModeNames[] = { "off", "solid", "flash", "fade" };
		std::string out = "{\"enabled\":";
		out += g_app.ledMirrorEnabled ? "true" : "false";
		out += ",\"regions\":[";
		for (size_t region = 0; region < g_app.ledRegions.size(); ++region)
		{
			if (region != 0)
				out += ",";
			const LedRegion& led = g_app.ledRegions[region];
			char hex[16];
			::snprintf(hex, sizeof hex, "#%02X%02X%02X", led.curR, led.curG, led.curB);
			char intensity[16];
			::snprintf(intensity, sizeof intensity, "%.3f",
				led.mode == LedMode::Off ? 0.0f : led.intensity);
			out += "{\"region\":" + std::to_string(region) + ",";
			out += "\"mode\":\"" + std::string(kLedModeNames[static_cast<size_t>(led.mode)]) + "\",";
			out += "\"color\":\"" + std::string(hex) + "\",";
			out += "\"intensity\":" + std::string(intensity) + "}";
		}
		out += "]}";
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

	// Resolves a favorite target straight from a tag's bin resource id - what
	// the web remote's pad state already carries for whatever is loaded on a
	// pad, so the "Favorite" button next to Clear can favorite "whatever is
	// on this pad" without the client needing to separately know its
	// franchise/name. A vehicle resolves to the exact build loaded
	// (buildNumberOut), same as everywhere else a specific variant is keyed.
	bool ResolveFavoriteTargetFromBin(int binResourceId, std::wstring& franchiseOut, std::wstring& nameOut,
		bool& isVehicleOut, int& buildNumberOut)
	{
		for (size_t franchiseIndex = 0; franchiseIndex < kFranchiseCount; ++franchiseIndex)
		{
			for (const auto& character : kFranchises[franchiseIndex].characters)
			{
				if (character.binResourceId == binResourceId)
				{
					franchiseOut = kFranchises[franchiseIndex].name;
					nameOut = character.name;
					isVehicleOut = false;
					buildNumberOut = 0;
					return true;
				}
			}
			for (const auto& vehicle : kFranchises[franchiseIndex].vehicles)
			{
				for (const auto& build : vehicle.builds)
				{
					if (build.binResourceId == binResourceId)
					{
						franchiseOut = kFranchises[franchiseIndex].name;
						nameOut = vehicle.baseName;
						isVehicleOut = true;
						buildNumberOut = build.buildNumber;
						return true;
					}
				}
			}
		}
		return false;
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
		case WebJob::Op::Leds:
			BuildLedsJson(job);
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
		case WebJob::Op::FavoritesGet:
			// Not cached like Catalog: favorites change at runtime.
			job.result = BuildFavoritesJson();
			job.ok = true;
			break;
		case WebJob::Op::FavoriteToggle:
		{
			// job.b is the bin resource id of whatever is loaded on the pad
			// the web remote's Favorite button was pressed for.
			std::wstring franchise;
			std::wstring name;
			bool isVehicle = false;
			int buildNumber = 0;
			bool favorited = false;
			if (!ResolveFavoriteTargetFromBin(job.b, franchise, name, isVehicle, buildNumber) ||
				!ToggleFavorite(franchise, name, isVehicle, buildNumber, favorited))
			{
				job.result = ErrJson(L"Nothing on this pad to favorite.");
				break;
			}
			job.ok = true;
			job.result = std::string("{\"ok\":true,\"favorited\":") + (favorited ? "true" : "false") + "}";
			break;
		}
		}
		SetEvent(job.done);
	}

	// Posts a fully-populated job to the UI thread and waits (bounded) for
	// its JSON result. Ownership of the job stays in this thread.
	std::string DispatchWebJobObj(HWND window, WebJob& job)
	{
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

	// Sends a simple int-only operation to the UI thread and waits for its
	// JSON result. For operations that also need string payloads (e.g.
	// FavoriteToggle), populate a WebJob directly and call DispatchWebJobObj.
	std::string DispatchWebJob(HWND window, WebJob::Op operation, int a, int b)
	{
		WebJob job;
		job.op = operation;
		job.a = a;
		job.b = b;
		return DispatchWebJobObj(window, job);
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
			else if (request.path == "/api/leds")
			{
				const std::string body = DispatchWebJob(window, WebJob::Op::Leds, 0, 0);
				SendHttpResponse(socket, 200, "OK", body, "application/json; charset=utf-8", false);
			}
			else if (request.path == "/api/favorites")
			{
				const std::string body = DispatchWebJob(window, WebJob::Op::FavoritesGet, 0, 0);
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
			else if (request.path == "/api/favorite")
			{
				const int bin = JsonInt(request.body, "bin");
				body = DispatchWebJob(window, WebJob::Op::FavoriteToggle, 0, bin);
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

	// Paces the controller poll. It does no work of its own beyond posting a
	// message - everything still happens on the UI thread in the kTickMessage
	// handler - so this is purely about which queue tier the poll arrives on.
	void TickThread(HWND window)
	{
		while (g_tickRunning.load(std::memory_order_relaxed))
		{
			if (!g_tickPending.exchange(true, std::memory_order_acq_rel))
			{
				if (!PostMessageW(window, kTickMessage, 0, 0))
					g_tickPending.store(false, std::memory_order_release);
			}
			Sleep(g_tickIntervalMs.load(std::memory_order_relaxed));
		}
	}

	void StartTickThread(HWND window)
	{
		bool expected = false;
		if (!g_tickRunning.compare_exchange_strong(expected, true))
			return;
		g_tickThread = std::thread(TickThread, window);
	}

	void StopTickThread()
	{
		if (!g_tickRunning.exchange(false))
			return;
		if (g_tickThread.joinable())
			g_tickThread.join();
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
			// Bit 30 of lParam is the previous key state: set means this is a
			// hardware auto-repeat rather than a fresh press. Values are only
			// edited on a fresh press, for the same reason the stick repeat
			// above skips them.
			case VK_LEFT:
				if (g_app.screen != Screen::Settings || (lParam & (1 << 30)) == 0)
					NavigateGrid(-1, 0);
				break;
			case VK_RIGHT:
				if (g_app.screen != Screen::Settings || (lParam & (1 << 30)) == 0)
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
					OpenBrowseScreen();
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
			case 'G':
				if (g_app.screen == Screen::PadViewer)
					ToggleLedDemo();
				break;
			}
			InvalidateRect(window, nullptr, FALSE);
			return 0;
		case WM_CHAR:
			return DefWindowProcW(window, message, wParam, lParam);
		case WM_MOUSEMOVE:
		{
			if (g_app.draggingWindow)
			{
				// Screen coordinates on both ends: the client-relative
				// lParam would chase its own tail once the window starts
				// moving under the cursor.
				POINT cursor{};
				GetCursorPos(&cursor);
				SetWindowPos(window, HWND_TOPMOST,
					g_app.dragStartWindow.x + (cursor.x - g_app.dragStartCursor.x),
					g_app.dragStartWindow.y + (cursor.y - g_app.dragStartCursor.y),
					0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
				return 0;
			}
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
		case WM_CAPTURECHANGED:
			// Something took the mouse away mid-drag (Alt-Tab, a shell
			// gesture). Keep wherever it ended up rather than snapping back.
			if (g_app.draggingWindow)
			{
				g_app.draggingWindow = false;
				RECT frame{};
				GetWindowRect(window, &frame);
				g_app.savedWindowX = frame.left;
				g_app.savedWindowY = frame.top;
				g_app.hasSavedWindowPos = true;
				SaveWindowSettingsToIni();
			}
			return 0;
		case WM_SETCURSOR:
			// A move cursor over the draggable background is the only hint
			// that the window can be picked up - there is no title bar to
			// give it away. Controls keep the normal arrow.
			if (LOWORD(lParam) == HTCLIENT && g_app.windowDraggable)
			{
				POINT cursor{};
				GetCursorPos(&cursor);
				ScreenToClient(window, &cursor);
				if (g_app.draggingWindow || ActionButtonIndexAt(cursor) < 0)
				{
					SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
					return TRUE;
				}
			}
			return DefWindowProcW(window, message, wParam, lParam);
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
			else if (g_app.windowDraggable)
			{
				// The overlay is borderless, so anything that isn't a
				// clickable control doubles as the title bar. Dragged by
				// hand rather than through WM_NCLBUTTONDOWN/HTCAPTION so the
				// controller poll and LED mirror keep running instead of
				// stalling inside the system's modal move loop.
				RECT frame{};
				GetWindowRect(window, &frame);
				GetCursorPos(&g_app.dragStartCursor);
				g_app.dragStartWindow = POINT{frame.left, frame.top};
				g_app.draggingWindow = true;
				SetCapture(window);
			}
			return 0;
		}
		case WM_LBUTTONUP:
		{
			if (g_app.draggingWindow)
			{
				g_app.draggingWindow = false;
				if (GetCapture() == window)
					ReleaseCapture();
				RECT frame{};
				GetWindowRect(window, &frame);
				g_app.savedWindowX = frame.left;
				g_app.savedWindowY = frame.top;
				g_app.hasSavedWindowPos = true;
				SaveWindowSettingsToIni();
				return 0;
			}
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
				PlaySelectSound();
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
		case kLedMessage:
			if (wParam)
			{
				LedPollFrame* frame = reinterpret_cast<LedPollFrame*>(wParam);
				ApplyLedFrame(*frame);
				delete frame;
			}
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
					// Re-detect on every copy rather than trusting the address
					// cached at startup, so a VPN/Docker interface that wasn't
					// up then (or that took over since) doesn't hand the user a
					// dead link.
					const std::wstring fresh = GetLanAddress();
					if (!fresh.empty())
						g_app.webUrl = fresh;
					else if (g_app.webUrl.empty())
						g_app.webUrl = L"http://<this-pc-lan-ip>:" + std::to_wstring(g_app.webPort) + L"/";
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
		case kTickMessage:
		{
			// Clear the in-flight flag first: the pacing thread may queue the
			// next tick while this one is still running, and that is fine -
			// what must not happen is a backlog building up behind a slow
			// frame.
			g_tickPending.store(false, std::memory_order_release);

			PollController(window);
			SyncScreenTransition();
			SyncSelectionTap();
			SyncStatusToast();

			const bool ledAnimating = AdvanceLedAnimation();
			if (g_app.overlayVisible)
			{
				// Anything still moving asks for another frame, but only when
				// the frame would actually look different. The breathing halo
				// is stepped (see kFocusPulseSteps), so an otherwise idle
				// screen repaints ~19 times a second instead of continuously,
				// and stops entirely at the turning points of the breath.
				// Genuinely continuous animations - LED flashes, screen
				// transitions, the show/dismiss fade - get a plain frame-rate
				// cap instead.
				static DWORD lastAnimationFrame = 0;
				static int lastPulseStep = -1;
				const bool pulsing = ScreenHasPulsingFocus();
				const int pulseStep = pulsing ? FocusPulseStep() : -1;
				const bool continuous = ledAnimating || ScreenTransitionActive() ||
					WindowFadeActive() || SelectionTapActive() || StatusToastActive();
				if (continuous || (pulsing && pulseStep != lastPulseStep))
				{
					constexpr DWORD kMinAnimationFrameMs = 16; // never above ~60fps
					const DWORD now = GetTickCount();
					if (now - lastAnimationFrame >= kMinAnimationFrameMs)
					{
						lastAnimationFrame = now;
						lastPulseStep = pulseStep;
						InvalidateRect(window, nullptr, FALSE);
					}
				}
			}

			// The sneak-peek HUD paints on its own window and only when the
			// frame would differ: while it fades, and otherwise whenever the
			// pad or LED state it mirrors actually changed. An idle HUD over
			// a running game therefore costs nothing per tick.
			if (g_peekShown)
			{
				static DWORD lastPeekFrame = 0;
				static uint64_t lastPeekSignature = 0;
				const uint64_t signature = PeekContentSignature();
				if (PeekFadeActive() || signature != lastPeekSignature)
				{
					constexpr DWORD kMinPeekFrameMs = 16; // never above ~60fps
					const DWORD now = GetTickCount();
					if (now - lastPeekFrame >= kMinPeekFrameMs)
					{
						lastPeekFrame = now;
						lastPeekSignature = signature;
						PaintPeekWindow(g_peekWindow);
					}
				}
				FinishPeekHide();
			}

			// The dismiss fade lands here rather than in HideOverlay, so
			// the last frame of it is actually presented before the
			// window disappears.
			if (g_overlayHiding && !WindowFadeActive())
				FinishOverlayHide(window);
			return 0;
		}
		case WM_ACTIVATE:
			UpdateInputOwnership(window);
			return 0;
		case WM_SIZE:
			ApplyWindowCornerRadius(window);
			UpdateInputOwnership(window);
			return 0;
		case WM_DESTROY:
			StopTickThread();
			if (g_peekWindow)
			{
				HidePeekWindow(true);
				DestroyWindow(g_peekWindow);
				g_peekWindow = nullptr;
			}
			StopUiSounds();
			UnregisterHotKey(window, kToggleHotkeyId);
			RemoveTrayIcon(window);
			StopWebServer();
			StopLedPoll();
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
	// The pad-skin list has to exist before the settings load, because the
	// saved skin is stored by name and resolved against this list.
	BuildPadSkinList();
	g_app.port = ReadPort();
	LoadShortcutFromIni();
	LoadInputSettingsFromIni();
	LoadWindowSettingsFromIni();
	LoadWebSettingsFromIni();
	LoadFavoritesFromIni();
	LoadFranchiseOrderFromIni();
	RebuildFranchiseDisplay();

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
		ReleasePadDiskBitmaps();
		UnloadUIFont();
		Gdiplus::GdiplusShutdown(g_gdiplusToken);
		WSACleanup();
		return 1;
	}
	g_mainWindow = window;

	// The sneak-peek HUD's own window. WS_EX_NOACTIVATE keeps it out of the
	// activation chain (the game never loses focus to it), WS_EX_TRANSPARENT
	// makes it click-through, and WS_EX_LAYERED is what UpdateLayeredWindow
	// needs. It is created hidden and sized to a monitor on first use.
	const wchar_t* peekClassName = L"LegoToypadPeekWindow";
	WNDCLASSW peekClass{};
	peekClass.hInstance = instance;
	peekClass.lpfnWndProc = PeekWindowProcedure;
	peekClass.lpszClassName = peekClassName;
	RegisterClassW(&peekClass);
	g_peekWindow = CreateWindowExW(
		WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
		peekClassName, L"LEGO Dimensions Toypad Sneak Peek", WS_POPUP,
		0, 0, 16, 16, nullptr, nullptr, instance, nullptr);

	ApplyOverlayTransparency(window);
	ApplyWindowCornerRadius(window);
	AddTrayIcon(window);
	RegisterToggleHotkeyIfNeeded(window);
	StartTickThread(window);

	// Start the LED mirror poll: it reports the running game's pad glows so the
	// overlay lights up like a physical toypad. It only talks when Cemu is up,
	// and not at all when the Settings screen's "Toypad LEDs" row is Off.
	if (g_app.ledMirrorEnabled)
		StartLedPoll(window);

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
	ReleasePadDiskBitmaps();
	UnloadUIFont();
	SDL_Quit();
	Gdiplus::GdiplusShutdown(g_gdiplusToken);
	WSACleanup();
	return static_cast<int>(message.wParam);
}
