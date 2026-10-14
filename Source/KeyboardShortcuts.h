// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) Dave Wheitner and MEGAN XG contributors

#pragma once

#include <JuceHeader.h>
#include <array>

//==============================================================================
// KeyboardShortcuts — the plugin's command table and its key bindings.
//
// Deliberately NOT juce::ApplicationCommandManager. That class wants to own a
// menu bar and a focus-following command target chain; this plugin builds its
// menus by hand as PopupMenus from one editor class, and a hosted plugin has no
// application-wide key mapping to install into. What is actually needed is
// small: a stable list of commands, one KeyPress each, and a lookup from a
// key event to a command id. That is this file.
//
// Two rules the whole design hangs on:
//
//  * A binding NEVER swallows a key the plugin does not own. The editor's
//    keyPressed() returns false for anything not found here, so it bubbles to
//    the host — a DAW's transport, its own shortcuts and its text fields keep
//    working. This is why the defaults below all carry a modifier: an unmodified
//    letter would be taken from the host the moment the plugin window has focus.
//
//  * Shortcuts describe the INSTALLATION, not the song. They live in
//    settings.xml alongside the other global options and never travel in DAW
//    project state — see reference_mu100_settings_scope_map. Storing them in
//    both places would let a project silently rewrite the user's key map.
//
// Only bindings that DIFFER from the default are written to settings.xml. That
// keeps the file small, and it means a later change to a default reaches every
// user who never touched that row instead of being frozen by their settings.
//==============================================================================
namespace kbsc
{

//==============================================================================
// Command ids. The order is the order the shortcut editor lists them in, so it
// is grouped by category. Ids are persisted BY STRING (see kCommands), not by
// this integer, so inserting a command in the middle cannot scramble a saved map.
//
// The nine Display* entries MUST stay in GraphicDisplayPanel::Mode order —
// the handler converts with `mode = cmd - DisplayVolume`.
enum Cmd
{
    FileNew = 0,
    FileOpen,
    FileSave,
    FileSaveAs,
    FileExportSyx,
    FileReadDevice,
    FileExit,

    EditUndo,
    EditRedo,
    EditHistory,

    PartPrev,
    PartNext,
    PartPrev8,
    PartNext8,
    PartVoiceList,
    PartCopy,
    PartPaste,
    PartReset,
    PartMenu,

    NavGroupNext,
    NavGroupPrev,
    NavPrev,
    NavNext,
    NavIncrease,
    NavDecrease,
    NavIncreaseFine,
    NavDecreaseFine,
    NavIncreaseX,
    NavDecreaseX,
    NavIncreaseXFine,
    NavDecreaseXFine,
    NavIncreaseQ,
    NavDecreaseQ,
    NavIncreaseQFine,
    NavDecreaseQFine,
    NavActivate,
    NavActivateAlt,
    NavReset,

    MixMute,
    MixSolo,
    MixMuteExclusive,
    MixSoloExclusive,
    MixEqBypass,
    MixEqBypassAll,
    MixClear,

    DisplayVolume,
    DisplayPan,
    DisplayAmpEG,
    DisplayPitchEG,
    DisplayReverb,
    DisplayChorus,
    DisplayVar,
    DisplayEQ,
    DisplayHPF2,

    ViewToggle,
    ViewZoomIn,
    ViewZoomOut,
    ViewZoomReset,

    DevicePanic,

    // Its own category, not Help: in the menus it is an Options item, and a shortcut
    // editor listing itself under the wrong heading is the first thing a reader checks.
    OptionsShortcuts,

    HelpManual,
    HelpAbout,

