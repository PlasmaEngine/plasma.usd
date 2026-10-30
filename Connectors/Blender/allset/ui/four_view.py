# SPDX-License-Identifier: GPL-3.0-or-later
"""Maya Four View = four real 3D View panels (step 237, Issue #148).

Blender's Quad View splits one 3D View region in four: one header, one shading / overlay set for all four panes. A
Maya model panel has its own camera, shading, Show settings and panel menu bar, so the kit's four view is four 3D
View *areas* (``screen.area_split`` three times), each with its own ``SpaceView3D``:

    top   | persp
    ------+------
    front | side

Blender's split rules (measured, CLAUDE.md steps 49 / 106 / 116; ``hypershade_layout`` docstring): HORIZONTAL with
factor > 0.5 puts the new area on top, VERTICAL with factor < 0.5 on the left — so the panels' places are known
without reading rects (areas get their rects one tick after a split).

Space tap / Panels > Layouts > Single Perspective View leave the four view by joining the other three areas into the
panel under the mouse (or the persp panel) with ``screen.area_join`` (GUI only).
Each panel's settings (shading, overlays, object types shown, lens / clip, camera) are kept per *role* (top / persp /
front / side) as JSON in the Screen ID property ``allset_four_view`` — saved with the file, never area pointers
(#36) — and put back when the four view comes back, like Maya's panels keep theirs. Blender Quad View is not used.
"""

import json
import math

import bpy
from mathutils import Euler, Quaternion

PROP = "allset_four_view"
ROLES = ("top", "persp", "front", "side")
# (column, row from the top) of each role in Maya's Four View
SLOTS = {"top": (0, 0), "persp": (1, 0), "front": (0, 1), "side": (1, 1)}
# Blender view rotations of Maya's ortho cameras (Maya side looks from +X = Blender Right view)
AXIS = {"top": Quaternion(), "front": Euler((math.pi / 2, 0.0, 0.0)).to_quaternion(),
        "side": Euler((math.pi / 2, 0.0, math.pi / 2)).to_quaternion()}
# Maya 2026 factory shading of a new four view: every panel Smooth Shade All (= Blender Solid)
DEFAULT_SHADING = {"top": 'SOLID', "persp": 'SOLID', "front": 'SOLID', "side": 'SOLID'}
SPLITS = (0.5001, 0.4999)            # HORIZONTAL (new on top), VERTICAL (new on the left)

_SPACE_PROPS = ("lens", "clip_start", "clip_end")
_SKIP = {"rna_type"}


# --- the record on the screen --------------------------------------------------------------------------------------

def state(screen):
    try:
        return json.loads(screen.get(PROP) or "{}")
    except (TypeError, ValueError):
        return {}


def _save_state(screen, st):
    screen[PROP] = json.dumps(st)


def is_four(screen):
    return screen is not None and bool(state(screen).get("on")) and len(panes(screen)) == 4


def panes(screen):
    """The 3D View panels of ``screen`` (the Tool Box column is not a panel)."""
    from .toolbox import is_toolbox
    return [a for a in screen.areas if a.type == 'VIEW_3D' and not is_toolbox(a.spaces.active)]


def roles(screen, areas=None):
    """{role: area} of a four view by the panels' places (GUI: rects)."""
    areas = areas if areas is not None else panes(screen)
    if len(areas) != 4:
        return {}
    cx = sum(a.x + a.width / 2 for a in areas) / 4
    cy = sum(a.y + a.height / 2 for a in areas) / 4
    out = {}
    for a in areas:
        col = 0 if a.x + a.width / 2 < cx else 1
        row = 0 if a.y + a.height / 2 > cy else 1           # Blender y goes up
        role = next(r for r, s in SLOTS.items() if s == (col, row))
        out.setdefault(role, a)
    return out if len(out) == 4 else {}


# --- panel settings -------------------------------------------------------------------------------------------------

def _props(struct):
    out = {}
    for p in struct.bl_rna.properties:
        if p.is_readonly or p.identifier in _SKIP or p.type in ('POINTER', 'COLLECTION'):
            continue
        v = getattr(struct, p.identifier)
        if p.type == 'ENUM' and p.is_enum_flag:
            v = sorted(v)
        elif not isinstance(v, (str, int, float, bool)):
            try:
                v = [float(x) for x in v]
            except TypeError:
                continue
        out[p.identifier] = v
    return out


