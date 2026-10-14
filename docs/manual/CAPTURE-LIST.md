# Screenshot capture checklist

Generated from the `<!-- CAPTURE: -->` notes in the chapters — regenerate with
`python gen_capture_list.py` after adding or moving a placeholder. Not part of
the built manual.

Drop each file into `images/` under the name given and rebuild; the placeholder
box in the HTML becomes the real image with no edit to the chapter.

**House style:** editor shots in the **dark** theme unless the note says
otherwise, window large enough that nothing is ellipsized, PNG, cropped to the
module plus a few pixels of margin. Full-window shots at a consistent size.

All 32 shots captured; none outstanding.


## 01-getting-started.md

- [x] **`images/first-run-hardware.png`** — The Hardware view on first run  
      Hardware view, freshly launched, LCD showing the normal play screen. Full window including the bottom bar.
- [x] **`images/first-run-editor.png`** — The Editor view  
      Editor view, dark theme, freshly launched at defaults, full window.

## 02-the-main-screen.md

- [x] **`images/two-views.png`** — Hardware view and Editor view  
      Two shots side by side or stacked - Hardware view (LCD lit, showing the play screen) and Editor view (dark theme, defaults). Same window size.
- [x] **`images/bottom-bar-annotated.png`** — The bottom bar  
      Editor view, dark theme, full-width crop of the bottom strip, annotated: File | Options | About | PANIC | undo - HISTORY - redo | path*
- [x] **`images/editor-modules.png`** — The editor's modules  
      Editor view, dark theme, full window, with each module outlined and labeled: System, Multi EQ, Graphic Display, Effect, Controller, Effect Parameters, Part, Voice, Keyboard.

## 03-controls.md

- [x] **`images/control-types.png`** — The editor's control types  
      Editor view, dark theme. A composite or annotated crop showing one XgKnob, one standalone ValueBox, one XgSelector (Effect Program box), a row of DotToggle LEDs, and the controller wheel. Annotate each with its name.
- [x] **`images/knob.png`** — A knob  
      Editor view, dark theme, close crop of one Part-module knob showing the label, the value box above the dial, and the dial's pointer dot.
- [x] **`images/history.png`** — The HISTORY window  
      Editor view, dark theme, HISTORY open with ~8 entries, the current one green and two dimmed redo entries below it.

## 04-system-and-multi-eq.md

- [x] **`images/system-module.png`** — The System module  
      Editor view, dark theme, crop of the System module showing all five knobs AND the HPF box in the title row (set it to a value, not Off, so the box reads as live). VST build so TEMPO shows dimmed. Re-shot 2026-09-07 for the HPF box added 2026-09-05.
- [x] **`images/multi-eq-module.png`** — The Multi EQ module  
      Editor view, dark theme, crop of the Multi EQ module with a non-FLAT program selected so the knobs show non-default values.

## 05-the-graphic-display.md

- [x] **`images/graphic-display.png`** — The Graphic Display in VOLUME mode  
      Editor view, dark theme, crop of the Graphic Display in VOLUME mode with a varied set of part levels so the bars are not all equal, and the animated level meters turned OFF so the solid setting bars are visible — the meter form of the same screen is the next capture down.
- [x] **`images/graphic-display-meters.png`** — The VOLUME screen with level meters running  
      Editor view, dark theme, crop of the Graphic Display in VOLUME mode with the meters option on and several parts sounding, so both the setting dashes and the colored meters are visible.

## 06-the-eq-display.md

- [x] **`images/eq-display.png`** — The EQ display  
      Editor view, dark theme, crop of the Graphic Display in EQ mode with a shaped curve (a low-cut plus one boost), a note sounding so the spectrum trace is visible, and one handle hovered to show its readout.

## 07-effects.md

- [x] **`images/effect-module.png`** — The Effect module  
      Editor view, dark theme, crop of the Effect module with VARIATION selected so CON, PART and CTRL are all live, and the routing strip along the top showing at least one assigned part rather than three OFFs.

## 08-effect-parameters.md

- [x] **`images/effect-parameters.png`** — The Effect Parameters module  
      Editor view, dark theme, crop of the Effect Parameters module with a rich effect loaded (e.g. Variation = DELAY LCR) so most knobs are in use and the SYNC toggle is visible.
- [x] **`images/effect-program-list.png`** — The Variation program list  
      Editor view, dark theme, the EFFECT PROGRAM popup open on the Variation unit, showing all three columns with the current program highlighted.

## 09-controllers.md

