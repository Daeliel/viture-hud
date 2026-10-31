// Copyright (C) 2026 Jurica Dujmovic
// SPDX-License-Identifier: GPL-3.0-or-later
//
// view_math.h - the geometry of the Viture HUD viewer, in one place.
//
// Everything about "where does a pixel go" lives here, as small pure
// functions with tests in tests/test_view_math.cpp. Painting, panning, the
// cursor, the picture-in-picture and the edge lens all call these, so they
// cannot drift apart.
//
// THE MODEL
// ---------
// The glasses are a display (typically 1920x1080). A fullscreen window covers
// it and shows ONE monitor - or one locked window - scaled by `scale`:
//
//   content (cW x cH)  x scale  =  magnified content (cW*scale x cH*scale)
//
// That is usually larger than the view, so you see a slice of it. `pan` is
// the slice's top-left in magnified pixels, one axis at a time. When the
// magnified content is smaller than the view it is centred instead, which
// shows up as a fixed negative pan.
//
//   view point = (content point * scale) - pan

#ifndef VIEW_MATH_H_
#define VIEW_MATH_H_

#include <cmath>

namespace viewmath {

// Height of the status bar. It is drawn over the image, so it does not
// reduce the area available to content.
constexpr int kBarHeight = 32;

// How long the status bar stays fully visible after a change, and how long it
// takes to fade out afterwards (milliseconds).
constexpr unsigned long kBarHoldMs = 1000;
constexpr unsigned long kBarFadeMs = 600;

// Opacity of the status bar, 0-255, given how long ago it was last shown.
inline int BarAlpha(unsigned long elapsedMs) {
  if (elapsedMs <= kBarHoldMs) return 255;
  unsigned long fade = elapsedMs - kBarHoldMs;
  if (fade >= kBarFadeMs) return 0;
  return 255 - (int)(255 * fade / kBarFadeMs);
}

// ---------------------------------------------------------------------------
// Zoom
// ---------------------------------------------------------------------------
// Alt+Z steps through these. 0 means "fit": the content (monitor or locked
// window) fills the glasses' height; anything wider pans sideways.
constexpr double kZoomSteps[] = {1.0, 1.5, 2.0, 0.0};
constexpr int kZoomStepCount = sizeof(kZoomSteps) / sizeof(kZoomSteps[0]);
constexpr int kFitStep = 3;

inline int NextZoomStep(int step, int direction) {
  int n = kZoomStepCount;
  return ((step + (direction >= 0 ? 1 : -1)) % n + n) % n;
}

// Scale at which the content fills the view's height. Fitting the whole of
// an ultrawide instead would halve its text (3840 wide in 1920), which is
// barely readable; filling the height shows a 3840x1600 monitor at 0.68x and
// pans across.
inline double FitHeightScale(int contentH, int viewH) {
  if (contentH <= 0 || viewH <= 0) return 1.0;
  return (double)viewH / contentH;
}

// The scale actually drawn for a zoom step value (0 = fit height).
inline double EffectiveScale(double zoom, int contentH, int viewH) {
  return zoom > 0 ? zoom : FitHeightScale(contentH, viewH);
}

// At whole-number scales every source pixel lands on whole view pixels, so it
// must be sampled without filtering to stay pixel-sharp.
inline bool IsPixelExact(double scale) {
  return scale >= 1.0 && std::fabs(scale - std::round(scale)) < 1e-9;
}

// ---------------------------------------------------------------------------
// Smooth panning
//
// Pan is the view's top-left in magnified content pixels, one axis at a time.
// When the magnified content is smaller than the view it is centred, which
// shows up as a fixed negative pan.
// ---------------------------------------------------------------------------
struct PanRange {
  double min = 0;
  double max = 0;
};

inline PanRange AxisPanRange(int contentSize, double scale, int viewSize) {
  double mag = contentSize * scale;
  if (mag >= viewSize) return {0.0, mag - viewSize};
  double centred = -(viewSize - mag) / 2.0;
  return {centred, centred};
}

inline double ClampToRange(double pan, const PanRange& r) {
  if (pan < r.min) return r.min;
  if (pan > r.max) return r.max;
  return pan;
}

// The focus point (mouse or text cursor) moves freely inside the middle of the
// view; the view only follows once it comes within this fraction of an edge.
constexpr double kDeadZoneFraction = 0.2;

inline double DeadZoneTarget(double focusMag, double pan, int viewSize,
                             const PanRange& r) {
  double margin = viewSize * kDeadZoneFraction;
  double pos = focusMag - pan;
  double target = pan;
  if (pos < margin) {
    target = focusMag - margin;
  } else if (pos > viewSize - margin) {
    target = focusMag - (viewSize - margin);
  }
  return ClampToRange(target, r);
}

// Time constant of the glide towards the target, in milliseconds.
constexpr double kPanEaseMs = 70.0;

inline double EaseToward(double pan, double target, double elapsedMs) {
  if (elapsedMs <= 0) return pan;
  double k = 1.0 - std::exp(-elapsedMs / kPanEaseMs);
  double next = pan + (target - pan) * k;
  if (std::fabs(target - next) < 0.5) next = target;
  return next;
}

// However the glide is going, the focus point never leaves the view: it stays
// at least kKeepVisiblePad pixels inside the far edge.
constexpr int kKeepVisiblePad = 32;

inline double KeepVisible(double pan, double focusMag, int viewSize,
                          const PanRange& r) {
  double lo = focusMag - (viewSize - kKeepVisiblePad);
  double hi = focusMag;
  if (pan < lo) pan = lo;
  if (pan > hi) pan = hi;
  return ClampToRange(pan, r);
}

// Pan that puts the focus point in the middle of the view.
inline double CentreOn(double focusMag, int viewSize, const PanRange& r) {
  return ClampToRange(focusMag - viewSize / 2.0, r);
}

// ---------------------------------------------------------------------------
// Picture-in-picture: a small live view of another monitor in a bottom corner.
// ---------------------------------------------------------------------------
constexpr double kPipWidthFraction = 0.25;  // of the view's width
constexpr int kPipMargin = 24;              // from the view's edges
constexpr int kPipDodge = 48;               // how near the focus may come

struct PipRect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  bool left = false;  // moved to the bottom-left corner
};

