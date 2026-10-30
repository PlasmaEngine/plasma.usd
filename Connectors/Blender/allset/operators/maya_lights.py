# SPDX-License-Identifier: GPL-3.0-or-later
"""Maya Ambient Light and Volume Light on Blender point lights (step 214, issue #120).

Maya (Maya Software):
- ambientLight: intensity 1, colour 1, Ambient Shade s 0.45, no shadows at factory render settings.
  v = I x colour x max(0, (1 - s) + s cos t), t = angle between the normal and the direction to the light's
  *position*; no distance falloff, rotation / scale ignored, NOT multiplied by the material's Diffuse, no specular.
- volumeLight (Sphere, Outward, factory ramp): v = I x colour x diffuse x (1 - r/R) x max(0, cos toward the centre),
  0 for r >= R, R = the sphere radius = the transform's scale (scale 1 = 1 Maya unit), Decay Rate ignored, no 1/r^2.

Blender stand-ins (approximations delegated by the main lane, errors measured by tests/test_step214_lights.py):
- Ambient Light = a point light at the object carrying the directional part s x I (No Decay: the Cycles Light Falloff
  Constant output of operators/lights.py; EEVEE / the viewport 1/r^2 matched at 10 units like every kit light),
  specular 0, no shadow; plus the flat part (1 - s) x I x colour added to the World for non-camera rays ("Maya
  Ambient" Background + Light Path Is Camera Ray, so the black camera background stays). Both parts are divided by
  material.MAYA_DIFFUSE (0.8): the kit's lambert family carries colour x diffuse in Base Color (step 214) while Maya's
  ambient light skips Diffuse — exact for every factory lambert-family material. Error: where (1 - s) + s cos t < 0
  Maya clamps the sum, Blender adds a clamped Lambert to a flat part (max error = min(s, 1 - s) x I x colour, at
  t = 180 degrees); Cycles shadows the World part (ambient occlusion) where Maya never shadows it.
- Volume Light = a point light at the centre. Cycles: exact — Light Falloff Constant whose Strength is
  max(0, 1 - Ray Length / R) (Light Path Ray Length in a light shader = the distance from the shading point to the
  light, measured step 214). EEVEE / the viewport ignore light nodes: 1/r^2 cut at R by Custom Distance (EEVEE
  attenuates by (1 - (r/R)^4)^2 inside the cutoff), matched to Maya at r = R/2.
Maya intensity is kept in an ID property of the light (``INTENSITY_PROP``): the power can't hold it (s may be 0).
"""

import math

import bpy
from bpy.props import FloatProperty

from .. import compat
from . import lights, units

KIND_PROP = lights.KIND_PROP           # "ambientLight" / "volumeLight" on the Light data
AMBIENT, VOLUME = "ambientLight", "volumeLight"
KINDS = {'AMBIENT_LIGHT': AMBIENT, 'VOLUME_LIGHT': VOLUME}
INTENSITY_PROP = "allset_maya_intensity"
SHADE_PROP = "allset_ambient_shade"
MAYA_SHADE = 0.45                      # ambientShade factory value
# EEVEE volume fit: I x A x (R/r)^2 x (1 - (r/R)^4)^2 = 0.5 at r = R/2 (Maya's 1 - r/R there)
VOLUME_A = 0.5 / (4.0 * (1.0 - 1.0 / 16.0) ** 2)

# volume light nodes (Cycles)
V_FALLOFF, V_PATH, V_RATIO, V_LEFT, V_SCALE = ("Maya Volume Falloff", "Maya Volume Ray Length", "Maya Volume r/R",
                                              "Maya Volume 1 - r/R", "Maya Volume Strength")
V_NODES = (V_FALLOFF, V_PATH, V_RATIO, V_LEFT, V_SCALE)
V_ON = "allset_volume_nodes"           # the kit turned the light's nodes on
# world nodes (ambient flat part)
W_BG, W_PATH, W_NOT_CAMERA, W_ADD = "Maya Ambient", "Maya Ambient Ray", "Maya Ambient Not Camera", "Maya Ambient Add"
W_NODES = (W_BG, W_PATH, W_NOT_CAMERA, W_ADD)
W_MADE = "allset_ambient_world"        # the kit made this World (no World in the scene)
W_ON = "allset_ambient_world_nodes"    # the kit turned the World's nodes on


