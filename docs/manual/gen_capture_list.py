#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) Dave Wheitner and MEGAN XG contributors
"""Regenerate CAPTURE-LIST.md from the <!-- CAPTURE: --> notes in the chapters.

Run from docs/manual/ after adding, moving or removing a screenshot placeholder:
    python gen_capture_list.py

The list is derived, never hand-maintained, so it cannot drift from the chapters.
CAPTURE-LIST.md is not part of the built manual (the builder only picks up files
whose names start with a digit or 'A').
"""
import pathlib
import re

HERE = pathlib.Path(__file__).resolve().parent
HEAD = """# Screenshot capture checklist

Generated from the `<!-- CAPTURE: -->` notes in the chapters — regenerate with
`python gen_capture_list.py` after adding or moving a placeholder. Not part of
the built manual.

Drop each file into `images/` under the name given and rebuild; the placeholder
box in the HTML becomes the real image with no edit to the chapter.

**House style:** editor shots in the **dark** theme unless the note says
otherwise, window large enough that nothing is ellipsized, PNG, cropped to the
module plus a few pixels of margin. Full-window shots at a consistent size.
"""

PAT = re.compile(r'!\[([^\]]*)\]\((images/[^)]+)\)\s*\n<!--\s*CAPTURE:(.*?)-->', re.S)


def main() -> int:
    rows = []
    # README.md carries an EXAMPLE placeholder to document the convention, so it
    # is skipped along with the generated list itself.
    for p in sorted(HERE.glob("*.md")):
        if p.name in ("CAPTURE-LIST.md", "README.md"):
            continue
        for m in PAT.finditer(p.read_text(encoding="utf-8")):
            rows.append((p.name, m.group(2), m.group(1), " ".join(m.group(3).split())))

    # Count what is genuinely MISSING, not the total. This line used to read
    # "{len(rows)} shots outstanding" and so stayed at 27 after the last
    # capture landed -- a tracked file asserting work was owed when none was.
    missing = sum(1 for _, src, _, _ in rows if not (HERE / src).exists())
    summary = (f"All {len(rows)} shots captured; none outstanding."
               if missing == 0 else
               f"{missing} of {len(rows)} shots outstanding.")
    out = [HEAD, summary + "\n"]
    cur = None
    for f, src, alt, note in rows:
        if f != cur:
            out.append(f"\n## {f}\n")
            cur = f
        done = "x" if (HERE / src).exists() else " "
        out.append(f"- [{done}] **`{src}`** — {alt}  \n      {note}")

    (HERE / "CAPTURE-LIST.md").write_text("\n".join(out) + "\n", encoding="utf-8")
    print(f"{len(rows)} entries -> CAPTURE-LIST.md")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
