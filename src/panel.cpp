// Copyright (C) 2026 Jurica Dujmovic
// SPDX-License-Identifier: GPL-3.0-or-later
//
// panel.cpp - the control panel window, drawn with Direct2D. See panel.h.

#include "panel.h"

#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <dwrite_1.h>
#include <shellapi.h>
#include <windowsx.h>

#include <cmath>
#include <map>

#include "hotkeys.h"

namespace panel {

namespace {

constexpr wchar_t kClassName[] = L"VitureHudPanel";

// Layout, in device-independent pixels (1/96 inch).
constexpr float kWidth = 1100;
constexpr float kHeight = 848;
// Left column (view, behaviour, buttons) and right column (edge lens). The
// monitor map spans both.
constexpr float kLeftEnd = 736;
constexpr float kRightX = 760;
constexpr float kRightEnd = 1076;  // kWidth minus the 24 DIP margin
constexpr float kPad = 24;
constexpr float kRowH = 56;
constexpr float kControlW = 300;

// Shown in the About card. Keep the version in step with res/icon.rc.
constexpr wchar_t kVersion[] = L"1.0.2";
constexpr wchar_t kRepoUrl[] = L"https://github.com/Daeliel/viture-hud";

constexpr UINT_PTR kTimerRepaint = 1;
constexpr UINT_PTR kTimerThumbs = 2;
constexpr UINT kRepaintMs = 250;   // fps and status text
constexpr UINT kThumbsMs = 1500;   // live monitor thumbnails

// Palette: the same near-black and green as the toast and status bar on the
// glasses.
constexpr UINT32 kBg = 0x07090C;
constexpr UINT32 kSurface = 0x0E1319;
constexpr UINT32 kWell = 0x0A0E13;
constexpr UINT32 kText = 0xE6EDF3;
constexpr UINT32 kMuted = 0x8B96A3;
constexpr UINT32 kDim = 0x5C6672;
constexpr UINT32 kAccent = 0x39FF88;
constexpr UINT32 kAccentHover = 0x6BFFA8;
constexpr UINT32 kOnAccent = 0x04140B;
constexpr UINT32 kWarn = 0xFFA94D;
constexpr UINT32 kTrackOff = 0x262D36;
constexpr UINT32 kKnobOff = 0xC9D1D9;

const wchar_t* const kZoomLabels[] = {L"1x", L"1.5x", L"2x", L"Fit"};
const wchar_t* const kLensLabels[] = {L"Off", L"1x", L"1.5x", L"2x"};
const wchar_t* const kLensModeLabels[] = {L"Follow cursor", L"Fixed spot"};
const wchar_t* const kScreensLabels[] = {L"Main screen", L"All screens"};
const wchar_t* const kMethodLabels[] = {L"Switch off", L"Cover"};
constexpr int kMaxStandIns = 3;  // listed in the Privacy card

enum HitId {
  kHitNone = 0,
  kHitDisplay,
  kHitZoom,
  kHitLens,
  kHitLensMode,
  kHitLensHandle,  // resize corner in the lens editor
  kHitLensBody,    // the lens itself, dragged to move it (fixed spot)
  kHitLensInset,   // an edge of the dashed area: arg 0 left, 1 top, 2 right, 3 bottom
  kHitLensTrigger, // inner edge of the shaded band, same args
  kHitLensReset,
  kHitLock,
  kHitPip,
  kHitFollow,
  kHitAutoShow,
  kHitStartup,
  kHitNext,
  kHitStop,
  kHitAbout,        // "About" in the header
  kHitAboutGithub,  // buttons on the About card
  kHitCardClose,    // Close on any card
  kHitShortcuts,    // "Shortcuts" in the header
  kHitShortcut,     // a shortcut's keys on the Shortcuts card: arg is the Key
  kHitShortcutsReset,
  kHitPrivacy,          // Privacy mode on/off, next to Next monitor
  kHitPrivacySettings,  // the gear on that button
  kHitPrivacyToggle,    // on/off on the Privacy card
  kHitPrivacyScreens,
  kHitPrivacyMethod,
  kHitStandIn,  // arg 0 none, n for the n-th stand-in
  kHitPrivacyDim,
};

// A card shown over the panel. Only its own controls respond while it is open.
enum Card { kCardNone, kCardAbout, kCardShortcuts, kCardPrivacy };

struct Hit {
  D2D1_RECT_F rc;
  int id;
  int arg;
  bool enabled;
};

struct Thumb {
  ID2D1Bitmap* bitmap = nullptr;
  float w = 0;  // pixel size of the bitmap
  float h = 0;
};

struct Tile {
  D2D1_RECT_F rc;
  const Display* display;
};

HINSTANCE g_instance = nullptr;
HICON g_icon = nullptr;
Actions g_actions;
HWND g_hwnd = nullptr;
float g_dpi = 96;

ID2D1Factory* g_d2d = nullptr;
IDWriteFactory* g_dwrite = nullptr;
ID2D1HwndRenderTarget* g_rt = nullptr;
ID2D1SolidColorBrush* g_brush = nullptr;
ID2D1GradientStopCollection* g_shadeStops = nullptr;

IDWriteTextFormat* g_fTitle = nullptr;
IDWriteTextFormat* g_fBody = nullptr;
IDWriteTextFormat* g_fStrong = nullptr;
IDWriteTextFormat* g_fSmall = nullptr;
IDWriteTextFormat* g_fCaps = nullptr;
IDWriteTextFormat* g_fBadge = nullptr;
IDWriteTextFormat* g_fWrap = nullptr;  // small, wrapping, top-aligned
IDWriteTextFormat* g_fWrapBody = nullptr;  // body size, wrapping, top-aligned

std::map<std::wstring, Thumb> g_thumbs;
std::vector<Hit> g_hits;
int g_hoverId = kHitNone;
int g_hoverArg = 0;
int g_pressId = kHitNone;
int g_pressArg = 0;
bool g_trackingLeave = false;
Card g_card = kCardNone;

// The shortcut being recorded (a Key), or -1, and why the last keys pressed
// for it were refused.
int g_recording = -1;
std::wstring g_recordError;

// Dragging in the lens editor.
struct LensDrag {
  int id = kHitNone;  // kHitLensBody, kHitLensHandle, kHitLensInset/Trigger
  int edge = 0;       // for kHitLensInset/Trigger: which edge
  float startX = 0;   // pointer at the start, in DIPs
  float startY = 0;
  float previewW = 1;  // editor preview size, in DIPs
  float previewH = 1;
  viewmath::LensLayout start;
  viewmath::LensLayout now;
};
LensDrag g_drag;
D2D1_RECT_F g_lensPreview = {0, 0, 1, 1};  // where the editor preview was drawn

template <typename T>
void SafeRelease(T*& p) {
  if (p) {
    p->Release();
    p = nullptr;
  }
}

D2D1_COLOR_F Color(UINT32 rgb, float alpha = 1.0f) {
  return D2D1::ColorF(rgb, alpha);
}

D2D1_RECT_F Rect(float l, float t, float r, float b) {
  return D2D1::RectF(l, t, r, b);
}

bool Contains(const D2D1_RECT_F& rc, float x, float y) {
  return x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom;
}

UINT DpiForWindow(HWND hwnd) {
  using Fn = UINT(WINAPI*)(HWND);
  static Fn fn = nullptr;
  static bool looked = false;
  if (!looked) {
    looked = true;
    FARPROC p = GetProcAddress(GetModuleHandleW(L"user32.dll"),
                               "GetDpiForWindow");
    if (p) memcpy(&fn, &p, sizeof(fn));
  }
  UINT dpi = fn ? fn(hwnd) : 96;
  return dpi ? dpi : 96;
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------
IDWriteTextFormat* MakeFormat(float size, DWRITE_FONT_WEIGHT weight) {
  IDWriteTextFormat* f = nullptr;
  if (FAILED(g_dwrite->CreateTextFormat(
          L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
          DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f))) {
    return nullptr;
  }
  f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  IDWriteInlineObject* ellipsis = nullptr;
  if (SUCCEEDED(g_dwrite->CreateEllipsisTrimmingSign(f, &ellipsis))) {
    DWRITE_TRIMMING trim = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    f->SetTrimming(&trim, ellipsis);
    ellipsis->Release();
  }
  return f;
}

bool CreateFactories() {
  if (!g_d2d && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                         &g_d2d))) {
    return false;
  }
  if (!g_dwrite &&
      FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                 __uuidof(IDWriteFactory),
                                 (IUnknown**)&g_dwrite))) {
    return false;
  }
  if (!g_fTitle) {
    g_fTitle = MakeFormat(17, DWRITE_FONT_WEIGHT_BOLD);
    g_fBody = MakeFormat(14, DWRITE_FONT_WEIGHT_NORMAL);
    g_fStrong = MakeFormat(14, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_fSmall = MakeFormat(12, DWRITE_FONT_WEIGHT_NORMAL);
    g_fCaps = MakeFormat(11, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_fBadge = MakeFormat(10.5f, DWRITE_FONT_WEIGHT_BOLD);
    g_fWrap = MakeFormat(12, DWRITE_FONT_WEIGHT_NORMAL);
    if (g_fWrap) {
      g_fWrap->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
      g_fWrap->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    g_fWrapBody = MakeFormat(14, DWRITE_FONT_WEIGHT_NORMAL);
    if (g_fWrapBody) {
      g_fWrapBody->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
      g_fWrapBody->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
  }
  return g_fTitle && g_fBody && g_fStrong && g_fSmall && g_fCaps && g_fBadge &&
         g_fWrap && g_fWrapBody;
}

void DrawLabel(const std::wstring& text, IDWriteTextFormat* format,
               const D2D1_RECT_F& rc, UINT32 color, float alpha = 1.0f,
               DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
  format->SetTextAlignment(align);
  g_brush->SetColor(Color(color, alpha));
  g_rt->DrawText(text.c_str(), (UINT32)text.size(), format, rc, g_brush,
                 D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float TextWidth(const std::wstring& text, IDWriteTextFormat* format) {
  IDWriteTextLayout* layout = nullptr;
  if (FAILED(g_dwrite->CreateTextLayout(text.c_str(), (UINT32)text.size(),
                                        format, 2000, 100, &layout))) {
    return 0;
  }
  DWRITE_TEXT_METRICS m = {};
  layout->GetMetrics(&m);
  layout->Release();
  return m.widthIncludingTrailingWhitespace;
}

// Draws wrapping text |width| wide with its top-left at (x, y), or only
// measures it when |draw| is false. Returns its height.
float DrawWrapped(const std::wstring& text, IDWriteTextFormat* format, float x,
                  float y, float width, UINT32 color, bool draw = true) {
  IDWriteTextLayout* layout = nullptr;
  if (FAILED(g_dwrite->CreateTextLayout(text.c_str(), (UINT32)text.size(),
                                        format, width, 2000, &layout))) {
    return 0;
  }
  layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
  DWRITE_TEXT_METRICS m = {};
  layout->GetMetrics(&m);
  if (draw) {
    g_brush->SetColor(Color(color));
    g_rt->DrawTextLayout(D2D1::Point2F(x, y), layout, g_brush);
  }
  layout->Release();
  return m.height;
}

// Upper-case section label with a little letter spacing, like "DISPLAYS".
void DrawSpaced(const std::wstring& text, IDWriteTextFormat* format, float x,
                float y, float spacing, UINT32 color, float alpha = 1.0f) {
  IDWriteTextLayout* layout = nullptr;
  if (FAILED(g_dwrite->CreateTextLayout(text.c_str(), (UINT32)text.size(),
                                        format, 600, 24, &layout))) {
    return;
  }
  layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
  layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
  IDWriteTextLayout1* spaced = nullptr;
  if (SUCCEEDED(layout->QueryInterface(__uuidof(IDWriteTextLayout1),
                                       (void**)&spaced))) {
    DWRITE_TEXT_RANGE all = {0, (UINT32)text.size()};
    spaced->SetCharacterSpacing(0, spacing, 0, all);
    spaced->Release();
  }
  g_brush->SetColor(Color(color, alpha));
  g_rt->DrawTextLayout(D2D1::Point2F(x, y), layout, g_brush);
  layout->Release();
}

// ---------------------------------------------------------------------------
// Device resources
// ---------------------------------------------------------------------------
void DiscardTarget() {
  for (auto& it : g_thumbs) SafeRelease(it.second.bitmap);
  g_thumbs.clear();
  SafeRelease(g_shadeStops);
  SafeRelease(g_brush);
  SafeRelease(g_rt);
}

bool EnsureTarget() {
  if (g_rt) return true;
  RECT rc;
  GetClientRect(g_hwnd, &rc);
  D2D1_SIZE_U size = D2D1::SizeU((UINT32)(rc.right - rc.left),
                                 (UINT32)(rc.bottom - rc.top));
  D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_DEFAULT,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                        D2D1_ALPHA_MODE_PREMULTIPLIED),
      g_dpi, g_dpi);
  if (FAILED(g_d2d->CreateHwndRenderTarget(
          props, D2D1::HwndRenderTargetProperties(g_hwnd, size), &g_rt))) {
    return false;
  }
  g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
  g_rt->CreateSolidColorBrush(Color(kText), &g_brush);
  D2D1_GRADIENT_STOP stops[] = {
      {0.0f, Color(0x000000, 0.0f)},
      {0.45f, Color(0x000000, 0.72f)},
      {1.0f, Color(0x000000, 0.92f)},
  };
  g_rt->CreateGradientStopCollection(stops, 3, &g_shadeStops);
  return g_brush && g_shadeStops;
}

// ---------------------------------------------------------------------------
// Monitor map
// ---------------------------------------------------------------------------
D2D1_RECT_F MapArea() { return Rect(kPad, 102, kWidth - kPad, 294); }

// Lays the monitors out like Windows' display settings: same arrangement and
// proportions, scaled to fit the area.
std::vector<Tile> LayoutTiles(const State& s, const D2D1_RECT_F& area) {
  std::vector<Tile> tiles;
  if (s.displays.empty()) return tiles;
  RECT bb = s.displays[0].bounds;
  for (const auto& d : s.displays) UnionRect(&bb, &bb, &d.bounds);
  float bw = (float)(bb.right - bb.left);
  float bh = (float)(bb.bottom - bb.top);
  if (bw <= 0 || bh <= 0) return tiles;
  float aw = area.right - area.left;
  float ah = area.bottom - area.top;
  float scale = std::fmin(aw / bw, ah / bh);
  float ox = area.left + (aw - bw * scale) / 2;
  float oy = area.top + (ah - bh * scale) / 2;
  const float gap = 4;
  for (const auto& d : s.displays) {
    D2D1_RECT_F rc = Rect(ox + (d.bounds.left - bb.left) * scale + gap,
                          oy + (d.bounds.top - bb.top) * scale + gap,
                          ox + (d.bounds.right - bb.left) * scale - gap,
                          oy + (d.bounds.bottom - bb.top) * scale - gap);
    tiles.push_back({rc, &d});
  }
  return tiles;
}

// Grabs a small, smoothly scaled copy of each monitor for the map.
void UpdateThumbs() {
  if (!g_hwnd || IsIconic(g_hwnd) || !EnsureTarget()) return;
  State s = g_actions.getState();
  std::vector<Tile> tiles = LayoutTiles(s, MapArea());

  HDC screen = GetDC(nullptr);
  if (!screen) return;
  for (const auto& t : tiles) {
    if (t.display->isGlasses) continue;
    int w = (int)std::lround((t.rc.right - t.rc.left) * g_dpi / 96);
    int h = (int)std::lround((t.rc.bottom - t.rc.top) * g_dpi / 96);
    if (w <= 0 || h <= 0) continue;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) {
      DeleteDC(mem);
      continue;
    }
    HGDIOBJ old = SelectObject(mem, dib);
    SetStretchBltMode(mem, HALFTONE);
    SetBrushOrgEx(mem, 0, 0, nullptr);
    const RECT& b = t.display->bounds;
    StretchBlt(mem, 0, 0, w, h, screen, b.left, b.top, b.right - b.left,
               b.bottom - b.top, SRCCOPY);
    GdiFlush();

    ID2D1Bitmap* bitmap = nullptr;
    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        g_dpi, g_dpi);
    g_rt->CreateBitmap(D2D1::SizeU((UINT32)w, (UINT32)h), bits, (UINT32)w * 4,
                       props, &bitmap);
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);

    if (bitmap) {
      Thumb& thumb = g_thumbs[t.display->device];
      SafeRelease(thumb.bitmap);
      thumb.bitmap = bitmap;
      thumb.w = (float)w;
      thumb.h = (float)h;
    }
  }
  ReleaseDC(nullptr, screen);
}

