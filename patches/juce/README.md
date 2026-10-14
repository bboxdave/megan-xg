# MU100 guarded patches to the shared JUCE tree

The MU100 plugin makes a few small edits to the **shared** JUCE source tree
(`libs/JUCE`, which other projects may share). Both edits are wrapped
in project-unique preprocessor macros that are defined **only** in
`MEGAN XG.jucer` (`extraDefs`), so SQ-1 / MU128 get stock JUCE behaviour — the guards
make cross-project breakage impossible.

**"Stock behaviour", not "stock source".** A few spots in the VST3 wrapper are
restructured OUTSIDE a guard so that both branches can share them, with an `#else` that
reproduces JUCE's own code exactly: `getMidiControllerForParameter` gains a defaulted
`int* busOut = nullptr`, `isMidiControllerParamID` computes its span into a local, and
`addParameterChangeToMidiBuffer` writes through a `dest` reference that is `midiBuffer`
when the guard is off. With the macro undefined each of these does what stock JUCE does;
the source text simply is not byte-identical to stock. Plus two banner comments at the
top of the file.

Because the patches live in a vendored tree, **a JUCE upgrade replaces the whole tree
and silently wipes them.** These patch files + the apply script are the durable,
off-site-backed-up record so they can be re-applied after any upgrade.

## The patches

There are **two patch files**, one per target file. The standalone one carries three
separate edits under two guard macros.

| Patch file | Target (under `libs/JUCE`) | Guard macro | What it does |
|---|---|---|---|
| `juce_audio_plugin_client_VST3.cpp.patch` | `modules/juce_audio_plugin_client/juce_audio_plugin_client_VST3.cpp` | `JucePlugin_MU100_DualMidiIn` | Exposes two named VST3 event-input buses (MU100-A / MU100-B); routes bus 1 to the processor via `MidiBusSink`. Also gives bus 1 its **own** `IMidiMapping` CC/pitch-bend/after-touch parameter block so those channel-voice messages reach Port B (they travel as parameters, not events, and JUCE's stock single block is bus-blind → they'd otherwise all land on Port A). Also decodes those CC parameter values per HOST (`mu100HostCcToMidi`): Cubase/Nuendo encode a 7-bit lane value differently from `L/127`, so JUCE's `floor(v*128)` delivered most values one step high there -- see "The Cubase CC decode" below. 13 guarded `#if` sites. |
| `juce_StandaloneFilterWindow.h.patch` | `modules/juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h` | `JucePlugin_MU100_HideStandaloneOptions` | Hides JUCE's built-in standalone top-bar "Options" button (settings are consolidated into the plugin GUI's own "Options" menu). 1 guarded site. |
| *(same file)* | *(same file)* | `JucePlugin_MU100_NoStandaloneStatePersist` | Stops the standalone saving/restoring the plugin's setup via JUCE's `filterState` property, and asks before closing with unsaved changes. 3 guarded sites (an `#include` of `Source/StandaloneCloseGuard.h`, the `savePluginState`/`reloadPluginState` pair, and `closeButtonPressed`). See "Why the standalone does not persist its setup" below. |
| *(same file)* | *(same VST3 file)* | `JucePlugin_MU100_DynamicBusNames` | Lets the processor name output buses at runtime (per-part outputs named after the assigned voice) and tell the host to re-read them. 3 guarded `#if` sites (an `#include` of `Source/BusNameProvider.h`; `getBusInfo`, which asks the processor before falling back to `bus->getName()`; and `audioProcessorChanged`, which ORs in `Vst::kIoTitlesChanged` when the processor raises its dirty flag). |

**`juce_audio_plugin_client_VST3.cpp.patch` carries BOTH VST3 macros.** The apply script
keys its already-applied test on `JucePlugin_MU100_DualMidiIn` alone, which is correct for
the case that matters (an upgrade wipes both together) but would skip the file if a tree
somehow had one macro and not the other.

### Exact guard counts (measured 2026-08-04, DualMidiIn re-measured 2026-10-07 — use these to verify a re-application)
The previously documented "12 sites" for the dual-MIDI patch was never accurate; these are
counted from the live tree and reproduced exactly by applying the patch to pristine.

| Macro | File | `#if` sites | total token occurrences |
|---|---|---|---|
| `JucePlugin_MU100_DualMidiIn` | VST3 | 13 | 15 |
| `JucePlugin_MU100_DynamicBusNames` | VST3 | 3 | 4 |
| `JucePlugin_MU100_HideStandaloneOptions` | Standalone | 0 (it is an `#ifndef`-style negation) | 1 |
| `JucePlugin_MU100_NoStandaloneStatePersist` | Standalone | 3 | 3 |

Total occurrences exceed `#if` sites because each macro is also named in the explanatory
comment above its block.

The macros are added in `MEGAN XG.jucer`:
```
extraDefs="ASIO_STANDALONE=1
           JucePlugin_MU100_DualMidiIn=1
           JucePlugin_MU100_HideStandaloneOptions=1
           JucePlugin_MU100_NoStandaloneStatePersist=1
           JucePlugin_MU100_DynamicBusNames=1"
```

