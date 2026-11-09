# Viture HUD

Use your VITURE XR glasses as a sharp, private view of your other monitors on
Windows. One monitor at a time shows on the glasses at native 1:1 pixels, the
view follows your mouse and text cursor, and one hotkey switches screens. Your
real monitors are never reconfigured: no mirroring, no resolution changes, and
no virtual display driver needed.

**Privacy mode** blacks out your real screens while the glasses keep showing
your work, so a laptop on its own becomes a private workspace that nobody can
read over your shoulder.

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
- **Privacy mode** (Ctrl+Alt+P): your screens go dark, the glasses keep
  working. Your screens are covered, or switched off entirely when a virtual
  display can stand in for them. Works on a laptop on its own and with any
  number of monitors.
- **Your own shortcuts**: every hotkey can be changed in the control panel,
  which also warns you when another app already uses one.
- **Starts by itself** when the glasses are plugged in.
- **Control panel** with live thumbnails of your monitor layout and every
  setting. Single small exe, no installer, no admin rights.

## Requirements

- Windows 10 (1903 or later) or Windows 11, 64-bit. Privacy mode's covering
  needs Windows 10 version 2004 or later.
- VITURE glasses connected as a display (USB-C with DisplayPort, or through an
  HDMI adapter or dock), plus at least one other screen. A laptop's own screen
  counts.
