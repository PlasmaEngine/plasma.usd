# SPDX-License-Identifier: GPL-3.0-or-later
"""Maya Light Linking Editor (Lighting/Shading > Light Linking Editor; Maya Relationship Editor light-centric /
object-centric, Maya 2026): two lists, the left one picks a light (light-centric)
or an object (object-centric), the right one shows what it is linked to — click a row to make / break the link.
A kit popover (keep_open) on ``operators/light_links.py``; Light / Shadow switches between light and shadow links
(Maya's editor has one per kind). Step 179."""

import bpy
from bpy.props import EnumProperty, StringProperty

from ..operators import light_links as ll

_MODES = [("LIGHT_CENTRIC", "Light Centric", "Pick a light, link objects to it"),
          ("OBJECT_CENTRIC", "Object Centric", "Pick an object, link lights to it")]


def _current(context):
    wm = context.window_manager
    ob = bpy.data.objects.get(wm.allset_ll_current)
    want_light = wm.allset_ll_mode == "LIGHT_CENTRIC"
    if ob is not None and ll.is_light(ob) == want_light and ob.name in context.scene.objects:
        return ob
    for o in context.selected_objects:
        if ll.is_light(o) == want_light and (want_light or ll.linkable(o)):
            return o
    return None


def lists(context):
    """(left objects, right objects) of the editor in its current mode."""
    lights = [o for o in context.scene.objects if ll.is_light(o)]
    objects = [o for o in context.scene.objects if not ll.is_light(o) and ll.linkable(o)]
    if context.window_manager.allset_ll_mode == "LIGHT_CENTRIC":
        return lights, objects
    return objects, lights


class ALLSET_PT_light_linking(bpy.types.Panel):
    """Maya Light Linking Editor"""
    bl_idname = "ALLSET_PT_light_linking"
    bl_label = "Light Linking Editor"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_ui_units_x = 22

    def draw(self, context):
        layout = self.layout
        wm = context.window_manager
        row = layout.row()
        row.prop(wm, "allset_ll_mode", expand=True)
        row.prop(wm, "allset_ll_kind", expand=True)
        left, right = lists(context)
        current = _current(context)
        split = layout.split(factor=0.5)
        col_l, col_r = split.column(align=True), split.column(align=True)
        light_centric = wm.allset_ll_mode == "LIGHT_CENTRIC"
        col_l.label(text="Light Sources" if light_centric else "Illuminated Objects")
        col_r.label(text="Illuminated Objects" if light_centric else "Light Sources")
        for o in left:
            col_l.operator(ALLSET_OT_light_link_pick.bl_idname, text=o.name, depress=o == current,
                           icon='LIGHT' if light_centric else 'OBJECT_DATA').name = o.name
        for o in right:
            on = current is not None and (ll.linked(current, o, wm.allset_ll_kind) if light_centric
                                          else ll.linked(o, current, wm.allset_ll_kind))
            sub = col_r.row(align=True)
            sub.enabled = current is not None
            op = sub.operator(ll.ALLSET_OT_light_link_toggle.bl_idname, text=o.name,
                              icon='CHECKBOX_HLT' if on else 'CHECKBOX_DEHLT', depress=on)
            op.light, op.target = (current.name, o.name) if light_centric else (o.name, current.name if current else "")
            op.kind = wm.allset_ll_kind
        if not left:
            col_l.label(text="(none)")


class ALLSET_OT_light_link_pick(bpy.types.Operator):
    """Show this node's links"""
    bl_idname = "allset.light_link_pick"
    bl_label = "Pick"
    bl_options = {'INTERNAL'}

    name: StringProperty(options={'SKIP_SAVE'})

    def execute(self, context):
        context.window_manager.allset_ll_current = self.name
        return {'FINISHED'}


class ALLSET_OT_light_linking_editor(bpy.types.Operator):
    """Open the Light Linking Editor (Maya Lighting/Shading > Light Linking Editor)"""
    bl_idname = "allset.light_linking_editor"
    bl_label = "Light Linking Editor"
    bl_options = {'INTERNAL'}

    mode: EnumProperty(items=_MODES, default="LIGHT_CENTRIC", options={'SKIP_SAVE'})

    def execute(self, context):
        wm = context.window_manager
        wm.allset_ll_mode = self.mode
        cur = _current(context)
        wm.allset_ll_current = cur.name if cur is not None else ""
        bpy.ops.wm.call_panel(name=ALLSET_PT_light_linking.bl_idname, keep_open=True)
        return {'FINISHED'}


classes = (ALLSET_PT_light_linking, ALLSET_OT_light_link_pick, ALLSET_OT_light_linking_editor)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)
    wm = bpy.types.WindowManager
    wm.allset_ll_mode = EnumProperty(items=_MODES, name="Mode", default="LIGHT_CENTRIC")
    wm.allset_ll_kind = EnumProperty(items=[("LIGHT", "Light Links", ""), ("SHADOW", "Shadow Links", "")],
                                     name="Links", default="LIGHT")
    wm.allset_ll_current = StringProperty(name="Current")


def unregister():
    wm = bpy.types.WindowManager
    del wm.allset_ll_current, wm.allset_ll_kind, wm.allset_ll_mode
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
