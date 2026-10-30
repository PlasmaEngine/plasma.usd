# SPDX-License-Identifier: GPL-3.0-or-later
"""Maya view furniture in the 3D View: ViewCube (top right), axis triad (bottom left), camera name label
(bottom centre), in place of Blender's navigation gizmo and text info (docs/steps/step_46.md).

Maya 2022 factory views (step 46): ViewCube 65 px, 26 px from the view's right and 27 px from its top, white faces at
opacity 0.5 (FRONT/RIGHT/TOP readable in the default persp), only in the active panel of a four view;
label "persp" / "top -Y" ... 11 px #e3e3e3, baseline 9 px above the bottom, centred 9 px left of the
middle; axis triad origin 26 px from the left and bottom, 13 px lines, lower-case letters.

A view gets these when Blender's navigation gizmo is off in it (``show_gizmo_navigate``; the Maya Workflow Kit
template turns it and ``overlay.show_text`` off): the Maya furniture replaces Blender's. What is shown is
global like Maya's HUD / ViewCube preferences (add-on preferences ``maya_viewcube``,
``maya_camera_names``, ``maya_view_axis``).

The triad shows Blender's axes (Z up): Maya's Y-up cannot be had (docs/user_decisions.md 5) and the
letters have to match the Channel Box channels. The camera labels are Maya's camera names with Maya's
Y-up view direction ("top -Y", "front -Z"): they name Maya's cameras, not axes (step 176).
"""

import math

import bpy
from mathutils import Vector

# --- Maya sizes and colours (step 46) -----------------------------------------------------
CUBE_PX = 65.0                 # ViewCube box in a 612 px tall Maya view
CUBE_INSET = (26.0, 27.0)      # from the right, from the top
MAYA_VIEW_H = 612.0
SMALL_EXP = 0.42               # 65 px in the 612 px view, 48 px in a four-view panel
LABEL_BASELINE = 9.0
LABEL_SHIFT = -9.0
LABEL_PX = 11.0
LABEL_RGB = (0.89, 0.89, 0.89)
TRIAD_INSET = 26.0
TRIAD_LEN = 13.0
CUBE_ALPHA = (0.5, 1.0)        # resting, under the mouse
MAYA_PERSP_DIR = Vector((-28.0, 28.0, -21.0)).normalized()   # Maya persp looking direction, Blender axes

# face -> (normal, text u axis, text v axis), Blender axes; names = view3d.view_axis types
FACES = {
    "TOP": ((0, 0, 1), (1, 0, 0), (0, 1, 0)),
    "BOTTOM": ((0, 0, -1), (1, 0, 0), (0, -1, 0)),
    "FRONT": ((0, -1, 0), (1, 0, 0), (0, 0, 1)),
    "BACK": ((0, 1, 0), (-1, 0, 0), (0, 0, 1)),
    "RIGHT": ((1, 0, 0), (0, 1, 0), (0, 0, 1)),
    "LEFT": ((-1, 0, 0), (0, -1, 0), (0, 0, 1)),
}
FACE_NAMES = tuple(FACES)

# Orthographic side views (key = the direction the view looks, Blender axes): Maya camera name + that direction
# in Maya's Y-up axes, as Maya prints it ("top -Y", "front -Z", "side -X";
# Blender X/Y/Z = Maya X/-Z/Y, CLAUDE.md step 57). Step 176 (delegated): Maya's own words, not Blender's axes.
SIDE_LABELS = {(0, 0, -1): "top -Y", (0, 0, 1): "bottom +Y", (0, 1, 0): "front -Z", (0, -1, 0): "back +Z",
               (-1, 0, 0): "side -X", (1, 0, 0): "left +X"}


# --- maths (mathutils only, no bpy) ---------------------------------------------------------

def _to_view(q, p):
    """World direction/point -> view space of a view whose rotation is ``q`` (view_rotation)."""
    return q.inverted() @ Vector(p)


