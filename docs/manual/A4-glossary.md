# A4 Glossary

Terms as this manual uses them. Definitions marked *(Yamaha)* are adapted from the
MU100 owner's manual.

**Aftertouch** — pressure applied to a key after it is down. *Channel* aftertouch
is one value for the whole keyboard; *poly* aftertouch is per key, and few
keyboards send it.

**AWM2** *(Yamaha)* — Advanced Wave Memory 2, the MU100's tone generation system:
sampled waveforms shaped by digital filters and envelopes.

**Bank** — a set of up to 128 voices. A voice is selected by a bank number (in two
parts, MSB and LSB) plus a program number. *(Yamaha)*

**Bulk dump** — a complete set of settings sent as one block of MIDI
system-exclusive data. How a module's whole state is saved to a file or moved
between devices.

**CC** — control change, the family of MIDI messages that carry continuous
controllers: CC1 is the mod wheel, CC7 volume, CC11 expression, CC64 sustain.

**Drum buffer** — one of four shared stores of per-key drum settings, DRUMS1 to
DRUMS4. A kit belongs to a buffer, and every part assigned to that buffer plays it.
See [Drum editing](#the-four-drum-buffers-the-thing-to-understand).

**EG** — envelope generator, the shape of a sound over time. The MU100 has one for
level (AMP EG) and one for pitch (PITCH EG). *(Yamaha)*

**Element** — one layer of a voice. Some voices use two elements, and use two
notes of polyphony as a result.

**Element reserve** — the number of notes guaranteed to a part when the module runs
short of polyphony.

**Filter** — a control that cuts or boosts part of the frequency range. *Low-pass*
lets low frequencies through and removes highs — the brightness control.
*High-pass* is the reverse. *(Yamaha)*

**Insertion effect** — an effect patched into one part's signal path, so the part
passes *through* it. The right choice for distortion, amp simulation, wah and
rotary speaker. Compare *send effect*.

**LFO** — low frequency oscillator: a slow, regular wobble used to modulate pitch
(vibrato), level (tremolo) or a filter. *(Yamaha)*

**MSB / LSB** — most and least significant byte. Bank selection uses both; the MSB
is what distinguishes drum kits (126 and 127) from melodic voices.

**Multi-timbral** — able to play several different sounds at once, on different
MIDI channels. The MU100 is 32-part multi-timbral. *(Yamaha)*

**NRPN** — non-registered parameter number: a way of reaching parameters that have
no dedicated CC.

**Part** — one of the module's 32 independent instrument slots, each with its own
voice, MIDI channel and settings. *(Yamaha)*

**PDC** — plugin delay compensation. A DAW shifting a plugin's events earlier so
that its output lands in time with everything else.

**Performance** — an operating mode of the MU100 in which four parts are layered
under one MIDI channel. Reachable through the
[Hardware view](#17-the-hardware-view) only. *(Yamaha)*

**Polyphony** — how many notes can sound at once. The MU100 has 64, shared across
all parts. *(Yamaha)*

**Port** — MIDI addresses 16 channels, so more than 16 parts need more than one
port. The MU100 has two, A and B. *(Yamaha)*

**Portamento** — a continuous glide in pitch between successive notes. *(Yamaha)*

**Program change** — the MIDI message that selects a voice. On a drum part it also
re-initializes that part's drum setup.

**Q** — the width of a peaking EQ band. Low Q is a broad, gentle hill; high Q a
narrow spike.

**Send effect** — an effect fed from a *send*, with its output returned to the mix
alongside the untreated sound. The MU100's reverb and chorus work this way, shared
by all parts. Compare *insertion effect*.

**Send / return** *(Yamaha)* — *send* is how much of a part goes into an effect;
*return* is how much of the effect's output comes back into the mix.

**Shelf** — an EQ band that lifts or cuts everything above (high shelf) or below
(low shelf) its frequency, rather than a band around it.

**SW1000XG** — Yamaha's PCI sound card built around the same XG tone generator
family. Setups made for it are largely interchangeable with MU-series setups.

**SysEx** — system-exclusive MIDI data: manufacturer-specific messages. Everything
the editor sends to the sound engine is XG SysEx, which is also why a whole setup
can be recorded onto a MIDI track.

**Tone generator** — a sound module with no keyboard of its own, played from a
keyboard or a computer. *(Yamaha)*

**Variation** — the MU100's third system effect section, which can also be switched
to work on a single part as an insertion. *(Yamaha)*

**Velocity** — how hard a note was played. *(Yamaha)*

**Voice** — one instrument sound. The MU100 has over 1 200. *(Yamaha)*

**XG** — Yamaha's extension of General MIDI: more voices, more effects, and a
defined set of editable parameters reached over SysEx. The reason a SysEx setup
saved from XGEdit twenty years ago still loads here. *(Yamaha)*

**XGedit95** — Gary Gregson's XG editor for Windows, the program this editor's
layout follows. MEGAN XG reads the XG **bulk dumps** (`.syx`) XGEdit can save; it
does not read XGEdit's own native setup files.
