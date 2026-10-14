# A3 DAW automation parameters

> **VST only** — these are native host parameters. The standalone has no host to
> automate from and shows no automation markers.

The plugin exposes **4 939 parameters** to the DAW:

- three **master** controls,
- **sixteen per part**, for all 32 parts (512),
- and **fourteen per drum key**, for every key of all four DRUMS buffers (4 424).

These are the ones with their own automation lane, fader assignment and MIDI-learn
slot in your DAW.

> **That is a long list, and it is meant to be browsed by folder rather than
> scrolled.** The drum keys make up nine tenths of it, and they are three folders
> deep — see [How they are named and grouped](#how-they-are-named-and-grouped). If
> your DAW shows one flat list, the master and part parameters are all at the top,
> before the first drum key.

Everything *else* in the editor is still automatable — as XG SysEx recorded onto
a MIDI track. See [Two kinds of automation](#two-kinds-of-automation) for which to
reach for.

> **They can be switched off for a project.** **Options ▸ Enable DAW automation**
> is on by default and is saved with the project. Untick it and every parameter
> below goes inert — your DAW still lists them and will still let you draw a lane,
> because a plugin has to declare its parameters before it knows which project it
> is in, but nothing will happen. The automation markers in the editor disappear
> at the same time, and that is the cue to look for. Existing lanes are ignored,
> not lost.

## How they are named and grouped

Every parameter begins with where it lives — the word `MASTER`, or the part
number — followed by the control's own name:

```
MASTER VOL      MASTER ATT      MASTER KSHIFT
01 VOL          01 PAN          01 CUTOFF       ...
32 VOL          32 PAN          32 CUTOFF       ...
```

A drum key parameter names its **buffer, its key and its control**:

```
DR1 C1 36 VOL       DR1 D1 38 CUTOFF      DR1 F#1 42 DECAY1
DR4 A#1 46 PAN      DR2 C2 48 ATTACK      ...
```

The key appears **twice, as a note name and as a MIDI note number** — `A#2 58`.
That is deliberate. The note name is what you read off the plugin's own keyboard,
but DAWs disagree about which octave number middle C carries, so the same note may
be called `A#2` here and `A#3` or `A#4` in your piano roll. The letter never
disagrees; only the octave digit does. **When the two disagree, the number is the
one to trust** — it is the note number the module itself uses.

> **This is the one place that carries both forms, and it is not affected by
> Options ▸ [Label notes as](#label-notes-as).** That setting switches the *editor*
> between note letters and note numbers; these parameter names are strings your DAW
> stores and caches, so they always say both. If you find yourself translating
> between the editor and this list, set the editor to note numbers and the two will
> read the same.

They are all **grouped into folders**: `MASTER`, one per part (`PART 01` …
`PART 32`), and one per DRUMS buffer (`DRUMS1` … `DRUMS4`) which opens into **one
folder per key**, named the same way — `A#2 58`. A DAW whose parameter picker
understands VST3 groups — Cubase does — shows them collapsed, so reaching a
snare's decay is three clicks rather than a scroll through thousands of entries. A
DAW that ignores groups shows one flat list, which is why the buffer and key are in
the parameter name as well.

Within a part the order follows the Part module's own layout: the knob row from
left to right, then the EQ grid beneath it — HPF2 first, then the five band gains.
Within a drum key it follows the same knob row, then HPF2, then the three AMP EG
rates from the Graphic Display's AMP EG screen.

> **Note** — in a narrow column (a mixer strip, a control-surface display) your
> DAW may show a shorter name instead: `VOL01`, `CUTOFF32`, `KSHIFMST`, and for a
> drum key `D158VOL` — buffer 1, note 58, volume. That is the same parameter under
> its internal identifier, which never changes.

Your DAW may also add the plugin's own name in front, and it may not do so
consistently: Cubase, for one, prefixes a parameter with the plugin name only while
the two together stay short, so `01 CHORUS` appears with the prefix and the longer
`01 EQ1GAIN` appears without it. That is the host's own labelling and it has no
effect on the parameter or on automation you record.

## The master three

| Parameter | Editor control | Range | Shown as | Default |
|---|---|---|---|---|
| `MASTER VOL` | System module **VOL** | 0 – 127 | `0`…`127` | 127 |
| `MASTER ATT` | System module **ATT** (attenuator) | 0 – 127 | `0`…`127` | 0 |
| `MASTER KSHIFT` | System module **KSHIFT** | −24 – +24 semitones | `-24`…`+24` | 0 |

## The per-part sixteen

Each of these appears 32 times, once per part, with the two-digit part number in
front — `01 VOL` through `32 VOL`, `01 CUTOFF` through `32 CUTOFF`, and so on.

| Parameter | Editor control | Range | Shown as | Default |
|---|---|---|---|---|
| `nn VOL` | Part **VOL** (volume) | 0 – 127 | `0`…`127` | 100 |
| `nn KSHIFT` | Part **KSHIFT** (note shift) | −24 – +24 semitones | `-24`…`+24` | 0 |
| `nn PAN` | Part **PAN** | left – center – right | `L63`…`C`…`R63` | C |
| `nn HPF` | Part **HPF** (high-pass cutoff) | −64 – +63 | `-64`…`+63` | 0 |
| `nn RES` | Part **RES** (resonance) | −64 – +63 | `-64`…`+63` | 0 |
| `nn CUTOFF` | Part **CUTOFF** (low-pass cutoff) | −64 – +63 | `-64`…`+63` | 0 |
| `nn REV` | Part **REV** (reverb send) | 0 – 127 | `0`…`127` | 40 |
| `nn CHORUS` | Part **CHORUS** (chorus send) | 0 – 127 | `0`…`127` | 0 |
| `nn VAR` | Part **VAR** (variation send) | 0 – 127 | `0`…`127` | 0 |
| `nn DRY` | Part **DRY** (dry level) | 0 – 127 | `0`…`127` | 127 |
| `nn HPF2` | Part **HPF2** (EQ grid, clean high-pass) | Off, 20 – 1000 Hz | `Off`, `20`…`1.00k` | Off |
| `nn EQ1GAIN` | Part EQ band **1 LO SHELF** gain | −12 – +12 dB | `-12.0dB`…`12.0dB` | 0.0dB |
| `nn EQ2GAIN` | Part EQ band **2 PEAK** gain | −12 – +12 dB | `-12.0dB`…`12.0dB` | 0.0dB |
| `nn EQ3GAIN` | Part EQ band **3 PEAK** gain | −12 – +12 dB | `-12.0dB`…`12.0dB` | 0.0dB |
| `nn EQ4GAIN` | Part EQ band **4 PEAK** gain | −12 – +12 dB | `-12.0dB`…`12.0dB` | 0.0dB |
| `nn EQ5GAIN` | Part EQ band **5 HI SHELF** gain | −12 – +12 dB | `-12.0dB`…`12.0dB` | 0.0dB |

Values read exactly as the editor's own controls read them, so a lane showing
`-12` on `05 CUTOFF` is showing the same `-12` you see on part 5's CUTOFF knob.

## The per-drum-key fourteen

Each of these appears for **every key of every DRUMS buffer** — 4 424 lanes in
all. They are the same controls the Part module's knob row retargets to when it is
in drum-key editing mode, plus HPF2 and the three AMP EG rates.

| Parameter | Editor control | Range | Shown as |
|---|---|---|---|
| `DRn kk VOL` | drum **VOL** (this drum's level) | 0 – 127 | `0`…`127` |
| `DRn kk DETUNE` | drum **DETUNE** (fine pitch) | −64 – +63 cents | `-64`…`+63` |
| `DRn kk KSHIFT` | drum **KSHIFT** (coarse pitch) | −64 – +63 semitones | `-64`…`+63` |
| `DRn kk PAN` | drum **PAN** | random, left – center – right | `RND`, `L63`…`C`…`R63` |
| `DRn kk HPF` | drum **HPF** (high-pass cutoff) | −64 – +63 | `-64`…`+63` |
| `DRn kk RES` | drum **RES** (resonance) | −64 – +63 | `-64`…`+63` |
| `DRn kk CUTOFF` | drum **CUTOFF** (low-pass cutoff) | −64 – +63 | `-64`…`+63` |
| `DRn kk REV` | drum **REV** (reverb send) | 0 – 127 | `0`…`127` |
| `DRn kk CHORUS` | drum **CHORUS** (chorus send) | 0 – 127 | `0`…`127` |
| `DRn kk VAR` | drum **VAR** (variation send) | 0 – 127 | `0`…`127` |
| `DRn kk HPF2` | drum **HPF2** (EQ grid, clean high-pass) | Off, 20 – 1000 Hz | `Off`, `20`…`1.00k` |
| `DRn kk ATTACK` | AMP EG **attack** | −64 – +63 | `-64`…`+63` |
| `DRn kk DECAY1` | AMP EG **decay 1** | −64 – +63 | `-64`…`+63` |
| `DRn kk DECAY2` | AMP EG **decay 2** | −64 – +63 | `-64`…`+63` |

**Their defaults are the kit's, not a fixed number.** A drum key's resting value
depends on which kit is loaded and which key it is — a hi-hat and a kick do not
load at the same level or pan — so each lane's default is that key's own value in
the Standard Kit, and `0` on the three EG rates and the filters means "as the kit
made it" rather than "zero".

**Three of the drum-edit knobs have no lane.** **K.ON** and **K.OFF** (receive
note-on / note-off for this key) are switches you set once rather than ride, and
the part's **DRY** has no drum equivalent at all. The editor's automation markers
show this directly: switch the Part module into drum-key editing and the marks
move to the knobs that have lanes, and on the Graphic Display to the **AMP EG**
button, whose three points are the key's EG rates.

> **The lane belongs to the BUFFER, not to a part.** `DR1` is the DRUMS1 buffer,
> and every part using that buffer hears the change — which is exactly how the
> hardware behaves, and why the drum key strip shows the same marks on two parts
> sharing a buffer. If you want two drum parts automated separately, put them on
> different buffers.

> **Watch out — a kit change moves the ground under a lane.** Loading a new kit
> into a buffer resets every key in it to the new kit's defaults, and `DR1 D1 38`
> is then a different instrument. A lane still holding a value will re-apply it over
> the new kit. Automate the kit change *or* the key's settings, not both, and
> remember that a program change at the top of a take re-initialises the buffer.

## The EQ band gains

The **gain** of each of the Part module's five EQ bands has a lane; their FREQ and
Q do not. That is deliberate rather than an oversight — gain is what you automate,
and it is enough to fade a band in or out, or switch it off entirely, in the middle
of a song. Set the frequency and Q by hand, then automate the gain.

Because a band at 0.0 dB does nothing, **automating a gain to 0.0 dB is how you
turn that band off**, and back up is how you turn it on.

Two of the five (**1 LO SHELF** and **5 HI SHELF**) are the hardware's own EQ and
travel to the module as SysEx like everything else. The middle three are the
plugin's own peaking bands, so — like HPF2 — they are free of the MIDI budget
below and are automatable *only* from a DAW lane. They behave identically
otherwise, which is why the editor marks all five together.

In the editor a single triangle sits by the **GAIN** row label
rather than one by each of the five boxes.

### Why the list stops here

Much of the editor is still **not** published as lanes, and that is a decision
rather than an omission. Every EQ band's FREQ and Q, every effect parameter and the
remaining plugin-only controls could have been added; the result would be a list in
which finding `12 CUTOFF` meant scrolling past tens of thousands of entries nobody
automates. A list that cannot be navigated is worse than a short one.

So the published set is the controls people actually ride during a song — levels,
pans, sends, cutoff, pitch, envelope shape, and enough EQ to fade a band in and out.

The drum keys are the one place where that rule produced a very long list anyway,
because a kit's interesting controls are per key and there was no honest way to
guess in advance *which* keys you would want. Publishing a few popular keys would
have meant re-visiting the decision the first time somebody automated a ride cymbal,
so every key is there and the folders keep it navigable.

**If you need more automation than this**, the better answer is usually not to look
for it here. Send the parts to
[their own stereo outputs](#per-part-stereo-outputs) and process them in your DAW,
where your own plugins bring their own automation and you are no longer limited to
what an XG module can be told over a MIDI cable.

> **Watch out** — these lanes address the **part's** EQ. In drum-key editing mode
> the same boxes retarget to the selected drum key, and the drum key's EQ band gains
> have **no** lane of its own. (Its HPF2 does — see the drum table above. The band
> gains do not.)

**HPF2 is the exception to almost everything else in this appendix.** It is a
plugin-only filter with no XG address, so — along with the three middle EQ band
gains — it *cannot* be recorded as SysEx on a MIDI track; a DAW lane is the only
way to automate it. For the same reason it costs nothing on the MIDI budget
described below, and it keeps working while the module is still starting up.
Anything under 20 Hz reads as `Off`.

> **Note** — `nn HPF2` addresses the **part's** HPF2. When the Part module is in
> drum-key editing mode the same box retargets to the selected drum key, and that
> per-key value has its own separate lane, `DRn kk HPF2`. They are two different
> filters that stack, not two names for one.

## Two kinds of automation

|  | DAW automation lane | XG SysEx on a MIDI track |
|---|---|---|
| Covers | the 4 939 above | every parameter the **device** has (not the plugin-only ones: HPF2 part and drum, the EQ mid bands) |
| Recorded by | arming a lane in Write or Touch | recording the plugin's MIDI output |
| Edited with | your DAW's automation editor | your DAW's list/SysEx editor |
| Timing | within about 1/30 s | sample-accurate on the note timeline |

Both work, both can run in the same project, and the editor marks which controls
have a native lane — an orange triangle before the control's name, switchable
under **Options ▸ Mark DAW-automatable parameters**.

> **Watch out** — do not automate the **same** parameter both ways at once. The
> MIDI track and the DAW lane will both write it, whichever arrives last wins, and
> the value visibly flickers.

## Things worth knowing

**They are not sample-accurate.** The plugin talks to its own sound engine over a
MIDI connection, exactly as a hardware MU100 would be driven, so an automation
move lands within about a thirtieth of a second rather than on an exact sample.
For faders, filter sweeps and sends — which is what these are — that is
inaudible. If you need something locked to a specific note, put it on a MIDI
track.

**There is a budget, and it is the MIDI connection, not your CPU.** That
connection carries about 3 125 bytes a second, the same as a real MIDI cable. In
practice **around ten parameters can move continuously at once**. Past that the
plugin deliberately thins its updates out rather than letting them pile up, so a
big simultaneous sweep gets coarser instead of lagging. Automating ten faders is
fine; automating a hundred at once is not.

**They move both ways.** Turning a knob in the editor, loading a file, or doing a
Read from Device moves the matching DAW parameter too — so a lane armed in Write
or Touch will record it.

> **Watch out** — that also applies to XG SysEx *arriving* from a MIDI track. This
> is genuinely useful: play back a recorded SysEx lane with a DAW lane armed in
> Write and you have converted it into native automation. It is a nasty surprise
> if you left Write armed by accident.

**Adding parameters later is safe, and so is renaming them.** Each one is
identified by an internal identifier rather than by its position or by the name
you see, so a future version that adds a parameter in the middle of the list — or
relabels one — will not break automation you have already recorded.
