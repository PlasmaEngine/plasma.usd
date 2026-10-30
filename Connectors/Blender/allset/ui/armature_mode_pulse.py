# SPDX-License-Identifier: GPL-3.0-or-later
"""A quiet, local highlight for the viewport's armature mode dropdown.

No Maya version has a pulsing header element (PR125_MAYA_LATER.md 4a): it only runs with the add-on preference
``armature_mode_pulse`` on (default off), and the timer idles cheaply otherwise."""

import math
import time

import blf
import bpy
import gpu
from gpu_extras.batch import batch_for_shader

PERIOD = 2.8
CUSTOM_UNITS = 8.4
COLOR = (0.18, 0.72, 0.65)
_handles = []
_custom = {}


def wanted(context=None):
    from .. import preferences
    try:
        return bool(getattr(preferences.get(context), "armature_mode_pulse", False))
    except KeyError:
        return False


def eligible(context):
    if not wanted(context):
        return False
    ob = context.active_object
    return (ob is not None and ob.type == 'ARMATURE' and ob.select_get()
            and context.mode in {'OBJECT', 'POSE', 'EDIT_ARMATURE'})


def opacity(seconds):
    # Zero slope at each end: no flashing or abrupt reversals.
    return 0.07 + 0.27 * (0.5 - 0.5 * math.cos(math.tau * seconds / PERIOD))


def mark(context):
    if context.region is not None:
        _custom[context.region.as_pointer()] = context.area.as_pointer()


def forget(context):
    if context.region is not None:
        _custom.pop(context.region.as_pointer(), None)


def _custom_menu(context, region):
    return _custom.get(region.as_pointer()) == context.area.as_pointer()


def _has_mode_menu(context):
    from . import panel_toolbar
    return (any(_custom_menu(context, r) for r in context.area.regions if r.type == 'HEADER')
            or not panel_toolbar.maya_layout(context))


def _draw():
    context = bpy.context
    region = context.region
    if (region is None or region.type != 'HEADER' or not eligible(context)
            or not _has_mode_menu(context)):
        return
    scale = context.preferences.system.ui_scale or 1.0
    item = bpy.types.Object.bl_rna.properties['mode'].enum_items[context.active_object.mode]
    label = bpy.app.translations.pgettext_iface(
        item.name, bpy.types.Object.bl_rna.properties['mode'].translation_context)
    blf.size(0, 11 * scale)
    text_width = blf.dimensions(0, label)[0]
    # Native header: the editor selector occupies 1.6 UI units, then the
    # mode button has an icon, label, and dropdown arrow. View2D accounts for
    # horizontal header scrolling and header zoom.
    if _custom_menu(context, region):
        from .toolbar_fit import unit
        u = unit(scale, context.preferences.system.pixel_size)
        start, width = int(0.4 * u), int(CUSTOM_UNITS * u)
    else:
        start, width = 40 * scale, text_width + 46 * scale
    x0 = region.view2d.view_to_region(start, 0, clip=False)[0]
    x1 = region.view2d.view_to_region(start + width, 0, clip=False)[0]
    y0, y1 = 4 * scale, region.height - 4 * scale
    if x1 <= 0 or x0 >= region.width or y1 <= y0:
        return
    radius = min(4 * scale, (y1 - y0) / 2)
    vertices = []
    for cx, cy, start in ((x1-radius, y1-radius, 0), (x0+radius, y1-radius, 90),
                          (x0+radius, y0+radius, 180), (x1-radius, y0+radius, 270)):
        for i in range(7):
            angle = math.radians(start + i * 15)
            vertices.append((cx + radius * math.cos(angle), cy + radius * math.sin(angle)))
    shader = gpu.shader.from_builtin('UNIFORM_COLOR')
    batch = batch_for_shader(shader, 'TRIS', {'pos': vertices},
                             indices=[(0, i, i + 1) for i in range(1, len(vertices) - 1)])
    previous_blend = gpu.state.blend_get()
    try:
        gpu.state.blend_set('ALPHA')
        shader.bind()
        shader.uniform_float('color', (*COLOR, opacity(time.monotonic())))
        batch.draw(shader)
    finally:
        gpu.state.blend_set(previous_blend)


def _tick():
    if not wanted():
        return 0.5
    active = False
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type != 'VIEW_3D':
                continue
            with bpy.context.temp_override(window=window, area=area):
                if eligible(bpy.context) and _has_mode_menu(bpy.context):
                    for region in area.regions:
                        if region.type == 'HEADER':
                            region.tag_redraw()
                    active = True
    return 1 / 24 if active else 0.2


def register():
    if not _handles:
        _handles.append(bpy.types.SpaceView3D.draw_handler_add(_draw, (), 'HEADER', 'POST_PIXEL'))
    if not bpy.app.timers.is_registered(_tick):
        bpy.app.timers.register(_tick, first_interval=0.2, persistent=True)
    _redraw()


def unregister():
    if bpy.app.timers.is_registered(_tick):
        bpy.app.timers.unregister(_tick)
    while _handles:
        bpy.types.SpaceView3D.draw_handler_remove(_handles.pop(), 'HEADER')
    _custom.clear()
    _redraw()


def _redraw():
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == 'VIEW_3D':
                area.tag_redraw()
