# SPDX-License-Identifier: GPL-3.0-or-later
"""Maya light / shadow linking on Blender light linking (docs/steps/step_179.md).

Maya 2022: every light illuminates and every object casts
shadows from every light by default; Lighting/Shading > Make / Break Light Links link / unlink every
selected light x selected object, and breaking a group breaks its whole hierarchy; a new object is linked to every
default light; a light whose Illuminates by Default is off (not in defaultLightSet) lights only the objects made links
with. Make / Break Shadow Links = the same for shadows. Select Objects Illuminated by Light / Shadowed by Light take the
*first* selected node, and select the transforms of the linked objects (no groups, no shapes, no sets), or nothing when
that node is not a light; Select Lights Illuminating / Shadowing Object do the reverse.

Blender 4.5 (Cycles probe, step_179.md): ``Object.light_linking.receiver_collection`` / ``blocker_collection`` of a
light; objects in it with link state EXCLUDE are not lit / cast no shadow while everything else is (an empty
collection = everything), and once any object is INCLUDE only the INCLUDE objects are. So:
- default light: a broken link = the object EXCLUDE in the light's collection, a made link = no entry;
- Illuminates by Default off (``Object.allset_illuminates_by_default``, Maya's light attribute): linked objects are
  INCLUDE, and the light itself is INCLUDE too as a placeholder (a light receives no light), so no links = nothing lit.
Collections: ``<light>_lightLinks`` / ``<light>_shadowLinks``, linked to no scene, fake user (Maya keeps the links on
its lightLinker node; delegated names).
"""

import bpy
from bpy.props import BoolProperty, EnumProperty, StringProperty

from .. import undo_steps

KINDS = {"LIGHT": ("receiver_collection", "_lightLinks"), "SHADOW": ("blocker_collection", "_shadowLinks")}
# objects Maya links (shapes Maya renders); empties take part only as group nodes (their hierarchy)
RENDERABLE = {'MESH', 'CURVE', 'SURFACE', 'META', 'FONT', 'CURVES', 'POINTCLOUD', 'VOLUME', 'GREASEPENCIL'}


def is_light(ob):
    return ob is not None and ob.type == 'LIGHT'


def linkable(ob):
    return ob is not None and (ob.type in RENDERABLE or ob.type == 'EMPTY')


def illuminates_by_default(light):
    return bool(getattr(light, "allset_illuminates_by_default", True))


def _collection(light, kind, create=False):
    attr, suffix = KINDS[kind]
    coll = getattr(light.light_linking, attr)
    if coll is None and create:
        coll = bpy.data.collections.new(light.name + suffix)
        coll.use_fake_user = True
        setattr(light.light_linking, attr, coll)
    return coll


def _entries(coll):
    """{object: link state} of a light linking collection."""
    if coll is None:
        return {}
    return {ob: co.light_linking.link_state for ob, co in zip(coll.objects, coll.collection_objects)}


def _set_entry(coll, ob, state):
    if state is None:
        if ob.name in coll.objects:
            coll.objects.unlink(ob)
        return
    if ob.name not in coll.objects:
        coll.objects.link(ob)
    idx = list(coll.objects).index(ob)
    coll.collection_objects[idx].light_linking.link_state = state


def linked(light, ob, kind="LIGHT"):
    """Maya ``cmds.lightlink(query=True)``: does ``light`` illuminate (kind LIGHT) / get shadows from (SHADOW)
    ``ob``."""
    ent = _entries(_collection(light, kind))
    includes = [o for o, s in ent.items() if s == 'INCLUDE']
    if includes:
        return ent.get(ob) == 'INCLUDE'
    if kind == "LIGHT" and not illuminates_by_default(light):
        return False
    return ent.get(ob) != 'EXCLUDE'


def set_linked(light, ob, kind, on):
    """Make (``on``) or break the link of ``light`` and ``ob``."""
    exclusive = kind == "LIGHT" and not illuminates_by_default(light)
    if not exclusive and on and _collection(light, kind) is None:
        return                                   # default: already linked
    coll = _collection(light, kind, create=True)
    if exclusive:
        _set_entry(coll, light, 'INCLUDE')       # placeholder: no INCLUDE object = nothing lit
        _set_entry(coll, ob, 'INCLUDE' if on else None)
    else:
        _set_entry(coll, ob, None if on else 'EXCLUDE')


def set_illuminates_by_default(light, value):
    """Maya Illuminates by Default (defaultLightSet membership): on = lights every object (light links cleared), off =
    lights only objects made links with — none right after switching (directionalLight1 links [] before the
    first Make, case not_default)."""
    if illuminates_by_default(light) == bool(value):
        return
    light["allset_illuminates_by_default"] = bool(value)
    coll = _collection(light, "LIGHT", create=not value)
    if coll is not None:
        for o in list(coll.objects):
            coll.objects.unlink(o)
        if not value:
            _set_entry(coll, light, 'INCLUDE')


def hierarchy(objects):
    """The objects with all their descendants (Maya breaks / makes a group's whole hierarchy)."""
    out, seen = [], set()
    for ob in objects:
        for o in (ob, *ob.children_recursive):
            if o.name not in seen and linkable(o):
                seen.add(o.name)
                out.append(o)
    return out