def _extent(q):
    pts = [_to_view(q, (x, y, z)) for x in (-.5, .5) for y in (-.5, .5) for z in (-.5, .5)]
    return max(max(p.x for p in pts) - min(p.x for p in pts), max(p.y for p in pts) - min(p.y for p in pts))


def _maya_extent():
    return _extent(MAYA_PERSP_DIR.to_track_quat('-Z', 'Y'))


_PX_PER_UNIT = 1.0 / _maya_extent()   # cube scale that makes its box CUBE_PX tall in Maya's persp


def cube_size(width, height, scale=1.0):
    """ViewCube box size (px) for a view region: Maya's 65 px, smaller in small panels."""
    m = min(width, height) / (MAYA_VIEW_H * scale)
    return CUBE_PX * scale * min(1.0, m ** SMALL_EXP)


def faces_2d(q, centre, size):
    """Visible cube faces on screen, back to front: [(name, polygon, facing, u2d, v2d)]."""
    k = size * _PX_PER_UNIT
    cx, cy = centre
    out = []
    for name, (n, u, v) in FACES.items():
        nv = _to_view(q, n)
        if nv.z <= 1e-4:
            continue
        nvec, uvec, vvec = Vector(n), Vector(u), Vector(v)
        poly = []
        for su, sv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            p = _to_view(q, nvec * 0.5 + uvec * 0.5 * su + vvec * 0.5 * sv)
            poly.append((cx + p.x * k, cy + p.y * k))
        u2, v2 = _to_view(q, u), _to_view(q, v)
        out.append((name, poly, nv.z, (u2.x, u2.y), (v2.x, v2.y)))
    out.sort(key=lambda f: f[2])
    return out


def point_in_poly(x, y, poly):
    inside = False
    j = len(poly) - 1
    for i in range(len(poly)):
        xi, yi = poly[i]
        xj, yj = poly[j]
        if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
            inside = not inside
        j = i
    return inside


def hit_face(faces, x, y):
    """Front-most face under (x, y), or None."""
    for name, poly, *_rest in reversed(faces):
        if point_in_poly(x, y, poly):
            return name
    return None


def label_for(perspective, side_view, view_dir, camera_name=None):
    """Maya camera label of a view: camera name, "top -Y" ... for side views, else "persp"."""
    if perspective == 'CAMERA' and camera_name:
        return camera_name
    if perspective == 'ORTHO' and side_view:
        key = tuple(int(round(c)) for c in view_dir)
        if key in SIDE_LABELS:
            return SIDE_LABELS[key]
    return "persp"


def layout(bounds, scale, size):
    """Region-local positions in a visible rect (x0, y0, x1, y1): cube centre, label (x centre,
    baseline y), triad origin."""
    x0, y0, x1, y1 = bounds
    cube = (x1 - CUBE_INSET[0] * scale - size / 2, y1 - CUBE_INSET[1] * scale - size / 2)
    label = ((x0 + x1) / 2 + LABEL_SHIFT * scale, y0 + LABEL_BASELINE * scale)
    triad = (x0 + TRIAD_INSET * scale, y0 + TRIAD_INSET * scale)
    return cube, label, triad


def triad(q, origin, length):
    """Axis lines on screen, back to front: [(axis index, end (x, y), depth)]."""
    out = []
    for i in range(3):
        d = [0.0, 0.0, 0.0]
        d[i] = 1.0
        p = _to_view(q, d)
        out.append((i, (origin[0] + p.x * length, origin[1] + p.y * length), p.z))
    out.sort(key=lambda a: a[2])
    return out


# --- per-view state -------------------------------------------------------------------------

def _prefs():
    from .. import preferences
    try:
        return preferences.get()
    except KeyError:
        return None


def maya_view(space):
    return space is not None and space.type == 'VIEW_3D' and not space.show_gizmo_navigate \
        and space.overlay.show_overlays


