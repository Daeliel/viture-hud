// Copyright (C) 2026 Jurica Dujmovic
// SPDX-License-Identifier: GPL-3.0-or-later
//
// viewer.cpp - the live view on the glasses: capture, rendering, smooth
// panning, picture-in-picture and the edge lens. See viewer.h.

#include "viewer.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <oleacc.h>

#include "view_math.h"

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

namespace viewer {

// ---------------------------------------------------------------------------
// Tracing: appends to %LOCALAPPDATA%\VitureHUD\viture_hud.log. The log starts
// over once it passes kMaxLogBytes, so it cannot grow without bound.
// ---------------------------------------------------------------------------
constexpr long kMaxLogBytes = 1024 * 1024;

void DebugTrace(const wchar_t* fmt, ...) {
  wchar_t buf[512];
  va_list args;
  va_start(args, fmt);
  _vsnwprintf_s(buf, _TRUNCATE, fmt, args);
  va_end(args);

  wchar_t path[MAX_PATH] = {0};
  DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return;
  wcscat_s(path, MAX_PATH, L"\\VitureHUD");
  CreateDirectoryW(path, nullptr);  // fails harmlessly when it exists
  wcscat_s(path, MAX_PATH, L"\\viture_hud.log");

  FILE* f = nullptr;
  _wfopen_s(&f, path, L"a");
  if (!f) return;
  fseek(f, 0, SEEK_END);
  if (ftell(f) > kMaxLogBytes) {
    fclose(f);
    f = nullptr;
    _wfopen_s(&f, path, L"w");
    if (!f) return;
  }
  fwprintf(f, L"%ls\n", buf);
  fclose(f);
}

namespace {

constexpr wchar_t kWindowClass[] = L"VitureViewerWindow";

// How often the text cursor is looked up, and how long to wait before trying
// Desktop Duplication again after it failed (e.g. during a UAC prompt).
constexpr DWORD kCaretPollMs = 40;
constexpr ULONGLONG kDupRetryMs = 1000;

template <typename T>
void SafeRelease(T*& p) {
  if (p) {
    p->Release();
    p = nullptr;
  }
}

// ---------------------------------------------------------------------------
// State shared between the UI thread and the render thread. The UI thread
// only writes requests here; every D3D and GDI resource belongs to the render
// thread, so nothing can be freed while a frame is using it.
// ---------------------------------------------------------------------------
struct Shared {
  std::wstring device;      // monitor being shown (\\.\DISPLAYn)
  std::wstring label;       // its name, for the status bar
  HWND lockHwnd = nullptr;  // window the view is locked to, or null
  std::wstring lockTitle;
  int zoomStep = 0;         // index into viewmath::kZoomSteps
  int savedZoomStep = 0;    // zoom to return to when the lock ends
  unsigned generation = 0;  // bumped on every change that should re-centre
  unsigned reload = 0;      // bumped when the display layout changed
  std::wstring pipDevice;   // monitor in the picture-in-picture, empty = off
  std::wstring pipLabel;
  double lensZoom = 1.0;    // edge lens magnification, 0 = off
  viewmath::LensLayout lensLayout;
  ULONGLONG lensPreviewUntil = 0;  // show the lens regardless, while adjusting
  ULONGLONG barShownAt = 0;
};

std::mutex g_lock;
Shared g_shared;

// The text cursor of the focused app, published by the caret thread.
// changedAt stays 0 until the caret actually moves, so a caret that merely
// exists never steals the view from the mouse.
struct Caret {
  bool valid = false;
  POINT pt = {0, 0};
  ULONGLONG changedAt = 0;
};

std::mutex g_caretLock;
Caret g_caret;

std::atomic<bool> g_running{false};
std::atomic<bool> g_stop{false};
std::atomic<bool> g_followCaret{true};
std::thread g_renderThread;

// Published by the render thread for the control panel.
std::atomic<int> g_statFps{0};
std::atomic<bool> g_statDup{false};
std::atomic<double> g_statScale{1.0};
std::thread g_caretThread;
HWND g_window = nullptr;

// ---------------------------------------------------------------------------
// Geometry helpers
// ---------------------------------------------------------------------------
RECT MonitorRect(const std::wstring& device) {
  RECT out = {0, 0, 0, 0};
  DEVMODEW dm = {};
  dm.dmSize = sizeof(dm);
  if (EnumDisplaySettingsW(device.c_str(), ENUM_CURRENT_SETTINGS, &dm)) {
    out.left = dm.dmPosition.x;
    out.top = dm.dmPosition.y;
    out.right = dm.dmPosition.x + (LONG)dm.dmPelsWidth;
    out.bottom = dm.dmPosition.y + (LONG)dm.dmPelsHeight;
  }
  return out;
}

// ---------------------------------------------------------------------------
// Text cursor lookup
//
// Classic Win32 apps report their caret through GetGUIThreadInfo. Chromium,
// Electron (VS Code) and many others do not, but expose it to screen readers
// as OBJID_CARET, so that is the fallback. Apps that do neither are simply not
// followed while typing.
// ---------------------------------------------------------------------------
bool ReadCaret(POINT* out) {
  HWND fg = GetForegroundWindow();
  if (!fg || fg == g_window) return false;

  GUITHREADINFO gti = {};
  gti.cbSize = sizeof(gti);
  DWORD tid = GetWindowThreadProcessId(fg, nullptr);
  if (!GetGUIThreadInfo(tid, &gti)) return false;

  if (gti.hwndCaret && gti.rcCaret.bottom > gti.rcCaret.top) {
    POINT p = {gti.rcCaret.left, (gti.rcCaret.top + gti.rcCaret.bottom) / 2};
    if (!ClientToScreen(gti.hwndCaret, &p)) return false;
    *out = p;
    return true;
  }

  HWND target = gti.hwndFocus ? gti.hwndFocus : fg;
  IAccessible* acc = nullptr;
  if (FAILED(AccessibleObjectFromWindow(target, (DWORD)OBJID_CARET,
                                        __uuidof(IAccessible),
                                        (void**)&acc)) ||
      !acc) {
    return false;
  }
  long x = 0, y = 0, w = 0, h = 0;
  VARIANT self;
  VariantInit(&self);
  self.vt = VT_I4;
  self.lVal = CHILDID_SELF;
  HRESULT hr = acc->accLocation(&x, &y, &w, &h, self);
  acc->Release();
  if (FAILED(hr) || (x == 0 && y == 0 && w == 0 && h == 0)) return false;
  out->x = x;
  out->y = y + h / 2;
  return true;
}

void CaretLoop() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  POINT last = {0, 0};
  bool had = false;
  while (!g_stop.load()) {
    POINT pt;
    bool ok = g_followCaret.load() && ReadCaret(&pt);
    {
      std::lock_guard<std::mutex> hold(g_caretLock);
      if (ok) {
        bool moved = had && (std::abs(pt.x - last.x) > 1 ||
                             std::abs(pt.y - last.y) > 1);
        g_caret.valid = true;
        g_caret.pt = pt;
        if (moved) g_caret.changedAt = GetTickCount64();
      } else {
        g_caret.valid = false;
      }
    }
    if (ok) last = pt;
    had = ok;
    Sleep(kCaretPollMs);
  }
  CoUninitialize();
}

// ---------------------------------------------------------------------------
// Renderer
//
// Capture: Desktop Duplication hands us the monitor's image as a GPU texture
// at its full refresh rate. When it is unavailable (HDR, rotated monitor,
// secure desktop) we fall back to a GDI BitBlt, which is slow on big monitors
// but always works.
//
// Drawing: the captured monitor is one texture; the visible slice is drawn as
// a single quad. The cursor and status bar are then drawn with GDI straight
// onto the swap chain's back buffer, which is created GDI-compatible for that.
// ---------------------------------------------------------------------------
const char kShaderSource[] = R"(
cbuffer Quad : register(b0) {
  float4 dst;  // left, top, right, bottom in clip space
  float4 uv;   // left, top, right, bottom in texture space
};
struct VsOut {
  float4 pos : SV_Position;
  float2 tc : TEXCOORD0;
};
VsOut vs_main(uint id : SV_VertexID) {
  float2 t = float2(id & 1, id >> 1);
  VsOut o;
  o.pos = float4(lerp(dst.x, dst.z, t.x), lerp(dst.y, dst.w, t.y), 0, 1);
  o.tc = lerp(uv.xy, uv.zw, t);
  return o;
}
Texture2D tex : register(t0);
SamplerState samp : register(s0);
float4 ps_main(VsOut i) : SV_Target {
  return float4(tex.Sample(samp, i.tc).rgb, 1);
}
)";

