// Copyright (C) 2026 Jurica Dujmovic
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the viewer geometry (src/view_math.h) and glasses detection
// (src/glasses_id.h). build.bat runs them.
//
// They lock in the behaviour that took a while to get right: zoom steps,
// panning that reaches exactly the content's edges, the dead zone and easing
// of smooth panning, and where the picture-in-picture and edge lens go.
#include "glasses_id.h"
#include "view_math.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

static int g_failures = 0;
static int g_checks = 0;

static void Check(bool ok, const char* what) {
  g_checks++;
  if (!ok) {
    g_failures++;
    printf("  FAIL: %s\n", what);
  }
}

static void CheckNear(double got, double want, double tol, const char* what) {
  g_checks++;
  if (std::fabs(got - want) > tol) {
    g_failures++;
    printf("  FAIL: %s (got %.3f, want %.3f)\n", what, got, want);
  }
}

static void TestBarAlpha() {
  printf("status bar fade\n");
  using namespace viewmath;
  Check(BarAlpha(0) == 255, "bar is opaque when just shown");
  Check(BarAlpha(900) == 255, "bar stays opaque during the hold");
  Check(BarAlpha(1300) < 255, "bar is fading after the hold");
  Check(BarAlpha(2000) == 0, "bar is invisible once the fade ends");
}

static void TestZoom() {
  printf("zoom steps\n");
  using namespace viewmath;
  Check(NextZoomStep(0, +1) == 1, "Alt+Z goes from 1x to the next step");
  Check(NextZoomStep(kZoomStepCount - 1, +1) == 0, "zoom wraps back to 1x");
  Check(NextZoomStep(0, -1) == kZoomStepCount - 1, "Alt+Shift+Z wraps backwards");
  CheckNear(kZoomSteps[0], 1.0, 1e-9, "the first step is native 1:1");
  CheckNear(kZoomSteps[kFitStep], 0.0, 1e-9, "the fit step is marked 0");

  CheckNear(EffectiveScale(0.0, 1600, 1080), 0.675, 1e-9,
            "fit fills the glasses' height with the LG (0.675x)");
  PanRange lgFit = AxisPanRange(3840, 0.675, 1920);
  CheckNear(lgFit.max, 3840 * 0.675 - 1920, 1e-9,
            "the LG at fit still pans sideways");
  CheckNear(EffectiveScale(0.0, 1080, 1080), 1.0, 1e-9,
            "fit shows a 1080p monitor at 1x");
  CheckNear(EffectiveScale(1.5, 1600, 1080), 1.5, 1e-9,
            "explicit zoom is used as is");

  Check(IsPixelExact(1.0), "1x is pixel exact");
  Check(IsPixelExact(2.0), "2x is pixel exact");
  Check(!IsPixelExact(1.5), "1.5x needs filtering");
  Check(!IsPixelExact(0.5), "shrinking needs filtering");
}

static void TestPanRange() {
  printf("pan range\n");
  using namespace viewmath;
  PanRange lg = AxisPanRange(3840, 1.0, 1920);
  CheckNear(lg.min, 0, 1e-9, "LG at 1x pans from 0");
  CheckNear(lg.max, 1920, 1e-9, "LG at 1x pans to 1920");

  PanRange fit = AxisPanRange(1600, 0.5, 1080);
  CheckNear(fit.min, -140, 1e-9, "smaller content is centred (800 in 1080)");
  CheckNear(fit.max, fit.min, 1e-9, "centred content cannot pan");
}

static void TestDeadZone() {
  printf("dead zone panning\n");
  using namespace viewmath;
  PanRange r = AxisPanRange(3840, 1.0, 1920);  // 0..1920
  // margin = 20% of 1920 = 384

  CheckNear(DeadZoneTarget(500 + 960, 500, 1920, r), 500, 1e-9,
            "focus in the middle does not move the view");
  CheckNear(DeadZoneTarget(500 + 400, 500, 1920, r), 500, 1e-9,
            "focus just inside the dead zone does not move the view");
  CheckNear(DeadZoneTarget(500 + 1800, 500, 1920, r), 500 + 1800 - 1536, 1e-9,
            "focus near the right edge pulls the view right");
  CheckNear(DeadZoneTarget(500 + 100, 500, 1920, r), 500 + 100 - 384, 1e-9,
            "focus near the left edge pulls the view left");
  CheckNear(DeadZoneTarget(3839, 1900, 1920, r), 1920, 1e-9,
            "the view stops at the content's right edge");
  CheckNear(DeadZoneTarget(10, 300, 1920, r), 0, 1e-9,
            "the view stops at the content's left edge");
}