void AddHit(const D2D1_RECT_F& rc, int id, int arg, bool enabled) {
  g_hits.push_back({rc, id, arg, enabled});
}

bool Hovered(int id, int arg = 0) {
  return g_hoverId == id && g_hoverArg == arg;
}

// The keys of shortcut |key| ("Alt+D"), or empty when it is not set.
std::wstring Keys(const State& s, int key) {
  return key < (int)s.shortcuts.size() ? s.shortcuts[(size_t)key].keys : L"";
}

// "text  ·  Alt+D", or just the text when the shortcut is not set.
std::wstring WithKeys(const std::wstring& text, const State& s, int key) {
  std::wstring keys = Keys(s, key);
  return keys.empty() ? text : text + L"  ·  " + keys;
}

int ShortcutProblems(const State& s) {
  int n = 0;
  for (const auto& sc : s.shortcuts) n += sc.problem.empty() ? 0 : 1;
  return n;
}

// Small padlock, |size| tall, centred on (cx, cy).
void DrawLockGlyph(float cx, float cy, UINT32 color, float alpha) {
  g_brush->SetColor(Color(color, alpha));
  D2D1_ROUNDED_RECT shackle = {Rect(cx - 4, cy - 8, cx + 4, cy + 2), 4, 4};
  g_rt->DrawRoundedRectangle(shackle, g_brush, 1.6f);
  D2D1_ROUNDED_RECT body = {Rect(cx - 6.5f, cy - 2, cx + 6.5f, cy + 7), 2, 2};
  g_rt->FillRoundedRectangle(body, g_brush);
}

// Small cog for "settings".
void DrawGearGlyph(float cx, float cy, UINT32 color, float alpha) {
  g_brush->SetColor(Color(color, alpha));
  g_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 4.2f, 4.2f), g_brush,
                    1.8f);
  for (int i = 0; i < 8; ++i) {
    float a = i * 3.14159265f / 4;
    float c = std::cos(a), sn = std::sin(a);
    g_rt->DrawLine(D2D1::Point2F(cx + c * 5.6f, cy + sn * 5.6f),
                   D2D1::Point2F(cx + c * 8.2f, cy + sn * 8.2f), g_brush, 2.4f);
  }
}

void DrawGlassesGlyph(float cx, float cy, UINT32 color, float alpha) {
  g_brush->SetColor(Color(color, alpha));
  const float lw = 22, lh = 14, gap = 8, stroke = 1.8f;
  D2D1_ROUNDED_RECT left = {Rect(cx - gap / 2 - lw, cy - lh / 2, cx - gap / 2,
                                 cy + lh / 2),
                            5, 5};
  D2D1_ROUNDED_RECT right = {Rect(cx + gap / 2, cy - lh / 2,
                                  cx + gap / 2 + lw, cy + lh / 2),
                             5, 5};
  g_rt->DrawRoundedRectangle(left, g_brush, stroke);
  g_rt->DrawRoundedRectangle(right, g_brush, stroke);
  g_rt->DrawLine(D2D1::Point2F(cx - gap / 2, cy - 2),
                 D2D1::Point2F(cx + gap / 2, cy - 2), g_brush, stroke);
  g_rt->DrawLine(D2D1::Point2F(cx - gap / 2 - lw, cy - 3),
                 D2D1::Point2F(cx - gap / 2 - lw - 7, cy - 6), g_brush, stroke);
  g_rt->DrawLine(D2D1::Point2F(cx + gap / 2 + lw, cy - 3),
                 D2D1::Point2F(cx + gap / 2 + lw + 7, cy - 6), g_brush, stroke);
}