def split_selection(objects):
    lights = [o for o in objects if is_light(o)]
    others = [o for o in objects if not is_light(o) and linkable(o)]
    return lights, others


def link_selection(objects, kind, on):
    """Maya Make / Break Light Links (shadow links too): (lights, objects) changed."""
    lights, others = split_selection(objects)
    targets = hierarchy(others)
    for light in lights:
        for ob in targets:
            set_linked(light, ob, kind, on)
    return lights, targets


def objects_of(light, kind, scene):
    """Maya Select Objects Illuminated / Shadowed by Light: transforms of linked renderable objects (no groups)."""
    return [o for o in scene.objects if o.type in RENDERABLE and linked(light, o, kind)]


def lights_of(ob, kind, scene):
    return [o for o in scene.objects if is_light(o) and linked(o, ob, kind)]


def first_selected(context):
    """Maya reads the first node of the active list; Blender keeps no selection order, so the active object when it
    is selected, else the first selected one (delegated)."""
    ob = context.active_object
    sel = list(context.selected_objects)
    if ob is not None and ob in sel:
        return ob
    return sel[0] if sel else None


def _select_only(context, objects):
    for o in context.selected_objects:
        o.select_set(False)
    for o in objects:
        try:
            o.select_set(True)
        except RuntimeError:            # not in the view layer
            pass
    if objects:
        context.view_layer.objects.active = objects[0]


# --- operators -----------------------------------------------------------------------------------

_KIND_ITEMS = [("LIGHT", "Light", ""), ("SHADOW", "Shadow", "")]


@undo_steps.no_redo_panel
class ALLSET_OT_light_link(bpy.types.Operator):
    """Make or break light (or shadow) links between the selected lights and the selected objects (Maya Make / Break
    Light Links, Make / Break Shadow Links)"""
    bl_idname = "allset.light_link"
    bl_label = "Light Links"
    bl_options = {'REGISTER', 'UNDO'}

    kind: EnumProperty(items=_KIND_ITEMS, default="LIGHT", options={'SKIP_SAVE'})
    make: BoolProperty(default=True, options={'SKIP_SAVE'})

    @classmethod
    def poll(cls, context):
        return context.mode == 'OBJECT'

    def execute(self, context):
        lights, targets = link_selection(list(context.selected_objects), self.kind, self.make)
        if not lights or not targets:
            self.report({'WARNING'}, "Select at least one light and one object")
            return {'CANCELLED'}
        for ob in targets:
            ob.update_tag()
        for light in lights:
            light.update_tag()
        return {'FINISHED'}


@undo_steps.no_redo_panel
class ALLSET_OT_light_link_select(bpy.types.Operator):
    """Select the objects the first selected light illuminates / shadows, or the lights illuminating / shadowing the
    first selected object (Maya Select Objects Illuminated by Light ...)"""
    bl_idname = "allset.light_link_select"
    bl_label = "Select by Light Links"
    bl_options = {'REGISTER', 'UNDO'}

    kind: EnumProperty(items=_KIND_ITEMS, default="LIGHT", options={'SKIP_SAVE'})
    lights: BoolProperty(default=False, options={'SKIP_SAVE'}, description="Select lights (else objects)")

    @classmethod
    def poll(cls, context):
        return context.mode == 'OBJECT'

    def execute(self, context):
        first = first_selected(context)
        if first is None:
            return {'CANCELLED'}
        if self.lights:
            found = [] if is_light(first) else lights_of(first, self.kind, context.scene)
        else:
            found = objects_of(first, self.kind, context.scene) if is_light(first) else []
        _select_only(context, found)     # Maya clears the selection even when nothing matches
        return {'FINISHED'}


class ALLSET_OT_light_link_toggle(bpy.types.Operator):
    """Make or break this link (Light Linking Editor)"""
    bl_idname = "allset.light_link_toggle"
    bl_label = "Toggle Light Link"
    bl_options = {'INTERNAL', 'UNDO'}

    light: StringProperty(options={'SKIP_SAVE'})
    target: StringProperty(options={'SKIP_SAVE'})
    kind: EnumProperty(items=_KIND_ITEMS, default="LIGHT", options={'SKIP_SAVE'})

    def execute(self, context):
        light, ob = bpy.data.objects.get(self.light), bpy.data.objects.get(self.target)
        if light is None or ob is None:
            return {'CANCELLED'}
        on = not linked(light, ob, self.kind)
        for o in hierarchy([ob]):
            set_linked(light, o, self.kind, on)
        light.update_tag()
        ob.update_tag()
        for area in context.screen.areas:
            area.tag_redraw()
        return {'FINISHED'}


classes = (ALLSET_OT_light_link, ALLSET_OT_light_link_select, ALLSET_OT_light_link_toggle)


def _ibd_get(self):
    return bool(self.get("allset_illuminates_by_default", True))


def _ibd_set(self, value):
    set_illuminates_by_default(self, value)


def register_props():
    bpy.types.Object.allset_illuminates_by_default = BoolProperty(
        name="Illuminates by Default", description="The light lights every object unless a link is broken (Maya "
        "Illuminates by Default); off: only the objects linked to it", get=_ibd_get, set=_ibd_set)


def unregister_props():
    del bpy.types.Object.allset_illuminates_by_default
