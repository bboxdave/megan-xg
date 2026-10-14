// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

#include <JuceHeader.h>
#include "Accessibility.h"
#include "KeyNav.h"
#include <array>
#include <memory>

class YamahaMU100AudioProcessor;
class ValueBox;
namespace mu100 { class DeviceState; }

//==============================================================================
// PartListPanel — the XGEdit "Voice" module (the Voice Box). A 32-row overview
// of the synth: each row is a part with its MIDI receive channel (CH) and
// element reserve (EL) boxes plus the assigned voice name. Below the list, a
// read-only readout shows the selected part's EL / MSB / LSB / PRG.
//
// Clicking a part's number or name selects it as the edit target (driving the
// Part module); clicking a voice name asks the host to open the voice browser
// (the host owns it so it can overlay the whole editor).
//
// Until device-state read-back exists (Phase 5), the per-part channel / reserve
// / voice shown here is the editor's own model (factory defaults, updated as the
// user edits), not a live read from the MU100.
//==============================================================================
class PartListPanel : public juce::Component,
                      public juce::SettableTooltipClient,
                      public keynav::Group,
                      private juce::Timer
{
public:

    // --- keyboard navigation (KeyNav.h) ---
    std::vector<keynav::Item> navItems() override;
    juce::String navGroupName() const override { return "Voice"; }

    explicit PartListPanel(YamahaMU100AudioProcessor& proc);
    ~PartListPanel() override;

    // Selecting a part here asks the host to make it the edit target.
    std::function<void(int)> onSelectPart;

    // Ask the host to open the voice browser for `part` (host owns the overlay).
    // Args: part, current bank MSB, bank LSB, program (0-based).
    std::function<void(int, int, int, int)> onRequestVoiceBrowse;

    // Right-click on a row: the part context menu (copy / paste / reset). Right-click
    // is this editor's "reset this control" gesture everywhere it lands on a CONTROL,
    // so the menu may only live where it currently means nothing -- a Voice row is one
    // of the two such places (the bar screens' part ruler is the other).
    std::function<void(int)> onPartContextMenu;

    // Reflect a part selection made elsewhere (e.g. the Part module's selector).
    void setSelectedPart(int part0to31);
    int  getSelectedPart() const { return selectedPart; }

    // Drum edit buffer (DRUMS1..DRUMS4 = 0..3) a part's kit is allocated to, or
    // -1 for none. The Voice list outlines the part's voice name in the buffer's
    // colour. setPartBuffer repaints; getPartBuffer lets the host cascade a kit
    // change to every part sharing the buffer.
    void setPartBuffer(int part, int buffer);
    int  getPartBuffer(int part) const;

    // Transmit the part's PART MODE to the device (0 = Normal, 1 = Drum, 2..5 =
    // DRUMS1..4). Called when the user (re)assigns a drum buffer so the device's
    // drum-setup selection matches the editor — without it, an in-editor DRUMS
    // assignment wouldn't round-trip (the device kept its old PART MODE).
    void sendPartMode(int part, int mode);

    // The part's currently-assigned voice (bank MSB/LSB + 0-based program), so
    // the host can re-point an open browser when the selected part changes.
    void getPartVoice(int part, int& msb, int& lsb, int& prg0) const;

    // The name this panel SHOWS for a part, in whichever form Options ▸ Voice names is
    // set to. Public because this panel is the owner of that question and the Part
    // module title may need the same string — see EditorView::pushPartTitleNames, and
    // KeyboardPanel::drumKeyText, which is public for the same reason.
    juce::String voiceName(int part) const;

    // Apply a voice/kit chosen in the browser to a part: stores the model,
    // transmits Bank Select + Program Change, and refreshes the display.
    void applyVoice(int part, int msb, int lsb, int program0, const juce::String& name);

    // Phase 5 read-back: refresh all 32 parts' channel / element-reserve / voice
    // from device state (a bulk-dump round-trip). Per-part bytes absent in `st`
    // keep their current model value. Sets quietly (no SysEx is sent back).
    void applyState(const mu100::DeviceState& st);
    // See the .cpp: which rows carry a MIDI-override mark, and the voice each one
    // has been moved to on the device.
    void refreshMidiOverrides();
    // See the .cpp: which rows carry the yellow roll-up tab. Driven by the model,
    // so it has its own trigger -- the override refresh cannot stand in for it.
    void refreshRowChanged();

    // Re-resolve all 32 cached voice names after Options -> Voice names changed.
    void refreshVoiceNames();
    // Start or stop the indicator fade timer to match "MIDI input indicators".
    void refreshMidiIndicators();

    // The M / S cell action, also reached from the keyboard shortcuts. Public because a
    // shortcut must take the SAME path a click does -- exclusive means "collapse onto
    // this part", and the mute/solo exclusivity rule lives below this in the processor.
    void handleMsClick(int part, bool solo, bool exclusive);

    void resized() override;
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    juce::String getTooltip() override;   // per-column (Pt = "Part N Select", name = voice)

private:
    // Column geometry. `ms` is the width of ONE of the two mute/solo cells and is 0
    // when the columns are hidden, so `msX < 0` is the single test for "not shown"
    // and the voice-name column simply reclaims the space. The canvas width does NOT
    // change with the option: a menu tick that resizes the plugin window is jarring,
    // and giving the 2 x kMsW back to the voice names is a straight improvement.
    // `num` now holds the part number AND the row's MIDI-override tab, which sits at
    // its right edge; `ovrX` is that tab's left edge. `nameGutter` is the clearance
    // kept at the far right of the name column for the VOICE override tab, so a long
    // name is ellipsized before it reaches the mark rather than running under it.
    struct Cols { int pad, num, ovrX, ch, el, ms, msX, nameX, nameW, nameGutter; };
    Cols    columns() const;
    bool    msVisible() const;
    static constexpr int kMsW = 18;     // one M or S cell, including its gutter
    juce::Rectangle<int> msCell(const Cols& c, int x0, int y, int rh) const;
    int     headerH()  const { return 15; }
    int     footerH()  const { return 40; }
    int     listHeight() const { return getHeight() - headerH() - footerH(); }
    int     rowHeight() const;
    int     rowAtY(int y) const;            // -1 if outside the 32 list rows

    // Accessibility pillar 3: invisible stand-ins over the three PAINTED stops of the
    // selected row (Mute, Solo, the voice name), so a screen reader has something that
    // can hold focus there. navItems() moves them onto the cells it computes -- the
    // cell rect stays the source of truth. See mu100a11y::CellProxy.
    void buildCellProxies();
    // Put the proxies where the cells are. It runs navItems() and throws the result
    // away, deliberately: that is the ONE place these rects are computed, and a second
    // copy of the geometry here is exactly the kind of thing that drifts. Called from
    // resized() and on a selection change, so the proxies are in place for a screen
    // reader that starts tabbing before any keyboard navigation has happened.
    void refreshCellProxies() { (void) navItems(); }
    mu100a11y::CellProxy msProxy[2];   // 0 = Mute, 1 = Solo
    mu100a11y::CellProxy nameProxy;
    static void  lookupVoice(int msb, int lsb, int prg0,
                             juce::String& name, int& model, int& elements);
    void    paintReadoutField(juce::Graphics&, juce::Rectangle<int>, const juce::String&);
    void    updateElementBudget();          // recompute remaining + per-box caps

    // Per-part "MIDI in" indicator: an orange tab at the far-right of each row that
    // fades in/holds/fades out as MIDI data arrives for the part. The timer polls
    // the processor's activity accumulators, advances the per-part envelopes and
    // repaints only the indicator strip. See timerCallback().
    void    timerCallback() override;
    void    repaintIndicatorColumn();
    juce::Rectangle<int> indicatorColumn() const;   // right strip repainted per tick

    std::array<float,  32> midiLevel { };       // 0..1 brightness per part
    std::array<double, 32> midiHoldUntil { };   // ms; level targets 1 while now < this

    static constexpr int kTotalElements = 64;   // MU100 element pool

    YamahaMU100AudioProcessor& audioProcessor;
    int selectedPart { 0 };

    // AddressMap param indices (MULTI PART), looked up once.
    int chParam { -1 }, reserveParam { -1 };
    int bankMsbParam { -1 }, bankLsbParam { -1 }, programParam { -1 };

    // Which parts have a voice the DEVICE has been moved off by incoming MIDI, and
    // what it was moved TO. Refreshed from the processor's override shadow; the row
    // marks itself from this rather than asking per repaint, because this list
    // repaints on every MIDI-activity tick.
    uint32_t     voiceOverrideMask { 0 };
    juce::String voiceOverrideName[32];
    // The two ROW roll-ups: anything changed / overridden on this part that the row
    // itself does not already show. Recomputed with the rest, never per repaint.
    uint32_t     rowChangedMask  { 0 };
    uint32_t     rowOverrideMask { 0 };
    int partModeParam { -1 };   // MULTI PART "PART MODE": 2..5 = DRUMS1..4 buffer

    // Per-part editor model (no read-back yet): assigned voice bank/program +
    // cached display name / model / element count for the row indicators.
    std::array<int, 32> partMsb, partLsb, partPrg;
    std::array<int, 32> partDefMsb, partDefLsb, partDefPrg;   // factory-default voice per part
    std::array<int, 32> partModel, partElem;
    std::array<int, 32> partBuffer;   // drum edit buffer 0..3, or -1
    std::array<juce::String, 32> partName;
    std::array<std::unique_ptr<ValueBox>, 32> chBox;   // MIDI receive channel
    std::array<std::unique_ptr<ValueBox>, 32> elBox;   // element reserve

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PartListPanel)
};