    NumCommands
};

//==============================================================================
struct CommandInfo
{
    const char* id;          // stable key for settings.xml — never renumber, never rename
    const char* category;    // section heading in the shortcut editor
    const char* name;        // what the editor and the menus call it
    const char* defaultKey;  // juce::KeyPress::createFromDescription syntax; "" = unbound
};

// The defaults. Two families of modifier, chosen for what they collide with:
//
//   Ctrl+...        only where taking it from the host is the POINT (undo/redo).
//   Ctrl+Alt+...    the File commands. A focused plugin swallowing the DAW's
//                   Ctrl+S is far worse than swallowing its Ctrl+Z, so the File
//                   menu deliberately sits off the plain Ctrl row.
//   Alt+...         everything that has no host equivalent to collide with.
//
// Nothing here is bound to an unmodified key except F1 (the Windows convention
// for help) — see the note at the top of the file.
inline constexpr CommandInfo kCommands[NumCommands] =
{
    { "file.new",          "File",    "New setup",                    "ctrl + alt + N" },
    { "file.open",         "File",    "Open file...",                 "ctrl + alt + O" },
    { "file.save",         "File",    "Save setup",                   "ctrl + alt + S" },
    { "file.saveAs",       "File",    "Save setup as...",             "ctrl + alt + shift + S" },
    { "file.exportSyx",    "File",    "Export multi 32-part syx...",  "ctrl + alt + E" },
    { "file.readDevice",   "File",    "Read from device",             "ctrl + alt + R" },
    { "file.exit",         "File",    "Exit (standalone only)",       "" },

    { "edit.undo",         "Edit",    "Undo",                         "ctrl + Z" },
    { "edit.redo",         "Edit",    "Redo",                         "ctrl + Y" },
    { "edit.history",      "Edit",    "History window",               "alt + H" },

    { "part.prev",         "Parts",   "Previous part",                "ctrl + cursor left" },
    { "part.next",         "Parts",   "Next part",                    "ctrl + cursor right" },
    { "part.prev8",        "Parts",   "Back 8 parts",                 "ctrl + shift + cursor left" },
    { "part.next8",        "Parts",   "Forward 8 parts",              "ctrl + shift + cursor right" },
    { "part.voiceList",    "Parts",   "Open voice list for part",     "alt + return" },

    // Copy / paste / reset of a whole part. CTRL+ALT, not plain Ctrl, for the reason
    // the File family sits there: Ctrl+C and Ctrl+V are the two chords a DAW user most
    // expects to reach the HOST, and a focused plugin swallowing them would take clip
    // copy/paste away with nothing on screen saying so.
    //
    // Reset takes ctrl + alt + Backspace because Backspace alone already means "reset
    // this control" (nav.reset) -- one idiom, two scopes.
    //
    // NOTE for anyone on a layout with AltGr, which IS Ctrl+Alt: these can arrive as a
    // character key instead. The whole File family already carries that exposure, and
    // every row here is rebindable.
    { "part.copy",         "Parts",   "Copy part settings",           "ctrl + alt + C" },
    { "part.paste",        "Parts",   "Paste part settings",          "ctrl + alt + V" },
    { "part.reset",        "Parts",   "Reset part to defaults",       "ctrl + alt + backspace" },
    // The context menu from the keyboard, at the part the cursor is on -- the Windows
    // convention, and what keeps the three above reachable without a mouse for anyone
    // who does not want to learn three chords.
    { "part.menu",         "Parts",   "Part menu",                    "shift + F10" },

    // Keyboard control of the value boxes (KeyNav.h). Unmodified keys, which is
    // unusual here and is the point: this is a mode you work IN, so every keystroke
    // in it has to be one key. All four are free -- nothing else in the plugin claims
    // Tab or a bare arrow, and the Parts rows above take the SAME arrows under Ctrl.
    //
    // Left/right navigate and up/down adjust, rather than XGEdit's arrangement where
    // arrows tried to do both -- which is why XGEdit's value half never worked. Up
    // increases, matching a drag; plain travels one wheel detent and Shift lands on
    // one step, matching the wheel.
    //
    // NOT bound to "+" and "-", though XGEdit offered them and they are free: JUCE's
    // Windows peer reports the UNSHIFTED character for that key, so it arrives as "="
    // whether or not Shift is held and `shift + =` is a different chord entirely (the
    // same measurement that made zoom `ctrl + =`). Binding "increase" to "=" would
    // silently do nothing for anyone who presses the key the way it is printed. They
    // are one rebind away for anyone who wants them.
    //
    // NOT bound to Space either: Space is transport play/stop in essentially every
    // DAW, and a focused plugin swallowing it would be a genuinely bad trade. Enter
    // is already this editor's "commit / act" key.
    { "nav.groupNext",     "Controls", "Next module",                 "tab" },
    { "nav.groupPrev",     "Controls", "Previous module",             "shift + tab" },
    { "nav.prev",          "Controls", "Previous control",            "cursor left" },
    { "nav.next",          "Controls", "Next control",                "cursor right" },
    { "nav.increase",      "Controls", "Increase value",              "cursor up" },
    { "nav.decrease",      "Controls", "Decrease value",              "cursor down" },
    { "nav.increaseFine",  "Controls", "Increase value (fine)",       "shift + cursor up" },
    { "nav.decreaseFine",  "Controls", "Decrease value (fine)",       "shift + cursor down" },

    // The HORIZONTAL axis, for a stop that has two -- the Graphic Display's envelope
    // nodes, where up/down is the level and these are the time, and its EQ nodes, where
    // up/down is the gain and these are the centre frequency.
    //
    // `.` and `,` because the caps print `>` and `<` above them, and because JUCE's
    // Windows peer reports the UNSHIFTED character for an OEM key: these ARE the
    // unshifted characters, so the binding matches the key as printed. (That same
    // measurement is why zoom is `ctrl + =` and not `ctrl + +`.) They are the only
    // other unmodified keys in this table, and they are safe for the same reason the
    // arrows are: a stop with no horizontal axis declines them, so everywhere else
    // in the plugin `,` and `.` still reach the host.
    { "nav.increaseX",     "Controls", "Increase value (horizontal)",  "." },
    { "nav.decreaseX",     "Controls", "Decrease value (horizontal)",  "," },
    { "nav.increaseXFine", "Controls", "Increase value (horizontal, fine)", "shift + ." },
    { "nav.decreaseXFine", "Controls", "Decrease value (horizontal, fine)", "shift + ," },

    // A THIRD axis, for the one kind of stop that carries three parameters at once:
    // the Graphic Display's EQ nodes, where up/down is the band's gain, `.` / `,` its
    // centre frequency and these its Q. (Only the three plugin-only mid bands have a
    // Q at all -- the hardware BASS/TREBLE shelves do not, and decline these keys.)
    //
    // `[` and `]` for the same reason as `.` and `,`: they ARE the unshifted characters
    // of those OEM keys, so the binding matches the cap as printed, and they are free
    // both here and in the hosts. `]` narrows the band, which reads as the pair
    // "widen / narrow" laid out left to right.
    //
    // NOT alt + cursor up/down, though the MOUSE spells Q as an Alt-drag on this same
    // screen: Windows and most hosts claim Alt+arrow for themselves (menu activation,
    // back/forward), so the keyboard half of that mirror would be intercepted before
    // the plugin ever saw it.
    { "nav.increaseQ",     "Controls", "Increase value (third axis, e.g. EQ Q)",  "]" },
    { "nav.decreaseQ",     "Controls", "Decrease value (third axis, e.g. EQ Q)",  "[" },
    { "nav.increaseQFine", "Controls", "Increase value (third axis, fine)", "shift + ]" },
    { "nav.decreaseQFine", "Controls", "Decrease value (third axis, fine)", "shift + [" },
    { "nav.activate",      "Controls", "Type value / press control",  "return" },
    // The SECONDARY action, where a stop has two. Only the Graphic Display's bar
    // columns do today (select the part the bar belongs to), and everywhere else the
    // key is declined so the host keeps it -- the same rule as the `.`/`,` axes.
    { "nav.activateAlt",   "Controls", "Secondary action (select a bar's part)", "ctrl + return" },
    { "nav.reset",         "Controls", "Reset control to default",    "backspace" },

    { "mix.mute",          "Mixer",   "Mute part",                    "alt + M" },
    { "mix.solo",          "Mixer",   "Solo part",                    "alt + S" },
    { "mix.muteExclusive", "Mixer",   "Mute part exclusively",        "alt + shift + M" },
    { "mix.soloExclusive", "Mixer",   "Solo part exclusively",        "alt + shift + S" },
    // EQ bypass sits with Mute and Solo because it is the same KIND of control -- live,
    // per-part, not part of the patch, and used the same way: engaged for a few seconds
    // to hear what something is doing. `alt + E` is free (the File menu's export is
    // ctrl+alt+E), and the shift form follows the exclusive mute/solo convention above:
    // shift widens the reach.
    { "mix.eqBypass",      "Mixer",   "Bypass part EQ",               "alt + E" },
    { "mix.eqBypassAll",   "Mixer",   "Bypass EQ on all parts",       "alt + shift + E" },
    { "mix.clear",         "Mixer",   "Clear all mutes, solos and EQ bypasses", "alt + shift + C" },

    { "disp.volume",       "Display", "Display: Volume",              "alt + 1" },
    { "disp.pan",          "Display", "Display: Pan",                 "alt + 2" },
    { "disp.ampEG",        "Display", "Display: Amp EG",              "alt + 3" },
    { "disp.pitchEG",      "Display", "Display: Pitch EG",            "alt + 4" },
    { "disp.reverb",       "Display", "Display: Reverb",              "alt + 5" },
    { "disp.chorus",       "Display", "Display: Chorus",              "alt + 6" },
    { "disp.var",          "Display", "Display: Variation",           "alt + 7" },
    { "disp.eq",           "Display", "Display: EQ",                  "alt + 8" },
    { "disp.hpf2",         "Display", "Display: HPF",                 "alt + 9" },

    { "view.toggle",       "View",    "Switch Hardware / Editor view", "alt + V" },
    // Zoom is a WINDOW RESIZE -- the editor is one canvas scaled to fit, so growing the
    // window is the only thing "zoom" can mean here. `ctrl + =` rather than `ctrl + +`
    // because that is the key Windows actually reports: the +/= key sends its UNSHIFTED
    // character, so holding Shift for the "+" would be a different chord entirely. It is
    // the same physical key either way, and the same one browsers use.
    { "view.zoomIn",       "View",    "Zoom in (5%)" ,                "ctrl + =" },
    { "view.zoomOut",      "View",    "Zoom out (5%)",               "ctrl + -" },
    { "view.zoomReset",    "View",    "Reset window size",            "ctrl + 0" },

    { "device.panic",      "Device",  "PANIC (all notes off)",        "ctrl + alt + P" },

    // The persisted id stays "help.shortcuts" though the category moved to Options --
    // ids are the settings.xml key and must never be renamed, or every user's custom
    // binding for this row is silently dropped.
    { "help.shortcuts",    "Options", "Edit keyboard shortcuts...",   "alt + B" },

    { "help.manual",       "Help",    "Open the manual",              "F1" },
    { "help.about",        "Help",    "About MEGAN XG",               "alt + A" },
};

//==============================================================================
// A key map: one KeyPress per command plus a per-command on/off, matching the
// checkbox column in the editor window. "Off" is not the same as "unbound": an
// unbound row has no key to show anywhere, while a disabled row keeps its key
// so switching it back on restores what the user had.
class Bindings
{
public:
    Bindings() { resetAll(); }

