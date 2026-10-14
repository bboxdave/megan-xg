# 22 Known issues and troubleshooting

## No sound at all

**Look at the bottom bar first — it takes one second.** If anything is muted or
soloed, a colored `SOLO`/`MUTE` badge sits at its right-hand end. Click it to
clear everything and try again. Solo is the one to suspect: soloing a part that
happens to be playing nothing silences the whole plugin, and the badge is the only
thing on screen that says so if the Voice module's M and S columns are hidden or
you are in the Hardware view. See [mute and solo](#m-and-s-mute-and-solo).

**Then check the ROM files.** Switch to the Hardware view. If it reports that the
emulation engine stopped, or the LCD never lights, the ROMs are missing or
incomplete. See [ROM files](#rom-files-what-you-need-and-where-to-put-them) — all
eight files, in `mu100.zip`, in `Documents\MEGAN XG\`.

**Then check you are getting audio out.** Right-drag the on-screen keyboard. If
that makes no sound:

> **Standalone only** — **Options ▸ Audio...** and confirm your real sound card is
> selected.

> **VST only** — confirm the instrument track's output is routed somewhere you can
> hear.

If you can hear *something*, but it is faint and washy rather than absent, this is
a different problem — see
[everything is faint](#everything-is-faint-and-vanishes-when-i-turn-the-reverb-down)
below.

**Then check MIDI is arriving.** Watch the activity tabs at the right of the Voice
module while you play. Nothing lighting means nothing is reaching the plugin —
check your track's output port (`MEGAN XG - A` or `- B`), or, in the standalone,
**Options ▸ MIDI Input...**.

## Everything is faint, and vanishes when I turn the reverb down

Almost certainly the per-part outputs. Open **Options ▸ Audio...** and look at
**Separate stereo output per part** and **...and remove them from the main
output**.

Those two settings live in `settings.xml`, which belongs to the *installation*, not
to your project — so a fresh instance comes up with whatever you last used, in
projects and DAWs where you never set the extra outputs up. If they are both on and
your DAW has the plugin's extra outputs switched on but going nowhere you are
listening to, the parts have been taken out of the main mix and are playing into
buses you cannot hear.

**The giveaway is the reverb.** Only the *dry* part signal is removed from the main
output; what the parts sent to reverb, chorus and variation still comes back there.
So the main output is carrying the effect returns and nothing else — distant,
washy, far too quiet — and turning the reverb down, or setting it to `NO EFFECT`,
takes away the last of what you could hear and leaves silence.

Either fix works:

- Untick **...and remove them from the main output** — or both checkboxes — to put
  the parts back in the main mix; or
- Set the extra outputs up in your DAW so you actually hear them. See
  [per-part stereo outputs](#per-part-stereo-outputs).

> **Watch out** — change these with the transport stopped. See
> [switching part outputs during playback](#switching-part-outputs-during-playback-causes-stuck-notes).

## One part is silent

**Check the M and S cells on its row in the Voice module.** A cyan **M** means you
muted it; a red **S** anywhere in the column means some *other* part is soloed and
this one is being held back by that. The badge at the right of the bottom bar tells
you the same thing without hunting, and clicking it clears both. Note a muted
part's **level meter keeps moving** on the VOLUME screen — that is deliberate, and
it is not evidence the part is audible.

**Then check its receive switches.** This is the most common cause that survives a
restart, because unlike mute and solo it is saved with your patch.

Turn on **Options ▸ Show part MIDI receive switches**, select the part, and look
at the LED row along the bottom of the Part module. If **NO** is off, that part
receives no notes — it is effectively muted, and with the row hidden there is
nothing on screen to say so.

Then, in order:

- **CH** in the Voice module — is the part on the channel you are sending to? Is
  it set to OFF?
- **VOL** — is it at zero?
- The **note range** — the red bar over the keyboard. Are you playing outside it?
- The **velocity limits** — are you playing too softly or too hard?
- **DRY** — if variation is in insertion mode on this part, its dry level behaves
  differently. See [Variation's two modes](#variations-two-modes).

## A note is stuck

Press **PANIC**.

If it happens repeatedly, the usual cause is note-offs being lost — a MIDI
connection dropping mid-note, or the part outputs being switched during playback
(see below).

## A knob is grayed out and I cannot move it

Three possible reasons, and the tooltip or the module title usually says which:

- **The part holds a drum kit** and the device ignores that control on drum parts:
  mono/poly, portamento, scale tuning, pitch EG, FMOD and AMOD.
- **You are in drum-edit mode with no key selected.** The Part module title reads
  `Drum Key – none selected`, and its value boxes are blank. Click a drum key.
- **Options ▸ Enable plugin-only parameters** is unticked, which locks HPF2 and EQ
  bands 2–4.

And one that is not graying: **VAR** and **DRY** go inert when variation is
switched to insertion mode.

## The first seconds of a reopened project sound wrong

Expected on large projects. The plugin re-sends the whole setup to its sound
engine, which takes ten to fifteen seconds, and notes played during that queue up
behind it and arrive late. Then it corrects itself.

There is nothing to fix and no way to hurry it. It is the alternative to a project
that plays the wrong sound indefinitely.

## Switching part outputs during playback causes stuck notes

Known, and not going to be fixed. Changing the plugin's output layout makes your
DAW stop and restart it, and notes that were sounding genuinely lose their
note-offs.

**Set the part outputs up before you press play.** Press PANIC if it happens.

## It is using too much CPU

This plugin emulates a processor and a sound chip in software, so it costs real
work — a noticeable fraction of one core per instance, whatever it is playing. That
is the floor, and nothing here gets underneath it. What follows is everything above
the floor, **in the order worth trying**.

**First, be sure it is actually this.** Your DAW's performance meter measures the
audio callback, and this plugin does almost nothing there — the emulation runs on
its own thread, which no host meter can see. So a calm meter is not proof the
plugin is comfortable, and a busy one may be someone else's fault. The symptom to
go by is what you hear: clicks, dropouts, or a stutter of a second or two.

**Raise the buffer threshold.** **Options ▸ Audio... ▸ Buffer threshold**. This is
the big lever and it is the first thing to try — it gives the sound engine a deeper
reserve to draw on, so a brief interruption is covered instead of heard. The cost
is latency, which your DAW compensates for while mixing and which you will feel
while playing live. See [Buffer threshold](#buffer-threshold).

**Then turn the animations down.** All of these are under **Options ▸ Display
Animation...**, none of them affects the sound, and all of them are safe to leave
on for most people — the drawing happens on the interface, so the worst an
over-ambitious setting can do on its own is make the *display* sluggish. On a
machine that is already short of headroom, the interface is competing for the same
processor, so they are still worth reclaiming:

| Setting | What to do |
|---|---|
| **EQ animated spectrum** | Untick it. This is the heaviest of them by some way. Or leave it on and drop **Resolution** to 1024 or 2048, which is most of the saving |
| **Animated volume level meters** | Untick. The only one that is not purely drawing — it also switches on a measurement inside the emulated mixer, though a tiny one |
| **Played notes on the keyboard** | Untick |
| **MIDI input indicators** | Untick. The cheapest of the four, so the last to bother with |

**Then the two settings that are not animations:**

- **Options ▸ Enable DAW automation** *(VST only)*. Untick it if you are not using
  native automation lanes at all. It is a small saving in itself; what it mainly
  does is quiet the traffic between the plugin and your DAW. See
  [Enable DAW automation](#enable-daw-automation).
- **Separate stereo output per part** *(VST only)*. Running all 32 costs a fixed
  amount whatever you are playing — measured at about 2.5 percentage points of a
  Cubase performance meter. If you are not mixing the parts separately, turn it
  off. See [per-part stereo outputs](#per-part-stereo-outputs).

**Then count your instances.** Each one is a complete, independent MU100 running
its own emulated processor and sound chip, so two instances cost twice as much.
32 parts and 64 notes of polyphony is a lot; if you can fit the song into one
instance, do.

**Finally, close the editor window.** Nothing in the editor is needed for the
plugin to make sound, and with the window shut none of it is drawn at all. Worth
knowing for a long bounce or a heavy mixdown.

> **If the trouble is a stutter that comes back every couple of minutes** in an
> otherwise clean session, that is a specific symptom with its own answer — the two
> largest buffer settings exist for it. See
> [when the top two settings are the ones you need](#when-the-top-two-settings-are-the-ones-you-need).
> It is also worth suspecting something else on the machine entirely: anything that
> wakes on a timer — an updater, a backup, a virus scan, a manufacturer's
> system-management service — can do this, and it will look exactly like the plugin. It
> did during development, and a deep buffer hid it convincingly before the real cause
> was found. That case is written up in the same place.

## My exported mixdown is choppy, or full of static bursts

You exported faster than real time. The emulated MU100 plays at the speed a real
one does, so a DAW rendering a five-minute song in thirty seconds collects audio
that was never produced. Confirmed in Cubase: it came out as a series of short
bursts of static, and the same project exported in real time was correct.

The exact artifact varies with your DAW, buffer size and machine — static,
stuttering, clicks or gaps. Any of them, running through the whole file rather
than appearing once, means the same thing.

Turn on the **real-time** (or *online*) option in your DAW's export dialog and do
it again, or record the output to an audio track while the song plays. The same
applies to freeze and render-in-place. See
[Exporting audio](#exporting-audio-bounce-in-real-time).

## Text is clipped or squeezed after changing a text size

The five `editorText*Pct` settings grow the text but not the boxes around it, so
past a modest increase something will start to squeeze.

**Make the window bigger instead.** The whole editor scales to the window — text
and boxes together — and cannot clip. Then add 10 or 20 % of text scaling on top
if you still want it. A taller window helps more than a wider one. See
[Text size](#text-size-settings-only).

## The editor shows different values than I hear

Something changed the module without going through the editor. There are two
ways that happens: the **Hardware view's front panel**, which the editor cannot
see, and a **program change from a MIDI track**, which it does not follow.

**File ▸ Read from Device** brings the editor back in step either way. See
[Using both views on one project](#using-both-views-on-one-project) and
[changing sounds from a MIDI track](#changing-sounds-from-a-midi-track).

> **Watch out** — do not save the project until you have done that. The save
> stores what the *editor* holds, and loading it back sends that to the device, so
> a stale display becomes the sound.

## An automated parameter flickers

You have automated it **both** as a DAW lane and as SysEx on a MIDI track. Both
are writing it and whichever arrives last wins. Pick one.

## Automation is stepping coarsely

You are moving more parameters at once than the emulated MIDI connection can
carry — about ten continuously-moving ones. The plugin deliberately thins its
updates rather than falling behind. Automate fewer things at once, or move some of
them to SysEx on a MIDI track.

## MIDI stops arriving in the standalone

Windows occasionally stops delivering MIDI while everything still looks connected.
**Options ▸ Reconnect MIDI Inputs**, then PANIC if a note was held.

## Output track names came back wrong in my DAW

First check that per-part outputs are actually running: they have to be switched on
in **Options ▸ Audio...** *and* enabled on the host's side, since most DAWs leave a
plugin's extra outputs off until you ask for them. With no part outputs there are no
tracks to rename.

If they are on and the names still look wrong, the usual cause is the **order of
operations**, which differs by DAW. In **Cubase**, the plugin's multiple outputs
must be enabled in the DAW's settings window for the plugin *before* you run
**Name All Output Tracks from Voices**. In **Bitwig**, **Name All Output Tracks
from Voices** must be run *before* Bitwig's **Add Missing Chains**. Done the other
way round, the names are published but not applied — you have to select the new
name for each track by hand from a dropdown on the track. Running the command again
in the right order is the fix.

Beyond that it is your DAW rather than the plugin: the plugin publishes the names,
what a host does with them differs, and some DAWs do not support renaming from a
plugin at all, in which case nothing will happen and there is nothing to do about it
from this side. Check what yours documents.

Remember too that the command renames **all 32 at once**, so it will have overwritten
any of those tracks you had named by hand.

## My drum tuning disappeared

Assigning a kit to a drum buffer **resets that buffer's per-key settings** — even
if it is the same kit it already had, because the device re-initializes the drum
setup on any program write.

Undo will get it back if you have not done anything else since. In future, save a
`.mds` before changing a kit. See
[Drum editing](#the-four-drum-buffers-the-thing-to-understand).

If the kit was changed by a **MIDI track** rather than by you, undo has nothing to
offer, because the editor never saw it: the reset happened on the device and the
panels still show the old values. Read from Device, then reload your `.mds`. See
[changing sounds from a MIDI track](#changing-sounds-from-a-midi-track).

## My EQ or filter settings vanished after a round trip

You saved as `.syx` and loaded it back. The `.syx` format cannot carry HPF2, EQ
bands 2–4 or the tempo-sync links — that is what keeps it compatible with real
hardware. Save a `.mss` for your own work. See
[what saves where](#important-what-saves-where).

## Reporting a problem

The plugin writes a log to:

```
Documents\MEGAN XG\enginelog.txt
```

It records each launch, whether the emulation started cleanly, and details of any
crash. **Include it with a bug report**, along with what you were doing, whether
it was the VST or the standalone, and which DAW.

The file rotates at 1 MB and keeps one previous generation. It costs nothing to
leave alone. For what is in it and how to read it, see
[the engine log](#a5-the-engine-log).

### Where to report, and what a report is for

Reports go to the project's issue tracker on GitHub:
[github.com/bboxdave/megan-xg/issues](https://github.com/bboxdave/megan-xg/issues).

**Please do not read a report as a support request that will be answered.** This is
one person's project, built over many months with AI assistance, released because it
seemed a waste for the work not to exist for anyone else. There is no commitment here
to fix what you report, or to reply, or to keep developing it at all.

File it anyway. A written-up problem is worth more than the promise attached to it:

- It is a permanent, searchable record. The next person to hit the same thing finds
  your issue instead of assuming their machine is broken.
- It is what makes the project **forkable in practice**. The license lets anyone pick
  this up and carry it on; a tracker full of specific, reproducible reports is the
  difference between inheriting a program and inheriting a program plus a map of where
  it is weak. That applies whether the person who picks it up is the original author,
  a contributor, or someone who has never met either.
- Some of what is here has never been exercised properly. The
  [accessibility work](#screen-readers-and-what-works-with-shortcuts-switched-off) is
  the clearest example — it was built to published guidance and verified from the
  inside, but has never been through a session with a real screen reader. Reports are
  the only way that gap gets described accurately by someone who would know.

A good report says what you did, what happened, what you expected, whether it was the
VST or the standalone, which DAW and version, and carries `enginelog.txt`. If it is
reproducible, the steps matter more than anything else in the report.

### If you have forked the project

Two things are worth knowing.

**Point your AI assistant at the log.** If you are working on the code with an AI
coding tool — which is how most of this was built — `enginelog.txt` is the single most
useful file to hand it. It is plain text, it is timestamped and thread-tagged, and it
covers the boundary the tool cannot see by reading the source: what the emulated
machine actually did at runtime, in what order, and how a run ended. A pasted log
turns "it stopped making sound" into a specific line that is missing.
[Appendix A5](#a5-the-engine-log) describes the lines, which is the context the tool
needs to use them.

**Judge a boot by the log, not by the window.** The editor draws itself whether or not
the emulation came up behind it, so a screen that looks right is not evidence. The
line that means the machine is running is in
[what a normal run looks like](#what-a-normal-run-looks-like).