def _apply_props(struct, values, first=("type",)):
    flags = {p.identifier for p in struct.bl_rna.properties if p.type == 'ENUM' and p.is_enum_flag}
    for key in (*[k for k in first if k in values], *[k for k in values if k not in first]):
        try:
            setattr(struct, key, set(values[key]) if key in flags else values[key])
        except (AttributeError, TypeError, ValueError):
            pass                     # a value this Blender / scene can't take (a studio light that went away ...)


def capture(space):
    """One panel's settings: shading, overlays, object types shown, lens / clip and the camera."""
    rv3d = space.region_3d
    view = {}
    if rv3d is not None:
        view = {"perspective": rv3d.view_perspective, "rotation": list(rv3d.view_rotation),
                "location": list(rv3d.view_location), "distance": rv3d.view_distance,
                "side": bool(rv3d.is_orthographic_side_view)}
    shown = {p.identifier: getattr(space, p.identifier) for p in space.bl_rna.properties
             if p.identifier.startswith("show_object_viewport_")}
    return {"shading": _props(space.shading), "overlay": _props(space.overlay),
            "space": {k: getattr(space, k) for k in _SPACE_PROPS}, "shown": shown, "view": view}


def apply(space, data):
    _apply_props(space.shading, data.get("shading", {}))
    _apply_props(space.overlay, data.get("overlay", {}), first=())
    _apply_props(space, {**data.get("space", {}), **data.get("shown", {})}, first=())
    view, rv3d = data.get("view"), space.region_3d
    if view and rv3d is not None:
        rv3d.view_perspective = view["perspective"] if view["perspective"] != 'CAMERA' else 'PERSP'
        rv3d.view_rotation = Quaternion(view["rotation"])
        rv3d.view_location = view["location"]
        rv3d.view_distance = view["distance"]
        if view["perspective"] == 'ORTHO':
            rv3d.is_orthographic_side_view = view["side"]


def default(space, role, scene):
    """A panel showing Maya's factory camera for ``role`` (persp = Maya's new-scene persp)."""
    from ..operators import units, view
    space.shading.type = DEFAULT_SHADING[role]
    rv3d = space.region_3d
    if rv3d is None:
        return
    if role == "persp":
        rv3d.view_perspective = 'PERSP'
        view.home_view(rv3d, units.maya_unit(scene))
        return
    rv3d.view_perspective = 'ORTHO'
    rv3d.view_rotation = AXIS[role]
    rv3d.view_location = (0.0, 0.0, 0.0)
    rv3d.view_distance = view.maya_ortho_distance(space, scene)
    rv3d.is_orthographic_side_view = True


# --- four <-> one ---------------------------------------------------------------------------------------------------

def _window_region(area):
    return next((r for r in area.regions if r.type == 'WINDOW'), None)


def _split(context, window, area, direction, factor):
    screen = window.screen
    before = {a.as_pointer() for a in screen.areas}
    with context.temp_override(window=window, screen=screen, area=area, region=_window_region(area)):
        if bpy.ops.screen.area_split(direction=direction, factor=factor) != {'FINISHED'}:
            return None
    return next((a for a in screen.areas if a.as_pointer() not in before), None)


def _place(context, role, area, saved):
    space = area.spaces.active
    if role in saved:
        apply(space, saved[role])
    else:
        default(space, role, context.scene)
    area.tag_redraw()


def enter(context, area, window=None):
    """``area`` (one 3D View panel) becomes the four view. Returns the roles placed now ({} when it can't split: always
    in background mode, where a split-off area stays 0 x 0).

    Splits: ``area`` top / bottom, then the bottom left / right (front / side). A just-split area has no size until
    the next redraw (measured in the GUI too: splitting it in the same call fails), so the top part is split into
    top / persp from a timer one tick later (``_split_top_later``; pointers only, #36)."""
    window = window or context.window
    screen = window.screen
    if bpy.app.background:
        return {}
    st = state(screen)
    saved = st.get("panes", {})
    single = st.get("single", "persp")
    saved[single] = capture(area.spaces.active)
    top = _split(context, window, area, 'HORIZONTAL', SPLITS[0])
    if top is None:
        return {}
    bottom_left = _split(context, window, area, 'VERTICAL', SPLITS[1])
    if bottom_left is None:
        return {}
    placed = {"persp": top, "front": bottom_left, "side": area}
    for role, a in placed.items():
        _place(context, role, a, saved)
    st.update(on=True, panes=saved)
    _save_state(screen, st)
    _split_top_later(window.as_pointer(), top.as_pointer())
    return placed


