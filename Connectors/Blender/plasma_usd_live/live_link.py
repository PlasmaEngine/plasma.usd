"""The live link between this Blender and one USD live session.

This Blender's edits go into its own layer in the session folder (``blender-<user>.usda``); the other participants'
edits show up here. Two tiers carry them:
  * the Plasma editor's live hub (see hub.py): layers travel whole over a local socket as soon as they change, without
    saving. While connected, the hub decides the content of the layers it relays and their files are not reloaded.
    When the session owner commits (folds every layer into the asset and clears them), all layers are read again.
  * the session files: the layer file is saved shortly after each change, and the other layers are reloaded when their
    files change. This is all there is without a hub, and what remains after the connection drops.

Synced both ways:
  * object transforms (written as ``xformOp:transform``, like Plasma does)
  * visibility: hiding an object makes its prim invisible, unhiding makes it visible and active again;
    a prim another participant hides or deactivates is hidden here.
  * material values: Base Color, Metallic, Roughness and Alpha of the materials imported from the stage (materials.py)
Synced from Blender:
  * deleting a linked object deactivates its prim (active = false), as Plasma does. Nothing is deactivated after an
    undo or redo, while the linked collection is gone, or when more than half of the linked objects vanish at once.
    Undoing the delete makes the prim active again. A prim another layer makes active again cannot be re-created here
    and is reported in the panel.
  * a mesh object added to the linked collection, or parented to a linked object, becomes a Mesh prim in this Blender's
    layer (meshes.py), under the prim of its parent object or the stage's default prim. Its transform, visibility and
    geometry follow it, and deleting it removes the prim from the layer again.
Meshes, lights and cameras from the stage are imported once and their data is not synced. Other objects stay local,
including those that were already in the file when joining. Edits made while not connected are not sent.

The imported objects live in a collection tagged with the stage, so a saved .blend joins again without a new import.
"""

import os
import re
import time
import traceback

import bpy
from bpy.app.handlers import persistent
from mathutils import Matrix

from . import hub, materials, meshes, sessions

bpy.utils.expose_bundled_modules()
from pxr import Gf, Sdf, Tf, Usd, UsdGeom, Vt  # noqa: E402

PRIM_PATH_PROP = "usd_prim_path"
STAGE_PROP = "usd_live_stage"
CONVERSION_PROP = "usd_live_conversion"
TICK_SECONDS = 0.25  # file polling, hidden state, deleted and added objects
HUB_TICK_SECONDS = 0.05  # the timer's rate while a hub connection is open
SAVE_DELAY_SECONDS = 0.2

# Blender appends ".001" and so on to clashing names; USD prim names cannot contain a '.'.
NAME_SUFFIX = re.compile(r"\.\d{3,}$")

# PrimState.state
PRESENT = "present"
ABSENT = "absent"  # the object vanished in a way that is not taken as a delete; the prim is kept
DELETED = "deleted"  # the object was deleted: the prim was deactivated, or removed if this Blender added it

_active = None


def active_link():
    return _active


def to_blender(m):
    """Gf.Matrix4d (row vectors) to a mathutils Matrix (column vectors)."""
    if isinstance(m, tuple):
        m = m[0]
    return Matrix([[m[i][j] for j in range(4)] for i in range(4)]).transposed()


def to_usd(m):
    return Gf.Matrix4d(*[m[j][i] for i in range(4) for j in range(4)])


def is_close(a, b):
    return all(abs(a[i][j] - b[i][j]) <= 1e-4 + 1e-5 * abs(b[i][j]) for i in range(4) for j in range(4))


def is_static_xformable(prim):
    """Animated transforms come in through Blender's importer as animation and are not synced."""
    return prim.IsA(UsdGeom.Xformable) and not UsdGeom.Xformable(prim).TransformMightBeTimeVarying()


def main_view_layer():
    wm = bpy.context.window_manager
    if wm is not None and wm.windows:
        return wm.windows[0].view_layer
    return bpy.context.view_layer


def find_view3d():
    wm = bpy.context.window_manager
    if wm is None:
        return None, None, None
    for window in wm.windows:
        for area in window.screen.areas:
            if area.type == 'VIEW_3D':
                for region in area.regions:
                    if region.type == 'WINDOW':
                        return window, area, region
    return None, None, None


def redraw_view3d():
    wm = bpy.context.window_manager
    if wm is None:
        return
    for window in wm.windows:
        for area in window.screen.areas:
            if area.type == 'VIEW_3D':
                area.tag_redraw()


def find_linked_collection(stage_path):
    for collection in bpy.data.collections:
        stage = collection.get(STAGE_PROP)
        if stage and sessions.same_path(stage, stage_path):
            return collection
    return None


