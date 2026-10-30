# SPDX-License-Identifier: GPL-3.0-or-later
"""Step 152 (#101) Maya option windows of the Display menu: Display > Grid ❒ = Maya's Grid Options.

Same spec as operators/option_windows.py (``F`` fields, ``Window`` = one ``allset.opt_<key>`` operator;
option_windows._extend appends ``WINDOWS``). Labels, kinds, factory defaults and buttons are Maya 2022's
with factory prefs (Grid Options;
grid size 12, spacing 5, divisions 5,
displayColor grid / gridAxis / gridHighlight = index 2 = 0.25 grey).

Maya keeps grid options as optionVars (gridSize / gridSpacing / gridDivisions, displayGrid*: per user), so the kit
keeps them where every option window keeps its values: the add-on preference ``option_settings`` (key
"grid_options"). ui/maya_grid.py draws from ``grid_settings()`` = the applied values over Maya's factory ones.
Colours: Maya picks them on its 31-entry colour index slider; Blender has a colour picker (delegated, step 152), with
Maya's index 2 grey (#404040) as the default. Grid numbers are not drawn by the kit (Maya's factory: Hide).
"""

import json

import bpy
from bpy.props import FloatVectorProperty

from .option_windows import F, Window

KEY = "grid_options"
MAYA_GREY = (0.25, 0.25, 0.25)       # Maya colour index 2 (colorIndex -q 2), every grid colour's factory value


class FColor(F):
    """A colour (Maya: a colour index slider): display-space RGB, drawn as is like the grid lines."""

    def __init__(self, name, label, default=MAYA_GREY):
        super().__init__(name, label, "vec3color", default)

    def prop(self):
        return FloatVectorProperty(name=self.label.rstrip(":"), default=self.default, size=3, subtype='COLOR_GAMMA',
                                   min=0.0, max=1.0)


FIELDS = [
    # Size
    F("size", "Length and width:", "float", 12.0, lo=0.01, hi=1000.0),
    F("spacing", "Grid lines every:", "float", 5.0, lo=0.01, hi=1000.0),
    F("divisions", "Subdivisions:", "int", 5, lo=1, hi=20),
    # Color
    FColor("axis_color", "Axes:"),
    FColor("line_color", "Grid lines & numbers:"),
    FColor("division_color", "Subdivision lines:"),
    # Display
    F("show_axes", "Display:", "bool", True, text="Axes"),
    F("thick_axes", "", "bool", True, text="Thicker line for axes"),
    F("show_lines", "", "bool", True, text="Grid lines"),
    F("show_divisions", "", "bool", True, text="Subdivision lines"),
]


def _run_grid(context, v):
    """Apply = the grid redraws with the window's values (execute / Apply stored them first). Maya's Apply doesn't
    turn a hidden grid on (delegated: the values only change how the grid looks, Display > Grid shows / hides it)."""
    _CACHE.clear()
    wm = context.window_manager
    for win in wm.windows if wm else ():
        for area in win.screen.areas:
            if area.type == 'VIEW_3D':
                area.tag_redraw()
    return {'FINISHED'}


# --- Display > Toggle Show/Hide ❒ (step 162) ----------------------------------------------------------------------
# Maya 2022 "Toggle Visibility Options": First Selection: Independent (factory, optionVar
# toggleVisibilityAndKeepSelectionBehaviour 1) / Dependent (2); buttons Apply and Close / Apply / Close. The plain
# click and h run with the saved value (operators/visibility.toggle_dependent).
TOGGLE_KEY = "toggle_show_hide"


def _run_toggle(context, v):
    return bpy.ops.allset.visibility('EXEC_DEFAULT', action='TOGGLE',
                                     behaviour='DEPENDENT' if v["first_selection"] == "Dependent" else 'INDEPENDENT')


WINDOWS = [
    Window(TOGGLE_KEY, "Toggle Visibility Options", "Apply and Close", None, [
        F("first_selection", "First Selection:", "enum", "Independent", items=("Independent", "Dependent"))],
        _run_toggle, paths=("Display/Toggle Show/Hide",)),
    Window(KEY, "Grid Options", "Apply and Close", None, FIELDS, _run_grid, paths=("Display/Grid",),
           missing=("Perspective grid numbers: Hide / On Axes / Along Edge (factory Hide)",
                    "Orthographic grid numbers: Hide / On Axes / Along Edge (factory Hide)"),
           click_saved=False),
]


# --- the applied values (read by ui/maya_grid.py every draw) ----------------------------------------

def defaults():
    return {f.name: f.default for f in FIELDS}


_CACHE = {}


def grid_settings():
    """Maya grid options in Maya units: the applied ones over the factory ones (cached on the stored text)."""
    from .. import preferences
    try:
        raw = preferences.get().option_settings or ""
    except (KeyError, AttributeError):
        raw = ""
    hit = _CACHE.get("raw")
    if hit is not None and hit == raw:
        return _CACHE["values"]
    values = defaults()
    try:
        saved = json.loads(raw).get(KEY) if raw else None
    except ValueError:
        saved = None
    for k, val in (saved or {}).items():
        if k in values:
            values[k] = tuple(val) if isinstance(values[k], tuple) else type(values[k])(val)
    _CACHE["raw"], _CACHE["values"] = raw, values
    return values
