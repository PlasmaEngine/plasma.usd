# SPDX-License-Identifier: GPL-3.0-or-later
"""Maya Graph Editor View / Show modes Blender lacks (step 150 lane A, #96).

Maya 2022's Graph Editor View menu (with its factory
states) has check / radio items Blender's Graph Editor has no setting for. The kit adds them as add-on
preferences (``preferences.graph_auto_frame`` / ``graph_auto_frame_time`` / ``graph_curve_names``; the kit writes no
Blender preference, store review #40) mapped in ``editor_menu_map``:

* **Auto Frame** (Maya factory on, "As selection changes, reframe the selected curves"): a timer compares the object
  / pose-bone selection; when it changes, every Graph Editor in F-Curve mode runs ``graph.view_all`` in its WINDOW
  region (Blender lists the selected objects' curves with Only Show Selected, so that frames them).
* **Auto Frame Time** (factory off, "... reframe ... along the time axis"): only the time axis. Python can't set a
  View2D, and ``view2d.zoom`` divides by the UI scale (NaN headless, measured), so the kit uses Blender's own
  horizontal-only frame, ``anim.scene_range_frame`` (sets the view's time range to the scene / preview range exactly,
  value axis kept, measured 4.5): the preview range is set to the curves' key range + 5 % for that one call and put
  back at once.
* **Curve Name** Always / Never / Active Only (factory Never): a WINDOW POST_PIXEL handler writes each visible
  curve's Maya plug name ("pCube1.translateX") in the curve colour just right of its last key (clamped into the
  view), at the curve's stored value — the view's value axis is the stored value (step 137). Normalized views
  (``use_normalization``) are skipped: Blender's normalisation factor isn't exposed.
* **Show > Objects > Clear Below**: Maya's result (no object-type filter left = every type shown) = Blender's type
  filters back on (``allset.graph_show_clear``).
"""

import math
import re

import bpy

from .. import live_refs

POLL = 0.25                       # s between selection checks (like operators/graph_keys snap time, step 92)
TIME_MARGIN = 0.05                # Auto Frame Time: share of the key span added before / after
LABEL_POINTS = 11.0               # Blender's default UI text size
LABEL_DX = 6.0                    # px right of the last key (x UI scale)
LABEL_DY = 4.0                    # baseline px above the curve (x UI scale)

# Show > Objects > Clear Below turns these type filters back on (the ones the Show > Objects items map to)
OBJECT_FILTERS = ("show_meshes", "show_curves", "show_cache_files", "show_cameras", "show_armatures")

# ... and the kit's own Show > Objects items (WindowManager flags, ui/editor_chrome.py) off
KIT_OBJECT_FILTERS = ("allset_show_ik_handles", "allset_show_sets", "allset_show_characters")

_state = {"sig": None}
_handles = []


# --- bpy-free maths / names -----------------------------------------------------------------------------------

_BONE = re.compile(r'^pose\.bones\["((?:[^"\\]|\\.)*)"\]\.')
_SHAPE = re.compile(r'^key_blocks\["((?:[^"\\]|\\.)*)"\]\.')


def maya_attr(label):
    """Maya long attribute name of a Graph Editor label: "Translate X" -> "translateX" (custom names as they are)."""
    words = label.split()
    if not words or len(words) == 1 and not words[0][:1].isupper():
        return label
    return words[0][:1].lower() + words[0][1:] + "".join(w[:1].upper() + w[1:] for w in words[1:])


def curve_node(owner_name, data_path):
    """Maya node a curve belongs to: the pose bone (joint) or shape key name, else the animated object."""
    for rx in (_BONE, _SHAPE):
        m = rx.match(data_path)
        if m:
            return m.group(1).replace('\\"', '"')
    return owner_name


def plug_name(owner_name, data_path, label):
    """Maya curve name drawn by View > Curve Name: "<node>.<attribute>" ("pCube1.translateX")."""
    return f"{curve_node(owner_name, data_path)}.{maya_attr(label)}"