void DrawTile(const Tile& t, const State& s) {
  const Display& d = *t.display;
  const D2D1_RECT_F& rc = t.rc;
  float w = rc.right - rc.left;
  float h = rc.bottom - rc.top;
  D2D1_ROUNDED_RECT rr = {rc, 8, 8};
  bool shown = s.showing && d.displayIndex == s.shownIndex;
  bool hover = !d.isGlasses && Hovered(kHitDisplay, d.displayIndex);

  g_brush->SetColor(Color(kSurface));
  g_rt->FillRoundedRectangle(rr, g_brush);

  if (d.isGlasses) {
    DrawGlassesGlyph((rc.left + rc.right) / 2, rc.top + h * 0.42f, kMuted,
                     0.9f);
    DrawLabel(L"Glasses", g_fSmall,
              Rect(rc.left + 8, rc.bottom - 30, rc.right - 8, rc.bottom - 10),
              kMuted, 1.0f, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_brush->SetColor(Color(kMuted, 0.35f));
    ID2D1StrokeStyle* dashed = nullptr;
    D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
        D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_DASH, 0.0f);
    g_d2d->CreateStrokeStyle(sp, nullptr, 0, &dashed);
    g_rt->DrawRoundedRectangle(rr, g_brush, 1.2f, dashed);
    SafeRelease(dashed);
    return;
  }

  // Live thumbnail, clipped to the rounded tile.
  auto thumb = g_thumbs.find(d.device);
  if (thumb != g_thumbs.end() && thumb->second.bitmap) {
    ID2D1BitmapBrush* bb = nullptr;
    if (SUCCEEDED(g_rt->CreateBitmapBrush(thumb->second.bitmap, &bb))) {
      // The bitmap is pixel-sized at the panel's dpi, i.e. exactly w x h DIPs.
      float bwDip = thumb->second.w * 96 / g_dpi;
      float bhDip = thumb->second.h * 96 / g_dpi;
      bb->SetTransform(D2D1::Matrix3x2F::Scale(w / bwDip, h / bhDip) *
                       D2D1::Matrix3x2F::Translation(rc.left, rc.top));
      bb->SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
      bb->SetOpacity(shown || hover ? 1.0f : 0.72f);
      g_rt->FillRoundedRectangle(rr, bb);
      bb->Release();
    }
  }

  // Shade the bottom so the name stays readable over any picture.
  float shadeH = std::fmin(76.0f, h);
  ID2D1LinearGradientBrush* shade = nullptr;
  if (SUCCEEDED(g_rt->CreateLinearGradientBrush(
          D2D1::LinearGradientBrushProperties(
              D2D1::Point2F(0, rc.bottom - shadeH),
              D2D1::Point2F(0, rc.bottom)),
          g_shadeStops, &shade))) {
    g_rt->FillRoundedRectangle(rr, shade);
    shade->Release();
  }

  // Name and size.
  std::wstring detail = std::to_wstring(d.bounds.right - d.bounds.left) +
                        L" × " +
                        std::to_wstring(d.bounds.bottom - d.bounds.top);
  if (d.isPrimary) detail += L"  ·  primary";
  if (h >= 64) {
    DrawLabel(d.name, g_fStrong,
              Rect(rc.left + 10, rc.bottom - 42, rc.right - 10, rc.bottom - 24),
              kText);
    DrawLabel(detail, g_fSmall,
              Rect(rc.left + 10, rc.bottom - 24, rc.right - 10, rc.bottom - 8),
              kMuted);
  } else {
    DrawLabel(d.name, g_fStrong,
              Rect(rc.left + 8, rc.bottom - 24, rc.right - 8, rc.bottom - 6),
              kText);
  }

  if (shown) {
    const std::wstring badge = L"ON GLASSES";
    float bw = TextWidth(badge, g_fBadge) + 16;
    D2D1_ROUNDED_RECT pill = {
        Rect(rc.left + 8, rc.top + 8, rc.left + 8 + bw, rc.top + 26), 9, 9};
    g_brush->SetColor(Color(kAccent));
    g_rt->FillRoundedRectangle(pill, g_brush);
    DrawLabel(badge, g_fBadge, pill.rect, kOnAccent, 1.0f,
              DWRITE_TEXT_ALIGNMENT_CENTER);
  }

  if (shown) {
    g_brush->SetColor(Color(kAccent));
    g_rt->DrawRoundedRectangle(rr, g_brush, 2.0f);
  } else {
    g_brush->SetColor(Color(0xFFFFFF, hover ? 0.38f : 0.10f));
    g_rt->DrawRoundedRectangle(rr, g_brush, 1.0f);
  }

  AddHit(rc, kHitDisplay, d.displayIndex, true);
}

// ---------------------------------------------------------------------------
// Controls
// ---------------------------------------------------------------------------
void DrawCard(const D2D1_RECT_F& rc) {
  D2D1_ROUNDED_RECT rr = {rc, 10, 10};
  g_brush->SetColor(Color(kSurface));
  g_rt->FillRoundedRectangle(rr, g_brush);
  g_brush->SetColor(Color(0xFFFFFF, 0.08f));
  g_rt->DrawRoundedRectangle(rr, g_brush, 1.0f);
}

void DrawDivider(float left, float right, float y) {
  g_brush->SetColor(Color(0xFFFFFF, 0.06f));
  g_rt->DrawLine(D2D1::Point2F(left, y), D2D1::Point2F(right, y), g_brush,
                 1.0f);
}

// Label on the left of a card row, with a muted line underneath.
void DrawRowText(const D2D1_RECT_F& row, const std::wstring& title,
                 const std::wstring& detail, bool enabled) {
  float right = row.right - kControlW - 32;
  DrawLabel(title, g_fStrong,
            Rect(row.left + 18, row.top + 9, right, row.top + 29),
            enabled ? kText : kDim);
  DrawLabel(detail, g_fSmall,
            Rect(row.left + 18, row.top + 29, right, row.top + 47), kMuted,
            enabled ? 1.0f : 0.6f);
}

void DrawSegmented(const D2D1_RECT_F& rc, const wchar_t* const* labels,
                   int count, int selected, int hitId) {
  D2D1_ROUNDED_RECT outer = {rc, 8, 8};
  g_brush->SetColor(Color(kWell));
  g_rt->FillRoundedRectangle(outer, g_brush);
  g_brush->SetColor(Color(0xFFFFFF, 0.10f));
  g_rt->DrawRoundedRectangle(outer, g_brush, 1.0f);

  float segW = (rc.right - rc.left) / count;
  for (int i = 0; i < count; ++i) {
    D2D1_RECT_F seg = Rect(rc.left + i * segW, rc.top, rc.left + (i + 1) * segW,
                           rc.bottom);
    D2D1_ROUNDED_RECT inner = {
        Rect(seg.left + 3, seg.top + 3, seg.right - 3, seg.bottom - 3), 6, 6};
    bool on = i == selected;
    if (on) {
      g_brush->SetColor(Color(kAccent));
      g_rt->FillRoundedRectangle(inner, g_brush);
    } else if (Hovered(hitId, i)) {
      g_brush->SetColor(Color(0xFFFFFF, 0.07f));
      g_rt->FillRoundedRectangle(inner, g_brush);
    }
    DrawLabel(labels[i], g_fStrong, seg, on ? kOnAccent : kText,
              on ? 1.0f : 0.85f, DWRITE_TEXT_ALIGNMENT_CENTER);
    AddHit(seg, hitId, i, true);
  }
}

void DrawToggle(float right, float cy, bool on, bool hover) {
  const float w = 42, h = 24;
  D2D1_ROUNDED_RECT track = {Rect(right - w, cy - h / 2, right, cy + h / 2),
                             h / 2, h / 2};
  if (on) {
    g_brush->SetColor(Color(hover ? kAccentHover : kAccent));
  } else {
    g_brush->SetColor(Color(hover ? 0x323A45 : kTrackOff));
  }
  g_rt->FillRoundedRectangle(track, g_brush);
  float knobX = on ? right - h / 2 : right - w + h / 2;
  g_brush->SetColor(Color(on ? kBg : kKnobOff));
  g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, cy), 8.5f, 8.5f),
                    g_brush);
}

// A filled (primary) or outlined button, with an optional dimmer hint such as
// the keyboard shortcut after the label.
void DrawButton(const D2D1_RECT_F& rc, const std::wstring& label,
                const std::wstring& hint, bool primary, bool enabled,
                bool hover, bool leftAlign = false) {
  D2D1_ROUNDED_RECT rr = {rc, 9, 9};
  UINT32 textColor = kText;
  float textAlpha = enabled ? 1.0f : 0.4f;
  if (primary) {
    g_brush->SetColor(enabled ? Color(hover ? kAccentHover : kAccent)
                              : Color(kAccent, 0.18f));
    g_rt->FillRoundedRectangle(rr, g_brush);
    textColor = enabled ? kOnAccent : kAccent;
    textAlpha = enabled ? 1.0f : 0.55f;
  } else {
    if (enabled && hover) {
      g_brush->SetColor(Color(0xFFFFFF, 0.05f));
      g_rt->FillRoundedRectangle(rr, g_brush);
    }
    g_brush->SetColor(Color(0xFFFFFF, enabled && hover ? 0.34f : 0.16f));
    g_rt->DrawRoundedRectangle(rr, g_brush, 1.0f);
  }

  float hintW = hint.empty() ? 0 : TextWidth(hint, g_fSmall);
  float gap = hint.empty() ? 0 : 10;
  if (leftAlign) {
    D2D1_RECT_F labelRc = Rect(rc.left + 14, rc.top, rc.right - 14 - hintW - gap,
                               rc.bottom);
    DrawLabel(label, g_fStrong, labelRc, textColor, textAlpha);
    if (!hint.empty()) {
      DrawLabel(hint, g_fSmall,
                Rect(rc.right - 14 - hintW, rc.top, rc.right - 14, rc.bottom),
                textColor, textAlpha * 0.6f);
    }
    return;
  }
  float labelW = TextWidth(label, g_fStrong);
  float total = labelW + gap + hintW;
  float x = (rc.left + rc.right - total) / 2;
  DrawLabel(label, g_fStrong, Rect(x, rc.top, x + labelW + 1, rc.bottom),
            textColor, textAlpha);
  if (!hint.empty()) {
    DrawLabel(hint, g_fSmall,
              Rect(x + labelW + gap, rc.top, x + total + 1, rc.bottom),
              textColor, textAlpha * 0.6f);
  }
}

// ---------------------------------------------------------------------------
// Sections
// ---------------------------------------------------------------------------
void DrawHeader(const State& s) {
  UINT32 statusColor = s.glassesConnected ? kAccent : kWarn;
  g_brush->SetColor(Color(statusColor));
  g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kPad + 5, 38), 5, 5), g_brush);
  DrawSpaced(L"VITURE HUD", g_fTitle, kPad + 20, 26, 2.5f, kText);

  std::wstring status = s.glassesConnected ? L"Glasses connected"
                                           : L"Glasses not connected";
  float tw = TextWidth(status, g_fStrong);
  float pillW = tw + 40;
  D2D1_ROUNDED_RECT pill = {
      Rect(kWidth - kPad - pillW, 24, kWidth - kPad, 52), 14, 14};
  g_brush->SetColor(Color(statusColor, 0.10f));
  g_rt->FillRoundedRectangle(pill, g_brush);
  g_brush->SetColor(Color(statusColor, 0.35f));
  g_rt->DrawRoundedRectangle(pill, g_brush, 1.0f);
  g_brush->SetColor(Color(statusColor));
  g_rt->FillEllipse(
      D2D1::Ellipse(D2D1::Point2F(pill.rect.left + 16, 38), 3.5f, 3.5f),
      g_brush);
  DrawLabel(status, g_fStrong,
            Rect(pill.rect.left + 26, 24, pill.rect.right - 10, 52),
            statusColor);

  D2D1_RECT_F about = Rect(pill.rect.left - 12 - 84, 24, pill.rect.left - 12, 52);
  DrawButton(about, L"About", L"", false, true, Hovered(kHitAbout));
  AddHit(about, kHitAbout, 0, true);

  // Shortcuts, with an orange dot when one of them does not work.
  D2D1_RECT_F keys = Rect(about.left - 10 - 112, 24, about.left - 10, 52);
  DrawButton(keys, L"Shortcuts", L"", false, true, Hovered(kHitShortcuts));
  AddHit(keys, kHitShortcuts, 0, true);
  if (ShortcutProblems(s) > 0) {
    g_brush->SetColor(Color(kWarn));
    g_rt->FillEllipse(
        D2D1::Ellipse(D2D1::Point2F(keys.right - 7, keys.top + 7), 4, 4),
        g_brush);
  }
}

void DrawSectionTitle(const std::wstring& title, float y,
                      const std::wstring& hint = L"") {
  DrawSpaced(title, g_fCaps, kPad + 2, y, 1.6f, kMuted);
  if (!hint.empty()) {
    DrawLabel(hint, g_fSmall, Rect(kPad, y - 2, kWidth - kPad, y + 16), kDim,
              1.0f, DWRITE_TEXT_ALIGNMENT_TRAILING);
  }
}

