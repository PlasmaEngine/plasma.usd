# SPDX-License-Identifier: GPL-3.0-or-later
"""Step 152 (#101) Maya option windows of the Render menu.

Same spec as operators/option_windows.py (``F`` fields, ``Window`` = one ``allset.opt_<key>`` operator;
option_windows._extend appends ``WINDOWS``). Maya 2022 with factory prefs:

- Render > Render Sequence ❒ = "Render Sequence (Current Frame)": Render Options (Current Camera persp, All
  Render-Enabled Layers / Cameras, Render Region, Ignore Local Overrides off, Alternate Output File Location empty),
  Render View Options (Add to Render View off); buttons "Render Sequence and Close" / "Render Sequence" / "Close".
  The kit runs Current Camera and Alternate Output File Location; the rest are Maya concepts Blender has no part for.
- Render > Batch Render ❒: the factory renderer (Arnold) opens no window; Maya Software opens "Batch Render Frame"
  (Use all available processors on, Number of processors to use 0; "Batch render and close" / "Batch render").
  The kit's batch render is a background Blender, so the Maya Software window maps to its ``-t`` threads
  (delegated, step 152).
- Render > Render Current Frame ❒ / IPR Render Current Frame ❒ open no window in Maya: the Help Line reads
  'Renderer "<name>" does not provide render options.' — ``NO_OPTIONS`` (option_windows.ALLSET_OT_option_generic).
"""

import bpy
from bpy.props import EnumProperty, StringProperty

from .option_windows import F, Window

# Maya's warning for an option box its renderer has no window for; {name} = the render engine
NO_OPTIONS = {"Render/Render Current Frame": 'Renderer "{name}" does not provide render options.',
              "Render/IPR Render Current Frame": 'Renderer "{name}" does not provide IPR options.'}
ENGINE_NAMES = {'BLENDER_EEVEE_NEXT': "EEVEE", 'BLENDER_EEVEE': "EEVEE", 'CYCLES': "Cycles",
                'BLENDER_WORKBENCH': "Workbench"}


def engine_name(scene):
    return ENGINE_NAMES.get(scene.render.engine, scene.render.engine.title())


def no_options_text(path, scene):
    """Maya's Help Line warning for ``path``'s option box, or None when it has a window."""
    text = NO_OPTIONS.get(path)
    return text.format(name=engine_name(scene)) if text else None


def _cameras(_self, context):
    items = [("persp", "persp", "The view's camera, else the kit persp camera (Maya persp)")]
    scene = getattr(context, "scene", None)
    for ob in sorted((o for o in (scene.objects if scene else ()) if o.type == 'CAMERA' and not o.get("allset_persp")),
                     key=lambda o: o.name):
        items.append((ob.name, ob.name, ""))
    return items


class FCamera(F):
    """Maya's Current Camera combo: persp + the scene's cameras (items read when the window opens)."""

    def __init__(self, name, label):
        super().__init__(name, label, "camera", "persp")

    def prop(self):
        return EnumProperty(name=self.label.rstrip(":"), items=_cameras)


class FPath(F):
    """A folder field (Maya: text field + folder button)."""

    def __init__(self, name, label):
        super().__init__(name, label, "path", "")

    def prop(self):
        return StringProperty(name=self.label.rstrip(":"), default="", subtype='DIR_PATH')


def _run_sequence(context, v):
    camera = v.get("camera") or "persp"
    return bpy.ops.allset.render_sequence('INVOKE_DEFAULT', camera="" if camera == "persp" else camera,
                                          output=v.get("output") or "")


def _run_batch(context, v):
    threads = 0 if v["all_processors"] else max(1, int(v["processors"]))
    return bpy.ops.allset.batch_render('EXEC_DEFAULT', threads=threads)


WINDOWS = [
    Window("render_sequence", "Render Sequence (Current Frame)", "Render Sequence and Close", None, [
        FCamera("camera", "Current Camera:"),
        FPath("output", "Alternate Output File Location:")], _run_sequence, paths=("Render/Render Sequence",),
        apply_text="Render Sequence",
        missing=("All Render-Enabled Layers", "All Render-Enabled Cameras", "Render Region",
                 "Ignore Local Overrides", "Render View Options: Add to Render View")),
    Window("batch_render", "Batch Render Frame", "Batch render and close", None, [
        F("all_processors", "", "bool", True, text="Use all available processors"),
        F("processors", "Number of processors to use:", "int", 0, lo=0, hi=256)], _run_batch,
        paths=("Render/Batch Render",), apply_text="Batch render"),
]
