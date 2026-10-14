# MEGAN XG — User Manual

Version 0.9.0 · VST3 and Standalone · Windows 64-bit

Last updated {{LAST_UPDATED}}

This manual is licensed under
[Creative Commons Attribution-ShareAlike 4.0 International (CC BY-SA 4.0)](https://creativecommons.org/licenses/by-sa/4.0/).
The plugin itself is free software under the GNU Affero General Public License v3.

## What this is

MEGAN XG is an emulation of a **Yamaha MU100 tone generator**, running as a VST3
instrument in your DAW or as a standalone application, together with a
**full-screen parameter editor** in the style of XGEdit95 — with numerous changes
and additions. The name is an acronym: **M**ultitimbral **E**ditor,
**G**enerator, **A**udio **N**exus for **XG** MIDI.

Two things are bundled into one window:

- **The sound module.** The original MU100 firmware and sample ROMs run inside the
  plugin, so voices, effects and the way parameters interact are the device's own
  behavior rather than an approximation of it. It answers to XG SysEx, responds on
  32 parts across two MIDI ports, and sounds like the box. The chip it all runs on
  was never documented publicly, though, so a great deal of the emulation is
  **reverse engineered**: some of it is an approximation of the hardware, and a few
  things remain unresolved. See
  [differences from the hardware](#21-differences-from-the-mu100-hardware).
- **The editor.** Every parameter you would otherwise reach by scrolling through
  the module's LCD is laid out on one screen — a channel strip per part, the five
  effect units and their parameters, controller assignments, the drum kit editor,
  a graphic display for levels and envelopes, and a 32-part overview.

There is a third thing in the window, worth mentioning this early because it is
what the plugin opens in: a **Hardware view**, a picture of the module's front
panel with a working LCD and buttons that drive the real firmware. It is there for
the few things the editor does not cover. Its display is **not a faithful
reproduction** — it still has a few bugs that make it differ slightly from the
real hardware's screen, and some elements of that screen are missing. Treat it as
a way to reach functions, not as a replica. See
[the Hardware view](#17-the-hardware-view).

If you have used XGEdit, XGgold, or one of the other XG editors of that era, the
editor screen will be immediately familiar; it is deliberately modeled on XGEdit's
layout. The important difference is that here the editor and the sound module are
the *same program*, so there is no MIDI cable, no loopback driver, and no question
of the two getting out of step.

## Why this was created

Many years ago I bought an SW1000XG sound card and built a lot of projects around
it. As often happens with computer music hardware, it gradually stopped being
compatible with newer computers, and the manufacturer stopped supporting it. My
old, half-finished projects got harder and harder to work on.

So I bought a tone generator module that was compatible with those projects and
with the editing software I had been using — Gary Gregson's excellent XGEdit95.
That bought me some time. But once its author had stopped developing and
supporting it, that too became more of a struggle to use.

It made sense that the manufacturer and the programmer had to let these products
go; they were surely no longer commercially viable. I still wanted to work on my
old projects.

So, more than two decades after the hardware and the software were abandoned, I took
matters into my own hands. I used AI to help me build something that would work
on the computer I have now, and hopefully on the ones I will have later.

Inspired by someone else — Sojus Records — who had built a DAW-compatible plugin
on top of a MAME emulation, I set to work on MAME's MU100 emulation. I worked out
many of the remaining audio processing glitches and turned it into a VST3 and a
standalone application. I built a new JUCE editor view in the layout style of
XGEdit95, the editor I knew. And I added features that are useful to the way I
work.

Because it took many hours over many months — even with AI — and because it
builds on, or was inspired by, the work of others, I wanted other people to have
it too. There are probably plenty of others in the same boat I was in, with old
projects they would like to work on and finding it harder every year.

I have released this project so that the work of the people who made some
wonderful tools can live on, and so that a few more musicians' old projects can
find a second life.

## Who it is for

The manual assumes you are comfortable with the ideas a hardware XG module trades
in — parts, banks and programs, MIDI channels, sends and returns — but not that you
know anything about programming or audio engineering theory. Where a term is
unavoidable it is explained the first time it appears, and again in the
[glossary](#a4-glossary).

Three kinds of reader were in mind:

- **You already have MU100, SW1000XG, or other compatible XG module material** —
  `.syx` bulk dumps, including setups you saved out of XGEdit in that form, or
  MIDI files with XG SysEx in them — and you want to open it, hear it, and keep
  working. (It has to be a `.syx` dump: XGEdit's own native setup files are not
  read. See [XG bulk dump](#xg-bulk-dump-syx-for-everything-else).) Start with
  [Getting Started](#1-getting-started), then
  [Working with existing XG material](#loading-a-syx-file-from-xgedit-or-hardware).

  Material from some earlier models such as the MU80 and MU90 is likely to be
  compatible, but has not been tested. While files from the MU128 and later XG
  models may be largely compatible, they may not entirely be so — those machines
  added voices, polyphony and other features this one does not have, and anything
  that reaches for them will not come across.
- **You have the hardware and want the same thing in your DAW.** The chapter on
  [differences from the hardware](#21-differences-from-the-mu100-hardware) is the
  one to read early; it lists what the plugin adds, and the few things it does not
  do.
- **You have never touched an XG module.** Read chapters 1 to 3, then dip into the
  module chapters as you meet each part of the screen. The
  [effects overview](#understanding-the-effect-units) is worth reading before you
  touch any of the effect controls.

## How to read this manual

Chapters 4 to 13 walk through the editor one module at a time, in roughly the order
you meet them on screen. They are reference material — read the module chapter when
you want to know what a particular control does.

Chapter 3, **Controls**, is the one chapter worth reading properly before anything
else. Every knob and field in the editor obeys the same handful of mouse rules, and
learning them once makes the rest of the program obvious.

Chapter 20, **Workflows**, is the opposite: task-oriented recipes for the things
people actually sit down to do.

Throughout, three kinds of note are called out:

> **Standalone only** — applies when you run the plugin as its own application, not
> inside a DAW.

> **VST only** — applies when the plugin is loaded in a DAW. The standalone build
> either does not have the feature or has no use for it.

> **Watch out** — a place where the plugin will do something correct but
> surprising, or where you can lose work.

## A note on legality and origins

The plugin contains no Yamaha code or samples. It requires **ROM files from an
MU100 that you must supply yourself** — see
[ROM files](#rom-files-what-you-need-and-where-to-put-them). Yamaha, MU100, XG and
SW1000XG are trademarks of Yamaha Corporation, used here only to say what the
plugin emulates.

The editor's layout, and much of its hover text, follow **XGedit95 by Gary
Gregson**, one of the programs that defined what an XG editor looks like. This manual
borrows its structure too.

The editor's accessibility work — the names, roles and spoken values every control
reports, and the keyboard access underneath them — follows **joeloftusdev**'s
published guide to making JUCE plugins accessible, and the **Surge Synth Team**
plugins it cites as models of those guidelines in practice. The guidance is what was
followed; no code was taken from either.

What is here is a **foundation rather than a finished job**: it has never been tested
with a screen reader, and it may well need significant work before it properly serves
someone who depends on it. See
[Screen readers](#screen-readers-and-what-works-with-shortcuts-switched-off).