def _split_top_later(win_ptr, top_ptr, tries=20):
    from .. import live_refs

    def tick():
        window, top = live_refs.find_area(top_ptr, win_ptr)
        if window is None or top is None:
            return None                                    # the window / panel went away
        if top.width < 2 or top.height < 2:
            nonlocal tries
            tries -= 1
            return 0.05 if tries > 0 else None
        left = _split(bpy.context, window, top, 'VERTICAL', SPLITS[1])
        if left is not None:
            saved = state(window.screen).get("panes", {})
            _place(bpy.context, "top", left, saved)
            _place(bpy.context, "persp", top, saved)
        return None
    bpy.app.timers.register(tick, first_interval=0.0)


def remember(screen):
    """Keep the four panels' settings on the screen (Maya's panels keep theirs across layouts)."""
    placed = roles(screen)
    if not placed:
        return {}
    st = state(screen)
    saved = st.get("panes", {})
    for role, a in placed.items():
        saved[role] = capture(a.spaces.active)
    st["panes"] = saved
    _save_state(screen, st)
    return placed


def leave(context, keep=None, window=None, role=None):
    """Back to one panel: ``keep`` (an area of the four view) or the panel of ``role`` stays. GUI only."""
    window = window or context.window
    screen = window.screen
    placed = remember(screen)
    st = state(screen)
    if not placed:
        st["on"] = False
        _save_state(screen, st)
        return None
    if keep is None or keep.as_pointer() not in {a.as_pointer() for a in placed.values()}:
        keep = placed[role or "persp"]
    keep_role = next(r for r, a in placed.items() if a == keep)
    if bpy.app.background:           # screen.area_join crashes without a UI (CLAUDE.md step 49)
        return None
    keep_ptr = keep.as_pointer()
    _join_into(context, window, placed, keep_role)
    kept = next((a for a in screen.areas if a.as_pointer() == keep_ptr), None)
    st.update(on=False, single=keep_role)
    _save_state(screen, st)
    if kept is not None:
        kept.tag_redraw()
    return kept


ROWS = (("top", "persp"), ("front", "side"))


def _centre(*areas):
    x0 = min(a.x for a in areas)
    y0 = min(a.y for a in areas)
    x1 = max(a.x + a.width for a in areas)
    y1 = max(a.y + a.height for a in areas)
    return (x0 + x1) // 2, (y0 + y1) // 2


def _join(context, window, source_xy, target_xy):
    """``screen.area_join``: the area at ``source_xy`` takes over the one at ``target_xy`` (window px; same props in
    4.5 / 5.0 / 5.2, measured)."""
    with context.temp_override(window=window, screen=window.screen):
        try:
            return bpy.ops.screen.area_join(source_xy=source_xy, target_xy=target_xy) == {'FINISHED'}
        except RuntimeError:
            return False


def _join_into(context, window, placed, keep_role):
    """The four panels back into the kept one, joined only with each other (``screen.area_close`` gave a closing
    panel to whichever neighbour shared its edge — the Outliner / Properties column took half the viewport,
    measured). The other row first, then the kept panel takes its row mate, then the other row."""
    mine = next(r for r in ROWS if keep_role in r)
    other = next(r for r in ROWS if keep_role not in r)
    mate = next(r for r in mine if r != keep_role)
    k, m = placed[keep_role], placed[mate]
    o0, o1 = placed[other[0]], placed[other[1]]
    keep_c, mate_c = _centre(k), _centre(m)
    other_c = _centre(o0, o1)
    _join(context, window, _centre(o0), _centre(o1))
    _join(context, window, keep_c, mate_c)
    _join(context, window, keep_c, other_c)


def toggle(context, area=None, window=None):
    """Maya Space tap: four view -> the panel under the mouse alone; one panel -> the four view again."""
    window = window or context.window
    area = area or context.area
    from .toolbox import is_toolbox
    if window is None or area is None or area.type != 'VIEW_3D' or is_toolbox(area.spaces.active):
        return None
    if is_four(window.screen):
        return leave(context, keep=area, window=window)
    return enter(context, area, window)


def collapse_all(context):
    """Every four view back to its persp panel (switching to Blender / disabling: the user's screen as it was)."""
    if bpy.app.background:
        return 0
    n = 0
    for window in context.window_manager.windows:
        if is_four(window.screen):
            if leave(context, window=window, role="persp") is not None:
                n += 1
    return n
