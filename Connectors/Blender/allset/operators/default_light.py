# SPDX-License-Identifier: GPL-3.0-or-later
"""Maya's default light for kit renders of a scene without lights (step 152, #101).

Maya (Maya Software): Render Settings > Common > Render Options >
Enable Default Light (``defaultRenderGlobals.enableDefaultLight``, factory on) lights a scene that has no light node
(a hidden light, an ambient light or a light outside defaultLightSet all turn it off) with a white directional light
of intensity 1 that follows the render camera: its direction is the camera's view axis turned 25 deg up and 25 deg to
the left (toward the light, camera space normalize(-0.383, 0.423, 0.821)), no shadows. A lambert renders
color x diffuse x cos(angle to that direction).

Blender has no default light. The kit adds one for the duration of its own renders (Render > Render Current Frame /
Render Sequence / Batch Render, ``operators/render_persp.py``): a Sun (strength ``lights.power_factor('SUN')`` =
pi x Maya intensity 1, no shadow, angle 0) named ``defaultLight``, parented to the render camera with the Maya turn
as its local rotation (so an animated camera keeps it), marked by the ID property ``allset_default_light``, and
removed with its data when the render ends (EXEC: right after; INVOKE: a timer waits for the render job). Stray
marked lights (an undo step pushed while a render ran, a file saved during a render) are removed after undo / redo /
load. The kit Outliner, defaultLightSet and the Light Editor skip it; a marked light never counts as a scene light.

Blender's own render keys (step 214, #120): every ``bpy.app.handlers.render_*`` callback of a UI (INVOKE) render
runs on the render job's thread, not the main one ([O] from m1_threads.py, factory 4.5.4 GUI:
render_init / render_pre / render_post / render_complete all off ``threading.main_thread()``), and the engine builds
its depsgraph there, so a render_init handler can't add the light safely: no handler fallback. Instead Maya mode wraps
the render keys with ``allset.render_blender``: its invoke adds the light on the main thread, then starts
``render.render`` INVOKE (the Render window opens as with F12; [O] same file: the wrapped render lit the cube 0.66 at
the centre, the plain one 0.0, and the timer removed the light once the job ended). Keys (keymap/maya_patches.py
``patch_blender_render``): F12 = Render Image and Ctrl+F12 = Render Animation in "Screen" (Blender's default keys,
whose own items are switched off in Maya mode; F12 over a 3D View in object / edit-mesh mode or over a UV Editor stays
Maya's UV selection mask, a Maya default hotkey), Ctrl+Alt+Enter = IC's Render Animation. Blender's top bar Render >
Render Image / Render Animation (the kit's "Blender Render" menu) still runs ``render.render`` itself: no light.
"""

import math

import bpy
from bpy.app.handlers import persistent
from bpy.props import BoolProperty

from .. import results
from . import lights

MARK = "allset_default_light"
NAME = "defaultLight"
TURN = math.radians(25.0)        # Maya: view axis turned 25 deg up (about X), then 25 deg left (about Y)
MAYA_INTENSITY = 1.0
_pending = {"timer": None}


def is_default_light(ob):
    return ob is not None and bool(ob.get(MARK))


def scene_has_lights(scene):
    """Maya: any light node turns the default light off, hidden or not."""
    return any(ob.type == 'LIGHT' and not is_default_light(ob) for ob in scene.objects)


def needed(scene):
    return bool(getattr(scene, "allset_default_light", True)) and not scene_has_lights(scene)


def add(scene, camera):
    """The default light for a render of ``scene`` through ``camera``, or None when the scene doesn't need one."""
    if camera is None or not needed(scene):
        return None
    remove_all()
    data = bpy.data.lights.new(NAME, 'SUN')
    data[MARK] = True
    data.energy = MAYA_INTENSITY * lights.power_factor('SUN', 0, 1.0)
    data.angle = 0.0
    data.use_shadow = False
    ob = bpy.data.objects.new(NAME, data)
    ob[MARK] = True
    ob.hide_select = True
    scene.collection.objects.link(ob)
    ob.parent = camera                      # matrix_parent_inverse stays identity: local = camera space
    ob.rotation_mode = 'XYZ'
    ob.rotation_euler = (-TURN, -TURN, 0.0)  # R = Ry(-25) Rx(-25): +Z (toward the light) -> up and left
    return ob


