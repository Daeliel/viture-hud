// Copyright (C) 2026 Jurica Dujmovic
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Viture HUD - tray app: monitors, hotkeys, settings and the tray icon.
//
// The glasses are an ordinary display to Windows. The viewer (viewer.cpp)
// opens a fullscreen window on them and draws a live capture of another
// monitor into it, so the real monitors are never reconfigured. This file
// decides which monitor that is: Alt+D cycles through them, the control
// panel (panel.cpp) picks one, and plugging the glasses in starts it.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <dwmapi.h>
#include <windowsx.h>

#include "glasses_id.h"
#include "panel.h"
#include "viewer.h"

#include <cmath>
#include <map>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define ID_TRAY 1
#define HOTKEY_CYCLE 1
#define HOTKEY_PREV 2
#define HOTKEY_STOP 3
#define HOTKEY_ZOOM_NEXT 5
#define HOTKEY_ZOOM_PREV 6
#define HOTKEY_LOCK 7
#define HOTKEY_PIP 8

#define TIMER_TOAST 2
#define TIMER_RESCAN 3
#define TIMER_DISPLAY_CHANGE 4
#define TIMER_PICK_TIMEOUT 5

#define WM_PICK_MOVE (WM_APP + 2)
#define WM_PICK_DONE (WM_APP + 3)
#define WM_PICK_CANCEL (WM_APP + 4)

#define MENU_CYCLE 1001
#define MENU_STOP 1002
#define MENU_REFRESH 1003
#define MENU_EXIT 1004
#define MENU_AUTOSTART 1005
#define MENU_SHORTCUT 1006
#define MENU_OPEN 1007
#define MENU_ABOUT 1008

static const wchar_t* kAppName = L"Viture HUD";
static const wchar_t* kClassName = L"VitureHudTrayWnd";

// ---------------------------------------------------------------------------
// Monitor model
// ---------------------------------------------------------------------------
struct MonitorInfo {
  int displayIndex = 0;   // matches DISPLAYn
  std::wstring deviceName;  // \\.\DISPLAYn
  std::wstring label;       // human readable
  bool isViture = false;
  bool isPrimary = false;
  RECT bounds = {0, 0, 0, 0};
};

struct AppState {
  HINSTANCE instance = nullptr;
  HWND hwnd = nullptr;
  HICON icon = nullptr;
  std::vector<MonitorInfo> monitors;
  int currentSource = -1;  // DISPLAY index currently shown, -1 = extended
  int lastShown = -1;      // last monitor shown, for auto-start
  bool hotkeysOk = false;

  // Whether the glasses were connected at the last check, to spot plug-in.
  bool glassesPresent = false;

  // Where the mouse last was on each monitor, so Alt+D can put it back there.
  std::map<std::wstring, POINT> lastCursor;

  // The window last active on each monitor, so Alt+D can give it focus back.
  std::map<std::wstring, HWND> lastActive;

  // Settings, kept in HKCU\Software\VitureHUD.
  bool followCaret = true;
  bool autoShow = true;
  bool pipOn = false;
  int lensStep = 1;  // edge lens: index into kLensZooms
  viewmath::LensLayout lensLayout;  // its size and where it appears
};

static AppState g;

// Overlay window used for the on-glasses toast.
static HWND g_toast = nullptr;
static HFONT g_toastFont = nullptr;

// Green outline around the window under the mouse while picking one to lock.
static HWND g_pickFrame = nullptr;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
// Reads the EDID manufacturer id from the registry. The monitor id looks like
// "MONITOR\\GSM77D3\\{guid}\\0017", so the enum key is
// SYSTEM\\CurrentControlSet\\Enum\\DISPLAY\\<hardware id>
// where the hardware id is the first two backslash-separated parts.
static std::wstring ReadEdidManufacturer(const std::wstring& monitorDeviceId) {
  if (monitorDeviceId.empty()) return L"";

  // Split "MONITOR\\GSM77D3\\{...}\\0017" into "GSM77D3" + instance.
  size_t p1 = monitorDeviceId.find(L'\\');
  if (p1 == std::wstring::npos) return L"";
  size_t p2 = monitorDeviceId.find(L'\\', p1 + 1);
  if (p2 == std::wstring::npos) return L"";
  std::wstring hardware = monitorDeviceId.substr(p1 + 1, p2 - p1 - 1);
  size_t p3 = monitorDeviceId.rfind(L'\\');
  if (p3 == std::wstring::npos || p3 <= p2) return L"";
  std::wstring instance = monitorDeviceId.substr(p3 + 1);

  // The instance key is the display adapter enum: DISPLAY\\<hardware>\\<instance>
  std::wstring path = L"SYSTEM\\CurrentControlSet\\Enum\\DISPLAY\\" +
                      hardware + L"\\" + instance;
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &key) !=
      ERROR_SUCCESS) {
    return L"";
  }
  BYTE buf[128] = {0};
  DWORD size = sizeof(buf);
  DWORD type = 0;
  std::wstring result;
  if (RegQueryValueExW(key, L"EDID", nullptr, &type, buf, &size) ==
          ERROR_SUCCESS &&
      size >= 10) {
    unsigned int packed = ((unsigned int)buf[8] << 8) | buf[9];
    wchar_t m[4];
    m[0] = (wchar_t)(((packed >> 10) & 0x1F) + 'A' - 1);
    m[1] = (wchar_t)(((packed >> 5) & 0x1F) + 'A' - 1);
    m[2] = (wchar_t)((packed & 0x1F) + 'A' - 1);
    m[3] = L'\0';
    result = m;
  }
  RegCloseKey(key);
  return result;
}

static int DisplayIndexFromDeviceName(const std::wstring& name) {
  size_t pos = name.find(L"DISPLAY");
  if (pos == std::wstring::npos) return -1;
  return _wtoi(name.c_str() + pos + 7);
}

// One active display path: the GDI device name (\\.\DISPLAYn) and the name
// the monitor reports in its EDID ("DELL U2720Q", "VITURE", ...).
struct TargetName {
  std::wstring deviceName;
  std::wstring friendlyName;
};

