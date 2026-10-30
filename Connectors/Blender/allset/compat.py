# SPDX-License-Identifier: GPL-3.0-or-later
"""Blender version differences in one place (step 118, docs/compat_5x.md).

The kit's main target is Blender 5.2 (step 272); it supports 4.5.0 - 5.2.x. Every check of "which Blender is
this" lives here; feature code calls these helpers and stays the same on every version. Each helper does the
5.x thing first and falls back to the 4.5 path; the feature checks themselves are unchanged, so every version
gets the same result as before.
"""

import bpy

VERSION = tuple(bpy.app.version[:2])
TARGET = (5, 2)                         # main target since step 272 (Blender 5.2 LTS); 4.5 stays supported
STRICT_VERSIONS = ((4, 5), (5, 2))      # keymap patches must all apply to these minors' IC data
IS_TARGET = VERSION == TARGET
IS_STRICT = VERSION in STRICT_VERSIONS


# Properties tab flags newer than 4.5 (measured, step 118): a 4.5-saved file (the Maya Workflow Kit template) opens in 5.x with
# them on, so they carry no meaning in a kit dock's role (ui/maya_dock.role).
LATER_PROPERTIES_TABS = frozenset({"show_properties_strip", "show_properties_strip_modifier"})


def own_annotations(cls):
    """The annotations ``cls`` itself declares (not inherited), on every Python Blender runs.

    Python 3.14 (PEP 649 / 749, used by Linux distribution builds of Blender such as CachyOS, Issue #176) evaluates
    class annotations lazily: a class body no longer has a ``__annotations__`` name to read or update, and
    ``cls.__dict__`` holds ``__annotate__`` instead of ``__annotations__``. ``annotationlib`` (3.14+) evaluates them;
    older Pythons keep the dict in the class namespace."""
    try:
        import annotationlib
    except ImportError:
        return dict(cls.__dict__.get("__annotations__", {}))
    return dict(annotationlib.get_annotations(cls))


def has_op(module, name):
    """Does this Blender have the operator? (``bpy.ops.<mod>.<name>`` returns a wrapper for any name.)"""
    try:
        getattr(module, name).get_rna_type()
        return True
    except (AttributeError, KeyError):
        return False


def strips(sequence_editor):
    """The sequencer's top-level strips: ``strips`` (4.4+); 5.0 removed ``sequences``."""
    if hasattr(sequence_editor, "strips"):
        return sequence_editor.strips
    return sequence_editor.sequences


def fcurve_owners(action):
    """Collections holding the action's F-curves: every channelbag of a layered action (4.4+); 5.0 removed
    the legacy ``Action.fcurves`` / ``Action.groups``, which 4.5 still has as a proxy of the first slot."""
    owners = []
    for layer in getattr(action, "layers", ()):
        for strip in layer.strips:
            owners.extend(getattr(strip, "channelbags", ()))
    if not owners and hasattr(action, "fcurves"):
        owners.append(action)
    return owners


def action_fcurves(action, slot=None):
    """F-curves of the action for one slot (``slot`` None = every slot), a list."""
    if action is None:
        return []
    if slot is not None:
        bag = channelbag(action, slot)
        return list(bag.fcurves) if bag is not None else []
    return [fc for owner in fcurve_owners(action) for fc in owner.fcurves]


def channelbag(action, slot):
    """The channelbag of ``slot`` (None when the slot has no keys yet)."""
    try:
        from bpy_extras.anim_utils import action_get_channelbag_for_slot
    except ImportError:
        return None
    return action_get_channelbag_for_slot(action, slot)


def id_fcurves(anim_id):
    """F-curves the ID's animation_data plays (its action's assigned slot), a list."""
    ad = getattr(anim_id, "animation_data", None)
    if ad is None or ad.action is None:
        return []
    slot = getattr(ad, "action_slot", None)
    if slot is not None:
        return action_fcurves(ad.action, slot)
    return list(getattr(ad.action, "fcurves", ()))


def id_fcurve_collection(anim_id):
    """The collection (with ``find`` / ``new`` / ``remove``) of the ID's slot F-curves, or None."""
    ad = getattr(anim_id, "animation_data", None)
    if ad is None or ad.action is None:
        return None
    slot = getattr(ad, "action_slot", None)
    if slot is not None:
        bag = channelbag(ad.action, slot)
        return bag.fcurves if bag is not None else None
    return getattr(ad.action, "fcurves", None)


