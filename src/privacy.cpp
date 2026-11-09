// Copyright (C) 2026 Jurica Dujmovic
// SPDX-License-Identifier: GPL-3.0-or-later
//
// privacy.cpp - Privacy mode. See privacy.h.

#include "privacy.h"

#include <oleauto.h>
#include <wbemidl.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

#include "glasses_id.h"
#include "viewer.h"

namespace privacy {

namespace {

constexpr wchar_t kCoverClass[] = L"VitureHudPrivacyCover";
constexpr UINT_PTR kTopTimer = 1;
constexpr UINT kTopMs = 250;  // how often a cover makes sure it is on top

// SetWindowDisplayAffinity: shown on the monitor, left out of every capture.
constexpr DWORD kExcludeFromCapture = 0x11;  // WDA_EXCLUDEFROMCAPTURE

// Crash recovery, next to the settings. Not settings themselves: they exist
// only while Privacy mode has something to undo.
constexpr wchar_t kStateKey[] = L"Software\\VitureHUD";
constexpr wchar_t kRestoreBrightness[] = L"PrivacyRestoreBrightness";
constexpr wchar_t kRestoreDisplays[] = L"PrivacyRestoreDisplays";

Mode g_mode = Mode::kOff;
Options g_options;
std::wstring g_glasses;
std::wstring g_note;
std::vector<HWND> g_covers;
std::vector<RECT> g_coverRects;

// The screen "Main screen" means, as found by Start (\\.\DISPLAYn).
std::wstring g_mainDevice;
bool g_mainSwitched = false;
std::wstring g_summary;

// While switched off: the layout to go back to, and each stand-in in use.
struct InUse {
  std::wstring id;      // its monitor device path
  std::wstring device;  // its GDI name while on
};
std::vector<DISPLAYCONFIG_PATH_INFO> g_savedPaths;
std::vector<DISPLAYCONFIG_MODE_INFO> g_savedModes;
std::vector<InUse> g_standIns;

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------
void WriteState(const wchar_t* name, DWORD value) {
  HKEY k = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kStateKey, 0, nullptr, 0, KEY_SET_VALUE,
                      nullptr, &k, nullptr) != ERROR_SUCCESS) {
    return;
  }
  RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
  RegCloseKey(k);
}

bool ReadState(const wchar_t* name, DWORD* value) {
  DWORD size = sizeof(*value);
  return RegGetValueW(HKEY_CURRENT_USER, kStateKey, name, RRF_RT_REG_DWORD,
                      nullptr, value, &size) == ERROR_SUCCESS;
}

void DeleteState(const wchar_t* name) {
  RegDeleteKeyValueW(HKEY_CURRENT_USER, kStateKey, name);
}

// ---------------------------------------------------------------------------
// Built-in screen brightness, through WMI. WMI can take a few hundred
// milliseconds, so it runs on its own thread: the hotkey reacts at once.
// ---------------------------------------------------------------------------
IWbemServices* ConnectWmi() {
  IWbemLocator* locator = nullptr;
  if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                              IID_IWbemLocator, (void**)&locator))) {
    return nullptr;
  }
  IWbemServices* services = nullptr;
  BSTR ns = SysAllocString(L"ROOT\\WMI");
  HRESULT hr = locator->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr,
                                      nullptr, &services);
  SysFreeString(ns);
  locator->Release();
  if (FAILED(hr)) return nullptr;
  CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                    RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                    nullptr, EOAC_NONE);
  return services;
}

IEnumWbemClassObject* Query(IWbemServices* services, const wchar_t* wql) {
  BSTR language = SysAllocString(L"WQL");
  BSTR query = SysAllocString(wql);
  IEnumWbemClassObject* items = nullptr;
  HRESULT hr = services->ExecQuery(
      language, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
      nullptr, &items);
  SysFreeString(query);
  SysFreeString(language);
  if (FAILED(hr)) return nullptr;
  CoSetProxyBlanket(items, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                    RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                    nullptr, EOAC_NONE);
  return items;
}

bool ReadBrightness(IWbemServices* services, int* level) {
  IEnumWbemClassObject* items = Query(
      services,
      L"SELECT CurrentBrightness FROM WmiMonitorBrightness WHERE Active=TRUE");
  if (!items) return false;
  bool ok = false;
  IWbemClassObject* item = nullptr;
  ULONG got = 0;
  if (SUCCEEDED(items->Next(3000, 1, &item, &got)) && got == 1) {
    VARIANT v;
    VariantInit(&v);
    if (SUCCEEDED(item->Get(L"CurrentBrightness", 0, &v, nullptr, nullptr)) &&
        v.vt == VT_UI1) {
      *level = v.bVal;
      ok = true;
    }
    VariantClear(&v);
    item->Release();
  }
  items->Release();
  return ok;
}

