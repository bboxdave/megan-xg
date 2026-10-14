# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) Dave Wheitner and MEGAN XG contributors
"""The per-file licence notice every codegen tool emits at the top of its header.

One copy, imported by gen_address_map / gen_drum_defaults / gen_drums /
gen_effect_conv / build_effects / gen_factory_default / gen_tooltips /
gen_voices. A generated file is regenerated wholesale, so a notice added to it
by hand is deleted by the next run; it has to come from the generator.

The trailing blank line separates it from the generator's own "AUTO-GENERATED"
comment, matching the hand-written sources.
"""

CPP_LINES = [
    "// SPDX-License-Identifier: AGPL-3.0-or-later",
    "// Copyright (c) Dave Wheitner and MEGAN XG contributors",
    "",
]