struct QuadConstants {
  float dst[4];
  float uv[4];
};

// One captured monitor: its geometry and its image as a texture.
struct Source {
  std::wstring device;  // empty = none
  RECT monRect = {0, 0, 0, 0};
  int monW = 0;
  int monH = 0;
  ID3D11Texture2D* content = nullptr;
  ID3D11ShaderResourceView* contentSrv = nullptr;
  bool contentReady = false;
  bool mipsDirty = false;

  IDXGIOutput1* output = nullptr;
  IDXGIOutputDuplication* dup = nullptr;
  bool dupUnsupported = false;
  ULONGLONG dupRetryAt = 0;
  HRESULT dupLastError = S_OK;  // logged once, not on every retry

  HDC gdiDC = nullptr;  // fallback capture
  HBITMAP gdiBmp = nullptr;
  HGDIOBJ gdiOld = nullptr;
  void* gdiBits = nullptr;
  DWORD gdiIntervalMs = 0;  // 0 = every frame
  ULONGLONG gdiNextAt = 0;
};

struct Renderer {
  // Device and output window.
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  LUID adapterLuid = {0, 0};
  IDXGISwapChain* swap = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
  int viewW = 0;
  int viewH = 0;
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  ID3D11Buffer* cb = nullptr;
  ID3D11SamplerState* pointSamp = nullptr;   // pixel-exact zoom
  ID3D11SamplerState* linearSamp = nullptr;  // in-between zoom
  ID3D11SamplerState* mipSamp = nullptr;     // shrinking (fit)
  ID3D11RasterizerState* raster = nullptr;

  // The monitor being shown, and the one in the picture-in-picture.
  Source main;
  Source pip;

  // Overlay.
  HDC barDC = nullptr;
  HBITMAP barBmp = nullptr;
  HGDIOBJ barOld = nullptr;
  int barW = 0;
  HFONT barFont = nullptr;
  HFONT pipFont = nullptr;
  HCURSOR hotspotCursor = nullptr;
  POINT hotspot = {0, 0};

  // View.
  bool lensShowing = false;
  double panX = 0;
  double panY = 0;
  unsigned seenGeneration = ~0u;
  unsigned seenReload = 0;
  POINT lastMouse = {-100000, -100000};
  ULONGLONG mouseMovedAt = 0;
  LARGE_INTEGER freq = {};
  LARGE_INTEGER lastTick = {};
  int fps = 0;
  int fpsFrames = 0;
  ULONGLONG fpsMark = 0;
};

Renderer r;

bool CompileShader(const char* entry, const char* target, ID3DBlob** out) {
  ID3DBlob* errors = nullptr;
  HRESULT hr = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, nullptr,
                          nullptr, nullptr, entry, target, 0, 0, out, &errors);
  if (FAILED(hr)) {
    if (errors) {
      DebugTrace(L"shader %hs: %hs", entry,
                 (const char*)errors->GetBufferPointer());
    }
    SafeRelease(errors);
    return false;
  }
  SafeRelease(errors);
  return true;
}

void ReleaseSource(Source& src) {
  SafeRelease(src.dup);
  SafeRelease(src.output);
  SafeRelease(src.contentSrv);
  SafeRelease(src.content);
  src.contentReady = false;
  src.device.clear();
  if (src.gdiDC) {
    SelectObject(src.gdiDC, src.gdiOld);
    DeleteObject(src.gdiBmp);
    DeleteDC(src.gdiDC);
    src.gdiDC = nullptr;
    src.gdiBits = nullptr;
  }
}

void ReleaseDevice() {
  ReleaseSource(r.main);
  ReleaseSource(r.pip);
  SafeRelease(r.raster);
  SafeRelease(r.mipSamp);
  SafeRelease(r.linearSamp);
  SafeRelease(r.pointSamp);
  SafeRelease(r.cb);
  SafeRelease(r.ps);
  SafeRelease(r.vs);
  SafeRelease(r.rtv);
  SafeRelease(r.swap);
  if (r.ctx) r.ctx->ClearState();
  SafeRelease(r.ctx);
  SafeRelease(r.dev);
  r.adapterLuid = {0, 0};
}

void ReleaseOverlay() {
  if (r.barDC) {
    SelectObject(r.barDC, r.barOld);
    DeleteObject(r.barBmp);
    DeleteDC(r.barDC);
    r.barDC = nullptr;
    r.barW = 0;
  }
  if (r.barFont) {
    DeleteObject(r.barFont);
    r.barFont = nullptr;
  }
  if (r.pipFont) {
    DeleteObject(r.pipFont);
    r.pipFont = nullptr;
  }
}

