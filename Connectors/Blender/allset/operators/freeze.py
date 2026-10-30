# SPDX-License-Identifier: GPL-3.0-or-later
"""Maya Freeze Transformations, Reset Transformations and Center Pivot.

Design and matrix math: docs/steps/step_10.md 「계획」 0-3. The Maya pivot is the
Blender origin; ``delta_location`` plays the part of Maya's pivot offset
(``rotatePivot``/``rotatePivotTranslate``), so the
channels the Channel Box shows (``location``/``rotation``/``scale``) can read
0/0/1 while the pivot stays where it was.

- Freeze = ``cmds.makeIdentity(apply=True, t=1, r=1, s=1, pn=1)``
  bakes into the data, keeps world positions
  and the pivot, applies to the whole hierarchy below the selection.
- Reset = Modify > Reset Transformations: channels back to
  0/0/1, deltas (the last freeze) kept.
- Center Pivot = ``cmds.xform(centerPivots=True)``: pivot to the
  bounding box centre; vertices, translate and children stay.
"""

import bpy
from bpy.props import BoolProperty
from mathutils import Matrix, Vector

from .. import results

# Object types whose data can absorb a matrix (``ID.transform``).
BAKE_TYPES = {'MESH', 'CURVE', 'SURFACE', 'LATTICE', 'META'}
EPS = 1e-6


def _is_identity(m):
    return all(abs(m[i][j] - (1.0 if i == j else 0.0)) < EPS for i in range(4) for j in range(4))


def _same(a, b):
    return all(abs(a[i][j] - b[i][j]) < EPS for i in range(4) for j in range(4))


def hierarchy(objects):
    """Selected roots (no selected ancestor) and all their descendants, parents first."""
    chosen = set(objects)
    roots = []
    for ob in objects:
        p = ob.parent
        while p is not None and p not in chosen:
            p = p.parent
        if p is None:
            roots.append(ob)
    out, stack = [], list(reversed(roots))
    while stack:
        ob = stack.pop()
        out.append(ob)
        stack.extend(sorted(ob.children, key=lambda o: o.name, reverse=True))
    return out


def parents_first(objects):
    """``objects`` ordered so that a parent comes before its descendants."""
    def depth(ob):
        d, p = 0, ob.parent
        while p is not None:
            d, p = d + 1, p.parent
        return d
    return sorted(objects, key=depth)


def parent_frame(ob, w_old, w_new):
    """Matrix that maps ``ob.matrix_basis`` to world, after any parent change in ``w_new``.

    Taken from the object itself (``W @ basis⁻¹``), so object, bone and vertex
    parents and the parent inverse are all covered.
    """
    frame = w_old[ob] @ ob.matrix_basis.inverted_safe()
    p = ob.parent
    if p is not None and p in w_new:
        frame = w_new[p] @ w_old[p].inverted_safe() @ frame
    return frame


def clear_rotation(ob):
    ob.rotation_euler = (0.0, 0.0, 0.0)
    ob.rotation_quaternion = (1.0, 0.0, 0.0, 0.0)
    ob.rotation_axis_angle = (0.0, 0.0, 1.0, 0.0)
    ob.delta_rotation_euler = (0.0, 0.0, 0.0)
    ob.delta_rotation_quaternion = (1.0, 0.0, 0.0, 0.0)


def set_rotation(ob, quat):
    """Rotation channels = ``quat`` in the object's rotation mode, deltas cleared."""
    ob.delta_rotation_euler = (0.0, 0.0, 0.0)
    ob.delta_rotation_quaternion = (1.0, 0.0, 0.0, 0.0)
    if ob.rotation_mode == 'QUATERNION':
        ob.rotation_quaternion = quat
    elif ob.rotation_mode == 'AXIS_ANGLE':
        axis, angle = quat.to_axis_angle()
        ob.rotation_axis_angle = (angle, *axis)
    else:
        ob.rotation_euler = quat.to_euler(ob.rotation_mode, ob.rotation_euler)


def bake_into_data(ob, matrix):
    """``ob.data.transform(matrix)`` (single user first); keeps normals pointing out."""
    copied = False
    if ob.data.users > 1:
        ob.data = ob.data.copy()
        copied = True
    if ob.type == 'META':
        ob.data.transform(matrix)
    else:
        ob.data.transform(matrix, shape_keys=True)
    if ob.type == 'MESH' and matrix.to_3x3().determinant() < 0:
        ob.data.flip_normals()  # Maya preserve normals (-pn 1)
    return copied


def freeze(objects, translate=True, rotate=True, scale=True):
    """Freeze ``objects`` and their descendants. Returns (frozen count, data copies)."""
    obs = hierarchy(objects)
    w_old = {ob: ob.matrix_world.copy() for ob in obs}
    w_new = {}
    copies = 0
    for ob in obs:
        frame = parent_frame(ob, w_old, w_new)
        moved = ob.parent in w_new and not _same(w_new[ob.parent], w_old[ob.parent])
        b_eff = frame.inverted_safe() @ w_old[ob] if moved else ob.matrix_basis.copy()
        bake = ob.type in BAKE_TYPES and ob.data is not None
        t, q, s = b_eff.decompose()

        if translate:
            ob.location = (0.0, 0.0, 0.0)
            ob.delta_location = t           # the pivot stays (Maya rotatePivot)
        else:
            if moved:   # Maya carries the pivot offset into the parent's new frame like the translate
                carry = (frame.inverted_safe() @ w_old[ob] @ ob.matrix_basis.inverted_safe()).to_3x3()
                ob.delta_location = carry @ Vector(ob.delta_location)
            ob.location = t - Vector(ob.delta_location)
        if rotate and bake:
            clear_rotation(ob)
        elif moved:
            set_rotation(ob, q)
        if scale and bake:
            ob.scale = ob.delta_scale = (1.0, 1.0, 1.0)
        elif moved:
            ob.scale = s
            ob.delta_scale = (1.0, 1.0, 1.0)

        b_new = ob.matrix_basis.copy()
        d = b_new.inverted_safe() @ b_eff
        if bake and not _is_identity(d):
            copies += bake_into_data(ob, d)
        w_new[ob] = frame @ b_new
    return len(obs), copies


