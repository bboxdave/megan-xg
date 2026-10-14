// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

#include <JuceHeader.h>
#include "Accessibility.h"
#include "KeyNav.h"
#include <array>
#include <memory>
#include <vector>

class YamahaMU100AudioProcessor;
namespace mu100 { class DeviceState; }

//==============================================================================
// GraphicDisplayPanel — XGEdit's "Display Panel" (manual section 6).
//
// Seven modes, picked by the button row across the top:
//   VOLUME / PAN / REVERB / CHORUS / VAR  -> a 33-bar graph (S + 32 parts),
//        each bar a draggable slider for that part's send/level (PAN is centre-
//        anchored: up = right, down = left). The leftmost "S" bar is the system
//        master value (Master Volume / Reverb Return / Chorus Return); Pan and
//        Variation have no system-level value, so their S bar is inert.
//   AMP EG / PITCH EG -> the selected part's envelope, edited by dragging the
//        highlight points. Amp EG points are single-valued (the attack/decay/
//        release time offsets); Pitch EG points are (time, level) pairs.
//
// A bar/point turns yellow while it is being edited and stays yellow if its
// value differs from the factory default. The x-axis "0" labels (parts 10/20/30)
// are drawn orange. The local model is seeded with the factory defaults and
// refreshed from a device-state bulk dump via applyState() (Phase 5 read-back).
//==============================================================================
class GraphicDisplayPanel : public juce::Component,
                            public  juce::TooltipClient,
                            public  keynav::Group,
                            private juce::Timer
{
public:

    // --- keyboard navigation (KeyNav.h) ---
    // Just a bag of standard controls: the generic reading-order collector
    // is exactly right, and it cannot go stale when this panel gains one.
    std::vector<keynav::Item> navItems() override;
    juce::String navGroupName() const override { return "Graphic Display"; }

    explicit GraphicDisplayPanel(YamahaMU100AudioProcessor& proc);
    ~GraphicDisplayPanel() override;

    // Which part the AMP EG / PITCH EG screens edit (0..31). Bar screens edit
    // every part directly and ignore this.
    void setCurrentPart(int part0to31);

    // Switch screens from outside the button row (the Alt+1..9 shortcuts). Named
    // separately from the private setMode so the mode buttons stay the only thing that
    // can reach the internal one, and clamped here because the caller is an index.
    void selectMode(int m);

    // Fired when the user clicks a number on a bar screen's x-axis part ruler, which
    // selects that part exactly as the Voice module's Pt column does (the editor
    // routes both to the same place). 0..31; the "S" column never fires it.
    std::function<void(int part)> onSelectPart;
    // Right-click on the part ruler: the part menu. The ruler is one of only two places
    // in the editor where right-click was not already "reset this control".
    std::function<void(int part)> onPartContextMenu;

    juce::String getTooltip() override;   // the part ruler's "Part N Select"


    // Drum-edit mode: the AMP EG screen retargets the selected drum key's amp
    // envelope (DRUM SETUP EG ATTACK / DECAY1 / DECAY2 rates in setup `setup`).
    // The bar screens stay part-level. active=false returns to part editing.
    // (PITCH EG's inertness is NOT driven from here — it is a property of the
    // part's voice, not of drum-edit mode; see setDrumKitPart.)
    void setDrumEdit(bool active, int note, int setup);

    // Whether the current part's voice is a drum/SFX kit (bank MSB 127 / 126).
    // The MU100 MIDI data book's MULTI PART table states that for a DRUM PART
    // "the following parameters will have no effect", PITCH EG among them (with
    // BANK SELECT LSB, MONO/POLY, SCALE TUNING, PORTAMENTO, FMOD/AMOD DEPTH) —
    // and defines a DRUM PART as PART MODE = DRUM or DRUMS1..4. A kit voice is
    // what drives PART MODE to one of those, so kit-ness is the test, whether or
    // not the part is on an editable DRUMS buffer. The PITCH EG screen therefore
    // goes inert and says so, rather than sending changes the device ignores.
    void setDrumKitPart(bool isKit);
    // The current part's DRUMS buffer (-1 = none). A buffer means the two SHELF handles
    // are dead and belong to the drum key instead -- see eqShelvesDead.
    void setPartDrumBuffer(int buffer);

    // Read-back: refresh the whole local model from a device-state bulk dump
    // (called after a round-trip): the bar modes' system + per-part values, every
    // part's amp/pitch EG, and — while drum-editing — the drum key's amp EG.
    void applyState(const mu100::DeviceState& st);
    // See the .cpp: the AMP EG ghost nodes. Only the amp envelope has them -- the
    // pitch envelope is inert on the parts a controller can reach, and the bars and
    // EQ nodes are second faces on controls that carry their own mark already.
    void refreshMidiOverrides();

    //==========================================================================
    // Two-way value sync with the knob counterparts elsewhere in the editor.
    // Each bar-graph channel edits the SAME device parameter as a knob in the
    // Part / System / Effect modules, so the two displays are kept in lockstep.
    // (AMP EG / PITCH EG have no knob counterpart and are excluded.)
    enum class Channel { Volume, Pan, Reverb, Chorus, Var };

    // Fired when the user edits a synced bar, so the matching knob can mirror the
    // new value. part = -1 is the leftmost "S" (system) bar; 0..31 is that part's
    // bar. Not fired for values pushed in via setExternalBar (those came FROM a
    // knob, so re-notifying would loop).
    std::function<void(Channel ch, int part, int value)> onBarEdited;

    // Mirror an external knob edit into a bar: updates the stored value + repaints
    // WITHOUT sending to the device or firing onBarEdited (the knob already sent
    // it). part = -1 = the "S" bar.
    void setExternalBar(Channel ch, int part, int value);

    // HPF2 live sync with the Part module's HPF2 box (HPF2 is outside the Channel
    // knob-sync system above; both sides share the processor's cleanHpfHz value).
    // onHpf2Edited fires when a user edits an HPF2 bar (so the box can recall);
    // mirrorHpf2 repaints the HPF2 screen after the box edits `part` (value is
    // already in the processor, so this just refreshes the display).
    std::function<void(int part, int hz)> onHpf2Edited;
    void mirrorHpf2(int part, int hz);
    // The HPF2 screen's "S" bar is the SYSTEM HPF, whose box lives in the System
    // module. Same two-way shape as the bars above: this fires when the bar is
    // dragged, mirrorSystemHpf repaints after the box is typed into.
    std::function<void(int hz)> onSystemHpfEdited;
    void mirrorSystemHpf(int hz);

    //==========================================================================
    // EQ screen bridge to the Part module's HPF2 + EQ value boxes (the boxes are
    // the source of truth; see PartPanel::eqRaw). band -1 = HPF2, 0..4 = bands
    // 1..5; row 0 = GAIN, 1 = FREQ, 2 = Q. eqReadRaw returns -1 for absent cells.
    std::function<int(int band, int row)>            eqReadRaw;
    // The DEVICE's raw value for a cell an incoming message has moved, -1 where it
    // has not: the EQ screen's ghost nodes. Only bands 0 and 4 (the hardware shelves)
    // can ever answer -- see PartPanel::eqOverrideRaw.
    std::function<int(int band, int row)>            eqReadOverride;
    std::function<void(int band, int row, int raw)>  eqWriteRaw;
    // Text in and out for ONE EQ cell, answered by the Part module's box for that cell
    // (see PartPanel::eqTextFromValue). The node type-in uses them, so what you type at
    // a handle is parsed exactly as it would be in the box -- including that a shelf's
    // FREQ is an XG table index while a mid band's is Hz.
    std::function<juce::String(int band, int row, double v)>       eqTextOf;
    std::function<double(int band, int row, const juce::String&)>  eqParse;
    // One WHEEL DETENT's worth on that cell, by the value box's OWN law rather than by
    // a raw delta. The keyboard's frequency axis goes through this: a FREQ cell is not
    // the same quantity from band to band (Hz on the freqladder for HPF2 and the mid
    // bands, a coarse XG table index for the shelves), so "+4 raw" would mean four
    // different sizes of step and, on a shelf, four table entries at once. The box
    // already knows its own ladder -- and its wheel is the gesture a keyboard arrow is
    // meant to match. GAIN and Q stay raw deltas: those two ARE plain 0..127 / 1..120
    // scales, and they need a coarse step bigger than one.
    std::function<void(int band, int row, int dir, bool fine)> eqNudgeRaw;
    std::function<void(int band)>                    eqResetBand;
    // Repaint hook: the Part module fires this when a box is edited, so a visible
    // EQ screen mirrors it (the screen reads values live via eqReadRaw).
    void refreshEqIfShown() { if (mode == EQ) repaint(); }
    // Drum-edit corner label: the AMP EG / EQ screens show the drum tone name
    // (not "PART n"). PITCH EG shows its inert notice there instead, that being
    // the more useful thing to say about a screen you cannot edit.
    void setDrumKeyName(const juce::String& name);

    // Variation routing gate (driven by the Effect module's CON box): the
    // variation has a system-level return only when CON = SYS, so the Variation
    // "S" bar is editable only then (and seeded with that return `level`).
    void setVariationSystemActive(bool active, int level);

    // "Disable plugin-only parameters" (the Options toggle). Greys and un-grabs the
    // EQ screen's plugin-only handles (F = HPF2, and bands 2-4) and disables the
    // whole HPF2 mode button, switching off that screen if it is the one showing.
    // The response CURVE still includes those bands: they are still in the audio, and
    // this option blocks editing rather than bypassing anything.
    void setPluginOnlyLocked(bool locked);

    // "Mark DAW-automatable parameters" (XgKnob::AutoMarker value; EditorView has
    // already turned it into MarkerNone where no marker may show). Marks the SCREEN
    // BUTTON of each screen whose edits land on a DAW parameter -- once on the
    // button, like the Part module's EQ GAIN row label, not on every bar or node.
    void setAutomationMarkers(int marker, bool cc);

    void resized() override;
    void paint(juce::Graphics&) override;
    void visibilityChanged() override;
    void parentHierarchyChanged() override;

    // Called when the Display Animation options change, so the analyser and the
    // VOLUME meters pick up a new on/off state (resolution + decay are read live
    // each frame, so those need no nudge).
    void refreshSpectrumOptions();

    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    enum Mode { Volume = 0, Pan, AmpEG, PitchEG, Reverb, Chorus, Var, EQ, HPF2, NumModes };

    // HPF2 is special: a plugin-only per-part clean-HPF cutoff in Hz (0 = Off,
    // 20..1000), read/written via the processor (not an AddressMap param), on a
    // 0..1000 scale rather than 0..127. isHpf2() gates that special handling.
    static bool isHpf2(int m) { return m == HPF2; }
    static constexpr int kHpf2Max = 1000;

    // EQ: the composite-response node editor (F + 5 bands). Not a bar mode, not
    // an EG mode — it has its own paint/interaction path.
    static bool isEqMode(int m) { return m == EQ; }

    static bool isBarMode(int m) { return m != AmpEG && m != PitchEG && m != EQ; }

    // Channel <-> Mode translation for the sync hooks.
    static int     modeForChannel(Channel ch);
    static Channel channelForMode(int m);

    // Whether a mode's leftmost "S" bar is editable: it needs a mapped system
    // param, except Variation (gated on the Effect CON routing) and HPF2, whose S is
    // the plugin's own system HPF and so has no AddressMap index to be found by.
    bool sBarActive(int m) const
    { return m == Var ? varSysActive
           : isHpf2(m) ? true
           : (m >= 0 && m < NumModes && sParamIdx[m] >= 0); }

    // Whether the WHOLE channel (S + every part bar) is disabled — grayed and
    // uneditable. Only Variation, which is inert unless the Effect module routes
    // it to SYS (the per-part VARIATION SEND has no effect when CON=INS).
    bool channelInert(int m) const { return m == Var && ! varSysActive; }

    void setMode(int m);

    // ---- geometry ----
    juce::Rectangle<float> displayRect() const;   // green LCD area (below buttons)
    juce::Rectangle<float> plotRect() const;      // inner plot (minus labels/margins)

    // The left axis-label gutter ("127"/"0", "+12"/"-12", "1k"…) and one label's
    // height. MEASURED from the label font, not fixed: these hold edtext text, and
    // the gutter was a hardcoded 28 px with a 26 px box inside it, so a scaled-up
    // font first touched the plot's left edge — where the EQ's 20 Hz gridline sits
    // exactly — and then clipped. Sized to the widest label any mode draws plus
    // kAxisLabelGap of clearance. Both are floored at their old fixed values, but
    // only the GUTTER actually sits at its floor at 100% (the measurement comes to
    // ~27 < 28, so the default look is untouched and it starts widening ~105%).
    // axisLabelH measures 17 at 100% against a floor of 14 — the old 14 was already
    // clipping a 14.3 px font — so label boxes and the bar inset are a little taller
    // than before by design.
    static constexpr float kAxisLabelGap = 5.0f;   // label -> plot edge clearance
    float axisGutterW() const;
    float axisLabelH()  const;

    // The rect the BARS span (bar modes incl. HPF2): the plot inset at the top by
    // one axis-label height, so a full-scale bar tops out at the BOTTOM of the
    // "127" / "1k" label rather than level with its top. Mirrors the bottom, where
    // 0 already sits at the bottom of the "0" label — and leaves room for the
    // hover/drag readout above a full bar, which the bar itself used to mask at
    // 125..127. Everything that maps a bar value to a y must go through this
    // (paintBars, barVSpan, the centre guide line, HPF2's axis labels).
    juce::Rectangle<float> barPlot(juce::Rectangle<float> plot) const;

    // The x-axis part-number ruler under a bar screen — a SELECTOR, not part of the
    // graph: clicking a number selects that part, and the bar area's drag/wheel
    // handling stops at the plot's bottom edge so the two cannot fight over an event.
    // That edge is the bars' own zero line, so a value of 0..a few still has the whole
    // plot above it to be dragged or wheeled in; nothing editable lives below it.
    //
    // The rect runs from that edge to the BOTTOM OF THE PANEL, not merely the label
    // row's own height: the few pixels of LCD below the numbers would otherwise be a
    // sliver that still starts a bar drag, which is exactly the surprise this split is
    // meant to remove. The labels themselves are drawn in its top kAxisRowH.
    // Empty for the non-bar screens, which draw no ruler.
    static constexpr float kAxisRowH = 18.0f;
    juce::Rectangle<float> axisRowRect() const;
    // Part 0..31 under an x inside that rect; -1 for the "S" column or outside it.
    int partAtAxisX(juce::Point<float> pos) const;

    //== keyboard + screen-reader access to the ENVELOPE nodes =====================
    // The AMP EG and PITCH EG handles are PAINTED, and unlike the EQ nodes and the
    // bars they have no counterpart anywhere else in the editor -- no knob, no value
    // box -- so until they became stops those parameters could not be reached without
    // a mouse at all. That is what made them the first of the plot's handles to get
    // keyboard access; the EQ nodes and the bars mirror controls that already have it.
    //
    // ONE STOP PER NODE, marked ON the node, wherever the value has moved it to. A
    // PITCH EG node carries TWO parameters -- a TIME and a LEVEL, neither of which is
    // "the" value -- so it uses both axes: up/down for the level and `.` / `,` for the
    // time (keynav::Item::adjustX). An AMP EG node is a single segment length and
    // answers to both pairs.
    //
    // An earlier cut split each PITCH EG node into two stops and marked the time one
    // out on its segment, to keep the two marks apart. It was measured as confusing in
    // use -- a mark that is not where the handle is reads as belonging to nothing,
    // especially alongside the mouse -- and one stop per node removes the reason it
    // existed. Do not reintroduce it.
    struct EgStop
    {
        int  level;                  // pitchVal index for the vertical axis, or -1
        int  time;                   // pitchVal/ampSlot index for the horizontal axis
        bool pitch;                  // false = AMP EG (time only)
        juce::Rectangle<int> cell;   // where the mark goes, in panel coords: the NODE
        const char* table;           // for the accessible name (tooltips.csv)
        const char* param;
    };
    std::vector<EgStop> egStops() const;
    int  egRaw(int which, bool pitch) const;
    void egSet(int which, bool pitch, int raw);
    void egReset(const EgStop&);                  // right-click / Backspace: both axes
    juce::String egText(const EgStop&) const;     // what the plot prints by the node
    // Coarse step for BOTH the arrows and the wheel over these handles; Shift = 1.
    static constexpr int kEgStep = 4;
    // Half-extent of a node's keyboard mark, in design px. The focus ring is drawn
    // OUTSIDE it (keynav::ringBounds -> expanded(2), 2 px wide), so it reaches
    // kEgMarkHalf + 4 from the node's centre.
    static constexpr int kEgMarkHalf = 9;
    // How far a node's VALUE READOUT sits from that centre. Derived from the mark
    // rather than from the node square, because the ring is the bigger of the two and
    // the readout has to clear IT: at the node square's own half-width the text ends
    // up underneath the ring, which is what it did until this was measured.
    static constexpr float kEgReadoutGap = (float) kEgMarkHalf + 4.0f + 3.0f;
    // Three AMP EG nodes is the most either screen offers.
    mu100a11y::CellProxy egProxy[3];

    //== keyboard + screen-reader access to the BAR COLUMNS ========================
    // The bar screens (VOLUME / PAN / REVERB / CHORUS / VAR / HPF2) had NO keyboard
    // stops at all until 2026-09-05: `navItems()` offered the mode buttons and the
    // EG/EQ handles and stopped there, which left every per-part volume, pan and send
    // -- the mixing controls, 32 of each -- reachable by mouse alone. One stop per
    // COLUMN closes that; the column IS the control, exactly as a bar is to the mouse.
    //
    // 33 of them: the "S" system column, then parts 1..32, in that order, which is
    // both reading order and part order. The S column is offered only where it is
    // live (sBarActive), and NOTHING is offered on an inert channel (Variation with
    // CON=INS) -- a stop that cannot change anything is a stop that wastes a press.
    struct BarStop { int idx; juce::Rectangle<int> cell; };
    std::vector<BarStop> barStops() const;
    //== the TYPE-IN overlay =======================================================
    // ONE ValueBox for the whole panel, shown over whatever the cursor is on for the
    // length of an edit: a bar column, or an EG / EQ handle. A real ValueBox rather
    // than a bare TextEditor -- it brings the editor, Escape/Return/click-away,
    // setValueNotify (hence undo, the send throttle and the knob mirror) and a screen
    // reader seeing a proper edit box, all of which a second implementation would have
    // to re-earn. Created on first use, hidden whenever nothing is being typed.
    void openTypeIn(juce::Rectangle<int> near, juce::Component* focusBack,
                    double lo, double hi, double cur,
                    std::function<juce::String(double)> fmt,
                    std::function<double(const juce::String&)> parse,
                    std::function<double(double)> snap,
                    std::function<void(double)> apply);
    std::unique_ptr<ValueBox>   typeIn;
    juce::Component*            typeInFocusBack { nullptr };
    std::function<void(double)> typeInApply;
    void beginBarTypeIn(int idx);
    void beginEgTypeIn(const EgStop& st, juce::Component* focusBack);
    // beginEqTypeIn is declared with the EQ block below -- EqStop is defined there.

    //== which axis a multi-value handle types ======================================
    // A node stop carries two or three values (a Pitch EG point is a time and a level;
    // an EQ mid band is a gain, a frequency and a Q), so Enter has to be told WHICH.
    // The answer is the axis you last moved -- by key, or by a decisively one-directional
    // drag -- remembered GLOBALLY per screen kind rather than per node, so it behaves
    // like a mode that follows the cursor rather than 6 handles each with their own
    // memory. Defaults to the VERTICAL axis, which is what an untouched screen offers.
    //
    // Clamped at the point of use, never here: Q is remembered even while the cursor is
    // on a shelf, and beginEqTypeIn falls back to gain for that node without forgetting
    // the Q the next mid band should offer.
    int egTypeAxis { 0 };   // 0 = level (vertical), 1 = time
    int eqTypeAxis { 0 };   // 0 = gain (vertical), 1 = frequency, 2 = Q
    // A drag moves TWO values at once, so it names an axis only when it was decisively
    // one-directional -- this is the ratio, and a diagonal drag leaves the memory alone
    // rather than guessing. (An Alt-drag and the wheel say "Q" outright and skip it.)
    static constexpr float kAxisDominance = 2.0f;
    void         barNudge(int idx, int dir, bool fine);   // the wheel's body; keys share it
    void         barReset(int idx);                        // right-click's body
    juce::String barA11yName(int idx) const;               // what a screen reader calls it
    // How tall a MIDI-override tick is drawn over a bar. Deliberately SHORTER than
    // the VOLUME screen's 5 px setting dash, so the two never read as the same kind
    // of mark where they sit side by side, and short enough that the number it states
    // stays precise. Its visibility is bought in width instead -- see
    // paintBarOverrides.
    static constexpr float kOverrideTickH = 3.0f;

    static constexpr int kBarStops = 33;
    mu100a11y::CellProxy barProxy[kBarStops];

    //== keyboard + screen-reader access to the EQ NODES ===========================
    // The same shape as the envelope nodes above -- ONE STOP PER NODE, marked ON the
    // handle wherever its value has put it -- with one difference that is the whole
    // reason this needed a new axis: an EQ mid-band node carries THREE parameters, not
    // two. Gain, centre frequency and Q are all edited from that one square (the mouse
    // spells the third as an Alt-drag), and none of them is "the" value.
    //
    //   cursor up/down   GAIN, coarse kEqStep raw (0.75 dB) / Shift 1 raw
    //   `.` / `,`        FREQUENCY, one wheel detent of the box's own ladder
    //   `]` / `[`        Q, coarse kEqStep raw (0.4) / Shift 1 raw -- MID BANDS ONLY
    //   backspace        the whole band back to its default, exactly as right-click
    //
    // Node 0 (F = HPF2) has neither a gain nor a Q, so BOTH arrow pairs drive its
    // frequency -- the same accommodation an AMP EG node gets for being one-dimensional,
    // and for the same reason: a cursor that lands on a handle and then ignores the
    // arrow keys reads as broken.
    //
    // Unlike the envelope handles these nodes MIRROR controls that already have their
    // own stops (the Part module's HPF2 + EQ boxes), so nothing here is the only way to
    // reach a parameter. What it is the only way to reach is the RELATIONSHIP: the
    // curve those numbers add up to, which is the reason the screen exists.
    // Node index: 0 = F (HPF2); 1..5 = EQ bands 1..5 (band index = node-1). Declared
    // here rather than with the rest of the EQ screen below because eqProxy is sized by
    // it, and a class member cannot be used before it is declared.
    static constexpr int kEqNodes = 6;
    struct EqStop
    {
        int  node;                   // 0 = F (HPF2); 1..5 = EQ bands 1..5
        bool hasQ;                   // only the plugin-only mid bands (nodes 2-4)
        juce::Rectangle<int> cell;   // where the mark goes, in panel coords: the NODE
    };
    std::vector<EqStop> eqStops() const;
    void beginEqTypeIn(const EqStop& st, juce::Component* focusBack);
    // Coarse raw step for the GAIN and Q axes; Shift = 1. Matches kEgStep, and for the
    // same measured reason -- one step in 127 is a tolerable wheel and an unusable
    // keyboard. (The Part module's own GAIN box still steps 1 per press: that box IS
    // the raw value, while this handle is a position on a curve.)
    static constexpr int kEqStep = 4;
    // Half-extent of a node's keyboard mark, in design px. Derived from the handle
    // square (which scales with the plot) plus a pixel, so the mark stays at or under
    // keynav::kSmallCue and is therefore drawn as a closed RING rather than corner
    // brackets -- right for a handle with no text inside it.
    int   eqMarkHalf(juce::Rectangle<float> plot) const;
    // How far a node's value readout sits from the node's centre. Takes the RING into
    // account, not the square: the ring is drawn outside the mark (keynav::ringBounds
    // -> expanded(2), 2 px wide), so it reaches eqMarkHalf + 4 -- further than the
    // handle does, and the readout has to clear IT. Same derivation as kEgReadoutGap.
    float eqReadoutGap(juce::Rectangle<float> plot) const;
    // One keypress on one axis: 0 = GAIN, 1 = FREQ, 2 = Q. Every write goes through
    // the same eqWriteRaw / eqNudgeRaw bridge the mouse drag uses, so the send
    // throttle, the undo merge and the automation mirror all come along.
    void  eqKeyAdjust(const EqStop&, int axis, int dir, bool fine);
    juce::String eqA11yName(int node) const;    // what a screen reader calls the handle
    mu100a11y::CellProxy eqProxy[kEqNodes];

    struct EgPoint { juce::Point<float> pos; bool editable; };
    // Amp EG: P0(fixed) P1(atk) P2(dcy) P3(fixed) P4(rel)
    std::array<EgPoint, 5> ampPoints(juce::Rectangle<float> plot) const;
    // Pitch EG: A(fixed) B(fixed) P1(left edit) P2(right edit)
    struct PitchGeom { juce::Point<float> a, b, p1, p2; };
    PitchGeom pitchGeom(juce::Rectangle<float> plot) const;

    // ---- painting helpers ----
    void paintBars(juce::Graphics&, juce::Rectangle<float> plot);
    // The MIDI-override ticks over the bars, drawn as their own pass after every
    // column so nothing can bury them.
    void paintBarOverrides(juce::Graphics&, juce::Rectangle<float> plot);
    // The y a VALUE sits at within the bar plot, in the same terms the bar draws it:
    // the top of an upward bar, the far end of a downward PAN bar, the centre line at
    // 64. paintBars and paintBarOverrides SHARE it -- PAN's centre anchor and HPF2's
    // reduced full scale are each stated once, so a tick cannot drift from its bar.
    float barValueY(int m, int val, juce::Rectangle<float> bp) const;
    // Where a shelf's ghost node sits: the device's value on whichever axis was
    // overridden, the user's on the other. Either can be overridden alone -- they are
    // two separate parameters -- so a ghost is never both or nothing.
    juce::Point<float> eqGhostPos(int band, juce::Rectangle<float> plot) const;
    void paintAmpEG(juce::Graphics&, juce::Rectangle<float> plot);
    void paintPitchEG(juce::Graphics&, juce::Rectangle<float> plot);
    void paintEQ(juce::Graphics&, juce::Rectangle<float> plot);
    void paintCornerLabels(juce::Graphics&, juce::Rectangle<float> plot);
    void paintAxisLabels(juce::Graphics&, juce::Rectangle<float> plot);

    // ---- EQ screen (node editor over the 6 HPF2/EQ handles) ----
    // Node index and kEqNodes: with the keyboard block above.
    // On a part with a DRUMS buffer the two SHELF handles (nodes 1 and 5) are dead:
    // the device ignores part-level EQ there (owner's manual p.82) and the per-key DRUM
    // SETUP owns those two instead. The other four -- F (HPF2) and the peaking bands --
    // are the plugin's own per-voice filters and stay LIVE on such a part, stacked over
    // the key layer, so this must gate only the shelves. (Before 2026-09-05 it killed
    // the whole screen; see PluginProcessor.h partShelfEqInert.) In DRUM-EDIT mode all
    // six edit the KEY and none is dead.
    bool  eqShelvesDead() const { return partBuffer >= 0 && ! drumEditActive; }
    // The whole screen is inert only when there is nothing at all to edit: drum-edit
    // mode with no key chosen. A drum-buffer part in part mode is NOT inert any more.
    bool  eqInert() const { return drumEditActive && drumKey < 0; }
    // Is the part this screen is showing EQ-bypassed? Read live from the processor's
    // mask -- the same one the Part module's lamp reads -- so the two can never
    // disagree about it. The nodes stay editable: bypass is not a lock, and setting up
    // a curve while listening dry is a normal thing to want to do.
    bool  eqBypassed() const;   // out of line: the processor is only forward-declared here

    // Node 0 (F = HPF2) and nodes 2-4 (the peaking bands) are the plugin-only ones;
    // nodes 1 and 5 are the hardware XG BASS/TREBLE shelves. Locked nodes are simply
    // dropped from eqNodeAt(), which is the single entry point every interaction
    // (drag, hover, wheel, right-click reset) goes through — so gating it there
    // covers all of them and can't be missed at a new call site.
    static bool eqNodeIsPluginOnly(int node) { return node == 0 || (node >= 2 && node <= 4); }
    // Two independent reasons a handle can't be touched, and they cover disjoint sets:
    // the plugin-only option locks F and 2-4, a DRUMS buffer kills the shelves 1 and 5.
    bool  eqNodeLocked(int node) const
    {
        return (pluginOnlyLocked && eqNodeIsPluginOnly(node))
            || (eqShelvesDead()  && ! eqNodeIsPluginOnly(node));
    }

    // PITCH EG has no effect on a drum part (see setDrumKitPart). drumEditActive
    // implies a kit part, but is kept as a belt-and-braces term: a read-back can
    // set the drum context before the part's voice resolves.
    bool  pitchEgInert() const { return drumKitPart || drumEditActive; }
    bool  eqNodeOn(int node) const;                     // filled (contributes to the curve)
    // The cutoff the F node's filter ACTUALLY runs at: the part's own HPF2, or the
    // system floor when that is higher (swp30 runs one filter at max of the two).
    // PART MODE ONLY -- see the note at its definition.
    int   eqEffectiveHpfHz() const;
    int   eqBand(int node) const { return node - 1; }   // -1 for the HPF2 node
    float eqFreqHzOf(int node) const;                   // node centre frequency (Hz)
    float eqGainDbOf(int node) const;                   // node gain (dB); 0 for HPF2
    float freqToX(float hz, juce::Rectangle<float> plot) const;
    float xToFreq(float x,  juce::Rectangle<float> plot) const;
    float gainToY(float db, juce::Rectangle<float> plot) const;
    float eqGainPad(juce::Rectangle<float> plot) const;   // top/bottom inset so the rails clear the labels

    // "RANGE: ±12 ±24" selector in the EQ display's bottom-left margin. paint()
    // draws it and mouseDown() hit-tests it, so both take their geometry from here.
    void eqRangeLayout(juce::Rectangle<float>& label,
                       juce::Rectangle<float>& opt12,
                       juce::Rectangle<float>& opt24) const;
    void paintEqRange(juce::Graphics&);
    // U+00B1 PLUS-MINUS, built from its code point rather than typed into the source
    // so it can't depend on how the compiler reads this file's encoding.
    static juce::String pm() { return juce::String::charToString((juce::juce_wchar) 0x00B1); }
    juce::Point<float> eqNodePos(int node, juce::Rectangle<float> plot) const;
    int   eqNodeAt(juce::Point<float> pos, juce::Rectangle<float> plot) const;  // nearest handle, or -1
    float eqSquareSize(juce::Rectangle<float> plot) const;
    juce::String eqReadout(int node) const;             // hover/drag value text
    void  captureEqDragStart(int node);                 // seed dragStartVal/2/Q from the boxes

    // ---- value model + sending ----
    int  barValue(int mode, int idx) const;       // idx 0 = S, 1..32 = part
    void setBarValue(int mode, int idx, int v, bool send);
    bool barIsDefault(int mode, int idx) const;
    juce::String barReadoutText(int mode, int val) const;

    // ---- the spoken half of the MIDI-override marks ----
    // Each returns the DEVICE's reading for a painted stop, or "" when nothing has
    // moved it. A ghost node and a tick over a bar are marks with no text at all, so
    // without these the override is invisible AND inaudible on every painted surface
    // -- the value boxes and knobs carry theirs in ValueBox::spokenText. Folded into
    // the proxy's spoken value by navItems(), through mu100a11y::withOverride, which
    // is where the one wording lives.
    juce::String egOverrideText(const EgStop&) const;
    juce::String barOverrideText(int idx) const;
    juce::String eqOverrideText(int node) const;

    // ---- bar-value readout (hover + adjust) ----
    int  columnAtX(juce::Point<float> pos, juce::Rectangle<float> plot) const;
    void barVSpan(int idx, juce::Rectangle<float> plot, float& topY, float& botY) const;
    int  barHoverIdx(juce::Point<float> pos, juce::Rectangle<float> plot) const;  // filled bar under cursor, or -1
    bool shouldShowReadout(int i) const;
    void startHoverDelay(int idx);
    void startTransient(int idx);

    void timerCallback() override;                // hides the wheel value readout

    // ---- EQ animated spectrum (the thin live analyser line under the curve) ----
    // The panel's own juce::Timer is a ONE-SHOT multiplexed through timerJob
    // (hover delay / transient hide), so the analyser cannot share it — starting a
    // repeating tick there would cancel a pending hover readout and vice versa.
    // A separate member timer keeps the two independent without re-signing every
    // existing startTimer/timerCallback site (which is what MultiTimer would cost).
    struct SpectrumTick : juce::Timer
    {
        std::function<void()> fn;
        void timerCallback() override { if (fn) fn(); }
    };
    SpectrumTick spectrumTimer;

    void updateSpectrumTarget();   // (re)decide what the probe watches + run/stop the tick
    void spectrumFrame();          // one capture -> FFT -> log-bin -> smooth -> repaint
    bool spectrumActive() const;   // enabled && EQ shown && something to analyse
    // dBFS -> y. DELIBERATELY NOT gainToY: that maps EQ GAIN against the ±12/±24
    // RANGE zoom, while this maps absolute LEVEL over a fixed floor..0 dBFS span.
    // They are different quantities, so the RANGE selector must not move the
    // spectrum (it doesn't in Bitwig either). Do not "unify" them.
    float specToY(float db, juce::Rectangle<float> plot) const;

    // ---- VOLUME screen level meters (the animated bars) --------------------
    // With the option on, the VOLUME screen stops drawing solid bars: each part's
    // SETTING becomes a thin dash at the height the solid bar used to reach, and the
    // space below it is filled with that part's live output level, green at the
    // bottom through yellow and orange to red at the top. The meter is CLAMPED to the
    // underside of the dash, so it can neither exceed the setting indicator nor paint
    // over it — the setting stays readable at a glance while the level moves.
    //
    // Mouse behaviour is deliberately untouched: hit-testing goes through barVSpan /
    // barHoverIdx, which read the SETTING, so dragging, hovering and the readout all
    // behave exactly as they do with solid bars.
    //
    // Levels arrive as linear amplitude (1.0 = the main output's full scale) and are
    // held as dB, because that is the only scale on which quiet parts move visibly.
    // Attack is instant, decay is derived from real elapsed ms exactly as the
    // analyser's is — see spectrumFrame().
    // (SpectrumTick, declared above for the analyser, is reused for the meter's own
    // tick — they run at different rates and must not share a timer.)
    bool  metersActive() const;                     // enabled && VOLUME shown
    void  updateMeterTarget();                      // (re)arm the probe + run/stop the tick
    void  meterFrame();                             // one read -> dB -> smooth -> repaint
    float meterFrac(int idx) const;                 // 0..1 of the bar span, 0 = nothing
    void  paintMeterBar(juce::Graphics&, juce::Rectangle<float> bp,
                        float x, float barW, float settingTopY, int idx);

    static constexpr int   kMeterFps     = 30;
    static constexpr float kMeterFloorDb = -60.0f;  // bottom of the bar
    static constexpr float kMeterTopDb   =   0.0f;  // top (full scale)
    // Thickness of the setting dash. The user's reference is the MIDI input
    // indicators' width, and taking midiind::kW rather than a literal is what keeps
    // the two the same weight if either is ever retuned — the same reasoning that
    // header already applies to the Voice tabs and the Keyboard lamps.
    static float meterDashH();

    // meterDb[0] is the "S" bar (the plugin's own main output); 1..32 are the parts.
    std::array<float, 33> meterDb;
    bool          meterHasData { false };
    juce::uint32  meterLastMs  { 0 };
    SpectrumTick  meterTimer;

    static constexpr float kSpecFloorDb = -90.0f;   // bottom of the plot
    static constexpr float kSpecTopDb   =   0.0f;   // top of the plot (full scale)
    static constexpr int   kSpecFps     = 20;

    std::unique_ptr<juce::dsp::FFT>                     specFft;
    std::unique_ptr<juce::dsp::WindowingFunction<float>> specWindow;
    int                 specOrder { 0 };        // order specFft/specWindow were built for
    std::vector<float>  specFifo;               // 2*N scratch for performFrequencyOnly…
    std::vector<float>  specDb;                 // smoothed dB, ONE PER PIXEL COLUMN of plotRect
    bool                specHasData { false };  // false = nothing captured yet, draw nothing
    juce::uint32        specLastMs { 0 };       // for the frame-rate-independent decay

    void sendBar(int mode, int idx, int v);
    void sendAmp(int which, int v);               // which 0=atk 1=dcy 2=rel
    void sendPitch(int which, int v);             // which: param index in pitchParam
    void loadDrumAmp();                           // read the drum key's amp-EG rates
    void loadBars(const mu100::DeviceState& st);  // read bar modes' S + per-part values
    void loadPartEG(const mu100::DeviceState& st);// read every part's amp/pitch EG

    // AMP EG working value for slot `which` (0=atk 1=dcy 2=rel): the selected
    // drum key's value in drum-edit mode, otherwise the current part's. The const
    // overload (returns by value) serves the const geometry/paint helpers.
    int& ampSlot(int which) { return drumEditActive ? drumAmpVal[which]
                                                    : ampVal[currentPart][which]; }
    // The value the node loads with: a MULTI PART envelope's static 64, or the drum
    // key's own kit default. Anything comparing against "the default" must use this.
    int  ampBaseline(int which) const { return drumEditActive ? drumAmpDflt[which] : 64; }
    int  ampSlot(int which) const { return drumEditActive ? drumAmpVal[which]
                                                          : ampVal[currentPart][which]; }

    YamahaMU100AudioProcessor& audioProcessor;

    // A screen button that can carry the "Mark DAW-automatable parameters" marker.
    // Drawn by the LookAndFeel exactly as before while unmarked; see paintButton in
    // the .cpp for why a marked one lays its own text out.
    class ScreenButton final : public mu100a11y::FocusableButton
    {
    public:
        using FocusableButton::FocusableButton;
        void setAutomationMarker(int marker);   // XgKnob::AutoMarker -- the DAW triangle
        void setCcMarker(bool on);              // the MIDI CC underline
        void paintButton(juce::Graphics&, bool highlighted, bool down) override;
    private:
        int  autoMarker { 0 };
        bool ccMarker   { false };
    };

    std::array<std::unique_ptr<ScreenButton>, NumModes> buttons;

    // "Mark DAW-automatable parameters": the style EditorView hands down (already
    // MarkerNone in the standalone or with DAW automation off), and the per-screen
    // decision, re-made whenever drum-edit mode or the plugin-only lock changes.
    int  automationMarker { 0 };
    bool ccMarks { false };          // draw the MIDI CC underline at all (EditorView)
    bool screenIsAutomated(int m) const;
    bool screenSendsMidiCc(int m) const;
    void updateScreenMarkers();
    int mode { Volume };
    int currentPart { 0 };
    bool pluginOnlyLocked { false };   // see setPluginOnlyLocked

    // Local model. Bar modes use sVal/partVal (indexed by Mode); EG modes use the
    // per-part arrays.
    int sVal[NumModes];
    int partVal[NumModes][32];
    int ampVal[32][3];      // attack, decay, release  (raw 0..127, 64 = 0)
    int pitchVal[32][4];    // initial level, attack time, release level, release time

    // Resolved AddressMap param indices (-1 if unmapped/inert).
    int partParamIdx[NumModes];
    int sParamIdx[NumModes];
    int ampParam[3];
    // The DEVICE's amp-envelope values where incoming MIDI has moved them, -1 where
    // it has not. Refreshed with the rest of the override marks rather than asked
    // per paint: this panel repaints continuously while an animation is running.
    int ampOverride[3] { -1, -1, -1 };
    // The same, for the BAR screens: the device's value per column (0 = S, 1..32 =
    // parts), -1 where the device agrees with the model. Every bar mode is kept
    // current, not just the one on screen, so switching screens needs no recompute
    // -- the whole table is 6 modes x 33 lookups on an override change, and the
    // screen repaints continuously while the meters run.
    int barOverride[NumModes][kBarStops];
    // ...and for the EQ screen's two hardware shelves, [band][0 = GAIN, 1 = FREQ].
    // Bands 1..3 are plugin-only and stay -1 forever; they are carried anyway so the
    // array is indexed by band with no mapping to get wrong.
    int eqOverride[5][2];
    // The ghost envelope's node positions, computed from the device's values where
    // they differ and the model's where they do not. A separate point set because
    // the segments are CUMULATIVE -- an overridden attack moves every later node,
    // so a ghost cannot be one substituted point.
    std::array<juce::Point<float>, 3> ampGhostPoints(juce::Rectangle<float> plot) const;
    int pitchParam[4];

    // Variation "S" bar gate: true when the variation effect is routed to SYS
    // (set by the Effect module via setVariationSystemActive); the system return
    // value itself lives in sVal[Var].
    bool varSysActive { false };

    // Drum-edit mode: AMP EG -> DRUM SETUP amp rates for drumKey in drumSetup.
    bool drumEditActive { false };
    bool drumKitPart    { false };   // current part's voice is a drum/SFX kit
    int  drumKey   { -1 };
    int  drumSetup { 0 };
    int  partBuffer { -1 };   // current part's DRUMS buffer, -1 = none
    int  drumAmpParam[3] { -1, -1, -1 };
    int  drumAmpVal[3]   { 64, 64, 64 };   // working model (no drum read-back yet)
    // What each drum amp node LOADS WITH under the current kit, in the same inverted
    // units as drumAmpVal. The readout and the "changed" marker are both measured from
    // this, not from a hardcoded 64 -- see loadDrumAmp.
    int  drumAmpDflt[3]  { 64, 64, 64 };

    // Drag state.
    bool dragging { false };
    int  dragIdx { -1 };          // bar 0..32, EG point id, or EQ node 0..5

    // An undo gesture held open for the length of a drag that moves MORE THAN ONE
    // parameter — an EQ node (frequency horizontally, gain or Q vertically) and a
    // PITCH EG point (time and level). Those write two different addresses, which the
    // address-keyed merge policy correctly treats as two edits, so one drag of one
    // handle otherwise cost two undo steps and undoing it left the node halfway.
    // Single-parameter drags (bars, AMP EG) open none: the address merge already makes
    // them one step, and a gesture would replace their generated
    // "Part 5 VOLUME 100 -> 64" label with a fixed string.
    //
    // Explicit rather than RAII because it spans two EVENTS, so there is no enclosing
    // block — and because a drag can also be cancelled without a mouse-up (a mode or
    // kit switch clears `dragging`). endDragUndo() is therefore called from every
    // place that drops a drag, and is a no-op when none is open.
    bool dragUndoOpen { false };
    void beginDragUndo(const juce::String& label);
    void endDragUndo();

    int  dragStartVal { 0 };
    int  dragStartVal2 { 0 };     // pitch EG level, or EQ node gain
    int  dragStartVal3 { 0 };     // EQ node Q (raw 1..120)
    juce::Point<int> dragStartPos;

    // EQ screen: drum tone name for the corner label + the handle the cursor is
    // resting on (readout shown after the same hover delay the bars use).
    juce::String drumKeyName;
    int  eqHoverNode { -1 };

    // EQ screen's vertical display range, ± dB: 12 (default) or 24. A VIEW zoom
    // only — a band's GAIN still spans ±12 dB (kEqBandGainMaxDb). ±24 halves the
    // node travel but reveals composite peaks that overlapping bands push past 12 dB,
    // which the ±12 view has to clip away. Not persisted: a per-session view setting,
    // like `mode` itself.
    int  eqRangeDb { 12 };

    // Bar-mode value readout state. The readout (drawn at its existing position by
    // paintBars) is shown for a column when: it is being dragged; the cursor hovers
    // the filled bar (after a short delay, persisting until the cursor leaves it);
    // or briefly after an off-bar (above/below) wheel/drag adjust.
    int  hoverIdx { -1 };        // column whose filled bar the cursor is over
    bool hoverShown { false };   // hover delay elapsed -> readout visible
    int  transientIdx { -1 };    // off-bar adjust readout, hidden by the timer
    enum TimerJob { TimerNone, TimerHoverDelay, TimerTransientHide };
    TimerJob timerJob { TimerNone };
    static constexpr int kHoverDelayMs = 150;
    static constexpr int kTransientMs  = 500;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GraphicDisplayPanel)
};
