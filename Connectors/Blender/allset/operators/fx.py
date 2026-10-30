# SPDX-License-Identifier: GPL-3.0-or-later
"""Maya FX: nucleus, nParticles emitters, nCloth, colliders, rigid bodies, nCache (docs/steps/step_68.md).

Simulation scale (decision 10, PLAN W4): Maya's nucleus (spaceScale 1, gravity 9.8 in a cm scene, checked
in step 48) simulates one scene unit as one metre, so a 1 cm cloth moves like a 1 m one. Blender simulates
in metres, so the kit scales what it creates by ``g = maya_unit() / Space Scale`` (step 68 「계획」, measured):

- gravity 9.8 x g along Maya's gravity direction (Y-up -> Z-up: X->X, Y->Z, Z->-Y, like operators/joints.py);
- lengths (cloth collision distances, collider thickness, particle size / speed, rigid margin) x maya_unit();
- cloth stiffness and damping x g: with gravity g times smaller the springs must be g times weaker for the same
  sag (a pinned 1 cm cloth sagged -0.944 instead of -0.705 cloth widths without it, -0.703 with it);
- nucleus substeps (Maya 3) -> cloth quality 5 and rigid body substeps 10 (Blender's defaults) at 3.

Blender has no nucleus node: ``Scene.allset_nucleus`` holds Maya's values and ``apply`` writes them to the scene
gravity, the rigid body world and every cloth; the Attribute Editor shows it as the "nucleus1" tab (ui/ae_layout.py
NUCLEUS). Kit FX operators create it the first time, like Maya's first nObject creates nucleus1
(Maya behaviour); before that the scene gravity is left alone.
"""

import bpy
from bpy.props import BoolProperty, EnumProperty, FloatProperty, FloatVectorProperty, IntProperty

from .. import results
from . import naming, units

MAYA_GRAVITY = 9.8          # nucleus1.gravity (step 48)
MAYA_SUBSTEPS = 3           # nucleus1.subSteps (step 48)
CLOTH_QUALITY = 5           # Blender cloth quality default = Maya's 3 substeps
RIGID_SUBSTEPS = 10         # Blender rigid body world substeps default
CLOTH_G_PROP = "allset_cloth_g"
STIFFNESS = ("tension_stiffness", "compression_stiffness", "shear_stiffness", "bending_stiffness")
DAMPING = ("tension_damping", "compression_damping", "shear_damping", "bending_damping")
# Maya nParticle emitter defaults [K]: rate 100 particles/s, omni speed 1 unit/s; Blender particle size 0.05 per unit.
EMIT_RATE = 100.0
EMIT_SPEED = 1.0
PARTICLE_SIZE = 0.05
PARTICLE_DISPLAY = 0.1      # Blender display_size default, per unit


def maya_to_blender(v):
    """Maya Y-up vector -> Blender Z-up (X->X, Y->Z, Z->-Y)."""
    x, y, z = v
    return (x, -z, y)


# --- nucleus ---------------------------------------------------------------------------------

def _update(self, context):
    if self.exists:
        apply(context.scene)


class AllsetNucleus(bpy.types.PropertyGroup):
    exists: BoolProperty(name="nucleus1", description="Kit FX made the nucleus (Maya's first nObject creates nucleus1)")
    gravity: FloatProperty(name="Gravity", default=MAYA_GRAVITY, min=0.0, soft_max=100.0, update=_update,
                           description="Maya nucleus gravity in metres per second squared (one unit = one metre "
                                       "at Space Scale 1)")
    gravity_direction: FloatVectorProperty(name="Gravity Direction", size=3, default=(0.0, -1.0, 0.0), update=_update,
                                           description="Maya axes (Y up): (0, -1, 0) = down")
    space_scale: FloatProperty(name="Space Scale", default=1.0, min=1e-4, soft_max=10.0, update=_update,
                               description="Maya nucleus Space Scale: 1 simulates one unit as one metre (Maya's "
                                           "default), 0.01 simulates centimetres at their real size")
    substeps: IntProperty(name="Substeps", default=MAYA_SUBSTEPS, min=1, max=100, update=_update,
                          description="Maya nucleus substeps: cloth quality and rigid body substeps follow them")


def nucleus(scene=None):
    return (scene or bpy.context.scene).allset_nucleus


def sim_scale(scene=None):
    """g: Blender simulation scale for Maya units (maya_unit / Space Scale)."""
    scene = scene or bpy.context.scene
    return units.maya_unit(scene) / nucleus(scene).space_scale


