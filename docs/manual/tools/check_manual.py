#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) Dave Wheitner and MEGAN XG contributors
"""Mechanical consistency checks for the manual.  Run from docs/manual:

    python tools/check_manual.py          (after build_manual.py)

Every check here exists because it caught a real defect.  It deliberately does
NOT proof-read prose.  A scan for fragile phrasing ("the only ...", "twelve
...") was tried and produced 388 hits, nearly all ordinary English -- a check
that cries wolf is worse than no check, because it trains you to skim it.
What works is comparing the manual against something machine-readable: the
built HTML, or the source that defines the thing being described.

Exit code is the number of failed checks.
"""
import pathlib, re, sys

HERE = pathlib.Path(__file__).resolve().parent.parent
SRC  = HERE.parent.parent / "Source"
HTML = HERE / "MEGAN-XG-Manual.html"

def chapters():
    return [p for p in sorted(HERE.glob("*.md"))
            if p.name[0].isdigit() or re.match(r"A\d", p.name)]

def source(name):
    p = SRC / name
    return p.read_text(encoding="utf-8", errors="replace") if p.exists() else ""

results = []
def check(name, bad):
    bad = list(bad)
    results.append((name, bad))
    print(("PASS  " if not bad else "FAIL  ") + name + (f"  ({len(bad)})" if bad else ""))
    for b in bad[:20]:
        print("        " + str(b))
    if len(bad) > 20:
        print(f"        ... and {len(bad) - 20} more")

if not HTML.exists():
    sys.exit("MEGAN-XG-Manual.html not built -- run build_manual.py first")

html = HTML.read_text(encoding="utf-8")
md   = "\n".join(p.read_text(encoding="utf-8") for p in chapters())

# 1. Broken tables.  A table interrupted by prose and resumed without a fresh
#    header row silently renders as one run-on paragraph of pipe characters.
#    This is how the Mixer..Help half of ch. 15's shortcut table was found.
check("no run-on pipe paragraphs (broken tables)",
      (re.sub(r"\s+", " ", m.group(0))[:90]
       for m in re.finditer(r"<p>\s*\|[^<]{40,}", html)))

# 2. Internal links -- headings get renamed and nothing else notices.
ids   = set(re.findall(r'id="([^"]+)"', html))
links = re.findall(r'href="#([^"]+)"', html)
check("internal links resolve", sorted({l for l in links if l not in ids}))

# 3. Build-time tokens that nothing substituted.
check("no unsubstituted {{tokens}}", sorted(set(re.findall(r"\{\{[A-Z_]+\}\}", html))))

# 4. Menu item strings.  The manual quotes them EXACTLY by convention, and this
#    check has already caught "..." written as a single ellipsis character
#    across twelve chapters.  Verbatim matching is the point.
ed = source("PluginEditor.cpp")
#    Scoped to PopupMenu receivers: a ComboBox's items are dropdown VALUES,
#    which the manual paraphrases on purpose ("16384 (about 370 ms)"), so
#    including them reports 13 hits that are all correct as written.
items = {m.group(2) for m in re.finditer(
    r'(\w+)\.addItem\s*\(\s*(?:item\s*\(\s*)?(?:[\w:]+\s*,\s*)?"([^"]{4,})"', ed)
    if not m.group(1).endswith("Combo")}
items = {t for t in items if not t.startswith("-") and "%" not in t}
check(f"menu item strings appear verbatim ({len(items)} found)",
      sorted(t for t in items if t not in md))

# 5. settings.xml keys.  Scoped to the constructor block that parses
#    settings.xml; reading every getXAttribute in the file instead sweeps up
#    project-state and .mss attributes and reports 25 false hits.
lines = source("PluginProcessor.cpp").splitlines()
try:
    start = next(i for i, l in enumerate(lines) if "settingsFile.existsAsFile()" in l)
    end   = next(i for i, l in enumerate(lines[start:], start) if 'getenv("MU100_DIAG' in l)
    block = "\n".join(lines[start:end])
except StopIteration:
    block = ""
keys = set(re.findall(r'get(?:Int|String|Double|Bool)Attribute\s*\(\s*"([^"]+)"', block))
a2 = (HERE / "A2-settings-xml-reference.md").read_text(encoding="utf-8")
# Read so an existing file keeps working, never written -- so A2 omits them.
retired = {"showTooltips", "partOutputs", "partOutputsRemoveFromMain",
           "disablePluginOnlyParams", "midiOverridesLocked"}
check(f"settings.xml keys the code reads are in A2 ({len(keys)} keys)",
      sorted(k for k in keys - retired if k not in a2))

fails = sum(1 for _, bad in results if bad)
print(f"\n{len(results) - fails}/{len(results)} checks passed")
sys.exit(fails)
