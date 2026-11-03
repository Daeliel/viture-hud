# Viture HUD

Use your VITURE XR glasses as a sharp, private view of your other monitors on
Windows. One monitor at a time shows on the glasses at native 1:1 pixels, the
view follows your mouse and text cursor, and one hotkey switches screens. Your
real monitors are never reconfigured: no mirroring, no resolution changes, no
virtual display drivers.

> **Unofficial community project.** Viture HUD is not affiliated with, endorsed
> by or supported by VITURE. "VITURE" is a trademark of its owner and is used
> here only to say which glasses this works with.

## Features

- **Any monitor on the glasses** at 1:1 pixels, captured live at full refresh
  rate (Desktop Duplication, GPU-rendered with Direct3D 11).
- **Alt+D switches screens**, and takes the mouse and keyboard focus with it,
  back to where you left them on that screen.
- **Calm panning**: the view stays still while the mouse moves around the
  middle and glides along when it nears an edge, so big monitors (ultrawides,
  4K) are comfortable to use.
- **Follows the text cursor** while you type, in classic Windows apps, Chrome,
  Edge, VS Code and other Electron apps.
- **Zoom**: 1x, 1.5x, 2x, or Fit (fills the glasses' height).
- **Window lock**: show only one window, fitted to the glasses.
- **Edge lens**: XR glasses blur the edges and corners of the picture. Near an
  edge, a lens further in shows that area again, at the same size or
  magnified.
- **Picture-in-picture**: a small live view of your other monitor in a corner.
- **Starts by itself** when the glasses are plugged in.
- **Control panel** with live thumbnails of your monitor layout and every
  setting. Single small exe, no installer, no admin rights.

## Requirements

- Windows 10 (1903 or later) or Windows 11, 64-bit.
- VITURE glasses connected as a display (USB-C with DisplayPort, or through an
  HDMI adapter or dock), plus at least one other monitor.
- A graphics card with Direct3D 11 - any from the last decade.

Tested with VITURE Luma Ultra glasses on Windows 10 22H2 with an NVIDIA GPU.
Also reported working with the VITURE Beast. Other VITURE models should work as
long as Windows reports the display's name as "VITURE" or "VITURE <model>"
(Settings > System > Display > Advanced display). If yours does not, please
open an issue with the name it shows.

## Install

1. Download `viture_hud.exe` from the
   [Releases](../../releases) page and put it anywhere, e.g. a `Viture HUD`
   folder in your user folder.
2. Run it. It lives in the system tray (bottom-right, near the clock) as a
   green glasses icon. With the glasses plugged in, the monitor under your
   mouse appears on them straight away.
3. Optional: in the control panel turn on **Start with Windows**.

To uninstall, turn off **Start with Windows**, exit from the tray menu and
delete the exe. Settings live in `HKCU\Software\VitureHUD` and can be removed
with `reg delete HKCU\Software\VitureHUD /f`.

## Use

**Left-click** the tray icon for the control panel. **Right-click** it for the
quick menu: open the panel, pick a monitor to show, next monitor, stop, refresh
monitors, start with Windows, create a desktop shortcut, about, exit.

| Key | Action |
|---|---|
| **Alt+D** | Show the next monitor on the glasses (the mouse and focus go along) |
| **Alt+Shift+D** | Show the previous monitor |
| **Ctrl+Alt+D** | Stop: the glasses go back to a normal extended desktop |
| **Alt+Z** | Zoom: 1x → 1.5x → 2x → Fit → 1x |
| **Alt+Shift+Z** | Zoom the other way |
| **Alt+W** | Show only the active window, fitted to the glasses (again to unlock) |
| **Alt+P** | Picture-in-picture on or off |

These are global hotkeys: while Viture HUD runs, other apps do not receive
them.

The view follows whichever moved last: the mouse, or the text cursor while you
type.

### Control panel

- **Displays**: your monitors as they are arranged, with live thumbnails. The
  one on the glasses is outlined in green; click another to show it.
- **Zoom**: 1x is pixel-perfect. Fit fills the glasses' height (a 3840x1600
  ultrawide shows at 0.68x) and pans sideways with the mouse.
- **Window lock**: click **Pick a window to lock**, then click the window. A
  green outline shows what you are about to pick; Esc or right-click cancels,
  and picking gives up by itself after 20 seconds. The click itself is not
  passed on, so the window does not jump over the panel. While locked, zoom
  is Fit; unlocking restores your zoom. The lock ends by itself when the
  window closes, is minimised or moves to another monitor.
- **Picture-in-picture**: a live view of your other monitor (with three or
  more, the next one in Alt+D order) in the bottom-right corner. It moves to the bottom-left when the mouse or text
  cursor comes near, and shows a small cursor when the mouse is on that
  monitor.
- **Edge lens** (right-hand column):
  - **Magnification**: Off, 1x (same size, just moved in from the edge), 1.5x
    or 2x. It multiplies the current zoom: 2x on a view at 1.5x is 3x.
  - **Placement**: *Follow cursor* puts the lens next to the cursor, inside
    the sharp area. *Fixed spot* always shows it at the same place, wherever
    your glasses are sharpest.
  - The small picture of the glasses' screen shows the blurry edge band and
    your lens. Drag its corner to resize it (15-60% of the glasses). With
    *Follow cursor*, the dashed box is where the lens may appear: drag any of
    its edges to set how far from the edge it stays (0-30%, default 12%).
    With *Fixed spot*, drag the lens itself to move it.
  - The shaded band is the trigger: the lens appears when the cursor enters
    it. Drag its inner edge to make it wider or narrower (3-30%, default
    12%). The band's edge and the dashed edge start on top of each other;
    grab from the band side to move the trigger, from the inside to move the
    offset.
  - While you drag, the lens also shows live on the glasses so you can judge
    it from inside them. **Reset** restores the defaults.