// Asks the CCD API for the friendly name of every active target. The CCD
// target name is the reliable source of truth, unlike the GDI monitor string
// which is often just "Generic PnP Monitor".
static std::vector<TargetName> ActiveTargetNames() {
  std::vector<TargetName> out;

  UINT32 pathCount = 0, modeCount = 0;
  if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &pathCount, &modeCount) !=
      ERROR_SUCCESS) {
    return out;
  }
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
  if (QueryDisplayConfig(QDC_ALL_PATHS, &pathCount, paths.data(), &modeCount,
                         modes.data(), nullptr) != ERROR_SUCCESS) {
    return out;
  }
  paths.resize(pathCount);

  for (const auto& p : paths) {
    // Inactive paths carry stale source ids that alias other displays.
    if (!(p.flags & DISPLAYCONFIG_PATH_ACTIVE)) continue;
    DISPLAYCONFIG_TARGET_DEVICE_NAME tgt = {};
    tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    tgt.header.size = sizeof(tgt);
    tgt.header.adapterId = p.targetInfo.adapterId;
    tgt.header.id = p.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&tgt.header) != ERROR_SUCCESS) continue;

    DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {};
    src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    src.header.size = sizeof(src);
    src.header.adapterId = p.sourceInfo.adapterId;
    src.header.id = p.sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;

    out.push_back({src.viewGdiDeviceName, tgt.monitorFriendlyDeviceName});
  }
  return out;
}

// ---------------------------------------------------------------------------
// Monitor enumeration
// ---------------------------------------------------------------------------
static std::vector<MonitorInfo> EnumerateMonitors() {
  std::vector<MonitorInfo> result;

  std::vector<TargetName> targets = ActiveTargetNames();

  DISPLAY_DEVICEW dd = {};
  dd.cb = sizeof(dd);
  for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
    if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;

    MonitorInfo mi;
    mi.deviceName = dd.DeviceName;
    mi.displayIndex = DisplayIndexFromDeviceName(mi.deviceName);
    if (mi.displayIndex < 0) continue;
    mi.isPrimary = (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;

    // Monitor details: name and EDID manufacturer.
    std::wstring monitorName;
    std::wstring mfr;
    DISPLAY_DEVICEW mon = {};
    mon.cb = sizeof(mon);
    if (EnumDisplayDevicesW(mi.deviceName.c_str(), 0, &mon, 0)) {
      monitorName = mon.DeviceString;
      mfr = ReadEdidManufacturer(mon.DeviceID);
    }

    // A display is the glasses if its CCD name is "VITURE" or "VITURE <model>",
    // or its EDID manufacturer is one VITURE uses (see glasses_id.h). CCD also
    // gives the real monitor name, preferred over the driver string.
    std::wstring ccdName;
    for (const auto& t : targets) {
      if (t.deviceName != mi.deviceName) continue;
      ccdName = t.friendlyName;
      if (!t.friendlyName.empty()) monitorName = t.friendlyName;
    }
    // Never match on the adapter or driver string: those are generic.
    mi.isViture = glassesid::IsVitureDisplay(ccdName, mfr);

    // Current geometry.
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(mi.deviceName.c_str(), ENUM_CURRENT_SETTINGS,
                             &dm)) {
      mi.bounds.left = dm.dmPosition.x;
      mi.bounds.top = dm.dmPosition.y;
      mi.bounds.right = dm.dmPosition.x + (LONG)dm.dmPelsWidth;
      mi.bounds.bottom = dm.dmPosition.y + (LONG)dm.dmPelsHeight;
    }

    // Human label.
    std::wstring base = mi.isViture
                            ? L"Viture glasses"
                            : (monitorName.empty() ? L"Monitor" : monitorName);
    mi.label = base + L" (DISPLAY" + std::to_wstring(mi.displayIndex) + L")";
    if (mi.isPrimary) mi.label = base + L" (primary) (DISPLAY" +
                                        std::to_wstring(mi.displayIndex) + L")";

    result.push_back(mi);
  }

  // Sort by DISPLAY index for a stable cycle order.
  for (size_t i = 0; i + 1 < result.size(); ++i) {
    for (size_t j = i + 1; j < result.size(); ++j) {
      if (result[j].displayIndex < result[i].displayIndex) {
        MonitorInfo tmp = result[i];
        result[i] = result[j];
        result[j] = tmp;
      }
    }
  }
  return result;
}

static std::vector<int> CandidateSources() {
  std::vector<int> out;
  for (const auto& m : g.monitors) {
    if (!m.isViture) out.push_back(m.displayIndex);
  }
  return out;
}

static const MonitorInfo* FindMonitor(int displayIndex) {
  for (const auto& m : g.monitors) {
    if (m.displayIndex == displayIndex) return &m;
  }
  return nullptr;
}