def kind(light):
    """"ambientLight" / "volumeLight" for a kit Maya light, else None."""
    if not isinstance(light, bpy.types.Light):
        return None
    v = light.get(KIND_PROP)
    return v if v in (AMBIENT, VOLUME) else None


def _k(scene=None):
    scene = scene or getattr(bpy.context, "scene", None)
    return units.maya_unit(scene) if scene is not None else 1.0


def _diffuse():
    from . import material
    return material.MAYA_DIFFUSE


def intensity(light):
    v = light.get(INTENSITY_PROP)
    return float(v) if isinstance(v, (int, float)) else 1.0


def shade(light):
    v = light.get(SHADE_PROP)
    return max(0.0, min(1.0, float(v))) if isinstance(v, (int, float)) else MAYA_SHADE


def _object_of(light, scene=None):
    objs = scene.objects if scene is not None else bpy.data.objects
    return next((ob for ob in objs if ob.data == light), None)


def radius_m(light, scene=None, ob=None):
    """Volume radius in metres: the largest world scale of the light's object x maya_unit()."""
    scene = scene or getattr(bpy.context, "scene", None)
    ob = ob if ob is not None else _object_of(light, scene)
    s = max((abs(c) for c in ob.matrix_world.to_scale()), default=1.0) if ob is not None else 1.0
    return max(1e-6, s * _k(scene))


def lit(ob):
    """The light contributes (Light Editor Enable = hide_render + hide_viewport, Maya visibility)."""
    return not (ob.hide_render or ob.hide_viewport)


# --- light data ------------------------------------------------------------------------------------------------------

def _set(obj, attr, value):
    """Write only on change (handlers run on every depsgraph update)."""
    cur = getattr(obj, attr)
    try:
        same = abs(cur - value) < 1e-9 * max(1.0, abs(value))
    except TypeError:
        same = cur == value
    if not same:
        setattr(obj, attr, value)


def _volume_nodes(light, r_m):
    if compat.ensure_light_nodes(light):
        light[V_ON] = True
    tree = light.node_tree
    if tree is None:
        return
    emission = next((nd for nd in tree.nodes if nd.type == 'EMISSION'), None)
    if emission is None:
        return
    old = tree.nodes.get(lights.FALLOFF_NODE)     # lights.maya_defaults' No Decay node: the volume has its own
    if old is not None:
        tree.nodes.remove(old)
    n = {name: tree.nodes.get(name) for name in V_NODES}
    if any(v is None for v in n.values()):
        for v in n.values():
            if v is not None:
                tree.nodes.remove(v)
        x, y = emission.location.x, emission.location.y
        n[V_FALLOFF] = tree.nodes.new('ShaderNodeLightFalloff')
        n[V_PATH] = tree.nodes.new('ShaderNodeLightPath')
        for name in (V_RATIO, V_LEFT, V_SCALE):
            n[name] = tree.nodes.new('ShaderNodeMath')
        for i, name in enumerate((V_FALLOFF, V_SCALE, V_LEFT, V_RATIO, V_PATH)):
            n[name].name = n[name].label = name
            n[name].location = (x - 220 * (i + 1), y)
        n[V_RATIO].operation = 'DIVIDE'
        n[V_LEFT].operation, n[V_LEFT].use_clamp = 'SUBTRACT', True
        n[V_LEFT].inputs[0].default_value = 1.0
        n[V_SCALE].operation = 'MULTIPLY'
        n[V_FALLOFF].inputs["Smooth"].default_value = 0.0
        tree.links.new(n[V_PATH].outputs["Ray Length"], n[V_RATIO].inputs[0])
        tree.links.new(n[V_RATIO].outputs[0], n[V_LEFT].inputs[1])
        tree.links.new(n[V_LEFT].outputs[0], n[V_SCALE].inputs[0])
        tree.links.new(n[V_SCALE].outputs[0], n[V_FALLOFF].inputs["Strength"])
        tree.links.new(n[V_FALLOFF].outputs["Constant"], emission.inputs["Strength"])
    # Cycles: P x S / (4 pi^2) = I (1 - r/R) with P = 4 pi^2 A R^2 I  ->  S = (1 - r/R) / (A R^2)
    _set(n[V_RATIO].inputs[1], "default_value", r_m)
    _set(n[V_SCALE].inputs[1], "default_value", 1.0 / (VOLUME_A * r_m * r_m))