// Finds the DXGI adapter and output that drive |device|.
bool FindOutput(const std::wstring& device, IDXGIAdapter1** adapterOut,
                IDXGIOutput1** outputOut) {
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) {
    return false;
  }
  bool found = false;
  IDXGIAdapter1* adapter = nullptr;
  for (UINT a = 0; !found && factory->EnumAdapters1(a, &adapter) !=
                                 DXGI_ERROR_NOT_FOUND;
       ++a) {
    IDXGIOutput* output = nullptr;
    for (UINT o = 0; !found && adapter->EnumOutputs(o, &output) !=
                                   DXGI_ERROR_NOT_FOUND;
         ++o) {
      DXGI_OUTPUT_DESC desc;
      if (SUCCEEDED(output->GetDesc(&desc)) && device == desc.DeviceName &&
          SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput1),
                                           (void**)outputOut))) {
        *adapterOut = adapter;
        adapter->AddRef();
        found = true;
      }
      output->Release();
    }
    adapter->Release();
  }
  factory->Release();
  return found;
}

bool CreateDevice(IDXGIAdapter1* adapter) {
  HRESULT hr = D3D11CreateDevice(
      adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
      nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
      D3D11_SDK_VERSION, &r.dev, nullptr, &r.ctx);
  if (FAILED(hr)) {
    DebugTrace(L"D3D11CreateDevice failed 0x%lX", (unsigned long)hr);
    return false;
  }
  if (adapter) {
    DXGI_ADAPTER_DESC1 desc;
    if (SUCCEEDED(adapter->GetDesc1(&desc))) r.adapterLuid = desc.AdapterLuid;
  }

  // Swap chain on the glasses window. Blt model with one buffer, so the back
  // buffer can be drawn on with GDI before presenting.
  IDXGIDevice* dxgiDev = nullptr;
  IDXGIAdapter* dxgiAdapter = nullptr;
  IDXGIFactory* factory = nullptr;
  r.dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev);
  if (dxgiDev) dxgiDev->GetAdapter(&dxgiAdapter);
  if (dxgiAdapter) {
    dxgiAdapter->GetParent(__uuidof(IDXGIFactory), (void**)&factory);
  }
  if (factory) {
    RECT client;
    GetClientRect(g_window, &client);
    r.viewW = client.right - client.left;
    r.viewH = client.bottom - client.top;

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferDesc.Width = (UINT)r.viewW;
    sd.BufferDesc.Height = (UINT)r.viewH;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 1;
    sd.OutputWindow = g_window;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE;
    hr = factory->CreateSwapChain(r.dev, &sd, &r.swap);
    if (SUCCEEDED(hr)) {
      factory->MakeWindowAssociation(g_window, DXGI_MWA_NO_ALT_ENTER);
    } else {
      DebugTrace(L"CreateSwapChain failed 0x%lX", (unsigned long)hr);
    }
  }
  SafeRelease(factory);
  SafeRelease(dxgiAdapter);
  SafeRelease(dxgiDev);
  if (!r.swap) return false;

  ID3D11Texture2D* back = nullptr;
  r.swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back);
  if (!back) return false;
  r.dev->CreateRenderTargetView(back, nullptr, &r.rtv);
  back->Release();
  if (!r.rtv) return false;

  ID3DBlob* vsBlob = nullptr;
  ID3DBlob* psBlob = nullptr;
  if (!CompileShader("vs_main", "vs_4_0", &vsBlob) ||
      !CompileShader("ps_main", "ps_4_0", &psBlob)) {
    SafeRelease(vsBlob);
    SafeRelease(psBlob);
    return false;
  }
  r.dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                            nullptr, &r.vs);
  r.dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                           nullptr, &r.ps);
  vsBlob->Release();
  psBlob->Release();

  D3D11_BUFFER_DESC bd = {};
  bd.ByteWidth = sizeof(QuadConstants);
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  r.dev->CreateBuffer(&bd, nullptr, &r.cb);

  D3D11_SAMPLER_DESC sam = {};
  sam.AddressU = sam.AddressV = sam.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sam.ComparisonFunc = D3D11_COMPARISON_NEVER;
  sam.MaxLOD = 0;
  sam.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  r.dev->CreateSamplerState(&sam, &r.pointSamp);
  sam.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  r.dev->CreateSamplerState(&sam, &r.linearSamp);
  sam.MaxLOD = D3D11_FLOAT32_MAX;
  r.dev->CreateSamplerState(&sam, &r.mipSamp);

  D3D11_RASTERIZER_DESC rd = {};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = TRUE;
  r.dev->CreateRasterizerState(&rd, &r.raster);

  return r.vs && r.ps && r.cb && r.pointSamp && r.linearSamp && r.mipSamp &&
         r.raster;
}

void StartDuplication(Source& src) {
  if (!src.output || !r.dev) return;
  HRESULT hr = src.output->DuplicateOutput(r.dev, &src.dup);
  if (FAILED(hr)) {
    // Retried every second (e.g. 0x80070005 while the lock screen or a UAC
    // prompt is up): log a failure once, not on every retry.
    if (hr != src.dupLastError) {
      DebugTrace(L"DuplicateOutput %ls failed 0x%lX, using GDI",
                 src.device.c_str(), (unsigned long)hr);
    }
    src.dupLastError = hr;
    src.dup = nullptr;
    src.dupRetryAt = GetTickCount64() + kDupRetryMs;
    return;
  }
  if (FAILED(src.dupLastError)) {
    DebugTrace(L"Duplication of %ls works again", src.device.c_str());
  }
  src.dupLastError = S_OK;
  DXGI_OUTDUPL_DESC desc;
  src.dup->GetDesc(&desc);
  if (desc.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
      desc.Rotation != DXGI_MODE_ROTATION_IDENTITY) {
    // HDR (float) or rotated output: the copy below would not match.
    DebugTrace(L"Duplication of %ls unsupported (fmt=%d rot=%d), using GDI",
               src.device.c_str(), (int)desc.ModeDesc.Format,
               (int)desc.Rotation);
    SafeRelease(src.dup);
    src.dupUnsupported = true;
  } else if ((int)desc.ModeDesc.Width != src.monW ||
             (int)desc.ModeDesc.Height != src.monH) {
    // The monitor is changing resolution (a virtual display starting up at
    // 800x600, say): not a lasting problem, so try again shortly.
    DebugTrace(L"Duplication of %ls is %ux%u, expected %dx%d: retrying",
               src.device.c_str(), desc.ModeDesc.Width, desc.ModeDesc.Height,
               src.monW, src.monH);
    SafeRelease(src.dup);
    src.dupRetryAt = GetTickCount64() + kDupRetryMs;
  }
}