def time_span(times, margin=TIME_MARGIN):
    """(first, last) whole frames framing ``times`` with ``margin`` x span on both sides; None without times."""
    times = list(times)
    if not times:
        return None
    lo, hi = min(times), max(times)
    pad = max(hi - lo, 1.0) * margin
    return int(math.floor(lo - pad)), max(int(math.ceil(hi + pad)), int(math.floor(lo - pad)) + 1)


def shown_curves(fcurves, mode):
    """Curves that get a name: 'ALWAYS' every visible one, 'ACTIVE' the selected (Maya active) ones."""
    if mode == 'ALWAYS':
        return list(fcurves)
    if mode == 'ACTIVE':
        return [fc for fc in fcurves if fc.select]
    return []


def label_anchor(points, view_x0, view_x1):
    """(x, value) where a curve's name goes: its last key if it is in view, else the curve at the view's right
    edge (``points`` = [(time, value)] sorted; evaluated by the caller for the edge). None = curve not in view."""
    if not points:
        return None
    x_last = points[-1][0]
    if x_last < view_x0:
        return None                  # the view is right of the curve: Maya draws no name for it either
    return (x_last, points[-1][1]) if x_last <= view_x1 else (view_x1, None)


# --- selection signature / framing ----------------------------------------------------------------------------

def selection_signature(view_layer):
    """(selected object names, active name, selected pose bones of the active armature) — cheap."""
    if view_layer is None:
        return None
    try:
        sel = tuple(sorted(o.name for o in view_layer.objects.selected if o is not None))
        act = view_layer.objects.active
    except (AttributeError, ReferenceError):
        return None
    bones = ()
    if act is not None and act.type == 'ARMATURE' and act.mode == 'POSE' and act.pose is not None:
        from . import channel_box
        bones = tuple(pb.name for pb in act.pose.bones if channel_box.bone_selected(pb))
    return sel, act.name if act is not None else "", bones


def graph_editors():
    """[(window, area, WINDOW region)] of every Graph Editor in F-Curve mode."""
    out = []
    for w in live_refs.windows():
        screen = w.screen
        if screen is None:
            continue
        for a in screen.areas:
            if a.type != 'GRAPH_EDITOR' or a.spaces.active.mode != 'FCURVES':
                continue
            reg = next((r for r in a.regions if r.type == 'WINDOW'), None)
            if reg is not None:
                out.append((w, a, reg))
    return out


def dope_sheets():
    """[(window, area, WINDOW region)] of every Dope Sheet (its Dope Sheet / Action modes; not the Timeline)."""
    out = []
    for w in live_refs.windows():
        screen = w.screen
        if screen is None:
            continue
        for a in screen.areas:
            if a.type != 'DOPESHEET_EDITOR' or a.spaces.active.mode == 'TIMELINE':
                continue
            reg = next((r for r in a.regions if r.type == 'WINDOW'), None)
            if reg is not None:
                out.append((w, a, reg))
    return out


def frame_dope_sheet(window, area, region):
    """Maya Dope Sheet Auto Frame: Blender's View All in this Dope Sheet."""
    with bpy.context.temp_override(window=window, area=area, region=region):
        try:
            return 'FINISHED' in bpy.ops.action.view_all()
        except RuntimeError:
            return False


def visible_curves(window, area, region):
    with bpy.context.temp_override(window=window, area=area, region=region):
        return list(getattr(bpy.context, "visible_fcurves", None) or ())


def frame_all(window, area, region):
    """Maya Auto Frame: Blender's View All in this Graph Editor (its visible curves). True when it framed."""
    if not visible_curves(window, area, region):
        return False
    with bpy.context.temp_override(window=window, area=area, region=region):
        return 'FINISHED' in bpy.ops.graph.view_all()


def frame_time(window, area, region, scene):
    """Maya Auto Frame Time: frame the visible curves' keys along time only (value axis kept)."""
    span = time_span(k.co.x for fc in visible_curves(window, area, region) for k in fc.keyframe_points)
    if span is None:
        return False
    saved = (scene.use_preview_range, scene.frame_preview_start, scene.frame_preview_end)
    try:
        scene.use_preview_range = True
        scene.frame_preview_end = span[1]
        scene.frame_preview_start = span[0]
        scene.frame_preview_end = span[1]
        with bpy.context.temp_override(window=window, area=area, region=region):
            done = 'FINISHED' in bpy.ops.anim.scene_range_frame()
    finally:
        on, start, end = saved
        for _ in range(2):            # Blender pushes the other end of the range along: set both twice
            scene.frame_preview_start = start
            scene.frame_preview_end = end
        scene.use_preview_range = on
    return done