def _bounds(context):
    """Visible part of the region (sidebar / toolbar excluded), region-local."""
    from ..operators import view
    return view.visible_bounds(context.area, context.region)   # already region-local


def _scale():
    return bpy.context.preferences.system.ui_scale or 1.0


# Four view: the ViewCube shows in one panel only (Maya: the active panel), the last one the mouse was in.
_ACTIVE = {"region": None}


def _cube_here(context):
    space, region = context.space_data, context.region
    if not space.region_quadviews:
        return True
    if _ACTIVE["region"] is None:
        return region.data == space.region_3d
    return region.as_pointer() == _ACTIVE["region"]


# --- drawing --------------------------------------------------------------------------------

_shader = []


def _sh():
    if not _shader:
        import gpu
        _shader.append(gpu.shader.from_builtin('UNIFORM_COLOR'))
        _shader.append(gpu.shader.from_builtin('POLYLINE_UNIFORM_COLOR'))
    return _shader


def _fill(poly, colour):
    from gpu_extras.batch import batch_for_shader
    sh = _sh()[0]
    batch = batch_for_shader(sh, 'TRI_FAN', {"pos": poly})
    sh.uniform_float("color", colour)
    batch.draw(sh)


def _lines(points, colour, width, region, loop=False):
    from gpu_extras.batch import batch_for_shader
    sh = _sh()[1]
    pts = list(points) + ([points[0]] if loop else [])
    batch = batch_for_shader(sh, 'LINE_STRIP', {"pos": [(x, y, 0.0) for x, y in pts]})
    sh.bind()
    sh.uniform_float("viewportSize", (region.width, region.height))
    sh.uniform_float("lineWidth", width)
    sh.uniform_float("color", colour)
    batch.draw(sh)


def _text(s, x, y, px, colour, angle=0.0, centre=True):
    import blf
    fid = 0
    blf.size(fid, px)
    w, h = blf.dimensions(fid, s)
    if angle:
        blf.enable(fid, blf.ROTATION)
        blf.rotation(fid, angle)
    if centre:
        c, sn = math.cos(angle), math.sin(angle)
        x, y = x - (c * w / 2 - sn * h / 2), y - (sn * w / 2 + c * h / 2)
    blf.color(fid, *colour)
    blf.position(fid, x, y, 0)
    blf.draw(fid, s)
    if angle:
        blf.disable(fid, blf.ROTATION)


def draw_cube(context, hover=None, alpha=0.5):
    region, rv3d = context.region, context.region_data
    s = _scale()
    size = cube_size(region.width, region.height, s)
    centre, _label, _triad = layout(_bounds(context), s, size)
    faces = faces_2d(rv3d.view_rotation, centre, size)
    for name, poly, facing, u2, _v2 in faces:
        shade = 0.82 + 0.18 * facing
        rgb = (0.55, 0.75, 0.95) if name == hover else (shade, shade, shade)
        import gpu
        gpu.state.blend_set('ALPHA')         # blf.draw of the previous face's label turns blending off
        _fill(poly, (*rgb, alpha))
        _lines(poly, (0.44, 0.44, 0.44, alpha), 1.5, region, loop=True)
        if facing > 0.35:
            cx = sum(p[0] for p in poly) / 4
            cy = sum(p[1] for p in poly) / 4
            ang = math.atan2(u2[1], u2[0])
            if math.cos(ang) < 0:            # never upside down
                ang += math.pi
            _text(name, cx, cy, max(6, round(size * 0.14 * facing ** 0.5)), (0.3, 0.3, 0.3, alpha), ang)
    return faces


