# SPDX-License-Identifier: GPL-3.0-or-later
"""Maya Workflow Kit: make Blender feel like Maya (keymap, interaction, UI, concepts).

Shipped as one Blender 4.2+ extension (docs/user_decisions.md #3); Blender reads ``blender_manifest.toml``. The
legacy add-on zip gets its ``bl_info`` from ``tools/build.py`` (step 114: the extension build carries none, as the
extension guidelines recommend).

Each feature area is a sub-package with its own ``register()`` /
``unregister()``. Order matters: preferences first (others may read them),
keymap last (keymap items reference operators registered before it). ``CORE``
stays registered; ``MODE`` is Maya mode, switched on and off at run time by
``switch`` (step 60).

Opt-in (step 115): Maya mode is off after install, so enabling registers only CORE. Nothing persistent (step 130,
docs/user_decisions.md #13): no module writes Blender's preferences or files outside the extension's folders, and
``unregister()`` takes back everything ``register()`` added — there is nothing to restore on disable or Remove.
"""

from . import preferences, switch, welcome, operators, ui, command_line, marking, first_start, keymap

# Always registered: the preferences, the Maya / Blender switch (docs/steps/step_60.md) and the opt-in (step 115).
CORE = (preferences, switch, welcome)
# "Maya mode": registered while the add-on preference ``maya_mode`` is on; switching to
# Blender unregisters them, which takes back everything they registered.
MODE = (operators, ui, command_line, marking, first_start, keymap)


def register():
    for mod in CORE:
        mod.register()
    if switch.is_on():
        switch.register_mode()
        switch._registered[0] = True


def unregister():
    if switch._registered[0]:
        switch.unregister_mode()
        switch._registered[0] = False
    for mod in reversed(CORE):
        mod.unregister()
