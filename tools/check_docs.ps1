# Copyright (C) 2026 Jurica Dujmovic
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Checks README.md against the code, so the documentation cannot drift.
# build.bat runs it; it fails (exit 1) and lists what is missing when:
#   - a default shortcut in src/viture_hud.cpp's kHotkeys table is not in the
#     README's key table as **Keys**, or not in the settings table as
#     `HotkeyName` = `Keys`,
#   - a setting read from the registry is not in the README's settings table
#     with its default, written as `Name` = `default` (`Name` = empty for
#     text that is empty by default),
#   - a lens limit in src/view_math.h is not in the README as "min-max%".
#
# Run by hand: powershell -ExecutionPolicy Bypass -File tools\check_docs.ps1

param([string]$Readme = (Join-Path $PSScriptRoot "..\README.md"))

$root = Join-Path $PSScriptRoot ".."
$app = Get-Content -Raw (Join-Path $root "src\viture_hud.cpp")
$math = Get-Content -Raw (Join-Path $root "src\view_math.h")
$doc = Get-Content -Raw $Readme
$problems = @()

# Default shortcuts: {panel::kKeyNext, L"HotkeyNext", L"Alt+D", ...
$hotkeys = [regex]::Matches($app,
    '\{panel::kKey\w+,\s*L"(\w+)",\s*L"([^"]+)"')
foreach ($m in $hotkeys) {
    $name = $m.Groups[1].Value
    $combo = $m.Groups[2].Value
    if (-not $doc.Contains("**$combo**")) {
        $problems += "shortcut $combo is not in the README's key table"
    }
    if (-not $doc.Contains("``$name`` = ``$combo``")) {
        $problems += "shortcut setting $name is not in the README as ``$name`` = ``$combo``"
    }
}
if ($hotkeys.Count -eq 0) { $problems += "found no shortcuts in the code (check the pattern)" }

# Text settings: ReadSettingText(L"Name", L"default")
$texts = [regex]::Matches($app, 'ReadSettingText\(L"(\w+)",\s*L"([^"]*)"\)')
foreach ($m in $texts) {
    $name = $m.Groups[1].Value
    $default = $m.Groups[2].Value
    $want = if ($default -eq "") { "``$name`` = empty" } else { "``$name`` = ``$default``" }
    if (-not $doc.Contains($want)) {
        $problems += "setting $name is not in the README as $want"
    }
}

# Settings and their defaults: ReadSetting(L"Name", true) / ReadSettingNumber(L"Name", 120)
$settings = [regex]::Matches($app,
    'ReadSetting(Number)?\(L"(\w+)",\s*(true|false|\d+)\)')
foreach ($m in $settings) {
    $name = $m.Groups[2].Value
    $default = $m.Groups[3].Value
    if ($default -eq "true") { $default = "1" }
    if ($default -eq "false") { $default = "0" }
    if (-not $doc.Contains("``$name`` = ``$default``")) {
        $problems += "setting $name (default $default) is not in the README as ``$name`` = ``$default``"
    }
}
if ($settings.Count -eq 0) { $problems += "found no settings in the code (check the pattern)" }

# Lens limits, as whole percentages.
function Pct([string]$name) {
    $m = [regex]::Match($math, "constexpr double $name = ([0-9.]+);")
    if (-not $m.Success) { return $null }
    return [int][math]::Round([double]::Parse($m.Groups[1].Value,
        [Globalization.CultureInfo]::InvariantCulture) * 100)
}
$ranges = @(
    @("lens size", (Pct "kLensMinSize"), (Pct "kLensMaxSize")),
    @("edge offset", 0, (Pct "kMaxInsetFraction")),
    @("trigger band", (Pct "kMinTriggerFraction"), (Pct "kMaxTriggerFraction"))
)
$known = @()
foreach ($r in $ranges) {
    if ($null -eq $r[1] -or $null -eq $r[2]) {
        $problems += "could not read the $($r[0]) limits from view_math.h"
        continue
    }
    $text = "$($r[1])-$($r[2])%"
    $known += $text
    if (-not $doc.Contains($text)) {
        $problems += "$($r[0]) range $text is not in the README"
    }
}
# Every range the README mentions must be one of them, so a stale copy of a
# limit somewhere in the text is caught too.
foreach ($m in [regex]::Matches($doc, "\b\d+-\d+%")) {
    if ($known -notcontains $m.Value) {
        $problems += "README mentions the range $($m.Value), which matches no limit in the code"
    }
}

if ($problems.Count -gt 0) {
    Write-Host "[docs] README.md is out of date with the code:"
    foreach ($p in $problems) { Write-Host "  - $p" }
    exit 1
}
Write-Host "[docs] README matches the code: $($hotkeys.Count) shortcuts, $($settings.Count + $texts.Count) settings, $($ranges.Count) ranges"
exit 0
