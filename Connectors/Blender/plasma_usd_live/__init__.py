"""Plasma USD Live: join the Plasma editor's USD live sessions from Blender.

Start Blender normally and use the "USD Live" button in the 3D viewport header: it lists the sessions other tools
are in, and starts or joins a session on any USD file. Tools can also start Blender straight into a session:

    blender -- --usd-live-stage <stage.usda> --usd-live-session <name>

In a session, transforms, visibility and material values sync both ways; deleted and added objects go from Blender to
the session. Edits travel through the Plasma editor's live hub when it runs one for the session, otherwise through
the session files (live_link.py).

Modules: sessions (discovery and presence), live_link (the sync), hub (the connection to the Plasma hub),
materials and meshes (material values and added mesh prims), ui (header button, panel and operators).
"""

import argparse
import atexit
import sys

import bpy

from . import live_link, ui


def parse_command_line():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--usd-live-stage")
    parser.add_argument("--usd-live-session")
    args, _ = parser.parse_known_args(argv)
    return args


def at_exit():
    """Python exits with a session still joined (Blender normally leaves it in exit_pre): save pending edits and take
    this Blender off the open session list. Blender's data is gone by now, so nothing may touch it."""
    link = live_link.active_link()
    if link is None:
        return
    try:
        link.leave(redraw=False)
    except Exception:
        pass


def register():
    ui.register()

    if live_link.on_load_pre not in bpy.app.handlers.load_pre:
        bpy.app.handlers.load_pre.append(live_link.on_load_pre)
    exit_pre = getattr(bpy.app.handlers, "exit_pre", None)
    if exit_pre is not None and live_link.on_exit_pre not in exit_pre:
        exit_pre.append(live_link.on_exit_pre)
    atexit.unregister(at_exit)
    atexit.register(at_exit)

    args = parse_command_line()
    if args.usd_live_stage and args.usd_live_session:
        stage, session = args.usd_live_stage, args.usd_live_session

        def join():
            ui.join_session(None, stage, session)
            return None

        # Let the window open first; the import can take a while on large stages.
        bpy.app.timers.register(join, first_interval=0.5)


def unregister():
    live_link.leave_active()
    atexit.unregister(at_exit)

    handlers = bpy.app.handlers
    for handler_list, handler in ((handlers.load_pre, live_link.on_load_pre),
                                  (getattr(handlers, "exit_pre", []), live_link.on_exit_pre),
                                  (handlers.depsgraph_update_post, live_link.on_depsgraph_update),
                                  (handlers.undo_post, live_link.on_undo_redo),
                                  (handlers.redo_post, live_link.on_undo_redo)):
        if handler in handler_list:
            handler_list.remove(handler)

    ui.unregister()
