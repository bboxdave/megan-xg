// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#include "EditorView.h"
#include "EditorColours.h"
#include "PartPanel.h"
#include "PartListPanel.h"
#include "GraphicDisplayPanel.h"
#include "EffectPanel.h"
#include "EffectParametersPanel.h"
#include "MultiEqPanel.h"
#include "ControllersPanel.h"
#include "SystemPanel.h"
#include "KeyboardPanel.h"
#include "VoiceBrowser.h"
#include "XgKnob.h"          // AutoMarker values (refreshAutomationMarkers)
#include "BusyOverlay.h"
#include "PluginProcessor.h"
#include "codec/MU100Effects.h"
#include <set>

//==============================================================================
// Design-canvas layout. Coordinates are in pixels of the reference image
// (reference/xgedit gui enhanced.png, 1480x1063). Each region keeps XGEdit's
// relative position; the whole canvas scales uniformly to the window.
//==============================================================================
namespace
{
    // 1480 -> 1516 (2026-08-30): the Voice list gained the two mute/solo cells, and
    // the canvas is the only place that can pay for them. Its longest voice names
    // already only just fitted, so the 36 px comes from the CANVAS, not from the name
    // column -- kVoiceRect widens by exactly the same amount, and the KEYBOARD with it
    // (it is the only module that spans the whole canvas; every other right edge is at
    // 1170 and does not move). The default window width (PluginProcessor::editorViewW)
    // grew in the same proportion so the on-screen scale is unchanged for a new install.
    //
    // 1516 -> 1534 (2026-09-09): the Voice list's rows gained a MIDI-override tab
    // inside the part-number column (+9) and a reserved gutter at the right of the
    // name column for the voice override tab (+8, kept unconditionally so a name
    // never reflows when a program change arrives). Paid for by the canvas again,
    // for the same reason as the mute/solo pair: the longest voice names only just
    // fit, so taking it out of the name column would start ellipsizing them.
    constexpr float kDesignW = 1534.f;
    constexpr float kDesignH = 962.f;

    // The Keyboard module lost 15 px and the Effect Parameters module 10 px (their
    // keys / dials shrink accordingly) so the Part module could gain 25 px for its
    // receive-enable LED row; the Voice column grew to fill the space the Keyboard
    // gave up. Everything's outer edges (canvas top/bottom, module x extents) are
    // unchanged, so nothing to the right of the keys moved or resized.
    // 1472 -> 1508 wide (2026-08-30). The Keyboard is the ONLY module that spans the
    // whole canvas, so it is the only one the mute/solo widening moves: its right edge
    // is flush with the Voice module's, and leaving it at 1472 would have left the
    // keys ending 36 px short of the column above them. Everything else stops at 1170.
    // 1508 -> 1526 (2026-09-09), for the same reason and by the same amount as the
    // Voice module: this is still the only module whose right edge is flush with it.
    const juce::Rectangle<float> kKeyboardRect { 8.f, 810.f, 1526.f, 135.f };

    // System + Multi EQ are shortened (and their dials shrunk) and the Graphic /
    // Effect / Controller modules raised, to free the vertical space the taller Part
    // module needs for its 3-row EQ grid. The Voice column and Keyboard are unchanged.
    const juce::Rectangle<float> kSystemRect { 8.f, 14.f, 420.f, 118.f };

    const juce::Rectangle<float> kPartRect    { 8.f, 524.f, 1162.f, 283.f };
    const juce::Rectangle<float> kVoiceRect   { 1180.f, 14.f, 354.f, 790.f };
    const juce::Rectangle<float> kMultiEqRect { 8.f, 136.f, 420.f, 132.f };
    // The "Controller" section spans XGEdit's Modulation/LFO group (the source
    // selector + six depth knobs) and the Controller group (Controller 1/2), which
    // are one logical module.
    const juce::Rectangle<float> kControllersRect { 446.f, 276.f, 724.f, 120.f };
    // Graphic Display: the 7 mode buttons sit just above the LCD area. Raised to fill
    // the previously-empty band above it.
    const juce::Rectangle<float> kGraphicRect { 434.f, 14.f, 736.f, 250.f };
    const juce::Rectangle<float> kEffectRect       {   8.f, 276.f, 430.f, 120.f };
    const juce::Rectangle<float> kEffectParamsRect {   8.f, 402.f, 1162.f, 118.f };

    // Graphic Display bar channel <-> Part-module main-knob index, for two-way
    // value sync. Indices match kMainRow in PartPanel.cpp.
    int knobIndexForChannel(GraphicDisplayPanel::Channel ch)
    {
        switch (ch)
        {
            case GraphicDisplayPanel::Channel::Volume: return 0;
            case GraphicDisplayPanel::Channel::Pan:    return 3;
            case GraphicDisplayPanel::Channel::Reverb: return 12;
            case GraphicDisplayPanel::Channel::Chorus: return 13;
            case GraphicDisplayPanel::Channel::Var:    return 14;
        }
        return -1;
    }

    bool channelForKnobIndex(int i, GraphicDisplayPanel::Channel& out)
    {
        switch (i)
        {
            case 0:  out = GraphicDisplayPanel::Channel::Volume; return true;
            case 3:  out = GraphicDisplayPanel::Channel::Pan;    return true;
            case 12: out = GraphicDisplayPanel::Channel::Reverb; return true;
            case 13: out = GraphicDisplayPanel::Channel::Chorus; return true;
            case 14: out = GraphicDisplayPanel::Channel::Var;    return true;
            default: return false;
        }
    }
}

//==============================================================================