bool SetBrightness(IWbemServices* services, int level) {
  IWbemClassObject* methods = nullptr;
  BSTR className = SysAllocString(L"WmiMonitorBrightnessMethods");
  HRESULT hr = services->GetObject(className, 0, nullptr, &methods, nullptr);
  SysFreeString(className);
  if (FAILED(hr)) return false;
  IWbemClassObject* signature = nullptr;
  hr = methods->GetMethod(L"WmiSetBrightness", 0, &signature, nullptr);
  methods->Release();
  if (FAILED(hr) || !signature) return false;
  IWbemClassObject* args = nullptr;
  hr = signature->SpawnInstance(0, &args);
  signature->Release();
  if (FAILED(hr)) return false;
  VARIANT timeout;
  VariantInit(&timeout);
  timeout.vt = VT_I4;
  timeout.lVal = 0;
  args->Put(L"Timeout", 0, &timeout, 0);
  VARIANT brightness;
  VariantInit(&brightness);
  brightness.vt = VT_UI1;
  brightness.bVal = (BYTE)(level < 0 ? 0 : (level > 100 ? 100 : level));
  args->Put(L"Brightness", 0, &brightness, 0);

  bool ok = false;
  IEnumWbemClassObject* items = Query(
      services, L"SELECT * FROM WmiMonitorBrightnessMethods WHERE Active=TRUE");
  if (items) {
    BSTR method = SysAllocString(L"WmiSetBrightness");
    for (;;) {
      IWbemClassObject* item = nullptr;
      ULONG got = 0;
      if (FAILED(items->Next(3000, 1, &item, &got)) || got != 1) break;
      VARIANT path;
      VariantInit(&path);
      if (SUCCEEDED(item->Get(L"__PATH", 0, &path, nullptr, nullptr)) &&
          path.vt == VT_BSTR) {
        ok |= SUCCEEDED(services->ExecMethod(path.bstrVal, method, 0, nullptr,
                                             args, nullptr, nullptr));
      }
      VariantClear(&path);
      item->Release();
    }
    SysFreeString(method);
    items->Release();
  }
  args->Release();
  return ok;
}

// The thread moves the real brightness towards |want|. |dimmed| is true once
// it has saved the old level and set 0%, and false again once it is back.
struct Dimmer {
  std::mutex lock;
  std::condition_variable changed;
  bool started = false;
  bool want = false;
  bool dimmed = false;
};
Dimmer g_dim;

void DimmerLoop() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  std::unique_lock<std::mutex> hold(g_dim.lock);
  for (;;) {
    g_dim.changed.wait(hold, [] { return g_dim.want != g_dim.dimmed; });
    bool want = g_dim.want;
    hold.unlock();
    IWbemServices* services = ConnectWmi();
    if (services) {
      if (want) {
        int level = 0;
        DWORD saved = 0;
        // A level saved earlier is the real one: never overwrite it with 0.
        if (!ReadState(kRestoreBrightness, &saved) &&
            ReadBrightness(services, &level)) {
          WriteState(kRestoreBrightness, (DWORD)level);
        }
        if (ReadState(kRestoreBrightness, &saved)) SetBrightness(services, 0);
      } else {
        DWORD saved = 0;
        if (ReadState(kRestoreBrightness, &saved)) {
          SetBrightness(services, (int)saved);
          DeleteState(kRestoreBrightness);
        }
      }
      services->Release();
    } else {
      viewer::DebugTrace(L"Privacy: brightness control unavailable");
    }
    hold.lock();
    g_dim.dimmed = want;
    g_dim.changed.notify_all();
  }
}

void SetDim(bool on) {
  std::lock_guard<std::mutex> hold(g_dim.lock);
  g_dim.want = on;
  if (!g_dim.started) {
    g_dim.started = true;
    std::thread(DimmerLoop).detach();
  }
  g_dim.changed.notify_all();
}

// ---------------------------------------------------------------------------
// Display layout (CCD)
// ---------------------------------------------------------------------------
bool QueryLayout(UINT32 flags, std::vector<DISPLAYCONFIG_PATH_INFO>* paths,
                 std::vector<DISPLAYCONFIG_MODE_INFO>* modes) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(flags, &pathCount, &modeCount) !=
        ERROR_SUCCESS) {
      return false;
    }
    paths->resize(pathCount);
    modes->resize(modeCount);
    LONG r = QueryDisplayConfig(flags, &pathCount, paths->data(), &modeCount,
                                modes->data(), nullptr);
    if (r == ERROR_INSUFFICIENT_BUFFER) continue;  // changed in between
    if (r != ERROR_SUCCESS) return false;
    paths->resize(pathCount);
    modes->resize(modeCount);
    return true;
  }
  return false;
}

bool SameLuid(const LUID& a, const LUID& b) {
  return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

// Laptop panels report one of these as their connection.
bool IsBuiltInTarget(const DISPLAYCONFIG_PATH_INFO& p) {
  UINT32 tech = (UINT32)p.targetInfo.outputTechnology;
  return tech == 0x80000000u /* INTERNAL */ || tech == 11 /* DP embedded */ ||
         tech == 13 /* UDI embedded */ || tech == 6 /* LVDS */;
}

std::wstring SourceName(const DISPLAYCONFIG_PATH_INFO& p) {
  DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {};
  src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
  src.header.size = sizeof(src);
  src.header.adapterId = p.sourceInfo.adapterId;
  src.header.id = p.sourceInfo.id;
  if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) return L"";
  return src.viewGdiDeviceName;
}