# --- Timeline ------------------------------------------------------------------------------
# Blender 5.x: the Timeline is a Dope Sheet mode and bl_ui.space_time has no TIME_HT_editor_buttons /
# TIME_MT_editor_menus; the timeline header is DOPESHEET_HT_editor_buttons.draw_header and
# DOPESHEET_MT_editor_menus with ``mode == 'TIMELINE'`` ($S52\startup\bl_ui\space_dopesheet.py:198-230, :395-412).

def _legacy_timeline():
    from bl_ui import space_time
    return hasattr(space_time, "TIME_HT_editor_buttons")


LEGACY_TIMELINE = _legacy_timeline()


def timeline_header_classes():
    """(class whose ``draw_header`` draws the timeline buttons, class whose ``draw_collapsible`` draws
    the timeline menus) of this Blender."""
    if not LEGACY_TIMELINE:
        from bl_ui import space_dopesheet
        return space_dopesheet.DOPESHEET_HT_editor_buttons, space_dopesheet.DOPESHEET_MT_editor_menus
    from bl_ui import space_time
    return space_time.TIME_HT_editor_buttons, space_time.TIME_MT_editor_menus


# --- UV select mode -------------------------------------------------------------------------
# 4.5: uv_select_mode has 'ISLAND'; 5.x: uv_select_mode is VERTEX / EDGE / FACE and island selection is the
# separate ``use_uv_select_island`` (measured, step 118).

def uv_island_mode(ts):
    if hasattr(ts, "use_uv_select_island"):
        return ts.use_uv_select_island
    return ts.uv_select_mode == 'ISLAND'


def set_uv_island_mode(ts, island):
    """Island (Maya shell) selection on, or plain UV (vertex) selection."""
    if hasattr(ts, "use_uv_select_island"):
        if not island:
            ts.uv_select_mode = 'VERTEX'
        ts.use_uv_select_island = island
    else:
        ts.uv_select_mode = 'ISLAND' if island else 'VERTEX'


# --- Render output format -------------------------------------------------------------------
# 5.x: ImageFormatSettings.media_type (IMAGE / MULTI_LAYER_IMAGE / VIDEO) picks which file formats exist;
# 'FFMPEG' needs media_type 'VIDEO' first (measured, step 118). 4.5 has no media_type.
MEDIA_TYPE = "media_type" in bpy.types.ImageFormatSettings.bl_rna.properties


def set_file_format(image_settings, fmt):
    if MEDIA_TYPE:
        image_settings.media_type = ('VIDEO' if fmt == 'FFMPEG' else
                                     'MULTI_LAYER_IMAGE' if fmt == 'OPEN_EXR_MULTILAYER' else 'IMAGE')
    image_settings.file_format = fmt


# --- bmesh ----------------------------------------------------------------------------------
# 5.2 renamed pointmerge_facedata's ``vert_snap`` to ``vert_target`` (bmesh.ops docstring, measured, step 118).

def pointmerge_facedata(bm, verts, target):
    import bmesh
    key = "vert_target" if "vert_target" in (bmesh.ops.pointmerge_facedata.__doc__ or "") else "vert_snap"
    return bmesh.ops.pointmerge_facedata(bm, verts=verts, **{key: target})


# --- UV selection in bmesh ------------------------------------------------------------------
# 4.5: per UV layer, ``loop[uv].select`` / ``select_edge``; 5.x: one UV selection per loop,
# ``loop.uv_select_vert`` / ``uv_select_edge`` (BMLoopUV has only uv / pin_uv, measured, step 118).
def _loop_uv_select():
    import bmesh
    return hasattr(bmesh.types.BMLoop, "uv_select_vert")


LOOP_UV_SELECT = _loop_uv_select()


def uv_vert_selected(loop, uv):
    return loop.uv_select_vert if LOOP_UV_SELECT else loop[uv].select


def uv_edge_selected(loop, uv):
    return loop.uv_select_edge if LOOP_UV_SELECT else loop[uv].select_edge


def set_uv_vert_selected(loop, uv, value):
    if LOOP_UV_SELECT:
        loop.uv_select_vert = value
    else:
        loop[uv].select = value


def set_uv_edge_selected(loop, uv, value):
    if LOOP_UV_SELECT:
        loop.uv_select_edge = value
    else:
        loop[uv].select_edge = value