EditorView::EditorView(YamahaMU100AudioProcessor& proc) : audioProcessor(proc)
{
    // The design canvas holds every module at fixed design coordinates and is
    // scaled to the window as a whole (see the header note + resized()).
    addAndMakeVisible(canvas);

    partPanel = std::make_unique<PartPanel>(audioProcessor);
    canvas.addAndMakeVisible(*partPanel);

    partListPanel = std::make_unique<PartListPanel>(audioProcessor);
    canvas.addAndMakeVisible(*partListPanel);

    graphicDisplay = std::make_unique<GraphicDisplayPanel>(audioProcessor);
    canvas.addAndMakeVisible(*graphicDisplay);

    // Effect + Effect Parameters modules: the Effect module's unit selection
    // drives which effect + parameters the Effect Parameters module shows.
    effectPanel       = std::make_unique<EffectPanel>(audioProcessor);
    effectParamsPanel = std::make_unique<EffectParametersPanel>(audioProcessor);
    canvas.addAndMakeVisible(*effectPanel);
    canvas.addAndMakeVisible(*effectParamsPanel);
    effectPanel->onUnitChanged = [this](int unit) { effectParamsPanel->setUnit(unit); };
    effectPanel->onConnectionChanged = [this] { effectParamsPanel->refreshActive(); };
    effectParamsPanel->setUnit(effectPanel->getUnit());

    multiEqPanel     = std::make_unique<MultiEqPanel>(audioProcessor);
    controllersPanel = std::make_unique<ControllersPanel>(audioProcessor);
    systemPanel      = std::make_unique<SystemPanel>(audioProcessor);
    keyboardPanel    = std::make_unique<KeyboardPanel>(audioProcessor);
    canvas.addAndMakeVisible(*multiEqPanel);
    canvas.addAndMakeVisible(*controllersPanel);
    canvas.addAndMakeVisible(*systemPanel);
    canvas.addAndMakeVisible(*keyboardPanel);

    // The keyboard cursor's focus ring, on top of every module. Added before the
    // voice browser so the browser (a later canvas child) still covers it.
    canvas.addAndMakeVisible(navRing);
    navRing.setVisible(false);
    navRing.setAccessible(false);   // a painted cue, not something to announce

    // Every juce::Label on the canvas is DECORATIVE for accessibility purposes: it is
    // the abbreviated caption under a control ("PTIME", "M/P", "GAIN"), and since
    // pillar 1 the control itself carries the full spoken name ("Portamento Time"). Left
    // visible to the accessibility tree they are 55 extra elements (measured) that add
    // nothing and read as noise between the controls they label.
    //
    // Done as ONE walk here rather than per label: a label added to a panel tomorrow is
    // hidden automatically, where 55 call sites would leave the next one to be found by
    // a user. The walk re-runs whenever this view is rebuilt, which is what a theme
    // change does.
    std::function<void(juce::Component&)> hideLabels = [&hideLabels](juce::Component& c)
    {
        for (auto* k : c.getChildren())
        {
            if (dynamic_cast<juce::Label*>(k) != nullptr) k->setAccessible(false);
            else                                          hideLabels(*k);
        }
    };
    hideLabels(canvas);

    // Accessibility pillar 3: name the MODULES, so a screen reader says which part of
    // the editor the cursor has moved into rather than announcing nine unnamed
    // containers. The name is navGroupName() -- the one the manual, the keyboard
    // documentation and Tab-between-modules already use, so there is no second
    // vocabulary to keep in step.
    for (auto* g : navGroups())
        if (auto* c = dynamic_cast<juce::Component*>(g))
        {
            c->setTitle(g->navGroupName());
            c->setAccessible(true);
        }

    // Watch every mouse press in the canvas SUBTREE (the `true`), so a click moves the
    // keyboard cursor onto the control it landed on. One listener instead of a hook on
    // ~500 controls -- and, like keynav::collect, it cannot go stale when a panel gains
    // a control. Listeners are passive: this changes no event routing.
    canvas.addMouseListener(this, true);

    //==========================================================================
    // Two-way value sync between the Graphic Display's bar graphs and their knob
    // counterparts in the Part / System / Effect modules: every synced control
    // edits the same device parameter, so editing one updates the other's display
    // (quietly — only the control the user moved transmits to the device).
    //==========================================================================

    // Graphic Display bar edited by the user -> mirror into the matching knob.
    graphicDisplay->onBarEdited =
        [this](GraphicDisplayPanel::Channel ch, int part, int value)
    {
        if (part < 0)   // the leftmost "S" (system) bar
        {
            switch (ch)
            {
                case GraphicDisplayPanel::Channel::Volume:
                    systemPanel->mirrorMasterVolume(value);                    break;
                case GraphicDisplayPanel::Channel::Reverb:
                    effectPanel->mirrorReturnLevel(mu100fx::FX_REVERB, value); break;
                case GraphicDisplayPanel::Channel::Chorus:
                    effectPanel->mirrorReturnLevel(mu100fx::FX_CHORUS, value); break;
                case GraphicDisplayPanel::Channel::Var:
                    // Variation "S" has no AddressMap path — the Effect module sends it.
                    effectPanel->setVariationSystemLevel(value);              break;
                case GraphicDisplayPanel::Channel::Pan:
                    break;   // pan has no system value
            }
        }
        else            // a per-part bar
        {
            partPanel->mirrorMainKnob(knobIndexForChannel(ch), part, value);
        }
    };

    // Part-module main knob edited -> mirror into this part's Graphic Display bar.
    partPanel->onChannelKnobChanged = [this](int knobIndex, int value)
    {
        GraphicDisplayPanel::Channel ch;
        if (channelForKnobIndex(knobIndex, ch))
            graphicDisplay->setExternalBar(ch, partPanel->getCurrentPart(), value);
    };

    // HPF2 live sync (outside the Channel knob system): the graphic HPF2 bar and
    // the Part module's HPF2 box share the processor's per-part cleanHpfHz, so an
    // edit on either refreshes the other's display.
    graphicDisplay->onHpf2Edited = [this](int part, int /*hz*/)
    {
        if (partPanel->getCurrentPart() == part) partPanel->recallHpf2();
    };
    partPanel->onHpf2Changed = [this](int part, int hz)
    {
        graphicDisplay->mirrorHpf2(part, hz);
    };

    // EQ screen <-> Part module EQ/HPF2 boxes. The boxes are the source of truth
    // (part- or drum-key-aware); the graphic EQ node editor reads/writes them and
    // repaints when a box is edited (onEqChanged).
    graphicDisplay->eqReadRaw   = [this](int band, int row) { return partPanel->eqRaw(band, row); };
    graphicDisplay->eqReadOverride = [this](int band, int row)
                                     { return partPanel->eqOverrideRaw(band, row); };
    graphicDisplay->eqWriteRaw  = [this](int band, int row, int raw) { partPanel->setEqFromDisplay(band, row, raw); };
    graphicDisplay->eqNudgeRaw  = [this](int band, int row, int dir, bool fine)
                                  { partPanel->nudgeEqFromDisplay(band, row, dir, fine); };
    graphicDisplay->eqResetBand = [this](int band) { partPanel->resetEqBand(band); };
    // Text in and out for the node type-in: the Part module's own box for that cell
    // answers both, so a value typed at a handle is parsed exactly as it would be there.
    graphicDisplay->eqTextOf = [this](int band, int row, double v)
                               { return partPanel->eqTextFromValue(band, row, v); };
    graphicDisplay->eqParse  = [this](int band, int row, const juce::String& t)
                               { return partPanel->eqValueFromText(band, row, t); };
    partPanel->onEqChanged      = [this] { graphicDisplay->refreshEqIfShown(); };

    // System HPF box <-> the HPF2 screen's "S" bar. Same two-way shape as HPF2 above:
    // the processor holds the value, and each surface tells the other to look again.
    systemPanel->onSystemHpfChanged = [this](int hz)
    {
        graphicDisplay->mirrorSystemHpf(hz);
    };
    graphicDisplay->onSystemHpfEdited = [this](int hz)
    {
        systemPanel->mirrorSystemHpf(hz);
    };

    // System VOL knob <-> Graphic Display Volume "S" bar.
    systemPanel->onMasterVolumeChanged = [this](int value)
    {
        graphicDisplay->setExternalBar(GraphicDisplayPanel::Channel::Volume, -1, value);
    };

    // Effect return LEVEL knob -> Graphic Display Reverb / Chorus / Var "S" bar.
    effectPanel->onReturnLevelChanged = [this](int unit, int level)
    {
        GraphicDisplayPanel::Channel ch;
        switch (unit)
        {
            case mu100fx::FX_REVERB:    ch = GraphicDisplayPanel::Channel::Reverb; break;
            case mu100fx::FX_CHORUS:    ch = GraphicDisplayPanel::Channel::Chorus; break;
            case mu100fx::FX_VARIATION: ch = GraphicDisplayPanel::Channel::Var;    break;
            default: return;   // insertion units have no "S" bar
        }
        graphicDisplay->setExternalBar(ch, -1, level);
    };

    // Variation CON routing (INS/SYS) -> enable/seed the Graphic Display Var "S"
    // bar AND the Part module's VAR/DRY gating (grayed when CON=INS).
    effectPanel->onVariationSystemStateChanged = [this](bool sysActive, int level)
    {
        graphicDisplay->setVariationSystemActive(sysActive, level);
        partPanel->setVariationConnection(sysActive);
    };
    {
        const bool sys = effectPanel->variationIsSystem();
        graphicDisplay->setVariationSystemActive(sys, effectPanel->variationSystemLevel());
        partPanel->setVariationConnection(sys);
    }

    // Drum-edit mode: the Keyboard's DRUM KEY box drives the Part module, which
    // retargets its knobs to the part's DRUMS buffer + selected drum key.
    keyboardPanel->onDrumEditChanged = [this](bool active, int key)
    {
        // A MODE change: with drum edit armed, every control on the Part module edits
        // one drum key instead of the part, and with no key picked they edit nothing at
        // all. That last state is the one that silently swallows edits (see the drum-key
        // gating), so it is named rather than implied.
        announcer->say(mu100a11y::announce::Context,
                       ! active ? juce::String("Part edit")
                       : key < 0 ? juce::String("Drum edit, no key selected")
                                 : "Drum edit, " + keyboardPanel->drumKeyText());
        const int part  = partListPanel->getSelectedPart();
        const int setup  = juce::jmax(0, partListPanel->getPartBuffer(part));
        // `key` is already -1 both when nothing is picked and when the picked key
        // carries no drum in this kit; the Part module edits nothing either way, but
        // it names the two differently in its title, so pass which one this is.
        const bool noDrumOnKey = active && key < 0 && keyboardPanel->isDrumKeyHighlighted();
        partPanel->setDrumEdit(active, key, setup, noDrumOnKey);
        graphicDisplay->setDrumEdit(active, key, setup);   // AMP EG / EQ -> drum key
        graphicDisplay->setDrumKeyName(active ? keyboardPanel->drumKeyText() : juce::String());
        pushPartTitleNames();   // ...and the Part module title, if it is naming the tone
    };

    // Keep part selection in sync between the Voice list and the Part module.
    partListPanel->onSelectPart = [this](int p) { partPanel->setCurrentPart(p); };

    // The Graphic Display's x-axis part ruler selects too — same destination, so a
    // click under a moving level meter picks that part with everything else (Voice
    // list, Part / Controllers / Keyboard modules) following as usual.
    graphicDisplay->onSelectPart = [this](int p) { partPanel->setCurrentPart(p); };

    // Both right-click surfaces hand the part menu to the same place. They have already
    // selected the part, so there is nothing to decide here.
    partListPanel->onPartContextMenu  = [this](int p) { if (onPartContextMenu) onPartContextMenu(p); };
    graphicDisplay->onPartContextMenu = [this](int p) { if (onPartContextMenu) onPartContextMenu(p); };

    // Whenever the current part changes (via the Voice list's Pt/voice columns
    // or the Part module's own selector), re-point an open browser to it and
    // update the Graphic Display's EG screens (which edit the current part).
    partPanel->onPartChanged = [this](int p)
    {
        // Which part every other module is now editing. The single most useful thing to
        // announce here, because Ctrl+arrow moves it and nothing else says so.
        announcer->say(mu100a11y::announce::Context,
                       "Part " + juce::String(p + 1));
        partListPanel->setSelectedPart(p);
        graphicDisplay->setCurrentPart(p);
        controllersPanel->setCurrentPart(p);   // controller depths/assignments are per-part
        keyboardPanel->setCurrentPart(p);      // key range + velocity limits are per-part
        refreshDrumContext(p);                 // drum-edit box reflects this part's kit
        partPanel->applyState(audioProcessor.getEditorState());   // show the new part's values
        // The marks are per-PART, so every one of them now points at the wrong part --
        // and a part change moves no override, so the processor's override sequence
        // does not tick and nothing else recomputes them. While a transport is rolling
        // that sequence ticks constantly and hid this; stopped, the part just left kept
        // its marks painted over the part just arrived at until playback resumed.
        // USER-REPORTED 2026-09-18. Same reason the drum-key click refreshes its own.
        refreshMidiOverrides();
        // The Voice module offers the SELECTED row only, so every one of its stops has
        // just moved to a different row. Without this the ring stays painted over the
        // part you have left, claiming a control that is no longer the one the arrows
        // would act on. Re-measuring puts it on the same stop of the new row, which is
        // also what makes Ctrl+arrow and the arrows compose as intended.
        navUpdateRing();
        if (voiceBrowser->isVisible())
        {
            int msb, lsb, prg; partListPanel->getPartVoice(p, msb, lsb, prg);
            browserPart = p;
            voiceBrowser->showFor(msb, lsb, prg);
        }
    };

    // Voice browser is a full-editor overlay owned here (so it can cover the
    // whole window, not just the narrow Voice column).
    voiceBrowser = std::make_unique<VoiceBrowser>();
    // The browser walks its four areas with the SAME Controls bindings the editor's
    // modules use (Tab / Shift+Tab, left / right), so there is one key map and the
    // shortcut editor needs no rows of its own to describe the panel.
    // The Voice List draws its own focus frame; one setting governs both, or the two
    // would disagree about when a mark appears.
    voiceBrowser->markVisible     = [this] { return navMarkVisible(); };
    voiceBrowser->onKeyboardUsed  = [this] { navNoteKeyboardUse(); };
    // What a DRUMS buffer currently holds. PartListPanel is the authority: a part's
    // buffer is PART MODE, which a file load, a device read-back and a project restore
    // all write (PartListPanel::applyState), so this is the only answer that stays true
    // however the kit got there. Parts sharing a buffer are re-pointed together by the
    // onPick below, so the first one found speaks for all of them.
    voiceBrowser->kitForBuffer = [this](int buffer, int& msb, int& lsb, int& prg0)
    {
        if (partListPanel == nullptr || buffer < 0) return false;
        for (int p = 0; p < 32; ++p)
            if (partListPanel->getPartBuffer(p) == buffer)
            {
                partListPanel->getPartVoice(p, msb, lsb, prg0);
                return true;
            }
        return false;
    };
    //
    // THE VOICE LIST'S HALF OF THE ACCESSIBILITY KEY FLOOR.
    //
    // With the shortcut layer off -- its default when hosted -- this used to return -1
    // for everything, so the panel kept only Enter and Escape and the four regions
    // could not be walked at all. Up/down still moved within whichever column the panel
    // opened on, which made it look serviceable while it was in fact a DEAD END: the
    // category, the group and the DRUMS buffer box were all unreachable, so a voice
    // outside the current category could not be chosen by keyboard at any price.
    // Accessibility is not an option here, so the region keys get the
    // same floor PluginEditor::keyForFocusedControl gives the editor's controls: the
    // DEFAULT bindings, never the user's map, because with the layer off there is no
    // map in force and a fixed floor is something the manual can state plainly.
    //
    // TAB IS IN THIS ONE, and that is the deliberate difference from the editor's
    // floor. There Tab is left to JUCE's own traverser because the traverser does the
    // standard, expected thing across the canvas. Here it does not: no component in
    // this editor is a focus container, so an unconsumed Tab walks focus OUT of the
    // panel and into the controls behind it -- silently, with the panel still up and
    // its arrows now dead. A modeless panel that holds the keyboard has to own Tab for
    // as long as it is up.
    voiceBrowser->commandForKey = [this](const juce::KeyPress& k) -> int
    {
        if (audioProcessor.useKeyboardShortcuts)
            return audioProcessor.keyBindings.findCommand(k);

        for (const int cmd : { kbsc::NavGroupNext, kbsc::NavGroupPrev,
                               kbsc::NavPrev,      kbsc::NavNext })
            if (kbsc::Bindings::defaultFor(cmd) == k)
                return cmd;

        return -1;
    };
    voiceBrowser->onPick = [this](int msb, int lsb, int prg, juce::String name, int buffer,
                                  bool fromKeyboard)
    {
        if (browserPart < 0) return;

        // ONE undo step for the whole pick. It is a single user action, but it fans
        // out well past the three bank/program messages: a DRUMS kit re-points every
        // other part sharing the buffer, wipes that buffer's per-key plugin EQ/HPF,
        // and sets PART MODE. Scoped here rather than inside applyVoice, because the
        // scope has to span all of that, not just one part's messages.
        //
        // A pick made by ARROWING through the list takes a merge key, so a run of
        // them folds into one step -- exactly the XgSelector wheel case: each row is
        // a whole voice change, and a walk down a category would otherwise bury the
        // 20-deep stack. It still reverts to the voice held before the FIRST press,
        // because a merged scope never touches oldVal. A CLICK keeps key 0: that is
        // a deliberate separate action and stays its own step.
        YamahaMU100AudioProcessor::UndoScope undoStep(
            audioProcessor, "Voice Part " + juce::String(browserPart + 1) + " " + name,
            fromKeyboard ? mu100undo::gestureMergeKey(mu100undo::GestureVoicePick, browserPart)
                         : 0);

        // Which kit is LEAVING each buffer this pick touches, read BEFORE anything
        // below writes to the model. It has to be here, not inside resetDrumBuffer: by
        // the time that runs, sendPartMode may have put this part onto the buffer while
        // its own bank and program still name the kit it is coming FROM, and the peer
        // loop may already have moved the parts that were on it -- so the model no
        // longer agrees with itself about what the buffer held.
        //
        // TWO buffers can be involved, and missing the second was a reported bug:
        //  * `buffer`, the one being ASSIGNED, whose block resetDrumBuffer overwrites;
        //  * `leavingBuffer`, the one this part is moving OFF, which nothing writes
        //    now -- but undoing this pick re-sends the part's program with PART MODE
        //    back on it, and the device re-initialises it then.
        const int leavingBuffer = partListPanel->getPartBuffer(browserPart);

        int oldMsb = 0, oldLsb = 0, oldPgm = -1;
        if (buffer >= 0)
            audioProcessor.drumBufferKitFromModel(buffer, oldMsb, oldLsb, oldPgm);

        int lvMsb = 0, lvLsb = 0, lvPgm = -1;
        if (leavingBuffer >= 0 && leavingBuffer != buffer)
            audioProcessor.drumBufferKitFromModel(leavingBuffer, lvMsb, lvLsb, lvPgm);

        // PART MODE FIRST -- BEFORE any bank/program write, this part's or a peer's.
        // DRUMS buffer k -> mode k+2; a plain pick of a drum-bank kit (MSB 126/127) ->
        // mode 1 (generic Drum); a plain pick of a melodic voice -> mode 0 (Normal).
        // This makes an in-editor DRUMS assignment round-trip through the device.
        //
        // The ORDER is the whole point here, not housekeeping. Writing a part's PROGRAM
        // RE-INITIALISES the Drum Setup that part's PART MODE currently names (measured
        // against the device -- see sendPatchToDevice step 4; applyPartSetup has always
        // sent PART MODE ahead of the voice bytes for exactly this reason). So the mode
        // in force WHEN the program change lands decides which buffer the kit loads
        // into. Sent afterwards, as it was until 2026-09-07, every pick wrote the kit
        // into the part's OLD buffer:
        //   * picking a kit INTO a buffer left that buffer holding the PREVIOUS kit --
        //     the name and the swatch moved, the sound did not;
        //   * a plain (bufferless) pick on a part still in a DRUMS mode CLOBBERED that
        //     buffer, so every OTHER part sharing it changed sound while its own name
        //     and swatch sat still;
        //   * and a first pick on a fresh part took TWO clicks, because the first
        //     program change arrived while PART MODE was still 0 (Normal) -- Standard
        //     Kit the sole exception, being the kit mode 1 comes up on.
        // Carry the abandoned buffer's per-key edits in this step, or the undo that
        // puts the part back on it will let the device's re-init wipe them.
        if (leavingBuffer >= 0 && leavingBuffer != buffer)
            audioProcessor.preserveDrumBufferOnLeave(leavingBuffer, lvMsb, lvLsb, lvPgm);

        const int mode = (buffer >= 0) ? buffer + 2
                                       : ((msb == 126 || msb == 127) ? 1 : 0);
        partListPanel->sendPartMode(browserPart, mode);

        if (buffer >= 0)
        {
            // A kit allocated to a shared DRUMS buffer re-points every other part
            // already on that buffer to the same kit (the MU100/XGEdit model: a
            // program change to a buffer affects all parts using it).
            for (int q = 0; q < 32; ++q)
                if (q != browserPart && partListPanel->getPartBuffer(q) == buffer)
                    partListPanel->applyVoice(q, msb, lsb, prg, name);
            partListPanel->setPartBuffer(browserPart, buffer);
            // A new kit on this DRUMS buffer clears its per-drum-key plugin EQ/HPF
            // (matches the hardware drum params losing their values on kit change),
            // and repopulates the model's hardware-side block with the new kit's
            // defaults -- carrying the OUTGOING kit so the user's own per-key edits
            // are preserved for undo rather than overwritten.
            // prg is 0-based (it is the PROGRAM byte); the defaults table keys on
            // the 1-based program number MU100Drums.h uses, hence +1.
            audioProcessor.resetDrumBuffer(buffer, msb, lsb, prg + 1,
                                           oldMsb, oldLsb, oldPgm);
        }
        else
            partListPanel->setPartBuffer(browserPart, -1);  // plain pick: leave any buffer

        partListPanel->applyVoice(browserPart, msb, lsb, prg, name);

        pushDrumBufferMap();   // this part's drum-buffer membership may have changed

        // Refresh the module that is ON SCREEN, which is not always the part just
        // picked for. A kit assigned to a buffer changes the per-key values for EVERY
        // part on that buffer, so a peer being displayed is left just as stale as the
        // picked part would be -- same bug, one part sideways.
        const int shown = partListPanel->getSelectedPart();
        if (browserPart == shown
            || (buffer >= 0 && partListPanel->getPartBuffer(shown) == buffer))
            refreshDrumContext(shown);         // kit/buffer may have changed
    };
    // Closing the browser SEALS the open step, so a keyboard-navigation run cannot
    // absorb the next one. Without it, reopening within the coalesce window and
    // arrowing again would fold two separate browse sessions into a single undo
    // step -- the same reasoning as the HISTORY window's jump.
    voiceBrowser->onHide = [this]
    {
        audioProcessor.undoSealTop();
        // The browser held the keyboard (its columns take focus so the arrows can walk
        // them). Hiding it leaves JUCE focused on nothing, which sends every following
        // key to the host -- so hand the keyboard back to the editor.
        if (onKeyboardFocusReturn) onKeyboardFocusReturn();
    };

    // The browser is a canvas child so it rides the same design-space scale as the
    // rest of the editor (its fonts + rows scale with the window). Its geometry is
    // therefore in design coordinates.
    canvas.addChildComponent(*voiceBrowser);

    partListPanel->onRequestVoiceBrowse = [this](int part, int msb, int lsb, int prg)
    {
        browserPart = part;
        const auto design = canvas.getLocalBounds();
        if (voiceBrowser->getWidth() == 0)   // first open: size + centre (design px)
        {
            // 676 = the base 620 + col2's 16px + col3's 40px extra, so columns 1 and 2
            // keep their sizes and the added width all goes to column 3 (see VoiceBrowser::resized).
            const int w = juce::jmin(676, design.getWidth()  - 40);
            const int h = juce::jmin(470, design.getHeight() - 40);
            voiceBrowser->setBounds(juce::Rectangle<int>(0, 0, w, h)
                                        .withCentre(design.getCentre()));
        }
        else                                  // reopen: keep where the user left it
        {
            voiceBrowser->setBounds(voiceBrowser->getBounds().constrainedWithin(design));
        }
        voiceBrowser->showFor(msb, lsb, prg);
    };

    // Focus and the keyboard cursor are one thing (accessibility pillar 2): this is
    // the half that hears about focus the cursor did not move -- a click, Tab
    // traversal while the shortcut layer is off, or a screen reader grabbing focus.
    // Desktop-wide because JUCE has no per-subtree focus notification; the handler
    // ignores anything outside this canvas.
    juce::Desktop::getInstance().addFocusChangeListener(this);

    // Seeded here rather than only pushed from the host editor, so a view rebuilt by a
    // theme change comes up in the chosen mode. In `keyboard` mode nothing has been
    // pressed yet, so the mark starts hidden.
    navMarkMode  = audioProcessor.focusMarkMode;
    navMarkShown = (navMarkMode == 0);
    partPanel->setTitleNameMode(audioProcessor.partTitleNames);   // ...same reason

    refreshDrumContext(partListPanel->getSelectedPart());   // initial DRUM KEY box state
    refreshPluginOnlyLock();                               // honour the option on open
    refreshRcvRowVisible();                                // ...and this one
    refreshAutomationMarkers();                            // ...and the DAW-automatable marks
    refreshMidiMarkStyle();                                // ...and the override mark style
    refreshChangedMarks();                                 // ...and the changed markers

    // "Reading from device" overlay — added last so it sits above every panel and
    // the voice browser. Hidden until a read-back is in flight.
    readBackOverlay = std::make_unique<BusyOverlay>("Reading from device.",
                                                    "Please don't close this window");
    // "Stop reading" lets the user abort the auto read-back early — e.g. when they
    // intend to load a different .syx and don't need the initial device sync.
    readBackOverlay->onStop = [this] { cancelReadBack(); };
    readBackOverlay->setActionButton("Stop reading");
    addChildComponent(*readBackOverlay);

    // Seed the engine's part->buffer map from the initial part layout (the processor
    // re-pushes it to swp30 after boot via the drum-FX dirty flag).
    pushDrumBufferMap();
}

