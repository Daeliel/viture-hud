// Copyright (C) 2026 Jurica Dujmovic
// SPDX-License-Identifier: GPL-3.0-or-later
//
// hotkeys.h - keyboard shortcuts as text, and which ones are allowed.
//
// Shortcuts are stored as readable text ("Ctrl+Alt+P") in the registry and
// shown the same way in the control panel. Pure functions, no Windows calls,
// so they are covered by tests/test_view_math.cpp.

#ifndef HOTKEYS_H_
#define HOTKEYS_H_

#include <cwctype>
#include <string>

namespace hotkeys {

// The same bits RegisterHotKey takes (MOD_ALT, MOD_CONTROL, MOD_SHIFT,
// MOD_WIN), spelled out so this header does not need windows.h.
constexpr unsigned kAlt = 0x1;
constexpr unsigned kCtrl = 0x2;
constexpr unsigned kShift = 0x4;
constexpr unsigned kWin = 0x8;

// A shortcut: modifiers plus one virtual-key code. vk 0 means "not set".
struct Combo {
  unsigned mods = 0;
  unsigned vk = 0;
  bool empty() const { return vk == 0; }
  bool operator==(const Combo& o) const { return mods == o.mods && vk == o.vk; }
  bool operator!=(const Combo& o) const { return !(*this == o); }
};

namespace detail {

struct Named {
  unsigned vk;
  const wchar_t* name;
};

// Keys with a name of their own. Letters, digits and F-keys are worked out.
inline const Named* NamedKeys(int* count) {
  static const Named kKeys[] = {
      {0x20, L"Space"},    {0x0D, L"Enter"},    {0x08, L"Backspace"},
      {0x2D, L"Insert"},   {0x2E, L"Delete"},   {0x24, L"Home"},
      {0x23, L"End"},      {0x21, L"PageUp"},   {0x22, L"PageDown"},
      {0x25, L"Left"},     {0x26, L"Up"},       {0x27, L"Right"},
      {0x28, L"Down"},     {0x13, L"Pause"},    {0x91, L"ScrollLock"},
      {0x6A, L"Num*"},     {0x6B, L"Num+"},     {0x6D, L"Num-"},
      {0x6E, L"Num."},     {0x6F, L"Num/"},     {0x09, L"Tab"},
      {0x1B, L"Esc"},
  };
  *count = (int)(sizeof(kKeys) / sizeof(kKeys[0]));
  return kKeys;
}

// Punctuation keys (VK_OEM_*). They print differently on every keyboard
// layout, so they are stored by code ("0xBA") and the app shows the key's
// name for the current layout.
inline bool IsOemKey(unsigned vk) {
  return (vk >= 0xBA && vk <= 0xC0) || (vk >= 0xDB && vk <= 0xDF) ||
         vk == 0xE2;
}

inline bool EqualsNoCase(const std::wstring& a, const wchar_t* b) {
  size_t i = 0;
  for (; i < a.size() && b[i]; ++i) {
    if (std::towlower(a[i]) != std::towlower(b[i])) return false;
  }
  return i == a.size() && b[i] == 0;
}

}  // namespace detail

inline bool IsOemKey(unsigned vk) { return detail::IsOemKey(vk); }

// True for a key that may be the main key of a shortcut. Modifiers on their
// own are not keys here.
inline bool IsAllowedKey(unsigned vk) {
  if (vk >= 'A' && vk <= 'Z') return true;
  if (vk >= '0' && vk <= '9') return true;
  if (vk >= 0x70 && vk <= 0x87) return true;  // F1-F24
  if (vk >= 0x60 && vk <= 0x69) return true;  // Num0-Num9
  if (detail::IsOemKey(vk)) return true;
  int count = 0;
  const detail::Named* named = detail::NamedKeys(&count);
  for (int i = 0; i < count; ++i) {
    if (named[i].vk == vk) return true;
  }
  return false;
}

// The key's stored name: "D", "7", "F5", "Num5", "PageUp", or "0xBA".
inline std::wstring KeyName(unsigned vk) {
  if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
    return std::wstring(1, (wchar_t)vk);
  }
  if (vk >= 0x70 && vk <= 0x87) return L"F" + std::to_wstring(vk - 0x70 + 1);
  if (vk >= 0x60 && vk <= 0x69) return L"Num" + std::to_wstring(vk - 0x60);
  int count = 0;
  const detail::Named* named = detail::NamedKeys(&count);
  for (int i = 0; i < count; ++i) {
    if (named[i].vk == vk) return named[i].name;
  }
  wchar_t hex[8];
  const wchar_t* digits = L"0123456789ABCDEF";
  hex[0] = L'0';
  hex[1] = L'x';
  hex[2] = digits[(vk >> 4) & 0xF];
  hex[3] = digits[vk & 0xF];
  hex[4] = 0;
  return hex;
}