// Bottom-right normally. When the focus point (mouse or text cursor, in view
// pixels) is on or near that spot, it moves to the bottom-left, so the PiP
// never covers what you are working on.
inline PipRect PlacePip(int viewW, int viewH, int srcW, int srcH,
                        bool haveFocus, double focusX, double focusY) {
  PipRect p;
  if (viewW <= 0 || viewH <= 0 || srcW <= 0 || srcH <= 0) return p;
  p.width = (int)std::lround(viewW * kPipWidthFraction);
  p.height = (int)std::lround((double)p.width * srcH / srcW);
  p.y = viewH - kPipMargin - p.height;
  int rightX = viewW - kPipMargin - p.width;
  p.left = haveFocus && focusX >= rightX - kPipDodge &&
           focusY >= p.y - kPipDodge;
  p.x = p.left ? kPipMargin : rightX;
  return p;
}

// ---------------------------------------------------------------------------
// Edge lens
//
// The glasses' optics blur and darken the edges and corners of the picture.
// When the focus point (mouse or text cursor) gets near an edge, a lens
// further in shows the area around it - at the same size, or magnified.
// ---------------------------------------------------------------------------
constexpr double kEdgeZoneFraction = 0.12;  // default edge band that triggers it
constexpr double kMinTriggerFraction = 0.03;
constexpr double kMaxTriggerFraction = 0.3;
constexpr double kSafeInsetFraction = 0.12; // default: the lens stays this far in
constexpr double kMaxInsetFraction = 0.3;
constexpr int kLensHysteresis = 24;         // pixels further in before it hides

// Lens size, as a fraction of the view (the lens has the view's shape).
constexpr double kLensDefaultSize = 1.0 / 3;
constexpr double kLensMinSize = 0.15;
constexpr double kLensMaxSize = 0.6;

// Where the lens appears, as the user set it in the control panel.
struct LensLayout {
  bool fixed = false;               // false: follow the cursor
  double size = kLensDefaultSize;   // fraction of the view
  double centreX = 0.5;             // fixed spot, as fractions of the view
  double centreY = 0.5;
  double inset = kSafeInsetFraction;  // following: distance kept from edges
  double trigger = kEdgeZoneFraction;  // the lens shows within this of an edge
};

inline double ClampLensSize(double size) {
  if (size < kLensMinSize) return kLensMinSize;
  if (size > kLensMaxSize) return kLensMaxSize;
  return size;
}

