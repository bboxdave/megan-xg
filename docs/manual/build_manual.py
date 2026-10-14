#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) Dave Wheitner and MEGAN XG contributors
"""Build the MEGAN XG plugin user manual into one self-contained HTML file.

Usage (run from docs/manual/):
    python build_manual.py                 -> MEGAN-XG-Manual.html
    python build_manual.py --out other.html

Chapters are every *.md file in this directory whose name starts with a digit or
a capital 'A' (appendices), concatenated in filename order.  README.md is the
editing instructions for this manual and is deliberately NOT part of the output.

Stdlib only, deliberately: this has to keep working years from now with whatever
Python is installed.  The Markdown subset understood here is the subset the
manual actually uses -- headings, paragraphs, pipe tables, fenced code, block
quotes, bullet and numbered lists, horizontal rules, images, links, and inline
bold / italic / code.  If you need something outside that, add it here rather
than hand-writing HTML into a chapter.
"""

from __future__ import annotations

import argparse
import base64
import datetime
import html
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent

# Screenshots referenced but not yet taken; reported at the end of a build.
MISSING: list[str] = []

# --inline-images: embed every screenshot as a data: URI, so the output is ONE
# file that can be copied anywhere. The default build links images/ by relative
# path -- right for the web copy, which is served beside the folder, and wrong
# for a file a user drops into Documents\MEGAN XG\manual\ on its own.
INLINE_IMAGES = False
_MIME = {".png": "image/png", ".jpg": "image/jpeg", ".jpeg": "image/jpeg",
         ".gif": "image/gif", ".svg": "image/svg+xml", ".webp": "image/webp"}

# --------------------------------------------------------------------------
# Inline markup
# --------------------------------------------------------------------------

_CODE_SPAN = re.compile(r"`([^`]+)`")
_IMAGE = re.compile(r"!\[([^\]]*)\]\(([^)\s]+)(?:\s+\"([^\"]*)\")?\)")
_LINK = re.compile(r"\[([^\]]+)\]\(([^)\s]+)\)")
_BOLD = re.compile(r"\*\*(.+?)\*\*", re.S)
_ITALIC = re.compile(r"(?<![\*\w])\*([^*\n]+)\*(?!\*)")


def inline(text: str) -> str:
    """Convert inline Markdown to HTML.  Code spans are protected first."""
    spans: list[str] = []

    def stash(m: re.Match) -> str:
        spans.append(html.escape(m.group(1)))
        return f"\x00{len(spans) - 1}\x00"

    text = _CODE_SPAN.sub(stash, text)
    text = html.escape(text, quote=False)

    def image(m: re.Match) -> str:
        alt, src, title = m.group(1), m.group(2), m.group(3)
        # A screenshot that has not been taken yet renders as a labelled
        # placeholder rather than a broken-image icon, so the manual stays
        # presentable while the capture pass is outstanding. Drop the file in
        # and it becomes a real image with no edit to the chapter.
        if not (HERE / src).exists():
            MISSING.append(src)
            return (
                f'<span class="shot-todo"><strong>Screenshot to come</strong>'
                f'<span>{alt or src}</span></span>'
            )
        t = f' title="{html.escape(title, quote=True)}"' if title else ""
        if INLINE_IMAGES:
            path = HERE / src
            mime = _MIME.get(path.suffix.lower(), "application/octet-stream")
            data = base64.b64encode(path.read_bytes()).decode("ascii")
            return f'<img src="data:{mime};base64,{data}" alt="{alt}"{t}>'
        return f'<img src="{html.escape(src, quote=True)}" alt="{alt}"{t}>'

    text = _IMAGE.sub(image, text)
    text = _LINK.sub(
        lambda m: f'<a href="{html.escape(m.group(2), quote=True)}">{m.group(1)}</a>',
        text,
    )
    text = _BOLD.sub(lambda m: f"<strong>{m.group(1)}</strong>", text)
    text = _ITALIC.sub(lambda m: f"<em>{m.group(1)}</em>", text)

    return re.sub(r"\x00(\d+)\x00", lambda m: f"<code>{spans[int(m.group(1))]}</code>", text)