// Fills in |src| for |device| on the current D3D device. Takes ownership of
// |output|; without one (another adapter), only GDI capture is used.
bool InitSource(Source& src, const std::wstring& device, IDXGIOutput1* output) {
  src.device = device;
  src.output = output;
  src.monRect = MonitorRect(device);
  src.monW = src.monRect.right - src.monRect.left;
  src.monH = src.monRect.bottom - src.monRect.top;
  src.dupUnsupported = output == nullptr;
  src.dupRetryAt = 0;
  src.dupLastError = S_OK;
  src.gdiNextAt = 0;
  if (src.monW <= 0 || src.monH <= 0) return false;

  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)src.monW;
  td.Height = (UINT)src.monH;
  td.MipLevels = 0;  // full chain, for smooth shrinking
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
  if (FAILED(r.dev->CreateTexture2D(&td, nullptr, &src.content)) ||
      FAILED(r.dev->CreateShaderResourceView(src.content, nullptr,
                                             &src.contentSrv))) {
    return false;
  }

  StartDuplication(src);
  DebugTrace(L"Source %ls %dx%d at %ld,%ld via %ls", device.c_str(), src.monW,
             src.monH, src.monRect.left, src.monRect.top,
             src.dup ? L"duplication" : L"GDI");
  return true;
}

LUID AdapterLuid(IDXGIAdapter1* adapter) {
  LUID luid = {0, 0};
  DXGI_ADAPTER_DESC1 desc;
  if (adapter && SUCCEEDED(adapter->GetDesc1(&desc))) luid = desc.AdapterLuid;
  return luid;
}

bool SameLuid(const LUID& a, const LUID& b) {
  return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

// Points the main view at |device|, recreating the D3D device when the
// monitor is driven by a different graphics adapter.
bool SetupSource(const std::wstring& device) {
  ReleaseSource(r.main);
  IDXGIAdapter1* adapter = nullptr;
  IDXGIOutput1* output = nullptr;
  FindOutput(device, &adapter, &output);
  if (!r.dev || !SameLuid(AdapterLuid(adapter), r.adapterLuid)) {
    ReleaseDevice();  // also drops the PiP; it is set up again next frame
    if (!CreateDevice(adapter)) {
      SafeRelease(adapter);
      SafeRelease(output);
      ReleaseDevice();
      return false;
    }
  }
  SafeRelease(adapter);
  return InitSource(r.main, device, output);
}

// Points the picture-in-picture at |device| (empty = off). A monitor on
// another graphics adapter is captured with GDI, a few times a second.
void SetupPip(const std::wstring& device) {
  ReleaseSource(r.pip);
  if (device.empty() || !r.dev) return;
  IDXGIAdapter1* adapter = nullptr;
  IDXGIOutput1* output = nullptr;
  FindOutput(device, &adapter, &output);
  if (!SameLuid(AdapterLuid(adapter), r.adapterLuid)) SafeRelease(output);
  SafeRelease(adapter);
  if (!InitSource(r.pip, device, output)) {
    DebugTrace(L"PiP: could not set up %ls", device.c_str());
  }
  r.pip.device = device;  // even on failure, so it is not retried every frame
  r.pip.gdiIntervalMs = 250;
}

void CaptureGdi(Source& src) {
  if (src.gdiIntervalMs) {
    ULONGLONG now = GetTickCount64();
    if (now < src.gdiNextAt) return;
    src.gdiNextAt = now + src.gdiIntervalMs;
  }
  if (!src.gdiDC) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = src.monW;
    bi.bmiHeader.biHeight = -src.monH;  // top-down, like the texture
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    src.gdiDC = CreateCompatibleDC(nullptr);
    src.gdiBmp = CreateDIBSection(src.gdiDC, &bi, DIB_RGB_COLORS, &src.gdiBits,
                                  nullptr, 0);
    if (!src.gdiBmp) {
      DeleteDC(src.gdiDC);
      src.gdiDC = nullptr;
      return;
    }
    src.gdiOld = SelectObject(src.gdiDC, src.gdiBmp);
  }
  HDC screen = GetDC(nullptr);
  if (!screen) return;
  BitBlt(src.gdiDC, 0, 0, src.monW, src.monH, screen, src.monRect.left,
         src.monRect.top, SRCCOPY);
  ReleaseDC(nullptr, screen);
  GdiFlush();
  r.ctx->UpdateSubresource(src.content, 0, nullptr, src.gdiBits,
                           (UINT)src.monW * 4, 0);
  src.contentReady = true;
  src.mipsDirty = true;
}

void Capture(Source& src) {
  if (!src.content) return;
  if (!src.dup && !src.dupUnsupported && GetTickCount64() >= src.dupRetryAt) {
    StartDuplication(src);
  }
  if (!src.dup) {
    CaptureGdi(src);
    return;
  }

  DXGI_OUTDUPL_FRAME_INFO info;
  IDXGIResource* res = nullptr;
  HRESULT hr = src.dup->AcquireNextFrame(0, &info, &res);
  if (hr == DXGI_ERROR_WAIT_TIMEOUT) return;  // screen unchanged
  if (FAILED(hr)) {
    // Mode change, UAC prompt, full-screen game...: start over shortly.
    DebugTrace(L"AcquireNextFrame %ls 0x%lX, re-creating duplication",
               src.device.c_str(), (unsigned long)hr);
    SafeRelease(src.dup);
    src.dupRetryAt = GetTickCount64() + 200;
    return;
  }
  if (info.LastPresentTime.QuadPart != 0 || !src.contentReady) {
    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D),
                                      (void**)&tex))) {
      r.ctx->CopySubresourceRegion(src.content, 0, 0, 0, 0, tex, 0, nullptr);
      tex->Release();
      src.contentReady = true;
      src.mipsDirty = true;
    }
  }
  res->Release();
  src.dup->ReleaseFrame();
}