def set_uv_face_selected(face, uv, value):
    """A face's UVs picked / unpicked with UV Sync Selection off: its loops, and 5.x's own face flag
    (``BMFace.uv_select``, which a UV select-mode change flushes back to the loops — step 155)."""
    for loop in face.loops:
        set_uv_vert_selected(loop, uv, value)
        set_uv_edge_selected(loop, uv, value)
    if hasattr(face, "uv_select"):
        face.uv_select = value


# --- Bone visibility ------------------------------------------------------------------------
# 4.5: a bone is hidden in pose / object mode by ``Bone.hide``; 5.x moved that to ``PoseBone.hide`` and
# ``Bone.hide`` only hides the edit bone (step 118, api notes).
POSE_BONE_HIDE = "hide" in bpy.types.PoseBone.bl_rna.properties


def bone_hidden(pb):
    return pb.hide if POSE_BONE_HIDE else pb.bone.hide


def set_bone_hidden(pb, hidden):
    if POSE_BONE_HIDE:
        pb.hide = hidden
    else:
        pb.bone.hide = hidden


def timeline_marker_menu():
    """The Timeline's Marker menu: DOPESHEET_MT_marker (5.x, $S52 space_dopesheet.py:573), TIME_MT_marker (4.5)."""
    return "DOPESHEET_MT_marker" if not LEGACY_TIMELINE else "TIME_MT_marker"


def save_dialog_args(op):
    """Extra arguments Blender's own Ctrl+S / Ctrl+Shift+S items pass: 5.2 adds
    ``show_save_modified_images_dialog`` (IC keymap data, step 118); none in 4.5."""
    props = op.get_rna_type().properties
    return {"show_save_modified_images_dialog": True} if "show_save_modified_images_dialog" in props else {}


def read_region_row(fb, region, y):
    """One framebuffer row of the region (``read_color``), or None when ``y`` is outside it: 5.1+ raises on
    out-of-bounds reads where 4.5 returned garbage (step 118, api notes)."""
    y = int(y)
    if not 0 <= y < region.height or region.width <= 0:
        return None
    try:
        return fb.read_color(0, y, region.width, 1, 4, 0, 'UBYTE')
    except ValueError:
        return None


# --- Materials ------------------------------------------------------------------------------
# 5.0: every material uses nodes; ``Material.use_nodes`` is deprecated (prints a warning when set).
MATERIAL_ALWAYS_NODES = VERSION >= (5, 0)


def ensure_material_nodes(mat):
    if not MATERIAL_ALWAYS_NODES:
        mat.use_nodes = True


def material_uses_nodes(mat):
    return mat is not None and mat.node_tree is not None and (MATERIAL_ALWAYS_NODES or mat.use_nodes)


# --- Worlds ---------------------------------------------------------------------------------
# Assumed like materials (5.0: ``use_nodes`` deprecated, every World has its tree; step 214, not measured on 5.x).
WORLD_ALWAYS_NODES = MATERIAL_ALWAYS_NODES


def ensure_world_nodes(world):
    """Turn the World's nodes on; True when this call turned them on."""
    if WORLD_ALWAYS_NODES or (world.use_nodes and world.node_tree is not None):
        return False
    world.use_nodes = True
    return True


def release_world_nodes(world):
    if not WORLD_ALWAYS_NODES:
        world.use_nodes = False


# --- Lights ---------------------------------------------------------------------------------
# 5.1: every light has a node tree; ``Light.use_nodes`` is deprecated (warns when read or set, measured step 137).
LIGHT_ALWAYS_NODES = VERSION >= (5, 1)


def ensure_light_nodes(light):
    """Turn the light's nodes on; True when this call turned them on (4.5 / 5.0)."""
    if LIGHT_ALWAYS_NODES or light.use_nodes:
        return False
    light.use_nodes = True
    return True


def release_light_nodes(light):
    """Turn off nodes ``ensure_light_nodes`` turned on (no-op where every light uses nodes)."""
    if not LIGHT_ALWAYS_NODES:
        light.use_nodes = False


def toolbar_back(theme):
    """The 3D View toolbar / sidebar background of the user's theme (read only — the kit never writes a theme,
    step 130): ``view_3d.space.button`` (4.5), ``regions.sidebars.back`` (5.0 theme migration table)."""
    regions = getattr(theme, "regions", None)
    if regions is not None and hasattr(regions, "sidebars"):
        return regions.sidebars.back
    return theme.view_3d.space.button