static const MonitorInfo* FindGlasses() {
  for (const auto& m : g.monitors) {
    if (m.isViture) return &m;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// On-screen toast on the glasses
// ---------------------------------------------------------------------------
static void ShowToast(const std::wstring& text) {
  // A borderless, click-through window centred on the glasses that fades out
  // after ~1.2 s. Kept topmost so it sits over the viewer.
  if (!g_toastFont) {
    g_toastFont = CreateFontW(
        -42, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Segoe UI");
  }

  RECT target = {0, 0, 1920, 1080};
  if (const MonitorInfo* glasses = FindGlasses()) {
    target = glasses->bounds;
  }

  if (!g_toast) {
    g_toast = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT |
            WS_EX_LAYERED | WS_EX_NOACTIVATE,
        L"STATIC", L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, g.instance,
        nullptr);
    if (!g_toast) return;
  }

  HDC screen = GetDC(nullptr);

  // Size the toast to its text so long monitor names are not cut off, but
  // never wider than the glasses.
  const int kPadX = 48;
  RECT measure = {0, 0, 0, 0};
  HGDIOBJ measureOld = SelectObject(screen, g_toastFont);
  DrawTextW(screen, text.c_str(), -1, &measure, DT_CALCRECT | DT_SINGLELINE);
  SelectObject(screen, measureOld);

  int targetW = target.right - target.left;
  int w = (measure.right - measure.left) + kPadX * 2;
  if (w < 720) w = 720;
  if (w > targetW - 40) w = targetW - 40;
  int h = 110;
  int x = target.left + (targetW - w) / 2;
  int y = target.top + (target.bottom - target.top) / 6;

  SetWindowPos(g_toast, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW);

  HDC mem = CreateCompatibleDC(screen);
  HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
  HGDIOBJ oldBmp = SelectObject(mem, bmp);

  RECT r = {0, 0, w, h};
  HBRUSH bg = CreateSolidBrush(RGB(8, 12, 16));
  FillRect(mem, &r, bg);
  DeleteObject(bg);

  // Accent bar on the left.
  RECT bar = {0, 0, 6, h};
  HBRUSH accent = CreateSolidBrush(RGB(57, 255, 136));
  FillRect(mem, &bar, accent);
  DeleteObject(accent);

  SetBkMode(mem, TRANSPARENT);
  SetTextColor(mem, RGB(240, 245, 248));
  HGDIOBJ oldFont = SelectObject(mem, g_toastFont);
  DrawTextW(mem, text.c_str(), -1, &r,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  SelectObject(mem, oldFont);

  POINT src = {0, 0};
  SIZE size = {w, h};
  POINT dst = {x, y};
  BLENDFUNCTION bf = {AC_SRC_OVER, 0, 235, 0};
  UpdateLayeredWindow(g_toast, screen, &dst, &size, mem, &src, 0, &bf,
                      ULW_ALPHA);

  SelectObject(mem, oldBmp);
  DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);

  SetTimer(g.hwnd, TIMER_TOAST, 1200, nullptr);
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------
static void ShowBalloon(const std::wstring& title, const std::wstring& text) {
  NOTIFYICONDATAW nid = {};
  nid.cbSize = sizeof(nid);
  nid.hWnd = g.hwnd;
  nid.uID = ID_TRAY;
  nid.uFlags = NIF_INFO;
  nid.dwInfoFlags = NIIF_INFO;
  wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
  wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
  Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void RefreshMonitors() {
  g.monitors = EnumerateMonitors();
  // If the glasses vanished, nothing can be shown on them: close the viewer
  // rather than leave it painting onto a display that no longer exists.
  if (!FindGlasses()) {
    if (viewer::IsRunning()) viewer::Stop();
    g.currentSource = -1;
  }
}

// Alt+D takes the mouse along to the monitor now on the glasses, back to where
// it last was on that monitor (or its centre), so you can work there straight
// away. Nothing moves if the mouse is already on that monitor.
//
// Focus goes along too, to the window last used on that monitor (or the one
// under the mouse). That matters for apps that capture the mouse while they
// are active - Moonlight, games, remote desktops: they confine the cursor to
// their own window and hide it, so it could not leave until they lose focus.
static bool IsUserWindow(HWND hwnd);

static std::wstring MonitorDeviceOf(HWND hwnd) {
  MONITORINFOEXW info = {};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST),
                       &info)) {
    return L"";
  }
  return info.szDevice;
}

static void FocusWindowOn(const MonitorInfo& target, POINT at) {
  HWND pick = nullptr;
  auto last = g.lastActive.find(target.deviceName);
  if (last != g.lastActive.end() && IsUserWindow(last->second) &&
      !IsIconic(last->second) &&
      MonitorDeviceOf(last->second) == target.deviceName) {
    pick = last->second;
  }
  if (!pick) {
    HWND under = WindowFromPoint(at);
    if (under) under = GetAncestor(under, GA_ROOT);
    if (IsUserWindow(under)) pick = under;
  }
  if (!pick) return;
  // Allowed: this runs in response to our hotkey, the latest input event.
  BOOL ok = SetForegroundWindow(pick);
  viewer::DebugTrace(L"Focus on %ls -> %p (%ls)", target.deviceName.c_str(),
                     (void*)pick, ok ? L"ok" : L"refused");
}

static void MoveMouseTo(const MonitorInfo& target) {
  // Remember what was active on the monitor being left, to return to later.
  HWND active = GetForegroundWindow();
  if (IsUserWindow(active)) g.lastActive[MonitorDeviceOf(active)] = active;

  POINT cur;
  if (!GetCursorPos(&cur) || PtInRect(&target.bounds, cur)) return;

  for (const auto& m : g.monitors) {
    if (!m.isViture && PtInRect(&m.bounds, cur)) g.lastCursor[m.deviceName] = cur;
  }

  POINT dest = {(target.bounds.left + target.bounds.right) / 2,
                (target.bounds.top + target.bounds.bottom) / 2};
  auto saved = g.lastCursor.find(target.deviceName);
  if (saved != g.lastCursor.end() && PtInRect(&target.bounds, saved->second)) {
    dest = saved->second;
  }
  // Undo any confinement first, or SetCursorPos stays inside it.
  ClipCursor(nullptr);
  SetCursorPos(dest.x, dest.y);
  FocusWindowOn(target, dest);
}

// "DELL U2720Q (primary) (DISPLAY1)" -> "DELL U2720Q".
static std::wstring ShortLabel(const MonitorInfo& m) {
  std::wstring label = m.label;
  size_t paren = label.find(L" (");
  if (paren != std::wstring::npos) label = label.substr(0, paren);
  return label;
}

// The monitor for the picture-in-picture: the next one after the monitor on
// the glasses (with two monitors, simply the other one).
static const MonitorInfo* PipTarget() {
  std::vector<int> candidates = CandidateSources();
  if (candidates.size() < 2) return nullptr;
  size_t pos = 0;
  for (size_t i = 0; i < candidates.size(); ++i) {
    if (candidates[i] == g.currentSource) pos = i;
  }
  int next = candidates[(pos + 1) % candidates.size()];
  return next == g.currentSource ? nullptr : FindMonitor(next);
}

static void UpdatePip() {
  const MonitorInfo* m = g.pipOn && viewer::IsRunning() ? PipTarget() : nullptr;
  viewer::SetPip(m ? m->deviceName : L"", m ? ShortLabel(*m) : L"");
}

// |moveMouse| is true when the user picked the monitor (hotkey or menu), and
// false when the app chose it (auto-start, display changes).
static void ShowSource(int displayIndex, bool moveMouse) {
  RefreshMonitors();
  const MonitorInfo* m = FindMonitor(displayIndex);
  if (!m) return;

  const MonitorInfo* glasses = FindGlasses();
  if (!glasses) {
    ShowToast(L"\u26A0  Viture glasses not connected");
    return;
  }

  std::wstring error;
  viewer::DebugTrace(L"ShowSource(%d) device=%ls", displayIndex,
                     m->deviceName.c_str());

  // The viewer shows every monitor at once, so it only needs starting once.
  // Alt+D then just brings the chosen monitor into view.
  if (!viewer::IsRunning()) {
    if (!viewer::Start(m->deviceName, glasses->deviceName, &error)) {
      ShowBalloon(kAppName, error);
      ShowToast(L"\u26A0  " + error);
      return;
    }
  }

  std::wstring label = ShortLabel(*m);

  // Pan the view onto the chosen monitor.
  viewer::FocusMonitor(m->deviceName, label);
  if (moveMouse) MoveMouseTo(*m);
  panel::FollowMonitor(m->bounds);

  g.currentSource = displayIndex;
  g.lastShown = displayIndex;
  UpdatePip();

  ShowToast(L"Glasses: " + label + L"   \u2022   Alt+D next");
  panel::Invalidate();
}

static void Cycle(int direction, bool moveMouse) {
  RefreshMonitors();
  std::vector<int> candidates = CandidateSources();
  viewer::DebugTrace(L"Cycle: %zu candidates, glassesFound=%d, current=%d",
                     candidates.size(), FindGlasses() ? 1 : 0, g.currentSource);

  if (candidates.empty()) {
    ShowToast(L"No other monitors to show");
    return;
  }
  if (!FindGlasses()) {
    ShowToast(L"\u26A0  Viture glasses not connected");
    return;
  }

  int next;
  if (g.currentSource < 0) {
    next = direction > 0 ? candidates.front() : candidates.back();
  } else {
    int pos = -1;
    for (size_t i = 0; i < candidates.size(); ++i) {
      if (candidates[i] == g.currentSource) pos = (int)i;
    }
    if (pos < 0) {
      next = direction > 0 ? candidates.front() : candidates.back();
    } else {
      int n = (int)candidates.size();
      pos = (pos + (direction > 0 ? 1 : -1) + n) % n;
      next = candidates[(size_t)pos];
    }
  }
  ShowSource(next, moveMouse);
}

static void StopMirroring() {
  viewer::Stop();
  g.currentSource = -1;
  RefreshMonitors();
  ShowToast(L"Glasses: extended desktop");
  panel::Invalidate();
}

// True for an ordinary app window that can be locked to: not the desktop,
// taskbar, or one of our own windows.
static bool IsUserWindow(HWND hwnd) {
  if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd)) return false;
  if (hwnd == g.hwnd || hwnd == g_toast || hwnd == g_pickFrame ||
      hwnd == viewer::Window() ||
      hwnd == panel::Window() || hwnd == GetShellWindow()) {
    return false;
  }
  wchar_t cls[64] = {0};
  GetClassNameW(hwnd, cls, 64);
  return wcscmp(cls, L"Progman") != 0 && wcscmp(cls, L"WorkerW") != 0 &&
         wcscmp(cls, L"Shell_TrayWnd") != 0 &&
         wcscmp(cls, L"Shell_SecondaryTrayWnd") != 0 &&
         wcscmp(cls, L"NotifyIconOverflowWindow") != 0;
}