- [x] **`images/controller-module.png`** — The Controller module  
      Editor view, dark theme, crop of the Controller module with MODULATION selected and PMOD at a non-default value, plus the wheel.

## 10-the-part-module.md

- [x] **`images/part-module.png`** — The Part module  
      Editor view, dark theme, crop of the Part module in part mode (not drum-edit), a melodic part selected, the receive-switch LED row visible, and a couple of knobs off default so changed tabs show.

## 11-the-voice-module.md

- [x] **`images/voice-module.png`** — The Voice module  
      Editor view, dark theme, crop of the full Voice column, a part selected (highlighted row), at least one drum part showing its buffer color outline, the M and S columns visible (unlit), and the footer readout visible. At least one row should carry the yellow changed tab and one the cyan override tab beside the part number, since both are part of the row's anatomy.
- [x] **`images/mute-solo.png`** — Part 3 muted, part 5 soloed  
      Editor view, dark theme, crop of the top twelve rows of the Voice column with part 3's M cell lit cyan and part 5's S cell lit red.
- [x] **`images/voice-browser.png`** — The Voice List  
      Editor view, dark theme, Voice List open, Melody selected in column 1, a category in column 2, and a voice list in column 3. Include the DRUMS1-4 buttons and the legend.
- [x] **`images/voice-browser-drums.png`** — The keyboard on the DRUM BUFFER box  
      Editor view, dark theme, Voice List open with Drum Kit selected in column 1 and the kit list in column 2, the keyboard on the DRUM BUFFER box with DRUMS3 framed and not yet armed. Same crop as voice-browser.png.

## 12-the-keyboard.md

- [x] **`images/keyboard-module.png`** — The Keyboard module  
      Editor view, dark theme, crop of the Keyboard module with a note range set (red bar over roughly C1-B3), the velocity boxes visible, and the SYS/FX/BULK lamps at the right.
- [x] **`images/drum-marker-strip.png`** — The drum marker strip  
      Keyboard module, dark theme, close crop of the marker strip and the keys immediately below it, with several yellow tabs in the upper row, at least two cyan tabs in the lower row, one key showing both, and the red drum-key marker and its stem on a key that carries a mark.

## 13-drum-editing.md

- [x] **`images/drum-edit.png`** — Drum-edit mode  
      Editor view, dark theme. Part module in drum-edit mode showing the "Drum Key" title and the retargeted knob row (K.ON/K.OFF present, RATE/ DEPTH/DELAY inert, receive LED row grayed), plus the Keyboard module with a key selected and the drum name box filled.

## 14-menus.md

- [x] **`images/bottom-bar.png`** — The bottom bar  
      Editor view, dark theme, full-width crop of the bottom strip showing File, Options, About, PANIC, the undo trio, and a loaded file path with a trailing asterisk.
- [x] **`images/mute-solo-badge.png`** — The mute / solo badge  
      Editor view, dark theme, crop of the right-hand end of the bottom bar with one part soloed and one muted, so the badge reads SOLO 05 MUTE 03.

## 15-options-and-settings.md

- [x] **`images/display-animation.png`** — The Display Animation window  
      Options > Display Animation... window, all twelve rows visible.

## 16-files-and-formats.md

- [x] **`images/drum-buffer-chooser.png`** — The drum buffer chooser  
      Editor view, dark theme, DrumBufferChooser open over the editor, showing DRUMS1-4 with color swatches and part lists, plus the "Buffer File Was Saved With" row.

## 17-the-hardware-view.md

- [x] **`images/hardware-view.png`** — The Hardware view  
      Hardware view, LCD showing an edit page (not the plain play screen), one of the mode LEDs lit. Full window.

## 19-standalone.md

- [x] **`images/midi-input.png`** — The MIDI Input window  
      Standalone, Options > MIDI Input... window, at least two devices listed, one Active with Port A and Drum key ticked.

## 20-workflows.md

- [x] **`images/midi-overrides.png`** — The MIDI override marks  
      Editor view, dark theme, window 1636x1000, crop x85 y530 w1545 h470. Overrides standing on part 1 (CC7, CC10 and CC91 on port A channel 1, so VOL, PAN and REV each show a dial arc at the default mark style, with PAN's growing from the centre) and on part 26 (a program change on port B channel 10, so its voice row shows the tab). The crop reaches the bottom bar, where the count reads 4 MIDI OVERRIDES. Two parts on purpose: overrides are per part, and part 26's row is the one that falls in this crop.