// Draws |srv| (its uv rectangle) into the view rectangle x0,y0 - x1,y1.
void DrawTexture(ID3D11ShaderResourceView* srv, double x0, double y0,
                 double x1, double y1, const float uv[4],
                 ID3D11SamplerState* sampler) {
  QuadConstants q;
  q.dst[0] = (float)(x0 / r.viewW * 2 - 1);
  q.dst[1] = (float)(1 - y0 / r.viewH * 2);
  q.dst[2] = (float)(x1 / r.viewW * 2 - 1);
  q.dst[3] = (float)(1 - y1 / r.viewH * 2);
  for (int i = 0; i < 4; ++i) q.uv[i] = uv[i];
  D3D11_MAPPED_SUBRESOURCE mapped;
  if (SUCCEEDED(r.ctx->Map(r.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    memcpy(mapped.pData, &q, sizeof(q));
    r.ctx->Unmap(r.cb, 0);
  }
  D3D11_VIEWPORT vp = {0, 0, (float)r.viewW, (float)r.viewH, 0, 1};
  r.ctx->RSSetViewports(1, &vp);
  r.ctx->RSSetState(r.raster);
  r.ctx->IASetInputLayout(nullptr);
  r.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  r.ctx->VSSetShader(r.vs, nullptr, 0);
  r.ctx->VSSetConstantBuffers(0, 1, &r.cb);
  r.ctx->PSSetShader(r.ps, nullptr, 0);
  r.ctx->PSSetShaderResources(0, 1, &srv);
  r.ctx->PSSetSamplers(0, 1, &sampler);
  r.ctx->Draw(4, 0);
  ID3D11ShaderResourceView* none = nullptr;
  r.ctx->PSSetShaderResources(0, 1, &none);
}

// Frame, name and a small cursor for the picture-in-picture.
void DrawPipOverlay(HDC hdc, const viewmath::PipRect& pip,
                    const std::wstring& label) {
  HBRUSH frame = CreateSolidBrush(RGB(57, 255, 136));
  RECT outer = {pip.x - 1, pip.y - 1, pip.x + pip.width + 1,
                pip.y + pip.height + 1};
  FrameRect(hdc, &outer, frame);
  DeleteObject(frame);

  if (!label.empty()) {
    if (!r.pipFont) {
      r.pipFont = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                              DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                              L"Segoe UI");
    }
    HGDIOBJ of = SelectObject(hdc, r.pipFont);
    SIZE ts = {0, 0};
    GetTextExtentPoint32W(hdc, label.c_str(), (int)label.size(), &ts);
    RECT tag = {pip.x, pip.y, pip.x + ts.cx + 16, pip.y + ts.cy + 6};
    if (tag.right > pip.x + pip.width) tag.right = pip.x + pip.width;
    HBRUSH bg = CreateSolidBrush(RGB(8, 12, 16));
    FillRect(hdc, &tag, bg);
    DeleteObject(bg);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(230, 237, 243));
    DrawTextW(hdc, label.c_str(), -1, &tag,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(hdc, of);
  }

  // Where the mouse is, when it is on the monitor in the PiP.
  CURSORINFO ci = {};
  ci.cbSize = sizeof(ci);
  if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && ci.hCursor &&
      PtInRect(&r.pip.monRect, ci.ptScreenPos) && r.pip.monW > 0) {
    double k = (double)pip.width / r.pip.monW;
    int size = GetSystemMetrics(SM_CXCURSOR) * 2 / 3;
    int x = pip.x + (int)((ci.ptScreenPos.x - r.pip.monRect.left) * k);
    int y = pip.y + (int)((ci.ptScreenPos.y - r.pip.monRect.top) * k);
    DrawIconEx(hdc, x, y, ci.hCursor, size, size, 0, nullptr, DI_NORMAL);
  }
}

void DrawCursor(HDC hdc, const RECT& contentScreen, double scale) {
  CURSORINFO ci = {};
  ci.cbSize = sizeof(ci);
  if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) {
    return;
  }
  if (!PtInRect(&contentScreen, ci.ptScreenPos)) return;

  // Draw the cursor so its hotspot (not its corner) sits on the point.
  if (ci.hCursor != r.hotspotCursor) {
    r.hotspotCursor = ci.hCursor;
    r.hotspot = {0, 0};
    ICONINFO ii = {};
    if (GetIconInfo(ci.hCursor, &ii)) {
      r.hotspot = {(LONG)ii.xHotspot, (LONG)ii.yHotspot};
      if (ii.hbmMask) DeleteObject(ii.hbmMask);
      if (ii.hbmColor) DeleteObject(ii.hbmColor);
    }
  }
  double x = (ci.ptScreenPos.x - contentScreen.left) * scale -
             std::round(r.panX) - r.hotspot.x;
  double y = (ci.ptScreenPos.y - contentScreen.top) * scale -
             std::round(r.panY) - r.hotspot.y;
  DrawIconEx(hdc, (int)x, (int)y, ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
}

// Frame and cursor for the edge lens. |src| is in content pixels, starting at
// |contentScreen|'s corner.
void DrawLensOverlay(HDC hdc, const viewmath::LensRect& lens,
                     const viewmath::LensSource& src, double lensScale,
                     const RECT& contentScreen) {
  HBRUSH dark = CreateSolidBrush(RGB(0, 0, 0));
  HBRUSH accent = CreateSolidBrush(RGB(57, 255, 136));
  RECT shadow = {lens.x - 3, lens.y - 3, lens.x + lens.width + 3,
                 lens.y + lens.height + 3};
  FrameRect(hdc, &shadow, dark);
  for (int i = 1; i <= 2; ++i) {
    RECT f = {lens.x - i, lens.y - i, lens.x + lens.width + i,
              lens.y + lens.height + i};
    FrameRect(hdc, &f, accent);
  }
  DeleteObject(accent);
  DeleteObject(dark);

  CURSORINFO ci = {};
  ci.cbSize = sizeof(ci);
  if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) {
    return;
  }
  double cx = ci.ptScreenPos.x - contentScreen.left - src.x;
  double cy = ci.ptScreenPos.y - contentScreen.top - src.y;
  if (cx < 0 || cy < 0 || cx >= src.width || cy >= src.height) return;
  // r.hotspot is kept up to date by DrawCursor, which runs first.
  int x = lens.x + (int)(cx * lensScale) - r.hotspot.x;
  int y = lens.y + (int)(cy * lensScale) - r.hotspot.y;
  HRGN clip = CreateRectRgn(lens.x, lens.y, lens.x + lens.width,
                            lens.y + lens.height);
  SelectClipRgn(hdc, clip);
  DrawIconEx(hdc, x, y, ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
  SelectClipRgn(hdc, nullptr);
  DeleteObject(clip);
}