- **Follow the text cursor**, **Show a monitor when the glasses connect**,
  **Start with Windows**.
- The bottom line shows what is on the glasses, the zoom and the frame rate.
- **About** (top right, or in the tray menu): who made Viture HUD and why,
  with a link to this repository. Esc closes it.

The panel stays on the monitor shown on the glasses and moves with it when you
switch.

### Status bar on the glasses

For a moment after every change:

```
DELL U2720Q    1.00x    pan 480,60 of 1920,520    mon 3840x2160    60 fps
```

The monitor (and locked window), the zoom, the view's position and how far it
can pan, the monitor's size, and the frame rate. "(GDI)" after the frame rate
means the slower fallback capture is in use (see below).

## Settings

Everything is saved as you change it, under `HKCU\Software\VitureHUD`, except
zoom.

| Setting | Where to change it | Default | Stored as |
|---|---|---|---|
| Follow the text cursor | Panel, Behaviour | On | `FollowCaret` = `1` |
| Show a monitor when the glasses connect | Panel, Behaviour | On | `AutoShow` = `1` |
| Start with Windows | Panel, Behaviour; tray menu | Off | `VitureHUD` in `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` |
| Picture-in-picture | Panel, View; Alt+P | Off | `Pip` = `0` |
| Edge lens magnification | Panel, Edge lens | 1x | `EdgeLens` = `1` (0 off, 1 = 1x, 2 = 1.5x, 3 = 2x) |
| Edge lens placement | Panel, Edge lens | Follow cursor | `LensFixed` = `0` (1 = fixed spot) |
| Edge lens size | Lens editor: drag the corner | 33% of the glasses | `LensSize` = `333` (thousandths) |
| Fixed spot | Lens editor: drag the lens | Centre | `LensX` = `500`, `LensY` = `500` (thousandths) |
| Edge offset (follow cursor) | Lens editor: drag a dashed edge | 12% | `LensInset` = `120` (thousandths) |
| Trigger band | Lens editor: drag the band's edge | 12% | `LensTrigger` = `120` (thousandths) |
| Zoom | Panel, View; Alt+Z | 1x | Not saved: 1x after a restart |

Limits: lens size 15-60%, edge offset 0-30%, trigger band 3-30%, all as a share
of the glasses' width and height.

## Tips and troubleshooting

| Symptom | Cause and fix |
|---|---|
| "Viture glasses not connected" | Windows does not see a display named VITURE (or VITURE plus a model name). Check it appears in Settings > Display; unplug and replug the glasses. |
| Status bar says "(GDI)", low frame rate | Desktop Duplication is unavailable for that monitor, usually because **HDR** is on for it, or the monitor is rotated. Turn HDR off for that monitor to get full speed. |
| Edge lens never appears over a game or Moonlight | Games and game-streaming apps capture the mouse: Windows' cursor is hidden and held in the middle, and the cursor you see is part of the picture. In Moonlight, turn on **Optimize mouse for remote desktop instead of games**. |
| The view does not follow typing in some app | That app does not report its text cursor to Windows. The view keeps following the mouse there. |
| A hotkey does nothing | Another app registered it first. A message appears at start-up for Alt+D. |
| Glasses show 1280x720 stretched or cropped | Stale Windows display state. Unplug and replug the glasses. |

A log is kept at `%LOCALAPPDATA%\VitureHUD\viture_hud.log` (it starts over at
1 MB). Please attach it to bug reports.

## How it works

```
another monitor
      │  Desktop Duplication: its image as a GPU texture, every frame it changes
      ▼
zoomed and panned to follow the mouse or text cursor
      │  Direct3D 11; cursor, status bar and frames drawn with GDI
      ▼
fullscreen window on the glasses' display
```

The glasses are just another display to Windows. Viture HUD covers it with one
always-on-top window and draws a live copy of another monitor into it. Clicks
on that window are ignored and never take focus.

## Building from source

You need MinGW-w64 with `g++` and `windres` on PATH. The easiest way is
[MSYS2](https://www.msys2.org/):

```
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-binutils
```

Then add `C:\msys64\mingw64\bin` to PATH (or use the MSYS2 MINGW64 shell) and
run:

```
build.bat
```

It builds `build\viture_hud.exe` and runs the geometry tests in
`tests\test_view_math.cpp`. The exe is linked statically, so it runs without
any MinGW DLLs.

### Layout

| Path | What |
|---|---|
| `src/viture_hud.cpp` | Tray app: monitors, hotkeys, settings, window picking |
| `src/viewer.cpp`, `viewer.h` | The view on the glasses: capture, rendering, panning, PiP, edge lens |
| `src/panel.cpp`, `panel.h` | Control panel (Direct2D) |
| `src/view_math.h` | All geometry, as small pure functions |
| `src/glasses_id.h` | Which displays are VITURE glasses (name and EDID manufacturer) |
| `tests/test_view_math.cpp` | Tests for `view_math.h` and `glasses_id.h` |
| `tools/check_docs.ps1` | Checks this README against the code |
| `res/` | Icon |

Anything about where a pixel goes belongs in `view_math.h`, with a test.

## Contributing

Issues and pull requests are welcome. Please run `build.bat` (it must build
without warnings and pass the tests) and describe how you tested on real
glasses, since most of the app can only be checked by looking through them.

This README is checked against the code. `build.bat` runs
`tools\check_docs.ps1`, which fails the build when a hotkey, a setting, a
setting's default or a lens limit in the code is missing from this file. When
you add or change one, update the tables above.

## License

Copyright (C) 2026 Jurica Dujmovic

Viture HUD is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version. See [LICENSE](LICENSE).