def _no_decay_nodes(light, k):
    """lights.py's No Decay node, written only when missing or stale (a write tags the light: handler loop)."""
    tree = light.node_tree
    node = tree.nodes.get(lights.FALLOFF_NODE) if tree is not None else None
    want = lights.falloff_strength(0, k)
    if (node is None or not node.outputs["Constant"].is_linked
            or abs(node.inputs["Strength"].default_value - want) > 1e-9 * max(1.0, want)):
        lights._falloff_nodes(light, 0, k)


def apply_light(light, scene=None, ob=None):
    """Power / nodes / EEVEE cutoff of one kit Maya light from its Maya values."""
    k = _k(scene)
    i = max(0.0, intensity(light))
    what = kind(light)
    if what == AMBIENT:
        # directional part s x I, no decay (lights.py No Decay rule), / Maya diffuse (Maya's ambient skips it)
        _set(light, "energy", i * shade(light) / _diffuse() * lights.power_factor('POINT', 0, k))
        _no_decay_nodes(light, k)
    elif what == VOLUME:
        r_m = radius_m(light, scene, ob)
        _set(light, "energy", 4.0 * math.pi ** 2 * VOLUME_A * r_m * r_m * i)
        if hasattr(light, "use_custom_distance"):
            _set(light, "use_custom_distance", True)
            _set(light, "cutoff_distance", r_m)
        _volume_nodes(light, r_m)


def set_intensity(light, value):
    light[INTENSITY_PROP] = max(0.0, float(value))
    apply_light(light)
    sync_world(bpy.context.scene)


def defaults(light, k):
    """A new Maya Ambient / Volume Light."""
    light.shadow_soft_size = 0.0           # a point (Maya measures from the light's position / the volume centre)
    light.use_shadow = False               # no shadows at Maya's factory render settings (delegated for volume)
    light.color = (1.0, 1.0, 1.0)
    light[lights.DECAY_PROP] = 0
    light[INTENSITY_PROP] = 1.0
    if kind(light) == AMBIENT:
        light[SHADE_PROP] = MAYA_SHADE
        light.specular_factor = 0.0        # Maya's ambient light has no specular
    apply_light(light)


# --- World (ambient flat part) ---------------------------------------------------------------------------------------

def _ambient_sum(scene):
    """Σ colour x I x (1 - s) / diffuse over the scene's lit ambient lights (None: there is none)."""
    total, any_ = [0.0, 0.0, 0.0], False
    for ob in scene.objects:
        if ob is None or ob.type != 'LIGHT' or kind(ob.data) != AMBIENT:
            continue
        any_ = True
        if not lit(ob):
            continue
        f = intensity(ob.data) * (1.0 - shade(ob.data)) / _diffuse()
        for c in range(3):
            total[c] += ob.data.color[c] * f
    return total if any_ else None


def _output(tree):
    outs = [nd for nd in tree.nodes if nd.type == 'OUTPUT_WORLD']
    return next((nd for nd in outs if nd.is_active_output), outs[0] if outs else None)


