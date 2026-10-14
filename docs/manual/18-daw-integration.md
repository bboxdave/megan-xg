# 18 Using the plugin in a DAW

> **VST only** — this whole chapter. For running the plugin on its own, see
> [Running standalone](#19-running-standalone).

## The two MIDI inputs

Your DAW sees **two** MIDI inputs for this plugin, shown as something like
`MEGAN XG - A` and `MEGAN XG - B`.

| | |
|---|---|
| **A** | Parts 1–16 |
| **B** | Parts 17–32 |

That is the convention, and it matches the part numbering in the Voice module —
though what actually decides which part responds is that part's **CH** setting, so
you can arrange it differently if you want to.

One MIDI track on port A gives you the sixteen channels a General MIDI or XG file
expects. Port B is there when you want more parts than one MIDI port can address.

Pitch bend, modulation, aftertouch and all other controllers work independently on
both ports.

> **Editing messages ignore both the port and the channel, and that is correct.**
> Notes and controllers are addressed by port and channel, so they need the right
> one. An XG parameter message does not work that way: it names the part it is for
> **inside the message**, as an address, the same way the hardware's own front panel
> would. So a recorded edit for part 1 lands on part 1 whichever port and whichever
> channel the track carrying it happens to use. If you record your editing onto a
> port B track and find it still moving the port A parts you meant it for, nothing
> is wrong — that is the design, and it works the same way on the hardware.
>
> The practical consequence is the one worth remembering: **you cannot re-aim a
> recorded XG edit by moving the track to another port or channel.** To move an edit
> to a different part you have to make it again on that part.

## Recording your edits as MIDI

Every edit you make in the editor is sent out of the plugin's **MIDI output** as
system-exclusive data. Record that, and you have your editing captured on a MIDI
track — which is how XG setups have always been distributed inside song files.

That is the default. **Options ▸ MIDI Out sends edits as** can switch the output to
ordinary MIDI controllers instead, for the eleven controls that have one — see
[recording edits as controllers](#recording-edits-as-controllers-cc) below. What
follows applies to both, except where it says otherwise.

**Check first that your DAW can do this at all.** XG is an extension of General
MIDI, and the parts of it that are ordinary MIDI — notes, controllers, program
changes — every host handles. But the XG *parameter* settings, which is nearly
everything this editor changes, travel as **system-exclusive** messages, and support
for those varies more than almost anything else in a DAW. Some record and play them
back as readily as notes; some record them but give you no way to see or edit them;
some filter them out silently. Cubase, one of the two hosts this plugin was tested
in, handles them fully. If yours cannot carry SysEx, none of what follows will work,
and your automation is limited to the native lanes described next — a much smaller
subset of what the editor can change.

The recipe in Cubase, where a host that handles SysEx will want something similar
even though the names and the routing will not match step for step:

1. Create a MIDI track and set its **input** to the plugin's MIDI output.
2. Set its **output** back to `MEGAN XG - A`, so playing the track re-applies the
   edits.
3. Record-arm it and make your edits. They appear as SysEx events.

> **Watch out** — turn **input monitoring off** on that track while recording. If
> it is on, each edit goes out, comes back, and is applied twice. Whatever your DAW
> calls it, this is the step to look for: the loop from the plugin's MIDI output
> back into its input is what you are trying to avoid.

Why do this at all, when the DAW project already saves your setup? Because a
recorded setup **happens at a point in the timeline**. Put it in bar 1 and the
module configures itself every time the song plays from the top, exactly as an XG
MIDI file does — and you can change a sound halfway through a song by putting the
edit there.

> **Watch out** — the editor does **not** follow program changes coming the other
> way. If a MIDI track selects a voice, a drum kit or an effect program, the module
> obeys it and the editor's panels do not move, so what you see can stop describing
> what you hear. It matters most on a drum part, where the kit change also resets
> that buffer's per-key settings. See
> [changing sounds from a MIDI track](#changing-sounds-from-a-midi-track).

### Recording edits as controllers (CC)

Recorded SysEx is complete but hard to edit: most DAWs show it as a list of
numbers. Eleven of the editor's controls have an ordinary MIDI controller that does
the same job, and controllers are something every DAW can draw and edit as lines.
Pick **Options ▸ MIDI Out sends edits as ▸ MIDI CC (only parameters that have a
CC)**, and those eleven go out as controllers instead:

| Control | Where | Controller |
|---|---|---|
| VOL | Part module, or the VOLUME screen | CC7 |
| PAN | Part module, or the PAN screen | CC10 |
| REV | Part module, or the REVERB screen | CC91 |
| CHORUS | Part module, or the CHORUS screen | CC93 |
| VAR | Part module, or the VAR screen | CC94 |
| CUTOFF | Part module | CC74 |
| RES | Part module | CC71 |
| Attack | AMP EG screen | CC73 |
| Release | AMP EG screen | CC72 |
| PORTA | Part module | CC65 |
| PTIME | Part module | CC5 |

In this mode the editor **underlines** the names of these controls, and of the
Graphic Display screens that edit them, and shows an orange `MIDI OUT: CC` badge in
the bottom bar. Each knob's hover text also names its controller.

**Everything else sends nothing** in this mode: the effects, the EQ, the System
module, the voice itself, and every drum key in drum-edit mode. That is deliberate.
There is no fallback to SysEx, so a recording is all controllers. To capture those
other edits, record them in a separate pass with the setting back on XG SysEx.

The AMP EG's **decay** is the surprising absence. The XG standard lists a
controller for it (CC75), but the MU100 was measured to ignore it, so it is not
sent.

**The Controller module's wheel is sent too, in this mode only.** Moving the wheel
sends the modulation wheel, pitch bend, aftertouch or the CONTROLLER 1/2 assignment
it is set to, so a filter sweep played on the wheel records along with everything
else. In XG SysEx mode the wheel sends nothing to the MIDI output, as before.

The recipe is the same as for SysEx. Two differences matter on playback.

#### A controller goes to a channel, not to a part

XG SysEx names its part inside the message, which is why the warning above says a
recorded SysEx edit cannot be re-aimed. **A controller is the opposite: it carries
only a channel**, and every part listening on that channel obeys it. The plugin
sends each controller on the channel the part **receives** on (its Rcv CHANNEL in
the Voice module), so it lands where you meant it, but only while the routing still
agrees. Four consequences:

- **Set the track's MIDI channel to "Any"**, or to the channel the controllers were
  recorded on. Many DAWs rewrite every event on a track to the track's own channel
  when one is set, which would send part 5's controllers to part 1.
- **Route the track to the right port.** Parts on MIDI IN B get their controllers
  on the same channel numbers as port A. The plugin has a single MIDI output, so
  the recording cannot say which port a controller was meant for; the track's
  output does. If you edit parts on both ports, record each port's parts in a
  separate pass, onto a track routed to that port.
- **Two parts sharing a receive channel both follow the recording**, even though
  your edit in the editor moved only one of them. That is what a hardware
  controller on that channel would do.
- **A part whose Rcv CHANNEL is OFF sends no controller at all**, because there is
  no channel to put one on.

A part also ignores a controller if its matching receive switch is off: Rcv CC,
VOL, PAN or POR in the Part module's LED row.

#### Values that cannot be recorded exactly

- **PAN RND** has no controller value. It is recorded as **L63** (hard left), and
  plays back as L63.
- **PORTA** is on or off, so it is recorded as 127 or 0, and **each switch records
  as two events at the same moment**: the old value, then the new one. That is
  deliberate. Some DAWs (Cubase among them) play recorded controller data back as a
  curve that ramps between points, and PORTA flips halfway up the ramp, which would
  put every switch after the first one late. The repeated old value makes the ramp
  zero-length, so the switch lands exactly where you recorded it. If you edit the
  events by hand, keep the pairs together.
- Every other control records its exact value.

#### Things to watch on playback

- A controller played back from a track is incoming MIDI, so the editor shows it
  as a [MIDI override](#midi-override-marks) (the cyan marks) and does not change your
  saved setup.
- If a part's **CONTROLLER 1** or **CONTROLLER 2** is assigned to one of the eleven
  controller numbers above — say CONTROLLER 1 to 74 — then that controller does
  both jobs on playback: it moves CUTOFF and drives everything CONTROLLER 1 is set
  to modulate. The factory assignments (16 and 17) collide with nothing.
- **DAW automation lanes and Undo also send controllers** in this mode, exactly as
  they send SysEx in the other one. If lanes are playing while you record, their
  movements are recorded too.
- Do not drive one control from a recorded controller **and** a DAW lane or SysEx
  at the same time: they take turns overwriting each other.
- Put a starting value for each recorded controller at the start of the song. A
  controller changes the module, not the setup the project saves, so without one
  the song starts from wherever the last playback left it.

#### Editing the recorded controllers

The controllers land in the MIDI part like any other recorded controller data, and
how you edit them is up to your DAW. In Cubase, one of the two hosts this plugin was
tested in, there are three ways to get editable lines:

- open the part in the **Key Editor** and add a controller lane set to Main Volume
  (CC7), Pan (CC10) and so on;
- use **MIDI ▸ CC Automation Setup** to record controllers onto the MIDI track's
  **automation lanes** instead of into the part (it applies when both Record and the
  track's Write Automation are on);
- or use **Extract MIDI Automation** to move controllers already recorded into a
  part out onto those lanes.

Other DAWs have their own equivalents; check your DAW's documentation for
controller lanes.

## Native automation

4 939 controls have their own automation lane in the DAW: three master controls,
sixteen on each of the 32 parts, and fourteen on every key of the four DRUMS
buffers. They are listed in [appendix A3](#a3-daw-automation-parameters), which also
covers the limits worth knowing.

They are named by where they live — `01 CUTOFF`, `MASTER VOL`, `DR1 A#2 58 VOL` —
and grouped into folders: one per part, plus `MASTER`, plus one per DRUMS buffer
that opens into a folder per key. A DAW whose parameter picker understands those
folders (Cubase does) shows a collapsed list you open a part or a key at a time; one
that does not shows a single flat list, master and parts first, then the drum keys.

A drum key carries **both** its note name and its MIDI note number (`A#2 58`)
because DAWs disagree about octave numbering. If your piano roll calls that note
`A#3`, trust the number.

The editor marks which controls those are — a small orange triangle before the
control's name. Everything *else* in the editor is still automatable as SysEx
on a MIDI track, so the marker distinguishes two kinds of automation rather than
saying what can and cannot be automated.

The short version of the limits:

- Not sample-accurate; a move lands within about a thirtieth of a second.
- About ten parameters can move continuously at once before the plugin starts
  thinning updates out.
- Do not automate the **same** parameter as both a DAW lane and track SysEx.

If your DAW cannot record system-exclusive data, these lanes are all the automation
you have. They are chosen to be the controls people actually automate — volume, pan,
the effect sends, cutoff, and each drum key's level, pitch, filter, sends and
envelope — but they are still a fraction of the editor, and there is no way to reach
the rest without SysEx.

**The list stops where it does on purpose.** The editor has hundreds more controls
that could in principle have been added — every EQ band's frequency and Q, every
effect parameter. Publishing them all would have produced a list long enough to be
useless for finding the dozen parameters anyone actually automates, so the remaining
plugin-only controls were deliberately left out. The drum keys are the exception,
and they are grouped three folders deep for exactly that reason. See
[appendix A3](#a3-daw-automation-parameters).

If you need finer control than the lanes give you, the other route is to stop
asking the MU100 to do it: send the parts to
[their own audio outputs](#per-part-stereo-outputs) and shape them with your DAW's
own EQs, filters and dynamics, which automate however your DAW automates anything.

## Exporting audio: bounce in real time

> **Watch out** — **export, bounce or render your mixdown in real time.** Every
> other plugin in your project can be asked to render a five-minute song in thirty
> seconds; this one cannot.

Inside the plugin is an emulated MU100, and it runs at the speed a real MU100 runs
at — one second of audio per second, on its own thread, driven by the clock rather
than by your DAW's export. Ask your DAW to render faster than that and it will
collect whatever audio happens to have been produced by the time it asks, which is
not the same thing as the performance.

**What that sounds like.** The exported file is wrong all the way through rather than
subtly off. Confirmed in Cubase, where it came out as **a series of short bursts of
static**; on another machine, DAW or buffer size it may read more as stuttering, clicks
or gaps. What they have in common is that the damage is continuous and obvious.
Exporting the same project again with real-time switched on produced a correct file.

**What to do**, whatever your DAW calls it:

- Turn on the **real-time** (or *online*, or *play during export*) option in the
  export/bounce dialog and export as usual. Most hosts have one.
- If yours does not, record the plugin's output onto an audio track while the song
  plays, and use that.

The same applies to anything else that renders ahead of the clock — **freeze**,
*render in place*, *bounce in place*, or an offline stem export. Freezing a track
that feeds this plugin is fine if your DAW freezes in real time; check before you
trust the result.

**Listen to the exported file before you delete anything.** A faster-than-real-time
bounce does not report an error — it just quietly produces the wrong audio.

## Per-part stereo outputs

**Options ▸ Audio... ▸ Separate stereo output per part** gives each of the 32 parts
its own stereo output, so you can mix and process them in your DAW.

You have to enable the buses in your DAW as well — most hosts leave extra outputs
switched off until you ask for them, and some want a particular kind of track, or a
setting on the instrument, before the extra outputs appear at all.

**Consult your DAW's documentation** for how it handles multi-output instruments.
The plugin's setting and the host's have to agree, and only one of the two being
switched on is far and away the most common reason nothing shows up.

**What comes out of each part output:**

| Included | Not included |
|---|---|
| The part's notes | Reverb, chorus, variation |
| Its volume and pan | The insertion effects |
| Its filters and EQ | The Multi EQ |

In other words it is the **dry** part signal. That is deliberate — the effects are
shared, and a shared reverb cannot meaningfully be split across 32 outputs. Use
your DAW's own effects on the split-out parts, and keep the MU100's effects for
the parts you leave in the main mix.

The second checkbox, **...and remove them from the main output**, stops you hearing
each part twice. Note that a removed part's **reverb tail stays in the main mix** —
its dry signal has gone to its own output, but what it already sent to the reverb
is still there.

> **Watch out — this is the classic *why is it so quiet* trap.** Both checkboxes
> are stored per installation, not per project, so a new instance arrives with
> whatever you last used. Tick them in a project where the host has the extra
> outputs switched on but not routed anywhere you monitor, and the parts leave the
> main mix for buses you cannot hear. What is left in the main output is the effect
> returns alone: faint, distant, and gone completely the moment you turn the reverb
> down. See
> [everything is faint](#everything-is-faint-and-vanishes-when-i-turn-the-reverb-down).

**Mute and solo reach the output buses too.** A part silenced with the Voice
module's **M** or **S** cells is silent on its own output bus as well as in the
main mix, so soloing still does what you expect when you are monitoring the
separate outputs. What that means in practice: **do not leave a part muted while
you record or bounce the stems**, because its bus records silence. Mute and solo
are cleared when the plugin window closes and are never saved, so this can only
bite you within a single session — but a stem bounce is exactly the kind of long
unattended job where it would. See [mute and solo](#m-and-s-mute-and-solo).

**Cost.** Running all 32 costs a fixed amount of CPU whatever you are playing —
measured at about 2.5 percentage points of a Cubase performance meter. That reads
as a lot on an empty project and as very little on a busy one; it is the same work
either way.

> **Watch out** — do not switch the part outputs on or off **while the transport
> is running**. Your DAW has to stop and restart the plugin to change its output
> layout, and notes that were sounding get stuck. Set it up before you press play.

## Naming the output tracks

**File ▸ Name All Output Tracks from Voices** renames your DAW's 32 part output
tracks after the voices on them — `Pt 01 Concert Grand` rather than `Part 1`. It
needs per-part outputs to be on in the plugin **and** enabled in your DAW.

The names follow Options ▸ **Voice names**: with **Full** showing you get
`Pt 01 Concert Grand`, with **8 characters** you get `Pt 01 ConGrnd`. Since DAW
mixer strips may truncate names, the short form is often the one that stays
readable — so if you want full names in the editor and short ones on the tracks,
switch to **8 characters**, run **Name All Output Tracks from Voices**, then
switch back. The names have already gone to the host and stay as they are.

> **Watch out** — it is all 32 at once. Your DAW re-reads every name in one go, so
> if you have renamed some of those tracks by hand this will overwrite them,
> including on parts you did not touch. That is exactly why it is a command you
> choose rather than something that happens when you pick a voice.

**The procedure, and the order you do it in, varies by DAW.** The plugin publishes
the names; what happens to your tracks after that is the host's decision, and there
is no one standard for it. In the two hosts this plugin was tested in:

- **Cubase** — the plugin's multiple outputs must be enabled in the DAW's own
  settings window for the plugin **before** you run **Name All Output Tracks from
  Voices**.
- **Bitwig** — **Name All Output Tracks from Voices** must be run **before**
  Bitwig's **Add Missing Chains** function.

Done in that order, the voice names populate the tracks automatically. Done in the
opposite order, the names are still published but not applied: you have to pick the
new name for each track by hand from a dropdown on the track. Other DAWs may want
either order, and some do not support renaming from a plugin at all. Your DAW's
documentation is the place to find out which.

## Latency and buffering

Two settings under **Options ▸ Audio...**, and they do different things:

**Buffer threshold** is how far ahead the emulated sound engine works. Higher is
more robust, lower is more responsive. See
[Options and settings](#buffer-threshold) — and note that if you get an occasional
stutter in an otherwise clean session, the two largest settings exist for exactly that,
and the latency they add is compensated for by your DAW.

**PDC Trim** tells your DAW how much earlier to send events so that what comes out
lands where you wrote it. If this plugin's notes sound consistently late against
the rest of the project, raise it until they line up.

## Multiple instances

Nothing stops you loading several — each is a complete, independent MU100. Each
one runs its own emulated processor and sound chip, so the CPU cost multiplies. If
you need more than 32 parts, that is the way to do it; if you need more than 64
notes of polyphony, likewise.

They share one `settings.xml`, so preferences are common to all of them. Each
instance writes only the settings it changed itself, so a change you make in one
is never undone by another that still has the old value in front of it. The
instance that was already open goes on using its own value until it is created
again, which for a plugin window means closing it and opening it once more.

Two things are genuinely per instance rather than shared. **Separate stereo output
per part** and **Enable DAW automation** live in the project, so each instance
carries its own. And the Recent Files list is one list: if two instances both open
files, the list ends up holding the files of whichever opened one last.

## Reopening a project

Your DAW restores the whole setup — every part, voice, effect and drum edit, plus
the two per-part output checkboxes and **Enable DAW automation**, which are the
only Options-menu settings a project carries at all. Everything else in Options comes from your installation, so
a project opened on another computer picks up *that* machine's ROM folder, buffer,
PDC trim and MIDI input routing rather than dragging along settings that would not
work there. [What is remembered where](#what-is-remembered-where) has the details.

There is one behavior worth recognizing:

> **Watch out** — on a large project, the first ten to fifteen seconds after
> opening can sound wrong, with notes arriving late, while the plugin re-sends the
> whole setup to its sound engine. Then it corrects itself. That is expected on
> the projects where it happens, and there is nothing to fix.

Also worth knowing: if you save a project in the first few seconds after adding
the plugin, before the emulated module has finished booting, the snapshot of its
memory is not there to be saved — and that re-send is what covers you.
