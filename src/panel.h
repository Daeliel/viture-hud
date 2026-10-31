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
};

void Init(HINSTANCE instance, HICON icon, const Actions& actions);

// Opens the panel centred on the monitor with these bounds, or brings it to
// the front (moving it to that monitor) if it is already open. Never pass the
// glasses: the viewer covers that display.
void Show(const RECT& monitorBounds);
// Show, then open the About card (who made the app and how to contribute).
void ShowAbout(const RECT& monitorBounds);
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
