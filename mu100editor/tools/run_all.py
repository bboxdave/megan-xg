#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) Dave Wheitner and MEGAN XG contributors
"""Run all MU100 data extractors and print a combined coverage summary."""
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
# extract_voices.py is deliberately NOT here: it WRITES data/voices.json, which
# build_voices.py (XGVoice.ini + drums.json + short_names.json) now owns, so running
# it would silently replace the full display names with the PDF's 8-character ones.
# extract_short_names.py supersedes it -- it reads the same voice grid, but keeps the
# abbreviations where they belong, as voices.json's `short` field.
for script in ("extract_parameters.py", "extract_effects.py", "extract_short_names.py",
               "extract_tables.py", "extract_drums.py"):
    print(f"\n=== {script} ===")
    r = subprocess.run([sys.executable, str(HERE / script)])
    if r.returncode:
        sys.exit(f"{script} failed")