# --------------------------------------------------------------------------
# Anchors
# --------------------------------------------------------------------------

_slug_seen: dict[str, int] = {}


def slug(text: str) -> str:
    s = re.sub(r"<[^>]+>", "", text)
    s = re.sub(r"[^\w\s-]", "", s.lower()).strip()
    s = re.sub(r"[\s_]+", "-", s)
    s = s or "section"
    n = _slug_seen.get(s, 0)
    _slug_seen[s] = n + 1
    return s if n == 0 else f"{s}-{n}"


# --------------------------------------------------------------------------
# Block markup
# --------------------------------------------------------------------------

_ROW_SPLIT = re.compile(r"(?<!\\)\|")


def split_row(line: str) -> list[str]:
    line = line.strip()
    if line.startswith("|"):
        line = line[1:]
    if line.endswith("|"):
        line = line[:-1]
    return [c.strip().replace(r"\|", "|") for c in _ROW_SPLIT.split(line)]


def is_divider(line: str) -> bool:
    cells = split_row(line)
    return bool(cells) and all(re.fullmatch(r":?-{2,}:?", c) for c in cells)


def alignments(line: str) -> list[str]:
    out = []
    for c in split_row(line):
        left, right = c.startswith(":"), c.endswith(":")
        out.append("center" if left and right else "right" if right else "left")
    return out


class Doc:
    """Accumulates HTML and the heading list the table of contents is built from."""

    def __init__(self) -> None:
        self.parts: list[str] = []
        self.toc: list[tuple[int, str, str]] = []  # (level, text, anchor)

    def heading(self, level: int, text: str) -> None:
        rendered = inline(text)
        anchor = slug(text)
        if level <= 3:
            self.toc.append((level, rendered, anchor))
        self.parts.append(f'<h{level} id="{anchor}">{rendered}</h{level}>')


