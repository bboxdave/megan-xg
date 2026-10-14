# Contributing to MEGAN XG

MEGAN XG is a hobby project shared as-is. There is **no promise** of reviews, replies,
fixes or further releases. Issues and pull requests are welcome all the same: an issue
is a searchable record for the next person, and a pull request is something anyone can
build on, here or in a fork. If you want the project to go somewhere it has not, a fork
is a perfectly good way to take it there — the licence exists for exactly that.

To build it first, see [`BUILDING.md`](BUILDING.md). This note is the map you need
next: how the pieces fit, and the handful of rules that are easy to break without
noticing.

## Where things are

| Path | What |
|---|---|
| `Source/` | The plugin: the JUCE processor, the Hardware view and the XGEdit-style editor |
| `mu100editor/codec/` | JUCE-free SysEx codec, address map, voice and drum tables. The plugin compiles these files directly |
| `mu100editor/data/` | The JSON and CSV the codec headers are generated from |
| `mu100editor/tools/` | The Python pipeline that writes `data/` and the generated headers |
| `patches/juce/` | The guarded edits this project needs inside JUCE 8.0.13, plus the script that applies them |
| `docs/manual/` | The user manual (Markdown chapters + a builder + a checker) |
| `PROVENANCE.md` | Where every piece of data came from, and what each third-party component is licensed under |

The emulator itself lives in a separate repository, `mame-mu100`: MAME (a snapshot just after 0.287) with this
project's patches, built as static libraries and linked into the plugin. Changes to the
emulation go there, under MAME's own BSD-3-Clause terms.

## How it fits together

**The emulator runs on its own thread.** MAME runs headless, free-running in real time
on a dedicated high-priority thread. It produces audio into a ring buffer, and the host's
`processBlock()` only *drains* that ring. So a host CPU meter never sees the emulation's
cost, and **offline (faster than real time) rendering is not supported** — the host
collects whatever the ring holds, not the performance.

**Everything reaches the device as MIDI.** Notes from the host, every edit made in the
editor, file loads and DAW automation all become MIDI bytes pushed into a ring that the
emulated MU100's serial port reads. There is no back door into the emulated chip's
parameters; the plugin talks to the device the same way a sequencer would.

**The editor has a model.** `editorState` holds the XG parameter bytes of the current
setup. The editor panels read it, saves and DAW project state store it, and edits write
it as they transmit. Incoming MIDI does *not* change it: it changes the device, and the
editor marks the difference on screen as a "MIDI override".

**Plugin-only features run inside the sound chip.** HPF2, the three peaking EQ bands, the
system high-pass floor, per-part outputs and mute/solo are not MU100 features. Parts are
summed inside the emulated SWP30 chip, so these are applied there, per voice, in
`mame-mu100` — reached through the plain-types bridge `swp30_ext.h`. They have no XG
address, so they live in project state and are deliberately left out of `.syx` exports.

**The Hardware view drives the real firmware.** Its buttons press the emulated module's
front-panel switches and its LCD is the emulated display. Edits made there reach the
device but not the editor's model, which is why switching back to the editor warns.

## Rules that are easy to break

These are the ones that have each cost real debugging time. None of them is enforced by
the compiler.

- **Never dereference the MAME machine bare from another thread.** Take a `MameLease`
  (`PluginProcessor.h`). The machine is a stack object on the MAME thread and can vanish
  under you.
- **A lease is not thread safety.** Never call `running_machine::time()` or similar from
  the message or audio thread — use `senderMameTime()`, which reads a value the MAME
  thread publishes. Assume every MAME internal is single-threaded.
- **Nothing touches a device before the machine is RUNNING.** MAME calls the OSD's
  `update()` during startup, before devices are reset; anything that reaches a device
  from there must be behind the phase gate already in it.
- **The MIDI rings have several writers.** Push a whole message at once with
  `pushMidiMessageToPort()` or `pushSysExStream()`, never byte by byte, or a note can land
  inside a SysEx message. Never put a mutex on them: the audio thread is one of the
  writers.
- **Edits go through the funnel.** Send parameter changes with `sendParamChange()` or,
  for a continuous control like a knob drag, `sendParamChangeThrottled()`. Undo capture,
  rate limiting, the model update and the DAW automation mirror all happen there; a
  control that bypasses it silently loses all four.
- **A whole block goes out as bulk dumps, never parameter changes.** The firmware applies
  only the first byte of a multi-byte parameter change, so sending a block that way
  silently drops most of it. Use `DeviceState::toBulkDumps()`.
- **Writing an effect TYPE loads that type's defaults on the device.** Code that loads a
  file or applies incoming data must do the same to the model (`loadModelLikeDevice`), or
  the editor shows the old type's values under the new type's name.
- **DAW automation parameter IDs are frozen.** Hosts key saved automation on them.
  Renaming one orphans every recorded lane, so add new parameters; never rename old ones.
- **settings.xml is merged on save, not rewritten.** Several plugin instances share it.
  Each Option is either a machine setting (settings.xml) or part of the song (project
  state) — pick one store per Option, never both.

## Generated files

Several codec headers are generated. **Edit the source, then regenerate — never edit the
output,** or the next run silently undoes your change.