    juce::KeyPress get(int cmd) const
    {
        return isValid(cmd) ? keys[(size_t) cmd] : juce::KeyPress();
    }
    bool isEnabled(int cmd) const
    {
        return isValid(cmd) && on[(size_t) cmd];
    }
    void set(int cmd, const juce::KeyPress& k, bool enabled)
    {
        if (! isValid(cmd)) return;
        keys[(size_t) cmd] = k;
        on  [(size_t) cmd] = enabled;
    }

    static juce::KeyPress defaultFor(int cmd)
    {
        if (! isValid(cmd)) return {};
        const juce::String d(kCommands[cmd].defaultKey);
        return d.isEmpty() ? juce::KeyPress()
                           : juce::KeyPress::createFromDescription(d);
    }
    bool isDefault(int cmd) const
    {
        return isValid(cmd) && on[(size_t) cmd] && keys[(size_t) cmd] == defaultFor(cmd);
    }
    void resetToDefault(int cmd)
    {
        if (! isValid(cmd)) return;
        keys[(size_t) cmd] = defaultFor(cmd);
        on  [(size_t) cmd] = true;
    }
    void resetAll()
    {
        for (int c = 0; c < NumCommands; ++c)
            resetToDefault(c);
    }

    // The command a key event should run, or -1. Disabled and unbound rows are
    // skipped, so a key they hold is left to bubble up to the host.
    int findCommand(const juce::KeyPress& k) const
    {
        if (! k.isValid()) return -1;
        for (int c = 0; c < NumCommands; ++c)
            if (on[(size_t) c] && keys[(size_t) c].isValid() && keys[(size_t) c] == k)
                return c;
        return -1;
    }

