# MEGAN XG

**Multitimbral Editor, Generator, Audio Nexus for XG** — a software emulation of the
Yamaha MU100 XG tone generator, packaged as a Windows VST3 plugin and standalone
application, with a full parameter editor built in.

The sound engine is the MU100 emulation from [MAME](https://www.mamedev.org/), running
the device's own firmware, so it sounds and responds like the hardware: 64-voice
polyphony, 32 parts across two MIDI inputs, the XG effects, drum kits and voice banks.
Around it, an editor in the spirit of XGEdit95 shows every part, effect, drum key and
envelope on one screen, sends your edits to the device as XG SysEx, and can record them
into your DAW.

- **[User manual](https://bboxdave.github.io/megan-xg/)** — what everything does, with
  screenshots. It is rebuilt from `docs/manual/` on every change to this repository.
- Version **0.9.0**: the first public release. It is not 1.0 because it has been
  tested in two hosts by one person — see below.

## What you need

**The MU100's ROMs, which are not included and never will be.** They are Yamaha's
copyrighted firmware and sample data. Put them, as a MAME-style `mu100.zip`, in
`Documents\MEGAN XG\`. The zip needs **ten** files:

| | |
|---|---|
| firmware | `xu50720.ic11` (v1.11) |
| sample ROMs | `xs518b0.ic34`, `xs743b0.ic35`, `xt445a0-828.ic36`, `xt461a0-829.ic37`, `xt462a0.ic39`, `xt463a0.ic38` |
| device ROMs | `sin-table.bin`, `hd44780u_b04.bin`, `mulcd.svg` |

Every one is required — a missing file stops the engine from starting at all. The
manual's *Getting started* chapter covers this in more detail.

## What is built and tested

- **Built:** Windows x64, **VST3** and **Standalone**.
- **Tested:** in **Cubase** and **Bitwig Studio**. Those are the only two hosts it has
  been tried in, and they should not be taken as representative of every DAW.
- **Not built:** macOS, Linux, AU, AAX, CLAP, LV2. These are not excluded on principle;
  nobody has done the work. The barrier is the platform rather than the format: the
  MAME engine is linked as Windows MSVC static libraries, and the plugin's dual MIDI
  input and per-part output naming are implemented as patches to JUCE's VST3 wrapper,
  which other formats do not go through. Contributions are welcome.
- **Offline (faster-than-real-time) export is not supported.** The emulation runs on
  the wall clock, so a non-real-time bounce comes out as bursts of noise. Export in
  real time.
- **Accessibility:** keyboard control of every editor control, plus screen-reader names,
  values and announcements, are built in — but have **not been tested with a screen
  reader**. Treat them as groundwork.

## No promises

This is a hobby project, provided as-is. There is no undertaking to fix bugs, answer
reports or keep developing it. Reports are still worth filing: they leave a searchable
record for the next person, and they are what make the project practical to fork. The
manual's *Known issues and troubleshooting* chapter explains what to include, and its *Engine log*
appendix explains the log file worth attaching — and what personal details it contains.

## Building from source

Summary only — **[`BUILDING.md`](BUILDING.md) is the step-by-step guide**, with the
traps and a troubleshooting table.

- **Windows x64, Visual Studio 2026** (MSBuild 18, C++ workload). Use the **amd64**
  MSBuild.
- **JUCE 8.0.13, exactly.** `patches/juce/` applies only to that version. Use the
  Projucer that ships *in that JUCE tree*: an older Projucer stamps a version that the
  8.0.13 modules reject with an `#error`.
- **The patched MAME tree**, built as static libraries first:
  [bboxdave/megan-xg-mame](https://github.com/bboxdave/megan-xg-mame), cloned into
  `libs/mame-mu100`.
- **Python 3, callable as `python3`** — the emulator build runs Python scripts, and so
  do the manual and the editor's data pipeline.

Expected layout — the project file refers to its neighbours relatively:

```
<any folder>/
  libs/JUCE/          JUCE 8.0.13
  libs/mame-mu100/    the patched MAME tree, libraries built
  megan-xg/           this repository
```

Order of operations: build the MAME libraries → run
`patches\juce\apply-juce-patches.ps1` against the JUCE tree → resave
`MEGAN XG.jucer` with that tree's Projucer → build
`Builds\VisualStudio2026\MEGAN XG.sln`. The build does not install the plugin; copy
`MEGAN XG.vst3` into `C:\Program Files\Common Files\VST3\` yourself.

**If you contribute**, read [`CONTRIBUTING.md`](CONTRIBUTING.md) — how the pieces fit and the
rules that are easy to break — and enable the repository's commit guard once per clone:

```
git config core.hooksPath .githooks
```

It refuses to commit ROMs and the third-party reference material the editor's data
pipeline reads locally (see `PROVENANCE.md`).

## Licence

MEGAN XG is free software under the **GNU Affero General Public License v3** — see
[`LICENSE`](LICENSE). There is no contributor licence agreement; contributions are
accepted under the same licence. Each source file carries a two-line notice,
`SPDX-License-Identifier: AGPL-3.0-or-later` plus the copyright line; the generated
headers under `mu100editor/codec/` carry it too, emitted by the generators
(`mu100editor/tools/spdx_header.py`) so that regenerating cannot drop it.

The user manual (`docs/manual/`) is licensed separately under
[CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/), the usual choice for
documentation; the AGPL is written for programs and fits prose badly.

AGPLv3 follows from JUCE, whose open-source licence it is. The MAME components keep
their own per-file licences (overwhelmingly BSD-3-Clause), and the patched MAME tree is
published alongside this repository. `PROVENANCE.md` sets out where every part of this
repository came from, what was generated and from what, and the licence of each
third-party component.

## Credits

- **MAMEDev and contributors** — the MU100 emulation, the H8S CPU core and the SWP30
  sound chip emulation this plugin runs on.
- **Sojus Records** — the SD-1/32 MAME-to-JUCE port this project borrowed its plumbing
  from.
- **Gary Gregson** — XGEdit95, whose editor layout the Editor view follows. The views
  were rebuilt from scratch in JUCE; no XGEdit code is used. The full voice names are
  credited to XGEdit's voice list in `PROVENANCE.md`.
- **joeloftusdev** — published guidance on making JUCE plugins accessible, which the
  accessibility work follows. The shortfall in testing is ours.
- **JUCE**, and Steinberg's VST3 and ASIO SDKs as bundled by JUCE.
- Ported, fixed, extended and documented by **Dave Wheitner**, with AI assistance.

## Not affiliated with Yamaha

This is an independent project, created by reverse engineering for interoperability.
It is not affiliated with, authorised or endorsed by Yamaha Corporation, nor by the
authors of XGEdit95. **Yamaha, XG, MU100 and MU are trademarks of Yamaha Corporation**
and are used here only to say which device this software emulates. VST and ASIO are
trademarks of Steinberg Media Technologies GmbH. MAME is a registered trademark of
Gregory Ember.