void DrawDisplays(const State& s) {
  DrawSectionTitle(L"DISPLAYS", 78,
                   s.glassesConnected ? L"Click a monitor to show it on the glasses"
                                      : L"");
  D2D1_RECT_F area = MapArea();
  if (s.displays.empty()) {
    DrawLabel(L"No monitors found", g_fBody, area, kMuted, 1.0f,
              DWRITE_TEXT_ALIGNMENT_CENTER);
    return;
  }
  for (const auto& t : LayoutTiles(s, area)) DrawTile(t, s);
}

void DrawView(const State& s) {
  DrawSectionTitle(L"VIEW", 318);
  D2D1_RECT_F card = Rect(kPad, 340, kLeftEnd, 340 + kRowH * 3);
  DrawCard(card);

  // Zoom.
  D2D1_RECT_F row = Rect(card.left, card.top, card.right, card.top + kRowH);
  DrawRowText(row, L"Zoom", WithKeys(L"1x is pixel-perfect", s, kKeyZoom),
              true);
  float cy = (row.top + row.bottom) / 2;
  DrawSegmented(Rect(row.right - 16 - kControlW, cy - 17, row.right - 16,
                     cy + 17),
                kZoomLabels, 4, s.zoomStep, kHitZoom);
  DrawDivider(card.left + 16, card.right - 16, row.bottom);

  // Window lock.
  row = Rect(card.left, row.bottom, card.right, row.bottom + kRowH);
  cy = (row.top + row.bottom) / 2;
  std::wstring detail;
  if (s.locked) {
    detail = WithKeys(L"Showing only this window", s, kKeyWindowLock);
  } else if (s.picking) {
    detail = L"Click the window you want on the glasses";
  } else {
    detail = WithKeys(L"Show one window, fitted", s, kKeyWindowLock);
  }
  bool canLock = s.glassesConnected;
  DrawRowText(row, L"Window lock", detail, s.glassesConnected);
  D2D1_RECT_F button =
      Rect(row.right - 16 - kControlW, cy - 17, row.right - 16, cy + 17);
  std::wstring label;
  std::wstring hint;
  if (s.locked) {
    label = L"Unlock  ·  " + s.lockedTitle;
  } else if (s.picking) {
    label = L"Click a window…";
    hint = L"Esc cancels";
  } else {
    label = L"Pick a window to lock";
  }
  DrawButton(button, label, hint, s.locked || s.picking, canLock,
             Hovered(kHitLock) && canLock, true);
  AddHit(button, kHitLock, 0, canLock);
  DrawDivider(card.left + 16, card.right - 16, row.bottom);

  // Picture-in-picture.
  row = Rect(card.left, row.bottom, card.right, row.bottom + kRowH);
  bool hover = Hovered(kHitPip);
  if (hover) {
    D2D1_ROUNDED_RECT hl = {
        Rect(row.left + 4, row.top + 4, row.right - 4, row.bottom - 4), 8, 8};
    g_brush->SetColor(Color(0xFFFFFF, 0.03f));
    g_rt->FillRoundedRectangle(hl, g_brush);
  }
  std::wstring pipDetail = WithKeys(
      s.pipName.empty() ? L"Live view of your other monitor"
                        : L"Live view of " + s.pipName + L" in a corner",
      s, kKeyPip);
  DrawRowText(row, L"Picture-in-picture", pipDetail, true);
  DrawToggle(row.right - 18, (row.top + row.bottom) / 2, s.pipOn, hover);
  AddHit(row, kHitPip, 0, true);
}

void DrawBehaviour(const State& s) {
  DrawSectionTitle(L"BEHAVIOUR", 530);
  D2D1_RECT_F card = Rect(kPad, 552, kLeftEnd, 552 + kRowH * 3);
  DrawCard(card);

  struct Item {
    int id;
    const wchar_t* title;
    const wchar_t* detail;
    bool on;
  } items[] = {
      {kHitFollow, L"Follow the text cursor",
       L"While you type, the view follows where you are typing",
       s.followCaret},
      {kHitAutoShow, L"Show a monitor when the glasses connect",
       L"Starts on its own, with the monitor under the mouse", s.autoShow},
      {kHitStartup, L"Start with Windows",
       L"Launch Viture HUD when you sign in", s.startWithWindows},
  };
  for (int i = 0; i < 3; ++i) {
    D2D1_RECT_F row = Rect(card.left, card.top + i * kRowH, card.right,
                           card.top + (i + 1) * kRowH);
    bool hover = Hovered(items[i].id);
    if (hover) {
      D2D1_ROUNDED_RECT hl = {
          Rect(row.left + 4, row.top + 4, row.right - 4, row.bottom - 4), 8, 8};
      g_brush->SetColor(Color(0xFFFFFF, 0.03f));
      g_rt->FillRoundedRectangle(hl, g_brush);
    }
    DrawRowText(row, items[i].title, items[i].detail, true);
    DrawToggle(row.right - 18, (row.top + row.bottom) / 2, items[i].on, hover);
    if (i < 2) DrawDivider(card.left + 16, card.right - 16, row.bottom);
    AddHit(row, items[i].id, 0, true);
  }
}