def gravity_vector(scene=None):
    scene = scene or bpy.context.scene
    nuc = nucleus(scene)
    d = maya_to_blender(nuc.gravity_direction)
    length = sum(c * c for c in d) ** 0.5 or 1.0
    g = nuc.gravity * sim_scale(scene)
    return tuple(c / length * g for c in d)


def cloth_quality(substeps):
    return max(1, round(CLOTH_QUALITY * substeps / MAYA_SUBSTEPS))


def rigid_substeps(substeps):
    return max(1, round(RIGID_SUBSTEPS * substeps / MAYA_SUBSTEPS))


def cloth_modifiers(scene):
    for ob in scene.objects:
        for md in ob.modifiers:
            if md.type == 'CLOTH':
                yield ob, md


def _set_cloth_g(ob, md, g):
    """Rescale this cloth's stiffness / damping from the g it was made with to ``g``."""
    old = ob.get(CLOTH_G_PROP, 1.0)
    if abs(old - g) < 1e-12:
        return
    st = md.settings
    for attr in STIFFNESS + DAMPING:
        setattr(st, attr, getattr(st, attr) * g / old)
    ob[CLOTH_G_PROP] = g


def apply(scene):
    """Write the nucleus to Blender: scene gravity, rigid body world, every cloth (quality, stiffness)."""
    nuc = nucleus(scene)
    nuc["exists"] = True
    scene.use_gravity = True
    scene.gravity = gravity_vector(scene)
    world = scene.rigidbody_world
    if world is not None:
        world.substeps_per_frame = rigid_substeps(nuc.substeps)
    g = sim_scale(scene)
    for ob, md in cloth_modifiers(scene):
        md.settings.quality = cloth_quality(nuc.substeps)
        if CLOTH_G_PROP in ob:
            _set_cloth_g(ob, md, g)


# --- creating -----------------------------------------------------------------------------------

def _playback(scene):
    if scene.use_preview_range:
        return scene.frame_preview_start, scene.frame_preview_end
    return scene.frame_start, scene.frame_end


def setup_particles(settings, scene):
    """Maya nParticle emitter defaults in Maya units: omni speed 1, rate 100/s for the playback range."""
    k = units.maya_unit(scene)
    start, end = _playback(scene)
    fps = scene.render.fps / (scene.render.fps_base or 1.0)
    settings.frame_start, settings.frame_end = start, end
    settings.count = max(1, round(EMIT_RATE * (end - start + 1) / fps))
    settings.lifetime = end - start + 2                  # Maya: lifespan Live forever
    settings.emit_from = 'VERT'
    settings.use_emit_random = True
    settings.normal_factor = 0.0
    settings.factor_random = EMIT_SPEED * k              # omni: random directions
    settings.particle_size = PARTICLE_SIZE * k
    settings.display_method = 'DOT'                      # Maya nParticle "Points"
    settings.display_size = PARTICLE_DISPLAY * k         # the viewport draws dots this size (0.1 m = 10 units in cm)
    settings.render_type = 'HALO'


def _new_particles(ob, scene, name):
    md = ob.modifiers.new(name, 'PARTICLE_SYSTEM')
    psys = md.particle_system
    psys.name = name
    psys.settings.name = name
    setup_particles(psys.settings, scene)
    start, end = _playback(scene)
    psys.point_cache.frame_start, psys.point_cache.frame_end = start, end
    return psys


def _particle_name():
    taken = {s.name for s in bpy.data.particles} | {p.name for o in bpy.data.objects for p in o.particle_systems}
    return f"nParticle{naming.next_number('nParticle', taken)}"


def setup_cloth(ob, md, scene):
    k = units.maya_unit(scene)
    cs = md.collision_settings
    cs.distance_min *= k
    cs.self_distance_min *= k
    md.settings.quality = cloth_quality(nucleus(scene).substeps)
    ob[CLOTH_G_PROP] = 1.0
    _set_cloth_g(ob, md, sim_scale(scene))
    start, end = _playback(scene)
    md.point_cache.frame_start, md.point_cache.frame_end = start, end


def setup_collider(ob, scene):
    k = units.maya_unit(scene)
    col = ob.collision
    col.thickness_outer *= k
    col.thickness_inner *= k


def setup_rigid(ob, scene):
    k = units.maya_unit(scene)
    rb = ob.rigid_body
    rb.collision_margin *= k
    rb.deactivate_linear_velocity *= k