void DrawStatusBar(HDC hdc, const Shared& s, double scale,
                   const viewmath::PanRange& rx,
                   const viewmath::PanRange& ry) {
  int alpha = viewmath::BarAlpha((unsigned long)(GetTickCount64() -
                                                 s.barShownAt));
  if (alpha <= 0) return;

  const int barH = viewmath::kBarHeight;
  const int cw = r.viewW;
  if (!r.barDC || r.barW != cw) {
    if (r.barDC) {
      SelectObject(r.barDC, r.barOld);
      DeleteObject(r.barBmp);
      DeleteDC(r.barDC);
    }
    HDC screen = GetDC(nullptr);
    r.barDC = CreateCompatibleDC(screen);
    r.barBmp = CreateCompatibleBitmap(screen, cw, barH);
    ReleaseDC(nullptr, screen);
    r.barOld = SelectObject(r.barDC, r.barBmp);
    r.barW = cw;
  }
  if (!r.barFont) {
    r.barFont = CreateFontW(-19, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                            DEFAULT_CHARSET, 0, 0, 0, 0, L"Consolas");
  }

  std::wstring name = s.label.empty() ? L"unknown" : s.label;
  if (s.lockHwnd) {
    std::wstring title = s.lockTitle;
    if (title.size() > 40) title = title.substr(0, 39) + L"…";
    name += L" › " + title;
  }
  wchar_t zoom[32];
  if (viewmath::kZoomSteps[s.zoomStep] > 0) {
    swprintf_s(zoom, L"%.2fx", scale);
  } else {
    swprintf_s(zoom, L"fit %.2fx", scale);
  }
  int panX = rx.max > rx.min ? (int)std::round(r.panX) : 0;
  int panY = ry.max > ry.min ? (int)std::round(r.panY) : 0;
  wchar_t status[384];
  swprintf_s(status,
             L"  %ls    %ls    pan %d,%d of %d,%d    mon %dx%d    %d fps%ls  ",
             name.c_str(), zoom, panX, panY, (int)(rx.max - rx.min),
             (int)(ry.max - ry.min), r.main.monW, r.main.monH, r.fps,
             r.main.dup ? L"" : L" (GDI)");

  RECT bar = {0, 0, cw, barH};
  HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));
  FillRect(r.barDC, &bar, bg);
  DeleteObject(bg);

  SetBkMode(r.barDC, TRANSPARENT);
  SetTextColor(r.barDC, RGB(57, 255, 136));
  HGDIOBJ of = SelectObject(r.barDC, r.barFont);
  RECT tr = {8, 0, cw - 8, barH};
  DrawTextW(r.barDC, status, -1, &tr,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  SelectObject(r.barDC, of);

  BLENDFUNCTION blend = {AC_SRC_OVER, 0, (BYTE)alpha, 0};
  GdiAlphaBlend(hdc, 0, 0, cw, barH, r.barDC, 0, 0, cw, barH, blend);
}

// Ends a window lock the render thread found to be over (window closed,
// minimised or moved off the monitor).
void EndLock(HWND hwnd, const wchar_t* why) {
  std::lock_guard<std::mutex> hold(g_lock);
  if (g_shared.lockHwnd != hwnd) return;
  DebugTrace(L"Window lock ended: %ls", why);
  g_shared.lockHwnd = nullptr;
  g_shared.zoomStep = g_shared.savedZoomStep;
  g_shared.generation++;
  g_shared.barShownAt = GetTickCount64();
}