| Generated | From | Tool |
|---|---|---|
| `codec/MU100Tooltips.h` | `data/tooltips.csv` | `tools/gen_tooltips.py` |
| `codec/MU100Voices.h` | `data/voices.json` | `tools/gen_voices.py` |
| `codec/MU100Drums.h` | `data/drums.json` | `tools/gen_drums.py` |
| `codec/MU100DrumDefaults.h` | `data/drum_defaults.json` | `tools/gen_drum_defaults.py` |
| `codec/MU100EffectConv.h` | `data/effect_conv.json` + `data/tables.json` | `tools/gen_effect_conv.py` |
| `codec/MU100AddressMapData.h` | `data/parameters.json` | `tools/gen_address_map.py` |

The `extract_*` tools and a few others read Yamaha's manuals from
`mu100editor/reference/`, which is **not distributed** (see `PROVENANCE.md`). Their JSON
output is committed, so you can regenerate any header above without the manuals. What you
cannot do without them is re-extract the JSON from the PDFs — `data/parameters.json` in
particular is rebuilt wholesale by `extract_parameters.py`, so a device fact the manual
does not state belongs in that script's `OVERRIDES` table, never in the JSON.

Every generator writes the licence header from `tools/spdx_header.py`. A new generator
must do the same; a new hand-written file needs the two-line header added by hand.

## The JUCE patches

`patches/juce/` makes small edits inside JUCE: two named MIDI input ports, runtime output
bus names, a per-host MIDI CC decode, and changes to the standalone window. JUCE cannot
express any of them through its public API. Each sits behind a `JucePlugin_MU100_*` macro
that only this project defines. `patches/juce/README.md` explains each one, the expected
guard counts, and how to regenerate a patch — **always against a genuine JUCE download,
never a reconstruction.**

## Working on the editor

- **Colours are tokens.** Every colour lives in `edcol` (`EditorColours.h`) with a value
  for the dark AND the light theme. A hex literal at a call site is a pixel the other
  theme will never reach.
- **Text sizes scale; cells do not.** `edtext` lets users enlarge text, so any box
  holding text must measure it from the real font rather than use fixed pixels, or it
  clips at larger sizes.
- **Keyboard and screen readers.** Controls are reached by the arrow-key cursor
  (`KeyNav.h`) and described to screen readers (`Accessibility.h`). A new control class
  needs both; a control drawn by its parent panel needs a stop in that panel's
  `navItems()`. The accessibility work has **never been tested with a screen reader** —
  that test would be a valuable contribution in its own right.
- **Tooltips live in `mu100editor/data/tooltips.csv`**, not in panel code. The same text
  is the control's accessible name.
- **Rebindable shortcuts are stored by their string id.** Never renumber or rename an id
  in `kbsc` (`KeyboardShortcuts.h`) — that drops users' custom bindings.

## Diagnostics

`Documents\MEGAN XG\enginelog.txt` is written on every run and is the first thing to read
in any bug report — the manual's appendix on the engine log explains it. A healthy boot
logs `machine RUNNING`; the editor draws itself whether or not the emulator started, so a
screenshot proves nothing about that.

Extra logging is switched on by environment variables and costs nothing when unset:

| Variable | What it logs |
|---|---|
| `MU100_MIDIDIAG` | The MIDI delivery clock, once a second |
| `MU100_UNDO_TRACE` | Every undo push, merge, seal and apply |
| `MU100_PARTOUT_TRACE` | Per-part output bus levels, once a second |
| `MU100_A11Y_SPEAK=<file>` | Screen-reader announcements, to a file instead of speech |
| `MU100_DUMP_PARAMS` | The DAW parameter list, to `%TEMP%\mu100-params.txt` — re-run after any change to it |
| `MU100_FORCE_PATCH_PUSH` | Forces the project-reopen patch push, a path that is otherwise rare |

The emulator has its own `SWP30_*` switches for the sound chip and its effects processor;
grep `getenv` in `mame-mu100`'s `swp30.cpp`.

New diagnostics should follow the same pattern: read the variable once into a
function-local static, and do nothing at all when it is unset.

## The manual

Edit the chapters in `docs/manual/`, then:

```powershell
cd docs\manual
python build_manual.py
python tools\check_manual.py
```

The checker must pass. House style: user-facing text is **American English**; say
"computer keyboard" whenever "keyboard" could mean the on-screen piano or a MIDI keyboard;
write menu items exactly as they appear on screen (`...`, not `…`). Screenshots are taken
with the tools in `docs/manual/tools/` — read their README first, and capture with every
setting at its default.

## Before you open a pull request

- Build **Release x64** and confirm the standalone boots to `machine RUNNING`.
- After changing a header that alters a class layout, do a full `/t:Rebuild`: a stale
  object file shows up as a crash with a different error code on every run.
- Run the codec tests if you touched `mu100editor/codec/`:
  `powershell -ExecutionPolicy Bypass -File mu100editor\codec\test\build.ps1`. Tests that
  need the undistributed SysEx fixtures report `SKIP`; the rest must pass.
- Say which host you tested in, if any. Only Cubase and Bitwig have been tested so far,
  and hosts differ in ways that matter here (focus handling, MIDI CC encoding, output bus
  naming).
- Enable the commit guard once per clone — `git config core.hooksPath .githooks` — which
  refuses to commit ROMs and the reference material.

## Licence

MEGAN XG is under the **GNU Affero General Public License v3.0 or later**; the manual is
under **CC BY-SA 4.0**. There is no contributor licence agreement: your contribution stays
yours, under the same licence. Patches to `mame-mu100` stay **BSD-3-Clause**, matching the
MAME files they modify.
