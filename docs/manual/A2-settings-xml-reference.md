# A2 settings.xml reference

`settings.xml` holds the plugin's **machine-level preferences** — the things that
belong to your installation rather than to a song. It lives at:

```
Documents\MEGAN XG\settings.xml
```

One file is shared by the VST3 and the standalone, and by every instance of the
plugin running at once. It is written whenever you change a setting, and read once
when a plugin instance is created.

> **Watch out** — the plugin writes the whole file when a setting changes, so edit
> it with the plugin **closed** or your changes will be overwritten. Delete the
> file to return every setting to its default; nothing in it is irreplaceable.

Most entries here are also reachable from the **Options** menu and you never need
to touch the file. The exceptions — the five text-size percentages — are
settings-only, and are the reason this appendix exists.

Nothing in this file travels inside a DAW project, and everything in it applies to
every project you open. The two per-part output settings and **Enable DAW
automation** are the other way round — they live in the DAW project only and have
no entry here at all. See
[What is remembered where](#what-is-remembered-where).

## Attributes

All of these are attributes on the root element. Values shown as `0` / `1` are
off / on.

| Attribute | Default | What it does | Where in the UI |
|---|---|---|---|
| `romPath` | *Documents\MEGAN XG* | Folder holding `mu100.zip` | — |
| `defaultEditorView` | `Hardware` | Which view the plugin opens in: `Hardware` or `Editor` | Options ▸ View |
| `editorTheme` | `Dark` | Editor palette: `Dark` or `Light` | Options ▸ View |
| `editorViewW` | `1200` | Remembered editor-view window width | resize the window |
| `editorViewH` | `862` | Remembered editor-view window height | resize the window |
| `tooltipMode` | `1` | `0` pop-ups near cursor, `1` bottom bar, `2` none | Options ▸ Tooltips |
| `focusMarkMode` | `0` | `0` always show the arrow-key mark, `1` only while using the computer keyboard | Options ▸ Show the arrow-key mark |
| `partTitleNames` | `3` | Bit field: `1` name the part's voice in the Part module title, `2` name the drum key and instrument, `3` both, `0` neither | Options ▸ Part module title shows |
| `noteLabels` | `0` | `0` note letters (`C3`), `1` MIDI note numbers (`60`) — everywhere the editor prints a note as text | Options ▸ Label notes as |
| `midiOutFormat` | `0` | What the MIDI output carries for each edit: `0` XG SysEx, `1` MIDI controllers (CC) where a parameter has one, nothing where it does not | Options ▸ MIDI Out sends edits as |
| `automationMarkers` | `0` | `0` shows the DAW triangle, `2` hides it (`1`, a retired underline style, reads as shown) | Options ▸ Mark DAW-automatable parameters |
| `markMidiCcParams` | `1` | Underlines the controls that record as MIDI controllers, while MIDI Out is in MIDI CC mode | Options ▸ Mark parameters that send MIDI CC |
| `reverseDialAction` | `0` | Reverses drag and wheel direction on knobs and value boxes | Options ▸ Invert knob/dial direction |
| `enablePluginOnlyParams` | `1` | Allows HPF2 and EQ bands 2–4 to be edited | Options ▸ Enable plugin-only parameters |
| `showChangedMarks` | `1` | Shows the marks for values that differ from their default | Options ▸ Show changed markers |
| `showMuteSoloColumns` | `1` | Shows the Voice module's M and S cells | Options ▸ Show Mute and Solo columns |
| `showPartRcvSwitches` | `1` | Shows the Part module's 17 receive-switch LEDs | Options ▸ Show part MIDI receive switches |
| `shortVoiceNames` | `0` | `1` shows the hardware's 8-character voice names instead of the full ones | Options ▸ Voice names |
| `useKeyboardShortcuts` | `0` VST, `1` standalone | Turns all the keyboard shortcuts on or off | Options ▸ Use keyboard shortcuts |
| `midiDrumSelectDuringPlayback` | `0` | Lets incoming MIDI pick the drum key while the DAW transport rolls | Options ▸ Enable MIDI drum key selection during DAW playback |
| `midiInputIndicators` | `1` | Per-part activity tabs and the SYS/FX/BULK lamps | Options ▸ Display Animation... |
| `keyboardPlayedNotes` | `1` | Light the selected part's held keys on the keyboard | Options ▸ Display Animation... |
| `eqSpectrum` | `1` | The EQ screen's live spectrum trace | Options ▸ Display Animation... |
| `eqSpectrumRes` | `12` | FFT order — `10`/`11`/`12`/`13` = 1024/2048/4096/8192 points | Options ▸ Display Animation... |
| `eqSpectrumDecay` | `2` | `0` fast, `1` medium, `2` slow, `3` very slow | Options ▸ Display Animation... |
| `volumeMeters` | `1` | Animated level meters on the VOLUME screen | Options ▸ Display Animation... |
| `showMidiOverrides` | `1` | Mark controls the device has been moved off by incoming MIDI | Options ▸ Display Animation... |
| `midiOverrideMarks` | `1` | What those marks look like: `0` horizontal bars, `1` horizontal bars and knob arcs, `2` vertical bars and knob dots | Options ▸ Display Animation... |
| `midiFollowEnabled` | `1` | Watch what incoming MIDI does to the device and mark it | Options ▸ Follow MIDI changes to the device |
| `volumeMeterDecay` | `1` | `0` fast, `1` medium, `2` slow, `3` very slow | Options ▸ Display Animation... |
| `volumeMeterRamp` | `0` | Meter color ramp — `0` Classic, `1` Teal, `2` Ocean, `3` Amber, `4` Mono | Options ▸ Display Animation... |
| `volumeMeterSpread` | `0` | Where the colors start — `0` Standard, `1` Wide, `2` Full | Options ▸ Display Animation... |
| `volumeMeterFill` | `0` | Meter fill — `0` Solid, `1` LED segments | Options ▸ Display Animation... |
| `bufferThreshold` | `2048` | Emulator buffer, in samples (128–32768; clamped on read) | Options ▸ Audio... |
| `pdcTrimIndex` | `0` | `0`–`8` = 0/5/10/15/20/25/30/40/50 ms of delay compensation (VST only) | Options ▸ Audio... |
| `lastSetupDir` | *(empty)* | Folder the last `.mss`/`.mps`/`.mds`/`.mes` came from | — |
| `lastSyxDir` | *(empty)* | Folder of the last `.syx` export | — |
| `lastDumpDir` | *(empty)* | Folder of the last saved dump | — |
| `showTooltips` | — | **Obsolete.** Read once to migrate old settings to `tooltipMode` (on → bottom bar, off → none), never written | — |
| `partOutputs`, `partOutputsRemoveFromMain` | — | **Obsolete.** The per-part output settings are stored in the DAW project instead. An older file may still contain them; they are ignored, and disappear the next time the file is written | — |
| `midiOverridesLocked` | — | **Obsolete**, and inverted: replaced by `midiFollowEnabled`. A file written before the option was restated still carries it, and is read once so the setting is not lost; it disappears the next time the file is written | — |
| `disablePluginOnlyParams` | — | **Obsolete**, and inverted: replaced by `enablePluginOnlyParams`. A file written before the option was restated still carries it, and is read once so the setting is not lost; it disappears the next time the file is written | — |

## Text size — settings-only

These six have no menu item. Each is a percentage applied to one category of
text; `100` is the tuned default, not "unscaled".

| Attribute | Scales |
|---|---|
| `editorTextValuesPct` | Numbers in value boxes and knob readouts |
| `editorTextLabelsPct` | Control labels, and the bottom-bar text |
| `editorTextTitlesPct` | Module titles |
| `editorTextVoiceListPct` | Voice names, in the Voice module and the browser |
| `editorTextGrDisValuePct` | The Graphic Display's own value readouts |
| `editorTextOptionsPct` | The labels in the option windows — Audio, Display Animation, and the standalone's MIDI Input and MIDI Output |

Values are clamped to 50–400, but **that is a safety rail, not a supported
range**. The first five grow the *text* without growing the boxes it sits in, so
past a modest increase something somewhere will start to squeeze or clip.

`editorTextOptionsPct` is the exception, and behaves the way you would want the
others to: the option windows are ordinary dialogs, so their label columns, rows
and window widths are all sized from this percentage too. Raising it opens the
window up rather than crowding it. One thing it does **not** move is the text
beside a checkbox, or the text inside a drop-down: Windows draws those at a fixed
size that the plugin does not set.

> **Watch out** — if you want everything bigger, **make the window bigger**
> instead. The whole editor is drawn on one canvas that scales to the window, so
> resizing grows the text and the boxes together and can never clip. Keep the
> percentages for a small top-up — 110 or 120 — after the window is as large as
> you want it.
>
> The canvas is usually limited by window *height*, so a taller window buys more
> than a wider one.

## Child elements

| Element | Attributes | What it is |
|---|---|---|
| `recentFile` | `path` | One entry in File ▸ Open Recent, newest first, up to five |
| `midiDevice` | `id`, `enabled`, `routeMask`, `drumSelect` | A standalone MIDI input and which port it feeds (`1` = Port A, `2` = Port B, `3` = both). `drumSelect` (default `1`) lets notes from it select drum keys in drum-edit mode |
| `midiOutputDevice` | `id`, `enabled` | A standalone MIDI output the editor's edits are echoed to |
| `shortcut` | `id`, `key`, `enabled` | One keyboard shortcut you have changed |

The `midiDevice` and `midiOutputDevice` entries are written by the standalone's
**Options ▸ MIDI Input...** and **MIDI Output...** windows. The VST ignores them —
in a DAW the host does the routing.

`shortcut` entries appear **only for shortcuts you have changed** in the Keyboard
Shortcut Editor, so a fresh file has none at all. An empty `key` means you left
that one with no shortcut; `enabled="0"` means you unticked it but it has kept its
key. Because only your changes are saved, anything you have not touched picks up
whatever key a later version of MEGAN XG gives it.

## What is *not* in this file

- **Your sound.** Voices, effects, part settings and everything else you edit
  live in the DAW project, or in a `.mss` file you save yourself. See
  [Files and formats](#16-files-and-formats).
- **The two per-part output settings and Enable DAW automation.** Those three live
  in the DAW project only. See
  [What is remembered where](#what-is-remembered-where).
- **Standalone audio device choice and window position.** JUCE keeps those in
  `%APPDATA%\MEGAN XG\MEGAN XG.settings`.