def _meshes(context):
    return [ob for ob in context.selected_objects if ob.type == 'MESH']


def _numbered(base, taken):
    return f"{base}{naming.next_number(base, taken)}"


class ALLSET_OT_fx_emitter(bpy.types.Operator):
    """Create a point emitter at the origin: a one-vertex mesh emitter1 with the particles nParticle1
    (Maya nParticles > Create Emitter; Blender particles come from a mesh)"""
    bl_idname = "allset.fx_emitter"
    bl_label = "Create Emitter"
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        scene = context.scene
        name = _numbered("emitter", {o.name for o in bpy.data.objects})
        me = bpy.data.meshes.new(name.replace("emitter", "emitterShape"))
        me.from_pydata([(0.0, 0.0, 0.0)], [], [])
        ob = bpy.data.objects.new(name, me)
        ob.hide_render = True
        context.collection.objects.link(ob)
        _new_particles(ob, scene, _particle_name())
        for o in context.selected_objects:
            o.select_set(False)
        ob.select_set(True)
        context.view_layer.objects.active = ob
        apply(scene)
        results.result(self, f"Created {name}: Blender particles come from a mesh, so the emitter is a one-point mesh")
        return {'FINISHED'}


class ALLSET_OT_fx_emit_from_object(bpy.types.Operator):
    """Emit particles from the selected meshes' vertices (Maya nParticles > Emit from Object)"""
    bl_idname = "allset.fx_emit_from_object"
    bl_label = "Emit from Object"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return context.mode == 'OBJECT' and bool(_meshes(context))

    def execute(self, context):
        for ob in _meshes(context):
            _new_particles(ob, context.scene, _particle_name())
        apply(context.scene)
        return {'FINISHED'}


class ALLSET_OT_fx_ncloth(bpy.types.Operator):
    """Make the selected meshes nCloth: Blender cloth scaled to Maya units and the nucleus
    (Maya nCloth > Create nCloth)"""
    bl_idname = "allset.fx_ncloth"
    bl_label = "Create nCloth"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return context.mode == 'OBJECT' and bool(_meshes(context))

    def execute(self, context):
        scene = context.scene
        made = []
        for ob in _meshes(context):
            if any(md.type == 'CLOTH' for md in ob.modifiers):
                continue
            taken = {md.name for o in bpy.data.objects for md in o.modifiers}
            md = ob.modifiers.new(_numbered("nCloth", taken), 'CLOTH')
            setup_cloth(ob, md, scene)
            made.append(md.name)
        apply(scene)
        results.result(self, f"nCloth: {', '.join(made) or 'already cloth'} (collision distances in Maya units)")
        return {'FINISHED'}


class ALLSET_OT_fx_remove_ncloth(bpy.types.Operator):
    """Remove the cloth from the selected meshes (Maya nCloth > Remove nCloth)"""
    bl_idname = "allset.fx_remove_ncloth"
    bl_label = "Remove nCloth"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return any(md.type == 'CLOTH' for ob in context.selected_objects for md in ob.modifiers)

    def execute(self, context):
        for ob in context.selected_objects:
            for md in [m for m in ob.modifiers if m.type == 'CLOTH']:
                ob.modifiers.remove(md)
            if CLOTH_G_PROP in ob:
                del ob[CLOTH_G_PROP]
        return {'FINISHED'}


class ALLSET_OT_fx_passive(bpy.types.Operator):
    """Make the selected meshes collide with cloth and particles (Maya nCloth > Create Passive Collider)"""
    bl_idname = "allset.fx_passive"
    bl_label = "Create Passive Collider"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return context.mode == 'OBJECT' and bool(_meshes(context))

    def execute(self, context):
        for ob in _meshes(context):
            if any(md.type == 'COLLISION' for md in ob.modifiers):
                continue
            taken = {md.name for o in bpy.data.objects for md in o.modifiers}
            ob.modifiers.new(_numbered("nRigid", taken), 'COLLISION')
            setup_collider(ob, context.scene)
        apply(context.scene)
        return {'FINISHED'}