void RenderFrame() {
  Shared s;
  {
    std::lock_guard<std::mutex> hold(g_lock);
    s = g_shared;
  }

  bool snap = false;
  if (s.reload != r.seenReload) {
    // The layout changed: monitors may have moved, resized or changed
    // adapter. Capture is set up again; the window stays.
    r.seenReload = s.reload;
    r.main.device.clear();
    r.pip.device.clear();
    r.seenGeneration = ~0u;
  }
  if (s.generation != r.seenGeneration) {
    r.seenGeneration = s.generation;
    snap = true;
    if (s.device != r.main.device || !r.main.content) {
      // Windows lets a process capture a monitor only once: if the PiP has
      // the monitor now wanted in the main view, let it go first, or the main
      // view falls back to slow GDI capture. The PiP is set up again below.
      if (!r.pip.device.empty() && r.pip.device == s.device) {
        ReleaseSource(r.pip);
        r.pip.device.clear();
      }
      if (!SetupSource(s.device)) {
        // Try again shortly (the display may still be settling).
        r.seenGeneration = ~0u;
        r.main.device.clear();
        Sleep(500);
        return;
      }
    }
  }
  if (!r.swap || !r.main.content) {
    Sleep(100);
    return;
  }

  // The app may still name the main view's monitor for the PiP for a frame
  // or two after a switch. Capturing it twice fails, so wait for the update.
  const std::wstring wantPip =
      s.pipDevice == r.main.device ? std::wstring() : s.pipDevice;
  if (wantPip != r.pip.device) SetupPip(wantPip);
  Capture(r.main);
  Capture(r.pip);

  // What is shown: the whole monitor, or the locked window's part of it.
  RECT contentScreen = r.main.monRect;
  if (s.lockHwnd) {
    RECT wr;
    if (!IsWindow(s.lockHwnd) || IsIconic(s.lockHwnd)) {
      EndLock(s.lockHwnd, L"window closed or minimised");
    } else if ((FAILED(DwmGetWindowAttribute(s.lockHwnd,
                                             DWMWA_EXTENDED_FRAME_BOUNDS, &wr,
                                             sizeof(wr))) &&
                !GetWindowRect(s.lockHwnd, &wr)) ||
               !IntersectRect(&contentScreen, &wr, &r.main.monRect)) {
      contentScreen = r.main.monRect;
      EndLock(s.lockHwnd, L"window left the monitor");
    }
  }
  int cW = contentScreen.right - contentScreen.left;
  int cH = contentScreen.bottom - contentScreen.top;
  double scale = viewmath::EffectiveScale(viewmath::kZoomSteps[s.zoomStep],
                                          cH, r.viewH);

  // Focus: whichever moved last, the mouse or the text cursor.
  ULONGLONG now = GetTickCount64();
  POINT mouse;
  if (GetCursorPos(&mouse) &&
      (mouse.x != r.lastMouse.x || mouse.y != r.lastMouse.y)) {
    r.lastMouse = mouse;
    r.mouseMovedAt = now;
  }
  Caret caret;
  {
    std::lock_guard<std::mutex> hold(g_caretLock);
    caret = g_caret;
  }
  POINT focus = r.lastMouse;
  if (caret.valid && caret.changedAt > r.mouseMovedAt &&
      PtInRect(&contentScreen, caret.pt)) {
    focus = caret.pt;
  }
  bool focusInside = PtInRect(&contentScreen, focus) != FALSE;
  double fx = (focus.x - contentScreen.left) * scale;
  double fy = (focus.y - contentScreen.top) * scale;

  LARGE_INTEGER tick;
  QueryPerformanceCounter(&tick);
  double dtMs = (double)(tick.QuadPart - r.lastTick.QuadPart) * 1000.0 /
                (double)r.freq.QuadPart;
  r.lastTick = tick;

  viewmath::PanRange rx = viewmath::AxisPanRange(cW, scale, r.viewW);
  viewmath::PanRange ry = viewmath::AxisPanRange(cH, scale, r.viewH);
  if (snap) {
    r.panX = viewmath::CentreOn(focusInside ? fx : cW * scale / 2, r.viewW, rx);
    r.panY = viewmath::CentreOn(focusInside ? fy : cH * scale / 2, r.viewH, ry);
  } else if (focusInside) {
    r.panX = viewmath::EaseToward(
        r.panX, viewmath::DeadZoneTarget(fx, r.panX, r.viewW, rx), dtMs);
    r.panY = viewmath::EaseToward(
        r.panY, viewmath::DeadZoneTarget(fy, r.panY, r.viewH, ry), dtMs);
    r.panX = viewmath::KeepVisible(r.panX, fx, r.viewW, rx);
    r.panY = viewmath::KeepVisible(r.panY, fy, r.viewH, ry);
  } else {
    r.panX = viewmath::ClampToRange(r.panX, rx);
    r.panY = viewmath::ClampToRange(r.panY, ry);
  }

  // Draw the content as one quad; the viewport clips it to the glasses.
  ID3D11RenderTargetView* rtvs[] = {r.rtv};
  r.ctx->OMSetRenderTargets(1, rtvs, nullptr);
  const float black[4] = {0, 0, 0, 1};
  r.ctx->ClearRenderTargetView(r.rtv, black);
  if (r.main.contentReady) {
    bool exact = viewmath::IsPixelExact(scale);
    if (scale < 1.0 && r.main.mipsDirty) {
      r.ctx->GenerateMips(r.main.contentSrv);
      r.main.mipsDirty = false;
    }
    // Whole pixels only, so 1:1 stays sharp while the view glides.
    double x0 = -std::round(r.panX);
    double y0 = -std::round(r.panY);
    float uv[4] = {
        (float)(contentScreen.left - r.main.monRect.left) / r.main.monW,
        (float)(contentScreen.top - r.main.monRect.top) / r.main.monH,
        (float)(contentScreen.right - r.main.monRect.left) / r.main.monW,
        (float)(contentScreen.bottom - r.main.monRect.top) / r.main.monH};
    DrawTexture(r.main.contentSrv, x0, y0, x0 + cW * scale, y0 + cH * scale, uv,
                exact ? r.pointSamp
                      : (scale < 1.0 ? r.mipSamp : r.linearSamp));
  }

  // Picture-in-picture, dodging the focus point.
  bool showPip = r.pip.contentReady && r.pip.device != r.main.device;
  viewmath::PipRect pip;
  if (showPip) {
    pip = viewmath::PlacePip(r.viewW, r.viewH, r.pip.monW, r.pip.monH,
                             focusInside, fx - r.panX, fy - r.panY);
    if (r.pip.mipsDirty) {
      r.ctx->GenerateMips(r.pip.contentSrv);
      r.pip.mipsDirty = false;
    }
    const float all[4] = {0, 0, 1, 1};
    DrawTexture(r.pip.contentSrv, pip.x, pip.y, pip.x + pip.width,
                pip.y + pip.height, all, r.mipSamp);
  }

  // Edge lens: near an edge or corner, where the glasses are hard to see, show
  // the area around the focus point again further in.
  bool showLens = false;
  viewmath::LensRect lens;
  viewmath::LensSource lensSrc;
  double lensScale = scale * s.lensZoom;
  // While the lens is being adjusted in the control panel it shows anyway, so
  // its size and spot can be judged through the glasses.
  bool lensPreview = now < s.lensPreviewUntil;
  if (s.lensZoom > 0 && r.main.contentReady && (focusInside || lensPreview)) {
    double vx = fx - std::round(r.panX);
    double vy = fy - std::round(r.panY);
    r.lensShowing =
        lensPreview || viewmath::LensWanted(r.lensShowing, vx, vy, r.viewW,
                                            r.viewH, s.lensLayout.trigger);
  } else {
    r.lensShowing = false;
  }
  if (r.lensShowing) {
    lens = viewmath::PlaceLensFor(s.lensLayout, fx - std::round(r.panX),
                                  fy - std::round(r.panY), r.viewW, r.viewH);
    lensSrc = viewmath::LensSourceRect(focus.x - contentScreen.left,
                                       focus.y - contentScreen.top, cW, cH,
                                       lens.width, lens.height, lensScale);
    double ox = contentScreen.left - r.main.monRect.left;
    double oy = contentScreen.top - r.main.monRect.top;
    float uv[4] = {(float)((ox + lensSrc.x) / r.main.monW),
                   (float)((oy + lensSrc.y) / r.main.monH),
                   (float)((ox + lensSrc.x + lensSrc.width) / r.main.monW),
                   (float)((oy + lensSrc.y + lensSrc.height) / r.main.monH)};
    DrawTexture(r.main.contentSrv, lens.x, lens.y, lens.x + lens.width,
                lens.y + lens.height, uv,
                viewmath::IsPixelExact(lensScale)
                    ? r.pointSamp
                    : (lensScale < 1.0 ? r.mipSamp : r.linearSamp));
    showLens = true;
  }

  // Cursor and status bar via GDI on the back buffer.
  r.ctx->OMSetRenderTargets(0, nullptr, nullptr);
  IDXGISurface1* surface = nullptr;
  if (SUCCEEDED(r.swap->GetBuffer(0, __uuidof(IDXGISurface1),
                                  (void**)&surface))) {
    HDC hdc = nullptr;
    if (SUCCEEDED(surface->GetDC(FALSE, &hdc))) {
      DrawCursor(hdc, contentScreen, scale);
      if (showPip) DrawPipOverlay(hdc, pip, s.pipLabel);
      if (showLens) {
        DrawLensOverlay(hdc, lens, lensSrc, lensScale, contentScreen);
      }
      DrawStatusBar(hdc, s, scale, rx, ry);
      surface->ReleaseDC(nullptr);
    }
    surface->Release();
  }

  HRESULT hr = r.swap->Present(1, 0);
  if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
    DebugTrace(L"Device lost 0x%lX, re-creating", (unsigned long)hr);
    ReleaseDevice();
    r.main.device.clear();
    std::lock_guard<std::mutex> hold(g_lock);
    g_shared.generation++;
  }

  r.fpsFrames++;
  if (now - r.fpsMark >= 1000) {
    r.fps = (int)(r.fpsFrames * 1000 / (now - r.fpsMark));
    r.fpsMark = now;
    r.fpsFrames = 0;
  }
  g_statFps = r.fps;
  g_statDup = r.main.dup != nullptr;
  g_statScale = scale;
}