// The monitor's device path (stable), and its own name.
std::wstring TargetPath(const DISPLAYCONFIG_PATH_INFO& p, std::wstring* name) {
  DISPLAYCONFIG_TARGET_DEVICE_NAME tgt = {};
  tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
  tgt.header.size = sizeof(tgt);
  tgt.header.adapterId = p.targetInfo.adapterId;
  tgt.header.id = p.targetInfo.id;
  if (DisplayConfigGetDeviceInfo(&tgt.header) != ERROR_SUCCESS) return L"";
  if (name) *name = tgt.monitorFriendlyDeviceName;
  return tgt.monitorDevicePath;
}

// True when the target belongs to a software display adapter, which is what
// virtual display drivers create (Windows enumerates them under ROOT\ or
// SWD\). A real graphics card or USB display adapter drives a real screen:
// using one of those as the stand-in would light it up with your work.
bool IsVirtualAdapter(const LUID& adapter) {
  DISPLAYCONFIG_ADAPTER_NAME name = {};
  name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME;
  name.header.size = sizeof(name);
  name.header.adapterId = adapter;
  if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS) return false;
  std::wstring path = name.adapterDevicePath;
  for (auto& c : path) c = (wchar_t)towupper(c);
  return path.find(L"\\\\?\\ROOT#") == 0 || path.find(L"\\\\?\\SWD#") == 0;
}

// "\\?\DISPLAY#VIT1234#..." -> "VIT", the EDID manufacturer id.
std::wstring ManufacturerFromPath(const std::wstring& path) {
  size_t at = path.find(L"DISPLAY#");
  if (at == std::wstring::npos || at + 11 > path.size()) return L"";
  return path.substr(at + 8, 3);
}

// Display scaling ("Scale" in Settings > Display), per screen. Windows has
// no documented call for it; Settings itself uses these two undocumented
// DisplayConfig requests, which work on Windows 10 1607 and later. Scaling is
// told relative to the screen's recommended step: 0 is the recommended one,
// -1 the step below, and so on.
constexpr int kScales[] = {100, 125, 150, 175, 200, 225,
                           250, 300, 350, 400, 450, 500};
constexpr int kScaleCount = (int)(sizeof(kScales) / sizeof(kScales[0]));
constexpr auto kGetScale = (DISPLAYCONFIG_DEVICE_INFO_TYPE)-3;
constexpr auto kSetScale = (DISPLAYCONFIG_DEVICE_INFO_TYPE)-4;

struct GetScaleInfo {
  DISPLAYCONFIG_DEVICE_INFO_HEADER header;
  INT32 minRel;  // the lowest step, relative to the recommended one (<= 0)
  INT32 curRel;
  INT32 maxRel;
};
struct SetScaleInfo {
  DISPLAYCONFIG_DEVICE_INFO_HEADER header;
  INT32 rel;
};

bool ReadScale(const DISPLAYCONFIG_PATH_INFO& p, GetScaleInfo* info) {
  *info = {};
  info->header.type = kGetScale;
  info->header.size = sizeof(*info);
  info->header.adapterId = p.sourceInfo.adapterId;
  info->header.id = p.sourceInfo.id;
  return DisplayConfigGetDeviceInfo(&info->header) == ERROR_SUCCESS;
}

// The screen's scaling in percent, or 0 if Windows does not say.
int ScalePercent(const DISPLAYCONFIG_PATH_INFO& p) {
  GetScaleInfo info;
  if (!ReadScale(p, &info)) return 0;
  int at = -info.minRel + info.curRel;  // the recommended step is at -minRel
  return at >= 0 && at < kScaleCount ? kScales[at] : 0;
}

bool SetScalePercent(const DISPLAYCONFIG_PATH_INFO& p, int percent) {
  GetScaleInfo info;
  if (!ReadScale(p, &info)) return false;
  int want = -1;
  for (int i = 0; i < kScaleCount; ++i) {
    if (kScales[i] == percent) want = i;
  }
  int rel = want - (-info.minRel);
  if (want < 0 || rel < info.minRel || rel > info.maxRel) return false;
  if (rel == info.curRel) return true;
  SetScaleInfo set = {};
  set.header.type = kSetScale;
  set.header.size = sizeof(set);
  set.header.adapterId = p.sourceInfo.adapterId;
  set.header.id = p.sourceInfo.id;
  set.rel = rel;
  return DisplayConfigSetDeviceInfo(&set.header) == ERROR_SUCCESS;
}

std::wstring Size(UINT32 w, UINT32 h) {
  return std::to_wstring(w) + L" \u00D7 " + std::to_wstring(h);
}