def auto_frame(scene, prefs):
    """Frame every Graph Editor the Maya way for the current settings; the number of editors framed."""
    n = 0
    if prefs.graph_auto_frame or prefs.graph_auto_frame_time:
        for w, a, r in graph_editors():
            if prefs.graph_auto_frame:
                n += frame_all(w, a, r)
            else:
                n += frame_time(w, a, r, scene)
    if getattr(prefs, "dope_auto_frame", False):
        for w, a, r in dope_sheets():
            n += frame_dope_sheet(w, a, r)
    return n


def step(context, prefs):
    """One poll: frame when the selection changed since the last poll (and a Graph Editor shows). Returns the number
    of editors framed. The first poll after a Graph Editor opens only takes the baseline."""
    graph = (prefs.graph_auto_frame or prefs.graph_auto_frame_time) and graph_editors()
    dope = getattr(prefs, "dope_auto_frame", False) and dope_sheets()
    if not (graph or dope):
        _state["sig"] = None
        return 0
    sig = selection_signature(getattr(context, "view_layer", None))
    last, _state["sig"] = _state["sig"], sig
    if last is None or sig is None or sig == last:
        return 0
    return auto_frame(context.scene, prefs)


_sidebar_done = set()       # Graph Editor areas whose sidebar the kit already closed once


def hide_sidebars(context):
    """Maya's Graph Editor has no side panel (step 198, #105; audit 14 b04_graph_editor.png showed Blender's Active
    F-Curve / "RNA Array Index" sidebar in a Graph Editor shown in a main pane). Close it once per Graph Editor area
    while the kit's editor chrome is on; N opens it again and stays open."""
    from . import editor_chrome
    if not editor_chrome.enabled(context):
        return
    live = set()
    for window in context.window_manager.windows:
        screen = window.screen
        if screen is None:
            continue
        for area in screen.areas:
            if area.type != 'GRAPH_EDITOR':
                continue
            ptr = area.as_pointer()
            live.add(ptr)
            space = area.spaces.active
            if ptr in _sidebar_done or space.mode != 'FCURVES':
                continue
            _sidebar_done.add(ptr)
            if space.show_region_ui:
                with context.temp_override(window=window, area=area):
                    space.show_region_ui = False
    _sidebar_done.intersection_update(live)     # freed area pointers are reused (CLAUDE.md step 9)


def _tick():
    try:
        from .. import preferences
        step(bpy.context, preferences.get())
        hide_sidebars(bpy.context)
        from . import editor_chrome
        editor_chrome.sync(bpy.context)                   # step 203: loaded lists, pins, Show filters
        from ..operators import graph_weights
        watch = getattr(graph_weights, "watch", None)     # step 198: non-weighted / locked tangent lengths
        if watch is not None:
            watch(bpy.context)
        from ..operators import graph_menu_198
        graph_menu_198.watch(bpy.context)                 # step 198: Set Curve Colors rules on new curves
    except (KeyError, AttributeError, ReferenceError, RuntimeError):
        pass
    return POLL


@bpy.app.handlers.persistent
def _reset(*_args):
    _state["sig"] = None


# --- curve names ----------------------------------------------------------------------------------------------

def _owner_names():
    """{action pointer: first animated ID name} (a 4.5 F-curve's id_data is its Action)."""
    from . import graph_units
    return {ptr: ids[0].name for ptr, ids in graph_units._owners().items() if ids}


def _label(fc):
    from . import editor_chrome
    fn = getattr(editor_chrome, "attr_label", None)
    return fn(fc) if fn else fc.data_path.rsplit(".", 1)[-1]