static void TestEase() {
  printf("easing and keep-visible\n");
  using namespace viewmath;
  double p = EaseToward(0, 1000, 16);
  Check(p > 0 && p < 1000, "one frame moves part of the way");
  double q = 0;
  for (int i = 0; i < 60; ++i) q = EaseToward(q, 1000, 16);
  CheckNear(q, 1000, 1e-9, "a second of frames arrives exactly");
  CheckNear(EaseToward(400, 1000, 0), 400, 1e-9, "no time, no movement");

  PanRange r = AxisPanRange(3840, 1.0, 1920);
  // Focus at 3000 with pan 0 would be off screen; it must be pulled into view.
  double kept = KeepVisible(0, 3000, 1920, r);
  Check(3000 - kept <= 1920 - kKeepVisiblePad, "focus is kept inside the view");
  CheckNear(KeepVisible(500, 1000, 1920, r), 500, 1e-9,
            "a visible focus leaves the pan alone");
  CheckNear(CentreOn(1000, 1920, r), 40, 1e-9, "centring puts focus mid-view");
  CheckNear(CentreOn(100, 1920, r), 0, 1e-9, "centring respects the edges");
}

static void TestPip() {
  printf("picture-in-picture\n");
  using namespace viewmath;
  PipRect p = PlacePip(1920, 1080, 1920, 1080, false, 0, 0);
  Check(p.width == 480 && p.height == 270, "a 1080p monitor shows at 480x270");
  Check(!p.left && p.x == 1920 - 24 - 480, "it sits bottom-right by default");
  Check(p.y == 1080 - 24 - 270, "with a margin from the bottom");

  PipRect wide = PlacePip(1920, 1080, 3840, 1600, false, 0, 0);
  Check(wide.width == 480 && wide.height == 200, "an ultrawide keeps its shape");

  PipRect dodge = PlacePip(1920, 1080, 1920, 1080, true, 1600, 950);
  Check(dodge.left && dodge.x == 24, "focus under it moves it bottom-left");
  PipRect near = PlacePip(1920, 1080, 1920, 1080, true, 1400, 770);
  Check(near.left, "focus just next to it moves it too");
  PipRect above = PlacePip(1920, 1080, 1920, 1080, true, 1600, 300);
  Check(!above.left, "focus higher up leaves it bottom-right");
  PipRect away = PlacePip(1920, 1080, 1920, 1080, true, 200, 950);
  Check(!away.left, "focus bottom-left leaves it bottom-right");
}

static void TestEdgeLens() {
  printf("edge lens\n");
  using namespace viewmath;
  // 12% of 1920 = 230.4, of 1080 = 129.6
  Check(!LensWanted(false, 960, 540, 1920, 1080), "no lens in the middle");
  Check(LensWanted(false, 100, 540, 1920, 1080), "lens near the left edge");
  Check(LensWanted(false, 1850, 540, 1920, 1080), "lens near the right edge");
  Check(LensWanted(false, 960, 60, 1920, 1080), "lens near the top edge");
  Check(LensWanted(false, 1900, 1070, 1920, 1080), "lens in a corner");
  Check(!LensWanted(false, 240, 540, 1920, 1080), "just outside the band: none");
  Check(LensWanted(true, 240, 540, 1920, 1080),
        "once showing, it stays a little longer (no flicker)");
  Check(!LensWanted(true, 300, 540, 1920, 1080), "further in, it hides");
  Check(LensWanted(false, 400, 540, 1920, 1080, 0.25),
        "a wider trigger band shows it further from the edge");
  Check(!LensWanted(false, 100, 540, 1920, 1080, 0.04),
        "a narrow band only shows it right at the edge");
  LensLayout wide;
  wide.trigger = 0.9;
  CheckNear(ClampLensLayout(wide).trigger, kMaxTriggerFraction, 1e-9,
            "the trigger band has a maximum");
  wide.trigger = 0;
  CheckNear(ClampLensLayout(wide).trigger, kMinTriggerFraction, 1e-9,
            "and a minimum");

  LensRect l = PlaceLens(20, 540, 1920, 1080);
  Check(l.width == 640 && l.height == 360, "the lens is a third of the view");
  Check(l.x == 230, "at the left edge it sits just inside the safe area");
  Check(l.y == 540 - 180, "level with the focus point");
  LensRect corner = PlaceLens(1910, 1075, 1920, 1080);
  Check(corner.x + corner.width == 1920 - 230 &&
            corner.y + corner.height == 1080 - 130,
        "in the bottom-right corner it sits inside both safe edges");

  LensSource same = LensSourceRect(1000, 500, 3840, 1600, 640, 360, 1.0);
  CheckNear(same.width, 640, 1e-9, "at 1x the lens shows 640 content px");
  CheckNear(same.x, 680, 1e-9, "centred on the focus point");
  LensSource zoomed = LensSourceRect(1000, 500, 3840, 1600, 640, 360, 2.0);
  CheckNear(zoomed.width, 320, 1e-9, "at 2x it shows half as much");
  LensSource atEdge = LensSourceRect(5, 5, 3840, 1600, 640, 360, 1.0);
  CheckNear(atEdge.x, 0, 1e-9, "it never reaches past the content's edge");
  CheckNear(atEdge.y, 0, 1e-9, "top edge too");
  LensSource farEdge = LensSourceRect(3839, 1599, 3840, 1600, 640, 360, 1.0);
  CheckNear(farEdge.x + farEdge.width, 3840, 1e-9, "or the far edge");
}

