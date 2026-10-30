"""The viewport header button, the session list and the operators behind it.

The header of every 3D viewport shows "USD Live" with the number of open sessions, or the joined session's name. Its
popover (and the "USD Live" sidebar tab) lists the open sessions to join, and starts or joins a session on any USD file.
In a session it shows how edits travel ("Live via Plasma hub" with the round trip time, or "File sync"), the
participants (from the hub with their live state and latency while connected, otherwise from the session files), and
the recent sync events.
"""

import getpass
import os
import re
import traceback

import bpy
from bpy.props import EnumProperty, StringProperty

from . import hub, live_link, sessions

REFRESH_SECONDS = 2.0

_open_sessions = []
_signature = None

# Blender keeps only pointers to the strings of dynamic enum items, so the list has to outlive the callback.
_session_items = []


def user_name():
    return getpass.getuser().lower().replace(" ", "_")


def refresh_open_sessions():
    global _open_sessions, _signature
    found = sessions.list_open_sessions()
    signature = [(s.session_root, s.name, tuple(sorted(p.name for p in s.participants))) for s in found]
    if signature != _signature:
        _signature = signature
        _open_sessions = found
        live_link.redraw_view3d()
    return REFRESH_SECONDS


def join_session(operator, stage, session):
    """Joins or starts a session and reports the result on the operator, or the console without one."""

    def report(kind, text):
        if operator is not None:
            operator.report({kind}, text)
        print("[USD Live]", text, flush=True)

    session_root = sessions.session_folder(stage, session) + "/session.usda"
    participant = sessions.choose_participant(session_root, user_name())
    link = live_link.LiveLink(stage, session, participant)

    window = bpy.context.window
    if window is not None:
        window.cursor_set('WAIT')
    try:
        link.start()
    except Exception as e:
        traceback.print_exc()
        link.leave()
        report('ERROR', "Could not join '{}': {}".format(session, e))
        return {'CANCELLED'}
    finally:
        if window is not None:
            window.cursor_set('DEFAULT')

    refresh_open_sessions()
    report('INFO', "Joined '{}' as {}".format(session, participant))
    return {'FINISHED'}


# -- drawing -----------------------------------------------------------------------------------------------------


def draw_live(layout):
    link = live_link.active_link()
    if link is not None and link.stage is not None:
        draw_connected(layout, link)
    else:
        draw_sessions(layout)


def format_latency(ms):
    return "{:.0f} ms".format(ms) if isinstance(ms, (int, float)) and not isinstance(ms, bool) else None


def hub_status(link):
    """(text, icon) of the line saying how edits travel."""
    connection = link.hub
    if connection.live:
        latency = format_latency(connection.latency_ms)
        return "Live via Plasma hub" + (", " + latency if latency else ""), 'NETWORK_DRIVE'
    if connection.state != hub.OFF:
        return "File sync, connecting to the Plasma hub", 'FILE_REFRESH'
    return "File sync", 'FILE_REFRESH'


def draw_connected(layout, link):
    col = layout.column(align=True)
    col.label(text=os.path.basename(link.stage_path), icon='FILE_3D')
    col.label(text="Session: " + link.session_name, icon='LINKED')
    col.label(text="Your layer: " + link.participant + ".usda", icon='USER')
    text, icon = hub_status(link)
    col.label(text=text, icon=icon)

    box = layout.box()
    if link.hub.live and link.hub.participants:
        box.label(text="Participants (Plasma hub)")
        col = box.column(align=True)
        for p in link.hub.participants:
            name = str(p.get("participant", "?"))
            live = p.get("live") is True
            text = name + ("  (you)" if name == link.participant else "")
            if live:
                latency = format_latency(p.get("latencyMs"))
                text += "  - live" + (", " + latency if latency else "")
            else:
                text += "  - not live"
            col.label(text=text, icon='RADIOBUT_ON' if live else 'RADIOBUT_OFF')
    else:
        box.label(text="Participants, strongest first")
        col = box.column(align=True)
        for name, online in link.participants():
            text = name + ("  (you)" if name == link.participant else "") + ("" if online else "  - away")
            col.label(text=text, icon='RADIOBUT_ON' if online else 'RADIOBUT_OFF')

    box = layout.box()
    box.label(text="Recent")
    col = box.column(align=True)
    for line in reversed(link.log):
        col.label(text=line)

    layout.operator("usd_live.leave", icon='CANCEL')


def draw_sessions(layout):
    layout.label(text="Open sessions")

    if not _open_sessions:
        box = layout.box()
        box.label(text="No tool is in a session right now", icon='INFO')

    for session in _open_sessions:
        box = layout.box()
        row = box.row()
        col = row.column(align=True)
        col.label(text=session.name, icon='LINKED')
        col.label(text=os.path.basename(session.stage), icon='FILE_3D')
        col.label(text=", ".join(p.name for p in session.participants), icon='COMMUNITY')
        op = row.column().operator("usd_live.join", text="Join")
        op.stage = session.stage
        op.session = session.name

    layout.separator()
    layout.operator("usd_live.open_stage", text="Start or Join on a USD File...", icon='FILEBROWSER')