static std::wstring WindowTitle(HWND hwnd) {
  wchar_t title[256] = {0};
  if (hwnd) GetWindowTextW(hwnd, title, 256);
  return title[0] ? title : L"window";
}

// Shows only |hwnd|, fitted to the glasses. If it is on another monitor, the
// glasses switch to that monitor first.
static void LockToWindow(HWND hwnd) {
  if (!IsUserWindow(hwnd)) {
    ShowToast(L"Click a window first, then Alt+W");
    return;
  }

  RefreshMonitors();
  if (!FindGlasses()) {
    ShowToast(L"\u26A0  Viture glasses not connected");
    return;
  }
  MONITORINFOEXW info = {};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST),
                       &info)) {
    return;
  }
  const MonitorInfo* m = nullptr;
  for (const auto& mon : g.monitors) {
    if (mon.deviceName == info.szDevice) m = &mon;
  }
  if (!m || m->isViture) {
    ShowToast(L"That window is on the glasses");
    return;
  }

  if (!viewer::IsRunning() || g.currentSource != m->displayIndex) {
    ShowSource(m->displayIndex, false);
    if (!viewer::IsRunning()) return;
  }

  std::wstring name = WindowTitle(hwnd);
  viewer::LockWindow(hwnd, name);
  if (name.size() > 40) name = name.substr(0, 39) + L"\u2026";
  ShowToast(L"Locked to " + name + L"   \u2022   Alt+W unlocks");
}

static bool IsLocked() {
  return viewer::IsRunning() && viewer::IsWindowLocked();
}

static void Unlock() {
  viewer::LockWindow(nullptr, L"");
  ShowToast(L"Window lock off");
}

// ---------------------------------------------------------------------------
// Picking a window to lock (control panel)
//
// Clicking a window normally brings it to the front and buries the panel. So
// picking uses one-shot low-level hooks instead: the next left click selects
// the window under the mouse and is swallowed, so nothing is activated. A
// green frame shows which window that will be. Esc or right-click cancels.
// ---------------------------------------------------------------------------
static HHOOK g_pickMouse = nullptr;
static HHOOK g_pickKeys = nullptr;
static POINT g_pickDown = {0, 0};
static bool g_pickLeftDown = false;   // swallow the matching button-up too
static bool g_pickRightDown = false;
static RECT g_pickFrameRect = {0, 0, 0, 0};

static bool IsPicking() { return g_pickMouse != nullptr; }

static LPARAM PointParam(POINT pt) {
  return MAKELPARAM((short)pt.x, (short)pt.y);
}

// Hooks run on this thread between messages, so they only post and return.
static LRESULT CALLBACK PickMouseProc(int code, WPARAM wParam, LPARAM lParam) {
  if (code == HC_ACTION) {
    const MSLLHOOKSTRUCT* m = (const MSLLHOOKSTRUCT*)lParam;
    switch (wParam) {
      case WM_MOUSEMOVE:
        PostMessageW(g.hwnd, WM_PICK_MOVE, 0, PointParam(m->pt));
        break;
      case WM_LBUTTONDOWN:
        g_pickDown = m->pt;
        g_pickLeftDown = true;
        return 1;
      case WM_LBUTTONUP:
        if (g_pickLeftDown) {
          g_pickLeftDown = false;
          PostMessageW(g.hwnd, WM_PICK_DONE, 0, PointParam(g_pickDown));
          return 1;
        }
        break;
      case WM_RBUTTONDOWN:
        g_pickRightDown = true;
        return 1;
      case WM_RBUTTONUP:
        if (g_pickRightDown) {
          g_pickRightDown = false;
          PostMessageW(g.hwnd, WM_PICK_CANCEL, 0, 0);
          return 1;
        }
        break;
    }
  }
  return CallNextHookEx(nullptr, code, wParam, lParam);
}

static LRESULT CALLBACK PickKeysProc(int code, WPARAM wParam, LPARAM lParam) {
  if (code == HC_ACTION && wParam == WM_KEYDOWN &&
      ((const KBDLLHOOKSTRUCT*)lParam)->vkCode == VK_ESCAPE) {
    PostMessageW(g.hwnd, WM_PICK_CANCEL, 0, 0);
    return 1;
  }
  return CallNextHookEx(nullptr, code, wParam, lParam);
}

