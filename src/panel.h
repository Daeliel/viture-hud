// Copyright (C) 2026 Jurica Dujmovic
// SPDX-License-Identifier: GPL-3.0-or-later
//
// panel.h - the Viture HUD control panel.
//
// A small dark window opened from the tray icon. It shows the monitor layout
// with live thumbnails, and every viewer setting as a control. It owns no
// state of its own: everything is read through Actions::getState on each
// repaint, and every click goes back to the app through Actions.

#ifndef PANEL_H_
#define PANEL_H_

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "view_math.h"

namespace panel {

struct Display {
  int displayIndex = 0;  // DISPLAYn
  std::wstring device;   // \\.\DISPLAYn
  std::wstring name;     // the monitor's own name, e.g. "DELL U2720Q"
  RECT bounds = {0, 0, 0, 0};
  bool isGlasses = false;
  bool isPrimary = false;
};

// The keyboard shortcuts, in the order the Shortcuts card lists them.
enum Key {
  kKeyNext,
  kKeyPrevious,
  kKeyStop,
  kKeyZoom,
  kKeyZoomBack,
  kKeyWindowLock,
  kKeyPip,
  kKeyPrivacy,
  kKeyCount
};

struct Shortcut {
  std::wstring title;    // "Next monitor"
  std::wstring detail;   // what it does
  std::wstring keys;     // "Alt+D", or empty when not set
  std::wstring problem;  // why it does not work, or empty
};

// A virtual display that can stand in for a screen (see privacy.h).
struct StandIn {
  std::wstring id;
  std::wstring name;
};

struct State {
  bool glassesConnected = false;
  bool showing = false;  // the viewer is running
  int shownIndex = -1;   // DISPLAY index on the glasses
  std::vector<Display> displays;
  int zoomStep = 0;  // index into viewmath::kZoomSteps
  bool locked = false;
  std::wstring lockedTitle;
  bool picking = false;  // waiting for a click on the window to lock
  bool pipOn = false;
  std::wstring pipName;  // monitor the picture-in-picture shows
  int lensStep = 1;      // edge lens: 0 off, 1 = 1x, 2 = 1.5x, 3 = 2x
  viewmath::LensLayout lensLayout;
  RECT glassesBounds = {0, 0, 1920, 1080};  // for the lens editor's shape
  bool followCaret = true;
  bool autoShow = true;
  bool startWithWindows = false;
  int fps = 0;
  bool duplication = false;
  double scale = 1.0;

  // Keyboard shortcuts, indexed by Key.
  std::vector<Shortcut> shortcuts;

  // Privacy mode.
  int privacy = 0;            // 0 off, 1 covered, 2 switched off (some screens)
  std::wstring privacyNote;   // a warning from the last start, or empty
  std::wstring privacySummary;  // while on: what was done
  bool hasBuiltIn = false;    // a built-in (laptop) screen is on
  bool canCover = true;       // Windows 10 2004+: covers hide from capture
  int privacyScreens = 0;     // 0 main screen, 1 all screens
  int privacyMethod = 0;      // 0 switch off when a stand-in is set, 1 cover
  bool privacyDim = true;     // brightness to 0% while covered
  std::wstring standIn;       // the chosen stand-in's id, or empty
  std::vector<StandIn> standIns;
  int virtualScreensInUse = 0;  // virtual displays on as ordinary screens
};

struct Actions {
  std::function<State()> getState;
  std::function<void(int displayIndex)> showDisplay;
  std::function<void()> next;
  std::function<void()> stop;
  std::function<void(int step)> setZoom;
  std::function<void()> toggleLock;
  std::function<void(bool)> setPip;
  std::function<void(int step)> setLensStep;
  // |commit| is false while dragging (live preview) and true when done.
  std::function<void(const viewmath::LensLayout&, bool commit)> setLensLayout;
  std::function<void(bool)> setFollowCaret;
  std::function<void(bool)> setAutoShow;
  std::function<void(bool)> setStartWithWindows;

  // While a shortcut is being recorded the app's own hotkeys are off, so the
  // keys reach the panel.
  std::function<void()> beginRecording;
  std::function<void()> endRecording;
  // Sets shortcut |key| (vk 0 removes it). Returns why it was refused, or
  // an empty string when it was set.
  std::function<std::wstring(int key, unsigned mods, unsigned vk)> setShortcut;
  std::function<void()> resetShortcuts;

  std::function<void()> togglePrivacy;
  std::function<void(int screens)> setPrivacyScreens;
  std::function<void(int method)> setPrivacyMethod;
  std::function<void(bool)> setPrivacyDim;
  std::function<void(const std::wstring& id)> setStandIn;
};

void Init(HINSTANCE instance, HICON icon, const Actions& actions);

// Opens the panel centred on the monitor with these bounds, or brings it to
// the front (moving it to that monitor) if it is already open. Never pass the
// glasses: the viewer covers that display.
void Show(const RECT& monitorBounds);
// Show, then open the About card (who made the app and how to contribute).
void ShowAbout(const RECT& monitorBounds);
// Show, then open the Shortcuts card.
void ShowShortcuts(const RECT& monitorBounds);
// Show, then open the Privacy card.
void ShowPrivacy(const RECT& monitorBounds);
void Close();
bool IsOpen();

// The panel window, or null when closed.
HWND Window();

// Repaint soon, because something the panel shows has changed.
void Invalidate();

// Moves the panel onto the monitor with these bounds, keeping its relative
// position, so it stays on the screen being shown on the glasses.
void FollowMonitor(const RECT& monitorBounds);

}  // namespace panel

#endif  // PANEL_H_
