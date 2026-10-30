# SPDX-License-Identifier: GPL-3.0-or-later
"""Render > Test Resolution and Render > Batch Render / Cancel / Show Batch Render (step 152, #101).

Test Resolution (Maya's Render > Test Resolution submenu): Camera Panel,
Render Settings (WxH), then 10 / 25 / 50 / 75 % and 110 / 125 / 150 % of the Render Settings size, a check on the
current one, default Render Settings. It is a Maya preference (optionVar renderViewTestResolution) used by Render View
renders only, not by batch renders: the kit keeps it in the add-on preference ``test_resolution`` and applies it to
its Render Current Frame / Render Sequence for the duration of the render (``scaled``), Render Settings untouched.
Camera Panel = the size of the 3D View the render follows.

Batch Render (Maya renders the scene in a separate process; Cancel stops it, Show opens the last image): a copy of the
open file (``save_as_mainfile(copy=True)``: the open file keeps its name and unsaved state; relative paths remapped)
is written to Blender's session temp folder with Maya's persp camera and default light in it, then
``blender -b <copy> -o <output> -a`` (``-f <frame>`` when the output name has no frame number, Maya's single-frame
name.ext) runs as its own process. Output = the scene's render path made absolute against the open file, or, while
it is still Blender's default, the current project's images folder (step 69). Show Batch Render loads the newest
image of that output into an Image Editor window.
"""

import os
import subprocess

import bpy
from bpy.props import StringProperty

from .. import results
from . import default_light, project, render_persp

# key, Maya percentage (None = no scaling / the panel size)
TEST_RESOLUTIONS = (("PANEL", None), ("SETTINGS", None), ("10", 10), ("25", 25), ("50", 50), ("75", 75),
                    ("110", 110), ("125", 125), ("150", 150))
DEFAULT_TEST = "SETTINGS"
IMAGE_EXTS = (".png", ".jpg", ".jpeg", ".exr", ".tif", ".tiff", ".bmp", ".tga", ".hdr", ".webp", ".dpx", ".cin",
              ".jp2", ".rgb", ".sgi")


def _prefs(context=None):
    from .. import preferences
    try:
        return preferences.get(context)
    except KeyError:
        return None


def test_resolution(context=None):
    prefs = _prefs(context)
    return getattr(prefs, "test_resolution", DEFAULT_TEST) if prefs is not None else DEFAULT_TEST


def settings_size(scene):
    """Render Settings size in pixels (Blender's resolution scale included)."""
    r = scene.render
    return r.resolution_x * r.resolution_percentage // 100, r.resolution_y * r.resolution_percentage // 100


def menu_labels(scene):
    """[(key, Maya label)] of the Test Resolution submenu; None = a divider."""
    w, h = settings_size(scene)
    out = [("PANEL", "Camera Panel"), ("SETTINGS", f"Render Settings ({w}x{h})"), None]
    for key, perc in TEST_RESOLUTIONS[2:]:
        out.append((key, f"{perc}% Settings ({w * perc // 100}x{h * perc // 100})"))
        if key == "75":
            out.append(None)
    return out


def _view_region(space):
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.spaces.active == space:
                return next((rg for rg in area.regions if rg.type == 'WINDOW'), None)
    return None


def scaled(context, key=None):
    """Apply Test Resolution ``key`` (default: the preference) to the scene; returns a function that restores it."""
    key = key or test_resolution(context)
    scene = context.scene
    r = scene.render
    saved = (r.resolution_x, r.resolution_y, r.resolution_percentage)
    perc = dict(TEST_RESOLUTIONS).get(key)
    if key == "PANEL":
        space, _rv3d = render_persp.find_view(context)
        region = _view_region(space) if space is not None else None
        if region is not None and region.width > 1 and region.height > 1:
            r.resolution_x, r.resolution_y, r.resolution_percentage = region.width, region.height, 100
    elif perc is not None:
        r.resolution_percentage = max(1, round(saved[2] * perc / 100))

    def restore():
        r.resolution_x, r.resolution_y, r.resolution_percentage = saved
    return restore


