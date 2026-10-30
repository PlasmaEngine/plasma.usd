# SPDX-License-Identifier: GPL-3.0-or-later
"""Maya soft select look in the 3D View (issues #1, #2; docs/steps/step_129_softsel.md).

- Falloff colours (POST_VIEW): in Edit Mesh with soft select on, every vertex the transform will move gets Maya's
  false colour (Maya's soft select colour ramp: yellow at weight 1, red at 0.5, black at 0) on its edges
  and, in vertex select mode, its point — Maya colours the wireframe.
  Weights = Blender's transform rule (``operators/soft_select.weights``)
  with the scene's radius, falloff, Surface (connected) and Projected settings, so the colours show what G / R / S
  will move.
- Radius drag (POST_PIXEL): while B + LMB / MMB drags, a circle of the radius around the soft select centre and
  Maya's ``Falloff Radius: <value>`` next to the mouse.

Performance (dense meshes): per object cache of world positions / edges / selection / weights / GPU batches. The
depsgraph handler only marks objects dirty; the draw re-reads the selection when dirty and recomputes weights
only when (selection, radius, falloff, connected, projected view axis, geometry) changed. During a running
transform Blender's weights are fixed at its start, so only the drawn vertices' positions are re-read.
"""

from itertools import chain

import bpy
import gpu
import numpy as np
from gpu_extras.batch import batch_for_shader

LINE_PX = 2.0
POINT_PX = 5.0
CIRCLE_RGBA = (1.0, 1.0, 1.0, 0.85)
TEXT_RGBA = (1.0, 1.0, 1.0, 1.0)
DEPTH_BIAS = 0.001          # relative pull toward the camera so the colours win over Blender's edit wire

_handles = []
_cache = {}                 # object pointer -> _State
_dirty = {"geom": set(), "any": set()}     # mesh / object names touched since the last draw
_drag = {}                  # radius drag display state (empty = no drag)
STATS = {"weights": 0, "batches": 0}


def _units():
    from ..operators import units
    return units


def _soft():
    from ..operators import soft_select
    return soft_select


# --- per object state ----------------------------------------------------------------------------

class _State:
    def __init__(self, ob):
        self.key = None
        self.ob_ptr = ob.as_pointer()
        self.me_ptr = ob.data.as_pointer()
        self.name = ob.name
        self.counts = None
        self.co = None
        self.hide = None
        self.sel = None
        self.edges = None
        self.corners = None
        self.geom_version = 0
        self.wkey = None
        self.w = None
        self.bkey = None
        self.batches = None
        self.drawn = None          # vertex indices used by the batches


def _bm(ob):
    import bmesh
    bm = bmesh.from_edit_mesh(ob.data)
    bm.verts.ensure_lookup_table()
    return bm


def _read_coords(bm, mw, idx=None):
    verts = bm.verts if idx is None else [bm.verts[i] for i in idx]
    n = len(verts)
    local = np.fromiter(chain.from_iterable(v.co for v in verts), dtype=np.float64, count=3 * n).reshape(n, 3)
    m = np.array(mw, dtype=np.float64)
    return local @ m[:3, :3].T + m[:3, 3]


def _transform_running():
    win = getattr(bpy.context, "window", None)
    if win is None:
        return False
    try:
        return any(op.bl_idname.startswith("TRANSFORM_OT") for op in win.modal_operators)
    except (AttributeError, ReferenceError):
        return False


def _state(ob):
    st = _cache.get(ob.as_pointer())
    if st is None or st.name != ob.name or st.me_ptr != ob.data.as_pointer():
        st = _cache[ob.as_pointer()] = _State(ob)
    return st