// Keeps a fixed lens of |size| entirely on the view.
inline LensLayout ClampLensLayout(LensLayout l) {
  l.size = ClampLensSize(l.size);
  if (l.inset < 0) l.inset = 0;
  if (l.inset > kMaxInsetFraction) l.inset = kMaxInsetFraction;
  if (l.trigger < kMinTriggerFraction) l.trigger = kMinTriggerFraction;
  if (l.trigger > kMaxTriggerFraction) l.trigger = kMaxTriggerFraction;
  double half = l.size / 2;
  if (l.centreX < half) l.centreX = half;
  if (l.centreX > 1 - half) l.centreX = 1 - half;
  if (l.centreY < half) l.centreY = half;
  if (l.centreY > 1 - half) l.centreY = 1 - half;
  return l;
}

// Whether the lens should show for a focus point at x,y in view pixels.
// |showing| is the current state: once on, the point must move a little
// further in before the lens hides, so it does not flicker at the boundary.
inline bool LensWanted(bool showing, double x, double y, int viewW,
                       int viewH, double trigger = kEdgeZoneFraction) {
  double zx = viewW * trigger + (showing ? kLensHysteresis : 0);
  double zy = viewH * trigger + (showing ? kLensHysteresis : 0);
  return x < zx || x > viewW - zx || y < zy || y > viewH - zy;
}

struct LensRect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

// Following the cursor: inside the safe area, as close to the focus point x,y
// as it can get.
inline LensRect PlaceLens(double x, double y, int viewW, int viewH,
                          double size = kLensDefaultSize,
                          double inset = kSafeInsetFraction) {
  size = ClampLensSize(size);
  LensRect l;
  l.width = (int)std::lround(viewW * size);
  l.height = (int)std::lround(viewH * size);
  double insetX = viewW * inset;
  double insetY = viewH * inset;
  double minCx = insetX + l.width / 2.0;
  double maxCx = viewW - insetX - l.width / 2.0;
  double minCy = insetY + l.height / 2.0;
  double maxCy = viewH - insetY - l.height / 2.0;
  // A big lens with a big offset may not fit between the edges: centre it.
  if (minCx > maxCx) minCx = maxCx = viewW / 2.0;
  if (minCy > maxCy) minCy = maxCy = viewH / 2.0;
  double cx = x < minCx ? minCx : (x > maxCx ? maxCx : x);
  double cy = y < minCy ? minCy : (y > maxCy ? maxCy : y);
  l.x = (int)std::lround(cx - l.width / 2.0);
  l.y = (int)std::lround(cy - l.height / 2.0);
  return l;
}

// Where the lens goes for |layout|: at its fixed spot, or near x,y.
inline LensRect PlaceLensFor(const LensLayout& layout, double x, double y,
                             int viewW, int viewH) {
  if (!layout.fixed) {
    LensLayout l = ClampLensLayout(layout);
    return PlaceLens(x, y, viewW, viewH, l.size, l.inset);
  }
  LensLayout l = ClampLensLayout(layout);
  LensRect r;
  r.width = (int)std::lround(viewW * l.size);
  r.height = (int)std::lround(viewH * l.size);
  r.x = (int)std::lround(viewW * l.centreX - r.width / 2.0);
  r.y = (int)std::lround(viewH * l.centreY - r.height / 2.0);
  return r;
}

struct LensSource {
  double x = 0;
  double y = 0;
  double width = 0;
  double height = 0;
};

// The part of the content (in content pixels) the lens shows: centred on the
// focus point, kept inside the content, sized so it fills the lens at
// |lensScale|. At pixel-exact scales the corner is whole pixels, so 1x stays
// sharp.
inline LensSource LensSourceRect(double focusX, double focusY, int contentW,
                                 int contentH, int lensW, int lensH,
                                 double lensScale) {
  LensSource s;
  if (lensScale <= 0) return s;
  s.width = lensW / lensScale;
  s.height = lensH / lensScale;
  if (s.width > contentW) s.width = contentW;
  if (s.height > contentH) s.height = contentH;
  s.x = focusX - s.width / 2;
  s.y = focusY - s.height / 2;
  if (s.x < 0) s.x = 0;
  if (s.y < 0) s.y = 0;
  if (s.x + s.width > contentW) s.x = contentW - s.width;
  if (s.y + s.height > contentH) s.y = contentH - s.height;
  if (IsPixelExact(lensScale)) {
    s.x = std::floor(s.x);
    s.y = std::floor(s.y);
  }
  return s;
}

}  // namespace viewmath

#endif  // VIEW_MATH_H_