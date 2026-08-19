#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <Xinput.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <gdiplus.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "GeneratedAssetTable.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "xinput9_1_0.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdiplus.lib")

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

	// Tray icon / menu.
	constexpr UINT kTrayCallbackMessage = WM_APP + 1;
	constexpr UINT kTrayIconId = 1;
	constexpr UINT kMenuIdToggle = 1001;
	constexpr UINT kMenuIdSettings = 1002;
	constexpr UINT kMenuIdExit = 1003;

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

	constexpr size_t kSettingsItemCount = 2; // 0: change shortcut, 1: clear all pad slots

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
		// True while PadViewer is being shown specifically to pick a Move's
		// destination pad, rather than the normal "pick a pad to act on"
		// mode. Reuses the same screen/grid per the original design intent.
		bool selectingMoveDestination = false;
		size_t moveSourceSlotIndex = 0;

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

	// Forward-declared here; defined later in the file. Confirm()/Back() run
	// before those definitions appear.
	void UpdateInputOwnership(HWND window);
	void HideOverlay(HWND window);
	void BeginShortcutCapture();
	void CancelShortcutCapture();

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
	constexpr unsigned int kPadFocusBloom = 0x00F3DDB8; // selected pad bloom (RGB 184,221,243)
	constexpr unsigned int kPadFocusEdge = 0x0098D8F6; // selected pad edge (RGB 246,216,152)

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
		unsigned int color, int radius, int width, int height, BYTE alpha)
	{
		Gdiplus::Bitmap mask(width, height, PixelFormat32bppPARGB);
		Gdiplus::Graphics mg(&mask);
		mg.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
		Gdiplus::SolidBrush solid(Gdiplus::Color(alpha, GetRValue(color), GetGValue(color), GetBValue(color)));
		mg.FillPath(&solid, &path);
		CompositeGlow(g, &mask, radius, width, height);
	}

	void DrawGlow(Gdiplus::Graphics& g, const Gdiplus::GraphicsPath& path,
		unsigned int color, int radius, int width, int height)
	{
		DrawGlow(g, path, color, radius, width, height, 255);
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
	// kPadGlowMargin so the champagne/blue halo has room. The cache key includes
	// the slot's background resource id, since each of the 7 pads renders a
	// different base image even at identical visuals/size.
	constexpr int kPadGlowMargin = 20;

	Gdiplus::Bitmap* RenderPad(int slotIndex, const RECT& cell, PadVisual visual)
	{
		const int w = (cell.right - cell.left) + kPadGlowMargin * 2;
		const int h = (cell.bottom - cell.top) + kPadGlowMargin * 2;
		const GlossKey key{GlossKind::Pad, static_cast<int>(visual), kPadBackgroundResourceIds[slotIndex], 0, w, h};
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

		if (selected)
			DrawGlow(g, path, kPadFocusBloom, 18, w, h, 72);
		else if (moveSource)
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

		// Occupied has no dedicated art, so it's a soft green tint over the
		// same background clear enough to keep the pad's own texture visible.
		if (occupied)
		{
			Gdiplus::SolidBrush tint(Gdiplus::Color(40, 52, 168, 110));
			g.FillPath(&tint, &path);
		}
		if (selected)
		{
			Gdiplus::LinearGradientBrush focusWash(rect,
				Gdiplus::Color(34, 255, 255, 255), Gdiplus::Color(10, 142, 196, 238),
				Gdiplus::LinearGradientModeForwardDiagonal);
			g.FillPath(&focusWash, &path);
		}

		const unsigned int border = moveSource ? kGlowBlue
			: (selected ? kPadFocusEdge
				: (occupied ? kPadBorderOccupied : kPadBorderIdle));
		Gdiplus::Pen borderPen(
			Gdiplus::Color(selected ? 175 : 255, GetRValue(border), GetGValue(border), GetBValue(border)),
			selected ? 1.25f : 2.0f);
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
	constexpr int kPortraitMargin = 8;

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
		g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
		g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

		const float d = static_cast<float>(diameter);
		const Gdiplus::RectF circle(static_cast<float>(kPortraitMargin), static_cast<float>(kPortraitMargin), d, d);
		Gdiplus::GraphicsPath circlePath;
		circlePath.AddEllipse(circle);

		if (focused)
			DrawGlow(g, circlePath, ringColor, 7, w, h);

		Gdiplus::Bitmap* photo = GetAssetBitmap(resId);
		if (photo)
		{
			// Cover-fit the photo inside the circle, then clip.
			const float scale = std::max(d / photo->GetWidth(), d / photo->GetHeight());
			const float drawW = photo->GetWidth() * scale;
			const float drawH = photo->GetHeight() * scale;
			const Gdiplus::RectF dest(kPortraitMargin + (d - drawW) / 2.0f,
				kPortraitMargin + (d - drawH) / 2.0f, drawW, drawH);
			Gdiplus::Region clip(&circlePath);
			g.SetClip(&clip);
			g.DrawImage(photo, dest);
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
		g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

		const float d = static_cast<float>(diameter);
		const Gdiplus::RectF circle(static_cast<float>(kPortraitMargin), static_cast<float>(kPortraitMargin), d, d);
		Gdiplus::GraphicsPath circlePath;
		circlePath.AddEllipse(circle);

		if (focused)
			DrawGlow(g, circlePath, ringColor, 7, w, h);

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
		g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

		const float d = static_cast<float>(diameter);
		const Gdiplus::RectF circle(static_cast<float>(kPortraitMargin), static_cast<float>(kPortraitMargin), d, d);
		Gdiplus::GraphicsPath circlePath;
		circlePath.AddEllipse(circle);

		if (focused)
			DrawGlow(g, circlePath, ringColor, 7, w, h);

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
	// drawn on top of it, plus a gold glow when focused. The logo alone
	// identifies the world - no text label.
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

		if (focused)
			DrawGlow(g, path, kGlowGold, 8, w, h);

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

		Gdiplus::Pen borderPen(Gdiplus::Color(200, 88, 96, 112), 1.5f);
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

	// Full-window background: the starfield resource scaled to cover the
	// window, with a dark overlay baked in for text legibility.
	Gdiplus::Bitmap* RenderBackground(int width, int height)
	{
		const GlossKey key{GlossKind::Background, 0, kBackgroundResourceId, 0, width, height};
		const auto cached = g_glossCache.find(key);
		if (cached != g_glossCache.end())
			return cached->second;

		Gdiplus::Bitmap* bitmap = new Gdiplus::Bitmap(width, height, PixelFormat32bppPARGB);
		Gdiplus::Graphics g(bitmap);
		g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

		Gdiplus::Bitmap* background = GetAssetBitmap(kBackgroundResourceId);
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
	// Basic GDI text helpers
	// ---------------------------------------------------------------------

	void DrawTextLine(HDC dc, const std::wstring& text, int x, int y, int width, COLORREF color, int height = 30)
	{
		SetTextColor(dc, color);
		RECT rect{x, y, x + width, y + height};
		DrawTextW(dc, text.c_str(), -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
	}

	void DrawTextLineCentered(HDC dc, const std::wstring& text, int x, int y, int width, COLORREF color, int height = 30)
	{
		SetTextColor(dc, color);
		RECT rect{x, y, x + width, y + height};
		DrawTextW(dc, text.c_str(), -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
	}

	// Centered word-wrap variant: shows the FULL string, wrapping onto extra
	// lines if it doesn't fit the width - never truncates with "…". Used for
	// names that must stay readable (roster labels, pad occupant names).
	void DrawTextWrappedCentered(HDC dc, const std::wstring& text, int x, int y, int width, int height, COLORREF color)
	{
		SetTextColor(dc, color);
		RECT rect{x, y, x + width, y + height};
		DrawTextW(dc, text.c_str(), -1, &rect, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
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

	// Sources the tag bytes from the embedded resource and sends the same
	// LOAD message as before, to whatever pad slot the user picked.
	void LoadRosterEntryToPad(const RosterEntry& entry)
	{
		std::wstring error;
		if (!SendLoadResourceToSlot(entry.binResourceId, g_app.slotIndex, error))
		{
			g_app.status = error;
			return;
		}

		// The listener always clears the destination before loading, so a
		// direct Load action still intentionally overwrites that pad.
		const std::wstring name = EntryDisplayName(entry);
		auto& slot = g_app.padState[g_app.slotIndex];
		slot.occupied = true;
		slot.figureName = name;
		slot.binResourceId = entry.binResourceId;
		slot.portraitResourceId = entry.portraitResourceId;
		slot.ringColor = entry.ringColor;

		g_app.status = L"LOAD sent: " + name + L" -> " + kSlots[g_app.slotIndex].label;
		g_app.screen = Screen::PadViewer;
	}

	void ClearSelectedPad()
	{
		std::wstring error;
		if (!SendClearSlot(g_app.slotIndex, error))
		{
			g_app.status = error;
			return;
		}

		g_app.padState[g_app.slotIndex] = PadSlot{};
		g_app.status = std::wstring(L"CLEAR sent: ") + kSlots[g_app.slotIndex].label;
		g_app.screen = Screen::PadViewer;
	}

	void ClearAllPads()
	{
		std::wstring error;
		size_t clearedCount = 0;
		for (size_t index = 0; index < kSlots.size(); ++index)
		{
			if (!SendClearSlot(index, error))
			{
				g_app.status = L"Clear all stopped after " + std::to_wstring(clearedCount) + L" pads: " + error;
				g_app.screen = Screen::Settings;
				return;
			}

			g_app.padState[index] = PadSlot{};
			++clearedCount;
		}

		g_app.status = L"Clear all pad sent: all 7 slots cleared.";
		g_app.screen = Screen::PadViewer;
	}

	// Moves whatever is tracked at moveSourceSlotIndex to destIndex. Called
	// once the user has picked a destination pad in the PadViewer's
	// move-destination mode.
	void MoveToDestination(size_t destIndex)
	{
		if (!g_app.padState[g_app.moveSourceSlotIndex].occupied)
		{
			g_app.status = L"There is nothing tracked on that pad to move.";
			g_app.selectingMoveDestination = false;
			g_app.screen = Screen::PadViewer;
			return;
		}

		std::wstring error;
		if (!SendMoveSlotToSlot(g_app.moveSourceSlotIndex, destIndex, error))
		{
			g_app.status = error;
			return;
		}

		const PadSlot sourceSlot = g_app.padState[g_app.moveSourceSlotIndex];
		const PadSlot destSlot = g_app.padState[destIndex];
		const std::wstring movedName = sourceSlot.figureName;
		if (destIndex != g_app.moveSourceSlotIndex)
		{
			if (destSlot.occupied)
			{
				if (!SendLoadResourceToSlot(destSlot.binResourceId, g_app.moveSourceSlotIndex, error))
				{
					g_app.padState[destIndex] = sourceSlot;
					g_app.padState[g_app.moveSourceSlotIndex] = PadSlot{};
					g_app.status = L"MOVE sent, but swap reload failed: " + error;
					g_app.selectingMoveDestination = false;
					g_app.screen = Screen::PadViewer;
					return;
				}

				g_app.padState[destIndex] = sourceSlot;
				g_app.padState[g_app.moveSourceSlotIndex] = destSlot;
				g_app.status = L"SWAP sent: " + sourceSlot.figureName + L" <-> " + destSlot.figureName;
				g_app.selectingMoveDestination = false;
				g_app.screen = Screen::PadViewer;
				return;
			}

			g_app.padState[destIndex] = sourceSlot;
			g_app.padState[g_app.moveSourceSlotIndex] = PadSlot{};
		}

		if (destIndex == g_app.moveSourceSlotIndex)
			g_app.status = L"REFRESH sent: " + movedName + L" on " + kSlots[destIndex].label;
		else
			g_app.status = L"MOVE sent: " + movedName + L" (" + kSlots[g_app.moveSourceSlotIndex].label +
				L" -> " + kSlots[destIndex].label + L")";
		g_app.selectingMoveDestination = false;
		g_app.screen = Screen::PadViewer;
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

	void MoveRosterSelection(int dx, int dy)
	{
		if (g_app.rosterSlots.empty())
			return;
		const size_t rows = (g_app.rosterSlots.size() + kRosterCols - 1) / kRosterCols;
		size_t row = g_app.rosterIndex / kRosterCols;
		size_t col = g_app.rosterIndex % kRosterCols;

		row = (row + static_cast<size_t>(dy) + rows) % rows;
		const size_t lastCol = std::min(kRosterCols, g_app.rosterSlots.size() - row * kRosterCols) - 1;
		col = (col + static_cast<size_t>(dx) + lastCol + 1) % (lastCol + 1);

		g_app.rosterIndex = row * kRosterCols + col;

		// Keep the focused row inside the visible viewport.
		const int focusedRow = static_cast<int>(row);
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
				ClearAllPads();
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
			g_app.screen = Screen::FranchiseList;
			break;
		case Screen::PlusPicker:
			g_app.plusGroup = nullptr;
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
		icon.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON));
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

	// Roster grid layout: 5 columns of larger portrait-circles with wrapped
	// name labels, vertical scrolling. Origin leaves room for the world logo.
	constexpr int kRosterOriginX = 60;
	constexpr int kRosterOriginY = 120;
	constexpr int kRosterPitchX = 156;
	constexpr int kRosterPitchY = 160;
	constexpr int kPortraitDiameter = 90;

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

		Gdiplus::Bitmap* pad = RenderPad(static_cast<int>(index), cell, visual);
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
			DrawTextLineCentered(dc, options[row], x + 8, y + kPillPadV + static_cast<int>(row) * kPillRowH,
				kPillWidth - 16, row == focusedIndex ? RGB(255, 255, 255) : RGB(196, 204, 216), kPillRowH);
		}
	}

	// Three floating circular image buttons for the selected pad's actions.
	// Upper side pads tuck the row against the pad's lower edge so the buttons
	// sit in the gap before the lower pads; center/lower pads place the row
	// just beneath the selected piece of glass.
	constexpr int kActionButtonSize = 42;
	constexpr int kActionButtonGap = 4;
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

	RECT GetActionButtonRect(size_t slotIndex, size_t actionIndex)
	{
		const RECT& cell = kPadCells[slotIndex];
		const int cellW = cell.right - cell.left;
		const int rowW = static_cast<int>(kPadActionCount) * kActionButtonSize +
			static_cast<int>(kPadActionCount - 1) * kActionButtonGap;
		const int rowX = cell.left + (cellW - rowW) / 2;
		int rowY = cell.bottom + 4;
		if (slotIndex == 0 || slotIndex == 2)
			rowY = cell.bottom - kActionButtonSize + 4;

		const int x = rowX + static_cast<int>(actionIndex) * (kActionButtonSize + kActionButtonGap);
		return {x, rowY, x + kActionButtonSize, rowY + kActionButtonSize};
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
			const int radius = kActionButtonSize / 2;
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
			const int size = kActionButtonSize + grow;

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
			DrawTextWrappedCentered(dc, EntryDisplayName(build), x - 16, circleY + diam + 8, step, labelH,
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

	void DrawRosterGrid(Gdiplus::Graphics& g, HDC dc)
	{
		if (g_app.rosterSlots.empty())
		{
			DrawTextLine(dc, L"No figures in this world.", 24, 200, 480, RGB(224, 230, 237));
			return;
		}

		const size_t visibleRows = kRosterVisibleRows;
		const size_t totalRows = (g_app.rosterSlots.size() + kRosterCols - 1) / kRosterCols;
		for (size_t row = 0; row < visibleRows; ++row)
		{
			for (size_t col = 0; col < kRosterCols; ++col)
			{
				const size_t slotIndex = (static_cast<size_t>(g_app.rosterTopRow) + row) * kRosterCols + col;
				if (slotIndex >= g_app.rosterSlots.size())
					break;
				const RosterSlot& slot = g_app.rosterSlots[slotIndex];
				const int x = kRosterOriginX + static_cast<int>(col) * kRosterPitchX;
				const int y = kRosterOriginY + static_cast<int>(row) * kRosterPitchY;
				const bool focused = slotIndex == g_app.rosterIndex;

				Gdiplus::Bitmap* visual = nullptr;
				std::wstring label;
				unsigned int color = 0;
				int resId = 0;
				if (slot.kind == RosterSlot::Kind::Plus)
				{
					label = L"+";
					color = slot.group && !slot.group->builds.empty() ? slot.group->builds[0].ringColor : kPadBorderIdle;
					visual = RenderPlusTile(color, focused, kPortraitDiameter);
				}
				else if (slot.entry)
				{
					label = slot.entry->name;
					color = slot.entry->ringColor;
					resId = slot.entry->portraitResourceId;
					visual = resId != 0
						? RenderPortrait(resId, color, focused, kPortraitDiameter)
						: RenderPlaceholder(label.empty() ? L'?' : label[0], color, focused, kPortraitDiameter);
				}

				const int circleX = x + (kRosterPitchX - kPortraitDiameter) / 2;
				const int circleY = y;
				if (visual)
					g.DrawImage(visual, circleX - kPortraitMargin, circleY - kPortraitMargin);

				DrawTextWrappedCentered(dc, label, x, y + kPortraitDiameter + 8, kRosterPitchX, 58,
					focused ? RGB(255, 236, 190) : RGB(214, 220, 230));
			}
		}

		const int trackTop = kRosterOriginY - 14;
		const int trackBottom = kRosterOriginY
			+ static_cast<int>(kRosterVisibleRows - 1) * kRosterPitchY + kPortraitDiameter + 58;
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
			const int gridBottom = kRosterOriginY + static_cast<int>(kRosterVisibleRows - 1) * kRosterPitchY + kPortraitDiameter + 58;
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
			// header the roster screen (this screen's parent) uses.
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
			const std::array<std::wstring, kSettingsItemCount> rows = {
				L"Toggle shortcut: " + DescribeShortcut(),
				L"Clear all pad",
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
					DrawTextLine(dc, L"Release every controller button first...", 24, y, width - 48, RGB(255, 204, 51), 26);
				else
					DrawTextLine(dc, L"Listening: press a controller combo, or a keyboard shortcut.", 24, y, width - 48, RGB(255, 204, 51), 26);
				DrawTextLine(dc, L"Controller combo needs a non-nav button. Back+Start cancels. Keyboard needs a modifier. Esc cancels.", 24, y + 26, width - 48, RGB(255, 204, 51), 26);
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

		BitBlt(windowDC, 0, 0, width, height, dc, 0, 0, SRCCOPY);

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
		// XInputGetState on an EMPTY slot performs a slow device enumeration
		// (documented XInput behavior), and doing that for up to three empty
		// slots on every 8ms tick stalls the poll loop - badly so when an
		// emulator is saturating the CPU. Empty slots are probed at most once
		// every 2s; connected slots keep the full 8ms cadence.
		static std::array<DWORD, XUSER_MAX_COUNT> nextEmptyProbe{};
		static std::array<bool, XUSER_MAX_COUNT> slotWasConnected{};

		UpdateInputOwnership(window);

		bool anyConnected = false;
		WORD combinedButtons = 0;
		WORD combinedPressed = 0;
		bool stickUp = false;
		bool stickDown = false;
		bool stickLeft = false;
		bool stickRight = false;

		const DWORD pollTick = GetTickCount();
		for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i)
		{
			if (!slotWasConnected[i] && pollTick < nextEmptyProbe[i])
			{
				previousButtons[i] = 0;
				continue;
			}

			XINPUT_STATE state{};
			const bool connected = XInputGetState(i, &state) == ERROR_SUCCESS;
			slotWasConnected[i] = connected;
			if (!connected)
				nextEmptyProbe[i] = pollTick + 2000;
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
		if ((combinedPressed & XINPUT_GAMEPAD_Y) && g_app.screen == Screen::PadViewer)
		{
			g_app.screen = Screen::Settings;
			changed = true;
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

	LoadUIFont();

	g_app.port = ReadPort();
	LoadShortcutFromIni();

	size_t embeddedTags = 0;
	for (size_t i = 0; i < kFranchiseCount; ++i)
	{
		embeddedTags += kFranchises[i].characters.size();
		for (const auto& vehicle : kFranchises[i].vehicles)
			embeddedTags += vehicle.builds.size();
	}
	g_app.status = std::to_wstring(embeddedTags) +
		L" tags embedded. Listener port: " + std::to_wstring(g_app.port) +
		L" | Toggle: " + DescribeShortcut();

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

	SetLayeredWindowAttributes(window, 0, kOverlayAlpha, LWA_ALPHA);
	AddTrayIcon(window);
	RegisterToggleHotkeyIfNeeded(window);
	SetTimer(window, kControllerTimer, 8, nullptr);
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
	Gdiplus::GdiplusShutdown(g_gdiplusToken);
	WSACleanup();
	return static_cast<int>(message.wParam);
}