def _refresh(ob, st, ts):
    """Bring the cached arrays up to date with the edit mesh."""
    bm = _bm(ob)
    counts = (len(bm.verts), len(bm.edges), len(bm.faces))
    touched = ob.name in _dirty["any"] or ob.data.name in _dirty["any"] or st.co is None
    geom = ob.name in _dirty["geom"] or ob.data.name in _dirty["geom"]
    if st.co is not None and counts == st.counts and not touched:
        return
    _dirty["any"].discard(ob.name), _dirty["any"].discard(ob.data.name)
    _dirty["geom"].discard(ob.name), _dirty["geom"].discard(ob.data.name)
    mw = ob.matrix_world
    if st.co is None or counts != st.counts:
        bm.verts.index_update()
        st.counts = counts
        st.co = _read_coords(bm, mw)
        st.edges = np.fromiter(chain.from_iterable((e.verts[0].index, e.verts[1].index) for e in bm.edges),
                               dtype=np.int64, count=2 * counts[1]).reshape(-1, 2)
        st.corners = None
        st.geom_version += 1
    elif geom:
        if _transform_running() and st.drawn is not None:
            # Blender keeps the weights of the transform start: move only what is drawn.
            if len(st.drawn):
                st.co[st.drawn] = _read_coords(bm, mw, st.drawn)
                st.bkey = None
            return
        st.co = _read_coords(bm, mw)
        st.geom_version += 1
    n = counts[0]
    st.sel = np.fromiter((v.select for v in bm.verts), dtype=bool, count=n)
    st.hide = np.fromiter((v.hide for v in bm.verts), dtype=bool, count=n)


def _corners(ob, st):
    if st.corners is None:
        bm = _bm(ob)
        st.corners = _soft().face_corners([tuple(v.index for v in f.verts) for f in bm.faces if not f.hide])
    return st.corners


def _view_axis(ts, rv3d):
    if not ts.use_proportional_projected or rv3d is None:
        return None
    z = rv3d.view_matrix.inverted().col[2]
    return (round(z[0], 4), round(z[1], 4), round(z[2], 4))


def object_weights(ob, ts, rv3d=None):
    """Blender's proportional weight of every vertex of the edit mesh ``ob`` (numpy (n,)), cached."""
    st = _state(ob)
    _refresh(ob, st, ts)
    axis = _view_axis(ts, rv3d)
    sel = st.sel & ~st.hide
    key = (hash(sel.tobytes()), hash(st.hide.tobytes()), round(ts.proportional_size, 9), ts.proportional_edit_falloff,
           ts.use_proportional_connected, axis, st.geom_version)
    if key != st.wkey:
        visible = ~st.hide
        corners = _corners(ob, st) if ts.use_proportional_connected else None
        edges = st.edges[visible[st.edges[:, 0]] & visible[st.edges[:, 1]]] if ts.use_proportional_connected else None
        w = _soft().weights(st.co, sel, ts.proportional_size, ts.proportional_edit_falloff, edges, corners,
                            ts.use_proportional_connected, axis)
        w[st.hide] = 0.0
        st.w, st.wkey, st.bkey = w, key, None
        STATS["weights"] += 1
    return st.w


def invalidate_all():
    _cache.clear()


@bpy.app.handlers.persistent
def _on_depsgraph(_scene=None, depsgraph=None):
    if depsgraph is None:
        return
    for u in depsgraph.updates:
        if isinstance(u.id, (bpy.types.Mesh, bpy.types.Object)):
            name = u.id.original.name if u.id.original is not None else u.id.name
            _dirty["any"].add(name)
            if u.is_updated_geometry or u.is_updated_transform:
                _dirty["geom"].add(name)


# --- drawing -------------------------------------------------------------------------------------

def _shader(names):
    for n in names:
        try:
            return gpu.shader.from_builtin(n)
        except (ValueError, TypeError):
            continue
    return None