EditorView::~EditorView()
{
    juce::Desktop::getInstance().removeFocusChangeListener(this);
}

//==============================================================================
// Phase 5 device-state read-back.
void EditorView::requestReadBack(const std::vector<int>& fileDrumSetups)
{
    // Remember any file-derived drum setups so they survive until a run actually
    // starts (a request arriving while one is in flight is deferred, below).
    for (int s : fileDrumSetups)
        if (s >= 0 && s <= 3) pendingFileSetups.insert(s);

    // A read-back already in flight would otherwise pile a second full batch of dump
    // requests onto the device (and the drum requests are hundreds-strong). Instead
    // of dropping this request, queue it: pollReadBackTimeout() re-issues it when the
    // current run ends, so a load triggered during the open read-back isn't lost.
    if (readBackPending) { reReadPending = true; return; }

    readBackCancelled  = false;         // a fresh run supersedes any prior cancel tail
    readBackPending    = true;
    readBackFirstBurst = true;          // first reply burst clears + loads the model
    readBackRequestMs  = juce::Time::getMillisecondCounter();

    // Drop any stale capture residue so a prior tail can't merge into this
    // read-back's first burst (which would load a corrupted model).
    audioProcessor.clearDumpCapture();

    // Read back the DRUM SETUPs in use: any part on a DRUMS buffer 0..3, plus any
    // setup a just-loaded file wrote (pendingFileSetups — the part buffers aren't
    // refreshed until this read-back completes, so without this a file's 3rd/4th
    // drum setup wouldn't be requested on the same pass). Setup 0 is always included.
    // Reading every note of every setup is hundreds of requests, so skipping unused
    // setups keeps the round-trip short.
    std::set<int> setups { 0 };
    for (int p = 0; p < 32; ++p)
    {
        const int b = partListPanel->getPartBuffer(p);
        if (b >= 0 && b <= 3) setups.insert(b);
    }
    setups.insert(pendingFileSetups.begin(), pendingFileSetups.end());
    pendingFileSetups.clear();
    audioProcessor.requestDeviceDump(std::vector<int>(setups.begin(), setups.end()));

    // Show the "Reading from device" overlay and seed its time-based progress
    // estimate. requestDeviceDump paces one continuous request stream: ~16 global
    // blocks at ~20 ms, 32 parts x (4 chunks + 1 HPF 0A) at ~30 ms, then per drum
    // setup 79 notes x 4 sub-blocks at ~12 ms, ending one burst-gap (kReadBackBurstGapMs)
    // after the last reply. Match that so the bar tracks reality; pollReadBackTimeout
    // clamps it below 100% until truly complete.
    const int blockReqs = 16;
    const int partReqs  = 32 * 5;   // 4 x 08 chunks + 1 x 0A (HPF) per part
    const int drumReqs  = (int) setups.size() * 79 * 4;
    readBackStartMs    = readBackRequestMs;
    readBackExpectedMs = 200 + blockReqs * 22 + partReqs * 33 + drumReqs * 13 + kReadBackBurstGapMs;

    // The reply streams as a single burst delivered only after the request run goes
    // idle, so the first-burst window must outlast the whole run. Size it from the
    // estimate plus a generous margin for MAME running below real-time just after a
    // reset; otherwise a long read-back (many part chunks + several drum setups) can
    // time out before its reply lands — stranding the trailing dump into a spurious
    // "Save dump" dialog and leaving the panels on stale/default values.
    readBackFirstBurstLimitMs = juce::jmax(kReadBackInitialMs, readBackExpectedMs * 2 + 10000);

    if (readBackOverlay)
    {
        readBackOverlay->setProgress(0.0);
        readBackOverlay->setVisible(true);
        readBackOverlay->toFront(false);
    }

    // A read-back takes tens of seconds and the only cue is an overlay and a progress
    // bar. Told at the START rather than the end because that is when a user needs to
    // know to wait; finishReadBack says when it is over.
    announcer->say(mu100a11y::announce::Edit, "Reading from device");
}