# --- GPU image shader -----------------------------------------------------------------------
# Kit textures (shelf icons) hold scene-linear colours. 4.5's builtin 'IMAGE' shader converts them for display;
# from 5.0 'IMAGE' draws the values as they are and the converting shader is
# 'IMAGE_SCENE_LINEAR_TO_REC709_SRGB' (5.0 Python API notes, GPU; seen as darker shelf icons, step 119).
LINEAR_IMAGE_SHADER = 'IMAGE_SCENE_LINEAR_TO_REC709_SRGB' if VERSION >= (5, 0) else 'IMAGE'


# --- Unified paint settings -----------------------------------------------------------------
# 4.5: one ``ToolSettings.unified_paint_settings`` with ``unprojected_radius`` (a radius). 5.0+: each paint mode
# has its own (``tool_settings.weight_paint.unified_paint_settings``) and the scene size is ``unprojected_size``,
# a diameter (measured on 5.0.1 / 5.2.3: "Diameter of brush in Blender units", step 153, #90).
def unified_paint_settings(ts, paint="weight_paint"):
    """The unified paint settings that apply to paint mode ``paint`` (a ToolSettings member name)."""
    mode = getattr(ts, paint, None)
    ups = getattr(mode, "unified_paint_settings", None)
    return ups if ups is not None else ts.unified_paint_settings


def unprojected_radius(ups):
    """The brush radius in scene units."""
    if "unprojected_radius" not in ups.bl_rna.properties:
        return ups.unprojected_size / 2.0
    return ups.unprojected_radius


def set_unprojected_radius(ups, radius):
    if "unprojected_radius" not in ups.bl_rna.properties:
        ups.unprojected_size = radius * 2.0
    else:
        ups.unprojected_radius = radius


# --- Brush falloff curve ----------------------------------------------------------------------
# 4.5: ``Brush.curve_preset`` / ``Brush.curve``; 5.0+: ``curve_distance_falloff_preset`` / ``curve_distance_falloff``
# (PR #125 used the 5.x names only, AttributeError on 4.5).
def brush_falloff_props(brush):
    """(preset property, curve property) names of the brush's distance falloff."""
    if "curve_distance_falloff_preset" in brush.bl_rna.properties:
        return "curve_distance_falloff_preset", "curve_distance_falloff"
    return "curve_preset", "curve"


# --- Menus closing when the mouse leaves ----------------------------------------------------
# 4.5 closes a menu the mouse moved away from; 5.0 keeps menus and popups open until a click, with the preference
# ``PreferencesView.menu_close_leave`` (default off) for the old behaviour (5.0 release notes, user interface,
# 5953ff318c; measured on 5.2.3). Python has no call that closes a popup and the kit never writes preferences
# (step 130), so with the 5.x default a held option box can't close the Maya menu under it (step 153).
def menus_close_on_leave(prefs):
    return getattr(prefs.view, "menu_close_leave", True)


# --- Geometry Nodes modifier inputs --------------------------------------------------------------------
# 4.5 – 5.1 keep a Geometry Nodes modifier's inputs as ID properties on the modifier (``md[socket identifier]``,
# ``<identifier>_use_attribute`` / ``_attribute_name``); 5.2 moved them to ``md.properties.inputs.<identifier>``
# with ``value``, ``type`` ('VALUE' / 'ATTRIBUTE') and ``attribute_name``, and ``md[key] = value`` raises
# (measured on 5.2.3, step 200).
GN_INPUT_STRUCT = "properties" in bpy.types.NodesModifier.bl_rna.properties


def _gn_input(md, key):
    return getattr(md.properties.inputs, key, None)


def gn_input_get(md, key):
    """The value of input ``key`` (socket identifier) of the Geometry Nodes modifier ``md``, or None."""
    if GN_INPUT_STRUCT:
        item = _gn_input(md, key)
        return item.value if item is not None else None
    return md[key] if key in md else None


def gn_input_set(md, key, value):
    if GN_INPUT_STRUCT:
        item = _gn_input(md, key)
        if item is not None:
            item.value = value
        return
    md[key] = value