def _batches(st, vertex_mode):
    key = (st.wkey, vertex_mode, id(st.co))      # st.bkey = None (positions moved) forces a rebuild
    if st.batches is not None and st.bkey == key:
        return st.batches
    w = st.w
    on = w > 0.0
    e = st.edges
    em = (on[e[:, 0]] | on[e[:, 1]]) & ~st.hide[e[:, 0]] & ~st.hide[e[:, 1]]
    e = e[em]
    rgba = np.concatenate([_soft().ramp(w), np.ones((len(w), 1))], axis=1).astype(np.float32)
    co = st.co.astype(np.float32)
    lines = _shader(('POLYLINE_SMOOTH_COLOR',))
    out = []
    if len(e) and lines is not None:
        flat = e.reshape(-1)
        out.append(('LINES', lines, batch_for_shader(lines, 'LINES', {"pos": co[flat], "color": rgba[flat]})))
    pts = np.flatnonzero(on & ~st.hide) if vertex_mode else np.zeros(0, dtype=np.int64)
    if len(pts):
        ps = _shader(('POINT_FLAT_COLOR', 'FLAT_COLOR'))
        if ps is not None:
            out.append(('POINTS', ps, batch_for_shader(ps, 'POINTS', {"pos": co[pts], "color": rgba[pts]})))
    st.drawn = np.unique(np.concatenate([e.reshape(-1), pts]))
    st.batches, st.bkey = out, key
    STATS["batches"] += 1
    return out


def _biased_projection(rv3d):
    from mathutils import Matrix
    p = Matrix(gpu.matrix.get_projection_matrix())
    if rv3d is not None and rv3d.is_perspective:
        p[2][3] -= DEPTH_BIAS * abs(p[2][3])
    else:
        p[2][3] -= DEPTH_BIAS * 0.1
    return p


def _draw_view():
    context = bpy.context
    if context.mode != 'EDIT_MESH' or not _soft().is_on():
        return
    ts = context.scene.tool_settings
    if not ts.use_proportional_edit:
        return
    region, rv3d, space = context.region, context.region_data, context.space_data
    if region is None or rv3d is None or space is None:
        return
    from . import toolbox
    if toolbox.is_toolbox(space):
        return
    vertex_mode = bool(ts.mesh_select_mode[0])
    scale = context.preferences.system.ui_scale or 1.0
    xray = getattr(space.shading, "show_xray", False)
    gpu.state.depth_test_set('NONE' if xray else 'LESS_EQUAL')
    gpu.state.blend_set('ALPHA')
    gpu.matrix.push_projection()
    try:
        gpu.matrix.load_projection_matrix(_biased_projection(rv3d))
        for ob in context.objects_in_mode_unique_data or ():
            if ob is None or ob.type != 'MESH':
                continue
            try:
                w = object_weights(ob, ts, rv3d)
            except (ReferenceError, ValueError):
                continue
            if w is None or not (w > 0).any():
                continue
            st = _cache[ob.as_pointer()]
            for kind, shader, batch in _batches(st, vertex_mode):
                shader.bind()
                if kind == 'LINES':
                    shader.uniform_float("viewportSize", (region.width, region.height))
                    shader.uniform_float("lineWidth", LINE_PX * scale)
                else:
                    gpu.state.point_size_set(POINT_PX * scale)
                batch.draw(shader)
        gpu.state.point_size_set(1.0)
    finally:
        gpu.matrix.pop_projection()
        gpu.state.depth_test_set('NONE')
        gpu.state.blend_set('NONE')


# --- radius drag ---------------------------------------------------------------------------------

def _centre(context):
    """Soft select centre in world space: selected vertices' mean (edit mesh), else the selected objects'."""
    pts = []
    if context.mode == 'EDIT_MESH':
        ts = context.scene.tool_settings
        for ob in context.objects_in_mode_unique_data or ():
            if ob is None or ob.type != 'MESH':
                continue
            st = _state(ob)
            _refresh(ob, st, ts)
            sel = st.sel & ~st.hide
            if sel.any():
                pts.append(st.co[sel])
    else:
        obs = [o for o in (getattr(context, "selected_objects", None) or ())]
        if obs:
            pts.append(np.array([o.matrix_world.translation[:] for o in obs]))
    if not pts:
        return None
    return tuple(np.concatenate(pts).mean(axis=0))


