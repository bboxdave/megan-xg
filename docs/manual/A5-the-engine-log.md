# A5 The engine log

The plugin keeps a plain-text log of what the emulated machine does. It lives beside
`settings.xml`:

```
Documents\MEGAN XG\enginelog.txt
```

There is nothing to switch on and nothing to switch off. A launch that goes well writes
about a dozen lines, and the cost is invisible.

This appendix exists for two readers: anyone attaching the file to a
[bug report](#reporting-a-problem) who would like to know what they are sending, and
anyone working on the source who wants to use it as an instrument. If you are just
using the plugin you never need to open it.

## What is in the file, and what is not

The log records the *machine*, not your music. Nothing you play, no note, no parameter
value and no file you have loaded goes into it.

What it does contain is worth knowing before you post it in public:

| | |
|---|---|
| Your Windows user name | inside the full paths it prints — the plugin binary, the temporary working folder |
| The names of your MIDI input devices | one line per port opened, as Windows reports them |
| The DAW you were running | as the full path of the host executable |
| Version and build details | the plugin's module name and where Windows loaded it |

None of it is secret, but it is more than a bare error message, so read it before you
attach it if that matters to you. It is a text file; anything you would rather not
post can be deleted from your copy.

## One file, several instances

**A DAW loads every instance of the plugin into one process, and they all write to the
same log.** That is deliberate — an interaction between two instances is exactly the
kind of thing worth having in one timeline — but it means the file is interleaved
rather than sequential, and you separate the strands by the thread id in each line.

## The line format

Every line starts with the same three fields:

```
20:20:56.677 +   922ms t4012  machine RUNNING (mameIsFullyBooted=true)
```

| | |
|---|---|
| `20:20:56.677` | local wall-clock time, to the millisecond |
| `+   922ms` | milliseconds since the **first** instance in this process started logging. This is the column to read when you care about *ordering* and *gaps*, because it does not jump at midnight and does not depend on the clock |
| `t4012` | the Windows thread id. The interface, each instance's emulation thread, and the audio callback are all different numbers, and this is how you tell whose line you are looking at |

## What a normal run looks like

A clean launch writes roughly this, in this order:

| Line | What it means |
|---|---|
| `======== RUN START pid= exe=` | the process started logging. The `exe=` is your DAW, or the standalone |
| `module= base=` | where Windows loaded the plugin binary. Only interesting for decoding a crash — see below |
| `processor ctor wrapper=` | an instance was created. One of these per instance, so counting them counts instances |
| `prepareToPlay sr=` | the host handed over its sample rate |
| `midi-in open OK :` | one line per MIDI input port opened, naming the device. A failure says so on the same line |
| `engine thread start` | the emulation's own thread began |
| `MAME -samplerate` | the rate the emulator was started at |
| `engine thread: entering ...  workdir=` | the emulator is being launched, from a temporary folder created for this run |
| `osd init: machine= phase=` | the emulated machine object exists |
| **`machine RUNNING`** | **the emulation is actually up.** This is the line that matters |
| `engine thread exit`, `... execute() returned`, `audio totals:`, `midi totals:` | the run ended, and how much it moved |

**`machine RUNNING` is the test for a healthy boot, and it is the only test.** The
editor window draws itself from the plugin's own copy of the settings, so it looks
entirely normal with no emulation behind it — a screen that reads right is not
evidence of anything. If ROM files are missing or wrong, if a path is bad, or if the
emulator rejected something, the log stops before that line and usually says why.

Messages from the emulator itself appear prefixed `mame[...]`, which is where an
explanation of a refused boot normally turns up. They are capped per run so a noisy
build cannot turn the log into the slowest thing on the machine.

## A crash

A fault inside the emulation is caught and written out before anything else happens,
beginning:

```
!!!! MAME ENGINE FAULT  code=0x...  flags=0x...  at ...
```

followed by the address that was touched, what the process thought was mapped there,
and a numbered stack of frames, each printed as a module name plus an offset:

```
     #0  mu100.dll                    +0x0123abcd   (0x00007ff8...)
```

Those offsets are the useful part, and they are **only** meaningful against the
debugging symbols for the exact build that produced them. A frame list from one build
decoded against another gives confident, wrong answers.

If you are reporting a crash, the whole block matters — the frames are not readable
without the `code=` line above them, and the `module= base=` header near the top of
the file is what ties the numbers to a binary.

## Rotation

The file rotates when it passes 1 MB, which is a few hundred launches. The previous
generation is kept as `enginelog.txt.old` and the new file starts empty.

**So if the thing you want to report already scrolled past, look in `.old` before you
give up.** Nothing here is ever deleted without one generation of warning, and neither
file is needed by the plugin — deleting both is harmless, and a fresh log starting at
the launch that went wrong is often easier to read than a long one.