static HWND PickTargetAt(POINT pt) {
  HWND hwnd = WindowFromPoint(pt);
  if (!hwnd) return nullptr;
  hwnd = GetAncestor(hwnd, GA_ROOT);
  return IsUserWindow(hwnd) ? hwnd : nullptr;
}

// Outlines the window under |pt| (inside its edges, so a maximised window's
// frame does not spill onto the next monitor).
static void UpdatePickFrame(POINT pt) {
  HWND target = PickTargetAt(pt);
  RECT r;
  if (!target || FAILED(DwmGetWindowAttribute(target,
                                              DWMWA_EXTENDED_FRAME_BOUNDS, &r,
                                              sizeof(r)))) {
    ShowWindow(g_pickFrame, SW_HIDE);
    g_pickFrameRect = {0, 0, 0, 0};
    return;
  }
  if (EqualRect(&r, &g_pickFrameRect) && IsWindowVisible(g_pickFrame)) return;
  g_pickFrameRect = r;

  const int t = 4;
  int w = r.right - r.left;
  int h = r.bottom - r.top;
  HRGN outer = CreateRectRgn(0, 0, w, h);
  HRGN inner = CreateRectRgn(t, t, w - t, h - t);
  CombineRgn(outer, outer, inner, RGN_DIFF);
  DeleteObject(inner);
  SetWindowPos(g_pickFrame, HWND_TOPMOST, r.left, r.top, w, h,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);
  SetWindowRgn(g_pickFrame, outer, TRUE);  // the window now owns the region
}

static void StopPick() {
  if (g_pickMouse) UnhookWindowsHookEx(g_pickMouse);
  if (g_pickKeys) UnhookWindowsHookEx(g_pickKeys);
  g_pickMouse = nullptr;
  g_pickKeys = nullptr;
  g_pickLeftDown = g_pickRightDown = false;
  if (g_pickFrame) ShowWindow(g_pickFrame, SW_HIDE);
  g_pickFrameRect = {0, 0, 0, 0};
  KillTimer(g.hwnd, TIMER_PICK_TIMEOUT);
  panel::Invalidate();
}

static void StartPick() {
  if (IsPicking()) return;
  RefreshMonitors();
  if (!FindGlasses()) {
    ShowToast(L"\u26A0  Viture glasses not connected");
    return;
  }
  if (!g_pickFrame) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = g.instance;
    wc.lpszClassName = L"VitureHudPickFrame";
    wc.hbrBackground = CreateSolidBrush(RGB(57, 255, 136));
    RegisterClassExW(&wc);
    g_pickFrame = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT |
            WS_EX_NOACTIVATE,
        L"VitureHudPickFrame", L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
        g.instance, nullptr);
    if (g_pickFrame) SetLayeredWindowAttributes(g_pickFrame, 0, 255, LWA_ALPHA);
  }
  g_pickMouse = SetWindowsHookExW(WH_MOUSE_LL, PickMouseProc, g.instance, 0);
  g_pickKeys = SetWindowsHookExW(WH_KEYBOARD_LL, PickKeysProc, g.instance, 0);
  if (!g_pickMouse || !g_pickFrame) {
    StopPick();
    return;
  }
  SetTimer(g.hwnd, TIMER_PICK_TIMEOUT, 20000, nullptr);
  ShowToast(L"Click the window to show   \u2022   Esc cancels");
  POINT cur;
  if (GetCursorPos(&cur)) UpdatePickFrame(cur);
  panel::Invalidate();
}

static void FinishPick(POINT pt) {
  HWND target = PickTargetAt(pt);
  StopPick();
  if (target) {
    LockToWindow(target);
  } else {
    ShowToast(L"No window there to lock");
  }
}

static void CancelPick() {
  if (!IsPicking()) return;
  StopPick();
  ShowToast(L"Window pick cancelled");
}

static void WriteSetting(const wchar_t* name, bool on);

// Alt+P: picture-in-picture on or off.
static void SetPipOn(bool on) {
  g.pipOn = on;
  WriteSetting(L"Pip", on);
  UpdatePip();
  panel::Invalidate();
  if (!on) {
    ShowToast(L"Picture-in-picture off");
    return;
  }
  RefreshMonitors();
  const MonitorInfo* m = PipTarget();
  if (!m) {
    ShowToast(L"Picture-in-picture needs a second monitor");
  } else if (!viewer::IsRunning()) {
    ShowToast(L"Picture-in-picture on   \u2022   shows with Alt+D");
  } else {
    ShowToast(L"Picture-in-picture: " + ShortLabel(*m));
  }
}

// Alt+W: lock to the active window, or unlock.
static void ToggleWindowLock() {
  if (IsPicking()) {
    CancelPick();
  } else if (IsLocked()) {
    Unlock();
  } else {
    LockToWindow(GetForegroundWindow());
  }
}

// Starts showing a monitor as soon as the glasses appear (plugged in, or
// already connected when the app starts): the monitor the mouse is on, else
// the last one shown. The mouse is left where it is.
static void CheckGlassesArrival() {
  bool present = FindGlasses() != nullptr;
  bool arrived = present && !g.glassesPresent;
  g.glassesPresent = present;
  if (!arrived || !g.autoShow || viewer::IsRunning()) return;

  std::vector<int> candidates = CandidateSources();
  if (candidates.empty()) return;
  int pick = candidates.front();
  for (int idx : candidates) {
    if (idx == g.lastShown) pick = idx;
  }
  POINT cur;
  if (GetCursorPos(&cur)) {
    for (int idx : candidates) {
      const MonitorInfo* m = FindMonitor(idx);
      if (m && PtInRect(&m->bounds, cur)) pick = idx;
    }
  }
  viewer::DebugTrace(L"Glasses arrived, auto-showing DISPLAY%d", pick);
  ShowSource(pick, false);
}

// Resolution, layout or connection changed. The viewer is restarted so its
// window and capture match the new layout.
static void OnDisplayChange() {
  RefreshMonitors();  // also stops the viewer if the glasses are gone
  if (viewer::IsRunning() && g.currentSource >= 0) {
    int current = g.currentSource;
    viewer::Stop();
    g.currentSource = -1;
    ShowSource(current, false);
  }
  CheckGlassesArrival();
}

// ---------------------------------------------------------------------------
// Settings (HKCU\Software\VitureHUD)
// ---------------------------------------------------------------------------
static const wchar_t* kSettingsKey = L"Software\\VitureHUD";

static bool ReadSetting(const wchar_t* name, bool fallback) {
  DWORD value = 0;
  DWORD size = sizeof(value);
  if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD,
                   nullptr, &value, &size) != ERROR_SUCCESS) {
    return fallback;
  }
  return value != 0;
}