    // The command already holding `k`, ignoring `except` — the conflict test the
    // shortcut editor runs before it commits a Learn. Disabled rows COUNT here:
    // silently letting two rows share a key would make re-enabling one of them
    // change what the other does, with nothing on screen having said so.
    int conflictWith(const juce::KeyPress& k, int except) const
    {
        if (! k.isValid()) return -1;
        for (int c = 0; c < NumCommands; ++c)
            if (c != except && keys[(size_t) c].isValid() && keys[(size_t) c] == k)
                return c;
        return -1;
    }

    //== display ===============================================================
    // JUCE spells its descriptions lower case ("ctrl + cursor left"); the UI wants
    // Surge's capitalised form ("Ctrl + cursor left").
    //
    // THE RULE for the key half: a key that carries a PRINTED LABEL on the cap is
    // shown as that label, capitalised (Enter, Tab, Backspace — like "F1" or "Z");
    // a key that carries no label of its own keeps JUCE's lower-case DESCRIPTION,
    // because that is a phrase and not a name ("cursor left", "spacebar"). So a
    // chord reads "Ctrl + Enter" or "Ctrl + cursor left", and the mixed case is
    // the rule working rather than an oversight.
    //
    // "return" in particular is JUCE's internal vocabulary and was never meant to
    // be read by a user; every keyboard this runs on says Enter.
    //
    // DISPLAY ONLY — do NOT let any of this reach `writeTo`/`readFrom` or the
    // defaults in kCommands. Those round-trip through getTextDescription() /
    // createFromDescription(), which speaks only JUCE's spelling; rewriting the
    // stored form would fail to parse AND silently drop every user's custom
    // binding on that key.
    static juce::String describe(const juce::KeyPress& k)
    {
        if (! k.isValid()) return {};
        juce::String d = k.getTextDescription();

        // Keys whose caps carry a printed label. Matched on the KEY CODE, not on the
        // text, so a description that happens to contain one of these words cannot be
        // rewritten by accident.
        struct Cap { int code; const char* juceName; const char* label; };
        static const Cap caps[] =
        {
            { juce::KeyPress::returnKey,    "return",    "Enter"     },
            { juce::KeyPress::tabKey,       "tab",       "Tab"       },
            { juce::KeyPress::backspaceKey, "backspace", "Backspace" },
            { juce::KeyPress::deleteKey,    "delete",    "Delete"    },
            { juce::KeyPress::insertKey,    "insert",    "Insert"    },
            { juce::KeyPress::homeKey,      "home",      "Home"      },
            { juce::KeyPress::endKey,       "end",       "End"       },
            { juce::KeyPress::pageUpKey,    "page up",   "Page Up"   },
            { juce::KeyPress::pageDownKey,  "page down", "Page Down" },
            // "Escape" rather than the cap's "Esc": the manual spells it out, and a
            // shortcut editor disagreeing with the manual is worse than a long word.
            { juce::KeyPress::escapeKey,    "escape",    "Escape"    },
        };

        for (const auto& c : caps)
            if (k.getKeyCode() == c.code)
            {
                const juce::String name(c.juceName);
                if (d.endsWith(name))
                    d = d.dropLastCharacters(name.length()) + c.label;
                break;
            }

        for (auto* m : { "ctrl", "alt", "shift", "command", "cmd" })
        {
            const juce::String lower(m);
            juce::String title = lower.substring(0, 1).toUpperCase() + lower.substring(1);
            if (lower == "cmd" || lower == "command") title = "Cmd";
            d = d.replace(lower + " + ", title + " + ", /*ignoreCase*/ false);
        }
        return d;
    }
    // What a menu item or tooltip should show for `cmd` — empty when shortcuts
    // are switched off wholesale, when this row is off, or when it is unbound.
    juce::String descriptionFor(int cmd, bool globallyEnabled) const
    {
        if (! globallyEnabled || ! isEnabled(cmd)) return {};
        return describe(get(cmd));
    }