def _install(scene):
    world = scene.world
    if world is None:
        world = bpy.data.worlds.new("World")      # Blender's default name; Maya renders a black background
        world[W_MADE] = True
        scene.world = world
        compat.ensure_world_nodes(world)
        bg = next((nd for nd in world.node_tree.nodes if nd.type == 'BACKGROUND'), None)
        if bg is not None:
            bg.inputs["Color"].default_value = (0.0, 0.0, 0.0, 1.0)
    elif compat.ensure_world_nodes(world):
        world[W_ON] = True
        bg = next((nd for nd in world.node_tree.nodes if nd.type == 'BACKGROUND'), None)
        if bg is not None and not bg.inputs["Color"].is_linked:
            bg.inputs["Color"].default_value = (*world.color, 1.0)
    tree = world.node_tree
    if tree.nodes.get(W_ADD) is not None:
        return tree
    out = _output(tree)
    if out is None:
        out = tree.nodes.new('ShaderNodeOutputWorld')
    surface = out.inputs["Surface"]
    original = surface.links[0].from_socket if surface.is_linked else None
    x, y = out.location.x, out.location.y
    bg = tree.nodes.new('ShaderNodeBackground')
    ray = tree.nodes.new('ShaderNodeLightPath')
    nc = tree.nodes.new('ShaderNodeMath')
    add = tree.nodes.new('ShaderNodeAddShader')
    for node, name, loc in ((bg, W_BG, (x - 220, y - 200)), (ray, W_PATH, (x - 660, y - 300)),
                            (nc, W_NOT_CAMERA, (x - 440, y - 300)), (add, W_ADD, (x - 40, y + 120))):
        node.name = node.label = name
        node.location = loc
    nc.operation = 'SUBTRACT'
    nc.inputs[0].default_value = 1.0
    tree.links.new(ray.outputs["Is Camera Ray"], nc.inputs[1])
    tree.links.new(nc.outputs[0], bg.inputs["Strength"])
    if original is not None:
        tree.links.new(original, add.inputs[0])
    tree.links.new(bg.outputs[0], add.inputs[1])
    tree.links.new(add.outputs[0], surface)
    return tree


def remove_world_nodes(world, scene=None):
    """Take the kit's ambient nodes out of ``world`` (original link back); drop a kit-made World."""
    tree = world.node_tree if world is not None else None
    if tree is None:
        return
    add = tree.nodes.get(W_ADD)
    if add is None and not any(tree.nodes.get(n) for n in W_NODES):
        return
    original = add.inputs[0].links[0].from_socket if add is not None and add.inputs[0].is_linked else None
    targets = [lk.to_socket for lk in add.outputs[0].links] if add is not None else []
    for name in W_NODES:
        node = tree.nodes.get(name)
        if node is not None:
            tree.nodes.remove(node)
    if original is not None:
        for sock in targets:
            tree.links.new(original, sock)
    if world.get(W_ON):
        del world[W_ON]
        compat.release_world_nodes(world)
    if world.get(W_MADE):
        for sc in bpy.data.scenes:
            if sc.world == world:
                sc.world = None
        if world.users == 0:
            bpy.data.worlds.remove(world)


def sync_world(scene):
    """World ambient = the sum of the scene's ambient lights' flat parts; removed when none is left."""
    if scene is None:
        return
    total = _ambient_sum(scene)
    if total is None:
        if scene.world is not None:
            remove_world_nodes(scene.world, scene)
        return
    tree = _install(scene)
    bg = tree.nodes.get(W_BG)
    col = bg.inputs["Color"]
    want = (*total, 1.0)
    if any(abs(a - b) > 1e-7 for a, b in zip(col.default_value, want)):
        col.default_value = want


def sync(scene):
    """Every kit Maya light of ``scene``: power / nodes (radius, intensity) and the World ambient."""
    if scene is None:
        return
    for ob in scene.objects:
        if ob is not None and ob.type == 'LIGHT' and kind(ob.data) is not None:
            apply_light(ob.data, scene, ob)
    sync_world(scene)


# --- handlers --------------------------------------------------------------------------------------------------------

_busy = [False]


def _guarded(scene):
    if _busy[0]:
        return
    _busy[0] = True
    try:
        sync(scene)
    finally:
        _busy[0] = False


# The sync adds / removes World datablocks, node trees and scene.world: doing that inside depsgraph_update_post /
# frame_change_post crashed Blender 5.2.3 on the next relations build (std::bad_alloc in DepsgraphNodeBuilder, 2 of 3
# runs of test_step214_lights, step 229). The handlers only queue the scene (by name: no ID kept across ticks) and a
# 0 s timer syncs it outside the depsgraph evaluation; ``flush`` does it at once (operators, tests).
_pending = set()