static void TestLensLayout() {
  printf("edge lens layout\n");
  using namespace viewmath;
  CheckNear(ClampLensSize(0.05), kLensMinSize, 1e-9, "size has a minimum");
  CheckNear(ClampLensSize(0.9), kLensMaxSize, 1e-9, "and a maximum");

  LensLayout big;
  big.size = 0.5;
  LensRect grown = PlaceLensFor(big, 20, 540, 1920, 1080);
  Check(grown.width == 960 && grown.height == 540,
        "a bigger lens follows the cursor at its own size");
  Check(grown.x == 230, "still just inside the safe area");

  LensLayout fixed;
  fixed.fixed = true;
  fixed.size = 0.25;
  fixed.centreX = 0.5;
  fixed.centreY = 0.75;
  LensRect spot = PlaceLensFor(fixed, 20, 20, 1920, 1080);
  Check(spot.width == 480 && spot.height == 270, "fixed lens has its size");
  Check(spot.x == 960 - 240 && spot.y == 810 - 135,
        "fixed lens sits at its spot, wherever the cursor is");

  LensLayout further;
  further.inset = 0.25;
  LensRect deeper = PlaceLensFor(further, 20, 540, 1920, 1080);
  Check(deeper.x == 480, "a bigger edge offset keeps the lens further in");
  LensLayout flush;
  flush.inset = 0;
  Check(PlaceLensFor(flush, 20, 540, 1920, 1080).x == 0,
        "offset 0 lets it touch the edge");
  CheckNear(ClampLensLayout(LensLayout{false, 0.3, 0.5, 0.5, 0.9}).inset,
            kMaxInsetFraction, 1e-9, "the offset has a maximum");
  LensLayout crowded;
  crowded.size = 0.6;
  crowded.inset = 0.3;
  LensRect centred = PlaceLensFor(crowded, 20, 540, 1920, 1080);
  Check(centred.x == (1920 - centred.width) / 2,
        "if lens and offset do not fit side by side, it is centred");

  LensLayout off;
  off.fixed = true;
  off.size = 0.4;
  off.centreX = 0.99;
  off.centreY = 0.01;
  LensLayout kept = ClampLensLayout(off);
  CheckNear(kept.centreX, 0.8, 1e-9, "a fixed lens never hangs off the right");
  CheckNear(kept.centreY, 0.2, 1e-9, "or off the top");
  LensRect onScreen = PlaceLensFor(off, 0, 0, 1920, 1080);
  Check(onScreen.x + onScreen.width <= 1920 && onScreen.y >= 0,
        "so it is always fully on the glasses");
}

static void TestGlassesId() {
  printf("glasses detection\n");
  using namespace glassesid;
  Check(IsVitureName(L"VITURE"), "Luma Ultra: the name VITURE");
  Check(IsVitureName(L"VITURE Beast"), "Beast: VITURE plus a model name");
  Check(IsVitureName(L"VITURE Luma Ultra"), "a model name with spaces");
  Check(IsVitureName(L"viture beast"), "the name is not case sensitive");
  Check(!IsVitureName(L"VITUREX"), "a longer word is not VITURE");
  Check(!IsVitureName(L"My VITURE"), "VITURE must come first");
  Check(!IsVitureName(L"DELL U2720Q"), "an ordinary monitor is not glasses");
  Check(!IsVitureName(L""), "no name is not glasses");
  Check(IsVitureManufacturer(L"CVT"), "Luma Ultra's manufacturer id CVT");
  Check(IsVitureManufacturer(L"VIT"), "Beast's manufacturer id VIT");
  Check(!IsVitureManufacturer(L"GSM"), "an LG monitor's id is not glasses");
  Check(!IsVitureManufacturer(L""), "no id is not glasses");
  Check(IsVitureDisplay(L"VITURE Beast", L"VIT"), "the Beast as Windows reports it");
  Check(IsVitureDisplay(L"", L"CVT"), "the id alone is enough");
  Check(IsVitureDisplay(L"VITURE", L""), "the name alone is enough");
  Check(!IsVitureDisplay(L"Generic PnP Monitor", L"DEL"), "a Dell is not glasses");
}

int main() {
  TestGlassesId();
  TestLensLayout();
  TestEdgeLens();
  TestPip();
  TestZoom();
  TestPanRange();
  TestDeadZone();
  TestEase();
  TestBarAlpha();

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}