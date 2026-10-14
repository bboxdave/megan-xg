# Provenance

Where everything in this repository came from.

This project is an independent, unofficial software emulation of a 1990s Yamaha XG
tone generator, together with a parameter editor for it. It is not affiliated with,
authorised by, or endorsed by Yamaha Corporation, nor by the authors of XGEdit95.
Yamaha, XG and MU100 are trademarks of their respective owners and are used here
descriptively — to say which device this software interoperates with — never as the
identity of this project.

Everything below is a statement of fact about this repository's contents, written so
that a reader can check each claim against the tree. **It is not legal advice.**

---

## 1. The interoperability position, stated plainly

The MU100 is driven entirely over MIDI system-exclusive messages. A program that
speaks XG SysEx must use **the same parameter names, the same addresses, the same
byte sizes and the same value ranges** as the specification, because those *are* the
interface. Using different ones would not be a different expression of the same idea;
it would be a program that does not work.

That is the whole of what this project takes from the specification. Concretely, and
verifiably:

- **`mu100editor/data/parameters.json`** holds 711 parameter rows across 16 address
  tables. Every row carries exactly: `name`, `addr`, `size`, `range`, `display_raw`,
  `default_raw`, plus internal bookkeeping (`review`, `default_src`, `enc`). **There
  is no description field and no descriptive prose.** The longest parameter name is
  33 characters (`HIGH PASS FILTER CUTOFF FREQUENCY`); of the 711 `display_raw`
  values, 251 are empty and only 8 exceed 45 characters — those eight being
  value-range notation with nibble-packing detail (`-102.4...0...+102.3 [cent] 1st
  bit3-0→bit15-12 …`), which is encoding data, not commentary.
- **`mu100editor/codec/MU100AddressMapData.h`** is generated from that JSON. Its
  quoted strings are parameter names, address-table names, unit suffixes (`" Hz"`,
  `" dB"`, `" cent"`) and the value-range notation above. No explanatory text from
  the specification is reproduced anywhere in this repository.

**Re-check this section if the extraction pipeline changes.** It is true as written,
and its value comes entirely from being checkable.

## 2. What was reverse-engineered, and how

The emulation's behaviour was determined by observation and by reading disassembled
firmware running under emulation — not from any Yamaha source code, which was never
available to this project.

Work of this kind that is documented in the commit history includes: the sound chip's
voice→part association (read from the firmware's own live data structures at note-on),
its filter topology, its DPCM decoder behaviour, its mixer output mapping, and the
effect-processor programs. Where the emulation departs from the hardware it does so
deliberately and says so in the code.

**Some parts are approximate.** This is an emulation built without access to the
device's design documents, and it is not represented as bit-exact.

## 3. What is generated, and from what

The editor's data is not hand-maintained. It is extracted from published Yamaha
documentation by the tools in `mu100editor/tools/`, and compiled into C++ headers:

| Generated file | Generator | Source |
|---|---|---|
| `data/parameters.json` | `extract_parameters.py` | Yamaha MU100 Sound List & MIDI Data book (PDF), read by page coordinates |
| `codec/MU100AddressMapData.h` | `gen_address_map.py` | `data/parameters.json` |
| `data/drums.json` | `extract_drums.py` | the same Yamaha PDF (XG Drum Map pages) |
| `codec/MU100Drums.h` | `gen_drums.py` | `data/drums.json` |
| `data/short_names.json` | `extract_short_names.py` | the same Yamaha PDF (XG Voice List pages) for the mapping; the MU100 program ROM for the strings themselves — see §5 |
| `data/voices.json` | `build_voices.py` | XGEdit95's `XGVoice.ini` for melodic/SFX names; **`data/drums.json`, i.e. Yamaha's own PDF, for the 37 kit names** — see §5; **`data/short_names.json` for every voice's 8-character `short`** |
| `codec/MU100Voices.h` | `gen_voices.py` | `data/voices.json` |
| `data/effects.json`, `data/tables.json` | `extract_effects.py`, `extract_tables.py` | the same Yamaha PDF |
| `codec/MU100Tooltips.h` | `gen_tooltips.py` | `data/tooltips.csv` — see §4 |
| `codec/MU100FactoryDefault.h` | `gen_factory_default.py` | a factory-state SysEx dump — see §6 |

**The public repository ships the generated data but cannot regenerate it.** The
source documents — Yamaha's owner's manual and Sound List/MIDI Data book, XGEdit95's
manual and `XGVoice.ini`, and the factory dump — are third-party works and are
deliberately **not** redistributed here. To re-run the pipeline you must supply your
own copies. That is a consequence of not republishing other people's documents, and
it is the correct trade.

## 4. What was rewritten

The editor's hover tooltips were originally mined from XGEdit95's own string
resources. `mu100editor/data/tooltips.csv` holds 242 rows, 156 of which came from
that source.

Those 156 were reviewed individually, not swept:

- **20 carried authored phrasing and were rewritten** in this project's own words.
- **The remaining 136 are two-to-four-word functional labels** — "Part Pan", "Drum Low
  Pass Filter Resonance" — and were deliberately left alone. There is no other
  sensible wording for them, and they stand on the same functionally-dictated ground
  as the parameter names in §1.

`mu100editor/data/tooltip-review.csv` is the decision record. The `xg_id` column is
**deliberately retained** on rewritten rows: it records what was referenced, which is
the point of keeping provenance at all.

The user manual in `docs/manual/` is original writing. It names XGEdit95 throughout,
descriptively, because a comparison to the program most readers will have used is the
clearest way to explain this one.

## 5. Voice names — the one item derived from another program

`mu100editor/data/voices.json` and the generated `codec/MU100Voices.h` list 1260
selectable voices and kits, each with two display names (the plugin's Options ▸ Voice
names switches between them). The underlying facts — which voices exist, and at which
program number and bank MSB/LSB — are Yamaha's, functionally dictated, and the same in
any XG editor. The **display names** are not all from the same place, and this section
says which are which, because that distinction is easy to lose:

- **The 37 drum and SFX kit names (bank MSB 126/127) come from Yamaha's own Sound List
  PDF**, by way of `data/drums.json`. (Before 2026-08-21 they came from XGEdit; 33 of
  the 37 were identical either way.)
- **The melodic and SFX voice names are XGEdit95's expanded forms**, read from its
  `XGVoice.ini`. This was measured rather than assumed: Yamaha's published sound list
  gives only the device's own **8-character LCD abbreviations** (`GrandPno`,
  `MelloGrP`, `BrConGrK`), and of the 1095 entries the two sources share, **773 names
  differ**. The expanded forms (`Grand Piano`, `Mellow Grand Piano`) appear nowhere in
  Yamaha's documentation — they are XGEdit's editorial expansion, down to
  `Brght Cncert GrndKSP`, which is a truncation to XGEdit's own list-widget width.
  There is therefore no Yamaha specification to re-derive them from.