def reset(objects, translate=True, rotate=True, scale=True):
    for ob in objects:
        if translate:
            ob.location = (0.0, 0.0, 0.0)
        if rotate:
            ob.rotation_euler = (0.0, 0.0, 0.0)
            ob.rotation_quaternion = (1.0, 0.0, 0.0, 0.0)
            ob.rotation_axis_angle = (0.0, 0.0, 1.0, 0.0)
        if scale:
            ob.scale = (1.0, 1.0, 1.0)
    return len(objects)


def _bbox_center(corners):
    lo = Vector(tuple(min(c[i] for c in corners) for i in range(3)))
    hi = Vector(tuple(max(c[i] for c in corners) for i in range(3)))
    return (lo + hi) / 2


def local_center(ob):
    """Bounding-box centre in ``ob``'s local space, or None (nothing to centre on)."""
    if ob.type in BAKE_TYPES and ob.data is not None:
        return _bbox_center([Vector(v) for v in ob.bound_box])
    corners = []
    stack = list(ob.children)
    while stack:
        ch = stack.pop()
        stack.extend(ch.children)
        if ch.type in BAKE_TYPES and ch.data is not None:
            corners.extend(ch.matrix_world @ Vector(v) for v in ch.bound_box)
    if not corners:
        return None
    return ob.matrix_world.inverted_safe() @ _bbox_center(corners)


def shift_pivot(ob, c):
    """Move ``ob``'s origin by the local offset ``c``: data, translate and children stay."""
    w_old = {ob: ob.matrix_world.copy()}
    for ch in ob.children:
        w_old[ch] = ch.matrix_world.copy()
    lin = ob.matrix_basis.to_3x3()
    ob.delta_location = Vector(ob.delta_location) + lin @ c
    if ob.type in BAKE_TYPES and ob.data is not None:
        bake_into_data(ob, Matrix.Translation(-c))
    w_new = {ob: w_old[ob] @ Matrix.Translation(c)}
    for ch in ob.children:
        frame = parent_frame(ch, w_old, w_new)
        b_new = frame.inverted_safe() @ w_old[ch]
        diff = b_new.translation - ch.matrix_basis.translation
        ch.delta_location = Vector(ch.delta_location) + diff


def center_pivot(context, objects):
    done = 0
    for ob in parents_first(objects):
        context.view_layer.update()  # bound_box / matrix_world after the previous object
        c = local_center(ob)
        if c is None or c.length < EPS:
            continue
        shift_pivot(ob, c)
        done += 1
    return done


class _SelectedObjectsOp:
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return context.mode == 'OBJECT'

    def objects(self, context):
        obs = list(context.selected_objects)
        if not obs:
            self.report({'WARNING'}, "Nothing selected")
        return obs


class ALLSET_OT_freeze_transform(_SelectedObjectsOp, bpy.types.Operator):
    """Bake translate/rotate/scale into the geometry: the channels read 0/0/1 and the
    pivot stays in place (Maya Modify > Freeze Transformations)"""
    bl_idname = "allset.freeze_transform"
    bl_label = "Freeze Transformations"

    translate: BoolProperty(name="Translate", default=True)
    rotate: BoolProperty(name="Rotate", default=True)
    scale: BoolProperty(name="Scale", default=True)

    def execute(self, context):
        obs = self.objects(context)
        if not obs:
            return {'CANCELLED'}
        count, copies = freeze(obs, self.translate, self.rotate, self.scale)
        msg = f"Froze {count} object(s)"
        if copies:
            msg += f"; {copies} shared data block(s) made single user"
        results.result(self, msg)
        return {'FINISHED'}


class ALLSET_OT_reset_transform(_SelectedObjectsOp, bpy.types.Operator):
    """Set translate/rotate/scale back to 0/0/1, i.e. to the last frozen state
    (Maya Modify > Reset Transformations)"""
    bl_idname = "allset.reset_transform"
    bl_label = "Reset Transformations"

    translate: BoolProperty(name="Translate", default=True)
    rotate: BoolProperty(name="Rotate", default=True)
    scale: BoolProperty(name="Scale", default=True)

    def execute(self, context):
        obs = self.objects(context)
        if not obs:
            return {'CANCELLED'}
        reset(obs, self.translate, self.rotate, self.scale)
        return {'FINISHED'}


class ALLSET_OT_center_pivot(_SelectedObjectsOp, bpy.types.Operator):
    """Move the pivot (origin) to the centre of the bounding box without moving the
    geometry or changing translate (Maya Modify > Center Pivot)"""
    bl_idname = "allset.center_pivot"
    bl_label = "Center Pivot"

    def execute(self, context):
        obs = self.objects(context)
        if not obs:
            return {'CANCELLED'}
        center_pivot(context, obs)
        return {'FINISHED'}


classes = (ALLSET_OT_freeze_transform, ALLSET_OT_reset_transform, ALLSET_OT_center_pivot)