bool EditorView::inReadBackGrace() const
{
    // After a user "Stop reading", keep swallowing dumps for the whole post-cancel
    // tail (cleared by pollReadBackTimeout once the device has gone quiet) — the tail
    // can outlast the fixed grace window if MAME is running below real-time.
    if (readBackCancelled) return true;
    return readBackEndedMs != 0
        && (juce::Time::getMillisecondCounter() - readBackEndedMs) < (juce::uint32) kReadBackGraceMs;
}

void EditorView::cancelReadBack()
{
    reReadPending = false;
    pendingFileSetups.clear();
    if (! readBackPending)
        return;
    readBackPending    = false;
    readBackFirstBurst = false;
    readBackEndedMs    = juce::Time::getMillisecondCounter();
    readBackCancelled  = true;            // swallow the trailing reply tail until idle
    // Drop the queued Dump-Request stream (hundreds of requests paced seconds ahead) so
    // the device stops replying and any subsequent .syx load isn't stuck behind them.
    audioProcessor.flushMidiInputPort0();
    audioProcessor.clearDumpCapture();
    if (readBackOverlay)
        readBackOverlay->setVisible(false);
}

void EditorView::pollReadBackTimeout()
{
    // Clear a "Stop reading" cancel once the device's trailing reply stream has truly
    // ended: past the fixed grace window AND its MIDI-OUT quiet for that long. The
    // grace floor guarantees at least kReadBackGraceMs of swallowing even if the idle
    // timer reads stale; the idle check extends it for a slow (below-real-time) tail.
    if (readBackCancelled)
    {
        const juce::uint32 now = juce::Time::getMillisecondCounter();
        if (now - readBackEndedMs >= (juce::uint32) kReadBackGraceMs
            && audioProcessor.dumpStreamIdleMs() >= (juce::uint32) kReadBackGraceMs)
        {
            readBackCancelled = false;
            audioProcessor.clearDumpCapture();   // drop any final residue before re-enabling saves
        }
    }

    if (! readBackPending) return;

    const juce::uint32 nowMs = juce::Time::getMillisecondCounter();

    // Advance the overlay's progress bar from the time estimate, clamped below 1.0
    // so it never claims "done" before the read-back actually completes.
    if (readBackOverlay && readBackExpectedMs > 0)
        readBackOverlay->setProgress(juce::jlimit(0.0, 0.99,
            (double)(nowMs - readBackStartMs) / (double) readBackExpectedMs));

    // Before any burst: allow a window long enough to outlast the whole request run
    // (sized in requestReadBack). After bursts have started: a short idle gap ends it.
    const juce::uint32 limit = readBackFirstBurst ? (juce::uint32) readBackFirstBurstLimitMs
                                                  : (juce::uint32) kReadBackBurstGapMs;
    if (nowMs - readBackRequestMs > limit)
    {
        readBackPending = false;
        readBackEndedMs = nowMs;        // open the grace window for trailing replies
        // Mark the model captured ONLY if at least one burst actually arrived, so an
        // interrupted (or answerless) read-back leaves editorStatePopulated false and
        // restarts cleanly on the next GUI open instead of stranding a partial model.
        if (! readBackFirstBurst)
            audioProcessor.editorStatePopulated = true;
        // Drop any reply residue still buffered so it can't finalize into a spurious
        // "Save dump" dialog; inReadBackGrace() suppresses any that arrive just after.
        audioProcessor.clearDumpCapture();
        if (readBackOverlay)
        {
            readBackOverlay->setProgress(1.0);
            readBackOverlay->setVisible(false);
        }
        // ...and that it is over. A read-back that answered nothing leaves the model
        // unpopulated, and saying so beats a silence that reads as "still going".
        announcer->say(mu100a11y::announce::Edit,
                       readBackFirstBurst ? "Read from device: no reply"
                                          : "Read from device complete");
        // Honour a read-back requested while this one was running (e.g. a file load
        // during the open read-back), now that the device is free again.
        if (reReadPending)
        {
            reReadPending = false;
            requestReadBack();
        }
    }
}

void EditorView::consumeDump(const std::vector<uint8_t>& bytes)
{
    // A large read-back (all drum notes) can arrive as several bursts. Keep the
    // request "pending" so every burst routes here; clear the model only on the
    // first burst and append the rest. Each burst extends the timeout window, and
    // pollReadBackTimeout() ends the read-back once the bursts stop.
    readBackRequestMs = juce::Time::getMillisecondCounter();
    if (readBackFirstBurst)
    {
        audioProcessor.loadEditorState(bytes.data(), bytes.size());   // clear + load
        readBackFirstBurst = false;
    }
    else
        audioProcessor.appendEditorState(bytes.data(), bytes.size()); // accumulate
    applyDeviceState();
}

void EditorView::notifyViewShown(bool /*shown*/)
{
    // Pass-through: the Graphic Display re-derives everything from isShowing(), so
    // it only needs to be told that NOW is a moment worth re-deciding.
    if (graphicDisplay != nullptr)
        graphicDisplay->refreshSpectrumOptions();
}

void EditorView::refreshSpectrumOptions()
{
    if (graphicDisplay != nullptr)
        graphicDisplay->refreshSpectrumOptions();
}