- **The 8-character `short` name beside every full one is Yamaha's, not XGEdit's.**
  These are the abbreviations the device's own LCD prints, and `XGVoice.ini` does not
  carry them at all. The mapping (which abbreviation belongs to which bank and
  program) is read out of the Sound List PDF's XG Voice List grid; the strings are
  verified byte-for-byte against the name fields in the MU100's program ROM, which is
  also the tie-breaker where the printed list and the shipped firmware disagree (one
  name, `0/40/74`). The kit abbreviations come from a name table in the same ROM.
  So the short form of a voice name is derived only from Yamaha's own documentation
  and firmware — none of it from another program.
- The per-voice `model` (XG / MU80 / MU90 / MU100 addition) and `elements` (1 or 2)
  fields likewise come from `XGVoice.ini`. Both are recoverable from markers in the
  Yamaha PDF, and that was tried: it reproduces 1074 of 1095 models and 1087 of 1095
  element counts, with the residual differences traced to markers the PDF itself
  omits. The XGEdit values were kept as the more reliable of the two.

We are grateful to the authors of XGEdit95, whose editor is also the layout
inspiration for this one. They are credited by name in the README.

## 6. `codec/MU100FactoryDefault.h`

This is the MU100's **power-on state**, captured as a SysEx byte array (about 159 KB)
and compiled in, so the plugin has something to seed a default patch with. The native
Performance model (`0x59`) is stripped so the editor's state stays Multi-32 in scope.

It is device register state: factual, functional, containing no creative content, no
waveform data and no firmware. It is included on the same interoperability footing as
§1 — a program that emulates a device has to be able to start in the state that device
starts in. Regenerating it requires a factory dump, which this repository does not
ship.

## 7. What is deliberately absent

- **No ROMs and no firmware.** None, in the tree or in its history. The emulation
  requires ROM images that the user must dump from, or otherwise lawfully obtain for,
  hardware they own; the software will not run without them and does not help you get
  them.
- **No third-party documents.** No Yamaha manuals, no XGEdit manual or binaries, no
  `XGVoice.ini`, no third-party screenshots.
- **No descriptive text from the specification** — see §1.
- **No JUCE and no MAME source.** This repository contains *patches* to JUCE
  (`patches/juce/`), not JUCE itself.

## 8. Original work

The following exist in no XG hardware and are this project's own design and
implementation: the per-part clean high-pass filter (HPF2), the per-part three-band
peaking EQ and its per-drum-key layer, the effect tempo-sync layer, the DAW-automatable
parameter set and its host integration, the per-part stereo output buses, the
undo/redo and history system, the editor's graphic-display screens including the live
spectrum analyser and the level meters, the partial-setup file formats, and the user
manual. So is all of the plugin shell, the MAME-to-plugin bridge, and the emulation
work described in §2.