def name_items(context, region):
    """[(region x, region y, text, rgba)] of the curve names this Graph Editor draws now."""
    space = context.space_data
    if space is None or space.type != 'GRAPH_EDITOR' or space.mode != 'FCURVES' or space.use_normalization:
        return []
    from .. import preferences
    try:
        mode = preferences.get(context).graph_curve_names
    except KeyError:
        return []
    fcs = shown_curves(getattr(context, "visible_fcurves", None) or (), mode)
    if not fcs:
        return []
    v2d = region.view2d
    x0 = v2d.region_to_view(0, 0)[0]
    x1 = v2d.region_to_view(region.width, 0)[0]
    owners = None
    out = []
    for fc in fcs:
        pts = sorted((k.co.x, k.co.y) for k in fc.keyframe_points)
        anchor = label_anchor(pts, x0, x1)
        if anchor is None:
            continue
        x, y = anchor
        if y is None:
            y = fc.evaluate(x)
        idb = fc.id_data
        if isinstance(idb, bpy.types.Action):
            if owners is None:
                owners = _owner_names()
            owner = owners.get(idb.as_pointer(), idb.name)
        else:
            owner = idb.name
        rx, ry = v2d.view_to_region(x, y, clip=False)
        out.append((rx, ry, plug_name(owner, fc.data_path, _label(fc)), (*fc.color[:3], 1.0)))
    return out


def _draw_names():
    context = bpy.context
    region = context.region
    if region is None or region.type != 'WINDOW':
        return
    try:
        items = name_items(context, region)
    except (AttributeError, ReferenceError):
        return
    if not items:
        return
    import blf
    s = context.preferences.system.ui_scale or 1.0
    font = 0
    blf.size(font, LABEL_POINTS * s)
    blf.enable(font, blf.SHADOW)
    blf.shadow(font, 6, 0.0, 0.0, 0.0, 0.8)
    blf.shadow_offset(font, 0, 0)
    for x, y, text, rgba in items:
        w, h = blf.dimensions(font, text)
        px = min(x + LABEL_DX * s, region.width - w - 4 * s)
        # above the curve (the constant extrapolation line runs on through the last key: text on it is unreadable)
        py = min(max(y + LABEL_DY * s, 4 * s), region.height - h - 4 * s)
        blf.color(font, *rgba)
        blf.position(font, round(px), round(py), 0)
        blf.draw(font, text)
    blf.disable(font, blf.SHADOW)


# --- Show > Objects > Clear Below -----------------------------------------------------------------------------

class ALLSET_OT_graph_show_clear(bpy.types.Operator):
    """Clear the object-type filters: every object type is listed again (Maya Show > Objects > Clear Below)"""
    bl_idname = "allset.graph_show_clear"
    bl_label = "Clear Below"
    bl_options = {'UNDO'}

    @classmethod
    def poll(cls, context):
        space = context.space_data
        return space is not None and space.type in {'GRAPH_EDITOR', 'DOPESHEET_EDITOR'}

    def execute(self, context):
        ds = context.space_data.dopesheet
        for attr in OBJECT_FILTERS:
            setattr(ds, attr, True)
        wm = context.window_manager
        for attr in KIT_OBJECT_FILTERS:      # step 203: the kit's Show > Objects check items
            if getattr(wm, attr, False):
                setattr(wm, attr, False)
        return {'FINISHED'}


def register():
    bpy.utils.register_class(ALLSET_OT_graph_show_clear)
    _handles.append(bpy.types.SpaceGraphEditor.draw_handler_add(_draw_names, (), 'WINDOW', 'POST_PIXEL'))
    bpy.app.timers.register(_tick, first_interval=POLL, persistent=True)
    bpy.app.handlers.load_post.append(_reset)
    _state["sig"] = None


def unregister():
    if _reset in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.remove(_reset)
    if bpy.app.timers.is_registered(_tick):
        bpy.app.timers.unregister(_tick)
    while _handles:
        bpy.types.SpaceGraphEditor.draw_handler_remove(_handles.pop(), 'WINDOW')
    bpy.utils.unregister_class(ALLSET_OT_graph_show_clear)
    _state["sig"] = None