// Puts back the layout from before the switch. Windows' saved layout is the
// fallback: the switch was never saved to it.
void RestoreDisplays() {
  LONG r = ERROR_GEN_FAILURE;
  if (!g_savedPaths.empty()) {
    r = SetDisplayConfig((UINT32)g_savedPaths.size(), g_savedPaths.data(),
                         (UINT32)g_savedModes.size(), g_savedModes.data(),
                         SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG |
                             SDC_ALLOW_CHANGES);
  }
  if (r != ERROR_SUCCESS) {
    r = SetDisplayConfig(0, nullptr, 0, nullptr,
                         SDC_APPLY | SDC_USE_DATABASE_CURRENT);
  }
  viewer::DebugTrace(L"Privacy: displays restored (%ld)", r);
  g_savedPaths.clear();
  g_savedModes.clear();
  g_standIns.clear();
  DeleteState(kRestoreDisplays);
}

bool IsVirtualTarget(const DISPLAYCONFIG_PATH_INFO& p) {
  return IsVirtualAdapter(p.targetInfo.adapterId);
}

const DISPLAYCONFIG_SOURCE_MODE* SourceModeOf(
    const DISPLAYCONFIG_PATH_INFO& p,
    const std::vector<DISPLAYCONFIG_MODE_INFO>& modes) {
  UINT32 idx = p.sourceInfo.modeInfoIdx;
  if (idx >= modes.size() ||
      modes[idx].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
    return nullptr;
  }
  return &modes[idx].sourceMode;
}

// The screen "Main screen" means: the built-in one if it is on, otherwise
// the primary one, otherwise any. Never the glasses. -1 if there is none.
int MainScreen(const std::vector<DISPLAYCONFIG_PATH_INFO>& active,
               const std::vector<DISPLAYCONFIG_MODE_INFO>& modes) {
  int primary = -1, any = -1;
  for (size_t i = 0; i < active.size(); ++i) {
    if (SourceName(active[i]) == g_glasses) continue;
    if (IsBuiltInTarget(active[i])) return (int)i;
    const DISPLAYCONFIG_SOURCE_MODE* m = SourceModeOf(active[i], modes);
    if (m && m->position.x == 0 && m->position.y == 0 && primary < 0) {
      primary = (int)i;
    }
    if (any < 0) any = (int)i;
  }
  return primary >= 0 ? primary : any;
}

