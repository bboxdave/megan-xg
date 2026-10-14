#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) Dave Wheitner and MEGAN XG contributors
"""Assemble THIRD-PARTY-NOTICES.txt for a MEGAN XG binary release.

Usage:
    python gen_notices.py --mame <libs/mame-mu100> --juce <libs/JUCE> --out <file>

A source checkout redistributes none of this; a binary zip redistributes all of
it, because the plugin links MAME and its bundled libraries statically. Every
licence text is pulled from the SAME trees the binary was built from, so the
notices cannot drift from the code.

The list deliberately OVER-includes: one entry per library on the link line
(`externalLibraries` in MEGAN XG.jucer) and per JUCE dependency that the
modules this project uses compile on Windows. The linker discards unreferenced
objects, so some entries may contribute no code at all -- proving that per
library is fragile, and a surplus notice costs nothing where a missing one is a
licence breach.

Each extract names a start and an end marker. A marker that is no longer
found is a hard failure: a library update that moves its licence text must
stop the release, not ship an empty notice.
"""

import argparse
import re
import sys
from pathlib import Path

# (title, licence, used by, tree, path, start marker, end marker)
# tree: "mame" | "juce" | "here".  Markers are regexes matched per line; None
# means the whole file.  The end line is included.
MAME_ENTRIES = [
    ("expat", "MIT", "XML parsing", "mame", "3rdparty/expat/COPYING", None, None),
    ("zlib", "zlib", "compression", "mame", "3rdparty/zlib/LICENSE", None, None),
    ("PortMidi", "MIT-style", "MIDI I/O layer", "mame", "3rdparty/portmidi/license.txt", None, None),
    ("libFLAC", "BSD-3-Clause (Xiph.Org)", "audio codec", "mame", "3rdparty/flac/COPYING.Xiph", None, None),
    ("LZMA SDK", "Public domain", "7z/LZMA decompression", "mame", "3rdparty/lzma/DOC/lzma-sdk.txt",
     r"^LICENSE\s*$", r"include it to GNU GPL"),
    ("Berkeley SoftFloat 3", "BSD-3-Clause", "floating-point emulation", "mame",
     "3rdparty/softfloat3/COPYING.txt", None, None),
    ("utf8proc", "MIT", "Unicode handling", "mame", "3rdparty/utf8proc/LICENSE.md", None, None),
    ("libjpeg (Independent JPEG Group)", "IJG", "JPEG images", "mame", "3rdparty/libjpeg/README",
     r"^LEGAL ISSUES\s*$", r"^REFERENCES\s*$"),
    ("SQLite", "Public domain", "database", "mame", "3rdparty/sqlite3/sqlite3.c",
     r"The author disclaims copyright", r"May you share freely"),
    ("Zstandard", "BSD-3-Clause", "compression", "mame", "3rdparty/zstd/LICENSE", None, None),
    ("PortAudio", "MIT", "audio I/O layer", "mame", "3rdparty/portaudio/LICENSE.txt", None, None),
    ("Lua", "MIT", "scripting engine", "mame", "3rdparty/lua/src/lua.h",
     r"^\* Copyright \(C\) 1994-", r"SOFTWARE OR THE USE OR OTHER DEALINGS"),
    ("lsqlite3", "MIT", "Lua SQLite binding", "mame", "3rdparty/lsqlite3/lsqlite3.c",
     r"Copyright \(C\) 2002-", r"SOFTWARE OR THE USE OR OTHER DEALINGS"),
    ("LuaFileSystem", "MIT", "Lua library", "mame", "3rdparty/luafilesystem/LICENSE", None, None),
    ("lua-linenoise", "MIT", "Lua library", "mame", "3rdparty/lua-linenoise/linenoise.c",
     r"Copyright \(c\) 2011-2015 Rob Hoelz", r"OTHER DEALINGS IN THE SOFTWARE"),
    ("lua-zlib", "MIT", "Lua library", "mame", "3rdparty/lua-zlib/README",
     r"The MIT License", r"^\W*THE SOFTWARE\.\s*$"),
    ("AsmJit", "Zlib", "code generation", "mame", "3rdparty/asmjit/LICENSE.md", None, None),
    ("bgfx", "BSD-2-Clause", "rendering", "mame", "3rdparty/bgfx/LICENSE", None, None),
    ("bimg", "BSD-2-Clause", "image library", "mame", "3rdparty/bimg/LICENSE", None, None),
    ("bx", "BSD-2-Clause", "base library", "mame", "3rdparty/bx/LICENSE", None, None),
    ("ymfm", "BSD-3-Clause", "FM sound cores", "mame", "3rdparty/ymfm/LICENSE", None, None),
    ("WDL FFT", "Zlib", "FFT", "mame", "3rdparty/wdlfft/fft.c",
     r"Copyright \(C\) 2006 and later Cockos", r"This notice may not be removed"),
    ("linenoise", "BSD-2-Clause", "line editing", "mame", "3rdparty/linenoise/linenoise.c",
     r"Copyright \(c\) 2010, Salvatore", r"POSSIBILITY OF SUCH DAMAGE"),
]