void EditorView::refreshPluginOnlyLock()
{
    // The panels speak in terms of a LOCK; the option is stated in the affirmative.
    // Invert once, here, rather than renaming the panels' own vocabulary -- greying a
    // control is what they do, and "locked" is the right word for it.
    const bool locked = ! audioProcessor.enablePluginOnlyParams;
    if (partPanel      != nullptr) partPanel->setPluginOnlyLocked(locked);
    if (graphicDisplay != nullptr) graphicDisplay->setPluginOnlyLocked(locked);
    // The System module has one plugin-only control of its own now (the system HPF).
    if (systemPanel    != nullptr) systemPanel->setPluginOnlyLocked(locked);
}

void EditorView::refreshAutomationMarkers()
{
    // The standalone exposes no host parameters, so an indicator there would point
    // at automation that does not exist. Decided here, once, rather than in each
    // panel: the panels only draw what they are handed.
    // ...and with DAW automation switched off there is nothing to mark either: the
    // parameters still EXIST (VST3 fixes the list at construction, so no per-project
    // switch can remove them) but the bridge that moves them is stopped, so a triangle
    // would point at a lane that records and does nothing. Their absence is the only
    // cue the user gets in the place they are actually looking.
    const bool hosted =
        audioProcessor.wrapperType != juce::AudioProcessor::wrapperType_Standalone
        && audioProcessor.dawAutomationEnabled;

    // The DAW mark is always the TRIANGLE now: the underline belongs to the MIDI CC
    // mark below, so the only question left is whether to draw it.
    const int marker = (hosted && audioProcessor.markDawAutomatable)
                           ? XgKnob::MarkerTriangle : XgKnob::MarkerNone;

    // The MIDI CC mark (the UNDERLINE) -- both hosts, since the standalone's MIDI
    // Output devices can be recorded too. Drawn only while MIDI Out is in CC mode:
    // in SysEx mode every XG edit records, so there is nothing to distinguish.
    const bool cc = audioProcessor.markMidiCcParams
                 && audioProcessor.midiOutFormat == YamahaMU100AudioProcessor::MidiOutCC;

    if (systemPanel != nullptr) systemPanel->setAutomationMarkers(marker);   // no CC params
    if (partPanel   != nullptr) partPanel->setAutomationMarkers(marker, cc);
    if (graphicDisplay != nullptr) graphicDisplay->setAutomationMarkers(marker, cc);
}

// Options -> "Show changed markers". A REPAINT is the whole update, and that is the
// feature's central property rather than an accident: the gutter every changed tab
// lives in is reserved unconditionally, so no module is re-laid-out and no value moves
// -- exactly the reason the Mute/Solo columns get a repaint and the override mark STYLE
// does not. Repainting this component covers every panel under it, which is what the
// option needs: the marks are drawn by six different surfaces and none of them holds a
// copy of the flag.
void EditorView::refreshChangedMarks()
{
    changedmarks::setShow(audioProcessor.showChangedMarks);
    repaint();
}

void EditorView::refreshMidiMarkStyle()
{
    // The global is what every ValueBox and XgKnob consults, so push it first; only
    // then does asking a panel to re-lay itself out give the right answer.
    midimarks::setStyle(audioProcessor.midiOverrideMarkStyle);
    if (partPanel         != nullptr) partPanel->refreshMidiMarkStyle();
    if (systemPanel       != nullptr) systemPanel->refreshMidiMarkStyle();
    if (multiEqPanel      != nullptr) multiEqPanel->refreshMidiMarkStyle();
    if (controllersPanel  != nullptr) controllersPanel->refreshMidiMarkStyle();
    if (effectPanel       != nullptr) effectPanel->refreshMidiMarkStyle();
    if (effectParamsPanel != nullptr) effectParamsPanel->refreshMidiMarkStyle();
}

// "MIDI input indicators" was switched. Unlike the other option mirrors this one is
// not cosmetic: PartListPanel's fade timer is stopped while the feature is off, and a
// stopped timer cannot notice the flag coming back.
void EditorView::refreshMidiIndicators()
{
    if (partListPanel != nullptr) partListPanel->refreshMidiIndicators();
}

void EditorView::refreshRcvRowVisible()
{
    if (partPanel != nullptr)
        partPanel->setRcvRowVisible(audioProcessor.showPartRcvSwitches);
}

// "Show Mute and Solo columns". PartListPanel derives its column widths from the
// processor flag at paint time, and the CH/EL boxes it positions in resized() sit to
// the LEFT of the new cells, so nothing has to be re-laid-out -- a repaint is the
// whole update. The canvas width is unaffected by design; see kDesignW.
void EditorView::refreshMuteSoloCols()
{
    if (partListPanel != nullptr) partListPanel->repaint();
}

// A mute or solo bit changed from somewhere other than the cell itself (the bottom
// bar's clear-all, or a second editor on the same processor).
void EditorView::refreshMuteSolo()
{
    if (partListPanel != nullptr) partListPanel->repaint();
}

// An EQ-bypass bit moved, from wherever: the lamp, the shortcut, or the bottom bar's
// clear-all. Both surfaces that show the state read the processor's mask directly, so
// this only has to tell them to look again.
void EditorView::refreshEqBypass()
{
    if (partPanel      != nullptr) partPanel->refreshEqBypass();
    if (graphicDisplay != nullptr) graphicDisplay->refreshEqIfShown();
}

// Options -> Voice names. The live global (voicenames::) is already switched by the
// time this runs; the two surfaces that show a voice name just have to be told to
// say it again. No SysEx, no model change -- purely a relabel.
void EditorView::refreshVoiceNames()
{
    if (partListPanel != nullptr) partListPanel->refreshVoiceNames();
    if (voiceBrowser  != nullptr) voiceBrowser->refreshVoiceNames();
    // The Part module title can be showing one of those names too, and it holds its own
    // copy of the string — so the relabel has to reach it or the title keeps the form
    // (full / 8-character) the option was just switched away from.
    pushPartTitleNames();
}

void EditorView::setPartTitleNames(int mask)
{
    if (partPanel == nullptr) return;
    partPanel->setTitleNameMode(mask);
    // The names may never have been pushed (the option can be turned on at any time),
    // and pushing is cheap, so do it here rather than making the panel ask.
    pushPartTitleNames();
}

// Hand the Part module the two strings its title can carry. It deliberately looks up
// NEITHER itself: PartListPanel owns the voice name (and with it Options > Voice
// names) and KeyboardPanel owns the drum tone name, so a lookup in PartPanel would be
// a second answer to a question that already has an owner - the failure shape of the
// VoiceBrowser drum-buffer cache. Called from every place either name can change:
// refreshDrumContext (part select, voice pick, init, applyDeviceState),
// refreshVoiceNames (the full/8-character option) and onDrumEditChanged (mode/key).
//
// Pushed whatever the option says. The strings are held by the panel, so turning the
// option on shows the CURRENT name rather than a stale one or a blank waiting for the
// next part change.
//
// The drum half is composed HERE, key and instrument together ("C1 KICK"), for the
// same reason the name is pushed at all: this is the only place that already holds
// both halves, and PartPanel looking either of them up would be a second answer to a
// question that has an owner. It also means the key follows Options > Label notes as
// with no extra path -- notelabel::text is the one place a note becomes text.
void EditorView::pushPartTitleNames()
{
    if (partPanel == nullptr) return;
    if (partListPanel != nullptr)
        partPanel->setPartVoiceName(partListPanel->voiceName(partListPanel->getSelectedPart()));
    // Only a real, occupied key has a tone to name. drumKeyText() reports "NO DRUM KEY"
    // for an empty one, and the title already says "empty key" in that state, so an
    // empty string here is what keeps it from saying both.
    const int  key   = keyboardPanel != nullptr ? keyboardPanel->effectiveDrumKey() : -1;
    const bool named = keyboardPanel != nullptr
                       && keyboardPanel->isDrumEditActive()
                       && key >= 0;
    partPanel->setDrumToneName(named ? notelabel::text(key) + " " + keyboardPanel->drumKeyText()
                                     : juce::String());
}

// Options > Label notes as. The global moves first (a panel reads it at paint time),
// then the two surfaces that cache text rather than painting it straight from the
// global: the Keyboard's own labels are painted, so a repaint is enough, but the Part
// module title holds a PUSHED string and would otherwise keep the old form until the
// next key change.
void EditorView::refreshNoteLabels()
{
    notelabel::setMode(audioProcessor.noteLabelMode);
    pushPartTitleNames();
    repaint();
}

void EditorView::applyDeviceState()
{
    const auto& st = audioProcessor.getEditorState();
    partPanel->applyState(st);
    if (graphicDisplay)    graphicDisplay->applyState(st);   // drum AMP EG read-back
    partListPanel->applyState(st);
    if (effectPanel)       effectPanel->applyState(st);
    if (effectParamsPanel) effectParamsPanel->applyState(st);
    if (multiEqPanel)      multiEqPanel->applyState(st);
    if (controllersPanel)  controllersPanel->applyState(st);
    if (systemPanel)       systemPanel->applyState(st);
    if (keyboardPanel)     keyboardPanel->applyState(st);
    refreshMidiOverrides();
    refreshDrumContext(partListPanel->getSelectedPart());
    pushDrumBufferMap();   // buffers may have changed with the new state (drum routing)
}

// The override marks. Called on its own trigger (the processor's override sequence)
// AND at the end of applyDeviceState, because a model change can retire a mark just
// as surely as an incoming message can raise one -- the shadow is defined as the
// DIFFERENCE between the two, so either side moving can change it.
void EditorView::refreshMidiOverrides()
{
    partPanel->refreshMidiOverrides();
    partListPanel->refreshMidiOverrides();   // the voice rows: the program-change case
    if (graphicDisplay) graphicDisplay->refreshMidiOverrides();   // the AMP EG ghosts
    if (keyboardPanel)  keyboardPanel->refreshMidiOverrides();    // the per-key strip

    // ...and the five modules that carry ordinary knobs and boxes. Every one of them
    // is reachable by a SysEx parameter change, so leaving them out made the feature
    // stop at the Part module for no reason the user could see.
    if (systemPanel)       systemPanel->refreshMidiOverrides();
    if (multiEqPanel)      multiEqPanel->refreshMidiOverrides();
    if (controllersPanel)  controllersPanel->refreshMidiOverrides();
    if (effectPanel)       effectPanel->refreshMidiOverrides();
    if (effectParamsPanel) effectParamsPanel->refreshMidiOverrides();
}