static void WriteSetting(const wchar_t* name, bool on) {
  HKEY k = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0,
                      KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) {
    return;
  }
  DWORD value = on ? 1 : 0;
  RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
  RegCloseKey(k);
}

static DWORD ReadSettingNumber(const wchar_t* name, DWORD fallback) {
  DWORD value = 0;
  DWORD size = sizeof(value);
  if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD,
                   nullptr, &value, &size) != ERROR_SUCCESS) {
    return fallback;
  }
  return value;
}

static void WriteSettingNumber(const wchar_t* name, DWORD value) {
  HKEY k = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0,
                      KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) {
    return;
  }
  RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
  RegCloseKey(k);
}

// Edge lens settings: off, then magnification relative to the view.
static const double kLensZooms[] = {0.0, 1.0, 1.5, 2.0};
static const int kLensStepCount = 4;

static void ApplyLens() {
  viewer::SetEdgeLens(kLensZooms[g.lensStep], g.lensLayout);
}

static void SetLensStep(int step) {
  if (step < 0 || step >= kLensStepCount) return;
  g.lensStep = step;
  ApplyLens();
}

// Size and spot are stored as thousandths of the glasses' size.
static void SaveLensLayout() {
  WriteSetting(L"LensFixed", g.lensLayout.fixed);
  WriteSettingNumber(L"LensSize", (DWORD)std::lround(g.lensLayout.size * 1000));
  WriteSettingNumber(L"LensX", (DWORD)std::lround(g.lensLayout.centreX * 1000));
  WriteSettingNumber(L"LensY", (DWORD)std::lround(g.lensLayout.centreY * 1000));
  WriteSettingNumber(L"LensInset", (DWORD)std::lround(g.lensLayout.inset * 1000));
  WriteSettingNumber(L"LensTrigger",
                     (DWORD)std::lround(g.lensLayout.trigger * 1000));
}

static void LoadLensLayout() {
  viewmath::LensLayout l;
  l.fixed = ReadSetting(L"LensFixed", false);
  l.size = ReadSettingNumber(L"LensSize", 333) / 1000.0;
  l.centreX = ReadSettingNumber(L"LensX", 500) / 1000.0;
  l.centreY = ReadSettingNumber(L"LensY", 500) / 1000.0;
  l.inset = ReadSettingNumber(L"LensInset", 120) / 1000.0;
  l.trigger = ReadSettingNumber(L"LensTrigger", 120) / 1000.0;
  g.lensLayout = viewmath::ClampLensLayout(l);
}

// From the control panel's lens editor. While dragging, |commit| is false:
// the glasses show the lens live, and it is saved once the drag ends.
static void SetLensLayout(const viewmath::LensLayout& layout, bool commit) {
  g.lensLayout = viewmath::ClampLensLayout(layout);
  ApplyLens();
  viewer::PreviewEdgeLens(1500);
  if (commit) SaveLensLayout();
}

static void LoadSettings() {
  g.followCaret = ReadSetting(L"FollowCaret", true);
  g.autoShow = ReadSetting(L"AutoShow", true);
  g.pipOn = ReadSetting(L"Pip", false);
  viewer::SetFollowCaret(g.followCaret);
  LoadLensLayout();
  SetLensStep((int)ReadSettingNumber(L"EdgeLens", 1));
}

// ---------------------------------------------------------------------------
// Tray icon + menu
// ---------------------------------------------------------------------------
static void AddTrayIcon() {
  NOTIFYICONDATAW nid = {};
  nid.cbSize = sizeof(nid);
  nid.hWnd = g.hwnd;
  nid.uID = ID_TRAY;
  nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  nid.uCallbackMessage = WM_APP + 1;
  nid.hIcon = g.icon;
  wcsncpy_s(nid.szTip, L"Viture HUD  -  Alt+D cycles the glasses",
            _TRUNCATE);
  Shell_NotifyIconW(NIM_ADD, &nid);
}

static void RemoveTrayIcon() {
  NOTIFYICONDATAW nid = {};
  nid.cbSize = sizeof(nid);
  nid.hWnd = g.hwnd;
  nid.uID = ID_TRAY;
  Shell_NotifyIconW(NIM_DELETE, &nid);
}

// ---- autostart (HKCU Run key) -------------------------------------------
static std::wstring ExePath() {
  wchar_t buf[MAX_PATH] = {0};
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return buf;
}

static bool IsAutostartEnabled() {
  HKEY k = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                    0, KEY_READ, &k) != ERROR_SUCCESS) {
    return false;
  }
  wchar_t val[MAX_PATH * 2] = {0};
  DWORD size = sizeof(val);
  DWORD type = 0;
  bool has = RegQueryValueExW(k, L"VitureHUD", nullptr, &type, (LPBYTE)val,
                              &size) == ERROR_SUCCESS;
  RegCloseKey(k);
  return has;
}

static void SetAutostart(bool enable) {
  HKEY k = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &k,
                      nullptr) != ERROR_SUCCESS) {
    return;
  }
  if (enable) {
    std::wstring quoted = L"\"" + ExePath() + L"\"";
    RegSetValueExW(k, L"VitureHUD", 0, REG_SZ, (const BYTE*)quoted.c_str(),
                   (DWORD)((quoted.size() + 1) * sizeof(wchar_t)));
  } else {
    RegDeleteValueW(k, L"VitureHUD");
  }
  RegCloseKey(k);
}

// ---- desktop shortcut ----------------------------------------------------
static void CreateDesktopShortcut() {
  // Ask the shell to create a .lnk, so the icon and start-in folder are right.
  wchar_t desktop[MAX_PATH] = {0};
  if (FAILED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0,
                              desktop))) {
    return;
  }
  std::wstring lnk = std::wstring(desktop) + L"\\Viture HUD.lnk";
  std::wstring exe = ExePath();

  IShellLinkW* link = nullptr;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                              IID_IShellLinkW, (void**)&link))) {
    return;
  }
  link->SetPath(exe.c_str());
  wchar_t dir[MAX_PATH] = {0};
  wcscpy_s(dir, exe.c_str());
  for (int i = (int)wcslen(dir) - 1; i >= 0; --i) {
    if (dir[i] == L'\\') { dir[i] = L'\0'; break; }
  }
  link->SetWorkingDirectory(dir);
  link->SetDescription(L"Viture HUD - show monitors on the glasses (Alt+D)");

  IPersistFile* pf = nullptr;
  if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void**)&pf))) {
    pf->Save(lnk.c_str(), TRUE);
    pf->Release();
  }
  link->Release();
  ShowBalloon(kAppName, L"Shortcut created on the Desktop.");
}