def remove_all():
    """Every marked default light (objects and their light data); number of objects removed."""
    n = 0
    for ob in [o for o in bpy.data.objects if o.get(MARK)]:
        data = ob.data
        bpy.data.objects.remove(ob)
        n += 1
        if data is not None and data.users == 0:
            bpy.data.lights.remove(data)
    for data in [d for d in bpy.data.lights if d.get(MARK) and d.users == 0]:
        bpy.data.lights.remove(data)
    return n


# --- after an INVOKE render: wait for the render job, then clean up ----------------------------------------------

_after = []      # callables run once the render job is over (default light removal, Test Resolution restore)


def _render_running():
    try:
        return bpy.app.is_job_running('RENDER')
    except (TypeError, ValueError):
        return False


def _tick():
    if _render_running():
        return 0.2
    _pending["timer"] = None
    finish()
    return None


def finish():
    """Run the end-of-render work now (also from unregister)."""
    while _after:
        func = _after.pop(0)
        try:
            func()
        except (ReferenceError, RuntimeError):
            pass
    remove_all()


def after_render(func=None):
    """Clean up when the running (INVOKE) render job ends; ``func`` runs first."""
    if func is not None:
        _after.append(func)
    if _pending["timer"] is None or not bpy.app.timers.is_registered(_tick):
        _pending["timer"] = _tick
        bpy.app.timers.register(_tick, first_interval=0.2, persistent=True)    # survives a file load


@persistent
def _stray(*_args):
    if not _render_running() and not _after:
        remove_all()


# --- Blender's render keys with the default light (step 214) -----------------------------------------------------

class ALLSET_OT_render_blender(bpy.types.Operator):
    """Render with Blender's own render command (F12 / Ctrl+F12 in Maya mode), lit by Maya's default light while the
    scene has no lights"""
    bl_idname = "allset.render_blender"
    bl_label = "Render"
    bl_options = set()            # no Adjust Last Operation (step 147)

    animation: BoolProperty(name="Animation", default=False, options={'SKIP_SAVE'})
    use_viewport: BoolProperty(name="Use 3D Viewport", default=False, options={'SKIP_SAVE'})

    @classmethod
    def poll(cls, context):
        return context.scene is not None

    def _props(self):
        return {"animation": self.animation, "use_viewport": self.use_viewport}

    def invoke(self, context, event):
        if context.window is None or bpy.app.background:
            return self.execute(context)
        if _render_running():          # a render is already on: its light stays, Blender reports the busy render
            return bpy.ops.render.render('INVOKE_DEFAULT', **self._props())
        scene = context.scene
        lit = add(scene, scene.camera) is not None
        try:
            res = bpy.ops.render.render('INVOKE_DEFAULT', **self._props())
        except RuntimeError as ex:
            remove_all()
            self.report({'ERROR'}, str(ex))
            return {'CANCELLED'}
        if lit:
            if 'CANCELLED' in res or not _render_running():
                remove_all()
            else:
                after_render()
        return {'FINISHED'} if 'CANCELLED' not in res else {'CANCELLED'}

    def execute(self, context):
        scene = context.scene
        add(scene, scene.camera)
        try:
            res = bpy.ops.render.render('EXEC_DEFAULT', **self._props())
        except RuntimeError as ex:
            self.report({'ERROR'}, str(ex))
            return {'CANCELLED'}
        finally:
            remove_all()
        if 'CANCELLED' in res:
            return {'CANCELLED'}
        if scene.camera is not None:
            results.result(self, f"Rendered through {scene.camera.name}")
        return {'FINISHED'}


HANDLERS = ("undo_post", "redo_post", "load_post")


def register():
    bpy.utils.register_class(ALLSET_OT_render_blender)
    bpy.types.Scene.allset_default_light = BoolProperty(
        name="Enable Default Light", default=True,
        description="Maya Enable Default Light: a scene without lights renders lit by a light from the camera "
                    "(Maya Workflow Kit renders and render keys only)")
    for name in HANDLERS:
        getattr(bpy.app.handlers, name).append(_stray)


def unregister():
    for name in HANDLERS:
        lst = getattr(bpy.app.handlers, name)
        if _stray in lst:
            lst.remove(_stray)
    if _pending["timer"] is not None and bpy.app.timers.is_registered(_tick):
        bpy.app.timers.unregister(_tick)
    _pending["timer"] = None
    try:
        finish()
    except AttributeError:          # restricted context (no bpy.data) while Blender quits
        _after.clear()
    del bpy.types.Scene.allset_default_light
    bpy.utils.unregister_class(ALLSET_OT_render_blender)