// The yellow row roll-ups. POLLED rather than driven by an edit, and that is the
// honest option rather than the lazy one: the model is written from ~50 places
// (every knob, every automation tick, every incoming dump) and none of them
// announces itself to a panel. Polling covers a source added tomorrow with no edit,
// which is the same argument undo makes for capturing at the funnel.
//
// The CALLER owns the rate. It is ~4 Hz, on the editor's existing dirty-marker
// divider, because the walk is two address ranges per part x 32 parts and a
// changed-marker need not be instant -- the same judgement the '*' already makes.
void EditorView::refreshRowChanged()
{
    partListPanel->refreshRowChanged();
    // The Keyboard module's per-key drum strip is the same kind of answer about a
    // different block, has no event to hang on for the same reason, and so rides the
    // same divider.
    if (keyboardPanel) keyboardPanel->refreshDrumMarks();
}

void EditorView::refreshDrumContext(int part)
{
    int msb = 0, lsb = 0, prg0 = 0;
    partListPanel->getPartVoice(part, msb, lsb, prg0);
    // The Keyboard's DRUM KEY box needs an *editable* drum part (a kit on a DRUMS
    // buffer, the only kind with a per-key DRUM SETUP to edit). The Graphic
    // Display's PITCH EG only needs to know the part is a drum part at all —
    // the device ignores PITCH EG for any kit voice, buffer or no buffer — so it
    // is told about kit-ness itself, the same test the voice-browser pick above
    // uses to drive PART MODE.
    const int buffer = partListPanel->getPartBuffer(part);
    keyboardPanel->setDrumContext(buffer >= 0, msb, lsb, prg0, buffer);
    // A DRUMS buffer -- not kit-ness -- is what kills the part-level SHELVES: only a
    // buffer brings the per-key DRUM SETUP that owns them instead. (HPF2 and the three
    // plugin mid bands stay live on such a part.) Both surfaces that draw the EQ need
    // to know.
    partPanel->setPartDrumBuffer(buffer);
    // ...and re-read the drum key on screen. setPartDrumBuffer above early-outs when
    // the buffer NUMBER has not changed, which is exactly the case a kit assignment
    // hits: same part, same buffer, same selected key, every value underneath it
    // different. Without this the drum controls went on showing the previous kit's
    // values -- the model and the device having both moved -- until the user clicked
    // another key and back. USER-REPORTED 2026-09-08.
    partPanel->refreshDrumKey(buffer);
    graphicDisplay->setPartDrumBuffer(buffer);
    const bool isKit = (msb == 126 || msb == 127);
    graphicDisplay->setDrumKitPart(isKit);
    // The same kit-ness test drives every drum-part exclusion the spec lists for the
    // MULTI PART table (sound_list.txt p46): PITCH EG on the Graphic Display above,
    // MONO/POLY + SCALE TUNING + PORTAMENTO in the Part module, FMOD/AMOD DEPTH in
    // the Controller module. Buffer membership is irrelevant — a plain kit pick
    // (PART MODE 1, no DRUMS buffer) is just as excluded.
    partPanel->setDrumKitPart(isKit);
    controllersPanel->setDrumKitPart(isKit);
    pushPartTitleNames();   // the part's voice may have changed with all of the above
}

void EditorView::pushDrumBufferMap()
{
    for (int p = 0; p < 32; ++p)
        audioProcessor.setPartDrumBuffer(p, partListPanel->getPartBuffer(p));
}

//==============================================================================
// Keyboard-shortcut entry points. See the header for why each routes through the
// same handler the mouse does.

void EditorView::selectPart(int part0to31)
{
    if (partPanel == nullptr) return;
    partPanel->setCurrentPart(juce::jlimit(0, 31, part0to31));
}

void EditorView::stepSelectedPart(int delta)
{
    // A part step MOVES the mark (the Voice module offers the selected row only), so by
    // the same rule as the nav keys it reveals it.
    navNoteKeyboardUse();
    const int cur = getCurrentPart();
    if (cur < 0) return;
    // Wraps rather than clamping: the 32 parts are a ring in every other part of the
    // editor (the bar axis, the Voice list scroll), and a shortcut held down that goes
    // silent at part 32 reads as broken.
    selectPart(((cur + delta) % 32 + 32) % 32);
}

void EditorView::setDisplayMode(int mode)
{
    if (graphicDisplay != nullptr)
        graphicDisplay->selectMode(mode);
}

void EditorView::toggleMuteSolo(bool solo, bool exclusive)
{
    const int part = getCurrentPart();
    if (partListPanel == nullptr || part < 0) return;
    partListPanel->handleMsClick(part, solo, exclusive);
}

void EditorView::openVoiceListForCurrentPart()
{
    const int part = getCurrentPart();
    if (partListPanel == nullptr || part < 0 || ! partListPanel->onRequestVoiceBrowse)
        return;

    int msb = 0, lsb = 0, prg0 = 0;
    partListPanel->getPartVoice(part, msb, lsb, prg0);
    partListPanel->onRequestVoiceBrowse(part, msb, lsb, prg0);
}

int EditorView::getCurrentPart() const
{
    return partPanel != nullptr ? partPanel->getCurrentPart() : -1;
}

// The keyboard route to the part menu (Shift+F10). It deliberately uses the SELECTED
// part rather than whatever the keynav cursor is on: every item on the menu acts on the
// selected part, and the cursor can be sitting in a module that has nothing to do with
// one.
void EditorView::requestPartContextMenu()
{
    const int p = getCurrentPart();
    if (p >= 0 && onPartContextMenu) onPartContextMenu(p);
}

int EditorView::getCurrentPartDrumBuffer() const
{
    const int part = getCurrentPart();
    return (partListPanel != nullptr && part >= 0) ? partListPanel->getPartBuffer(part) : -1;
}

EditorView::Xform EditorView::transform() const
{
    const float scale = juce::jmin((float) getWidth()  / kDesignW,
                                   (float) getHeight() / kDesignH);
    const float ox = ((float) getWidth()  - kDesignW * scale) * 0.5f;
    const float oy = ((float) getHeight() - kDesignH * scale) * 0.5f;
    return { scale, ox, oy };
}

void EditorView::paint(juce::Graphics& g)
{
    g.fillAll(edcol::kViewBg);
    // All modules are now real children of the canvas; the letterbox margins
    // around the scaled canvas show this fill (as do any gaps between modules).
}

void EditorView::resized()
{
    // Lay the canvas out ONCE at design coordinates (its size is fixed), then scale
    // the whole thing to the window with a single transform. The modules position
    // themselves relative to the canvas, i.e. in design pixels.
    canvas.setBounds(0, 0, juce::roundToInt(kDesignW), juce::roundToInt(kDesignH));
    const auto t = transform();
    canvas.setTransform(juce::AffineTransform::scale(t.scale).translated(t.ox, t.oy));

    partPanel->setBounds(kPartRect.toNearestInt());
    partListPanel->setBounds(kVoiceRect.toNearestInt());
    graphicDisplay->setBounds(kGraphicRect.toNearestInt());
    effectPanel->setBounds(kEffectRect.toNearestInt());
    effectParamsPanel->setBounds(kEffectParamsRect.toNearestInt());
    multiEqPanel->setBounds(kMultiEqRect.toNearestInt());
    controllersPanel->setBounds(kControllersRect.toNearestInt());
    systemPanel->setBounds(kSystemRect.toNearestInt());
    keyboardPanel->setBounds(kKeyboardRect.toNearestInt());

    // Voice browser lives in the canvas (design space); keep it within bounds.
    if (voiceBrowser && voiceBrowser->isVisible())
        voiceBrowser->setBounds(voiceBrowser->getBounds().constrainedWithin(canvas.getLocalBounds()));

    // The read-back overlay is a direct child of the view (unscaled) so it covers
    // the full window, letterbox margins included.
    if (readBackOverlay)
        readBackOverlay->setBounds(getLocalBounds());
}


//==============================================================================
// Keyboard control of the value boxes — the cursor (see KeyNav.h).
//
// Modules in canvas READING ORDER, which is the Tab order. Authored rather than
// derived from the rects: the canvas is not a clean grid (the Graphic Display spans
// two columns' worth of height), and Tab order is a usability decision, not a
// geometric fact. A panel that was never built is simply absent.
//==============================================================================
std::vector<keynav::Group*> EditorView::navGroups() const
{
    std::vector<keynav::Group*> g;
    auto add = [&g](keynav::Group* p) { if (p != nullptr) g.push_back(p); };

    add(systemPanel.get());
    add(multiEqPanel.get());
    add(effectPanel.get());
    add(effectParamsPanel.get());
    add(graphicDisplay.get());
    add(partPanel.get());
    add(controllersPanel.get());
    add(partListPanel.get());
    add(keyboardPanel.get());

    // SORTED INTO STRICT READING ORDER from the modules' own rectangles: rows top to
    // bottom, left to right within a row. The list above is only "which panels exist";
    // the ORDER is derived, so a module that is added or moved lands in the right place
    // with no second list to update -- the same reason keynav::collect re-runs per
    // keystroke rather than caching.
    //
    // The order this yields is System, Graphic Display, Voice, Multi EQ, Effect,
    // Controller, Effect Parameters, Part, Keyboard. The Voice module coming third is
    // the honest consequence of it being the top-RIGHT module while spanning almost the
    // whole canvas height; the alternative -- hand-listing a "nicer" order -- is what
    // this replaced, and it had drifted into putting Controller after Part for no
    // reason anyone could state.
    //
    // Rows are BANDED, like the controls within a module, so a module nudged a few px
    // does not silently jump the queue. The current rows are exact (y = 14, 136, 276,
    // 402, 524, 810), comfortably further apart than the tolerance.
    constexpr int kRowTol = 24;   // design px

    std::sort(g.begin(), g.end(), [](keynav::Group* a, keynav::Group* b)
    {
        auto* ca = dynamic_cast<juce::Component*>(a);
        auto* cb = dynamic_cast<juce::Component*>(b);
        if (ca == nullptr || cb == nullptr) return ca != nullptr;
        if (ca->getY() != cb->getY()) return ca->getY() < cb->getY();
        return ca->getX() < cb->getX();
    });

    for (size_t i = 0; i < g.size(); )
    {
        auto* first = dynamic_cast<juce::Component*>(g[i]);
        const int bandY = first != nullptr ? first->getY() : 0;

        size_t j = i;
        while (j < g.size())
        {
            auto* c = dynamic_cast<juce::Component*>(g[j]);
            if (c == nullptr || std::abs(c->getY() - bandY) > kRowTol) break;
            ++j;
        }

        std::sort(g.begin() + (long) i, g.begin() + (long) j,
                  [](keynav::Group* a, keynav::Group* b)
                  {
                      auto* ca = dynamic_cast<juce::Component*>(a);
                      auto* cb = dynamic_cast<juce::Component*>(b);
                      if (ca == nullptr || cb == nullptr) return ca != nullptr;
                      return ca->getX() < cb->getX();
                  });
        i = j;
    }
    return g;
}