JUCE_ENTRIES = [
    ("Steinberg VST3 SDK", "MIT", "VST3 plug-in format", "juce",
     "modules/juce_audio_processors_headless/format_types/VST3_SDK/LICENSE.txt", None, None),
    ("Steinberg ASIO SDK", "GPLv3 (chosen branch of a dual licence; full text at the end)",
     "standalone audio devices", "juce", "modules/juce_audio_devices/native/asio/LICENSE.txt", None, None),
    ("jpeglib, JUCE's copy (Independent JPEG Group)", "IJG", "JPEG images", "juce",
     "modules/juce_graphics/image_formats/jpglib/README", r"^LEGAL ISSUES\s*$", r"^REFERENCES\s*$"),
    ("libpng", "libpng (zlib-style)", "PNG images", "juce",
     "modules/juce_graphics/image_formats/pnglib/LICENSE", None, None),
    ("zlib, JUCE's copy", "zlib", "compression", "juce", "modules/juce_core/zip/zlib/README",
     r"^Copyright notice:", r"madler@alumni"),
    ("HarfBuzz", "Old MIT", "text shaping", "juce", "modules/juce_graphics/fonts/harfbuzz/COPYING", None, None),
    ("SheenBidi", "Apache-2.0", "bidirectional text", "juce",
     "modules/juce_graphics/unicode/sheenbidi/LICENSE", None, None),
    ("Ogg Vorbis", "BSD-3-Clause (Xiph.Org)", "audio codec", "juce",
     "modules/juce_audio_formats/codecs/oggvorbis/Ogg Vorbis Licence.txt", None, None),
]

APPENDIX = [
    ("MAME: GNU Lesser General Public License v2.1",
     "Four LGPL-2.1-or-later files (disk/cartridge format code) are linked from MAME's formats "
     "library. Their source is in the megan-xg-mame repository named above.",
     "mame", "docs/legal/LGPL-2.1"),
    ("GNU General Public License v3 (for the Steinberg ASIO SDK)",
     "The ASIO SDK is used under the GPLv3 branch of its dual licence.",
     "here", "licenses/GPL-3.0.txt"),
]

MAME_HOLDER = "Copyright (c) 1997-2026 MAMEdev and contributors (each source file names its own copyright holders)"

# Comment decoration around licence text quoted from source files.
_LEAD = re.compile(r"^\s*(?:/\*+|\*+/|\*+|//+|#)\s?")
_TRAIL = re.compile(r"\s*\*+/?\s*$")
RULE = "=" * 78


def read(root: Path, rel: str) -> list[str]:
    path = root / rel
    if not path.is_file():
        sys.exit(f"missing licence source: {path}")
    return path.read_text(encoding="utf-8", errors="replace").splitlines()