// Privacy mode as a split button: the main part turns it on or off, the cog
// on the right opens its settings. Green while it is on.
void DrawPrivacyButton(const D2D1_RECT_F& rc, const State& s) {
  const bool on = s.privacy != 0;
  const bool can = on || s.glassesConnected;
  const float gearW = 44;
  D2D1_RECT_F main = Rect(rc.left, rc.top, rc.right - gearW, rc.bottom);
  D2D1_RECT_F gear = Rect(rc.right - gearW, rc.top, rc.right, rc.bottom);
  const bool hoverMain = Hovered(kHitPrivacy) && can;
  const bool hoverGear = Hovered(kHitPrivacySettings);

  D2D1_ROUNDED_RECT rr = {rc, 9, 9};
  if (on) {
    g_brush->SetColor(Color(kAccent, 0.14f));
    g_rt->FillRoundedRectangle(rr, g_brush);
  }
  // Each half lights up on its own when hovered.
  if (hoverMain || hoverGear) {
    g_brush->SetColor(Color(0xFFFFFF, 0.06f));
    g_rt->PushAxisAlignedClip(hoverMain ? main : gear,
                              D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    g_rt->FillRoundedRectangle(rr, g_brush);
    g_rt->PopAxisAlignedClip();
  }
  if (on) {
    g_brush->SetColor(Color(kAccent, 0.85f));
    g_rt->DrawRoundedRectangle(rr, g_brush, 1.4f);
  } else {
    g_brush->SetColor(Color(0xFFFFFF, hoverMain || hoverGear ? 0.34f : 0.16f));
    g_rt->DrawRoundedRectangle(rr, g_brush, 1.0f);
  }
  g_brush->SetColor(on ? Color(kAccent, 0.45f) : Color(0xFFFFFF, 0.12f));
  g_rt->DrawLine(D2D1::Point2F(gear.left, rc.top + 10),
                 D2D1::Point2F(gear.left, rc.bottom - 10), g_brush, 1.0f);

  const UINT32 fg = on ? kAccent : kText;
  const float alpha = can ? 1.0f : 0.4f;
  const std::wstring label = on ? L"Privacy on" : L"Privacy";
  const std::wstring keys = Keys(s, kKeyPrivacy);
  float labelW = TextWidth(label, g_fStrong);
  float keysW = keys.empty() ? 0 : TextWidth(keys, g_fSmall) + 10;
  float total = 13 + 10 + labelW + keysW;
  float x = (main.left + main.right - total) / 2;
  DrawLockGlyph(x + 6.5f, (rc.top + rc.bottom) / 2 + 1, fg, alpha);
  x += 13 + 10;
  DrawLabel(label, g_fStrong, Rect(x, rc.top, x + labelW + 1, rc.bottom), fg,
            alpha);
  if (!keys.empty()) {
    DrawLabel(keys, g_fSmall,
              Rect(x + labelW + 10, rc.top, x + labelW + keysW + 1, rc.bottom),
              fg, alpha * 0.6f);
  }
  DrawGearGlyph((gear.left + gear.right) / 2, (rc.top + rc.bottom) / 2,
                on ? kAccent : kMuted, hoverGear ? 1.0f : 0.85f);
  AddHit(main, kHitPrivacy, 0, can);
  AddHit(gear, kHitPrivacySettings, 0, true);
}

void DrawActions(const State& s) {
  float top = 746, bottom = 794;
  bool canNext = s.glassesConnected;
  D2D1_RECT_F stop = Rect(kLeftEnd - 168, top, kLeftEnd, bottom);
  D2D1_RECT_F privacy = Rect(stop.left - 12 - 246, top, stop.left - 12, bottom);
  D2D1_RECT_F next = Rect(kPad, top, privacy.left - 12, bottom);
  DrawButton(next, L"Next monitor", Keys(s, kKeyNext), true, canNext,
             Hovered(kHitNext) && canNext);
  AddHit(next, kHitNext, 0, canNext);
  DrawPrivacyButton(privacy, s);
  DrawButton(stop, L"Stop", Keys(s, kKeyStop), false, s.showing,
             Hovered(kHitStop) && s.showing);
  AddHit(stop, kHitStop, 0, s.showing);

  // Live status line.
  std::wstring line;
  if (!s.glassesConnected) {
    line = L"Plug in the glasses to start";
  } else if (!s.showing) {
    std::wstring next = Keys(s, kKeyNext);
    line = next.empty() ? L"Nothing on the glasses  ·  click a monitor above"
                        : L"Nothing on the glasses  ·  press " + next;
  } else {
    std::wstring name;
    for (const auto& d : s.displays) {
      if (d.displayIndex == s.shownIndex) name = d.name;
    }
    wchar_t buf[200];
    swprintf_s(buf, L"Showing %ls  ·  %.2fx  ·  %d fps  ·  %ls",
               name.c_str(), s.scale, s.fps,
               s.duplication ? L"Desktop Duplication" : L"GDI capture (slow)");
    line = buf;
    if (s.privacy != 0) line = L"Privacy on  ·  " + line;
  }
  DrawLabel(line, g_fSmall, Rect(kPad + 2, 808, kLeftEnd - 150, 828),
            s.privacy != 0 ? kAccent : kMuted);
  std::wstring previous = Keys(s, kKeyPrevious);
  if (!previous.empty()) {
    DrawLabel(previous + L" previous", g_fSmall, Rect(kPad, 808, kLeftEnd, 828),
              kDim, 1.0f, DWRITE_TEXT_ALIGNMENT_TRAILING);
  }
}

// The edge lens column: magnification, placement, and a small picture of the
// glasses' screen where the lens can be dragged and resized.
void DrawLensEditor(const State& s) {
  DrawSpaced(L"EDGE LENS", g_fCaps, kRightX + 2, 318, 1.6f, kMuted);
  bool on = s.lensStep > 0;
  const viewmath::LensLayout& layout =
      g_drag.id != kHitNone ? g_drag.now : s.lensLayout;

  float left = kRightX + 16;
  float right = kRightEnd - 16;
  float y = 340 + 14;
  float cardTop = 340;

  // Card first, sized to everything below.
  float glassesW = (float)(s.glassesBounds.right - s.glassesBounds.left);
  float glassesH = (float)(s.glassesBounds.bottom - s.glassesBounds.top);
  if (glassesW <= 0 || glassesH <= 0) {
    glassesW = 1920;
    glassesH = 1080;
  }
  float pw = right - left;
  float ph = pw * glassesH / glassesW;
  float cardBottom =
      y + 24 + 34 + 16 + 24 + 34 + 16 + ph + 10 + 50 + 22 + 30 + 16;
  DrawCard(Rect(kRightX, cardTop, kRightEnd, cardBottom));

  DrawLabel(L"Magnification", g_fStrong, Rect(left, y, right, y + 20), kText);
  y += 24;
  DrawSegmented(Rect(left, y, right, y + 34), kLensLabels, 4, s.lensStep,
                kHitLens);
  y += 34 + 16;

  DrawLabel(L"Placement", g_fStrong, Rect(left, y, right, y + 20),
            on ? kText : kDim);
  y += 24;
  if (on) {
    DrawSegmented(Rect(left, y, right, y + 34), kLensModeLabels, 2,
                  layout.fixed ? 1 : 0, kHitLensMode);
  } else {
    D2D1_ROUNDED_RECT off = {Rect(left, y, right, y + 34), 8, 8};
    g_brush->SetColor(Color(kWell));
    g_rt->FillRoundedRectangle(off, g_brush);
    DrawLabel(layout.fixed ? L"Fixed spot" : L"Follow cursor", g_fStrong,
              off.rect, kDim, 1.0f, DWRITE_TEXT_ALIGNMENT_CENTER);
  }
  y += 34 + 16;

  D2D1_RECT_F insetEdges[4];
  bool haveInsetEdges = false;

  // The glasses' screen, with the blurry edge band and the safe area.
  D2D1_RECT_F screen = Rect(left, y, right, y + ph);
  g_lensPreview = screen;
  D2D1_ROUNDED_RECT screenR = {screen, 6, 6};
  g_brush->SetColor(Color(0xFFFFFF, 0.06f));
  g_rt->FillRoundedRectangle(screenR, g_brush);  // the edge band
  float bandX = pw * (float)layout.trigger;
  float bandY = ph * (float)layout.trigger;
  D2D1_RECT_F inner = Rect(screen.left + bandX, screen.top + bandY,
                           screen.right - bandX, screen.bottom - bandY);
  g_brush->SetColor(Color(kWell));
  g_rt->FillRectangle(inner, g_brush);
  bool triggerActive = g_drag.id == kHitLensTrigger;
  for (int e = 0; e < 4; ++e) triggerActive |= Hovered(kHitLensTrigger, e);
  if (on) {
    g_brush->SetColor(Color(0xFFFFFF, triggerActive ? 0.85f : 0.3f));
    g_rt->DrawRectangle(inner, g_brush, triggerActive ? 1.6f : 1.0f);
  }
  // Each line is grabbed from its own side: the band's edge from the band
  // side, the dashed edge from the inside, so they work even when they meet.
  D2D1_RECT_F triggerEdges[4] = {
      Rect(inner.left - 6, inner.top, inner.left + 2, inner.bottom),
      Rect(inner.left, inner.top - 6, inner.right, inner.top + 2),
      Rect(inner.right - 2, inner.top, inner.right + 6, inner.bottom),
      Rect(inner.left, inner.bottom - 2, inner.right, inner.bottom + 6)};
  g_brush->SetColor(Color(0xFFFFFF, 0.14f));
  g_rt->DrawRoundedRectangle(screenR, g_brush, 1.0f);
  if (on && !layout.fixed) {
    ID2D1StrokeStyle* dashed = nullptr;
    D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
        D2D1_LINE_JOIN_MITER, 10.0f, D2D1_DASH_STYLE_DASH, 0.0f);
    g_d2d->CreateStrokeStyle(sp, nullptr, 0, &dashed);
    float sx = pw * (float)layout.inset;
    float sy = ph * (float)layout.inset;
    D2D1_RECT_F safe = Rect(screen.left + sx, screen.top + sy,
                            screen.right - sx, screen.bottom - sy);
    bool insetActive = Hovered(kHitLensInset, 0) ||
                       Hovered(kHitLensInset, 1) ||
                       Hovered(kHitLensInset, 2) ||
                       Hovered(kHitLensInset, 3) ||
                       g_drag.id == kHitLensInset;
    g_brush->SetColor(Color(kAccent, insetActive ? 0.9f : 0.45f));
    g_rt->DrawRectangle(safe, g_brush, insetActive ? 1.6f : 1.0f, dashed);
    SafeRelease(dashed);
    insetEdges[0] = Rect(safe.left - 2, safe.top, safe.left + 6, safe.bottom);
    insetEdges[1] = Rect(safe.left, safe.top - 2, safe.right, safe.top + 6);
    insetEdges[2] = Rect(safe.right - 6, safe.top, safe.right + 2, safe.bottom);
    insetEdges[3] = Rect(safe.left, safe.bottom - 6, safe.right, safe.bottom + 2);
    haveInsetEdges = true;
  }

  // The lens. Following the cursor, it is shown for a cursor at the left edge.
  double exampleX = glassesW * 0.04;
  double exampleY = glassesH * 0.5;
  viewmath::LensRect lens = viewmath::PlaceLensFor(
      layout, exampleX, exampleY, (int)glassesW, (int)glassesH);
  float k = pw / glassesW;
  D2D1_RECT_F lensR = Rect(screen.left + lens.x * k, screen.top + lens.y * k,
                           screen.left + (lens.x + lens.width) * k,
                           screen.top + (lens.y + lens.height) * k);
  float alpha = on ? 1.0f : 0.35f;
  bool bodyHover = Hovered(kHitLensBody) || g_drag.id == kHitLensBody;
  g_brush->SetColor(Color(kAccent, (bodyHover ? 0.24f : 0.14f) * alpha));
  g_rt->FillRectangle(lensR, g_brush);
  g_brush->SetColor(Color(kAccent, alpha));
  g_rt->DrawRectangle(lensR, g_brush, 1.5f);
  DrawLabel(kLensLabels[on ? s.lensStep : 1], g_fBadge, lensR, kAccent, alpha,
            DWRITE_TEXT_ALIGNMENT_CENTER);
  if (on && !layout.fixed) {
    D2D1_POINT_2F c = D2D1::Point2F(screen.left + (float)exampleX * k,
                                    screen.top + (float)exampleY * k);
    g_brush->SetColor(Color(kText, 0.9f));
    g_rt->DrawEllipse(D2D1::Ellipse(c, 3.5f, 3.5f), g_brush, 1.5f);
  }
  D2D1_RECT_F handle = Rect(lensR.right - 5, lensR.bottom - 5, lensR.right + 5,
                            lensR.bottom + 5);
  bool handleHover = Hovered(kHitLensHandle) || g_drag.id == kHitLensHandle;
  g_brush->SetColor(Color(handleHover ? kAccentHover : kAccent, alpha));
  g_rt->FillRectangle(handle, g_brush);
  // Handle first, then the dashed edges, then the body: they overlap, and
  // the first hit wins.
  AddHit(Rect(handle.left - 4, handle.top - 4, handle.right + 4,
              handle.bottom + 4),
         kHitLensHandle, 0, on);
  if (haveInsetEdges) {
    for (int e = 0; e < 4; ++e) AddHit(insetEdges[e], kHitLensInset, e, on);
  }
  for (int e = 0; e < 4; ++e) AddHit(triggerEdges[e], kHitLensTrigger, e, on);
  AddHit(lensR, kHitLensBody, 0, on && layout.fixed);
  y += ph + 10;

  const wchar_t* help =
      !on ? L"Pick a magnification above to turn the lens on."
          : layout.fixed
                ? L"When the cursor enters the shaded band, the lens appears "
                  L"here. Drag the lens, the band's edge, or the corner to "
                  L"resize."
                : L"When the cursor enters the shaded band, the lens appears "
                  L"next to it inside the dashed area. Drag either line, or "
                  L"the corner to resize.";
  g_fWrap->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
  g_brush->SetColor(Color(kMuted));
  g_rt->DrawText(help, (UINT32)wcslen(help), g_fWrap,
                 Rect(left, y, right, y + 50), g_brush);
  y += 50;

  wchar_t size[96];
  if (layout.fixed) {
    swprintf_s(size, L"Size %d%%   \u00B7   Trigger %d%%",
               (int)std::lround(layout.size * 100),
               (int)std::lround(layout.trigger * 100));
  } else {
    swprintf_s(size, L"Size %d%%   \u00B7   Offset %d%%   \u00B7   Trigger %d%%",
               (int)std::lround(layout.size * 100),
               (int)std::lround(layout.inset * 100),
               (int)std::lround(layout.trigger * 100));
  }
  DrawLabel(size, g_fSmall, Rect(left, y, right, y + 18), on ? kText : kDim);
  y += 22;
  DrawLabel(L"of the glasses' size", g_fSmall, Rect(left, y, right - 88, y + 30),
            kDim);
  D2D1_RECT_F reset = Rect(right - 80, y, right, y + 30);
  DrawButton(reset, L"Reset", L"", false, on, Hovered(kHitLensReset) && on);
  AddHit(reset, kHitLensReset, 0, on);
}

// Dims the panel and draws an empty card |w| x |h| in the middle, with a
// green dot and |caption| at its top. Returns the card's top-left.
void DrawModal(float w, float h, const wchar_t* caption, float* left,
               float* top) {
  *left = (kWidth - w) / 2;
  *top = std::fmax(12.0f, (kHeight - h) / 2);
  g_brush->SetColor(Color(0x000000, 0.66f));
  g_rt->FillRectangle(Rect(0, 0, kWidth, kHeight), g_brush);
  D2D1_ROUNDED_RECT card = {Rect(*left, *top, *left + w, *top + h), 14, 14};
  g_brush->SetColor(Color(kSurface));
  g_rt->FillRoundedRectangle(card, g_brush);
  g_brush->SetColor(Color(0xFFFFFF, 0.10f));
  g_rt->DrawRoundedRectangle(card, g_brush, 1.0f);
  const float x = *left + 32, y = *top + 28;
  g_brush->SetColor(Color(kAccent));
  g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + 4, y + 7), 4, 4), g_brush);
  DrawSpaced(caption, g_fCaps, x + 16, y, 1.6f, kMuted);
}

