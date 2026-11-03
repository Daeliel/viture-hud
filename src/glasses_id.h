// Copyright (C) 2026 Jurica Dujmovic
// SPDX-License-Identifier: GPL-3.0-or-later
//
// glasses_id.h - tells whether a display is a pair of VITURE glasses, from
// the name it reports to Windows and its EDID manufacturer id. Pure functions
// with no Windows calls, so tests/test_view_math.cpp can check them.
//
// Known so far:
//   Luma Ultra   name "VITURE"         manufacturer "CVT"
//   Beast        name "VITURE Beast"   manufacturer "VIT"
// Other models are expected to follow one of these patterns.

#ifndef GLASSES_ID_H_
#define GLASSES_ID_H_

#include <string>

namespace glassesid {

// "VITURE" on its own, or followed by a space and a model name
// ("VITURE Beast"). Not case sensitive. "VITUREX" or "My VITURE" do not match.
inline bool IsVitureName(const std::wstring& name) {
  static const wchar_t kBrand[] = L"VITURE";
  const size_t n = sizeof(kBrand) / sizeof(kBrand[0]) - 1;
  if (name.size() < n) return false;
  for (size_t i = 0; i < n; ++i) {
    wchar_t c = name[i];
    if (c >= L'a' && c <= L'z') c = (wchar_t)(c - L'a' + L'A');
    if (c != kBrand[i]) return false;
  }
  return name.size() == n || name[n] == L' ';
}

// EDID manufacturer ids VITURE glasses have been seen with.
inline bool IsVitureManufacturer(const std::wstring& id) {
  return id == L"CVT" || id == L"VIT";
}

inline bool IsVitureDisplay(const std::wstring& name, const std::wstring& id) {
  return IsVitureName(name) || IsVitureManufacturer(id);
}

}  // namespace glassesid

#endif  // GLASSES_ID_H_