// In one display change, a stand-in takes each target screen's exact place,
// size and (if it was) primary role, and the target goes off. |targets| are
// indexes into the active paths; the preferred stand-in is used first, then
// any other spare virtual display. |switched| gets the GDI names of the
// screens that went off. A target with no stand-in left is not switched.
bool SwitchOff(const std::vector<size_t>& targets, const std::wstring& preferred,
               std::vector<std::wstring>* switched, std::wstring* error,
               std::wstring* note) {
  std::vector<DISPLAYCONFIG_PATH_INFO> active;
  std::vector<DISPLAYCONFIG_MODE_INFO> activeModes;
  std::vector<DISPLAYCONFIG_PATH_INFO> all;
  std::vector<DISPLAYCONFIG_MODE_INFO> allModes;
  if (!QueryLayout(QDC_ONLY_ACTIVE_PATHS, &active, &activeModes) ||
      !QueryLayout(QDC_ALL_PATHS, &all, &allModes)) {
    *error = L"Windows did not report the display layout.";
    return false;
  }

  // Spare outputs: off, connected, on a virtual adapter, not the glasses,
  // and with a source nothing else uses. One path per stand-in display.
  std::vector<const DISPLAYCONFIG_PATH_INFO*> spare;
  std::vector<std::wstring> spareIds;
  auto sourceTaken = [&](const DISPLAYCONFIG_PATH_INFO& p) {
    for (const auto& a : active) {
      if (SameLuid(a.sourceInfo.adapterId, p.sourceInfo.adapterId) &&
          a.sourceInfo.id == p.sourceInfo.id) {
        return true;
      }
    }
    for (const auto* s : spare) {
      if (SameLuid(s->sourceInfo.adapterId, p.sourceInfo.adapterId) &&
          s->sourceInfo.id == p.sourceInfo.id) {
        return true;
      }
    }
    return false;
  };
  for (int pass = 0; pass < 2; ++pass) {  // the preferred one first
    for (const auto& p : all) {
      if ((p.flags & DISPLAYCONFIG_PATH_ACTIVE) || !p.targetInfo.targetAvailable ||
          IsBuiltInTarget(p) || !IsVirtualTarget(p)) {
        continue;
      }
      std::wstring name;
      std::wstring id = TargetPath(p, &name);
      if (id.empty() || (pass == 0) != (id == preferred)) continue;
      if (glassesid::IsVitureDisplay(name, ManufacturerFromPath(id))) continue;
      bool on = false;
      for (const auto& a : active) on |= TargetPath(a, nullptr) == id;
      bool seen = false;
      for (const auto& s : spareIds) seen |= s == id;
      if (on || seen || sourceTaken(p)) continue;
      spare.push_back(&p);
      spareIds.push_back(id);
    }
  }
  if (spare.empty() || spareIds[0] != preferred) {
    bool inUse = false;
    for (const auto& a : active) inUse |= TargetPath(a, nullptr) == preferred;
    *error = inUse ? L"The stand-in is in use as a screen. Disconnect it in Settings."
                   : L"The stand-in is missing, or not disconnected in Settings.";
    return false;
  }

  // Which target gets which stand-in. Virtual screens are nobody's to read,
  // so they are left alone.
  struct Pair {
    size_t target;
    const DISPLAYCONFIG_PATH_INFO* standIn;
    std::wstring id;
    DISPLAYCONFIG_SOURCE_MODE screen;
    int scale;  // the screen's scaling, %, 0 if unknown
  };
  std::vector<Pair> pairs;
  for (size_t t : targets) {
    if (pairs.size() == spare.size()) break;
    if (t >= active.size() || IsVirtualTarget(active[t])) continue;
    const DISPLAYCONFIG_SOURCE_MODE* m = SourceModeOf(active[t], activeModes);
    if (!m) continue;
    pairs.push_back({t, spare[pairs.size()], spareIds[pairs.size()], *m,
                     ScalePercent(active[t])});
  }
  if (pairs.empty()) {
    *error = L"There is no screen to switch off.";
    return false;
  }

  // The new layout: every active path but the targets', plus the stand-ins
  // with the targets' source modes. Mode indexes are renumbered, since the
  // targets' modes are left out.
  std::vector<DISPLAYCONFIG_PATH_INFO> paths;
  std::vector<DISPLAYCONFIG_MODE_INFO> modes;
  std::map<UINT32, UINT32> renumber;
  auto keep = [&](UINT32 idx) -> UINT32 {
    if (idx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID || idx >= activeModes.size()) {
      return DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    }
    auto it = renumber.find(idx);
    if (it != renumber.end()) return it->second;
    modes.push_back(activeModes[idx]);
    renumber[idx] = (UINT32)modes.size() - 1;
    return (UINT32)modes.size() - 1;
  };
  for (size_t i = 0; i < active.size(); ++i) {
    bool goesOff = false;
    for (const auto& pr : pairs) goesOff |= pr.target == i;
    if (goesOff) continue;
    DISPLAYCONFIG_PATH_INFO p = active[i];
    p.sourceInfo.modeInfoIdx = keep(p.sourceInfo.modeInfoIdx);
    p.targetInfo.modeInfoIdx = keep(p.targetInfo.modeInfoIdx);
    paths.push_back(p);
  }
  std::vector<std::wstring> goingOff;
  for (const auto& pr : pairs) {
    goingOff.push_back(SourceName(active[pr.target]));
    DISPLAYCONFIG_PATH_INFO s = *pr.standIn;
    s.flags = DISPLAYCONFIG_PATH_ACTIVE;
    s.targetInfo.rotation = DISPLAYCONFIG_ROTATION_IDENTITY;
    s.targetInfo.scaling = DISPLAYCONFIG_SCALING_PREFERRED;
    s.targetInfo.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_UNSPECIFIED;
    s.targetInfo.refreshRate.Numerator = 0;
    s.targetInfo.refreshRate.Denominator = 0;
    s.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    DISPLAYCONFIG_MODE_INFO sm = {};
    sm.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
    sm.id = s.sourceInfo.id;
    sm.adapterId = s.sourceInfo.adapterId;
    sm.sourceMode = pr.screen;
    modes.push_back(sm);
    s.sourceInfo.modeInfoIdx = (UINT32)modes.size() - 1;
    paths.push_back(s);
  }

  // Marked before the change, so a crash in the middle is still undone.
  WriteState(kRestoreDisplays, 1);
  g_savedPaths = active;
  g_savedModes = activeModes;
  LONG r = SetDisplayConfig((UINT32)paths.size(), paths.data(),
                            (UINT32)modes.size(), modes.data(),
                            SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG |
                                SDC_ALLOW_CHANGES);
  viewer::DebugTrace(L"Privacy: switch off %u screen(s) (%ld)",
                     (unsigned)pairs.size(), r);
  if (r != ERROR_SUCCESS) {
    g_savedPaths.clear();
    g_savedModes.clear();
    DeleteState(kRestoreDisplays);
    *error = L"Windows refused to switch off (error " + std::to_wstring(r) +
             L").";
    return false;
  }

  // Check each stand-in really took its screen's place and size.
  std::vector<DISPLAYCONFIG_PATH_INFO> now;
  std::vector<DISPLAYCONFIG_MODE_INFO> nowModes;
  QueryLayout(QDC_ONLY_ACTIVE_PATHS, &now, &nowModes);
  std::vector<InUse> inUse;
  std::vector<const DISPLAYCONFIG_PATH_INFO*> placedPaths;
  for (const auto& pr : pairs) {
    bool placed = false;
    std::wstring device;
    for (const auto& p : now) {
      if (TargetPath(p, nullptr) != pr.id) continue;
      const DISPLAYCONFIG_SOURCE_MODE* m = SourceModeOf(p, nowModes);
      placed = m && m->width == pr.screen.width &&
               m->height == pr.screen.height &&
               m->position.x == pr.screen.position.x &&
               m->position.y == pr.screen.position.y;
      device = SourceName(p);
      placedPaths.push_back(&p);
    }
    if (!placed) {
      RestoreDisplays();
      *error = L"The stand-in can't do " +
               Size(pr.screen.width, pr.screen.height) +
               L": add it to your virtual display driver.";
      return false;
    }
    inUse.push_back({pr.id, device});
  }
  g_standIns = inUse;
  *switched = goingOff;

  // Each stand-in gets its screen's scaling, so apps keep their size. Windows
  // remembers it for the stand-in, so next time it is right from the start.
  for (size_t i = 0; i < pairs.size() && i < placedPaths.size(); ++i) {
    const int want = pairs[i].scale;
    const DISPLAYCONFIG_PATH_INFO& p = *placedPaths[i];
    const int got = ScalePercent(p);
    if (!want || !got || got == want) continue;
    bool set = SetScalePercent(p, want);
    viewer::DebugTrace(L"Privacy: stand-in scaling %d%% -> %d%% (%ls)", got,
                       want, set ? L"ok" : L"failed");
    if (!set) {
      *note = L"Couldn't set a stand-in to " + std::to_wstring(want) +
              L"% scaling. Set it in Settings > Display.";
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Covers
// ---------------------------------------------------------------------------
LRESULT CALLBACK CoverProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_NCHITTEST:
      return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_TIMER:
      if (wParam == kTopTimer) {
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                         SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
      }
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      FillRect(dc, &ps.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH));
      EndPaint(hwnd, &ps);
      return 0;
    }
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool RegisterCoverClass() {
  static bool registered = false;
  if (registered) return true;
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = CoverProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
  wc.lpszClassName = kCoverClass;
  registered = RegisterClassExW(&wc) != 0;
  return registered;
}

// A black, click-through, always-on-top window over |rc| that screen capture
// does not see. Null if Windows cannot leave it out of capture: then the
// glasses would show black too.
HWND MakeCover(const RECT& rc, bool show) {
  if (!RegisterCoverClass()) return nullptr;
  HWND hwnd = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED |
          WS_EX_TRANSPARENT,
      kCoverClass, L"", WS_POPUP, rc.left, rc.top, rc.right - rc.left,
      rc.bottom - rc.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!hwnd) return nullptr;
  SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
  if (!SetWindowDisplayAffinity(hwnd, kExcludeFromCapture)) {
    DestroyWindow(hwnd);
    return nullptr;
  }
  if (show) {
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetTimer(hwnd, kTopTimer, kTopMs, nullptr);
  }
  return hwnd;
}