// The About card: who made the app, why, and the one thank-you asked for.
// Drawn over the dimmed panel; while it is open only its buttons respond.
void DrawAbout() {
  const std::wstring name = L"Jurica Dujmovi\u0107";
  const std::wstring tagline =
      L"Senior full-stack developer from Croatia, working on AI systems";
  const std::wstring why =
      L"I was disappointed by how little VITURE offers people who don\u2019t "
      L"want to go down the janky cable route, or whose laptop doesn\u2019t "
      L"have a USB-C port that also carries DisplayPort. So I built Viture "
      L"HUD.";
  const std::wstring ask =
      L"It is free. The only thank-you I ask is that you contribute: report "
      L"and fix bugs, improve the code, and see what you can make the app do "
      L"yourself.";
  std::wstring footer = L"Version ";
  footer += kVersion;
  footer += L"  \u00B7  GPL-3.0-or-later  \u00B7  Unofficial; not affiliated "
            L"with VITURE";

  const float cardW = 600, pad = 32, textW = cardW - 2 * pad;
  const float whyH = DrawWrapped(why, g_fWrapBody, 0, 0, textW, kText, false);
  const float askH = DrawWrapped(ask, g_fWrapBody, 0, 0, textW, kText, false);
  const float cardH =
      28 + 26 + 26 + 20 + 22 + whyH + 14 + askH + 26 + 40 + 20 + 18 + 26;
  float left = 0, top = 0;
  DrawModal(cardW, cardH, L"ABOUT THE DEVELOPER", &left, &top);
  const float x = left + pad, right = left + cardW - pad;
  float y = top + 28 + 26;
  DrawLabel(name, g_fTitle, Rect(x, y, right, y + 26), kText);
  y += 26;
  DrawLabel(tagline, g_fBody, Rect(x, y, right, y + 20), kMuted);
  y += 20 + 22;
  DrawWrapped(why, g_fWrapBody, x, y, textW, kText);
  y += whyH + 14;
  DrawWrapped(ask, g_fWrapBody, x, y, textW, kText);
  y += askH + 26;

  D2D1_RECT_F close = Rect(right - 110, y, right, y + 40);
  D2D1_RECT_F github = Rect(x, y, x + 240, y + 40);
  DrawButton(github, L"Contribute on GitHub", L"", true, true,
             Hovered(kHitAboutGithub));
  AddHit(github, kHitAboutGithub, 0, true);
  DrawButton(close, L"Close", L"Esc", false, true, Hovered(kHitCardClose));
  AddHit(close, kHitCardClose, 0, true);
  y += 40 + 20;
  DrawLabel(footer, g_fSmall, Rect(x, y, right, y + 18), kDim);
}

// A row on a card: a dark well with a title and a muted line on the left and
// a toggle on the right. The whole row is the click target.
void DrawToggleRow(const D2D1_RECT_F& row, const std::wstring& title,
                   const std::wstring& detail, bool on, int hitId,
                   bool enabled) {
  bool hover = Hovered(hitId) && enabled;
  D2D1_ROUNDED_RECT well = {row, 10, 10};
  g_brush->SetColor(Color(kWell));
  g_rt->FillRoundedRectangle(well, g_brush);
  if (hover) {
    g_brush->SetColor(Color(0xFFFFFF, 0.03f));
    g_rt->FillRoundedRectangle(well, g_brush);
  }
  g_brush->SetColor(Color(0xFFFFFF, hover ? 0.16f : 0.07f));
  g_rt->DrawRoundedRectangle(well, g_brush, 1.0f);
  float mid = (row.top + row.bottom) / 2;
  DrawLabel(title, g_fStrong, Rect(row.left + 18, mid - 19, row.right - 80, mid + 1),
            enabled ? kText : kDim);
  DrawLabel(detail, g_fSmall, Rect(row.left + 18, mid + 1, row.right - 80, mid + 19),
            kMuted, enabled ? 1.0f : 0.6f);
  if (enabled) {
    DrawToggle(row.right - 18, mid, on, hover);
  } else {
    D2D1_ROUNDED_RECT track = {
        Rect(row.right - 60, mid - 12, row.right - 18, mid + 12), 12, 12};
    g_brush->SetColor(Color(kTrackOff, 0.5f));
    g_rt->FillRoundedRectangle(track, g_brush);
  }
  AddHit(row, hitId, 0, enabled);
}

// The Privacy card: on/off, which screens, how, the stand-in display and
// the brightness.
void DrawPrivacy(const State& s) {
  const float cardW = 680, pad = 32, textW = cardW - 2 * pad;
  const bool switchOff = s.privacyMethod == 0;
  const std::wstring keys = Keys(s, kKeyPrivacy);

  std::wstring intro =
      L"Your screens go dark, so nobody can read them over your shoulder. "
      L"The glasses keep showing your work.";
  if (!keys.empty()) intro += L" " + keys + L" turns it on and off.";

  std::wstring status;
  if (s.privacy != 0) {
    status = L"On  ·  " + (s.privacySummary.empty() ? std::wstring(L"active")
                                                    : s.privacySummary);
  } else if (!s.glassesConnected) {
    status = L"Off  ·  connect the glasses to use it";
  } else {
    status = L"Off";
  }

  const bool all = s.privacyScreens == 1;
  const std::wstring which =
      all ? L"every screen but the glasses"
          : (s.hasBuiltIn ? L"the built-in screen" : L"the main screen");
  std::wstring help;
  if (switchOff) {
    help = L"Switch off turns " + which + L" off. A stand-in display takes " +
           (all ? L"each one's" : L"its") +
           L" exact place and size, so your windows stay where they are." +
           (all ? L" Each screen needs its own stand-in; screens without one "
                  L"are covered."
                : L" Without a stand-in, the screen is covered instead.");
  } else {
    help = L"Cover lays black over " + which +
           L". The glasses still see what is underneath and nothing in "
           L"Windows changes, but the mouse pointer and some pop-ups can show "
           L"on top.";
  }
  if (!all && !s.hasBuiltIn) {
    help += L" The main screen is your primary one.";
  }
  if (!s.canCover) {
    help += L" Covering needs Windows 10 version 2004 or newer.";
  }

  const bool showStandIns = switchOff;
  const int standInCount =
      (int)std::fmin((float)s.standIns.size(), (float)kMaxStandIns);
  const std::wstring setup =
      L"No spare display found. Install a virtual display driver (for "
      L"example Virtual Display Driver), give it the same resolution as the "
      L"screen it stands in for, then choose “Disconnect this display” for "
      L"it in Settings > Display. It shows up here.";

  const float introH = DrawWrapped(intro, g_fWrapBody, 0, 0, textW, kText, false);
  const float helpH = DrawWrapped(help, g_fWrap, 0, 0, textW, kMuted, false);
  const float setupH = DrawWrapped(setup, g_fWrap, 0, 0, textW, kMuted, false);
  const float noteH = s.privacyNote.empty()
                          ? 0
                          : DrawWrapped(s.privacyNote, g_fWrap, 0, 0, textW - 28,
                                        kWarn, false);
  float standInH = 0;
  if (showStandIns) {
    standInH = 22 + (standInCount == 0 ? setupH + 4 : 40.0f * (standInCount + 1)) +
               18;
  }
  const float cardH = 28 + 26 + 30 + introH + 20 + 62 + 22 + 22 + 36 + 12 +
                      helpH + 18 + standInH + 58 + 16 +
                      (noteH > 0 ? noteH + 20 + 14 : 0) + 40 + 28;

  float left = 0, top = 0;
  DrawModal(cardW, cardH, L"PRIVACY MODE", &left, &top);
  const float x = left + pad, right = left + cardW - pad;
  float y = top + 28 + 26;

  DrawLabel(L"Black out your screens", g_fTitle, Rect(x, y, right, y + 26), kText);
  y += 30;
  DrawWrapped(intro, g_fWrapBody, x, y, textW, kText);
  y += introH + 20;

  bool canToggle = s.privacy != 0 || s.glassesConnected;
  DrawToggleRow(Rect(x, y, right, y + 62), L"Privacy mode", status,
                s.privacy != 0, kHitPrivacyToggle, canToggle);
  y += 62 + 22;

  // Which screens, and how, side by side.
  const float colW = (textW - 16) / 2;
  DrawSpaced(L"SCREENS", g_fCaps, x + 2, y, 1.6f, kMuted);
  DrawSpaced(L"HOW", g_fCaps, x + colW + 16 + 2, y, 1.6f, kMuted);
  y += 22;
  const wchar_t* const screensLabels[] = {
      s.hasBuiltIn ? L"Built-in screen" : L"Main screen", kScreensLabels[1]};
  DrawSegmented(Rect(x, y, x + colW, y + 36), screensLabels, 2,
                s.privacyScreens, kHitPrivacyScreens);
  DrawSegmented(Rect(x + colW + 16, y, right, y + 36), kMethodLabels, 2,
                s.privacyMethod, kHitPrivacyMethod);
  y += 36 + 12;
  DrawWrapped(help, g_fWrap, x, y, textW, kMuted);
  y += helpH + 18;

  if (showStandIns) {
    DrawSpaced(L"STAND-IN DISPLAY", g_fCaps, x + 2, y, 1.6f, kMuted);
    y += 22;
    if (standInCount == 0) {
      DrawWrapped(setup, g_fWrap, x, y, textW, kMuted);
      y += setupH + 4;
    } else {
      for (int i = 0; i <= standInCount; ++i) {
        const bool none = i == 0;
        const std::wstring id = none ? L"" : s.standIns[(size_t)i - 1].id;
        const std::wstring name =
            none ? L"None  ·  cover the screen instead"
                 : s.standIns[(size_t)i - 1].name;
        const bool picked = s.standIn == id;
        D2D1_RECT_F row = Rect(x, y, right, y + 36);
        const bool hover = Hovered(kHitStandIn, i);
        if (hover || picked) {
          D2D1_ROUNDED_RECT hl = {row, 8, 8};
          g_brush->SetColor(picked ? Color(kAccent, 0.07f)
                                   : Color(0xFFFFFF, 0.04f));
          g_rt->FillRoundedRectangle(hl, g_brush);
        }
        float cy = (row.top + row.bottom) / 2;
        D2D1_POINT_2F c = D2D1::Point2F(row.left + 20, cy);
        g_brush->SetColor(picked ? Color(kAccent) : Color(0xFFFFFF, 0.35f));
        g_rt->DrawEllipse(D2D1::Ellipse(c, 8, 8), g_brush, 1.6f);
        if (picked) {
          g_rt->FillEllipse(D2D1::Ellipse(c, 4, 4), g_brush);
        }
        DrawLabel(name, none ? g_fBody : g_fStrong,
                  Rect(row.left + 40, row.top, row.right - 110, row.bottom),
                  none ? kMuted : kText);
        if (picked && !none && s.privacy == 2) {
          const std::wstring badge = L"IN USE";
          float bw = TextWidth(badge, g_fBadge) + 16;
          D2D1_ROUNDED_RECT pill = {
              Rect(row.right - 12 - bw, cy - 9, row.right - 12, cy + 9), 9, 9};
          g_brush->SetColor(Color(kAccent));
          g_rt->FillRoundedRectangle(pill, g_brush);
          DrawLabel(badge, g_fBadge, pill.rect, kOnAccent, 1.0f,
                    DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        AddHit(row, kHitStandIn, i, true);
        y += 40;
      }
    }
    y += 18;
  }

  DrawToggleRow(Rect(x, y, right, y + 58), L"Turn the brightness down",
                s.hasBuiltIn
                    ? L"While covered, the built-in screen goes to 0%, and back after"
                    : L"Only for a laptop's own screen; there is none here",
                s.privacyDim, kHitPrivacyDim, s.hasBuiltIn);
  y += 58 + 16;

  if (noteH > 0) {
    D2D1_ROUNDED_RECT box = {Rect(x, y, right, y + noteH + 20), 8, 8};
    g_brush->SetColor(Color(kWarn, 0.08f));
    g_rt->FillRoundedRectangle(box, g_brush);
    g_brush->SetColor(Color(kWarn, 0.35f));
    g_rt->DrawRoundedRectangle(box, g_brush, 1.0f);
    g_brush->SetColor(Color(kWarn));
    g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + 15, y + 17), 3.5f, 3.5f),
                      g_brush);
    DrawWrapped(s.privacyNote, g_fWrap, x + 28, y + 10, textW - 28, kWarn);
    y += noteH + 20 + 14;
  }

  D2D1_RECT_F close = Rect(right - 110, y, right, y + 40);
  DrawButton(close, L"Close", L"Esc", false, true, Hovered(kHitCardClose));
  AddHit(close, kHitCardClose, 0, true);
}

