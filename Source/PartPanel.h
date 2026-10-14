// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

#include <JuceHeader.h>
#include "KeyNav.h"
#include <vector>
#include <memory>
#include "DotToggle.h"

class YamahaMU100AudioProcessor;
class XgKnob;
class ValueBox;
namespace mu100 { class DeviceState; }

//==============================================================================
// PartPanel — the XGEdit "Part" module. Edits the currently-selected XG part
// (1..32); every control transmits the matching MULTI PART parameter change.
//
//   * main knob row : Volume / Detune / Note-shift / Pan / Velocity / Vibrato /
//                     Filter (LPF) / effect sends / Dry
//   * EQ grid       : 3 rows (GAIN/FREQ/Q) x 5 bands. Bands 1 & 5 are the hardware
//                     XG BASS/TREBLE shelves; bands 2-4 are plugin-only peaking EQ
//                     (proc.setPartEq*, per voice in swp30 — like HPF2). Plus HPF2.
//   * enum controls : Mono/Poly, Assign (Same-Note), Portamento switch + Time
//
// HPF cutoff lives in the separate per-part "0A nn 20" block (not the main 08
// block), so it is a normal SysEx param that reads back from the bulk dump like
// the rest.
//==============================================================================
class PartPanel : public juce::Component,
                  public keynav::Group
{
public:

    // --- keyboard navigation (KeyNav.h) ---
    // Just a bag of standard controls: the generic reading-order collector
    // is exactly right, and it cannot go stale when this panel gains one.
    std::vector<keynav::Item> navItems() override { return keynav::collect(*this); }
    juce::String navGroupName() const override { return "Part"; }

    explicit PartPanel(YamahaMU100AudioProcessor& proc);
    ~PartPanel() override;

    // Select which XG part (0..31) the controls edit. Also updates the selector.
    void setCurrentPart(int part0to31);
    // The EQ BYPASS lamp and the "EQ - BYPASSED" bracket, refreshed from the
    // processor's live mask -- whichever surface moved the bit (this lamp, the
    // keyboard shortcut, or the bottom bar's clear-all).
    void refreshEqBypass();
    // The EQ BYP lamp's tooltip, which names WHY the part is bypassed on a drum part:
    // pressed here, or inherited from the kit. See the definition.
    juce::String eqBypTooltip() const;
    int  getCurrentPart() const { return currentPart; }

    // Drum-edit mode (driven by the Keyboard's DRUM KEY box). When active the
    // main knob row + Part-EQ retarget the DRUM SETUP table for drum key `note`
    // (13..91) in drum setup `setup` (0..3 = the part's DRUMS buffer); knobs with
    // no drum equivalent go inert and a few drum-only controls (GRP/VPTCH/VLPF)
    // appear. Passing active=false returns to normal part editing.
    //
    // note < 0 means "no key to edit", and NOTHING on the panel is then editable —
    // every drum-mode send path drops the write (see updateEditGating). It arises
    // two ways, which `noDrumOnKey` tells apart purely so the title can name the
    // right one: no key picked yet, or a picked key the kit maps no drum tone to.
    void setDrumEdit(bool active, int note, int setup, bool noDrumOnKey = false);
    // Re-read the drum key currently on screen. A kit assignment moves every per-key
    // value without changing the mode OR the selected key, which is the one case
    // setDrumEdit does not cover. No-op outside drum edit.
    void refreshDrumKey(int setup);

    // Fired whenever the edited part changes (selector or setCurrentPart), so the
    // Voice list can keep its highlight in sync. Not fired for no-op reselects.
    std::function<void(int)> onPartChanged;

    // Fired when a part-mode main-row knob is edited, so the Graphic Display's bar
    // for this part can mirror it. `knobIndex` is the kMainRow index (0=VOL, 3=PAN,
    // 12=REV, 13=CHORUS, 14=VAR …); only the synced ones are routed by EditorView.
    // Not fired in drum-edit mode (the knobs then target DRUM SETUP, not the part).
    std::function<void(int knobIndex, int value)> onChannelKnobChanged;

    // Mirror a Graphic Display bar edit into the matching main knob: a quiet update
    // (no SysEx), and only when `part` is the part on screen and we're not in
    // drum-edit mode (where the knobs show DRUM SETUP values instead).
    void mirrorMainKnob(int knobIndex, int part, int value);

    // Variation routing gate (driven by the Effect module's CON box via EditorView).
    // In normal part-edit mode the VAR and DRY controls are active only when the
    // variation is a SYSTEM effect (CON=SYS); when CON=INS they gray out and go
    // uneditable. Drum-edit mode overrides this (VAR always active, DRY always
    // inactive) and is unaffected by CON.
    void setVariationConnection(bool sys);

    // Whether the part on screen holds a DRUM or SFX kit voice (bank MSB 126/127) —
    // pushed by EditorView::refreshDrumContext, the same signal the Graphic Display's
    // PITCH EG uses. The device ignores MONO/POLY MODE for a drum part (spec p46), so
    // M/P grays out; XGEdit additionally grays the PP / SOF / BS receive LEDs. Values
    // are left alone (see setDrumKitPart's comment).
    void setDrumKitPart(bool isKit);

    // The DRUMS buffer assigned to the current part (-1 = none). NOT the same test as
    // setDrumKitPart: a kit can be picked without a buffer (PART MODE 1), and only a
    // BUFFER brings the per-key DRUM SETUP that owns the shelves. See partShelfEqInert.
    void setPartDrumBuffer(int buffer);

    // Options ▸ Part module title shows ▸. A BITMASK: the two halves are independent
    // settings — the menu is two check items, and neither ticked is the default and
    // the behaviour this module has always had. Mirrors the processor's PartTitleNames.
    enum TitleNames { TitleNamePart = 1, TitleNameDrum = 2 };
    void setTitleNameMode(int mask);

    // The two names the title can carry, pushed by EditorView (which owns both the
    // panel that knows the voice name and the one that knows the drum tone). Safe to
    // call with the option off — the strings are simply held until it is turned on,
    // which is what lets the menu take effect without touching any other panel.
    void setPartVoiceName(const juce::String& name);
    void setDrumToneName(const juce::String& name);

    // "Show part MIDI receive switches" (the Options toggle, default on). Hides/shows
    // the bottom LED row outright — a visibility preference only: the switches keep
    // their values, nothing is sent, and the module keeps its height (the strip is
    // just left blank; see the processor's showPartRcvSwitches comment).
    void setRcvRowVisible(bool shouldShow);

    // "Disable plugin-only parameters" (the Options toggle). When locked, the two
    // controls the MU100/XG hardware does not have — HPF2 and the EQ grid's three
    // peaking mid bands (2-4) — grey out and stop accepting input, in part-edit and
    // drum-edit mode alike (both target the same boxes). The hardware BASS/TREBLE
    // shelves (bands 1 & 5) are untouched: they ARE XG params and DO export.
    void setPluginOnlyLocked(bool locked);

    // "Mark DAW-automatable params" (the Options dropdown; XgKnob::AutoMarker values).
    // Marks the ten main-row knobs that AutomationParams also exposes as host
    // parameters: VOL / KSHIFT / PAN / HPF / RES / CUTOFF / REV / CHORUS / VAR / DRY.
    // Automatically suppressed in drum-edit mode, where those knobs retarget DRUM
    // SETUP — which has no host parameters at all.
    // `cc` = draw the MIDI CC underline on the controls that send one (see EditorView).
    void setAutomationMarkers(int marker, bool cc);

    // Phase 5 read-back: refresh every control to reflect the current part's
    // values in `st` (device state from a bulk-dump round-trip). Controls whose
    // bytes aren't present in `st` keep their current value. Sets quietly (no
    // SysEx is sent back).
    void applyState(const mu100::DeviceState& st);
    // Re-read the MIDI-override shadow onto this part's controls. Separate from
    // applyState because the two move independently: the shadow changes while the
    // model stands still, which is the whole situation it exists to report.
    void refreshMidiOverrides();

    // Re-lay-out and repaint for a change of MIDI-override mark style (Display
    // Animation -> "Mark them with"). The horizontal bar occupies layout, not just
    // pixels, so this cannot be a bare repaint.
    void refreshMidiMarkStyle();

    void resized() override;
    void paint(juce::Graphics&) override;

    // Knob value-display style (public so the part/drum knob config table can
    // name it). Plain raw 0-127; Pan RND/L/C/R; KShift/Signed bipolar; GainDb dB;
    // EqFreq Hz; DetuneHz Hz; OnOff OFF/ON.
    enum class Kind { Plain, Pan, KShift, GainDb, Signed, EqFreq, DetuneHz, OnOff };

private:
    std::unique_ptr<XgKnob> makeKnob(const char* label, const char* paramName,
                                     int dflt, Kind kind);
    static void applyKind(XgKnob& k, Kind kind, int paramIndexForRange);
    void addEnum(const char* label, const char* paramName,
                 const juce::StringArray& items, int dflt);
    void buildScaleControls();
    void buildRcvLeds();
    void layOutRcvLeds(juce::Rectangle<int> area);   // bottom strip, from resized()
    void updateDrumKitGating();     // gray M/P + SCALE/TUNE + PORTA/PTIME on a kit part
    void updateRcvRowGating();      // gray the whole LED row in drum-key-edit mode
    void buildEqGrid();
    void allowOverrideMarks();      // let this module's controls carry override marks
    void recallPartEq();            // load the plugin mid-bands (2-4) from processor state
    void buildDrumControls();
    void applyDrumMode();                       // reconfigure controls for the mode
    void updateModeTooltips();                  // EQ + HPF2 + ASSIGN tooltips for the mode
    void updateAutomationMarkers();             // re-apply the DAW-automatable markers
    void updateVarDryEnabled();                 // gray/ungray VAR + DRY per CON (part mode only)
    void setMainKnobActive(int knobIndex, bool active);  // full part knob vs bare/grayed slot
    void loadDrumKey();                         // read-back: show the drum key's values
    void updateEditGating();                    // gray whatever can't currently be edited
    void dispatchMain(int knobIndex, double v); // route a main-knob change per mode
    void dispatchEq(int eqIndex, double v);     // route a Part-EQ change per mode
    void sendDrum(const char* drumParamName, double v);

    YamahaMU100AudioProcessor& audioProcessor;
    int currentPart { 0 };

    // See setPluginOnlyLocked. Applied through updateEditGating, which is the ONE
    // place that decides a control's enabled state — the plugin-only lock and the
    // drum-key gate overlap on HPF2 + the mid bands, so neither may set it alone.
    bool pluginOnlyLocked { false };

    // The EQ grid's two markers (XgKnob::AutoMarker), decided by
    // updateAutomationMarkers and drawn by paint — HPF2's against eqColLabel[0][1],
    // the GAIN row's against eqRowLabel[EqGain]. Separate from automationMarker
    // below, and from each other, because all three are gated differently.
    int hpf2Marker   { 0 };
    int eqGainMarker { 0 };

    // See setAutomationMarkers. XgKnob::AutoMarker value; MarkerNone until pushed,
    // so a panel built before the option is read shows nothing rather than guessing.
    int automationMarker { 0 };

    // The MIDI CC mark (underline): whether to draw it at all (decided by EditorView
    // from the option + MIDI Out mode), and whether PORTA's label -- a plain Label,
    // painted by this panel -- currently carries it.
    bool ccMarks       { false };
    bool portaCcMarked { false };

    juce::Label    titleLabel;     // "Part", or "Drum Key" in drum-key-edit mode

    // Title naming (Options ▸ Name in the Part module title). Default 0 = neither,
    // i.e. the bare mode word this module has always shown.
    int          titleNameMode { 0 };
    juce::String partVoiceName;    // pushed; empty until EditorView has said
    juce::String drumToneName;     // pushed; empty outside drum-edit mode

    std::vector<std::unique_ptr<XgKnob>> mainKnobs;
    std::unique_ptr<XgKnob>              ptimeKnob;     // Portamento Time (next to Porta)

    // EQ grid: 3 rows (GAIN / FREQ / Q) x 5 bands, value boxes only (no knobs).
    // Columns are bands 1..5 left-to-right: band 1 (col 0) = low shelf and band 5
    // (col 4) = high shelf are the hardware XG BASS/TREBLE params; bands 2-4 (cols
    // 1-3) are the plugin-only peaking EQ (proc.setPartEq*). The shelves have no Q,
    // so eqBox[2][0] and eqBox[2][4] are null.
    //
    // On screen the grid has SIX columns: HPF2 (below) is drawn as the first one,
    // holding only a FREQ cell. eqBox is still indexed by band 0..4 — the HPF2
    // column is laid out separately — so grid column = band + 1.
    enum { EqGain = 0, EqFreq = 1, EqQ = 2 };
    std::unique_ptr<ValueBox> eqBox[3][5];
    juce::Label eqRowLabel[4];       // GAIN / FREQ / Q / BAND (left of the rows)
    // Two-line label under each of the six columns: [col][line], col 0 = HPF2.
    // "F/HPF2", "1 LO/SHELF", "2/PEAK", "3/PEAK", "4/PEAK", "5 HI/SHELF".
    juce::Label eqColLabel[6][2];

    struct EnumControl {
        std::unique_ptr<juce::Label> label;
        std::unique_ptr<ValueBox>    box;
    };
    std::vector<EnumControl> enums;                     // M/P, Assign

    juce::Label portaLabel;                            // "PORTA"
    DotToggle   portaToggle;                           // Portamento switch

    // EQ BYPASS lamp, in the gap to the left of the EQ grid's row labels, on the FREQ
    // row. NOT a device parameter and not part of the patch: it is a monitoring
    // control, like Mute and Solo, and lives on the processor's live mask
    // (setPartEqBypass). Lit AMBER rather than the editor's green, because lit means
    // BYPASSED -- the abnormal state -- and green means "working" everywhere else here.
    juce::Label eqBypLabel;                            // "EQ BYP"
    DotToggle   eqBypToggle;

    // Receive-enable LED row along the bottom of the module (XGEdit's bottom strip):
    // the 17 MULTI PART "Rcv <message>" switches, 08 nn 30..40, all ON by default.
    // Same LED size as PORTA, label to the right of each. XGEdit's 18th (M/P) is
    // omitted — it is a mode switch, and it already has a cell in the left cluster.
    struct RcvLed {
        std::unique_ptr<DotToggle>   dot;
        std::unique_ptr<juce::Label> label;
        int  param { -1 };      // AddressMap index
    };
    std::vector<RcvLed>  rcvLeds;
    juce::Rectangle<int> rcvSepLine;      // separator above the row (drawn in paint)
    bool drumKitPart { false };
    bool rcvRowVisible { true };          // see setRcvRowVisible

    // HPF2 — the plugin-only clean (non-resonant) per-part high-pass, absolute Hz
    // (0 = off). Not a SysEx param: drives proc.setCleanHpfHz(currentPart, hz).
    // Value box only (no knob). It sits INSIDE the EQ grid as its first column, on
    // the FREQ row (it has no gain or Q), under the same "EQ" bracket; its label is
    // the "F / HPF2" band label in eqColLabel[0].
    std::unique_ptr<ValueBox> hpf2Box;

public:
    // Reload the current part's HPF2 value into the box (quiet). Public so the
    // Graphic Display's HPF2 screen can refresh it after editing a bar.
    void recallHpf2();

    // Fired when the HPF2 box edits the current part, so the Graphic Display's
    // HPF2 screen can repaint that part's bar (live sync; both share cleanHpfHz).
    std::function<void(int part, int hz)> onHpf2Changed;

    //==========================================================================
    // Graphic Display EQ-screen bridge. The EQ screen is a node editor over the
    // same values these boxes hold, so it reads/writes them through here (the box
    // is the single source of truth; it is already part- or drum-key-aware,
    // whichever the panel is currently showing).
    //   band -1 = HPF2 (row ignored, value is Hz); band 0..4 = EQ bands 1..5.
    //   row 0 = GAIN, 1 = FREQ, 2 = Q.
    //
    // A FREQ cell's value is NOT the same quantity for every band: the shelves
    // (0 and 4) hold a raw XG EQ-table index because that is all the firmware
    // accepts, while HPF2 and the plugin-only mid bands hold absolute Hz on the
    // finer freqladder. Ask eqFreqIsHz which, or go through eqFreqToHz.
    //==========================================================================
    int  eqRaw(int band, int row) const;                 // current raw value; -1 if no such cell
    // The DEVICE's raw value for that cell where incoming MIDI has moved it, -1 where
    // it has not -- the EQ screen's ghost nodes. Mode-aware exactly as eqRaw is: a
    // shelf addresses MULTI PART by part, or DRUM SETUP by (key, buffer) while a drum
    // key is being edited. Only the two SHELVES can ever answer: the mid bands and
    // HPF2 are plugin-only, so no message can reach them.
    //
    // It does NOT consult the "Show MIDI overrides" option -- this is the data, and
    // the caller decides whether to draw it (the panel's own marks are gated in
    // refreshMidiOverrides, which is the one place that policy lives).
    int  eqOverrideRaw(int band, int row) const;
    // The same cell's TEXT rules, so the EQ screen's type-in reads and writes exactly
    // what the box would. Asking the box rather than re-deriving the format keeps
    // "+3.5dB" / "800Hz" / "2.4" defined once -- and only the box knows that a shelf's
    // FREQ is an XG table index while a mid band's is absolute Hz.
    juce::String eqTextFromValue(int band, int row, double v) const;
    double       eqValueFromText(int band, int row, const juce::String& t) const;
    void setEqFromDisplay(int band, int row, int raw);   // write back WITH notification (sends + updates box)
    // One wheel detent on that cell, by the BOX's own law (ValueBox::nudge -- the very
    // code path its mouse wheel takes). The EQ screen's keyboard frequency axis goes
    // through this rather than computing a raw delta, because a FREQ cell is absolute Hz
    // on the freqladder for HPF2 and the mid bands but a coarse XG table index on the
    // shelves: one detent is the only step size that means the same thing on all of
    // them. Declines on a disabled or absent cell, so the plugin-only lock and the
    // drum-key gate cover it without a second test here.
    void nudgeEqFromDisplay(int band, int row, int dir, bool fine);
    void resetEqBand(int band);                          // restore that band (or HPF2) to its default(s)
    static void   eqFreqRange(int band, int& lo, int& hi); // that band's FREQ range, for the screen's x-scaling
    static bool   eqFreqIsHz(int band);                    // true = FREQ is Hz (mid bands); false = XG table index
    static double eqFreqToHz(int band, int v);             // a FREQ cell's value -> Hz, whichever it is
    static double eqFreqDefault(int band);                 // that band's default FREQ, in its own units

    // Fired whenever an EQ / HPF2 value box is edited (by the user or by the EQ
    // screen), so the EQ screen can repaint — it reads the values live via eqRaw.
    std::function<void()> onEqChanged;

private:
    void notifyEqChanged() { if (onEqChanged) onEqChanged(); }


    // SCALE micro-tuning: a note selector (C..B) + a cents value for that note.
    // (12 SCALE TUNING bytes 0x41-0x4C; selector picks which one TUNE edits.)
    juce::Label               scaleLabel, tuneLabel;
    std::unique_ptr<ValueBox> scaleNoteBox;            // C..B (local, not a SysEx param)
    std::unique_ptr<ValueBox> tuneBox;                 // -64..+63 cents for the selected note
    int scaleParam[12];                                // AddressMap indices, C..B
    int scaleCache[12];                                // selected part's 12 scale-tune cents

    // Drum-edit mode state + drum-only mid-row controls (shown only in drum mode,
    // occupying the Portamento-Time / SCALE / TUNE cells).
    bool drumEditActive { false };
    int  drumKey   { -1 };   // 13..91, or -1 = nothing to edit (see setDrumEdit)
    bool drumNoDrumOnKey { false };   // drumKey<0 because the key holds no drum tone
    int  drumSetup { 0 };    // 0..3 (the part's DRUMS buffer)
    int  partBuffer { -1 };  // the CURRENT part's DRUMS buffer, -1 = none

    // Are the two HARDWARE shelf columns (bands 1 and 5) dead for the current part?
    // True exactly when a DRUMS buffer is assigned and we are not editing a key: the
    // device ignores part-level EQ on a drum part, so those two do nothing there and
    // the per-key DRUM SETUP owns them instead.
    //
    // The FOUR PLUGIN columns -- HPF2 and mid bands 2-4 -- are NOT covered by this and
    // must not be gated on it: they are our own per-voice filters and run on a drum
    // part like any other, stacked on top of the key layer. (They were switched off
    // with the shelves until 2026-09-05; see PluginProcessor.h partShelfEqInert.)
    bool partShelfEqInert() const { return partBuffer >= 0 && ! drumEditActive; }
    bool varConnectionSys { false };   // variation CON = SYS (else INS) — gates VAR/DRY
    // Per-knob AddressMap indices for each mode (resolved once in the ctor).
    int  mainPartParam[16] {};   int mainDrumParam[16] {};
    int  eqPartParam[4]    {};   int eqDrumParam[4]    {};
    juce::Label               grpLabel, vptchLabel, vlpfLabel;
    std::unique_ptr<ValueBox> grpBox, vptchBox, vlpfBox;
    // Mid-row cell rects captured in resized() so the drum-only boxes can overlay
    // the Portamento-Time / SCALE / TUNE cells.
    juce::Rectangle<int> ptimeCell, scaleCell, tuneCell;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PartPanel)
};