juce::Component* EditorView::navGroupComponent(int group) const
{
    const auto g = navGroups();
    if (group < 0 || group >= (int) g.size()) return nullptr;
    return dynamic_cast<juce::Component*>(g[(size_t) group]);
}

std::vector<keynav::Item> EditorView::navCurrentItems() const
{
    const auto g = navGroups();
    if (navGroup < 0 || navGroup >= (int) g.size()) return {};
    return g[(size_t) navGroup]->navItems();
}

// Land on the first USABLE stop at or after `item` in `group`, walking whole groups
// in `dir` when a group offers none. Bounded by the group count so a canvas where
// every control is disabled terminates instead of spinning.
bool EditorView::navPlace(int group, int item, int dir)
{
    const auto g = navGroups();
    if (g.empty()) return false;

    const int n = (int) g.size();
    if (dir == 0) dir = 1;

    for (int tries = 0; tries <= n; ++tries)
    {
        const int gi = ((group % n) + n) % n;
        auto* comp = navGroupComponent(gi);
        auto  items = g[(size_t) gi]->navItems();

        if (comp != nullptr && ! items.empty())
        {
            const int count = (int) items.size();
            int i = juce::jlimit(0, count - 1, item < 0 ? (dir > 0 ? 0 : count - 1) : item);

            for (int step = 0; step < count; ++step, i += dir)
            {
                if (i < 0 || i >= count) break;
                if (keynav::usable(items[(size_t) i], *comp))
                {
                    navGroup = gi;
                    navItem  = i;
                    navUpdateRing();
                    navFocusCursor();
                    return true;
                }
            }
        }

        group = gi + dir;
        item  = -1;   // entering a fresh group: start at whichever end we came in from
    }

    navClear();
    return false;
}

void EditorView::setFocusMarkMode(int mode)
{
    if (navMarkMode == mode) return;
    navMarkMode = mode;
    // Switching TO "always" must reveal the mark at once rather than waiting for the
    // next keystroke, or the option looks like it did nothing.
    if (navMarkMode == 0) navMarkShown = true;
    navUpdateRing();
    if (voiceBrowser != nullptr) voiceBrowser->repaint();
}

// Any key that moves or acts on the mark reveals it. Deliberately NOT every key: Alt+A
// opens About and has nothing to do with the cursor.
void EditorView::navNoteKeyboardUse()
{
    if (navMarkShown) return;
    navMarkShown = true;
    navUpdateRing();
    if (voiceBrowser != nullptr) voiceBrowser->repaint();
}

bool EditorView::navMarkVisible() const
{
    return navMarkMode == 0 || navMarkShown;
}

void EditorView::navClear()
{
    navGroup = navItem = -1;
    navRing.setVisible(false);
}

void EditorView::navUpdateRing()
{
    auto* comp = navGroupComponent(navGroup);
    const auto items = navCurrentItems();

    if (comp == nullptr || navItem < 0 || navItem >= (int) items.size()
        || ! navMarkVisible()
        || (voiceBrowser != nullptr && voiceBrowser->isVisible()))
    {
        navRing.setVisible(false);
        return;
    }

    // A selector's pop-up list is MODAL, so while it is open the plugin editor sees no
    // keys at all. Hand it a way back out: left/right close the list and carry the
    // cursor on. Installed here rather than at construction because only the control
    // the cursor is on can need it, and this runs on every placement -- including the
    // one a mouse click makes, so a clicked-open list has it too.
    if (auto* sel = dynamic_cast<XgSelector*>(items[(size_t) navItem].comp))
        sel->onNavigateAway = [this](int dir) { navStepItem(dir); };

    // A 2px ring OUTSIDE the control, so it frames the value instead of covering it.
    // Two design px, not one: the canvas is scaled to the window (~0.81 at the default
    // size), and a 1px rule lands on a fraction of a device pixel and antialiases to
    // grey — the same measurement that widened the automation-marker underline.
    const auto mark = keynav::ringBounds(items[(size_t) navItem], canvas);
    navRing.cue = keynav::cueFor(mark);
    navRing.setBounds(mark.expanded(2));
    navRing.setVisible(true);
    navRing.toFront(false);
    navRing.repaint();
}

void EditorView::NavRing::paint(juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced(1.0f);
    g.setColour(edcol::kNavFocus);

    if (cue == keynav::Item::Cue::Ring)
    {
        g.drawRoundedRectangle(r, 2.5f, 2.0f);
        return;
    }

    // Corner brackets -- see keynav::Item::Cue. Both ENDS in full (they sit in the
    // cell's margins, clear of left-aligned text) plus a short stub of the top and
    // bottom edges at each corner, so nothing is drawn alongside the glyphs. The stub
    // is a quarter of the width, capped, so a narrow cell still reads as a frame
    // rather than as four disconnected ticks.
    const float stub = juce::jlimit(4.0f, 10.0f, r.getWidth() * 0.25f);

    juce::Path p;
    p.startNewSubPath(r.getX() + stub, r.getY());
    p.lineTo(r.getX(), r.getY());
    p.lineTo(r.getX(), r.getBottom());
    p.lineTo(r.getX() + stub, r.getBottom());

    p.startNewSubPath(r.getRight() - stub, r.getY());
    p.lineTo(r.getRight(), r.getY());
    p.lineTo(r.getRight(), r.getBottom());
    p.lineTo(r.getRight() - stub, r.getBottom());

    g.strokePath(p.createPathWithRoundedCorners(2.5f), juce::PathStrokeType(2.0f));
}

// The Voice List is a modeless overlay that takes the keyboard: its columns hold
// focus, and anything it does not consume BUBBLES past it to the editor's shortcut
// handler. That made every Controls key act on the canvas UNDERNEATH the panel --
// invisibly, because navUpdateRing already hides the ring while the browser is up.
// A ListBox eats up/down, so the visible half was silent, but left/right and Tab
// walked the hidden cursor and BACKSPACE reset whatever control it had reached.
// The cursor's state is deliberately left alone (the panel is not a mode change);
// only acting on it is refused, so closing the browser resumes where it left off.
bool EditorView::navBlocked() const
{
    return voiceBrowser != nullptr && voiceBrowser->isVisible();
}

bool EditorView::navStepGroup(int delta)
{
    if (navBlocked()) return false;
    navNoteKeyboardUse();
    if (navGroup < 0) return navPlace(0, -1, 1);          // first press: enter at the top
    return navPlace(navGroup + (delta >= 0 ? 1 : -1), -1, delta >= 0 ? 1 : -1);
}

bool EditorView::navStepItem(int delta)
{
    if (navBlocked()) return false;
    navNoteKeyboardUse();
    if (navGroup < 0) return navPlace(0, -1, 1);

    const int dir = (delta >= 0 ? 1 : -1);
    const auto items = navCurrentItems();
    const int  next  = navItem + dir;

    // Past either end of a module, carry on into the next one. Walking off the end of
    // the Part module and into the Controller module is what makes left/right a single
    // continuous path through the editor, with Tab as the way to skip a module whole.
    if (next < 0 || next >= (int) items.size())
        return navPlace(navGroup + dir, -1, dir);

    return navPlace(navGroup, next, dir);
}

bool EditorView::navAdjust(int dir, bool fine)
{
    if (navBlocked()) return false;
    navNoteKeyboardUse();
    if (navGroup < 0) return navPlace(0, -1, 1);   // nothing selected yet: just show up

    auto* comp = navGroupComponent(navGroup);
    const auto items = navCurrentItems();
    if (comp == nullptr || navItem < 0 || navItem >= (int) items.size()) return false;

    const auto& it = items[(size_t) navItem];
    if (! keynav::usable(it, *comp)) return navPlace(navGroup, navItem, 1);

    keynav::adjust(it, dir, fine);
    // An adjust can move the thing it is standing on (a Voice-module mute repaints the
    // row; a drum-key arm rebuilds the Part module), so the ring is re-measured rather
    // than assumed to be where it was.
    navUpdateRing();
    return true;
}

// The horizontal axis of a two-dimensional stop. Declines -- handing the key to the
// host -- for every stop that has only one axis, which is all of them but the Graphic
// Display's envelope nodes. That is what lets `,` and `.` be unmodified keys.
bool EditorView::navAdjustX(int dir, bool fine)
{
    if (navBlocked()) return false;
    navNoteKeyboardUse();
    if (navGroup < 0) return false;   // no cursor: nothing of ours to move

    auto* comp = navGroupComponent(navGroup);
    const auto items = navCurrentItems();
    if (comp == nullptr || navItem < 0 || navItem >= (int) items.size()) return false;

    const auto& it = items[(size_t) navItem];
    if (! it.adjustX || ! keynav::usable(it, *comp)) return false;

    it.adjustX(dir, fine);
    navUpdateRing();   // the node MOVES with its value; the mark follows it
    return true;
}

// The THIRD axis: an EQ mid-band node's Q. Declines exactly as navAdjustX does, for
// every stop that has no third parameter -- which is all of them but those three nodes
// -- so `[` and `]` reach the host from everywhere else in the editor.
bool EditorView::navAdjustQ(int dir, bool fine)
{
    if (navBlocked()) return false;
    navNoteKeyboardUse();
    if (navGroup < 0) return false;   // no cursor: nothing of ours to move

    auto* comp = navGroupComponent(navGroup);
    const auto items = navCurrentItems();
    if (comp == nullptr || navItem < 0 || navItem >= (int) items.size()) return false;

    const auto& it = items[(size_t) navItem];
    if (! it.adjustQ || ! keynav::usable(it, *comp)) return false;

    it.adjustQ(dir, fine);
    // Q does not move the node the way gain and frequency do, but the mark is
    // re-measured anyway rather than special-cased: it is one rect, and this axis is
    // the one most likely to gain a stop whose handle DOES move with it.
    navUpdateRing();
    return true;
}