void DestroyCovers() {
  for (HWND h : g_covers) DestroyWindow(h);
  g_covers.clear();
  g_coverRects.clear();
}

std::wstring ActiveBuiltInDevice() {
  std::vector<DISPLAYCONFIG_PATH_INFO> paths;
  std::vector<DISPLAYCONFIG_MODE_INFO> modes;
  if (!QueryLayout(QDC_ONLY_ACTIVE_PATHS, &paths, &modes)) return L"";
  for (const auto& p : paths) {
    if (IsBuiltInTarget(p)) return SourceName(p);
  }
  return L"";
}

struct MonitorRect {
  std::wstring device;
  RECT rc;
};

BOOL CALLBACK AddMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM out) {
  MONITORINFOEXW info = {};
  info.cbSize = sizeof(info);
  if (GetMonitorInfoW(monitor, &info)) {
    ((std::vector<MonitorRect>*)out)->push_back({info.szDevice, info.rcMonitor});
  }
  return TRUE;
}

// Where covers belong right now.
std::vector<RECT> WantedCovers() {
  std::vector<MonitorRect> monitors;
  EnumDisplayMonitors(nullptr, nullptr, AddMonitor, (LPARAM)&monitors);
  std::vector<RECT> out;
  for (const auto& m : monitors) {
    if (m.device == g_glasses) continue;
    bool standIn = false;
    for (const auto& s : g_standIns) standIn |= s.device == m.device;
    if (standIn) continue;
    if (!g_options.allScreens &&
        (g_mainSwitched || m.device != g_mainDevice)) {
      continue;
    }
    out.push_back(m.rc);
  }
  return out;
}

bool SameRects(const std::vector<RECT>& a, const std::vector<RECT>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (!EqualRect(&a[i], &b[i])) return false;
  }
  return true;
}