def opinion_paths(layer):
    """Paths of the prim specs in a layer that hold opinions of their own, not only children."""
    paths = set()

    def visit(path):
        if path.IsPrimPath():
            spec = layer.GetPrimAtPath(path)
            if spec.specifier != Sdf.SpecifierOver or len(spec.properties) or len(spec.ListInfoKeys()) > 1:
                paths.add(str(path))

    layer.Traverse(Sdf.Path.absoluteRootPath, visit)
    return paths


def in_scope(path, scope):
    """True when the prim path or one of its ancestors is in scope."""
    while path:
        if path in scope:
            return True
        path = path.rpartition("/")[0]
    return False


def parent_depth(obj):
    depth = 0
    while obj.parent is not None:
        obj = obj.parent
        depth += 1
    return depth


class PrimState:
    """What this Blender last knew about one linked prim. Transforms are USD-space local matrices."""

    __slots__ = ("name", "usd_local", "blender_local", "hidden", "overruled", "state", "added", "geometry_dirty",
                 "reported")

    def __init__(self, name, hidden=False, state=PRESENT, added=False):
        self.name = name  # name of the object that drives the prim; copies of it are unlinked
        self.usd_local = None  # composed value last applied to the object
        self.blender_local = None  # object's value when last synced
        self.hidden = hidden
        self.overruled = False  # a stronger layer decides the transform; reported once
        self.state = state
        self.added = added  # this Blender created the prim for an object added here
        self.geometry_dirty = False  # an added object's mesh changed and is written on the next tick
        self.reported = False  # a deleted prim is active again and that was reported