class ALLSET_OT_fx_rigid(bpy.types.Operator):
    """Make the selected meshes rigid bodies in Maya units (Maya Fields/Solvers > Create Active / Passive Rigid Body)"""
    bl_idname = "allset.fx_rigid"
    bl_label = "Create Rigid Body"
    bl_options = {'REGISTER', 'UNDO'}

    type: EnumProperty(items=(('ACTIVE', "Active", ""), ('PASSIVE', "Passive", "")), name="Type")

    @classmethod
    def poll(cls, context):
        return context.mode == 'OBJECT' and bool(_meshes(context))

    def execute(self, context):
        obs = [ob for ob in _meshes(context) if ob.rigid_body is None]
        if obs:
            active = context.view_layer.objects.active
            if active not in obs:
                context.view_layer.objects.active = obs[0]
            bpy.ops.rigidbody.objects_add(type=self.type)
            context.view_layer.objects.active = active
            for ob in obs:
                if ob.rigid_body is not None:
                    setup_rigid(ob, context.scene)
        apply(context.scene)
        return {'FINISHED'}


# --- nCache ------------------------------------------------------------------------------------------

def point_caches(ob):
    """(owner, point cache) of an object's cloth / soft body / particle systems."""
    for md in ob.modifiers:
        if md.type in {'CLOTH', 'SOFT_BODY'}:
            yield md, md.point_cache
    for psys in ob.particle_systems:
        yield psys, psys.point_cache


def fluid_domains(obs):
    return [ob for ob in obs for md in ob.modifiers if md.type == 'FLUID' and md.fluid_type == 'DOMAIN']


def _sim_node_objects(obs):
    """Objects with a Geometry Nodes modifier that has a simulation zone."""
    out = []
    for ob in obs:
        for md in ob.modifiers:
            tree = getattr(md, "node_group", None) if md.type == 'NODES' else None
            if tree is not None and any(n.bl_idname == 'GeometryNodeSimulationOutput' for n in tree.nodes):
                out.append(ob)
                break
    return out


def cache_targets(context):
    return list(context.selected_objects) or list(context.scene.objects)


def bake_point_caches(context, obs, bake=True):
    """Bake (or free) the point caches of ``obs`` and, if any of them is a rigid body, the rigid body world's."""
    count = 0
    caches = [pc for ob in obs for _owner, pc in point_caches(ob)]
    world = context.scene.rigidbody_world
    if world is not None and world.point_cache is not None and any(ob.rigid_body for ob in obs):
        caches.append(world.point_cache)
    for pc in caches:
        with context.temp_override(point_cache=pc):
            if bake:
                bpy.ops.ptcache.bake(bake=True)
            else:
                bpy.ops.ptcache.free_bake()
        count += 1
    sims = _sim_node_objects(obs)
    if sims:
        with context.temp_override(selected_objects=sims, active_object=sims[0], object=sims[0]):
            if bake:
                bpy.ops.object.simulation_nodes_cache_bake(selected=True)
            else:
                bpy.ops.object.simulation_nodes_cache_delete(selected=True)
        count += len(sims)
    return count


def bake_fluids(context, obs, bake=True):
    count = 0
    for ob in fluid_domains(obs):
        with context.temp_override(object=ob, active_object=ob):
            if bake:
                bpy.ops.fluid.bake_all()
            else:
                bpy.ops.fluid.free_all()
        count += 1
    return count


class ALLSET_OT_ncache(bpy.types.Operator):
    """Bake or delete the simulation caches of the selection, or of the whole scene when nothing is selected
    (Maya nCache; Blender keeps a cache per object, saved with the file)"""
    bl_idname = "allset.ncache"
    bl_label = "nCache"
    bl_options = {'REGISTER'}

    action: EnumProperty(items=(('CREATE', "Create New Cache", ""), ('DELETE', "Delete Cache", "")), name="Action")
    kind: EnumProperty(items=(('NOBJECT', "nObject", "Cloth, particles, soft and rigid bodies, simulation nodes"),
                              ('FLUID', "Maya Fluid", "Mantaflow domains"),
                              ('ALL', "All", "")), name="Kind", default='NOBJECT')

    def execute(self, context):
        obs = cache_targets(context)
        bake = self.action == 'CREATE'
        n = 0
        if self.kind in {'NOBJECT', 'ALL'}:
            n += bake_point_caches(context, obs, bake)
        if self.kind in {'FLUID', 'ALL'}:
            n += bake_fluids(context, obs, bake)
        if not n:
            self.report({'WARNING'}, "No simulation to cache in the selection")
            return {'CANCELLED'}
        results.result(self, f"{'Baked' if bake else 'Deleted'} {n} cache(s)")
        return {'FINISHED'}