    //== persistence ===========================================================
    // Non-default rows only, as <shortcut> children of settings.xml's root.
    void writeTo(juce::XmlElement& root) const
    {
        for (int c = 0; c < NumCommands; ++c)
        {
            if (isDefault(c)) continue;
            auto* el = root.createNewChildElement("shortcut");
            el->setAttribute("id", kCommands[c].id);
            el->setAttribute("key", keys[(size_t) c].isValid()
                                        ? keys[(size_t) c].getTextDescription()
                                        : juce::String());
            el->setAttribute("enabled", on[(size_t) c] ? 1 : 0);
        }
    }
    void readFrom(const juce::XmlElement& root)
    {
        resetAll();   // anything the file does not mention keeps the current default
        for (auto* el : root.getChildWithTagNameIterator("shortcut"))
        {
            const int c = commandForId(el->getStringAttribute("id"));
            if (c < 0) continue;                    // a row from a newer build: ignore it
            const juce::String desc = el->getStringAttribute("key");
            keys[(size_t) c] = desc.isEmpty() ? juce::KeyPress()
                                              : juce::KeyPress::createFromDescription(desc);
            on[(size_t) c] = el->getIntAttribute("enabled", 1) != 0;
        }
    }

    static int commandForId(const juce::String& id)
    {
        for (int c = 0; c < NumCommands; ++c)
            if (id == kCommands[c].id) return c;
        return -1;
    }

private:
    static bool isValid(int cmd) { return cmd >= 0 && cmd < NumCommands; }

    std::array<juce::KeyPress, NumCommands> keys;
    std::array<bool,           NumCommands> on { };
};

} // namespace kbsc