def gn_input_attribute(md, key, attribute):
    """Make input ``key`` read the named attribute ``attribute`` (a vertex group) instead of a value."""
    if GN_INPUT_STRUCT:
        item = _gn_input(md, key)
        if item is not None:
            item.type = 'ATTRIBUTE'
            item.attribute_name = attribute
        return
    md[f"{key}_use_attribute"] = True
    md[f"{key}_attribute_name"] = attribute


def compare_inputs(node):
    """The (A, B) input sockets of a ``FunctionNodeCompare`` for its current data type. 4.5 – 5.1 keep every type's
    sockets on the node (A / B float first, then ``A_INT`` / ``B_INT`` ...; the others unavailable); 5.2 keeps only
    the current type's two (an index 2 raised IndexError, step 212). Looked up by identifier, else the first two."""
    suffix = {'INT': "_INT", 'VECTOR': "_VEC3", 'RGBA': "_COL", 'STRING': "_STR"}.get(node.data_type, "")
    by_id = {s.identifier: s for s in node.inputs}
    a, b = by_id.get("A" + suffix), by_id.get("B" + suffix)
    if a is not None and b is not None:
        return a, b
    return node.inputs[0], node.inputs[1]


# --- Top bar: where the first menu of the Maya menu bar opens ------------------------------------------
# Option box click model (ui/option_click.py, step 183): top bar region left -> the first Maya menu's pulldown,
# in widget units. 4.5: 1.48 (step 183 table); 5.0 – 5.2: 1.25 (step 200, gui_step183_option_click at UI 1.0 =
# 25 px of 20, UI 1.25 = 31 px of 25 on 5.0.1 / 5.1.2 / 5.2.3: the header row starts closer to the region edge).
MENU_BAR_LEAD_UNITS = 1.25 if VERSION >= (5, 0) else 1.48



# --- Mesh selection attributes ----------------------------------------------------------------------------------
# Blender 5.0.1 (measured, step 229, tmp/p229/vanilla7.py / vanilla8.py; 4.5 and 5.2.3 make a BOOLEAN): computing a
# mesh's edges (``Mesh.from_pydata`` with no edges, ``Mesh.update(calc_edges=True)``) while it has a ``.select_vert``
# attribute creates ``.select_edge`` as an EDGE **INT32_2D** attribute (the ``.edge_verts`` layout). Blender then
# crashes on reading it as a selection: writing ``MeshEdge.select`` (rna_MeshEdge_select_set), leaving weight paint
# (mesh_select_vert_flush), and edit mode gets a garbage edge selection. Feature check by type: a selection attribute
# that is not BOOLEAN on its domain is dropped and rebuilt from the vertex selection.
_SELECT_ATTRS = ((".select_vert", 'POINT'), (".select_edge", 'EDGE'), (".select_poly", 'FACE'))


def repair_selection_attributes(mesh):
    """Replace wrongly typed selection attributes (Blender 5.0.1 calc_edges bug) by BOOLEAN ones derived from the
    vertex selection. Returns True when something was repaired. Object mode / non-edit meshes only."""
    bad = [(a.name, d) for n, d in _SELECT_ATTRS for a in mesh.attributes
           if a.name == n and (a.data_type != 'BOOLEAN' or a.domain != d)]
    if not bad:
        return False
    import numpy as np
    for name, _domain in bad:
        for a in [a for a in mesh.attributes if a.name == name]:
            mesh.attributes.remove(a)
    vsel = np.zeros(len(mesh.vertices), bool)
    if len(vsel):
        mesh.vertices.foreach_get("select", vsel)
    for name, domain in bad:
        if name == ".select_vert":
            continue                      # absent = nothing selected
        a = mesh.attributes.new(name, 'BOOLEAN', domain)
        if name == ".select_edge":
            ev = np.zeros(2 * len(mesh.edges), np.int32)
            mesh.edges.foreach_get("vertices", ev)
            ev = ev.reshape(-1, 2)
            values = vsel[ev[:, 0]] & vsel[ev[:, 1]] if len(ev) else np.zeros(0, bool)
        else:
            values = np.array([all(vsel[i] for i in p.vertices) for p in mesh.polygons], bool)
        a.data.foreach_set("value", values.tolist())
    mesh.update()
    return True


def mesh_from_pydata(mesh, vertices, edges, faces):
    """``Mesh.from_pydata`` + the Blender 5.0.1 selection-attribute repair above (use it whenever edges are left for
    Blender to compute)."""
    mesh.from_pydata(vertices, edges, faces)
    repair_selection_attributes(mesh)