// The Shortcuts card: every hotkey, click one and press new keys.
void DrawShortcuts(const State& s) {
  const float cardW = 660, pad = 32, textW = cardW - 2 * pad;
  const float rowH = 52;
  const std::wstring intro =
      L"Click a shortcut, then press the keys you want. Esc cancels, "
      L"Backspace removes the shortcut.";
  const std::wstring caveat =
      L"Shortcuts that only work inside another app, like Alt+B in Firefox, "
      L"cannot be detected. If an app stops reacting to a key, pick a "
      L"different one here.";
  const float introH = DrawWrapped(intro, g_fWrap, 0, 0, textW, kMuted, false);
  const float caveatH = DrawWrapped(caveat, g_fWrap, 0, 0, textW, kDim, false);
  const int count = (int)s.shortcuts.size();
  const float cardH = 28 + 26 + 30 + introH + 18 + rowH * count + 8 + 14 +
                      caveatH + 22 + 40 + 28;

  float left = 0, top = 0;
  DrawModal(cardW, cardH, L"SHORTCUTS", &left, &top);
  const float x = left + pad, right = left + cardW - pad;
  float y = top + 28 + 26;

  DrawLabel(L"Keyboard shortcuts", g_fTitle, Rect(x, y, right, y + 26), kText);
  y += 30;
  DrawWrapped(intro, g_fWrap, x, y, textW, kMuted);
  y += introH + 18;

  D2D1_ROUNDED_RECT well = {Rect(x, y, right, y + rowH * count + 8), 10, 10};
  g_brush->SetColor(Color(kWell));
  g_rt->FillRoundedRectangle(well, g_brush);
  g_brush->SetColor(Color(0xFFFFFF, 0.07f));
  g_rt->DrawRoundedRectangle(well, g_brush, 1.0f);
  y += 4;

  const float chipW = 190;
  for (int i = 0; i < count; ++i) {
    const Shortcut& sc = s.shortcuts[(size_t)i];
    D2D1_RECT_F row = Rect(x, y, right, y + rowH);
    const bool recording = g_recording == i;
    const bool hover = Hovered(kHitShortcut, i);
    float cy = (row.top + row.bottom) / 2;

    // What it does, or what went wrong.
    std::wstring line = sc.detail;
    UINT32 lineColor = kMuted;
    if (recording && !g_recordError.empty()) {
      line = g_recordError;
      lineColor = kWarn;
    } else if (!sc.problem.empty()) {
      line = sc.problem;
      lineColor = kWarn;
    }
    float textRight = right - 16 - chipW - 16;
    DrawLabel(sc.title, g_fStrong, Rect(x + 18, cy - 19, textRight, cy + 1), kText);
    DrawLabel(line, g_fSmall, Rect(x + 18, cy + 1, textRight, cy + 19), lineColor);

    // The keys, as a chip.
    D2D1_RECT_F chip = Rect(right - 16 - chipW, cy - 16, right - 16, cy + 16);
    D2D1_ROUNDED_RECT cr = {chip, 8, 8};
    std::wstring text = sc.keys;
    UINT32 textColor = kText;
    if (recording) {
      g_brush->SetColor(Color(kAccent, 0.10f));
      g_rt->FillRoundedRectangle(cr, g_brush);
      g_brush->SetColor(Color(kAccent));
      g_rt->DrawRoundedRectangle(cr, g_brush, 1.6f);
      text = L"Press keys…";
      textColor = kAccent;
    } else {
      g_brush->SetColor(Color(0xFFFFFF, hover ? 0.08f : 0.04f));
      g_rt->FillRoundedRectangle(cr, g_brush);
      UINT32 border = sc.problem.empty() ? 0xFFFFFF : kWarn;
      g_brush->SetColor(Color(border, sc.problem.empty() ? (hover ? 0.34f : 0.14f)
                                                         : 0.7f));
      g_rt->DrawRoundedRectangle(cr, g_brush, 1.0f);
      if (text.empty()) {
        text = L"Not set";
        textColor = kDim;
      }
    }
    DrawLabel(text, g_fStrong, chip, textColor, 1.0f,
              DWRITE_TEXT_ALIGNMENT_CENTER);
    AddHit(chip, kHitShortcut, i, true);

    if (i + 1 < count) DrawDivider(x + 16, right - 16, row.bottom);
    y += rowH;
  }
  y += 4 + 14;

  DrawWrapped(caveat, g_fWrap, x, y, textW, kDim);
  y += caveatH + 22;

  D2D1_RECT_F reset = Rect(x, y, x + 170, y + 40);
  DrawButton(reset, L"Reset to defaults", L"", false, true,
             Hovered(kHitShortcutsReset));
  AddHit(reset, kHitShortcutsReset, 0, true);
  D2D1_RECT_F close = Rect(right - 110, y, right, y + 40);
  DrawButton(close, L"Close", L"Esc", false, true, Hovered(kHitCardClose));
  AddHit(close, kHitCardClose, 0, true);
}