def extract(lines: list[str], start: str | None, end: str | None, where: str) -> list[str]:
    if start is None:
        return lines
    s = next((i for i, l in enumerate(lines) if re.search(start, l)), None)
    if s is None:
        sys.exit(f"start marker /{start}/ not found in {where}")
    e = next((i for i in range(s + 1, len(lines)) if re.search(end, lines[i])), None)
    if e is None:
        sys.exit(f"end marker /{end}/ not found after the start in {where}")
    return lines[s:e + 1]


def undecorate(lines: list[str]) -> list[str]:
    out = []
    for l in lines:
        if re.fullmatch(r"\s*[*/=#-]{3,}\s*", l):
            continue  # a pure box-drawing line
        out.append(_TRAIL.sub("", _LEAD.sub("", l)).rstrip())
    while out and not out[0].strip():
        out.pop(0)
    while out and not out[-1].strip():
        out.pop()
    return out


def block(title: str, licence: str, used_by: str, text: list[str]) -> list[str]:
    return [RULE, title, f"Licence: {licence}    Used for: {used_by}", RULE, "", *text, "", ""]


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--mame", required=True, type=Path)
    ap.add_argument("--juce", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    a = ap.parse_args()
    roots = {"mame": a.mame, "juce": a.juce, "here": Path(__file__).resolve().parent}

    out = [
        "MEGAN XG -- THIRD-PARTY NOTICES",
        "",
        "MEGAN XG is free software under the GNU Affero General Public License v3",
        "or later (LICENSE.txt). Its source, and the source of the patched MAME it",
        "is built from, are published at:",
        "    https://github.com/bboxdave/megan-xg",
        "    https://github.com/bboxdave/megan-xg-mame",
        "",
        "The program is built with the JUCE framework, used under the AGPLv3, and",
        "statically links the components below. Each is listed with its own",
        "licence, reproduced as its terms require. The list is inclusive: it",
        "names every library on the build's link line, even where the linker may",
        "keep none of its code.",
        "",
        "This software is based in part on the work of the Independent JPEG Group.",
        "",
        "Contents",
        "--------",
    ]
    entries = [("MAME", "BSD-3-Clause (per-file; see below)", "hardware emulation", None, None, None, None),
               *MAME_ENTRIES, *JUCE_ENTRIES]
    out += [f"  {t:<46} {lic.split(' (')[0]}" for t, lic, *_ in entries]
    out += [f"  {t}" for t, *_ in APPENDIX]
    out += ["", ""]

    mame_bsd = read(a.mame, "docs/legal/BSD-3-Clause")
    if not mame_bsd[0].startswith("Copyright <YEAR>"):
        sys.exit("MAME's BSD-3-Clause template changed; check the holder line")
    out += block("MAME", "BSD-3-Clause, per file; nothing GPL-2.0 is linked",
                 "hardware emulation", [MAME_HOLDER, *mame_bsd[1:]])

    for title, lic, used, tree, rel, start, end in [*MAME_ENTRIES, *JUCE_ENTRIES]:
        text = extract(read(roots[tree], rel), start, end, f"{tree}:{rel}")
        # Only text quoted out of a source file carries comment decoration; a
        # licence FILE is reproduced as is (its own "*" bullets are content).
        text = undecorate(text) if start is not None and rel.endswith((".c", ".h")) else text
        if not any(re.search(r"(?i)copyright|public domain|licen[cs]e|permission", l) for l in text):
            sys.exit(f"{title}: extract has no licence wording -- markers wrong?")
        out += block(title, lic, used, text)

    for title, note, tree, rel in APPENDIX:
        out += [RULE, title, RULE, "", note, "", *read(roots[tree], rel), "", ""]

    a.out.write_text("\n".join(out).rstrip() + "\n", encoding="utf-8", newline="\r\n")
    print(f"{a.out}: {len(entries)} components, {len(out)} lines")


if __name__ == "__main__":
    main()