class ALLSET_OT_test_resolution(bpy.types.Operator):
    """Resolution of Render Current Frame / Render Sequence (Maya Render > Test Resolution; batch renders use the
    Render Settings size)"""
    bl_idname = "allset.test_resolution"
    bl_label = "Test Resolution"
    bl_options = {'INTERNAL'}

    choice: StringProperty(name="Choice", default=DEFAULT_TEST)

    def execute(self, context):
        prefs = _prefs(context)
        if prefs is None or self.choice not in dict(TEST_RESOLUTIONS):
            return {'CANCELLED'}
        prefs.test_resolution = self.choice
        return {'FINISHED'}


class ALLSET_MT_test_resolution(bpy.types.Menu):
    """Maya Render > Test Resolution"""
    bl_idname = "ALLSET_MT_test_resolution"
    bl_label = "Test Resolution"

    def draw(self, context):
        layout = self.layout
        current = test_resolution(context)
        for entry in menu_labels(context.scene):
            if entry is None:
                layout.separator()
                continue
            key, label = entry
            layout.operator("allset.test_resolution", text=label,
                            icon='CHECKBOX_HLT' if key == current else 'CHECKBOX_DEHLT').choice = key


# --- Batch Render ------------------------------------------------------------------------------------------------

_batch = {"proc": None, "out": "", "copy": "", "log": "", "started": 0.0, "cmd": []}


def running():
    proc = _batch["proc"]
    return proc is not None and proc.poll() is None


def output_path(scene, context=None):
    """Absolute output pattern of a batch render of ``scene``."""
    fp = scene.render.filepath
    stem = bpy.path.display_name_from_filepath(bpy.data.filepath) or "untitled"
    if fp in project.DEFAULT_RENDER_PATHS:
        return os.path.join(project.ensure_root(context), "images", stem)
    if fp.startswith("//") and not bpy.data.filepath:
        # an untitled scene has no folder for "//": the project's scenes folder stands for it
        path = os.path.join(project.ensure_root(context), "scenes", fp[2:])
    else:
        path = bpy.path.abspath(fp)
    if path.endswith(("/", "\\")):
        path = os.path.join(path, stem)
    return os.path.normpath(path)


def frame_args(scene):
    """Maya: a name without a frame number renders the current frame, else the animation range."""
    # bpy.path.basename: os.path reads a blend-relative "//dir/name" as a UNC share on Windows (basename "")
    if "#" in bpy.path.basename(scene.render.filepath):
        return ["-a"]
    return ["-f", str(scene.frame_current)]


def command(copy_path, out, frames, threads=0):
    """The background Blender command; ``threads`` > 0 = Maya's "Number of processors to use" (Blender ``-t``)."""
    extra = ["-t", str(threads)] if threads > 0 else []
    return [bpy.app.binary_path, "-b", copy_path, *extra, "-o", out, *frames]


def write_copy(context, scene):
    """Save the batch copy of the open file (persp camera + default light in it); its path."""
    folder = bpy.app.tempdir or os.path.join(project.ensure_root(context), "renderData")
    os.makedirs(folder, exist_ok=True)
    stem = bpy.path.display_name_from_filepath(bpy.data.filepath) or "untitled"
    path = os.path.join(folder, f"batch_{stem}.blend")
    cam = render_persp.render_camera(context)
    default_light.add(scene, cam)
    try:
        bpy.ops.wm.save_as_mainfile(filepath=path, copy=True, check_existing=False)
    finally:
        default_light.remove_all()
    return path


def start(cmd, log_path):
    log = open(log_path, "w", encoding="utf-8", errors="replace")
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    try:
        return subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                creationflags=flags)
    finally:
        log.close()


def _watch():
    proc = _batch["proc"]
    if proc is None:
        return None
    if proc.poll() is None:
        return 1.0
    _cleanup_copy()
    return None


def _cleanup_copy():
    path = _batch["copy"]
    if path and os.path.exists(path):
        try:
            os.remove(path)
        except OSError:
            pass


def stop():
    """Stop the running batch render; True when one was running."""
    proc = _batch["proc"]
    if proc is None or proc.poll() is not None:
        return False
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
    _cleanup_copy()
    return True