def draw_header(self, context):
    link = live_link.active_link()
    if link is not None and link.stage is not None:
        text, icon = link.session_name, 'LINKED'
    else:
        count = len(_open_sessions)
        text, icon = ("USD Live ({})".format(count) if count else "USD Live"), 'UNLINKED'
    self.layout.popover(panel="VIEW3D_PT_usd_live_popover", text=text, icon=icon)


class VIEW3D_PT_usd_live_popover(bpy.types.Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_label = "USD Live"
    bl_ui_units_x = 18

    def draw(self, context):
        draw_live(self.layout)


class VIEW3D_PT_usd_live(bpy.types.Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "USD Live"
    bl_label = "Plasma USD Live"

    def draw(self, context):
        draw_live(self.layout)


# -- operators ---------------------------------------------------------------------------------------------------


class USD_LIVE_OT_join(bpy.types.Operator):
    bl_idname = "usd_live.join"
    bl_label = "Join USD Live Session"
    bl_description = "Join the session: import the stage, or reuse it from this file, and sync edits both ways"

    stage: StringProperty(options={'HIDDEN'})
    session: StringProperty(options={'HIDDEN'})

    def execute(self, context):
        return join_session(self, self.stage, self.session)


def session_items(self, context):
    global _session_items
    items = []
    for name, participants in sessions.list_sessions_beside(self.stage):
        description = "Participants: " + (", ".join(participants) or "none yet")
        items.append((name, name, description, 'LINKED', len(items)))
    items.append(("__new__", "New session", "Start a new session on this stage, with your layer strongest", 'ADD',
                  len(items)))
    _session_items = items
    return items


class USD_LIVE_OT_choose_session(bpy.types.Operator):
    bl_idname = "usd_live.choose_session"
    bl_label = "USD Live Session"
    bl_description = "Join one of the sessions beside a USD file, or start a new one"

    stage: StringProperty(options={'HIDDEN'})
    session: EnumProperty(name="Session", items=session_items)
    new_name: StringProperty(name="Name", default="session")

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self, width=380, confirm_text="Join")

    def draw(self, context):
        layout = self.layout
        layout.label(text=os.path.basename(self.stage), icon='FILE_3D')
        layout.column().prop(self, "session", expand=True)
        if self.session == "__new__":
            layout.prop(self, "new_name")

    def execute(self, context):
        name = self.session
        if name == "__new__":
            name = re.sub(r"[^A-Za-z0-9_\-]+", "-", self.new_name).strip("-")
            if not name:
                self.report({'ERROR'}, "The session needs a name")
                return {'CANCELLED'}
        return join_session(self, self.stage, name)


class USD_LIVE_OT_open_stage(bpy.types.Operator):
    bl_idname = "usd_live.open_stage"
    bl_label = "Choose USD Stage"
    bl_description = "Pick a USD file, then join one of its sessions or start a new one"

    filepath: StringProperty(subtype='FILE_PATH')
    filter_glob: StringProperty(default="*.usd;*.usda;*.usdc;*.usdz", options={'HIDDEN'})

    def invoke(self, context, event):
        context.window_manager.fileselect_add(self)
        return {'RUNNING_MODAL'}

    def execute(self, context):
        if not os.path.isfile(self.filepath):
            self.report({'ERROR'}, "Not a file: " + self.filepath)
            return {'CANCELLED'}
        bpy.ops.usd_live.choose_session('INVOKE_DEFAULT', stage=self.filepath)
        return {'FINISHED'}


class USD_LIVE_OT_leave(bpy.types.Operator):
    bl_idname = "usd_live.leave"
    bl_label = "Leave Session"
    bl_description = "Stop syncing. Your layer and the imported objects stay, and joining again reuses them"

    def execute(self, context):
        live_link.leave_active()
        refresh_open_sessions()
        return {'FINISHED'}


classes = (
    VIEW3D_PT_usd_live_popover,
    VIEW3D_PT_usd_live,
    USD_LIVE_OT_join,
    USD_LIVE_OT_choose_session,
    USD_LIVE_OT_open_stage,
    USD_LIVE_OT_leave,
)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)
    bpy.types.VIEW3D_HT_header.append(draw_header)
    bpy.app.timers.register(refresh_open_sessions, first_interval=0.5, persistent=True)


def unregister():
    if bpy.app.timers.is_registered(refresh_open_sessions):
        bpy.app.timers.unregister(refresh_open_sessions)
    bpy.types.VIEW3D_HT_header.remove(draw_header)
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