# --- step 109: FX shelf buttons Blender can do (docs/steps/step_109.md 「계획」 3) ---------------------------
# Maya 3D Fluid Container with Emitter (Maya 2022): fluid1 10 x 10 x 10 units, base resolution 10, at the
# origin, with the omni emitter fluidEmitter1 (max distance 1) at its centre. Blender: a Mantaflow domain of the
# same size around a radius-1 emitter mesh (Blender flows come from meshes).
FLUID_SIZE, FLUID_RESOLUTION, FLUID_EMITTER_RADIUS = 10.0, 10, 1.0


class ALLSET_OT_fluid_container_emitter(bpy.types.Operator):
    """Create a 3D fluid container with an emitter at the origin (Maya FX shelf: 3D Fluid Container with
    Emitter; a Mantaflow smoke domain around an emitter mesh)"""
    bl_idname = "allset.fluid_container_emitter"
    bl_label = "3D Fluid Container with Emitter"
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        if context.mode != 'OBJECT':
            bpy.ops.object.mode_set(mode='OBJECT')
        k = units.maya_unit(context.scene)
        bpy.ops.mesh.primitive_ico_sphere_add(subdivisions=2, radius=FLUID_EMITTER_RADIUS * k,
                                              location=(0.0, 0.0, 0.0), align='WORLD')
        emitter = context.view_layer.objects.active
        naming.apply_maya_name(emitter, "fluidEmitter")
        before = set(bpy.data.objects)
        res = bpy.ops.allset.quick_smoke(style='SMOKE')
        if res != {'FINISHED'}:
            return res
        domain = next((ob for ob in bpy.data.objects if ob not in before), None)
        if domain is not None:
            domain.location = (0.0, 0.0, 0.0)
            domain.scale = (FLUID_SIZE * k / 2,) * 3      # Blender's domain cube is 2 BU wide
            naming.apply_maya_name(domain, "fluid")
            md = next((m for m in domain.modifiers if m.type == 'FLUID'), None)
            if md is not None and md.domain_settings is not None:
                md.domain_settings.resolution_max = FLUID_RESOLUTION
        return {'FINISHED'}


class ALLSET_OT_fluid_collide(bpy.types.Operator):
    """Make the selected meshes collide with fluids (Maya Fluids > Make Collide: a Mantaflow effector)"""
    bl_idname = "allset.fluid_collide"
    bl_label = "Make Collide"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return context.mode == 'OBJECT' and bool(_meshes(context))

    def execute(self, context):
        for ob in _meshes(context):
            md = next((m for m in ob.modifiers if m.type == 'FLUID'), None)
            if md is None:
                md = ob.modifiers.new("Fluid", 'FLUID')
            if md.fluid_type == 'NONE':
                md.fluid_type = 'EFFECTOR'
        return {'FINISHED'}


class ALLSET_OT_ncloth_display(bpy.types.Operator):
    """Show the selected cloth objects' input mesh (cloth off in the viewport) or the simulated current mesh
    (Maya nCloth > Display Input Mesh / Display Current Mesh)"""
    bl_idname = "allset.ncloth_display"
    bl_label = "Display nCloth Mesh"
    bl_options = {'REGISTER', 'UNDO'}

    mesh: bpy.props.EnumProperty(items=(('INPUT', "Input Mesh", ""), ('CURRENT', "Current Mesh", "")),
                                 options={'SKIP_SAVE'})

    @classmethod
    def poll(cls, context):
        return any(md.type == 'CLOTH' for ob in context.selected_objects for md in ob.modifiers)

    def execute(self, context):
        for ob in context.selected_objects:
            for md in ob.modifiers:
                if md.type == 'CLOTH':
                    md.show_viewport = self.mesh == 'CURRENT'
        return {'FINISHED'}


classes = (AllsetNucleus, ALLSET_OT_fx_emitter, ALLSET_OT_fx_emit_from_object, ALLSET_OT_fx_ncloth,
           ALLSET_OT_fx_remove_ncloth, ALLSET_OT_fx_passive, ALLSET_OT_fx_rigid, ALLSET_OT_ncache,
           ALLSET_OT_fluid_container_emitter, ALLSET_OT_fluid_collide, ALLSET_OT_ncloth_display)


def register_props():
    bpy.types.Scene.allset_nucleus = bpy.props.PointerProperty(type=AllsetNucleus)


def unregister_props():
    del bpy.types.Scene.allset_nucleus