void Paint() {
  if (!EnsureTarget()) return;
  State s = g_actions.getState();
  g_hits.clear();

  g_rt->BeginDraw();
  g_rt->SetTransform(D2D1::Matrix3x2F::Identity());
  g_rt->Clear(Color(kBg));
  DrawHeader(s);
  DrawDisplays(s);
  DrawView(s);
  DrawBehaviour(s);
  DrawActions(s);
  DrawLensEditor(s);
  if (g_card != kCardNone) {
    g_hits.clear();  // the panel underneath does not respond while it is open
    if (g_card == kCardAbout) DrawAbout();
    if (g_card == kCardShortcuts) DrawShortcuts(s);
    if (g_card == kCardPrivacy) DrawPrivacy(s);
  }
  if (g_rt->EndDraw() == (HRESULT)D2DERR_RECREATE_TARGET) DiscardTarget();
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
const Hit* HitAt(LPARAM lParam) {
  float x = GET_X_LPARAM(lParam) * 96.0f / g_dpi;
  float y = GET_Y_LPARAM(lParam) * 96.0f / g_dpi;
  for (const auto& h : g_hits) {
    if (Contains(h.rc, x, y)) return &h;
  }
  return nullptr;
}

void StopRecording() {
  if (g_recording < 0) return;
  g_recording = -1;
  g_recordError.clear();
  if (g_actions.endRecording) g_actions.endRecording();
}

void StartRecording(int key) {
  StopRecording();
  if (g_actions.beginRecording) g_actions.beginRecording();
  g_recording = key;
  g_recordError.clear();
  SetFocus(g_hwnd);
}

void OpenCard(Card card) {
  StopRecording();
  g_card = card;
  g_hoverId = kHitNone;  // the hit list changes when a card opens or closes
  if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

bool IsModifierKey(UINT vk) {
  return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
         vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LCONTROL ||
         vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU ||
         vk == VK_LWIN || vk == VK_RWIN || vk == VK_CAPITAL ||
         vk == VK_NUMLOCK || vk == VK_APPS;
}

// A key went down while a shortcut is being recorded.
void RecordKey(UINT vk) {
  if (IsModifierKey(vk)) return;  // wait for the main key
  unsigned mods = 0;
  if (GetKeyState(VK_CONTROL) < 0) mods |= hotkeys::kCtrl;
  if (GetKeyState(VK_MENU) < 0) mods |= hotkeys::kAlt;
  if (GetKeyState(VK_SHIFT) < 0) mods |= hotkeys::kShift;
  if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) {
    mods |= hotkeys::kWin;
  }
  if (mods == 0 && vk == VK_ESCAPE) {
    StopRecording();
  } else if (mods == 0 && (vk == VK_BACK || vk == VK_DELETE)) {
    g_actions.setShortcut(g_recording, 0, 0);
    StopRecording();
  } else {
    std::wstring error = g_actions.setShortcut(g_recording, mods, vk);
    if (error.empty()) {
      StopRecording();
    } else {
      g_recordError = error;  // keep listening: try other keys
    }
  }
  InvalidateRect(g_hwnd, nullptr, FALSE);
}

void Activate(int id, int arg) {
  State s = g_actions.getState();
  switch (id) {
    case kHitDisplay: g_actions.showDisplay(arg); break;
    case kHitZoom: g_actions.setZoom(arg); break;
    case kHitLens: g_actions.setLensStep(arg); break;
    case kHitLensMode: {
      viewmath::LensLayout l = s.lensLayout;
      l.fixed = arg == 1;
      g_actions.setLensLayout(l, true);
      break;
    }
    case kHitLensReset:
      g_actions.setLensLayout(viewmath::LensLayout(), true);
      break;
    case kHitLock: g_actions.toggleLock(); break;
    case kHitPip: g_actions.setPip(!s.pipOn); break;
    case kHitFollow: g_actions.setFollowCaret(!s.followCaret); break;
    case kHitAutoShow: g_actions.setAutoShow(!s.autoShow); break;
    case kHitStartup: g_actions.setStartWithWindows(!s.startWithWindows); break;
    case kHitNext: g_actions.next(); break;
    case kHitStop: g_actions.stop(); break;
    case kHitAbout: OpenCard(kCardAbout); break;
    case kHitShortcuts: OpenCard(kCardShortcuts); break;
    case kHitPrivacySettings: OpenCard(kCardPrivacy); break;
    case kHitCardClose: OpenCard(kCardNone); break;
    case kHitAboutGithub:
      ShellExecuteW(g_hwnd, L"open", kRepoUrl, nullptr, nullptr, SW_SHOWNORMAL);
      break;
    case kHitShortcut:
      if (g_recording == arg) {
        StopRecording();
      } else {
        StartRecording(arg);
      }
      break;
    case kHitShortcutsReset:
      StopRecording();
      g_actions.resetShortcuts();
      break;
    case kHitPrivacy:
    case kHitPrivacyToggle: g_actions.togglePrivacy(); break;
    case kHitPrivacyScreens: g_actions.setPrivacyScreens(arg); break;
    case kHitPrivacyMethod: g_actions.setPrivacyMethod(arg); break;
    case kHitPrivacyDim: g_actions.setPrivacyDim(!s.privacyDim); break;
    case kHitStandIn:
      if (arg == 0) {
        g_actions.setStandIn(L"");
      } else if (arg - 1 < (int)s.standIns.size()) {
        g_actions.setStandIn(s.standIns[(size_t)arg - 1].id);
      }
      break;
  }
  InvalidateRect(g_hwnd, nullptr, FALSE);
}

void SetHover(int id, int arg) {
  if (id == g_hoverId && arg == g_hoverArg) return;
  g_hoverId = id;
  g_hoverArg = arg;
  InvalidateRect(g_hwnd, nullptr, FALSE);
}

// Sizes the window so its client area is kWidth x kHeight DIPs; when
// |centreOn| is given, centres it on that monitor.
void PlaceWindow(const RECT* centreOn) {
  RECT client, window;
  GetClientRect(g_hwnd, &client);
  GetWindowRect(g_hwnd, &window);
  int frameW = (window.right - window.left) - (client.right - client.left);
  int frameH = (window.bottom - window.top) - (client.bottom - client.top);
  int w = (int)std::lround(kWidth * g_dpi / 96) + frameW;
  int h = (int)std::lround(kHeight * g_dpi / 96) + frameH;
  int x = window.left, y = window.top;
  if (centreOn) {
    POINT mid = {(centreOn->left + centreOn->right) / 2,
                 (centreOn->top + centreOn->bottom) / 2};
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(mid, MONITOR_DEFAULTTONEAREST), &mi);
    x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2;
    y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;
    if (y < mi.rcWork.top) y = mi.rcWork.top;
  }
  SetWindowPos(g_hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(hwnd, &ps);
      Paint();
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_SIZE:
      if (g_rt) {
        g_rt->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
      }
      return 0;
    case WM_DPICHANGED: {
      g_dpi = (float)HIWORD(wParam);
      DiscardTarget();  // thumbnails and target are dpi-sized
      const RECT* suggested = (const RECT*)lParam;
      SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                   suggested->right - suggested->left,
                   suggested->bottom - suggested->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      PlaceWindow(nullptr);
      UpdateThumbs();
      return 0;
    }
    case WM_TIMER:
      if (IsIconic(hwnd)) return 0;
      if (wParam == kTimerThumbs) UpdateThumbs();
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_MOUSEMOVE: {
      if (g_drag.id != kHitNone) {
        // Fractions of the glasses' size the pointer moved.
        float x = GET_X_LPARAM(lParam) * 96.0f / g_dpi;
        float y = GET_Y_LPARAM(lParam) * 96.0f / g_dpi;
        double dx = (x - g_drag.startX) / g_drag.previewW;
        double dy = (y - g_drag.startY) / g_drag.previewH;
        viewmath::LensLayout l = g_drag.start;
        if (g_drag.id == kHitLensBody) {
          l.centreX += dx;
          l.centreY += dy;
        } else if (g_drag.id == kHitLensInset ||
                   g_drag.id == kHitLensTrigger) {
          // Moving an edge towards the middle makes the distance bigger.
          const double inward[4] = {dx, dy, -dx, -dy};
          if (g_drag.id == kHitLensInset) {
            l.inset += inward[g_drag.edge];
          } else {
            l.trigger += inward[g_drag.edge];
          }
        } else {
          l.size += dx + dy;  // corner out = bigger, on both axes
        }
        g_drag.now = viewmath::ClampLensLayout(l);
        g_actions.setLensLayout(g_drag.now, false);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      if (!g_trackingLeave) {
        TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
        g_trackingLeave = TrackMouseEvent(&tme) != FALSE;
      }
      const Hit* h = HitAt(lParam);
      if (h && h->enabled) {
        SetHover(h->id, h->arg);
      } else {
        SetHover(kHitNone, 0);
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      g_trackingLeave = false;
      SetHover(kHitNone, 0);
      return 0;
    case WM_LBUTTONDOWN: {
      const Hit* h = HitAt(lParam);
      if (h && h->enabled &&
          (h->id == kHitLensBody || h->id == kHitLensHandle ||
           h->id == kHitLensInset || h->id == kHitLensTrigger)) {
        g_drag.id = h->id;
        g_drag.edge = h->arg;
        g_drag.startX = GET_X_LPARAM(lParam) * 96.0f / g_dpi;
        g_drag.startY = GET_Y_LPARAM(lParam) * 96.0f / g_dpi;
        g_drag.previewW = g_lensPreview.right - g_lensPreview.left;
        g_drag.previewH = g_lensPreview.bottom - g_lensPreview.top;
        g_drag.start = g_actions.getState().lensLayout;
        g_drag.now = g_drag.start;
        g_actions.setLensLayout(g_drag.now, false);  // show it on the glasses
        SetCapture(hwnd);
        return 0;
      }
      g_pressId = h && h->enabled ? h->id : kHitNone;
      g_pressArg = h ? h->arg : 0;
      SetCapture(hwnd);
      return 0;
    }
    case WM_LBUTTONUP: {
      ReleaseCapture();
      if (g_drag.id != kHitNone) {
        g_actions.setLensLayout(g_drag.now, true);
        g_drag.id = kHitNone;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      const Hit* h = HitAt(lParam);
      if (h && h->enabled && h->id == g_pressId && h->arg == g_pressArg) {
        Activate(h->id, h->arg);
      }
      g_pressId = kHitNone;
      return 0;
    }
    case WM_SETCURSOR:
      if (LOWORD(lParam) == HTCLIENT) {
        int id = g_drag.id != kHitNone ? g_drag.id : g_hoverId;
        int arg = g_drag.id != kHitNone ? g_drag.edge : g_hoverArg;
        LPCWSTR shape = IDC_ARROW;
        if (id == kHitLensInset || id == kHitLensTrigger) {
          shape = (arg == 0 || arg == 2) ? IDC_SIZEWE : IDC_SIZENS;
        } else if (id == kHitLensHandle) {
          shape = IDC_SIZENWSE;
        } else if (id == kHitLensBody) {
          shape = IDC_SIZEALL;
        } else if (id != kHitNone) {
          shape = IDC_HAND;
        }
        SetCursor(LoadCursorW(nullptr, shape));
        return TRUE;
      }
      break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
      // While recording, every key is for the shortcut: Alt+F4 or F10 must
      // not reach Windows' own handling.
      if (g_recording >= 0) {
        RecordKey((UINT)wParam);
        return 0;
      }
      if (msg == WM_KEYDOWN && wParam == VK_ESCAPE) {
        if (g_card != kCardNone) {
          OpenCard(kCardNone);
        } else {
          Close();
        }
        return 0;
      }
      break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
    case WM_CHAR:
    case WM_SYSCHAR:
      if (g_recording >= 0) return 0;
      break;
    case WM_SYSCOMMAND:
      if (g_recording >= 0 && (wParam & 0xFFF0) == SC_KEYMENU) return 0;
      break;
    case WM_ACTIVATE:
      // Recording only while the panel has the keyboard: elsewhere the
      // hotkeys must work.
      if (LOWORD(wParam) == WA_INACTIVE && g_recording >= 0) {
        StopRecording();
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      break;
    case WM_CLOSE:
      Close();
      return 0;
    case WM_DESTROY:
      KillTimer(hwnd, kTimerRepaint);
      KillTimer(hwnd, kTimerThumbs);
      DiscardTarget();
      g_hwnd = nullptr;
      g_hits.clear();
      g_hoverId = kHitNone;
      g_trackingLeave = false;
      g_drag.id = kHitNone;
      StopRecording();
      g_card = kCardNone;
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void Init(HINSTANCE instance, HICON icon, const Actions& actions) {
  g_instance = instance;
  g_icon = icon;
  g_actions = actions;

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = instance;
  wc.hIcon = icon;
  wc.hIconSm = icon;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = kClassName;
  RegisterClassExW(&wc);
}

void Show(const RECT& monitorBounds) {
  if (g_hwnd) {
    if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
    FollowMonitor(monitorBounds);
    SetForegroundWindow(g_hwnd);
    return;
  }
  if (!CreateFactories()) return;

  g_hwnd = CreateWindowExW(
      0, kClassName, L"Viture HUD",
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT,
      CW_USEDEFAULT, 800, 800, nullptr, nullptr, g_instance, nullptr);
  if (!g_hwnd) return;

  // Dark title bar to match (attribute 20 on Windows 10 20H1+, 19 before).
  BOOL dark = TRUE;
  if (FAILED(DwmSetWindowAttribute(g_hwnd, 20, &dark, sizeof(dark)))) {
    DwmSetWindowAttribute(g_hwnd, 19, &dark, sizeof(dark));
  }

  g_dpi = (float)DpiForWindow(g_hwnd);
  PlaceWindow(&monitorBounds);
  // The target monitor may have a different dpi than where the window was
  // created; re-check after moving.
  float placedDpi = (float)DpiForWindow(g_hwnd);
  if (placedDpi != g_dpi) {
    g_dpi = placedDpi;
    PlaceWindow(&monitorBounds);
  }

  UpdateThumbs();
  ShowWindow(g_hwnd, SW_SHOW);
  SetForegroundWindow(g_hwnd);
  SetTimer(g_hwnd, kTimerRepaint, kRepaintMs, nullptr);
  SetTimer(g_hwnd, kTimerThumbs, kThumbsMs, nullptr);
}

void ShowAbout(const RECT& monitorBounds) {
  Show(monitorBounds);
  if (g_hwnd) OpenCard(kCardAbout);
}

void ShowShortcuts(const RECT& monitorBounds) {
  Show(monitorBounds);
  if (g_hwnd) OpenCard(kCardShortcuts);
}

void ShowPrivacy(const RECT& monitorBounds) {
  Show(monitorBounds);
  if (g_hwnd) OpenCard(kCardPrivacy);
}

void Close() {
  if (g_hwnd) DestroyWindow(g_hwnd);
}

bool IsOpen() { return g_hwnd != nullptr; }

HWND Window() { return g_hwnd; }

void Invalidate() {
  if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

void FollowMonitor(const RECT& monitorBounds) {
  if (!g_hwnd || IsIconic(g_hwnd)) return;
  POINT centre = {(monitorBounds.left + monitorBounds.right) / 2,
                  (monitorBounds.top + monitorBounds.bottom) / 2};
  HMONITOR target = MonitorFromPoint(centre, MONITOR_DEFAULTTONEAREST);
  HMONITOR current = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
  if (target == current) return;

  MONITORINFO from = {}, to = {};
  from.cbSize = to.cbSize = sizeof(MONITORINFO);
  if (!GetMonitorInfoW(current, &from) || !GetMonitorInfoW(target, &to)) return;
  RECT wr;
  GetWindowRect(g_hwnd, &wr);
  int w = wr.right - wr.left;
  int h = wr.bottom - wr.top;

  // Same relative spot: 0 = against the left/top of the work area, 1 = against
  // the right/bottom.
  auto fraction = [](LONG pos, LONG start, LONG end, int size) {
    int room = (int)(end - start) - size;
    if (room <= 0) return 0.5;
    double f = (double)(pos - start) / room;
    return f < 0 ? 0.0 : (f > 1 ? 1.0 : f);
  };
  auto place = [](double f, LONG start, LONG end, int size) {
    int room = (int)(end - start) - size;
    return room <= 0 ? (int)start : (int)start + (int)std::lround(f * room);
  };
  double fx = fraction(wr.left, from.rcWork.left, from.rcWork.right, w);
  double fy = fraction(wr.top, from.rcWork.top, from.rcWork.bottom, h);
  int x = place(fx, to.rcWork.left, to.rcWork.right, w);
  int y = place(fy, to.rcWork.top, to.rcWork.bottom, h);
  // A dpi change on the new monitor resizes the window via WM_DPICHANGED.
  SetWindowPos(g_hwnd, nullptr, x, y, 0, 0,
               SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

}  // namespace panel