## The Cubase CC decode (2026-10-07)
VST3 delivers MIDI CC to an instrument as **parameter changes** through `IMidiMapping`,
normalised 0..1, and the wrapper turns them back into 0..127. JUCE uses
`floor(v * 128)`, which is exact for a host that sends `L/127` -- **Bitwig does
(measured)**. **Cubase does not**, and uses a different encoding per kind of lane
(measured with a probe logging the raw value, every value typed into the lane):

| lane kind | example | Cubase sends |
|---|---|---|
| unipolar | Volume (CC7) | `v*127 = L+1` for L <= 63, `64 + (L-63)*63/64` for L >= 63 |
| bipolar-display | Pan (CC10), and Reverb send (CC91) measured the same way | raw r in 14 bits: `r*130` (r <= 63), `(r+1)*128` (r >= 64), over 16383 |

Under `floor(v*128)` Volume arrived one step high for every value but 127 and Pan one
step high from C rightwards -- the device played off the lane, and the MIDI override
mark never cleared at equality (the user-visible report).

- **One decode serves both Cubase encodings**: `floor(x - 0.75*min(x,64)/64)`,
  `x = v*128`. Any bias peak from 0.504 to 0.988 decodes every value of both full
  128-value models; 0.75 is the middle. The tightest margin is the bottom end (Pan L63 vs
  Volume lane 0, 0.008 of a step apart), far above float error.
- **It is host-gated (`isCubase() || isNuendo()`) because no host-agnostic rule exists**:
  Cubase lane 63 and Bitwig lane 64 send the identical value, 0.503937.
- **Declaring the CC parameters discrete (stepCount 127) does NOT change what Cubase
  sends** -- tried, bit-identical values, reverted. Do not retry it.
- Controllers only. Channel pressure and pitch bend keep JUCE's conversion: unmeasured.
- **A Cubase update could change its encoding.** The symptom would be the same report:
  the override mark not clearing at the lane's value. Re-measure by logging the raw
  `value` in `addParameterChangeToMidiBuffer` before changing the decode.
- MU128 and SQ-1 compile the stock wrapper (the guard is MU100-only), so they still
  carry the off-by-one in Cubase.

## The patches did NOT apply before 2026-08-04 — `--ignore-whitespace` is load-bearing

Found while regenerating the VST3 patch for the second macro, and it invalidated the
recovery procedure this whole directory exists for: **`git apply` without
`--ignore-whitespace` rejected BOTH patches outright** ("patch does not apply" at the
first hunk). The patch files are stored **LF** (`.gitattributes`) while the JUCE tree on
this machine is **CRLF**, so every context line differed by a trailing CR.

It had never been noticed because the patches were generated *from* the already-patched
files and the script's idempotency check then reports `[SKIP] already applied` — the
failing path only runs on a tree that has actually been wiped, i.e. exactly once, after a
JUCE upgrade, when it would have been most expensive to discover.

`apply-juce-patches.ps1` now passes `--ignore-whitespace`. Verified: without it neither
patch applies to the CRLF tree; with it both do.

**Keep the patches LF.** Storing them CRLF would also work against *this* tree, but LF +
`--ignore-whitespace` is the combination that works whether the upgraded tree arrives LF
or CRLF, which is the case that cannot be predicted.

### Known cosmetic consequence
Applying an LF patch to a CRLF tree gives the **added** lines LF terminators, so a
re-patched file ends up mixed. Harmless to the compiler, and the round-trip below is
verified **content-identical** (byte-identical once line endings are normalised) rather
than byte-identical — the earlier "byte-for-byte" claim was only ever true of the
generation step, not of re-application.

