# SPDX-License-Identifier: GPL-3.0-or-later
"""Maya-style UI: Channel Box, Layer Editor, Attribute Editor, Outliner (3D View
sidebar), menu bar + menu sets, Status Line (top bar), Shelf (strip under it), Time
Slider + Range Slider (Timeline header), viewport panel menus (3D View header), Maya grid, view furniture and selection wire (3D View),
Tool Box (3D View toolbar) and the Maya dock areas / floating panels (maya_dock, step 116).

Blender's Python UI can't replace every C-drawn part; the modules note where they work around it.
"""

from . import (attribute_editor, cb_channels, cb_points, workspaces, editor_chrome, file_project, render_settings, script_editor, input_line, graph_editor, graph_units, graph_view, hypershade, light_editor, render_view, channel_box, dock_tabs, layer_editor, maya_grid, maya_outliner, menu_sets, outliner_sets, notices, outliner,
               creation_inputs, in_view, manipulator, mesh_tools, modeling_toolkit, uv_toolkit, paint_weights, weight_numbers, panel_menus, camera_tools, light_link_editor, set_driven_key, shelf, selection_wire, component_colors, status_line, time_slider,
               tool_settings, toolbox, view_furniture, view_message, range_bar, status_icons, kit_icons, panel_toolbar, maya_dock, soft_overlay, save_prompt, scene_carry, armature_mode_pulse)
from . import mesh_tools_poly  # step 144 POLY (#30)
from . import mesh_tools_loop  # step 144 LOOP (#31)
from . import uv_display  # step 155 UV Editor display keys / Move UV Shell
from . import uv_hud  # step 206 UV Editor heads-up displays (Isolate Select / UV Position / UV Statistics)
from . import hs_drag  # step 162: Hypershade swatch MMB drag
from . import ghosting  # step 171: Maya ghosting (Visualize)
from . import reference_editor  # step 209: Maya Reference Editor window
from . import outliner_joints  # step 258: kit skeleton rows read their root joint (#155)
from . import outliner_order  # step 211: Maya Outliner order (startup cameras first, creation order)
from . import marked_window, component_editor, shape_editor, connection_editor  # step 213 (#119)
from . import hotkey_editor  # step 215 (#121): Maya Hotkey Editor window
from . import maya_prefs  # step 215 (#121): Maya Preferences window

_modules = (status_icons, kit_icons, notices, input_line, cb_points, channel_box, cb_channels, creation_inputs, layer_editor, attribute_editor, outliner, menu_sets, workspaces, file_project, status_line, shelf,
            time_slider, panel_menus, panel_toolbar, camera_tools, light_link_editor, maya_grid, view_furniture, selection_wire, component_colors, ghosting, toolbox, dock_tabs, maya_outliner, outliner_joints, outliner_sets, outliner_order,
            paint_weights, weight_numbers, manipulator, set_driven_key, modeling_toolkit, mesh_tools, mesh_tools_poly, mesh_tools_loop, in_view, uv_toolkit, uv_display, uv_hud, hypershade, hs_drag, light_editor, render_view, graph_editor, graph_units, graph_view, editor_chrome, render_settings, script_editor, reference_editor, marked_window, component_editor, shape_editor, connection_editor, hotkey_editor, maya_prefs,
            view_message, range_bar, tool_settings, maya_dock, soft_overlay, save_prompt, scene_carry, armature_mode_pulse)


def register():
    for mod in _modules:
        mod.register()


def unregister():
    for mod in reversed(_modules):
        mod.unregister()
