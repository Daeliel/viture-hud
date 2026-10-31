// Copyright (C) 2026 Jurica Dujmovic
// SPDX-License-Identifier: GPL-3.0-or-later
//
// viewer.h - live monitor viewer on the Viture glasses.
//
// The glasses are an ordinary 1920x1080 display. To show another monitor on
// them without mirroring (and therefore without touching the real monitors),
// we open a fullscreen always-on-top window on the glasses and draw a live
// capture of that monitor into it, following the mouse or the text cursor.
// The real monitors keep their own resolution and desktop throughout.

#ifndef VIEWER_H_
#define VIEWER_H_

#include <windows.h>

#include <string>

#include "view_math.h"

namespace viewer {

// Opens the viewer window on |targetMonitor| (a \\.\DISPLAYn device name) and
// starts following |sourceMonitor|. Pass an empty source to follow the focused
// window on any monitor.
bool Start(const std::wstring& sourceMonitorDevice,
           const std::wstring& targetMonitorDevice, std::wstring* error);

// Closes the viewer window and stops the paint loop.
void Stop();

bool IsRunning();

// Centre the view on |monitorDevice| (a \.\DISPLAYn name). |label| is the
// name shown in the status bar.
void FocusMonitor(const std::wstring& monitorDevice, const std::wstring& label);

// Alt+Z / Alt+Shift+Z: next or previous zoom step (1x, 1.5x, 2x, fit).
void StepZoom(int direction);

// Picks a zoom step directly (index into viewmath::kZoomSteps).
void SetZoomStep(int step);
int ZoomStep();

// Whether the view follows the text cursor while typing. On by default.
void SetFollowCaret(bool enabled);

// Live numbers for the control panel.
struct Stats {
  int fps = 0;
  bool duplication = false;  // false = slow GDI fallback
  double scale = 1.0;
};
Stats GetStats();

// Shows only |hwnd|, fitted to the glasses, until it closes, is minimised or
// leaves the monitor. Pass nullptr to unlock. The window must be on the
// monitor currently shown.
void LockWindow(HWND hwnd, const std::wstring& title);
bool IsWindowLocked();
std::wstring LockedTitle();

// The viewer's own window on the glasses, or null when stopped.
HWND Window();

// Picture-in-picture: a small live view of |monitorDevice| in a bottom corner
// of the glasses, labelled |label|. Pass an empty device to turn it off.
void SetPip(const std::wstring& monitorDevice, const std::wstring& label);

// Edge lens: near an edge or corner of the glasses, repeat the area around the
// cursor further in, magnified by |zoom| relative to the view (1 = same size,
// only moved), sized and placed by |layout|. A zoom of 0 turns it off.
void SetEdgeLens(double zoom, const viewmath::LensLayout& layout);

// Shows the lens for |ms| even away from the edges, so a change made in the
// control panel can be judged through the glasses.
void PreviewEdgeLens(unsigned ms);

// For diagnostics.
void DebugTrace(const wchar_t* fmt, ...);

}  // namespace viewer

#endif  // VIEWER_H_