def _draw_hud():
    context = bpy.context
    space, region, rv3d = context.space_data, context.region, context.region_data
    if rv3d is None or not maya_view(space):
        return
    prefs = _prefs()
    if prefs is None:
        return
    _check_strip(context)
    import gpu
    s = _scale()
    _cube, label, origin = layout(_bounds(context), s, 0.0)
    gpu.state.blend_set('ALPHA')
    if prefs.maya_camera_names:
        cam = context.scene.camera
        d = rv3d.view_rotation @ Vector((0.0, 0.0, -1.0))
        text = label_for(rv3d.view_perspective, rv3d.is_orthographic_side_view, tuple(d),
                         cam.name if cam else None)
        import blf
        blf.size(0, round(LABEL_PX * s))
        w = blf.dimensions(0, text)[0]
        _text(text, label[0] - w / 2, label[1], round(LABEL_PX * s), (*LABEL_RGB, 1.0), centre=False)
    if prefs.maya_view_axis:
        ui = context.preferences.themes[0].user_interface
        colours = (ui.axis_x, ui.axis_y, ui.axis_z)
        for i, end, _depth in triad(rv3d.view_rotation, origin, TRIAD_LEN * s):
            rgb = tuple(colours[i])[:3]
            gpu.state.blend_set('ALPHA')
            _lines((origin, end), (*rgb, 1.0), 1.0, region)
            dx, dy = end[0] - origin[0], end[1] - origin[1]
            n = math.hypot(dx, dy) or 1.0
            _text("xyz"[i], end[0] + dx / n * 5 * s, end[1] + dy / n * 5 * s, round(10 * s), (*rgb, 1.0))
    gpu.state.blend_set('NONE')


def _hud_callback():
    try:
        _draw_hud()
    except Exception:  # never let a draw error repeat every frame
        import traceback
        traceback.print_exc()
        _remove_handler()


# --- asset shelf strip (step 124) ----------------------------------------------------------
# Pose / paint modes give the view an ASSET_SHELF_HEADER region (29 px at the bottom, measured) that never draws
# while the shelf is collapsed: it expects Region Overlap, which Maya mode turns off (step 112). Without overlap the
# WINDOW region shrinks above it and the strip keeps the pixels drawn there before the mode change — the camera
# label and triad showed twice ("persp" under "persp"). A draw handler on that region is never called (measured),
# so the strip is hidden instead (screen.region_toggle; Maya has no asset shelf) and shown again when the shelf is
# opened or the kit unregisters. The HUD draw sees the state; the toggle runs from a timer.

def strip_action(area, overlap):
    """'HIDE' / 'SHOW' for the asset shelf header of ``area``, or None."""
    shelf = next((r for r in area.regions if r.type == 'ASSET_SHELF'), None)
    header = next((r for r in area.regions if r.type == 'ASSET_SHELF_HEADER'), None)
    if shelf is None or header is None:
        return None
    if header.height > 1 and shelf.height <= 1 and not overlap:
        return 'HIDE'
    if header.height <= 1 and area.as_pointer() in _HIDDEN and (shelf.height > 1 or overlap):
        return 'SHOW'
    return None


_HIDDEN = set()       # area pointers whose shelf header the kit hid (this session)
_PENDING = {}        # area pointer -> its timer function


def _toggle_strip(window, area):
    region = next((r for r in area.regions if r.type == 'WINDOW'), None)
    if region is None:
        return False
    with bpy.context.temp_override(window=window, screen=window.screen, area=area, region=region):
        return bpy.ops.screen.region_toggle(region_type='ASSET_SHELF_HEADER') == {'FINISHED'}


def _strip_later(area_ptr):
    def tick():
        _PENDING.pop(area_ptr, None)
        overlap = bpy.context.preferences.system.use_region_overlap
        for window in bpy.context.window_manager.windows:
            for area in window.screen.areas:
                if area.as_pointer() != area_ptr:
                    continue
                act = strip_action(area, overlap)
                if act and _toggle_strip(window, area):
                    (_HIDDEN.add if act == 'HIDE' else _HIDDEN.discard)(area_ptr)
                    area.tag_redraw()
        return None
    if area_ptr not in _PENDING:
        _PENDING[area_ptr] = tick
        bpy.app.timers.register(tick, first_interval=0.0)