// Puts covers exactly where they are wanted. Returns how many there are.
size_t PlaceCovers() {
  std::vector<RECT> wanted = WantedCovers();
  if (SameRects(wanted, g_coverRects)) return g_covers.size();
  // New covers first, then the old ones go, so nothing shows in between.
  std::vector<HWND> made;
  for (const RECT& rc : wanted) {
    if (HWND h = MakeCover(rc, true)) made.push_back(h);
  }
  DestroyCovers();
  g_covers = made;
  g_coverRects = wanted;
  return g_covers.size();
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool CanCover() {
  static int known = -1;
  if (known < 0) {
    RECT rc = {0, 0, 16, 16};
    HWND probe = MakeCover(rc, false);
    known = probe ? 1 : 0;
    if (probe) DestroyWindow(probe);
  }
  return known == 1;
}

bool HasBuiltInScreen() {
  // While switched off it is still there, just off.
  for (const auto& p : g_savedPaths) {
    if (IsBuiltInTarget(p)) return true;
  }
  return !ActiveBuiltInDevice().empty();
}

std::wstring Summary() { return g_summary; }

std::vector<std::wstring> VirtualScreensInUse() {
  std::vector<std::wstring> out;
  std::vector<DISPLAYCONFIG_PATH_INFO> paths;
  std::vector<DISPLAYCONFIG_MODE_INFO> modes;
  if (!QueryLayout(QDC_ONLY_ACTIVE_PATHS, &paths, &modes)) return out;
  for (const auto& p : paths) {
    if (!IsVirtualAdapter(p.targetInfo.adapterId)) continue;
    std::wstring name;
    std::wstring id = TargetPath(p, &name);
    if (id.empty() || glassesid::IsVitureDisplay(name, ManufacturerFromPath(id))) {
      continue;
    }
    bool standIn = false;
    for (const auto& s : g_standIns) standIn |= s.id == id;
    if (!standIn) out.push_back(id);
  }
  return out;
}

std::wstring VirtualDisplayName(const LUID& adapter, const std::wstring& id) {
  if (id.empty() || !IsVirtualAdapter(adapter)) return L"";
  std::vector<DISPLAYCONFIG_PATH_INFO> paths;
  std::vector<DISPLAYCONFIG_MODE_INFO> modes;
  if (!QueryLayout(QDC_ALL_PATHS, &paths, &modes)) return L"Virtual display";

  // Every virtual display, by the number the driver gave it ("...UID257#").
  auto uid = [](const std::wstring& path) {
    size_t at = path.find(L"UID");
    return at == std::wstring::npos ? 0L : wcstol(path.c_str() + at + 3, nullptr, 10);
  };
  std::vector<std::wstring> all;
  for (const auto& p : paths) {
    if (!p.targetInfo.targetAvailable) continue;
    if (!IsVirtualAdapter(p.targetInfo.adapterId)) continue;
    std::wstring name;
    std::wstring other = TargetPath(p, &name);
    if (other.empty()) continue;
    if (glassesid::IsVitureDisplay(name, ManufacturerFromPath(other))) continue;
    if (std::find(all.begin(), all.end(), other) == all.end()) all.push_back(other);
  }
  std::sort(all.begin(), all.end(), [&](const std::wstring& a, const std::wstring& b) {
    long ua = uid(a), ub = uid(b);
    return ua != ub ? ua < ub : a < b;
  });
  if (all.size() < 2) return L"Virtual display";
  for (size_t i = 0; i < all.size(); ++i) {
    if (all[i] == id) return L"Virtual display " + std::to_wstring(i + 1);
  }
  return L"Virtual display";
}

std::vector<StandIn> FindStandIns() {
  std::vector<StandIn> out;
  std::vector<DISPLAYCONFIG_PATH_INFO> paths;
  std::vector<DISPLAYCONFIG_MODE_INFO> modes;
  if (!QueryLayout(QDC_ALL_PATHS, &paths, &modes)) return out;
  // While switched off the stand-in is on: still list it, it is the one.
  std::vector<std::wstring> activeTargets;
  for (const auto& p : paths) {
    if (p.flags & DISPLAYCONFIG_PATH_ACTIVE) {
      activeTargets.push_back(TargetPath(p, nullptr));
    }
  }
  for (const auto& p : paths) {
    if (!p.targetInfo.targetAvailable || IsBuiltInTarget(p)) continue;
    if (!IsVirtualAdapter(p.targetInfo.adapterId)) continue;
    std::wstring name;
    std::wstring id = TargetPath(p, &name);
    if (id.empty()) continue;
    bool on = false;
    for (const auto& a : activeTargets) on |= a == id;
    bool isCurrent = false;
    for (const auto& s : g_standIns) isCurrent |= s.id == id;
    if (on && !isCurrent) continue;
    if (glassesid::IsVitureDisplay(name, ManufacturerFromPath(id))) continue;
    bool seen = false;
    for (const auto& s : out) seen |= s.id == id;
    if (seen) continue;
    out.push_back({id, VirtualDisplayName(p.targetInfo.adapterId, id)});
  }
  return out;
}

bool Start(const Options& options, const std::wstring& glassesDevice,
           std::wstring* summary, std::wstring* note) {
  if (g_mode != Mode::kOff) Stop();
  g_options = options;
  g_glasses = glassesDevice;
  g_note.clear();

  g_mainDevice.clear();
  g_mainSwitched = false;

  // The screens to black out: the main one, or every one but the glasses.
  std::vector<DISPLAYCONFIG_PATH_INFO> active;
  std::vector<DISPLAYCONFIG_MODE_INFO> modes;
  if (!QueryLayout(QDC_ONLY_ACTIVE_PATHS, &active, &modes)) {
    *summary = L"Windows did not report the display layout.";
    return false;
  }
  const int main = MainScreen(active, modes);
  if (main < 0) {
    *summary = L"There is no screen to black out.";
    return false;
  }
  const bool mainIsBuiltIn = IsBuiltInTarget(active[(size_t)main]);
  g_mainDevice = SourceName(active[(size_t)main]);
  std::vector<size_t> targets = {(size_t)main};
  size_t physical = IsVirtualTarget(active[(size_t)main]) ? 0 : 1;
  if (options.allScreens) {
    for (size_t i = 0; i < active.size(); ++i) {
      if ((int)i == main || SourceName(active[i]) == g_glasses) continue;
      targets.push_back(i);
      if (!IsVirtualTarget(active[i])) ++physical;
    }
  }

  std::vector<std::wstring> switched;
  std::wstring why;
  // No stand-in chosen ("None") means: cover, on purpose, so no note.
  if (options.switchOff && !options.standIn.empty() && physical > 0) {
    SwitchOff(targets, options.standIn, &switched, &why, &g_note);
  }
  for (const auto& d : switched) g_mainSwitched |= d == g_mainDevice;

  // Covers go over whatever is still lit.
  size_t covered = 0;
  if (!WantedCovers().empty()) {
    if (!CanCover()) {
      if (switched.empty()) {
        *summary = L"Covering screens needs Windows 10 version 2004 or newer.";
        return false;
      }
      g_note = L"Other screens weren't covered: that needs Windows 10 2004.";
    } else {
      covered = PlaceCovers();
    }
  }
  if (switched.empty() && covered == 0) {
    *summary = L"There is no screen to black out.";
    return false;
  }
  // The built-in screen, if still on and covered, goes dark as well.
  std::wstring builtIn = ActiveBuiltInDevice();
  if (options.dim && !builtIn.empty() &&
      (options.allScreens || builtIn == g_mainDevice)) {
    SetDim(true);
  }

  g_mode = switched.empty() ? Mode::kCovered : Mode::kSwitchedOff;
  if (!why.empty()) {
    g_note = why + L" Covered instead.";
  } else if (!switched.empty() && switched.size() < physical) {
    g_note = L"Only " + std::to_wstring(switched.size()) +
             L" stand-in display" + (switched.size() == 1 ? L"" : L"s") +
             L" for " + std::to_wstring(physical) +
             L" screens: the rest are covered.";
  }
  const std::wstring screen = mainIsBuiltIn ? L"Built-in screen" : L"Screen";
  if (!options.allScreens) {
    *summary = screen + (g_mainSwitched ? L" switched off" : L" covered");
  } else if (!switched.empty() && covered > 0) {
    *summary = std::to_wstring(switched.size()) + L" switched off, " +
               std::to_wstring(covered) + L" covered";
  } else if (!switched.empty()) {
    *summary = switched.size() == 1 ? screen + L" switched off"
                                    : L"Screens switched off";
  } else {
    *summary = covered == 1 ? screen + L" covered" : L"Screens covered";
  }
  g_summary = *summary;
  if (note) *note = g_note;
  viewer::DebugTrace(L"Privacy: on (%ls)%ls%ls", summary->c_str(),
                     g_note.empty() ? L"" : L": ", g_note.c_str());
  return true;
}

void Stop() {
  if (g_mode == Mode::kOff) return;
  DestroyCovers();
  SetDim(false);
  if (g_mode == Mode::kSwitchedOff) RestoreDisplays();
  g_mode = Mode::kOff;
  g_note.clear();
  g_summary.clear();
  g_mainDevice.clear();
  g_mainSwitched = false;
  viewer::DebugTrace(L"Privacy: off");
}

void Shutdown() {
  Stop();
  std::unique_lock<std::mutex> hold(g_dim.lock);
  if (g_dim.started) {
    g_dim.changed.wait_for(hold, std::chrono::seconds(3),
                           [] { return !g_dim.dimmed; });
  }
}

Mode Current() { return g_mode; }

std::wstring Note() { return g_note; }

void Refresh() {
  if (g_mode == Mode::kOff) return;
  PlaceCovers();
}

bool IsCoverWindow(HWND hwnd) {
  for (HWND h : g_covers) {
    if (h == hwnd) return true;
  }
  return false;
}

void RecoverAfterCrash() {
  DWORD value = 0;
  if (ReadState(kRestoreDisplays, &value)) {
    LONG r = SetDisplayConfig(0, nullptr, 0, nullptr,
                              SDC_APPLY | SDC_USE_DATABASE_CURRENT);
    viewer::DebugTrace(L"Privacy: restored displays after a crash (%ld)", r);
    DeleteState(kRestoreDisplays);
  }
  if (ReadState(kRestoreBrightness, &value)) {
    // The thread puts the saved level back when told "not dimmed".
    std::lock_guard<std::mutex> hold(g_dim.lock);
    g_dim.dimmed = true;
    g_dim.want = false;
    if (!g_dim.started) {
      g_dim.started = true;
      std::thread(DimmerLoop).detach();
    }
    g_dim.changed.notify_all();
  }
}

}  // namespace privacy