def radius_drag(context, event, text):
    """Start / update (``text`` set) or end (None) the radius display of a B drag."""
    region = getattr(context, "region", None)
    if text is None:
        ptr = _drag.get("region")
        _drag.clear()
        _tag(context)
        return ptr
    if region is None or region.type != 'WINDOW' or getattr(context, "region_data", None) is None:
        _drag.clear()
        return None
    if _drag.get("region") != region.as_pointer():
        _drag.clear()
        _drag["region"] = region.as_pointer()
        _drag["centre"] = _centre(context)
    if event is not None:
        _drag["mouse"] = (event.mouse_region_x, event.mouse_region_y)
    _drag["text"] = text
    _drag["radius"] = context.scene.tool_settings.proportional_size
    _tag(context)
    return _drag["region"]


def _tag(context):
    area = getattr(context, "area", None)
    if area is not None:
        area.tag_redraw()


def circle_px(region, rv3d, centre, radius):
    """(centre_xy, radius_px) of a world sphere of ``radius`` around ``centre`` seen in the region, or None."""
    from bpy_extras.view3d_utils import location_3d_to_region_2d
    from mathutils import Vector
    c = Vector(centre)
    c2 = location_3d_to_region_2d(region, rv3d, c)
    right = rv3d.view_matrix.inverted().col[0].xyz.normalized()
    e2 = location_3d_to_region_2d(region, rv3d, c + right * radius)
    if c2 is None or e2 is None:
        return None
    return (c2.x, c2.y), (e2 - c2).length


def _draw_pixel():
    if not _drag:
        return
    context = bpy.context
    region, rv3d = context.region, context.region_data
    if region is None or rv3d is None or region.as_pointer() != _drag.get("region"):
        return
    import blf
    scale = context.preferences.system.ui_scale or 1.0
    gpu.state.blend_set('ALPHA')
    centre = _drag.get("centre")
    if centre is not None:
        got = circle_px(region, rv3d, centre, _drag["radius"])
        if got is not None:
            (cx, cy), r = got
            n = 96
            pts = []
            for k in range(n):
                for j in (k, k + 1):
                    a = 2 * np.pi * j / n
                    pts.append((cx + r * np.cos(a), cy + r * np.sin(a)))
            sh = gpu.shader.from_builtin('POLYLINE_UNIFORM_COLOR')
            sh.bind()
            sh.uniform_float("viewportSize", (region.width, region.height))
            sh.uniform_float("lineWidth", 1.5 * scale)
            sh.uniform_float("color", CIRCLE_RGBA)
            batch_for_shader(sh, 'LINES', {"pos": pts}).draw(sh)
    mouse = _drag.get("mouse")
    if mouse is not None:
        blf.size(0, 12 * scale)
        blf.color(0, *TEXT_RGBA)
        blf.position(0, mouse[0] + 16 * scale, mouse[1] + 12 * scale, 0)
        blf.draw(0, _drag["text"])
    gpu.state.blend_set('NONE')


# --- register ------------------------------------------------------------------------------------

def register():
    _handles.append(bpy.types.SpaceView3D.draw_handler_add(_draw_view, (), 'WINDOW', 'POST_VIEW'))
    _handles.append(bpy.types.SpaceView3D.draw_handler_add(_draw_pixel, (), 'WINDOW', 'POST_PIXEL'))
    bpy.app.handlers.depsgraph_update_post.append(_on_depsgraph)


def unregister():
    if _on_depsgraph in bpy.app.handlers.depsgraph_update_post:
        bpy.app.handlers.depsgraph_update_post.remove(_on_depsgraph)
    while _handles:
        bpy.types.SpaceView3D.draw_handler_remove(_handles.pop(), 'WINDOW')
    _cache.clear()
    _drag.clear()
    _dirty["geom"].clear()
    _dirty["any"].clear()