def flush():
    """Sync every queued scene now."""
    names = list(_pending)
    _pending.clear()
    for name in names:
        sc = bpy.data.scenes.get(name)
        if sc is not None:
            _guarded(sc)


def _tick():
    flush()
    return None


def _queue(scene):
    if scene is None:
        return
    _pending.add(scene.name)
    if not bpy.app.timers.is_registered(_tick):
        bpy.app.timers.register(_tick, first_interval=0.0)


@bpy.app.handlers.persistent
def _on_depsgraph(scene, depsgraph=None):
    if depsgraph is not None and not any(
            isinstance(u.id, (bpy.types.Object, bpy.types.Light, bpy.types.Scene, bpy.types.Collection))
            for u in depsgraph.updates):
        return
    _queue(scene)


def _frame_values(scene):
    """Frame change: the keyable values now (a render job runs no timers between frames) — light energy and the
    existing ambient World colour; anything structural (nodes, a World to add or remove) goes to the queue."""
    k = _k(scene)
    for ob in scene.objects:
        if ob is None or ob.type != 'LIGHT':
            continue
        light, what = ob.data, kind(ob.data)
        i = max(0.0, intensity(light))
        if what == AMBIENT:
            _set(light, "energy", i * shade(light) / _diffuse() * lights.power_factor('POINT', 0, k))
        elif what == VOLUME:
            r_m = radius_m(light, scene, ob)
            _set(light, "energy", 4.0 * math.pi ** 2 * VOLUME_A * r_m * r_m * i)
    tree = scene.world.node_tree if scene.world is not None else None
    bg = tree.nodes.get(W_BG) if tree is not None else None
    total = _ambient_sum(scene)
    if bg is not None and total is not None:
        col = bg.inputs["Color"]
        want = (*total, 1.0)
        if any(abs(a - b) > 1e-7 for a, b in zip(col.default_value, want)):
            col.default_value = want


@bpy.app.handlers.persistent
def _on_frame(scene, *_a):
    if scene is not None and not _busy[0]:
        _busy[0] = True
        try:
            _frame_values(scene)
        finally:
            _busy[0] = False
    _queue(scene)


@bpy.app.handlers.persistent
def _on_load(*_a):
    for sc in bpy.data.scenes:
        _guarded(sc)


@bpy.app.handlers.persistent
def _on_undo(*_a):
    scene = getattr(bpy.context, "scene", None)
    if scene is not None:
        _guarded(scene)


_HANDLERS = (("depsgraph_update_post", _on_depsgraph), ("frame_change_post", _on_frame), ("load_post", _on_load),
             ("undo_post", _on_undo), ("redo_post", _on_undo))


# --- properties ------------------------------------------------------------------------------------------------------

def _get_shade(self):
    return shade(self)


def _set_shade(self, value):
    self[SHADE_PROP] = max(0.0, min(1.0, float(value)))
    apply_light(self)
    sync_world(bpy.context.scene)


def register():
    bpy.types.Light.allset_ambient_shade = FloatProperty(
        name="Ambient Shade", min=0.0, max=1.0, precision=3, get=_get_shade, set=_set_shade,
        description="Maya Ambient Shade: 0 lights every side alike, 1 lights only the sides facing the light")
    for attr, fn in _HANDLERS:
        lst = getattr(bpy.app.handlers, attr)
        if fn not in lst:
            lst.append(fn)


def unregister():
    for attr, fn in _HANDLERS:
        lst = getattr(bpy.app.handlers, attr)
        if fn in lst:
            lst.remove(fn)
    if bpy.app.timers.is_registered(_tick):
        bpy.app.timers.unregister(_tick)
    _pending.clear()
    for world in list(bpy.data.worlds):
        remove_world_nodes(world)
    if hasattr(bpy.types.Light, "allset_ambient_shade"):
        del bpy.types.Light.allset_ambient_shade
