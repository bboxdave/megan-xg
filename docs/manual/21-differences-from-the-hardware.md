# 21 Differences from the MU100 hardware

The sound engine here is the MU100's own firmware and sample data running in
software, so voices, effects and the way parameters interact are the device's
behavior rather than an imitation of it. But it is an emulation running in a
plugin, and there are differences in both directions worth knowing about.

## What the plugin adds

### Filters and EQ the hardware does not have

**HPF2** — a clean, uncolored high-pass on each part, with the cutoff set in
hertz: two poles, 12 dB per octave, no resonance. The MU100's own HPF is
*resonant*, which colors everything it touches; this is the plain low-cut you would
reach for to clear space in a mix.

**EQ bands 2, 3 and 4** — three peaking bands per part, on top of the hardware's
own bass and treble shelves. A proper parametric per part, in other words, where
the hardware gives you two shelves.

Both are available per part **and** per drum key.

> **Watch out** — neither survives a `.syx` export. See
> [what saves where](#important-what-saves-where).

### Effect tempo-sync

Delay times and LFO rates expressed as note values, following your project tempo.
The hardware has no such feature; the plugin works out the milliseconds and sends
those, so the *sound* is entirely ordinary XG data.

### 32 parts over two MIDI ports

The hardware has 32 parts too, reached through two MIDI inputs. The plugin
presents both to your DAW as separate named inputs, which is more convenient than
a physical second cable.

### Per-part audio outputs

32 separate stereo outputs into your DAW. The hardware has one stereo pair (plus
digital out). This is the biggest practical gain of running it as a plugin.

### Native DAW automation

4 939 controls with real automation lanes. On hardware, everything is SysEx.

### An editor that cannot get out of step

XGEdit and its contemporaries talked to the module over a MIDI cable, and needed
a multi-client driver, a loopback and a certain amount of luck to work alongside a
sequencer. Here the editor and the module are one program.

What can still get out of step is a change the editor did not make. There are two,
and they are no longer the same kind of problem.

Work done on the [Hardware view's front panel](#17-the-hardware-view) reaches the
module and nothing else, and the editor cannot see it — so the panels keep showing
the old values and a save stores those. **File ▸ Read from Device** is the answer.

A [program change or controller from a MIDI track](#changing-sounds-from-a-midi-track)
also moves the module without moving your setup, but that one is deliberate and the
editor **shows you it has happened**: the affected controls are marked in cyan and
the bottom bar counts them. Saving stays safe, and you can put the module back on
your values with one click.

## What the plugin does not do

**The A/D input part.** The hardware can take a microphone or a line signal, run
it through the effects, and mix it in. There is no audio input here.

**PLUG-IN boards.** Yamaha's PLG series — the boards you slid into the MU100's
expansion slot to add a different synthesis engine, its own voices and its own
parameter set (PLG100-VL, -DX, -SG, -VH and the rest), and the VL board fitted as
standard in the MU100R — are **not supported**. The slot is not emulated, nothing
answers on a PLG part, and voices or setups that expect a board are unavailable.
Nor is there a plan for it: each board is a second piece of hardware, with its own
processor and ROMs, and would be a project in itself.

**Performance mode from the editor.** The module's four-part Performance mode is
fully present in the firmware and reachable through the
[Hardware view](#17-the-hardware-view), but the editor does not cover it.

**Follow program changes in the editor.** The module obeys a program change from a
MIDI track, so the sound is right, but the editor does not track it and its panels
can end up describing a sound you are no longer hearing — and on a drum part, one
whose per-key settings the device has just reset. Selecting sounds in the editor is
the way of working this was built around; driving them from the timeline is not,
and it is the one workflow where the editor and the module can disagree. See
[changing sounds from a MIDI track](#changing-sounds-from-a-midi-track).

**Digital output, and the front-panel headphone and volume controls.** Not
meaningful in a plugin.

## Where the emulation is not the hardware

This is an emulation. It is close, and it is derived from the real firmware and
ROMs, but it is not bit-identical, and it would be dishonest to claim otherwise.

**A great deal of it was worked out by reverse engineering.** Yamaha never
published how the MU100's sound chip works, so the emulation was built by reading
the firmware, watching what it writes to the chip, and comparing the result against
recordings of the hardware, one behavior at a time. Most of it now matches closely.
Some parts are **approximations** — the right shape, arrived at by measurement
rather than from a specification, and correct to within what could be heard and
measured. A few are **unresolved**: places where the hardware demonstrably does
something the emulation does not reproduce, and the reason has not been found yet.
Where such a thing is known, it is written down — in this chapter, in
[known issues and troubleshooting](#22-known-issues-and-troubleshooting), and in
the effect notes. Expect to meet the occasional rough edge, and expect the list to
shrink over time rather than grow.

**One known deliberate departure.** The way the sound chip decodes some of its
compressed samples is not fully understood, and as written it lets a very slow
drift build up in the signal. On real hardware that never surfaces; here, one
effect at an extreme setting could turn it into silence. The plugin bleeds that
drift away slowly. The measured effect on the sound is under 0.2 dB anywhere above
120 Hz, on any voice — inaudible, and it makes the difference between an effect
working and not.

**The Hardware view's LCD.** The panel's display runs the real firmware and shows
the real pages, but it is not a pixel-accurate reproduction — some elements of the
hardware's screen are missing from it. See the
[Hardware view](#17-the-hardware-view).

**Timing.** Notes you play arrive on the emulated module's MIDI connection with
the same timing your DAW gave them. Editor edits and DAW automation are not
sample-accurate — they land within about a thirtieth of a second, which is what a
knob turn on a real module would be too.

**Speed of bulk operations.** Loading a setup, or a drum buffer, takes real time,
because the data goes over an emulated MIDI connection at the same 31 250 baud a
real cable runs at. That is faithful, and occasionally inconvenient.

**CPU.** A real MU100 costs you nothing; this costs a processor core doing real
work. See [system requirements](#what-you-need), and
[it is using too much CPU](#it-is-using-too-much-cpu) for what can be reclaimed.

**It plays in real time, so it has to be recorded in real time.** A rack module
cannot be asked to play a song twice as fast, and neither can this: your DAW has to
**export, bounce and freeze in real time**, or it will collect audio the emulation
never got round to producing. See
[Exporting audio](#exporting-audio-bounce-in-real-time).

## Things that are the same, and surprise people

Worth stating, because they read as plugin limitations and are not:

**One reverb, one chorus, one variation, shared by all 32 parts.** Each part sets
how much of it to use, not which one. This is how every XG module works. See
[Understanding the effect units](#understanding-the-effect-units).

**64 notes of polyphony**, shared across all 32 parts, and a two-element voice
uses two of them. Element reserves in the Voice module are how you protect the
parts that matter.

**A drum kit belongs to a shared buffer, not to a part**, and assigning one resets
its per-key settings. See [Drum editing](#the-four-drum-buffers-the-thing-to-understand).

**Some controls do nothing on a drum part** — mono/poly, portamento, scale tuning,
the pitch envelope, and the filter and amplitude modulation depths. The editor
grays them out rather than letting you set something the device will ignore.