def _check_strip(context):
    area = context.area
    if area is not None and strip_action(area, context.preferences.system.use_region_overlap):
        _strip_later(area.as_pointer())


def _show_strips():
    """Unregister: give back every shelf header the kit hid."""
    wm = getattr(bpy.context, "window_manager", None)
    for window in (wm.windows if wm else ()):
        for area in window.screen.areas:
            if area.as_pointer() in _HIDDEN:
                header = next((r for r in area.regions if r.type == 'ASSET_SHELF_HEADER'), None)
                if header is not None and header.height <= 1:
                    try:
                        _toggle_strip(window, area)
                    except RuntimeError:
                        pass
    _HIDDEN.clear()
    for tick in _PENDING.values():
        if bpy.app.timers.is_registered(tick):
            bpy.app.timers.unregister(tick)
    _PENDING.clear()


_handle = []


def _remove_handler():
    while _handle:
        bpy.types.SpaceView3D.draw_handler_remove(_handle.pop(), 'WINDOW')


# --- ViewCube gizmo -------------------------------------------------------------------------

class ALLSET_GT_view_cube(bpy.types.Gizmo):
    bl_idname = "ALLSET_GT_view_cube"

    def setup(self):
        self.hover = None
        # one view3d.view_axis call per face: test_select returns the face's index as the part
        for i, name in enumerate(FACE_NAMES):
            op = self.target_set_operator("view3d.view_axis", index=i)
            op.type = name

    def draw(self, context):
        if not _cube_here(context):
            return
        import gpu
        from mathutils import Matrix
        region = context.region
        w, h = region.width, region.height
        ortho = Matrix(((2 / w, 0, 0, -1), (0, 2 / h, 0, -1), (0, 0, -1, 0), (0, 0, 0, 1)))
        with gpu.matrix.push_pop(), gpu.matrix.push_pop_projection():
            gpu.matrix.load_identity()
            gpu.matrix.load_projection_matrix(ortho)
            gpu.state.blend_set('ALPHA')
            gpu.state.depth_test_set('NONE')
            draw_cube(context, self.hover if self.is_highlight else None,
                      CUBE_ALPHA[1] if self.is_highlight else CUBE_ALPHA[0])
            gpu.state.blend_set('NONE')

    def test_select(self, context, location):
        space, region = context.space_data, context.region
        if space.region_quadviews and _ACTIVE["region"] != region.as_pointer():
            _ACTIVE["region"] = region.as_pointer()
            context.area.tag_redraw()
        s = _scale()
        size = cube_size(region.width, region.height, s)
        centre, _l, _t = layout(_bounds(context), s, size)
        name = hit_face(faces_2d(context.region_data.view_rotation, centre, size), *location)
        if name != self.hover:
            self.hover = name
            context.area.tag_redraw()
        return FACE_NAMES.index(name) if name else -1


class ALLSET_GGT_view_cube(bpy.types.GizmoGroup):
    bl_idname = "ALLSET_GGT_view_cube"
    bl_label = "Maya ViewCube"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'WINDOW'
    bl_options = {'PERSISTENT', 'SCALE'}

    @classmethod
    def poll(cls, context):
        prefs = _prefs()
        space = context.space_data
        return prefs is not None and prefs.maya_viewcube and maya_view(space) and space.show_gizmo \
            and context.region_data is not None

    def setup(self, context):
        self.gizmos.new(ALLSET_GT_view_cube.bl_idname)


classes = (ALLSET_GT_view_cube, ALLSET_GGT_view_cube)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)
    _handle.append(bpy.types.SpaceView3D.draw_handler_add(_hud_callback, (), 'WINDOW', 'POST_PIXEL'))


def unregister():
    _remove_handler()
    _show_strips()
    _ACTIVE["region"] = None
    _shader.clear()
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