def render(lines: list[str], doc: Doc) -> None:
    i, n = 0, len(lines)
    while i < n:
        line = lines[i]
        stripped = line.strip()

        if not stripped:
            i += 1
            continue

        # Fenced code
        if stripped.startswith("```"):
            i += 1
            body: list[str] = []
            while i < n and not lines[i].strip().startswith("```"):
                body.append(lines[i])
                i += 1
            i += 1
            doc.parts.append(
                "<pre><code>" + html.escape("\n".join(body)) + "</code></pre>"
            )
            continue

        # HTML comment (screenshot capture notes) -- passed through so they
        # survive in the page source without rendering.
        if stripped.startswith("<!--"):
            body = [line]
            while i < n and "-->" not in lines[i]:
                i += 1
                if i < n:
                    body.append(lines[i])
            i += 1
            doc.parts.append("\n".join(body))
            continue

        # Horizontal rule
        if re.fullmatch(r"(\*\s*){3,}|(-\s*){3,}|(_\s*){3,}", stripped):
            doc.parts.append("<hr>")
            i += 1
            continue

        # Heading
        m = re.match(r"(#{1,6})\s+(.*)", stripped)
        if m:
            doc.heading(len(m.group(1)), m.group(2).strip())
            i += 1
            continue

        # Table
        if "|" in stripped and i + 1 < n and is_divider(lines[i + 1]):
            header = split_row(stripped)
            align = alignments(lines[i + 1])
            i += 2
            rows: list[list[str]] = []
            while i < n and "|" in lines[i] and lines[i].strip():
                rows.append(split_row(lines[i]))
                i += 1
            out = ["<div class='table-wrap'><table><thead><tr>"]
            for j, c in enumerate(header):
                a = align[j] if j < len(align) else "left"
                out.append(f"<th style='text-align:{a}'>{inline(c)}</th>")
            out.append("</tr></thead><tbody>")
            for row in rows:
                out.append("<tr>")
                for j in range(len(header)):
                    c = row[j] if j < len(row) else ""
                    a = align[j] if j < len(align) else "left"
                    out.append(f"<td style='text-align:{a}'>{inline(c)}</td>")
                out.append("</tr>")
            out.append("</tbody></table></div>")
            doc.parts.append("".join(out))
            continue

        # Block quote (callouts)
        if stripped.startswith(">"):
            body = []
            while i < n and lines[i].strip().startswith(">"):
                body.append(re.sub(r"^\s*>\s?", "", lines[i]))
                i += 1
            inner = Doc()
            render(body, inner)
            cls = "callout"
            joined = " ".join(body)[:120]
            for key, name in (
                ("Standalone only", "sa"),
                ("VST only", "vst"),
                ("Watch out", "warn"),
            ):
                if key in joined:
                    cls = f"callout {name}"
                    break
            doc.parts.append(f'<blockquote class="{cls}">' + "".join(inner.parts) + "</blockquote>")
            continue

        # Lists
        m = re.match(r"[-*+]\s+(.*)", stripped)
        m_ord = re.match(r"\d+[.)]\s+(.*)", stripped)
        if m or m_ord:
            ordered = m_ord is not None
            items: list[list[str]] = []
            while i < n:
                s = lines[i].strip()
                mm = re.match(r"\d+[.)]\s+(.*)" if ordered else r"[-*+]\s+(.*)", s)
                if mm:
                    items.append([mm.group(1)])
                    i += 1
                elif s and (lines[i].startswith("  ") or lines[i].startswith("\t")) and items:
                    items[-1].append(s)
                    i += 1
                elif not s and i + 1 < n and re.match(
                    r"\s+(?:[-*+]|\d+[.)])\s", lines[i + 1]
                ):
                    i += 1
                else:
                    break
            tag = "ol" if ordered else "ul"
            doc.parts.append(
                f"<{tag}>" + "".join(f"<li>{inline(' '.join(it))}</li>" for it in items) + f"</{tag}>"
            )
            continue

        # Paragraph
        body = []
        while i < n and lines[i].strip() and not re.match(
            r"\s*(#{1,6}\s|```|>|[-*+]\s|\d+[.)]\s|<!--)", lines[i]
        ):
            if "|" in lines[i] and i + 1 < len(lines) and is_divider(lines[i + 1]):
                break
            body.append(lines[i].strip())
            i += 1
        if body:
            doc.parts.append("<p>" + inline(" ".join(body)) + "</p>")
        else:
            i += 1


# --------------------------------------------------------------------------
# Page assembly
# --------------------------------------------------------------------------

