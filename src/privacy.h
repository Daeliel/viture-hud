// Copyright (C) 2026 Jurica Dujmovic
// SPDX-License-Identifier: GPL-3.0-or-later
//
// privacy.h - Privacy mode: black out the real screens while the glasses keep
// showing their content.
//
// Two ways to do it:
//
//   Cover        A black, click-through window over each screen, excluded
//                from screen capture (Windows 10 2004+), so the glasses still
//                show what is underneath. The built-in screen's brightness
//                goes to 0% as well. Nothing in Windows' layout changes.
//
//   Switch off   For any real screen, with a spare virtual display set up as
//                its "stand-in": in one display change, the stand-in is
//                switched on at exactly the screen's place and size and the
//                screen is switched off, so windows stay where they are.
//                With several screens, each needs its own stand-in; the ones
//                left without one are covered. Undone the same way.
//
// "Main screen" is the built-in (laptop) screen when it is on, otherwise the
// primary screen. The glasses are never touched.
//
// The display change is never saved to Windows' display database, so after a
// crash or reboot Windows' own saved layout (with the built-in screen on) is
// what comes back. RecoverAfterCrash() puts it back on the next start too.

#ifndef PRIVACY_H_
#define PRIVACY_H_

#include <windows.h>

#include <string>
#include <vector>

namespace privacy {

enum class Mode { kOff, kCovered, kSwitchedOff };

struct Options {
  bool allScreens = false;  // every screen but the glasses, not just the main
  bool switchOff = true;    // switch screens off when there is a stand-in
  bool dim = true;          // built-in brightness to 0% while it is covered
  std::wstring standIn;     // the stand-in used first (device path), or empty
};

// A virtual display that is connected but switched off, which could stand in
// for a screen.
struct StandIn {
  std::wstring id;    // the monitor's device path, stable across restarts
  std::wstring name;  // what the monitor calls itself
};

// Turns Privacy mode on. |glassesDevice| (\\.\DISPLAYn) is never touched.
// On success |summary| says what was done; |note| may carry a warning worth
// showing (a fallback, a scaling mismatch). On failure |summary| says why.
bool Start(const Options& options, const std::wstring& glassesDevice,
           std::wstring* summary, std::wstring* note);

// Turns it off and puts everything back. Safe to call when it is off.
void Stop();

// Stop, and wait (briefly) for the brightness to be restored. For exit.
void Shutdown();

Mode Current();
// The last warning from Start, for the control panel.
std::wstring Note();

// Call after the display layout changed: covers follow the screens.
void Refresh();

// While on: what was done ("Screen switched off"), else empty.
std::wstring Summary();

// Whether a built-in screen is on.
bool HasBuiltInScreen();
// Whether covering works on this Windows (10 version 2004 or later).
bool CanCover();
// Displays that could be the stand-in.
std::vector<StandIn> FindStandIns();

// One of the cover windows (so the app never treats it as a user window).
bool IsCoverWindow(HWND hwnd);

// At start-up: undoes a Privacy mode left on by a crash.
void RecoverAfterCrash();

}  // namespace privacy

#endif  // PRIVACY_H_
