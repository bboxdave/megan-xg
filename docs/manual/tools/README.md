# Screenshot tooling

Two PowerShell scripts that capture the manual's screenshots from the **running
standalone**, so a GUI change can be re-shot rather than re-described.

```powershell
. .\ShotKit.ps1          # dot-source; defines the Shot-* functions
Shot-Launch -Width 1636 -Height 1000
Shot-Save "part-module.png" -L 80 -T 538 -R 378 -B 185
Shot-Quit
```

Then `.\Compose.ps1` rebuilds the four derived images (`editor-modules`,
`two-views`, `bottom-bar-annotated`, `control-types`) from the raw captures.

## Before you start

**Back up `Documents\MEGAN XG\settings.xml` and restore it afterwards.** The
plugin rewrites it on exit, and capture changes the theme, the view and the
window size. Also back up `%APPDATA%\MEGAN XG\MEGAN XG.settings`.

Set the theme to `Dark` and `defaultEditorView` to `Editor` in settings.xml
before launching — that is the manual's house style.

**Close the app with `Stop-Process`, not `CloseMainWindow`,** if the session
edited anything: the unsaved-changes guard puts up a modal and the app never
exits. That is also why the settings backup matters — nothing is written on the
way out.

## How it works, and the traps

**Clicks are guarded on `WindowFromPoint` belonging to the plugin's process.**
`SetForegroundWindow` fails routinely here (Windows refuses focus stealing) and
an unguarded click then lands in another application. `Shot-Click` refuses and
warns rather than clicking blind. Expect the "could not bring the window to the
front" warning — it is not a failure.

**`PrintWindow(hwnd, dc, PW_RENDERFULLCONTENT)` captures the main window even
unfocused or occluded.** That is what makes this work without stealing focus.

**It does NOT capture popup menus or dialogs** — those are separate top-level
windows. For a menu, use `Shot-Screen` (grabs the desktop region). For a dialog,
find it with `Shot-Windows` and pass its handle: `Shot-Save name.png -Hwnd $h`.

**Synthetic KEYS live in `Keys.ps1`** (dot-source it after ShotKit): `Shot-Key <vk>
[-Shift] [-Repeat n]` and `Shot-KeyAlt <vk>`, both guarded on the plugin actually being
the foreground window -- `keybd_event` is not addressed to an HWND, so an unguarded key
goes wherever focus really is.

**Every key carries its SCAN CODE, and that is not optional.** `keybd_event(vk, 0, ...)`
sends scan code 0, and JUCE's Windows peer resolves a WM_CHAR back to the key's
*unshifted* character by mapping the message's scan code. With 0 the mapping fails and
JUCE keeps the shifted character, so **`Shift + .` arrives as `>` rather than as
`shift + .`** and a punctuation binding looks broken when it is not. That produced one
wrong conclusion before it was spotted. `Keys.ps1` fills the scan code from
`MapVirtualKey(vk, MAPVK_VK_TO_VSC)`.

**There is no modifier-click helper, but you do not need one.** Wrap `Shot-Click` in
a `keybd_event(0x11, 0, 0, 0)` / `keybd_event(0x11, 0, 2, 0)` pair (P/Invoke from
`user32.dll`) and Ctrl+click works, guard and all — same for Shift (`0x10`) and Alt
(`0x12`). A mouse WHEEL still cannot be injected.

**Every capture parks the cursor first** (`Shot-Park`) and waits out any tooltip.
A hover tip is a real window and lands in the shot otherwise. Pass `-NoPark` when
the cursor must stay put — holding a note to get the EQ spectrum, for instance.

**Client coordinates equal image coordinates for the main window** — JUCE draws
its own title bar, so `ClientToScreen(0,0)` is the window origin. **This is not
true of the plugin's dialogs**, which have native title bars: their client origin
sits ~31 px down, so a coordinate read off a dialog screenshot needs that
subtracted before `Shot-Click -Hwnd`.

**Graphic-display bars drag RELATIVELY, not to the cursor.** Calibrate before
computing targets: one measured drag gave ≈0.87 units per pixel. Right-click
resets a bar, which is the reliable way back to a known state.

**Derive coordinates from a fresh capture each session.** The window does not
come back the same size, and the module rects in `Compose.ps1` assume a
**1636×1000** editor window.

**Why 1636 and not a round 1600** (changed 2026-08-30, when the Voice module gained
the mute/solo columns and the design canvas went 1480 → 1516 wide): at 1000 px tall
the canvas is HEIGHT-limited, so the scale is pinned at 966/962 whatever the width,
and the only thing a wider window changes is the horizontal centring offset. 1636 is
the width at which that offset comes out the same as 1600 gave on the old canvas —
so every module LEFT of the Voice column lands on exactly the pixels it did before,
and the existing crop rectangles for them are still correct. Measured by cross-
correlating the old and new full-window captures over the left 1100 px: best
alignment at **dx = 0**, a sharp minimum. Only `first-run-editor`, `voice-module`,
`keyboard-module` and `bottom-bar` actually had to be re-shot.