CSS = """
:root{
  --bg:#ffffff; --fg:#1e2124; --muted:#5c6570; --rule:#d7dce1;
  --panel:#f5f7f9; --code-bg:#f0f2f5; --link:#0b62b8;
  --sa:#0d7a5f; --vst:#7a4bbd; --warn:#b4520a;
}
@media (prefers-color-scheme: dark){
  :root:not([data-theme="light"]){
    --bg:#16181c; --fg:#e4e7ea; --muted:#9aa3ad; --rule:#333940;
    --panel:#1e2126; --code-bg:#22262c; --link:#6fb2f0;
    --sa:#4fd0a8; --vst:#c0a0f0; --warn:#f0a352;
  }
}
:root[data-theme="dark"]{
  --bg:#16181c; --fg:#e4e7ea; --muted:#9aa3ad; --rule:#333940;
  --panel:#1e2126; --code-bg:#22262c; --link:#6fb2f0;
  --sa:#4fd0a8; --vst:#c0a0f0; --warn:#f0a352;
}
*{box-sizing:border-box}
body{
  margin:0; background:var(--bg); color:var(--fg);
  font:16px/1.62 -apple-system,"Segoe UI",Roboto,Helvetica,Arial,sans-serif;
}
.wrap{max-width:52rem;margin:0 auto;padding:2.5rem 1.25rem 6rem}
h1,h2,h3,h4,h5,h6{line-height:1.25;font-weight:650;margin:2.2em 0 .6em}
h1{font-size:2rem;margin-top:0;padding-top:1.6em;border-top:1px solid var(--rule)}
h1:first-of-type{border-top:none;padding-top:0}
h2{font-size:1.4rem}
h3{font-size:1.13rem}
h4{font-size:1rem;color:var(--muted);text-transform:uppercase;letter-spacing:.04em}
p{margin:0 0 1em}
a{color:var(--link)}
hr{border:none;border-top:1px solid var(--rule);margin:2.4em 0}
code{background:var(--code-bg);padding:.12em .34em;border-radius:3px;
  font:.88em/1.4 ui-monospace,Consolas,"Courier New",monospace}
pre{background:var(--panel);border:1px solid var(--rule);border-radius:6px;
  padding:.9rem 1rem;overflow-x:auto;margin:0 0 1.2em}
pre code{background:none;padding:0;font-size:.85rem;white-space:pre}
ul,ol{margin:0 0 1.1em;padding-left:1.5rem}
li{margin:.28em 0}
img{max-width:100%;height:auto;border:1px solid var(--rule);border-radius:5px;display:block;
  margin:1.2em 0}
.shot-todo{display:flex;flex-direction:column;align-items:center;justify-content:center;
  gap:.3em;margin:1.2em 0;padding:2.2em 1rem;border:1px dashed var(--rule);border-radius:5px;
  background:var(--panel);color:var(--muted);text-align:center;font-size:.9rem}
.shot-todo strong{color:var(--muted);font-weight:600;letter-spacing:.04em;
  text-transform:uppercase;font-size:.78rem}
.table-wrap{overflow-x:auto;margin:0 0 1.3em}
table{border-collapse:collapse;width:100%;font-size:.94rem}
th,td{border:1px solid var(--rule);padding:.42rem .6rem;vertical-align:top}
th{background:var(--panel);font-weight:600}
blockquote.callout{
  margin:1.3em 0;padding:.75rem 1rem;background:var(--panel);
  border-left:4px solid var(--muted);border-radius:0 5px 5px 0;
}
blockquote.callout p:last-child{margin-bottom:0}
blockquote.sa{border-left-color:var(--sa)}
blockquote.vst{border-left-color:var(--vst)}
blockquote.warn{border-left-color:var(--warn)}
nav.toc{background:var(--panel);border:1px solid var(--rule);border-radius:8px;
  padding:1.1rem 1.4rem;margin:0 0 2.5em}
nav.toc h2{margin:0 0 .6em;font-size:1.1rem}
nav.toc ul{list-style:none;padding-left:0;margin:0}
nav.toc ul ul{padding-left:1.1rem}
nav.toc li{margin:.16em 0}
nav.toc .lvl1>a{font-weight:600}
nav.toc .lvl3{font-size:.92rem;color:var(--muted)}
.subtitle{color:var(--muted);font-size:1.05rem;margin-top:-.4em}
@media print{
  body{font-size:11pt}
  nav.toc{break-after:page}
  h1{break-before:page}
  h1:first-of-type{break-before:avoid}
}
"""


def build_toc(entries: list[tuple[int, str, str]]) -> str:
    # Nested lists live INSIDE the parent <li>, so the markup validates: an <ul>
    # as a direct child of another <ul> renders the same but is not legal HTML.
    out = ['<nav class="toc"><h2>Contents</h2><ul>']
    depth = 1
    open_item = False
    for level, text, anchor in entries:
        while depth < level:
            out.append("<ul>")  # opened inside the still-open <li>
            depth += 1
            open_item = False
        while depth > level:
            if open_item:
                out.append("</li>")
            out.append("</ul></li>")   # the </li> closes the PARENT item
            depth -= 1
            open_item = False
        if open_item:
            out.append("</li>")
        out.append(f'<li class="lvl{level}"><a href="#{anchor}">{text}</a>')
        open_item = True
    while depth > 1:
        if open_item:
            out.append("</li>")
        out.append("</ul></li>")
        depth -= 1
        open_item = False
    if open_item:
        out.append("</li>")
    out.append("</ul></nav>")
    return "".join(out)


