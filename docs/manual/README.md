# Editing this manual

The user manual is written as one Markdown file per chapter in this directory and
built into a single self-contained HTML file for reading and sharing.

## Building

```
cd docs/manual
python build_manual.py
```

That writes `MEGAN-XG-Manual.html` — one file, no external assets except the
images in `images/`, which are referenced relatively. Open it in any browser. It
follows the browser's light/dark setting and prints reasonably.

**`{{LAST_UPDATED}}` is substituted at build time** with the date the newest
chapter last changed, so the title page's "Last updated" line maintains itself.
Per chapter that is its last COMMIT date, or its file time if it has uncommitted
edits — never the build clock, so a rebuild that changes nothing does not
restamp. Both halves matter: the file time alone would stamp every fresh clone
(including CI, which publishes the manual) with the clone date, and the commit
date alone would miss the edit you are making right now. The token works in any
chapter, but there is only one use for it and it lives in `00-index.md`.

`python build_manual.py --inline-images` embeds every screenshot in the HTML
instead — one ~5 MB file that works on its own, which is what each release ships.

## Publishing

Nothing to do by hand. `.github/workflows/manual.yml` rebuilds the manual on every
push to `master` that touches `docs/manual/`, runs `tools/check_manual.py` (a
failure stops the publish), and deploys it to
[bboxdave.github.io/megan-xg](https://bboxdave.github.io/megan-xg/) — the address the plugin's
**Help ▸ MEGAN XG Manual...** falls back to. Publishing a GitHub Release also
attaches the single-file `--inline-images` build to that release.

`build_manual.py` picks up **every `.md` file in this directory whose name starts
with a digit or `A`**, in filename order. That is the whole build configuration —
to add a chapter, create `NN-name.md` with a number that sorts it into place. This
`README.md` is excluded on purpose.

## Chapter layout

| | |
|---|---|
| `00-index.md` | Title page, what the plugin is, how to read the manual |
| `01`–`02` | Installation and the overall screen |
| `03` | Controls — how the mouse works everywhere in the editor |
| `04`–`13` | One chapter per Editor module, plus drum editing |
| `14`–`16` | Menus, options and settings, file formats |
| `17`–`19` | Hardware view, DAW integration, standalone |
| `20`–`22` | Workflows, differences from the hardware, troubleshooting |
| `A1`–`A5` | Appendices |

## Conventions

- Menu paths use `▸`, e.g. `Options ▸ Display Animation...`, and must match the
  on-screen strings **exactly**. When a menu item is renamed in the code, grep this
  directory for the old string.
- Callouts are blockquotes with a bold lead. Three are styled by the builder:

  ```
  > **Standalone only** — …
  > **VST only** — …
  > **Watch out** — …
  ```

- Control tables give **name, what it does, range, default**, in that order.
- Ranges and defaults come from `mu100editor/data/parameters.json` (`display_raw`
  and `default_raw`). Don't type them from memory — that file is generated from the
  MU100 MIDI Data book and is the same source the plugin's own display rules use.
- The body of the manual doesn't mention MAME, source files, or emulation
  internals. Rationale appears only where it changes what the reader should *do*.

## Screenshots

Images live in `images/`. Places that still need one carry a placeholder and a
capture note:

```markdown
![The Part module](images/part-module.png)
<!-- CAPTURE: Editor view, dark theme, part 5 selected, a drum key NOT selected,
     EQ grid visible. Crop to the Part module plus 8px. -->
```

Until a file exists, the builder renders a labeled **"Screenshot to come"** box
rather than a broken-image icon, and reports how many are outstanding. Drop the
file into `images/` under the name in the placeholder and rebuild — the box
becomes the image with no edit to the chapter.

`CAPTURE-LIST.md` is the running checklist of what still needs shooting, derived
from those notes:

```
python gen_capture_list.py
```

Re-run it after adding, renaming or removing a placeholder. Do not hand-edit it.

To take or retake shots, see **`tools/README.md`** — `tools/ShotKit.ps1` drives
the running standalone and `tools/Compose.ps1` rebuilds the four derived images
(the module map, the two-view comparison, the annotated bottom bar, the control
montage) from the raw captures.

## Checking the manual against the code

```
python build_manual.py
python tools/check_manual.py
```

Five mechanical checks, each of which has caught a real defect: broken tables
(a table interrupted by prose and resumed without a fresh header row renders as
a run-on paragraph of pipe characters), unresolved internal links,
unsubstituted `{{TOKENS}}`, menu item strings that do not appear verbatim in
some chapter, and `settings.xml` keys the code reads that `A2` does not
document. Exit code is the number of failures.

**It deliberately does not proof-read prose.** A scan for the phrasing that
goes stale -- "the only two settings...", "Twelve settings" -- was tried and
returned 388 hits, nearly all ordinary English. A check that cries wolf is
worse than no check, because you learn to skim it. What works is comparing the
manual against something machine-readable.

**So the residue is real and has to be caught by reading.** A claim about a set
that the code defines but does not enumerate in one place -- how many settings
live in the project, how many rows a dialog has -- is the class of error to
watch for when adding to a feature that already has a count written down
somewhere else. Grep for the neighbouring nouns before adding to such a set.

## Generated appendices

`A1-effect-reference.md` is **generated** — do not hand-edit it:

```
python gen_effect_appendix.py
```

It reads `../../mu100editor/data/effects.json`, `effect_conv.json` and
`tables.json`. Re-run it whenever that data pipeline is re-run.