Values belonging to these plugin-only features are deliberately **excluded** from
`.syx` export, so a dump written by this editor stays byte-compatible with real XG
hardware and with other XG editors.

### Design influences

**No code was taken from any of the following.** Each is a user-interface idea
followed deliberately, recorded here rather than in the About box because that box has
room only for the project's foundational lineage — and because a partial list of
influences is worse than none.

| Element | Follows |
|---|---|
| The editor view's whole layout | **XGEdit95** (Gary Gregson). The project's organising model, and credited in the About box for that reason |
| Accessibility structure — names, roles, spoken values, and the keyboard access underneath them | **joeloftusdev**'s published guide to making JUCE plugins accessible, and the **Surge Synth Team** plugins it cites as models. Guidance only. It is groundwork and has never been screen-reader tested |
| The Keyboard Shortcut Editor window | **Surge XT** (Surge Synth Team). The category headings are this project's own addition |
| The undo glyph (hooked arrow) and the HISTORY step list | **Arturia Pigments** |
| The EQ node editor, and the live spectrum drawn beneath the response curve | **Bitwig** |
| Per-part mute/solo semantics — plain click adds, Ctrl+click is exclusive | **Cubase** |
| Part-first automation parameter names (`01 CUTOFF`) | **Pigments** and **Surge** |

Product names here are used descriptively, to say what an interface follows. See §10.

## 9. Third-party components and their licences

Nothing in this list is redistributed in this **source repository**; each is a
dependency you supply when building. **A binary release is different:** it links
MAME and its bundled libraries statically, so the release zip carries
`THIRD-PARTY-NOTICES.txt`, generated by `packaging/gen_notices.py` from the same
MAME and JUCE trees the binary was built from. That file lists every library on the
link line (expat, zlib, PortMidi, FLAC, LZMA, SoftFloat 3, utf8proc, libjpeg, SQLite,
Zstandard, PortAudio, Lua and four Lua add-ons, AsmJit, bgfx/bimg/bx, ymfm, WDL FFT,
linenoise) and the JUCE dependencies these modules compile (VST3 SDK, ASIO SDK,
jpeglib, libpng, zlib, HarfBuzz, SheenBidi, Ogg Vorbis), with each licence text.

| Component | Licence | Note |
|---|---|---|
| JUCE 8 | AGPLv3 **or** commercial (dual) | Determines this project's own licence — see below |
| MAME | per-file; see below | Provides the hardware emulation |
| Steinberg VST3 SDK | MIT (the interface headers bundled by JUCE) | |
| Steinberg ASIO SDK | proprietary **or** GPLv3 (dual) | Enabled via `JUCE_ASIO`; headers bundled by JUCE |

**JUCE is the binding constraint.** JUCE 8's own `LICENSE.md` offers its modules under
AGPLv3 or under the commercial JUCE 8 licence. The commercial licence's §2.3
specifically forbids putting the framework under any licence that requires source
disclosure, and §1.17 forbids redistributing the framework — so a copyleft release and
the commercial terms are alternatives, not a combination. A free, open-source release
of this project therefore takes the **AGPLv3** branch, which makes this project AGPLv3.
(AGPLv3's §13 network clause has no practical effect on an audio plugin.)

**MAME imposes no copyleft on this project, which is not the obvious answer.** MAME as
a whole is offered under the GPL, but individual files carry less restrictive terms,
and this build links only a subset. Census of the 1047 MAME source files actually
referenced by the build projects:

| Licence | Files |
|---|---|
| BSD-3-Clause | 953 |
| untagged (generated CPU cores, small utilities) | 21 |
| LGPL-2.1+ | 4 |
| Public domain | 2 |
| MIT | 1 |

**No GPL-2.0 file is linked at all.** The four LGPL-2.1+ files are TI-99 disk and
cartridge format code pulled in as dead weight by the shared formats library. The
"or later" on those permits LGPLv3, which is compatible with AGPLv3, as is the GPLv3
branch of the ASIO SDK (AGPLv3 §13 expressly permits combining with GPLv3 works).

The patched MAME tree this plugin links against is maintained separately. **If
compiled binaries are ever distributed, that tree's modified sources must be published
alongside them** to satisfy the terms of the files it contains.

## 10. Trademarks

Yamaha, XG, MU100 and MU are trademarks of Yamaha Corporation. XGEdit is the work of
its own authors. VST is a trademark of Steinberg Media Technologies GmbH. ASIO is a
trademark of Steinberg Media Technologies GmbH. MAME is a registered trademark of
Gregory Ember. All are used descriptively. No endorsement is claimed or implied.