static void ShowContextMenu() {
  RefreshMonitors();

  HMENU menu = CreatePopupMenu();
  HMENU sources = CreatePopupMenu();

  std::vector<int> candidates = CandidateSources();
  if (candidates.empty()) {
    AppendMenuW(sources, MF_STRING | MF_GRAYED, 0, L"No monitors found");
  } else {
    for (int index : candidates) {
      const MonitorInfo* m = FindMonitor(index);
      std::wstring text = m ? m->label : L"DISPLAY" + std::to_wstring(index);
      UINT flags = MF_STRING;
      if (index == g.currentSource) flags |= MF_CHECKED;
      AppendMenuW(sources, flags, MENU_CYCLE + 100 + index, text.c_str());
    }
  }

  bool hasGlasses = FindGlasses() != nullptr;
  std::wstring status = hasGlasses
                            ? L"Viture glasses connected"
                            : L"Viture glasses not detected";

  AppendMenuW(menu, MF_STRING, MENU_OPEN, L"Open Viture HUD");
  SetMenuDefaultItem(menu, MENU_OPEN, FALSE);
  AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, status.c_str());
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_POPUP, (UINT_PTR)sources, L"Show on glasses");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, MENU_CYCLE, L"Next monitor\tAlt+D");
  AppendMenuW(menu, MF_STRING, MENU_STOP, L"Stop mirroring\tCtrl+Alt+D");
  AppendMenuW(menu, MF_STRING, MENU_REFRESH, L"Refresh monitors");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING | (IsAutostartEnabled() ? MF_CHECKED : 0),
              MENU_AUTOSTART, L"Start with Windows");
  AppendMenuW(menu, MF_STRING, MENU_SHORTCUT, L"Create Desktop shortcut");
  AppendMenuW(menu, MF_STRING, MENU_ABOUT, L"About");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, MENU_EXIT, L"Exit");

  POINT pt;
  GetCursorPos(&pt);
  SetForegroundWindow(g.hwnd);
  TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g.hwnd,
                 nullptr);
  PostMessageW(g.hwnd, WM_NULL, 0, 0);

  DestroyMenu(sources);
  DestroyMenu(menu);
}

// ---------------------------------------------------------------------------
// Control panel
// ---------------------------------------------------------------------------
static panel::State PanelState() {
  panel::State s;
  s.glassesConnected = FindGlasses() != nullptr;
  s.showing = viewer::IsRunning();
  s.shownIndex = s.showing ? g.currentSource : -1;
  for (const auto& m : g.monitors) {
    panel::Display d;
    d.displayIndex = m.displayIndex;
    d.device = m.deviceName;
    d.name = m.isViture ? L"VITURE" : ShortLabel(m);
    d.bounds = m.bounds;
    d.isGlasses = m.isViture;
    d.isPrimary = m.isPrimary;
    s.displays.push_back(d);
  }
  s.zoomStep = viewer::ZoomStep();
  s.locked = IsLocked();
  if (s.locked) s.lockedTitle = viewer::LockedTitle();
  s.picking = IsPicking();
  s.pipOn = g.pipOn;
  s.lensStep = g.lensStep;
  s.lensLayout = g.lensLayout;
  if (const MonitorInfo* glasses = FindGlasses()) s.glassesBounds = glasses->bounds;
  if (const MonitorInfo* m = PipTarget()) s.pipName = ShortLabel(*m);
  s.followCaret = g.followCaret;
  s.autoShow = g.autoShow;
  s.startWithWindows = IsAutostartEnabled();
  viewer::Stats stats = viewer::GetStats();
  s.fps = stats.fps;
  s.duplication = stats.duplication;
  s.scale = stats.scale;
  return s;
}

// Where the panel opens: the monitor shown on the glasses, else the one under
// the mouse, else the primary. Never the glasses, where the viewer covers it.
static RECT PanelHome() {
  if (viewer::IsRunning()) {
    if (const MonitorInfo* m = FindMonitor(g.currentSource)) return m->bounds;
  }
  POINT cur;
  if (GetCursorPos(&cur)) {
    for (const auto& m : g.monitors) {
      if (!m.isViture && PtInRect(&m.bounds, cur)) return m.bounds;
    }
  }
  for (const auto& m : g.monitors) {
    if (m.isPrimary && !m.isViture) return m.bounds;
  }
  for (const auto& m : g.monitors) {
    if (!m.isViture) return m.bounds;
  }
  RECT fallback = {0, 0, GetSystemMetrics(SM_CXSCREEN),
                   GetSystemMetrics(SM_CYSCREEN)};
  return fallback;
}

