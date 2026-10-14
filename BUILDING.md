# Building MEGAN XG

This builds the **Windows x64 VST3 plugin and standalone application** from source.
Those are the only formats and platform the project builds. The other JUCE formats are
not switched off by choice: they would need a macOS or Linux build of the emulation
libraries first, and the plugin's two JUCE wrapper patches are VST3-specific.

Expect the first build to take a while — most of the time is the emulator libraries.

## What you need

- **Windows 10 or 11, x64.**
- **Visual Studio 2026** (Community is fine) with the **Desktop development with C++**
  workload. Both halves of the build use its v145 toolset.
- **Python 3, callable as `python3`.** The emulator build runs Python scripts during
  compilation (layouts, fonts, version resources, the driver list). The Python install
  manager from python.org and the Microsoft Store both provide `python3`; an older
  python.org installer may give you only `python` and `py`. If `python3 --version` does
  not answer in a new terminal, fix that before building.
- **Git**, and **Windows PowerShell 5.1** (included with Windows) for the patch script.
- **The MU100 ROMs**, to run what you build. They are not included and never will be —
  see [ROMs](#7-roms).

Run every command below in **PowerShell**, not Git Bash: MSBuild's `/p:` switches do not
survive Git Bash's argument handling, and the failure can look like a successful build
that did nothing.

## 1. Lay out the folders

The project refers to its neighbours by relative path, so the three trees must sit
side by side under one folder. The folder itself can be anywhere and can have any name:

```
<any folder>/
  libs/JUCE/          JUCE 8.0.13 (step 2)
  libs/mame-mu100/    the patched emulator (step 4)
  megan-xg/           this repository
```

```powershell
mkdir C:\src\megan, C:\src\megan\libs
cd C:\src\megan
git clone https://github.com/bboxdave/megan-xg.git
```

## 2. Get JUCE 8.0.13 — exactly that version

Download **`juce-8.0.13-windows.zip`** from the
[JUCE 8.0.13 release](https://github.com/juce-framework/JUCE/releases/tag/8.0.13) and
extract it into `libs\` — the zip holds a single `JUCE` folder, so you end up with
`libs\JUCE\modules\` and `libs\JUCE\Projucer.exe`.

Why the version matters:

- **The patches in step 3 apply to 8.0.13 only.** Another version will reject them, or
  worse, take them in the wrong place.
- **Use the Projucer from that same zip.** A Projucer from another JUCE release stamps
  its own version into the generated project, and the 8.0.13 modules stop the build
  with `#error "...outdated version of the Projucer..."`. If you already have JUCE
  installed elsewhere, its Projucer is the wrong one.

## 3. Patch JUCE

The plugin needs two small edits inside JUCE: two named MIDI input ports, runtime
output-bus names and a per-host MIDI CC fix in the VST3 wrapper, and three changes to
the standalone window. Each sits behind a macro that only this project defines, so any
other project using the same JUCE folder still gets stock JUCE behaviour.

```powershell
cd C:\src\megan\megan-xg
powershell -ExecutionPolicy Bypass -File patches\juce\apply-juce-patches.ps1
```

Both lines should end in `[ OK ] ... applied`. Running it again prints `[SKIP]` for both,
which is how you check later that nothing has been lost; `-Verify` reports without
changing anything. The script finds `..\libs\JUCE` on its own; pass `-JucePath` if yours
is elsewhere.

**If a patch reports `[FAIL]`,** see `patches\juce\README.md` before editing anything by
hand. The patches were verified against the 8.0.13 download in both line-ending styles,
so a failure almost always means a JUCE that is not 8.0.13.

## 4. Build the emulator libraries

MEGAN XG runs an emulated MU100 from a patched copy of MAME (a development snapshot
taken just after 0.287 — its README names the exact upstream commit), linked in as static
libraries.

```powershell
cd C:\src\megan\libs
git clone https://github.com/bboxdave/megan-xg-mame.git mame-mu100
cd mame-mu100
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" `
    "build\projects\windows\mamemu100\vs2022\mamemu100.sln" `
    /p:Configuration=Release /p:Platform=x64 /m /v:minimal
```

Use the **`amd64`** MSBuild as shown — the 32-bit one in the same folder adds a toolchain
warning to an x64 build. The libraries land in `build\vs2022\bin\x64\Release\`, where
the plugin project expects them.

The same build also produces **`mu100.exe`** in the repository root: a standalone
command-line MAME for the MU100 alone. The plugin does not need it, but it is useful for
testing the emulation with no plugin in the way, e.g.
`mu100.exe mu100 -rompath <ROM folder> -midiin1 song.mid -video none -nothrottle -str 40`.

## 5. Generate the Visual Studio project

`Builds\` is not in the repository; the Projucer generates it from `MEGAN XG.jucer`:

```powershell
cd C:\src\megan\megan-xg
Start-Process -FilePath "..\libs\JUCE\Projucer.exe" `
    -ArgumentList '--resave','"MEGAN XG.jucer"' -Wait -NoNewWindow
```

**Keep the `-Wait`.** The Projucer is a GUI program, so starting it any other way
returns immediately and a build started next reads a project that is not written yet.
Run this again whenever `MEGAN XG.jucer` changes.

## 6. Build the plugin

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" `
    "Builds\VisualStudio2026\MEGAN XG.sln" /p:Configuration=Release /p:Platform=x64 /m /v:minimal
```

The results:

- **VST3:** `Builds\VisualStudio2026\x64\Release\VST3\MEGAN XG.vst3`
- **Standalone:** `Builds\VisualStudio2026\x64\Release\Standalone Plugin\MEGAN XG.exe`

**Close any running `MEGAN XG.exe` first,** or the link fails with `LNK1104` because the
file is locked.

**Installing the VST3 is up to you** — the build does not copy it anywhere. Copy the whole
`MEGAN XG.vst3` folder into `C:\Program Files\Common Files\VST3\` (which needs an
administrator prompt), with your DAW **closed**: a DAW that has the plugin loaded locks
the file, the copy fails, and the DAW goes on loading the old build.

## 7. ROMs

The plugin looks for **`mu100.zip`** in **`Documents\MEGAN XG\`**. It must hold exactly
these ten files — a missing one stops the emulator from starting:

| | File |
|---|---|
| Firmware (v1.11) | `xu50720.ic11` |
| Wave ROM | `xs518b0.ic34`, `xs743b0.ic35`, `xt445a0-828.ic36`, `xt461a0-829.ic37`, `xt462a0.ic39`, `xt463a0.ic38` |
| Sound chip | `sin-table.bin` |
| LCD | `hd44780u_b04.bin`, `mulcd.svg` |

The last three are easy to overlook because they are not "the ROM", but the emulator
refuses to start without any one of them. The user manual's *Getting started* chapter
covers this in more detail; to keep the ROMs in another folder, set `romPath` as its
`settings.xml` appendix describes.

**To check a build boots,** do not trust the window: the editor draws itself whether or
not the emulator started. Open `Documents\MEGAN XG\enginelog.txt` and look for
`machine RUNNING` after the latest `RUN START` line. The user manual's appendix on the
engine log explains the rest of that file.

## 8. The user manual (optional)

```powershell
cd docs\manual
python build_manual.py --inline-images
python tools\check_manual.py
```

This writes `MEGAN-XG-Manual.html`, a single self-contained file. **F1** in the plugin
opens `Documents\MEGAN XG\manual\MEGAN-XG-Manual.html` if it exists and the online copy
otherwise, so copy it there to use the local one. Without `--inline-images` the HTML
needs the `images\` folder beside it.

## Troubleshooting

| Symptom | Cause |
|---|---|
| `#error "...outdated version of the Projucer..."` | The project was saved by a Projucer from another JUCE release. Re-run step 5 with `libs\JUCE\Projucer.exe`. |
| `MSB1008: Only one project can be specified`, or a "successful" build that did nothing | MSBuild was run from Git Bash. Use PowerShell. |
| Emulator build fails in a custom build step | `python3` is not on `PATH` in that terminal — see *What you need*. |
| `LNK1104: cannot open file 'MEGAN XG.exe'` | The standalone is running. Close it. |
| `LNK2019` / `LNK1181` naming a MAME library | Step 4 has not finished, or the folders are not laid out as in step 1. |
| The DAW still shows the old behaviour | The VST3 in `Common Files\VST3` is the old copy. Check its timestamp; see step 6. |
| Crashes early, with a different error code each run, right after changing a header | Stale object files. Rebuild with `/t:Rebuild` instead of a normal build. |
| "ROM missing" over the Hardware view | `Documents\MEGAN XG\mu100.zip` is missing or incomplete (step 7). |

## Contributing

Enable the repository's commit guard once per clone:

```powershell
git config core.hooksPath .githooks
```

It refuses to commit ROMs, `CLAUDE.md` working notes and the third-party reference
material the editor's data pipeline reads locally (see `PROVENANCE.md`).

Patches to the emulator belong in the `mame-mu100` repository and keep MAME's own
BSD-3-Clause terms, matching the files they modify.