It is also the width that keeps the on-screen SCALE the same as before, which is the
reason to prefer it over an arbitrary number: 1600 × (1516/1480) = 1639.

**A capture older than the window size cannot be re-framed, only re-composed.**
`drum-edit.png` was shot before the window went 1600 -> **1636**, so its canvas scale no
longer exists: locating its old crop inside the old full-window shot solved to `R = -28`,
which is impossible. Cross-correlating the two crops agreed on the origin and still
showed a ~2.7% scale difference. When that happens, stop trying to reproduce the frame —
pick a clean one at the CURRENT geometry (it now starts at the Part module's own left
border, like `part-module.png`) and accept the new dimensions.

**`first-run-editor.png` had been shot with the volume METERS on** for months, so its
VOLUME screen showed meter dashes rather than the solid setting bars `volumeMeters`'
documented default then gave. Corrected 2026-09-03 — and re-shot WITH the meters on
again 2026-09-04, when `volumeMeters` and `partTitleNames` were made default-on. The
rule that survives both is the one that was wrong the first time: **shoot at the
DOCUMENTED defaults, not the machine's**, and check every animation option rather than
only the ones the chapter is about. Write the shot `settings.xml` from the defaults
table in `A2-settings-xml-reference.md` and restore the user's afterwards.

**A drag leaves the FOCUS MARK on the control it moved** (`focusMarkMode` default 0 =
always show), and the older captures have no mark. Moving focus by clicking a voice-list
row opens the Voice Browser over the frame; the move that works is a bare click on a
knob in a module outside the crop — an `XgKnob` drags RELATIVELY, so a click that does
not move changes no value.

**A crop into a MODULE'S INTERIOR is only valid for the layout it was measured on.**
`Compose.ps1`'s "Value box" cell points at the Part module's EQ FREQ box, and the EQ
block moved on 2026-09-11 (up one band-label line, its rows taller by the MIDI-override
bar's strip) -- so the composite came back showing half a cell and part of its
neighbour. The module's own rect had not moved, which is exactly why nothing warned.
**Look at the four composites after re-shooting, rather than trusting the script's
"it only reads images/" idempotence.**

**Re-attaching to an already-running app needs BOTH script variables.** `Shot-Launch`
sets `$script:ShotProc` and `$script:ShotHwnd`; `Shot-Save` only needs the second, but
`Shot-Click` and `Shot-Drag` guard on `$script:ShotProc.Id`. Set only the hwnd and every
click is refused as "point not owned by the plugin" while captures keep working, which
reads as a hit-test problem and is not one.

**A SELECTOR'S POP-UP DOES NOT TAKE SYNTHETIC KEYS.** It opens on a click and draws
inside the main window, so `PrintWindow` captures it, but arrow keys sent with
`keybd_event` do not move its highlight -- the list is modal and its focus does not
receive them. **Drive the selector through the keyboard cursor instead**, which needs no
pop-up at all: Tab to the module, then Up steps to the NEXT item and Down to the
previous, live, exactly as the wheel does. Count the Tab stops from `navGroups()` order
-- System, Graphic Display, Voice, Multi EQ, Effect, Controller, Effect Parameters,
Part, Keyboard.
- **Count the Right presses from what is USABLE, not from what is drawn.** A bare
  control is skipped, so the Effect module's stops change with the unit: on an insertion
  the four return knobs are bare and PART is ONE Right from the unit box, while on a
  Variation with CON = INS it is two. Overshoot and the cursor walks into the NEXT
  module, where a following Enter opens a type-in and the digits land on some innocent
  parameter. That happened here; the recovery is to relaunch, since the standalone
  opens at defaults and persists nothing.

**Module crops can be COMPUTED rather than eyeballed.** Launch at **1636x1035**, which
puts the canvas at 1:1 (the height is what limits the scale, and 1000 gives 0.969).
Then find each module's bounding box by scanning for pixels that are not the page
background `(20,20,20)` inside a window containing only that module, and turn the box
into `Shot-Save` insets with a 2 px margin. The module design rects are in
`EditorView.cpp`. Do not scan a single ROW for the module background: the row will land
on a knob disc `(42,42,42)` or a value box and report a fragment.

## What cannot be captured here

The **VST-only** screens: the Audio window's per-part output checkboxes, the DAW
automation markers, and **`system-module.png`**, which carries both -- its VOL, ATT and
KSHIFT triangles and its dimmed read-only TEMPO exist only in a host. Both need a real
host. `../CAPTURE-LIST.md` tracks what is outstanding.