def last_updated(files: list[Path]) -> str:
    """The date the manual's text last changed, for the {{LAST_UPDATED}} token.

    Taken from the newest CHAPTER's modification time rather than from the build
    clock, so rebuilding without editing anything does not restamp the manual,
    and editing any chapter does.

    Per chapter: the COMMIT date when the file is committed and unmodified, its
    mtime when it has uncommitted edits (or git is unavailable).  Both halves are
    needed.  mtime alone is wrong in any fresh clone -- above all in CI, which
    builds the published manual from one, so every deploy would be stamped with
    the deploy date.  The commit date alone is wrong while a chapter is being
    edited, which is exactly when someone is writing the manual.  (CI must clone
    with full history, or `git log -- <file>` finds nothing and this falls back
    to mtime: see .github/workflows/manual.yml, fetch-depth: 0.)
    """
    dirty = _git("status", "--porcelain", "--", *[p.name for p in files])
    dirty_names = set()
    if dirty is not None:
        for line in dirty.splitlines():
            dirty_names.add(Path(line[3:].strip().strip('"')).name)

    newest = datetime.date.min
    for p in files:
        committed = None
        if dirty is not None and p.name not in dirty_names:
            out = _git("log", "-1", "--format=%cs", "--", p.name)
            if out:
                committed = datetime.date.fromisoformat(out.strip())
        d = committed or datetime.date.fromtimestamp(p.stat().st_mtime)
        newest = max(newest, d)
    return f"{newest:%B} {newest.day}, {newest.year}"


def _git(*args: str) -> str | None:
    """Run git in the manual's folder; None if git is missing or this is no repo."""
    try:
        r = subprocess.run(["git", *args], cwd=HERE, capture_output=True,
                           text=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return None
    return r.stdout if r.returncode == 0 else None


def chapter_files() -> list[Path]:
    files = [
        p
        for p in sorted(HERE.glob("*.md"))
        if p.name[0].isdigit() or re.match(r"A\d", p.name)
    ]
    return files


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default="MEGAN-XG-Manual.html")
    ap.add_argument("--inline-images", action="store_true",
                    help="embed screenshots as data: URIs -> one standalone file")
    args = ap.parse_args()
    global INLINE_IMAGES
    INLINE_IMAGES = args.inline_images

    files = chapter_files()
    if not files:
        print("no chapter files found in", HERE, file=sys.stderr)
        return 1

    doc = Doc()
    stamp = last_updated(files)
    for path in files:
        text = path.read_text(encoding="utf-8")
        text = text.replace("{{LAST_UPDATED}}", stamp)
        render(text.replace("\r\n", "\n").split("\n"), doc)

    title = "MEGAN XG Plugin - User Manual"
    page = (
        "<!doctype html>\n<html lang='en'>\n<head>\n"
        "<meta charset='utf-8'>\n"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>\n"
        f"<title>{html.escape(title)}</title>\n"
        f"<style>{CSS}</style>\n</head>\n<body>\n<div class='wrap'>\n"
        + build_toc(doc.toc)
        + "\n".join(doc.parts)
        + "\n</div>\n</body>\n</html>\n"
    )

    out = HERE / args.out
    out.write_text(page, encoding="utf-8")
    print(f"{len(files)} chapters -> {out}  ({len(page):,} bytes)")
    if MISSING:
        print(f"    {len(MISSING)} screenshot(s) still to capture "
              f"-- see CAPTURE-LIST.md")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