- A graphics card with Direct3D 11 - any from the last decade.
- Optional, for switching screens fully off in Privacy mode: a virtual
  display driver (see [Privacy mode](#privacy-mode)).

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
monitors, Privacy mode and its settings, start with Windows, create a desktop
shortcut, keyboard shortcuts, about, exit.

| Key (default) | Action |
|---|---|
| **Alt+D** | Show the next monitor on the glasses (the mouse and focus go along); after the last one, the glasses become a normal extended screen, then it starts over |
| **Alt+Shift+D** | Show the previous monitor |
| **Ctrl+Alt+D** | Stop: the glasses go back to a normal extended desktop, and the mouse onto them |
| **Alt+Z** | Zoom: 1x → 1.5x → 2x → Fit → 1x |
| **Alt+Shift+Z** | Zoom the other way |
| **Alt+W** | Show only the active window, fitted to the glasses (again to unlock) |
| **Alt+P** | Picture-in-picture on or off |
| **Ctrl+Alt+P** | Privacy mode on or off |

These are global hotkeys: while Viture HUD runs, other apps do not receive
them. Change any of them under **Shortcuts** in the control panel (see
[Shortcuts](#shortcuts)).

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
  monitor. It shows only while the glasses show a monitor, not while they
  are an ordinary extended screen.
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
- **Next monitor**, **Privacy** and **Stop** buttons. Privacy turns green
  while it is on; the cog on its right opens the Privacy card.
- The bottom line shows what is on the glasses, the zoom and the frame rate.
- **Shortcuts** (top right, or in the tray menu): change the hotkeys. An
  orange dot means one of them does not work.
- **About** (top right, or in the tray menu): who made Viture HUD and why,
  with a link to this repository. Esc closes it.

The panel stays on the monitor shown on the glasses and moves with it when you
switch.

### Privacy mode

Press **Ctrl+Alt+P** (or click **Privacy** in the panel) and your screens go
dark, while the glasses keep showing your work. Press it again and they come
back. Privacy mode also ends by itself when you unplug the glasses, exit
Viture HUD or sign out, so you are never left with a dark screen and nothing
to see on.

To work on the glasses as an ordinary extended screen while Privacy mode is
on, press **Ctrl+Alt+D**: the glasses stop showing a monitor, the mouse moves
onto them and the other screens stay dark. Turning Privacy mode on while the
glasses are an extended screen keeps them that way.

The Privacy card (the cog next to **Privacy**) has its settings:

- **Screens**: *Main screen* or *All screens*. The main screen is the
  laptop's own screen when it is on (the button then reads *Built-in
  screen*), otherwise your primary screen. *All screens* is every screen but
  the glasses, which are never blacked out.
- **How**:
  - *Cover* puts a black window over each screen. Windows leaves that window
    out of screen capture, so the glasses still show what is underneath, and
    nothing in your layout changes. A monitor's backlight stays on (a laptop
    screen can be dimmed, see **Turn the brightness down**), and the mouse
    pointer and some pop-ups, such as the Start menu or notifications, can
    still show on top.
  - *Switch off* turns the screen off entirely. In the same instant a
    **stand-in display** is switched on with exactly that screen's position
    and resolution (and its place as the main display), so your windows stay
    where they are and keep showing on the glasses. All screens flicker black
    for a second or two while Windows does this. Each screen needs its own
    stand-in: with *All screens*, the screens left without one are covered
    instead, and so is the screen when there is no stand-in at all.
- **Stand-in display**: the virtual display used first for *Switch off*. With
  *All screens*, any other spare virtual displays are used for the rest.
- **Turn the brightness down**: while covered, a laptop's built-in screen goes
  to 0% brightness, and back to where it was afterwards.

Setting up a stand-in, once:

1. Install a virtual display driver, for example
   [Virtual Display Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver).
   For *All screens*, give it one virtual display per screen.
2. Give each virtual display the same resolution as the screen it stands in
   for (in the driver's own settings).
3. In **Settings > System > Display**, select each virtual display and choose
   **Disconnect this display**. It stays available, just switched off.
4. Pick one under **Stand-in display** in the Privacy card.

A stand-in has to be a spare display. A virtual display you use as an extra
screen can't also be a stand-in, so with a single virtual display it is one or
the other: an extra screen, or *Switch off* (the Privacy card tells you when
this is the case). Some laptops struggle with more than one virtual display;
keeping the laptop's own screen as the primary one is the most stable setup.

Only virtual displays are offered as stand-ins, never a real screen that is
switched off: that would light it up with your work. Virtual displays that are
already on are not switched off, as nobody can see them. If a stand-in cannot
be set to its screen's resolution, Privacy mode puts everything back, covers
the screens instead, and says why. Each stand-in gets the same scaling (100%,
125%...) as the screen it replaces, so apps keep their size; Windows remembers
it for next time.

The switch is never saved to Windows' display settings. If Viture HUD is
closed by a crash while the screen is off, it puts the screen back the next
time it starts. You can always do it yourself with **Win+P** > **PC screen
only**, by unplugging the glasses, or by restarting.

### Shortcuts

Open **Shortcuts** in the panel's top right. Click a shortcut's keys, then
press the new ones. Esc cancels; Backspace removes the shortcut. Viture HUD
checks the new keys straight away and refuses them, with the reason, when:

- another app (or Windows) already has them as a global hotkey,
- they are already one of Viture HUD's own,
- they would break normal typing: a plain letter, Shift plus a letter, or
  Ctrl plus a key (Ctrl+C, Ctrl+S...) - add Alt, Win or Shift,
- Windows itself uses them (Alt+F4, Alt+Tab...),
- on your keyboard layout, Ctrl+Alt plus that key types a character (AltGr
  keys, such as @ on German keyboards).

F13-F24, if your keyboard or mouse has them, work without a modifier.
Shortcuts that only work inside one app, like Alt+B in Firefox, are not
visible to other programs, so they cannot be detected. If an app stops reacting
to a key while Viture HUD runs, pick a different shortcut. **Reset to
defaults** puts back the keys in the table above.

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
| Privacy: screens | Privacy card | Main screen | `PrivacyScreens` = `0` (1 = all screens) |
| Privacy: how | Privacy card | Switch off (covers without a stand-in) | `PrivacyMethod` = `0` (1 = always cover) |
| Privacy: stand-in display | Privacy card | None | `PrivacyStandIn` = empty (the display's device path) |
| Privacy: turn the brightness down | Privacy card | On | `PrivacyDim` = `1` |
| Shortcut: next monitor | Shortcuts card | Alt+D | `HotkeyNext` = `Alt+D` |
| Shortcut: previous monitor | Shortcuts card | Alt+Shift+D | `HotkeyPrevious` = `Alt+Shift+D` |
| Shortcut: stop | Shortcuts card | Ctrl+Alt+D | `HotkeyStop` = `Ctrl+Alt+D` |
| Shortcut: zoom | Shortcuts card | Alt+Z | `HotkeyZoom` = `Alt+Z` |
| Shortcut: zoom the other way | Shortcuts card | Alt+Shift+Z | `HotkeyZoomBack` = `Alt+Shift+Z` |
| Shortcut: window lock | Shortcuts card | Alt+W | `HotkeyWindowLock` = `Alt+W` |
| Shortcut: picture-in-picture | Shortcuts card | Alt+P | `HotkeyPip` = `Alt+P` |
| Shortcut: Privacy mode | Shortcuts card | Ctrl+Alt+P | `HotkeyPrivacy` = `Ctrl+Alt+P` |

Shortcuts are stored as text: modifiers in the order Ctrl, Alt, Shift, Win,
then the key (`A`-`Z`, `0`-`9`, `F1`-`F24`, `Num0`-`Num9`, `Space`, `Left`,
`PageUp`... and punctuation keys by code, such as `0xBA`). An empty value means
no shortcut.

While Privacy mode has something to undo, two more values exist next to
these: `PrivacyRestoreBrightness` (the brightness to go back to) and
`PrivacyRestoreDisplays`. They are removed as soon as everything is back.

Limits: lens size 15-60%, edge offset 0-30%, trigger band 3-30%, all as a share
of the glasses' width and height.

## Tips and troubleshooting

| Symptom | Cause and fix |
|---|---|
| "Viture glasses not connected" | Windows does not see a display named VITURE (or VITURE plus a model name). Check it appears in Settings > Display; unplug and replug the glasses. |
| Status bar says "(GDI)", low frame rate | Desktop Duplication is unavailable for that monitor, usually because **HDR** is on for it, or the monitor is rotated. Turn HDR off for that monitor to get full speed. |
| Edge lens never appears over a game or Moonlight | Games and game-streaming apps capture the mouse: Windows' cursor is hidden and held in the middle, and the cursor you see is part of the picture. In Moonlight, turn on **Optimize mouse for remote desktop instead of games**. |
| The view does not follow typing in some app | That app does not report its text cursor to Windows. The view keeps following the mouse there. |
| A hotkey does nothing | Another app registered it first. A message at start-up names it, and the Shortcuts card marks it in orange: pick other keys there. |
| An app stops reacting to a key | Viture HUD has that key as a global hotkey. Change it in the Shortcuts card. |
| Privacy mode covers instead of switching off | No stand-in display is picked, it is not connected, or it cannot take the screen's resolution. With *All screens*, a screen is also covered when there are fewer stand-ins than screens. The Privacy card says which. |
| Privacy mode says covering needs a newer Windows | Covering needs Windows 10 version 2004 or later. Switch off (with a stand-in) works without it. |
| A screen stayed off after a crash | Start Viture HUD again and it comes back, or press **Win+P** > **PC screen only**. |
| Mouse pointer visible on a covered screen | Windows draws the pointer above every window, so a cover cannot hide it. Use *Switch off* for a fully dark screen. |
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

It builds `build\viture_hud.exe` and runs the tests in
`tests\test_view_math.cpp`. The exe is linked statically, so it runs without
any MinGW DLLs.

### Layout

| Path | What |
|---|---|
| `src/viture_hud.cpp` | Tray app: monitors, hotkeys, settings, window picking, Privacy mode on/off |
| `src/viewer.cpp`, `viewer.h` | The view on the glasses: capture, rendering, panning, PiP, edge lens |
| `src/panel.cpp`, `panel.h` | Control panel (Direct2D), with the Shortcuts, Privacy and About cards |
| `src/privacy.cpp`, `privacy.h` | Privacy mode: covers, brightness, switching screens to stand-ins |
| `src/hotkeys.h` | Shortcuts as text, and which ones are allowed |
| `src/view_math.h` | All geometry, as small pure functions |
| `src/glasses_id.h` | Which displays are VITURE glasses (name and EDID manufacturer) |
| `tests/test_view_math.cpp` | Tests for `view_math.h`, `glasses_id.h` and `hotkeys.h` |
| `tools/check_docs.ps1` | Checks this README against the code |
| `res/` | Icon |

Anything about where a pixel goes belongs in `view_math.h`, with a test.

## Contributing

Issues and pull requests are welcome. Please run `build.bat` (it must build
without warnings and pass the tests) and describe how you tested on real
glasses, since most of the app can only be checked by looking through them.

This README is checked against the code. `build.bat` runs
`tools\check_docs.ps1`, which fails the build when a default shortcut, a
setting, a setting's default or a lens limit in the code is missing from this
file. When you add or change one, update the tables above.

Privacy mode's *Switch off* has not been tried on a laptop or with several
stand-ins yet. Reports, good or bad, are very welcome: say which virtual
display driver you used and how your screens are set up.

## License

Copyright (C) 2026 Jurica Dujmovic

Viture HUD is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version. See [LICENSE](LICENSE).