## Why the dynamic-bus-name patch is needed at all
JUCE fixes a bus name at construction — `AudioProcessor::Bus::name` has no public setter —
and `AudioProcessorListener::ChangeDetails` carries only `latencyChanged`,
`parameterInfoChanged`, `programChanged` and `nonParameterStateChanged`, so
`audioProcessorChanged()` can never emit `Vst::kIoTitlesChanged` ("bus title has changed,
re-query `getBusInfo`"). The constant is in the VST3 SDK and Cubase honours it — Groove
Agent SE renames an **already-created** output track when a drum pad is assigned to an
output — so this is purely a gap in JUCE's exposure, not a host or format limitation.

The wrapper reads the name live on every `getBusInfo` call and caches nothing, which is
what makes a runtime name change work at all.

## Why the standalone does not persist its setup
It used to persist it *partially*, which is worse than not at all. JUCE writes
`getStateInformation()` into the `filterState` property on close and feeds it back on
launch; inside that blob is the 128 KB MU100 work-RAM snapshot, which restores the
**emulated device** — so the previous session's voice and effects were audible — while the
**editor's own model** came up at factory defaults. The panels said Grand Piano with
Delay LCR and you heard a violin through a phaser.

Opening at the factory defaults every time is the intended behaviour now. Machine
preferences are unaffected: they live in the plugin's own `settings.xml`, and JUCE's
`audioSetup`, `shouldMuteInput` and window position stay in the same `.settings` file,
untouched. **VST3 project state is untouched too** — a DAW still saves and restores the
full setup, because that state belongs to the song rather than to the application.

`reloadPluginState()` also *removes* the `filterState` key rather than ignoring it, so a
settings file written by an older build sheds its ~200 KB of dead base64 on first launch.

Because nothing is written on close, unsaved edits are genuinely lost, so
`closeButtonPressed()` routes through `mu100::confirmStandaloneQuit()`
(`Source/StandaloneCloseGuard.h`, implemented in `PluginEditor.cpp`) for the same
unsaved-changes prompt File > New and File > Open already use. It returns `true` when it
has taken the quit over, and the modal callback quits only if the user confirms.

**Note the diagnosis this replaced was wrong, and why.** An earlier session concluded the
standalone "never persists at all — `filterState` is an empty blob". It had read
`%APPDATA%\Yamaha MU100\Yamaha MU100.settings`, which is a **stale pre-rename file** last
written 2026-07-16. `JucePlugin_Name` is `MU100`, so the live file is
`%APPDATA%\MU100\MU100.settings` — and its `filterState` was ~217 KB and current. Check
the name JUCE actually uses before concluding a property is not being written.

## Re-applying after a JUCE upgrade

```powershell
# from the repo root
powershell -File patches\juce\apply-juce-patches.ps1            # apply any missing patches
powershell -File patches\juce\apply-juce-patches.ps1 -Verify    # just report applied / not applied
```

The script is **idempotent** — it skips a patch whose guard macro is already present in
the target file, so it is safe to run repeatedly. It uses `git apply` (which works on
plain files even though `libs/JUCE` is not a git repo).

### If `git apply` rejects a hunk
A large JUCE upgrade can move the surrounding code enough that the unified-diff context
no longer matches. The script will say `[FAIL]` and tell you which patch. Re-apply that
patch by hand: open the `.patch` file, and insert each `+`-prefixed hunk at the matching
anchor in the new JUCE source. Every change is greppable by its guard macro, so you can
confirm completeness with e.g.
`grep -rn JucePlugin_MU100_DualMidiIn <target>` — see the guard-count table above for the
exact numbers to expect.

**Rule out the line-ending failure first.** A `[FAIL]` whose error names the *first* hunk
(`patch failed: ...:116`) is far more likely to be the CRLF/LF mismatch above than moved
code — confirm by re-running the same `git apply` by hand with `--ignore-whitespace`
before starting any manual re-application.

## How these patch files were generated
**Since 2026-10-13 they are plain `git diff`s against a GENUINE JUCE 8.0.13 download**
(`https://github.com/juce-framework/JUCE/archive/refs/tags/8.0.13.zip`) — not against a
reconstruction. Verified: both apply to that download as unpacked AND to a CRLF copy of
it; a second run skips both; the patched files are byte-identical to `libs/JUCE` once line
endings are normalised; the token counts match the table above.

**Why the change, and the reason this check exists.** Until then the baseline was
reconstructed *from the patched files themselves* and verified by round-trip against that
reconstruction — which proves the patch is self-consistent and nothing else. Run against
the real download, the VST3 patch **failed** at the `getBusInfo` hunk (`:3099`): the
reconstruction had a blank line between the `jassert` and `toString128 (info.name, ...)`
that genuine 8.0.13 does not have. A clean-room builder would have hit that on the first
step. **Regenerate against a real download, never a reconstruction:**
1. Unpack the tag's zip; `git init` a scratch tree with `core.autocrlf false`; copy the
   pristine target file in (LF), commit.
2. Copy the patched `libs/JUCE` file over it with CRs stripped; `git diff` > the patch.
3. Run `apply-juce-patches.ps1 -JucePath <fresh unpack>` on a NEW unpack, then on a
   CRLF copy; compare each result to `libs/JUCE` with line endings normalised.

Keep the guarded edits **ASCII-only** and capture `git diff` output as UTF-8 **bytes**
(not a text pipe) — on Windows a text pipe decodes git's UTF-8 as cp1252 and mangles any
non-ASCII byte elsewhere in the file.

### The 2026-08-04 regeneration, step by step (historical — superseded above)
Adding a second macro to an already-patched file makes plain reverse-application fail (the
new edits sit inside the old patch's context). What worked:
1. Strip **only the new** guarded blocks from the live file by exact string replacement,
   giving the file as it stood before the change.
2. `git apply -R --ignore-whitespace` the **old** patch to that, giving pristine. Confirm
   with `grep -c JucePlugin_MU100` → must be `0`.
3. `git init` a scratch tree, commit pristine, copy the live file over it, `git diff`.
   Set `core.autocrlf false` in that scratch repo or the diff is taken against normalised
   content.
4. Strip CRs from the diff (`.gitattributes` demands LF patches) and install it.
5. Round-trip: `git checkout -- .` back to pristine, `git apply --ignore-whitespace` the
   new patch, and compare against the live file with line endings normalised.