class LiveLink:
    def __init__(self, stage_path, session, participant):
        self.stage_path = os.path.abspath(stage_path).replace("\\", "/")
        self.session_name = session
        self.session_dir = sessions.session_folder(self.stage_path, session)
        self.session_root = self.session_dir + "/session.usda"
        self.participant = participant
        self.layer_path = self.session_dir + "/" + participant + ".usda"
        self.tool = "Blender " + bpy.app.version_string
        self.stage = None
        self.layer = None
        self.session_layer = None
        self.conversion = Matrix.Identity(4)  # Blender world = conversion @ USD world
        self.conversion_inv = Matrix.Identity(4)
        self.prims = {}
        self.mtimes = {}
        self.failed_reloads = {}
        self.dirty_since = None
        self.presence = None
        self.hub = hub.HubClient(self)
        self.materials = materials.MaterialSync(self)
        self.added_uids = {}  # session_uid of an added object -> its prim path; finds it when an undo drops its tag
        self.local_uids = set()  # objects without a prim when joining stay local
        self.after_undo = False
        self.applied = {}  # path_key -> usda last applied from the hub
        self.unplaced = {}  # path_key -> hub layer message for a layer the stage does not use yet
        self.next_file_tick = 0.0
        self.log = []
        self.last_note = None
        self.note_repeats = 0

    # -- session ---------------------------------------------------------------------------------------------

    def start(self):
        """Joins the session, imports the stage (or adopts an earlier import in this file) and starts syncing."""
        global _active
        if _active is not None:
            _active.leave()

        asset_layer = self.join()

        collection = find_linked_collection(self.stage_path)
        if collection is not None:
            self.adopt(collection)
        else:
            self.import_scene(asset_layer)

        self.pull()
        self.local_uids = {obj.session_uid for obj in bpy.data.objects if PRIM_PATH_PROP not in obj}

        _active = self
        handlers = bpy.app.handlers
        for handler_list, handler in ((handlers.depsgraph_update_post, on_depsgraph_update),
                                      (handlers.undo_post, on_undo_redo),
                                      (handlers.redo_post, on_undo_redo)):
            if handler not in handler_list:
                handler_list.append(handler)
        if not bpy.app.background:
            bpy.app.timers.register(self.tick, first_interval=HUB_TICK_SECONDS)

        self.presence = sessions.PresenceWriter({
            "version": 1,
            "tool": self.tool,
            "participant": self.participant,
            "stage": self.stage_path,
            "session": self.session_name,
            "sessionRoot": self.session_root,
            "document": bpy.data.filepath,
            "pid": os.getpid(),
        })
        self.presence.refresh(force=True)

    def leave(self, redraw=True):
        """Saves pending edits, says goodbye to the hub and stops syncing. Without redraw it touches no Blender
        data, for Python's exit when Blender has freed it already."""
        global _active
        if _active is self:
            _active = None
        if self.layer is not None:
            self.save_if_due(force=True)
        self.hub.close()
        if self.presence is not None:
            self.presence.remove()
            self.presence = None
        self.stage = None
        self.layer = None
        self.session_layer = None
        self.prims.clear()
        if redraw:
            redraw_view3d()

    def join(self):
        os.makedirs(self.session_dir, exist_ok=True)

        asset_layer = Sdf.Layer.FindOrOpen(self.stage_path)
        if asset_layer is None:
            raise RuntimeError("Cannot open " + self.stage_path)

        session_path = self.session_root
        if os.path.exists(session_path):
            session = Sdf.Layer.FindOrOpen(session_path)
        else:
            session = Sdf.Layer.CreateNew(session_path)
            session.documentation = ("Live session root. Each participant authors only into its own sublayer; "
                                     "the asset is the weakest layer.")
            if asset_layer.defaultPrim:
                session.defaultPrim = asset_layer.defaultPrim
            for key in ("metersPerUnit", "upAxis"):
                if asset_layer.pseudoRoot.HasInfo(key):
                    session.pseudoRoot.SetInfo(key, asset_layer.pseudoRoot.GetInfo(key))
            session.subLayerPaths.append(os.path.relpath(self.stage_path, self.session_dir).replace("\\", "/"))

        own_path = self.layer_path
        if os.path.exists(own_path):
            self.layer = Sdf.Layer.FindOrOpen(own_path)
        else:
            self.layer = Sdf.Layer.CreateNew(own_path)
            self.layer.documentation = "Opinions authored by {} during live session '{}'.".format(
                self.participant, self.session_name)
            self.layer.Save()

        # Whoever started the session is strongest; later participants sit just above the asset.
        own_rel = "./" + self.participant + ".usda"
        if own_rel not in list(session.subLayerPaths):
            session.subLayerPaths.insert(max(len(session.subLayerPaths) - 1, 0), own_rel)
        session.Save()

        self.session_layer = session
        self.stage = Usd.Stage.Open(session, Usd.Stage.LoadAll)
        self.stage.SetEditTarget(Usd.EditTarget(self.layer))
        self.note("Joined '{}' as {}".format(self.session_name, self.participant))
        return asset_layer

    def participant_keys(self):
        """path_key of every participant layer session.usda lists (all its sublayers but the asset)."""
        return {sessions.path_key(self.session_layer.ComputeAbsolutePath(p))
                for p in list(self.session_layer.subLayerPaths)[:-1]}

    def participants(self):
        """(layer name, online) of every participant, strongest first, from the session files and presence."""
        if self.session_layer is None:
            return []
        online = set()
        for session in sessions.list_open_sessions():
            if sessions.same_path(session.session_root, self.session_root):
                online = {p.name for p in session.participants}
        names = [os.path.splitext(os.path.basename(p))[0] for p in list(self.session_layer.subLayerPaths)[:-1]]
        return [(name, name in online or name == self.participant) for name in names]

    # -- import ----------------------------------------------------------------------------------------------

    def import_scene(self, asset_layer):
        # A fresh, untouched startup file only holds the default cube, light and camera.
        if not bpy.data.filepath and not bpy.data.is_dirty:
            for obj in list(bpy.data.objects):
                bpy.data.objects.remove(obj, do_unlink=True)

        objects_before = set(bpy.data.objects.keys())
        collections_before = set(bpy.data.collections.keys())
        materials_before = set(bpy.data.materials.keys())

        window, area, region = find_view3d()
        options = dict(
            filepath=self.stage_path,
            relative_path=False,
            import_visible_only=False,
            merge_parent_xform=False,
            import_textures_mode='IMPORT_NONE',
            create_collection=True,
            set_frame_range=False,
            property_import_mode='NONE',
        )
        if window is not None:
            with bpy.context.temp_override(window=window, area=area, region=region):
                bpy.ops.wm.usd_import(**options)
        else:
            bpy.ops.wm.usd_import(**options)

        imported = [o for o in bpy.data.objects if o.name not in objects_before]
        imported_names = {o.name for o in imported}

        for obj in imported:
            parts = []
            o = obj
            while o is not None and o.name in imported_names:
                parts.append(NAME_SUFFIX.sub("", o.name))
                o = o.parent
            path = "/" + "/".join(reversed(parts))
            obj[PRIM_PATH_PROP] = path
            self.prims[path] = PrimState(obj.name)

        # Blender puts the axis and unit conversion on root objects. Measure it against the asset's own values,
        # since the session layers may already move a root prim.
        asset_stage = Usd.Stage.Open(asset_layer, Usd.Stage.LoadNone)
        for obj in imported:
            if obj.parent is not None:
                continue
            prim = asset_stage.GetPrimAtPath(obj[PRIM_PATH_PROP])
            if prim and prim.IsA(UsdGeom.Xformable):
                local = to_blender(UsdGeom.Xformable(prim).GetLocalTransformation())
                self.conversion = obj.matrix_basis @ local.inverted()
                self.conversion_inv = self.conversion.inverted()
                break

        new_collections = [c for c in bpy.data.collections if c.name not in collections_before]
        for collection in new_collections:
            if any(o.name in imported_names for o in collection.all_objects):
                collection[STAGE_PROP] = self.stage_path
                collection[CONVERSION_PROP] = [v for row in self.conversion for v in row]
                break

        self.materials.map_imported([m for m in bpy.data.materials if m.name not in materials_before])

        self.note("Imported {} objects, {} materials".format(len(imported), len(self.materials.materials)))

        if window is not None:
            space = area.spaces.active
            space.shading.type = 'SOLID'
            space.shading.color_type = 'TEXTURE'
            space.clip_end = max(space.clip_end, 1000.0)
            with bpy.context.temp_override(window=window, area=area, region=region):
                bpy.ops.view3d.view_all(center=False)

    def adopt(self, collection):
        values = list(collection.get(CONVERSION_PROP, []))
        if len(values) == 16:
            self.conversion = Matrix([values[0:4], values[4:8], values[8:12], values[12:16]])
            self.conversion_inv = self.conversion.inverted()

        # Objects of the import, and objects added in an earlier session wherever they are now.
        view_layer = main_view_layer()
        members = {o.name for o in collection.all_objects}
        for obj in bpy.data.objects:
            path = obj.get(PRIM_PATH_PROP)
            if not path or path in self.prims:
                continue
            added = self.is_own_def(path)
            if obj.name not in members and not added:
                continue
            try:
                hidden = obj.hide_get(view_layer=view_layer)
            except RuntimeError:
                hidden = False
            self.prims[path] = PrimState(obj.name, hidden, added=added)
            if added:
                self.added_uids[obj.session_uid] = path

        # Prims deactivated here earlier, so another layer activating them again is reported.
        def visit(path):
            if path.IsPrimPath() and str(path) not in self.prims:
                spec = self.layer.GetPrimAtPath(path)
                if spec is not None and spec.HasInfo("active") and not spec.GetInfo("active"):
                    self.prims[str(path)] = PrimState(path.name, True, state=DELETED)

        self.layer.Traverse(Sdf.Path.absoluteRootPath, visit)
        self.materials.adopt()

        self.note("Using the {} objects already in this file".format(
            sum(st.state == PRESENT for st in self.prims.values())))

    # -- USD -> Blender --------------------------------------------------------------------------------------

    def usd_parent_world(self, path):
        """USD world matrix of a prim's parent; None for prims at the root."""
        parent = Sdf.Path(path).GetParentPath()
        if parent.IsAbsoluteRootPath():
            return None
        prim = self.stage.GetPrimAtPath(parent)
        if not prim:
            return None
        return to_blender(UsdGeom.XformCache().GetLocalToWorldTransform(prim))

    def follows_parent(self, obj, path):
        """True when the object's Blender parent drives its prim's parent, so their local matrices are the same."""
        parent = obj.parent
        return parent is not None and parent.get(PRIM_PATH_PROP) == str(Sdf.Path(path).GetParentPath())

    def blender_local(self, obj, path):
        """The object's transform as the local matrix of its prim, in USD space."""
        if self.follows_parent(obj, path):
            return obj.matrix_parent_inverse @ obj.matrix_basis
        world = self.conversion_inv @ (obj.matrix_basis if obj.parent is None else obj.matrix_world)
        parent_world = self.usd_parent_world(path)
        return world if parent_world is None else parent_world.inverted_safe() @ world

    def set_blender_local(self, obj, path, local):
        if self.follows_parent(obj, path):
            obj.matrix_basis = obj.matrix_parent_inverse.inverted() @ local
            return
        parent_world = self.usd_parent_world(path)
        world = self.conversion @ (local if parent_world is None else parent_world @ local)
        if obj.parent is None:
            obj.matrix_basis = world
        else:
            obj.matrix_world = world

    def linked_objects(self, scope=None):
        """(object, prim path, state) of the linked objects in a scene; with a scope, only those whose prim or an
        ancestor of it is in the scope."""
        in_scene = {obj.session_uid for scene in bpy.data.scenes for obj in scene.objects}
        if scope is None:
            objects = list(bpy.data.objects)
        else:
            objects = [self.object_of(path, st) for path, st in self.prims.items()
                       if st.state == PRESENT and in_scope(path, scope)]
        for obj in objects:
            if obj is None or obj.session_uid not in in_scene:
                continue
            st = self.resolve(obj)
            if st is not None and st.state == PRESENT:
                yield obj, obj[PRIM_PATH_PROP], st

    def pull(self, scope=None):
        """Applies the composed stage to the linked objects and materials whose prims changed since the last pull.
        A scope (prim paths) limits the objects looked at to those prims and their descendants."""
        view_layer = main_view_layer()
        moved = 0

        for obj, path, st in self.linked_objects(scope):
            prim = self.stage.GetPrimAtPath(path)
            alive = bool(prim) and prim.IsActive()

            if alive and is_static_xformable(prim):
                local = to_blender(UsdGeom.Xformable(prim).GetLocalTransformation())
                if st.usd_local is None or not is_close(local, st.usd_local):
                    st.usd_local = local
                    if not is_close(self.blender_local(obj, path), local):
                        self.set_blender_local(obj, path, local)
                        moved += 1
                    st.blender_local = self.blender_local(obj, path)

            hidden = not alive
            if alive and prim.IsA(UsdGeom.Imageable):
                hidden = UsdGeom.Imageable(prim).ComputeVisibility() == UsdGeom.Tokens.invisible

            if hidden != st.hidden:
                st.hidden = hidden
                try:
                    obj.hide_set(hidden, view_layer=view_layer)
                except RuntimeError:
                    pass
                obj.hide_render = hidden

        if moved:
            self.note("Moved {} objects from the session".format(moved))

        for path, st in self.prims.items():
            if st.state != DELETED or st.added:
                continue
            prim = self.stage.GetPrimAtPath(path)
            active = bool(prim) and prim.IsActive()
            if active and not st.reported:
                self.note("{} is active again in the session; Blender cannot re-create the deleted object".format(
                    Sdf.Path(path).name))
            st.reported = active

        changed = self.materials.pull()
        if changed:
            self.note("Updated {} materials from the session".format(changed))

    def reload_changed_layers(self):
        """Reloads the layers whose files changed. Returns (changed, scope): the prims a pull has to look at, or None
        when a layer other than a participant's changed (the session root, the asset or what it uses) and everything
        has to be looked at."""
        changed = False
        scope = set()
        own = self.layer.identifier
        participants = self.participant_keys()

        for layer in self.stage.GetUsedLayers():
            if layer.anonymous or layer.identifier == own or not layer.realPath:
                continue
            if self.hub.is_driven(layer.realPath):
                continue
            try:
                mtime = os.path.getmtime(layer.realPath)
            except OSError:
                continue
            old = self.mtimes.get(layer.realPath)
            if old is None:
                self.mtimes[layer.realPath] = mtime
                continue
            if mtime == old:
                continue

            participant = sessions.path_key(layer.realPath) in participants
            before = opinion_paths(layer) if participant and scope is not None else set()

            # A file that fails to parse keeps its old time, so the next tick tries again. Forced: the time check above
            # already decided, and Sdf's own check compares whole seconds on Windows, so it would skip a second save
            # within the same second.
            try:
                layer.Reload(True)
            except Exception as e:
                if self.failed_reloads.get(layer.realPath) != mtime:
                    self.failed_reloads[layer.realPath] = mtime
                    self.note("Cannot read {}: {}".format(os.path.basename(layer.realPath), str(e).strip()[-120:]))
                continue

            self.mtimes[layer.realPath] = mtime
            self.failed_reloads.pop(layer.realPath, None)
            changed = True
            if not participant:
                scope = None
            elif scope is not None:
                scope |= before | opinion_paths(layer)
            self.note(os.path.basename(layer.realPath) + " changed")

        return changed, scope

    def reload_from_disk(self, include):
        """Reloads the used layers include(layer) picks from their files, whatever their file times say."""
        for layer in self.stage.GetUsedLayers():
            if layer.anonymous or not layer.realPath or not include(layer):
                continue
            try:
                layer.Reload(True)
            except Exception as e:
                self.note("Cannot read {}: {}".format(os.path.basename(layer.realPath), str(e).strip()[-120:]))
                continue
            try:
                self.mtimes[layer.realPath] = os.path.getmtime(layer.realPath)
            except OSError:
                pass
            self.failed_reloads.pop(layer.realPath, None)
            self.applied.pop(sessions.path_key(layer.realPath), None)

    # -- hub -------------------------------------------------------------------------------------------------

    def apply_hub_layers(self, messages):
        """Puts other participants' layers from the hub into the stage and pulls the result. They are never saved."""
        if self.stage is None:
            return
        own = sessions.path_key(self.layer_path)
        participants = self.participant_keys()
        applied = 0
        scope = set()  # prims holding opinions in the old or the new content of a changed layer

        for message in messages:
            key = sessions.path_key(message["layer"])
            if key == own:
                continue
            layer = None
            if key in participants:
                layer = next((layer for layer in self.stage.GetUsedLayers() if not layer.anonymous and layer.realPath
                              and sessions.path_key(layer.realPath) == key), None)
            if layer is None:
                # A participant that joined after this Blender read session.usda: applied once the file lists it.
                self.unplaced[key] = message
                continue
            self.unplaced.pop(key, None)
            if self.applied.get(key) == message["usda"]:
                continue
            before = opinion_paths(layer)
            try:
                ok = layer.ImportFromString(message["usda"])
            except Exception:
                ok = False
                traceback.print_exc()
            if not ok:
                self.note("Cannot read {} from the hub".format(os.path.basename(layer.realPath)))
                continue
            self.applied[key] = message["usda"]
            scope |= before | opinion_paths(layer)
            applied += 1

        if applied:
            self.pull(scope)

    def apply_unplaced(self):
        if self.unplaced and self.hub.live:
            self.apply_hub_layers(list(self.unplaced.values()))

    def on_hub_connected(self, protocol):
        if protocol != hub.PROTOCOL:
            self.note("The Plasma hub speaks protocol {}, this add-on {}".format(protocol, hub.PROTOCOL))
        self.note("Live via the Plasma hub")

    def on_hub_lost(self, reason, driven):
        """Back to the session files: the layers the hub drove are read from disk again."""
        self.unplaced.clear()
        if _active is not self or self.stage is None:
            return  # leaving anyway
        self.note("Plasma hub disconnected ({}); syncing through files".format(reason))
        self.reload_from_disk(lambda layer: sessions.path_key(layer.realPath) in driven)
        self.pull()

    def on_committed(self):
        """The owner folded every participant layer into the asset and cleared them: everything is read again."""
        if self.stage is None:
            return
        self.note("The session was committed to the asset; reloading all layers")
        self.dirty_since = None
        self.hub.layer_pending = False
        self.reload_from_disk(lambda layer: True)
        self.pull()

    def redraw(self):
        redraw_view3d()

    # -- Blender -> USD --------------------------------------------------------------------------------------

    def resolve(self, obj):
        """The state of the prim obj drives, or None. A copy of a linked object is unlinked here."""
        path = obj.get(PRIM_PATH_PROP)
        if path is None:
            return None
        st = self.prims.get(path)
        if st is None:
            return None
        if obj.name != st.name:
            owner = bpy.data.objects.get(st.name)
            if owner is not None and owner.get(PRIM_PATH_PROP) == path:
                del obj[PRIM_PATH_PROP]
                return None
            st.name = obj.name
        return st

    def object_of(self, path, st):
        """The object that drives a prim, also after it was renamed."""
        obj = bpy.data.objects.get(st.name)
        if obj is not None and obj.get(PRIM_PATH_PROP) == path:
            return obj
        for obj in bpy.data.objects:
            if obj.get(PRIM_PATH_PROP) == path and self.resolve(obj) is st:
                return obj
        return None

    def push_transform(self, obj, path, st):
        local = self.blender_local(obj, path)
        if st.blender_local is not None and is_close(local, st.blender_local):
            return
        st.blender_local = local

        prim = self.stage.GetPrimAtPath(path)
        if not prim or not is_static_xformable(prim):
            return

        prim.CreateAttribute("xformOp:transform", Sdf.ValueTypeNames.Matrix4d, False).Set(to_usd(local))
        UsdGeom.Xformable(prim).CreateXformOpOrderAttr().Set(Vt.TokenArray(["xformOp:transform"]))

        composed = to_blender(UsdGeom.Xformable(prim).GetLocalTransformation())
        st.usd_local = composed
        if not is_close(composed, local) and not st.overruled:
            st.overruled = True
            self.note("{}: a stronger layer decides its transform".format(prim.GetName()))

        self.mark_dirty()

    def push_visibility(self, path, st, hidden):
        st.hidden = hidden
        prim = self.stage.GetPrimAtPath(path)
        if not prim:
            return
        if not hidden and not prim.IsActive():
            prim.SetActive(True)
        if prim.IsA(UsdGeom.Imageable):
            UsdGeom.Imageable(prim).CreateVisibilityAttr().Set(
                UsdGeom.Tokens.invisible if hidden else UsdGeom.Tokens.inherited)
        self.note("{} {}".format(prim.GetName(), "hidden" if hidden else "shown"))
        self.mark_dirty()

    def poll_hidden(self):
        view_layer = main_view_layer()
        for obj, path, st in self.linked_objects():
            try:
                hidden = obj.hide_get(view_layer=view_layer)
            except RuntimeError:
                continue
            if hidden != st.hidden:
                self.push_visibility(path, st, hidden)

    def push_materials(self):
        if self.materials.push():
            self.mark_dirty()

    # -- deleted and added objects ---------------------------------------------------------------------------

    def is_own_def(self, path):
        spec = self.layer.GetPrimAtPath(path)
        return spec is not None and spec.specifier == Sdf.SpecifierDef

    def is_addable(self, obj, members):
        """A new mesh object in the linked collection, or parented to a linked object."""
        if obj.type != 'MESH' or obj.library is not None or obj.session_uid in self.local_uids or not obj.users_scene:
            return False
        if any(c.name in members for c in obj.users_collection):
            return True
        if obj.parent is None:
            return False
        st = self.resolve(obj.parent)
        return st is not None and st.state == PRESENT

    def scan_objects(self):
        """Handles linked objects that vanished from or came back to the file, and mesh objects to add to the stage."""
        after_undo, self.after_undo = self.after_undo, False
        collection = find_linked_collection(self.stage_path)
        members = set()
        if collection is not None:
            members = {collection.name} | {c.name for c in collection.children_recursive}

        found = {}
        candidates = []
        for obj in list(bpy.data.objects):
            if PRIM_PATH_PROP not in obj:
                path = self.added_uids.get(obj.session_uid)
                if path is not None and path in self.prims:
                    obj[PRIM_PATH_PROP] = path  # an undo went back to before the tag was set
            st = self.resolve(obj)
            if st is None:
                if PRIM_PATH_PROP not in obj and self.is_addable(obj, members):
                    candidates.append(obj)
                continue
            found[obj[PRIM_PATH_PROP]] = (obj, st)

        present = [path for path, st in self.prims.items() if st.state == PRESENT]
        vanished = [path for path in present if path not in found]
        edited = False

        if vanished:
            if collection is None or len(vanished) * 2 > len(present):
                for path in vanished:
                    self.prims[path].state = ABSENT
                self.note("{} linked objects vanished at once; the session keeps them".format(len(vanished))
                          if collection is not None else "The linked collection is gone; the session keeps its prims")
            else:
                removed = []
                for path in vanished:
                    st = self.prims[path]
                    if after_undo and not st.added:
                        st.state = ABSENT  # an undo is not a delete
                    else:
                        if self.delete(path, st):
                            removed.append(path)
                        edited = True
                self.release_children(removed)

        depsgraph = None
        for path, (obj, st) in found.items():
            if st.state == ABSENT:
                st.state = PRESENT
            elif st.state == DELETED:
                if depsgraph is None:
                    depsgraph = bpy.context.evaluated_depsgraph_get()
                self.restore(obj, path, st, depsgraph)
                edited = True

        if candidates:
            if depsgraph is None:
                depsgraph = bpy.context.evaluated_depsgraph_get()
            for obj in sorted(candidates, key=parent_depth):
                if PRIM_PATH_PROP not in obj:
                    self.add_object(obj, depsgraph)

        if edited:
            self.pull()

    def delete(self, path, st):
        """The object of a prim was deleted. Returns True when the prim was removed rather than deactivated."""
        name = Sdf.Path(path).name
        st.state = DELETED
        st.reported = False
        if st.added and self.is_own_def(path):
            self.stage.RemovePrim(path)
            self.remove_inert(Sdf.Path(path).GetParentPath())
            self.note("{} removed from the session".format(name))
            self.mark_dirty()
            return True
        prim = self.stage.GetPrimAtPath(path)
        if prim:
            prim.SetActive(False)
            self.note("{} deleted (deactivated)".format(name))
            self.mark_dirty()
        return False

    def release_children(self, removed):
        """Added objects below a removed prim lost their prim with it; the next scan adds them where they are now."""
        for root in removed:
            prefix = root + "/"
            for path in [p for p in self.prims if p.startswith(prefix)]:
                st = self.prims.pop(path)
                obj = bpy.data.objects.get(st.name)
                if obj is not None and obj.get(PRIM_PATH_PROP) == path:
                    del obj[PRIM_PATH_PROP]
                self.added_uids = {uid: p for uid, p in self.added_uids.items() if p != path}

    def restore(self, obj, path, st, depsgraph):
        """A deleted object came back, by an undo: its prim is active, or defined, again."""
        if st.added:
            prim = self.stage.GetPrimAtPath(path)
            if not prim or not prim.IsDefined():
                meshes.author_mesh(self.stage, path, obj, depsgraph)
                st.blender_local = None
                self.push_transform(obj, path, st)
        else:
            prim = self.stage.GetPrimAtPath(path)
            if prim:
                prim.ClearActive()
                self.remove_inert(path)
        st.state = PRESENT
        st.reported = False
        st.hidden = False
        self.note("{} restored".format(obj.name))
        self.mark_dirty()

    def remove_inert(self, path):
        """Removes the specs from path up that hold no opinions anymore."""
        path = Sdf.Path(path)
        while not path.IsAbsoluteRootPath():
            spec = self.layer.GetPrimAtPath(path)
            if spec is not None:
                if not spec.isInert:
                    break
                self.layer.ScheduleRemoveIfInert(spec)
            path = path.GetParentPath()

    def add_object(self, obj, depsgraph):
        """Defines a Mesh prim for a new object under the prim of its parent object, or the default prim."""
        parent_path = Sdf.Path.absoluteRootPath
        default_prim = self.stage.GetDefaultPrim()
        if default_prim:
            parent_path = default_prim.GetPath()
        if obj.parent is not None:
            parent_st = self.resolve(obj.parent)
            if parent_st is not None and parent_st.state == PRESENT:
                parent_path = Sdf.Path(obj.parent[PRIM_PATH_PROP])

        base = Tf.MakeValidIdentifier(obj.name)
        name = base
        index = 1
        while True:
            path = parent_path.AppendChild(name)
            if (str(path) not in self.prims and not self.stage.GetPrimAtPath(path)
                    and self.layer.GetPrimAtPath(path) is None):
                break
            name = "{}_{}".format(base, index)
            index += 1

        key = str(path)
        meshes.author_mesh(self.stage, path, obj, depsgraph)
        obj[PRIM_PATH_PROP] = key
        st = PrimState(obj.name, added=True)
        self.prims[key] = st
        self.added_uids[obj.session_uid] = key
        self.push_transform(obj, key, st)
        self.note("{} added as {}".format(obj.name, key))
        self.mark_dirty()

    def author_dirty_meshes(self):
        dirty = [(path, st) for path, st in self.prims.items() if st.geometry_dirty and st.state == PRESENT]
        if not dirty:
            return
        depsgraph = bpy.context.evaluated_depsgraph_get()
        for path, st in dirty:
            st.geometry_dirty = False
            obj = self.object_of(path, st)
            if obj is None or not self.stage.GetPrimAtPath(path):
                continue
            meshes.author_mesh(self.stage, path, obj, depsgraph)
            self.mark_dirty()

    # -- saving ----------------------------------------------------------------------------------------------

    def mark_dirty(self):
        if self.dirty_since is None:
            self.dirty_since = time.monotonic()
        self.hub.layer_changed()

    def save_if_due(self, force=False):
        if self.dirty_since is None:
            return
        if not force and time.monotonic() - self.dirty_since < SAVE_DELAY_SECONDS:
            return
        # The hub gets this state first, so its saved notice names the layer message the file holds. When the rate
        # limit holds the message back, the save waits for a later tick.
        self.hub.send_layer_if_due(force=force)
        if self.hub.layer_pending:
            return
        self.layer.Save()
        self.dirty_since = None
        self.hub.layer_saved()

    # -- loop ------------------------------------------------------------------------------------------------

    def tick(self):
        if _active is not self or self.stage is None:
            return None
        now = time.monotonic()
        try:
            self.hub.poll(now)
            if now >= self.next_file_tick:
                self.next_file_tick = now + TICK_SECONDS
                changed, scope = self.reload_changed_layers()
                if changed:
                    self.apply_unplaced()
                    self.pull(scope)
                self.poll_hidden()
                self.scan_objects()
                self.author_dirty_meshes()
                if self.presence is not None:
                    self.presence.refresh()
            self.save_if_due()
            self.hub.send_layer_if_due()
        except Exception as e:  # keep the timer alive; the panel shows the error
            traceback.print_exc()
            self.note("Error: {!r}".format(e))
        return self.hub.poll_interval(HUB_TICK_SECONDS if self.hub.state != hub.OFF else TICK_SECONDS)

    def note(self, text):
        stamp = time.strftime("%H:%M:%S ")
        if self.log and text == self.last_note:
            self.note_repeats += 1
            self.log[-1] = "{}{} (x{})".format(stamp, text, self.note_repeats)
        else:
            self.last_note = text
            self.note_repeats = 1
            self.log.append(stamp + text)
            del self.log[:-8]
            print("[USD Live]", text, flush=True)
        redraw_view3d()