def last_image():
    """Newest image file of the last batch render's output, or None."""
    out = _batch["out"]
    if not out:
        return None
    folder, prefix = os.path.split(out)
    if not os.path.isdir(folder):
        return None
    files = [os.path.join(folder, f) for f in os.listdir(folder)
             if f.startswith(prefix) and f.lower().endswith(IMAGE_EXTS)]
    files = [f for f in files if os.path.getmtime(f) >= _batch["started"] - 1.0]
    return max(files, key=os.path.getmtime, default=None)


class ALLSET_OT_batch_render(bpy.types.Operator):
    """Render the scene in a separate Blender process (Maya Render > Batch Render): the frame range when the
    output name has a frame number, else the current frame"""
    bl_idname = "allset.batch_render"
    bl_label = "Batch Render"
    bl_options = {'REGISTER'}

    threads: bpy.props.IntProperty(name="Processors", default=0, min=0, options={'SKIP_SAVE', 'HIDDEN'},
                                   description="Number of processors to use (0 = all, Maya Batch Render Frame)")

    def execute(self, context):
        import time
        if running():
            self.report({'WARNING'}, "A batch render is already running (Render > Cancel Batch Render stops it)")
            return {'CANCELLED'}
        scene = context.scene
        out = output_path(scene, context)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        try:
            copy_path = write_copy(context, scene)
        except (RuntimeError, OSError) as ex:
            self.report({'ERROR'}, f"Batch Render: the scene copy could not be saved ({ex})")
            return {'CANCELLED'}
        cmd = command(copy_path, out, frame_args(scene), self.threads)
        log_path = os.path.splitext(copy_path)[0] + ".log"
        try:
            proc = start(cmd, log_path)
        except OSError as ex:
            self.report({'ERROR'}, f"Batch Render could not start Blender ({ex})")
            return {'CANCELLED'}
        _batch.update(proc=proc, out=out, copy=copy_path, log=log_path, started=time.time(), cmd=cmd)
        if not bpy.app.timers.is_registered(_watch):
            bpy.app.timers.register(_watch, first_interval=1.0, persistent=True)
        results.result(self, f"Batch render started: {os.path.dirname(out)}")
        return {'FINISHED'}


class ALLSET_OT_cancel_batch_render(bpy.types.Operator):
    """Stop the running batch render (Maya Render > Cancel Batch Render)"""
    bl_idname = "allset.cancel_batch_render"
    bl_label = "Cancel Batch Render"

    def execute(self, context):
        if not stop():
            self.report({'WARNING'}, "No batch render is running")
            return {'CANCELLED'}
        results.result(self, "Batch render cancelled")
        return {'FINISHED'}


class ALLSET_OT_show_batch_render(bpy.types.Operator):
    """Show the last image of the batch render in an Image Editor window (Maya Render > Show Batch Render)"""
    bl_idname = "allset.show_batch_render"
    bl_label = "Show Batch Render"

    def execute(self, context):
        path = last_image()
        if path is None:
            msg = "The batch render has written no image yet" if _batch["out"] else "No batch render has run"
            self.report({'WARNING'}, msg)
            return {'CANCELLED'}
        image = bpy.data.images.load(path, check_existing=True)
        image.reload()
        window = context.window
        if window is None or bpy.app.background:
            return {'FINISHED'}
        from . import menu_extras
        area = max(window.screen.areas, key=lambda a: a.width * a.height, default=None)
        new = menu_extras.open_window(context, area) if area is not None else None
        if new is None:
            return {'FINISHED'}
        target = new.screen.areas[0]
        target.ui_type = 'IMAGE_EDITOR'
        target.spaces.active.image = image
        return {'FINISHED'}


classes = (ALLSET_OT_test_resolution, ALLSET_MT_test_resolution, ALLSET_OT_batch_render,
           ALLSET_OT_cancel_batch_render, ALLSET_OT_show_batch_render)


def unregister():
    if bpy.app.timers.is_registered(_watch):
        bpy.app.timers.unregister(_watch)