void RenderLoop() {
  DebugTrace(L"RenderLoop: started");
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  QueryPerformanceFrequency(&r.freq);
  QueryPerformanceCounter(&r.lastTick);
  r.fpsMark = GetTickCount64();
  r.seenGeneration = ~0u;
  r.main.device.clear();
  while (!g_stop.load()) {
    RenderFrame();  // Present() waits for vsync, which paces the loop
  }
  ReleaseOverlay();
  ReleaseDevice();
  CoUninitialize();
  DebugTrace(L"RenderLoop: stopped");
}

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_MOUSEACTIVATE:
      // Clicking the viewer must never take focus away from the app the user
      // is working in.
      return MA_NOACTIVATE;
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MOUSEWHEEL:
      // The glasses are a display, not an input surface. Clicks that land here
      // are deliberately swallowed: whatever sits underneath is hidden by the
      // viewer, so passing them through would click things the user cannot
      // see.
      return 0;
    case WM_SETCURSOR:
      // The cursor is drawn into the image, so hide the system one here to
      // avoid showing it twice.
      SetCursor(nullptr);
      return TRUE;
    case WM_DESTROY:
      g_window = nullptr;
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool Start(const std::wstring& sourceMonitorDevice,
           const std::wstring& targetMonitorDevice, std::wstring* error) {
  if (g_running.load()) return true;

  RECT src = MonitorRect(sourceMonitorDevice);
  if (src.right <= src.left) {
    if (error) *error = L"Could not read the source monitor.";
    return false;
  }

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = kWindowClass;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
  RegisterClassExW(&wc);

  RECT gr = MonitorRect(targetMonitorDevice);
  int gw = gr.right - gr.left;
  int gh = gr.bottom - gr.top;
  if (gw <= 0) gw = 1920;
  if (gh <= 0) gh = 1080;

  g_window = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kWindowClass,
      L"Viture Viewer", WS_POPUP, gr.left, gr.top, gw, gh, nullptr, nullptr,
      GetModuleHandleW(nullptr), nullptr);
  if (!g_window) {
    if (error) *error = L"Could not create the viewer window.";
    return false;
  }
  ShowWindow(g_window, SW_SHOWNOACTIVATE);
  SetWindowPos(g_window, HWND_TOPMOST, gr.left, gr.top, gw, gh,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);

  {
    std::lock_guard<std::mutex> hold(g_lock);
    g_shared.device = sourceMonitorDevice;
    g_shared.lockHwnd = nullptr;
    g_shared.generation++;
    g_shared.barShownAt = GetTickCount64();
  }
  {
    std::lock_guard<std::mutex> hold(g_caretLock);
    g_caret = Caret();
  }

  DebugTrace(L"Start: source=%ls target=%ls", sourceMonitorDevice.c_str(),
             targetMonitorDevice.c_str());
  g_stop = false;
  g_running = true;
  g_renderThread = std::thread(RenderLoop);
  g_caretThread = std::thread(CaretLoop);
  return true;
}

void Stop() {
  if (!g_running.load() && !g_window) return;
  DebugTrace(L"Stop: begin");

  g_stop = true;
  if (g_renderThread.joinable()) g_renderThread.join();
  if (g_caretThread.joinable()) g_caretThread.join();

  if (g_window && IsWindow(g_window)) {
    DestroyWindow(g_window);
    g_window = nullptr;
  }
  {
    std::lock_guard<std::mutex> hold(g_lock);
    g_shared.lockHwnd = nullptr;
  }
  g_running = false;
  DebugTrace(L"Stop: done");
}

bool IsRunning() { return g_running.load(); }

bool Reload(const std::wstring& targetMonitorDevice) {
  if (!g_running.load() || !g_window || !IsWindow(g_window)) return false;
  RECT gr = MonitorRect(targetMonitorDevice);
  int gw = gr.right - gr.left;
  int gh = gr.bottom - gr.top;
  RECT client;
  GetClientRect(g_window, &client);
  if (gw <= 0 || gh <= 0 || client.right != gw || client.bottom != gh) {
    return false;  // the glasses changed size: the window must be rebuilt
  }
  SetWindowPos(g_window, HWND_TOPMOST, gr.left, gr.top, gw, gh,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);
  std::lock_guard<std::mutex> hold(g_lock);
  g_shared.reload++;
  DebugTrace(L"Reload: capture set up again, window kept");
  return true;
}

void FocusMonitor(const std::wstring& monitorDevice,
                  const std::wstring& label) {
  std::lock_guard<std::mutex> hold(g_lock);
  g_shared.device = monitorDevice;
  g_shared.label = label;
  if (g_shared.lockHwnd) {
    g_shared.lockHwnd = nullptr;
    g_shared.zoomStep = g_shared.savedZoomStep;
  }
  g_shared.generation++;
  g_shared.barShownAt = GetTickCount64();
  DebugTrace(L"FocusMonitor: %ls (%ls)", monitorDevice.c_str(), label.c_str());
}

void StepZoom(int direction) {
  std::lock_guard<std::mutex> hold(g_lock);
  g_shared.zoomStep = viewmath::NextZoomStep(g_shared.zoomStep, direction);
  g_shared.generation++;
  g_shared.barShownAt = GetTickCount64();
}

void LockWindow(HWND hwnd, const std::wstring& title) {
  std::lock_guard<std::mutex> hold(g_lock);
  if (hwnd) {
    if (!g_shared.lockHwnd) g_shared.savedZoomStep = g_shared.zoomStep;
    g_shared.lockHwnd = hwnd;
    g_shared.lockTitle = title;
    g_shared.zoomStep = viewmath::kFitStep;
  } else if (g_shared.lockHwnd) {
    g_shared.lockHwnd = nullptr;
    g_shared.zoomStep = g_shared.savedZoomStep;
  }
  g_shared.generation++;
  g_shared.barShownAt = GetTickCount64();
}

bool IsWindowLocked() {
  std::lock_guard<std::mutex> hold(g_lock);
  return g_shared.lockHwnd != nullptr;
}

std::wstring LockedTitle() {
  std::lock_guard<std::mutex> hold(g_lock);
  return g_shared.lockHwnd ? g_shared.lockTitle : std::wstring();
}

void SetZoomStep(int step) {
  if (step < 0 || step >= viewmath::kZoomStepCount) return;
  std::lock_guard<std::mutex> hold(g_lock);
  if (g_shared.zoomStep == step) return;
  g_shared.zoomStep = step;
  g_shared.generation++;
  g_shared.barShownAt = GetTickCount64();
}

int ZoomStep() {
  std::lock_guard<std::mutex> hold(g_lock);
  return g_shared.zoomStep;
}

void SetFollowCaret(bool enabled) {
  g_followCaret = enabled;
  if (!enabled) {
    std::lock_guard<std::mutex> hold(g_caretLock);
    g_caret = Caret();
  }
}

Stats GetStats() {
  Stats s;
  if (!g_running.load()) return s;
  s.fps = g_statFps.load();
  s.duplication = g_statDup.load();
  s.scale = g_statScale.load();
  return s;
}

HWND Window() { return g_window; }

void SetEdgeLens(double zoom, const viewmath::LensLayout& layout) {
  std::lock_guard<std::mutex> hold(g_lock);
  g_shared.lensZoom = zoom > 0 ? zoom : 0;
  g_shared.lensLayout = viewmath::ClampLensLayout(layout);
}

void PreviewEdgeLens(unsigned ms) {
  std::lock_guard<std::mutex> hold(g_lock);
  g_shared.lensPreviewUntil = GetTickCount64() + ms;
}

void SetPip(const std::wstring& monitorDevice, const std::wstring& label) {
  std::lock_guard<std::mutex> hold(g_lock);
  g_shared.pipDevice = monitorDevice;
  g_shared.pipLabel = label;
}

}  // namespace viewer