// Reads a key name back. Case does not matter.
inline bool KeyFromName(const std::wstring& name, unsigned* vk) {
  if (name.empty()) return false;
  if (name.size() == 1) {
    wchar_t c = (wchar_t)std::towupper(name[0]);
    if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) {
      *vk = (unsigned)c;
      return true;
    }
    return false;
  }
  if ((name[0] == L'F' || name[0] == L'f') && name.size() <= 3) {
    int n = 0;
    for (size_t i = 1; i < name.size(); ++i) {
      if (name[i] < L'0' || name[i] > L'9') return false;
      n = n * 10 + (name[i] - L'0');
    }
    if (n < 1 || n > 24) return false;
    *vk = 0x70 + (unsigned)(n - 1);
    return true;
  }
  if (name.size() == 4 && detail::EqualsNoCase(name.substr(0, 3), L"Num") &&
      name[3] >= L'0' && name[3] <= L'9') {
    *vk = 0x60 + (unsigned)(name[3] - L'0');
    return true;
  }
  if (name.size() == 4 && name[0] == L'0' && (name[1] == L'x' || name[1] == L'X')) {
    unsigned v = 0;
    for (size_t i = 2; i < 4; ++i) {
      wchar_t c = (wchar_t)std::towupper(name[i]);
      v <<= 4;
      if (c >= L'0' && c <= L'9') {
        v |= (unsigned)(c - L'0');
      } else if (c >= L'A' && c <= L'F') {
        v |= (unsigned)(c - L'A' + 10);
      } else {
        return false;
      }
    }
    if (!detail::IsOemKey(v)) return false;
    *vk = v;
    return true;
  }
  int count = 0;
  const detail::Named* named = detail::NamedKeys(&count);
  for (int i = 0; i < count; ++i) {
    if (detail::EqualsNoCase(name, named[i].name)) {
      *vk = named[i].vk;
      return true;
    }
  }
  return false;
}

// "Ctrl+Alt+Shift+Win+Key", always in that order. Empty when not set.
inline std::wstring Format(const Combo& c) {
  if (c.empty()) return L"";
  std::wstring out;
  if (c.mods & kCtrl) out += L"Ctrl+";
  if (c.mods & kAlt) out += L"Alt+";
  if (c.mods & kShift) out += L"Shift+";
  if (c.mods & kWin) out += L"Win+";
  return out + KeyName(c.vk);
}

// Reads Format's text back, in any order and case ("alt+shift+d" works). An
// empty string is a valid "not set". Returns false for anything else that
// is not exactly modifiers plus one allowed key.
inline bool Parse(const std::wstring& text, Combo* out) {
  Combo c;
  if (text.empty()) {
    *out = c;
    return true;
  }
  size_t start = 0;
  bool haveKey = false;
  while (start <= text.size()) {
    size_t plus = text.find(L'+', start);
    // "Num+" ends in a plus of its own: a '+' right at the end belongs to it.
    if (plus != std::wstring::npos && plus + 1 == text.size()) {
      plus = std::wstring::npos;
    }
    std::wstring part = text.substr(
        start, plus == std::wstring::npos ? std::wstring::npos : plus - start);
    if (part.empty()) return false;
    if (detail::EqualsNoCase(part, L"Ctrl")) {
      c.mods |= kCtrl;
    } else if (detail::EqualsNoCase(part, L"Alt")) {
      c.mods |= kAlt;
    } else if (detail::EqualsNoCase(part, L"Shift")) {
      c.mods |= kShift;
    } else if (detail::EqualsNoCase(part, L"Win")) {
      c.mods |= kWin;
    } else {
      unsigned vk = 0;
      if (haveKey || !KeyFromName(part, &vk) || !IsAllowedKey(vk)) return false;
      c.vk = vk;
      haveKey = true;
    }
    if (plus == std::wstring::npos) break;
    start = plus + 1;
  }
  if (!haveKey) return false;
  *out = c;
  return true;
}

// Why |c| cannot be a global shortcut, or null when it can. Global shortcuts
// take the key away from every other app, so the rules keep the everyday
// ones (Ctrl+C, Alt+F4, plain letters) safe.
inline const wchar_t* Problem(const Combo& c) {
  if (c.empty()) return nullptr;
  if (!IsAllowedKey(c.vk)) return L"That key cannot be used for a shortcut";
  const bool ctrl = (c.mods & kCtrl) != 0;
  const bool alt = (c.mods & kAlt) != 0;
  const bool shift = (c.mods & kShift) != 0;
  const bool win = (c.mods & kWin) != 0;
  const bool f13to24 = c.vk >= 0x7C && c.vk <= 0x87;  // keys no app uses
  if (c.mods == 0 && !f13to24) return L"Add Ctrl, Alt or Win to the key";
  if (shift && !ctrl && !alt && !win) return L"Add Ctrl, Alt or Win to the key";
  if (ctrl && !alt && !win && !shift) {
    return L"Ctrl plus a key is used by every app (Ctrl+C, Ctrl+S...). Add Alt or Shift";
  }
  // Windows' own.
  if (alt && !ctrl && !win && (c.vk == 0x73 /*F4*/ || c.vk == 0x09 /*Tab*/ ||
                               c.vk == 0x1B /*Esc*/ || c.vk == 0x20 /*Space*/ ||
                               c.vk == 0x0D /*Enter*/)) {
    return L"Windows uses that shortcut";
  }
  if (ctrl && alt && c.vk == 0x2E /*Delete*/) return L"Windows uses that shortcut";
  if (ctrl && c.vk == 0x1B /*Esc*/) return L"Windows uses that shortcut";
  return nullptr;
}

}  // namespace hotkeys

#endif  // HOTKEYS_H_