@persistent
def on_depsgraph_update(scene, depsgraph):
    link = _active
    if link is None or link.stage is None:
        return
    try:
        shading = False
        for update in depsgraph.updates:
            if isinstance(update.id, bpy.types.Object):
                obj = update.id.original
                st = link.resolve(obj)
                if st is None or st.state != PRESENT:
                    continue
                if update.is_updated_transform:
                    link.push_transform(obj, obj[PRIM_PATH_PROP], st)
                if st.added and (update.is_updated_geometry or update.is_updated_shading):
                    st.geometry_dirty = True
            elif isinstance(update.id, (bpy.types.Material, bpy.types.ShaderNodeTree)):
                shading = True
        if shading:
            link.push_materials()
        link.hub.send_layer_if_due()
    except Exception as e:
        traceback.print_exc()
        link.note("Error: {!r}".format(e))


@persistent
def on_undo_redo(*_):
    """Objects an undo or redo takes away are not deleted from the session."""
    if _active is not None:
        _active.after_undo = True


@persistent
def on_load_pre(*_):
    """Another .blend replaces the linked objects, so the session is left first."""
    if _active is not None:
        _active.leave()


@persistent
def on_exit_pre(*_):
    """Blender quits: leave while its data still exists."""
    if _active is not None:
        _active.leave()


def leave_active():
    if _active is not None:
        _active.leave()