bool EditorView::navActivate()
{
    if (navBlocked()) return false;
    navNoteKeyboardUse();
    if (navGroup < 0) return navPlace(0, -1, 1);

    auto* comp = navGroupComponent(navGroup);
    const auto items = navCurrentItems();
    if (comp == nullptr || navItem < 0 || navItem >= (int) items.size()) return false;

    const auto& it = items[(size_t) navItem];
    if (! keynav::usable(it, *comp)) return false;

    keynav::activate(it);
    navUpdateRing();
    return true;
}

// Ctrl+Enter. Deliberately NOT a fallback to navActivate: a stop either has a second
// action or the key is not ours. Returning false is what lets a DAW keep Ctrl+Enter
// everywhere in this editor except a bar column.
bool EditorView::navActivateAlt()
{
    if (navBlocked()) return false;
    if (navGroup < 0) return false;

    auto* comp = navGroupComponent(navGroup);
    const auto items = navCurrentItems();
    if (comp == nullptr || navItem < 0 || navItem >= (int) items.size()) return false;

    const auto& it = items[(size_t) navItem];
    if (! keynav::usable(it, *comp) || ! it.activateAlt) return false;

    navNoteKeyboardUse();
    keynav::activateAlt(it);
    navUpdateRing();
    return true;
}

bool EditorView::navReset()
{
    if (navBlocked()) return false;
    navNoteKeyboardUse();
    if (navGroup < 0) return false;   // nothing to reset, so let the host have the key

    auto* comp = navGroupComponent(navGroup);
    const auto items = navCurrentItems();
    if (comp == nullptr || navItem < 0 || navItem >= (int) items.size()) return false;

    const auto& it = items[(size_t) navItem];
    if (! keynav::usable(it, *comp)) return false;

    keynav::reset(it);
    navUpdateRing();
    return true;
}

//==============================================================================
// ONE ACTIVE CONTROL, NOT TWO.
//
// Without this the white ring could sit on one control while the mouse was typing
// into another, which reads as two things being focused at once -- and leaves the
// arrow keys pointing somewhere the user has stopped looking. So a press or a wheel
// takes the cursor with it.
//
// It also turns out to be the better workflow rather than merely the tidier one:
// click a knob to get there in one movement, then hold Shift and arrow to land on an
// exact value.
void EditorView::mouseDown(const juce::MouseEvent& e)              { navPointAt(e); }
// A dragged control can MOVE -- the Graphic Display's envelope nodes are drawn at a
// position their own value sets -- so the mark has to be re-measured as the drag runs,
// or it sits where the handle was. Cheap: a rect lookup on one stop.
void EditorView::mouseDrag(const juce::MouseEvent&)
{
    if (navGroup >= 0) navUpdateRing();
}
void EditorView::mouseWheelMove(const juce::MouseEvent& e,
                                const juce::MouseWheelDetails&)    { navPointAt(e); }

void EditorView::navPointAt(const juce::MouseEvent& e)
{
    auto* hit = e.eventComponent;
    if (hit == nullptr) return;

    // The Voice List owns the keyboard while it is up -- the same rule the nav KEYS
    // already follow (navBlocked), now applied to the mouse, because since the cursor
    // became juce focus this function MOVES FOCUS and so could take it off the panel.
    // It did, two ways, and both left the browser's arrows and Return dead:
    //
    //  * The click that OPENS the browser. canvas.addMouseListener(this, true) means
    //    this runs as a LISTENER, after PartListPanel's own mouseDown has already
    //    shown the panel and focused a column -- and that click is on the voice-name
    //    painted cell, a stop, so navFocusCursor() pulled focus onto its proxy.
    //  * A click on a ROW of the open browser. It matches no nav group, so the tail
    //    below handed the keyboard back to the plugin editor.
    //
    // Returning here leaves the cursor where it was, which is what closing the browser
    // is meant to resume to; the ring stays hidden meanwhile (navUpdateRing).
    if (navBlocked()) return;

    // The mouse is in use, so in `keyboard` mode the mark goes away -- and it is the
    // PRESS that hides it, never mere movement. The cursor still moves underneath: the
    // next nav key both acts on this control and reveals the mark on it.
    if (navMarkMode != 0) navMarkShown = false;

    const auto groups = navGroups();

    for (int gi = 0; gi < (int) groups.size(); ++gi)
    {
        auto* gc = navGroupComponent(gi);
        if (gc == nullptr || (gc != hit && ! gc->isParentOf(hit))) continue;

        const auto items = groups[(size_t) gi]->navItems();

        // A real control: the event landed ON it, or inside it (a click on an XgKnob
        // arrives at the inner dial or value box, never at the wrapper).
        for (int i = 0; i < (int) items.size(); ++i)
        {
            const auto& it = items[(size_t) i];
            if (it.isPainted() || it.comp == nullptr) continue;
            if (it.comp == hit || it.comp->isParentOf(hit))
            {
                if (! keynav::usable(it, *gc)) break;
                navGroup = gi; navItem = i; navUpdateRing();
                navFocusCursor();
                return;
            }
        }

        // A painted cell has no component of its own, so it is matched by POSITION in
        // the panel that draws it -- the Voice module's Mute/Solo and voice-name cells,
        // and the Keyboard module's DRUM KEY box.
        const auto p = gc->getLocalPoint(hit, e.getPosition());
        for (int i = 0; i < (int) items.size(); ++i)
        {
            const auto& it = items[(size_t) i];
            if (it.isPainted() && it.cell.contains(p))
            {
                navGroup = gi; navItem = i; navUpdateRing();
                // A painted cell's proxy is mouse-TRANSPARENT, so the click landed on
                // the panel and focus walked up to the plugin editor. Put it on the
                // proxy, or a screen reader is left describing the window while the
                // ring sits on a cell -- and the accessibility key floor, which asks
                // whether a stop is focused, would decline.
                navFocusCursor();
                return;
            }
        }
        break;   // inside this module but on nothing navigable
    }

    // Module background, a label, the piano keys, the graphic display's plot: the click
    // activated nothing the cursor can describe, so it should describe nothing.
    //
    // Clearing the ring is no longer enough on its own. Now that controls take keyboard
    // focus, JUCE's own click handling has ALREADY given the focus to something: a
    // component that does not want it passes the click's focus up to its parent, and a
    // parent with no interest hands it to its focus traverser's DEFAULT child -- the
    // top-left control on the canvas. The focus listener would then walk the cursor
    // straight back onto that control, so a click on empty space silently left the
    // cursor on the System module's first knob. Hand the keyboard back to the plugin
    // editor instead, which is where it sits when nothing is selected.
    navClear();
    if (onKeyboardFocusReturn) onKeyboardFocusReturn();
}

//==============================================================================
// Accessibility pillar 2 -- the cursor and JUCE keyboard focus are ONE thing.
//
// A screen reader has no way to be told "read this control" other than by moving
// KEYBOARD focus to it (AccessibilityHandler::grabFocus does both), so a cursor that
// is not focus is a cursor a screen reader cannot see. These two functions keep the
// two in step in both directions, and navFindStopFor is the shared lookup.
//
// The plugin editor remains the single READER of keys: none of these controls
// handles one. The XgKnob WRAPPER is the focus stop, not its inner juce::Slider, so
// juce::Slider::keyPressed -- which would eat unmodified arrows -- is never reached.

// The stop that owns `c`: the stop's own component, or an ancestor of `c` (a click or
// a type-in editor lands on a child, never on the XgKnob wrapper).
bool EditorView::navFindStopFor(juce::Component* c, int& group, int& item) const
{
    if (c == nullptr) return false;

    const auto groups = navGroups();
    for (int gi = 0; gi < (int) groups.size(); ++gi)
    {
        auto* gc = navGroupComponent(gi);
        if (gc == nullptr || (gc != c && ! gc->isParentOf(c))) continue;

        const auto items = groups[(size_t) gi]->navItems();
        for (int i = 0; i < (int) items.size(); ++i)
        {
            const auto& it = items[(size_t) i];
            // The focus target, so a painted cell is found by its PROXY -- which is
            // the component a screen reader will have focused.
            auto* t = it.focusTarget();
            if (t == nullptr) continue;
            if (t == c || t->isParentOf(c))
            {
                // A stop the cursor may not stand on (a bare slot, a control the
                // plugin-only lock or drum-edit mode has greyed) is not one to move
                // to either -- the same question keynav asks everywhere else.
                if (! keynav::usable(it, *gc)) return false;
                group = gi; item = i;
                return true;
            }
        }
        return false;   // inside this module, but not on one of its stops
    }
    return false;
}

// Cursor -> focus. Called from navPlace only, never from navUpdateRing: a ring
// re-measure also happens after navActivate, which has just opened a type-in editor,
// and grabbing focus there would close it again.
void EditorView::navFocusCursor()
{
    if (navGroup < 0 || navItem < 0) return;

    const auto items = navCurrentItems();
    if (navItem >= (int) items.size()) return;

    // A PAINTED cell has no component of its own, so focus goes to the invisible
    // proxy laid over it (pillar 3, keynav::Item::a11yProxy).
    auto* target = items[(size_t) navItem].focusTarget();
    if (target == nullptr || ! target->isShowing()) return;
    if (target->hasKeyboardFocus(false)) return;

    // Taking focus also calls SetFocus on our window. That is safe HERE and would not
    // be from a timer: the cursor only moves in response to a key or a click we
    // received, which means the window already had the keyboard.
    const juce::ScopedValueSetter<bool> guard(navMovingFocus, true);
    target->grabKeyboardFocus();
}

// Focus -> cursor: something else moved focus (a click, JUCE's Tab traverser while
// our shortcut layer is off, or a screen reader). Bring the ring to it.
void EditorView::globalFocusChanged(juce::Component* focused)
{
    if (navMovingFocus || focused == nullptr) return;
    if (navBlocked()) return;   // the Voice List owns the keyboard while it is up

    int g = -1, i = -1;
    if (! navFindStopFor(focused, g, i)) return;   // not one of ours: leave the ring alone
    if (g == navGroup && i == navItem) return;

    navGroup = g;
    navItem  = i;
    navUpdateRing();
}

bool EditorView::navHasFocusedStop() const
{
    if (navGroup < 0 || navItem < 0) return false;

    auto* f = juce::Component::getCurrentlyFocusedComponent();
    int g = -1, i = -1;
    return navFindStopFor(f, g, i) && g == navGroup && i == navItem;
}