static void InitPanel() {
  panel::Actions a;
  a.getState = PanelState;
  // Clicks in the panel never move the mouse: it is on the panel.
  a.showDisplay = [](int index) { ShowSource(index, false); };
  a.next = [] { Cycle(+1, false); };
  a.stop = [] { StopMirroring(); };
  a.setZoom = [](int step) { viewer::SetZoomStep(step); };
  a.toggleLock = [] {
    if (IsLocked()) {
      Unlock();
    } else if (IsPicking()) {
      CancelPick();
    } else {
      StartPick();
    }
  };
  a.setPip = [](bool on) { SetPipOn(on); };
  a.setLensStep = [](int step) {
    SetLensStep(step);
    WriteSettingNumber(L"EdgeLens", (DWORD)g.lensStep);
  };
  a.setLensLayout = [](const viewmath::LensLayout& layout, bool commit) {
    SetLensLayout(layout, commit);
  };
  a.setFollowCaret = [](bool on) {
    g.followCaret = on;
    viewer::SetFollowCaret(on);
    WriteSetting(L"FollowCaret", on);
  };
  a.setAutoShow = [](bool on) {
    g.autoShow = on;
    WriteSetting(L"AutoShow", on);
  };
  a.setStartWithWindows = [](bool on) { SetAutostart(on); };
  panel::Init(g.instance, g.icon, a);
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                LPARAM lParam) {
  switch (msg) {
    case WM_APP + 1:  // tray callback
      // Left click opens the control panel, right click the menu.
      if (LOWORD(lParam) == WM_LBUTTONUP) {
        panel::Show(PanelHome());
      } else if (LOWORD(lParam) == WM_RBUTTONUP) {
        ShowContextMenu();
      }
      return 0;

    case WM_HOTKEY:
      if (wParam == HOTKEY_CYCLE) {
        Cycle(+1, true);
      } else if (wParam == HOTKEY_PREV) {
        Cycle(-1, true);
      } else if (wParam == HOTKEY_STOP) {
        StopMirroring();
      } else if (wParam == HOTKEY_ZOOM_NEXT || wParam == HOTKEY_ZOOM_PREV) {
        if (viewer::IsRunning()) {
          viewer::StepZoom(wParam == HOTKEY_ZOOM_NEXT ? +1 : -1);
        } else {
          ShowToast(L"Show a monitor first (Alt+D)");
        }
      } else if (wParam == HOTKEY_LOCK) {
        ToggleWindowLock();
      } else if (wParam == HOTKEY_PIP) {
        SetPipOn(!g.pipOn);
      }
      return 0;

    case WM_PICK_MOVE:
      if (IsPicking()) {
        UpdatePickFrame({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
      }
      return 0;

    case WM_PICK_DONE:
      if (IsPicking()) FinishPick({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
      return 0;

    case WM_PICK_CANCEL:
      CancelPick();
      return 0;

    case WM_DISPLAYCHANGE:
      // Several of these arrive while Windows settles; act once it is quiet.
      SetTimer(hwnd, TIMER_DISPLAY_CHANGE, 1500, nullptr);
      return 0;

    case WM_TIMER:
      if (wParam == TIMER_TOAST) {
        KillTimer(hwnd, TIMER_TOAST);
        if (g_toast) ShowWindow(g_toast, SW_HIDE);
      } else if (wParam == TIMER_RESCAN) {
        RefreshMonitors();
        CheckGlassesArrival();
      } else if (wParam == TIMER_PICK_TIMEOUT) {
        CancelPick();
      } else if (wParam == TIMER_DISPLAY_CHANGE) {
        KillTimer(hwnd, TIMER_DISPLAY_CHANGE);
        OnDisplayChange();
      }
      return 0;

    case WM_COMMAND: {
      int id = LOWORD(wParam);
      if (id == MENU_OPEN) {
        panel::Show(PanelHome());
      } else if (id == MENU_ABOUT) {
        panel::ShowAbout(PanelHome());
      } else if (id == MENU_CYCLE) {
        Cycle(+1, true);
      } else if (id == MENU_STOP) {
        StopMirroring();
      } else if (id == MENU_REFRESH) {
        RefreshMonitors();
        ShowToast(L"Monitors refreshed");
      } else if (id == MENU_AUTOSTART) {
        SetAutostart(!IsAutostartEnabled());
      } else if (id == MENU_SHORTCUT) {
        CreateDesktopShortcut();
      } else if (id == MENU_EXIT) {
        DestroyWindow(hwnd);
      } else if (id >= MENU_CYCLE + 100) {
        ShowSource(id - MENU_CYCLE - 100, true);
      }
      return 0;
    }

    case WM_DESTROY:
      RemoveTrayIcon();
      UnregisterHotKey(hwnd, HOTKEY_CYCLE);
      UnregisterHotKey(hwnd, HOTKEY_PREV);
      UnregisterHotKey(hwnd, HOTKEY_STOP);
      UnregisterHotKey(hwnd, HOTKEY_ZOOM_NEXT);
      UnregisterHotKey(hwnd, HOTKEY_ZOOM_PREV);
      UnregisterHotKey(hwnd, HOTKEY_LOCK);
      UnregisterHotKey(hwnd, HOTKEY_PIP);
      StopPick();
      panel::Close();
      viewer::Stop();
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  g.instance = instance;

  // Single instance: a second copy would fight over the hotkeys.
  HANDLE mutex = CreateMutexW(nullptr, TRUE, L"VitureHudSingleInstance");
  if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
    return 0;
  }

  // Work in real pixels on every monitor. Desktop Duplication, window bounds
  // and the cursor must all agree, which they only do when Windows is not
  // scaling coordinates for us. Looked up at run time: Windows 10 1703+.
  using SetDpiContextFn = BOOL(WINAPI*)(HANDLE);
  FARPROC setDpi = GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                  "SetProcessDpiAwarenessContext");
  if (setDpi) {
    SetDpiContextFn fn;
    memcpy(&fn, &setDpi, sizeof(fn));
    fn((HANDLE)-4);  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
  }

  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = instance;
  wc.lpszClassName = kClassName;
  if (!RegisterClassExW(&wc)) return 1;

  g.hwnd = CreateWindowExW(0, kClassName, kAppName, 0, 0, 0, 0, 0, nullptr,
                           nullptr, instance, nullptr);
  if (!g.hwnd) return 1;

  // Tray icon: use the app icon when available, else the system one.
  g.icon = LoadIconW(instance, L"IDI_APP");
  if (!g.icon) g.icon = LoadIconW(nullptr, IDI_APPLICATION);

  AddTrayIcon();
  LoadSettings();
  InitPanel();
  RefreshMonitors();

  // Alt+D cycles, Alt+Shift+D goes back, Ctrl+Alt+D stops,
  // Alt+Z / Alt+Shift+Z zoom, Alt+W locks to the active window, Alt+P
  // toggles picture-in-picture.
  g.hotkeysOk = true;
  if (!RegisterHotKey(g.hwnd, HOTKEY_CYCLE, MOD_ALT | MOD_NOREPEAT, 'D')) {
    g.hotkeysOk = false;
  }
  RegisterHotKey(g.hwnd, HOTKEY_PREV, MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'D');
  RegisterHotKey(g.hwnd, HOTKEY_STOP, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'D');
  RegisterHotKey(g.hwnd, HOTKEY_ZOOM_NEXT, MOD_ALT | MOD_NOREPEAT, 'Z');
  RegisterHotKey(g.hwnd, HOTKEY_ZOOM_PREV, MOD_ALT | MOD_SHIFT | MOD_NOREPEAT,
                 'Z');
  RegisterHotKey(g.hwnd, HOTKEY_LOCK, MOD_ALT | MOD_NOREPEAT, 'W');
  RegisterHotKey(g.hwnd, HOTKEY_PIP, MOD_ALT | MOD_NOREPEAT, 'P');

  if (!g.hotkeysOk) {
    ShowBalloon(kAppName, L"Alt+D is already used by another app.");
  }

  // Re-scan displays periodically so plugging the glasses in is noticed, and
  // start right away if they are already connected.
  SetTimer(g.hwnd, TIMER_RESCAN, 4000, nullptr);
  CheckGlassesArrival();

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  if (mutex) CloseHandle(mutex);
  return 0;
}