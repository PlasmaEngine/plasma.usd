"""The live link between this 3ds Max and one USD live session.

3ds Max has no editable USD stage of its own, so the link works like the Blender connector's: the stage is imported
once as native nodes, each tagged with the path of its prim, and the link keeps the nodes and the session in step.
This 3ds Max's edits go into its own layer in the session folder (``max-<user>.usda``); the other participants' edits
show up on the nodes. Two tiers carry them:
  * the Plasma editor's live hub (see hub.py): layers travel whole over a local socket as soon as they change, without
    saving. While connected, the hub decides the content of the layers it relays and their files are not reloaded.
    When the session owner commits (folds every layer into the asset and clears them), all layers are read again.
  * the session files: the layer file is saved shortly after each change, and the other layers are reloaded when their
    files change. This is all there is without a hub, and what remains after the connection drops.

Synced both ways:
  * node transforms (written as ``xformOp:transform``, like Plasma does)
  * visibility: hiding a node makes its prim invisible, unhiding makes it visible and active again; a prim another
    participant hides or deactivates is hidden here.
Synced from 3ds Max:
  * deleting a linked node deactivates its prim (active = false), as Plasma does. Nothing is deactivated after an undo
    or redo, or when more than half of the linked nodes vanish at once. Undoing the delete makes the prim active again.
    A prim another layer makes active again cannot be re-created here and is reported in the panel.
  * a geometry node created while in the session becomes a Mesh prim in this 3ds Max's layer (meshes.py), under the
    prim of its parent node or the stage's default prim. Its transform, visibility and geometry follow it, and deleting
    it removes the prim from the layer again. Nodes that were in the scene before joining stay local.
Meshes, lights, cameras and materials from the stage are imported once and their data is not synced.

Everything 3ds Max specific goes through a host (max_host.MaxHost); this module needs only pxr, so it runs, and is
tested, without 3ds Max. Node ids are whatever the host uses (3ds Max node handles). Matrices are Gf.Matrix4d in row
vector form, which is also how 3ds Max's Matrix3 stores them: world = local * parent world.

The scene keeps a tag (the stage and the measured axis and unit conversion), so a saved .max file joins again without
a new import.
"""

import os
import time
import traceback

from pxr import Gf, Sdf, Tf, Usd, UsdGeom, Vt

from . import hub, meshes, sessions

TICK_SECONDS = 0.25  # file polling, hidden state, transforms
HUB_TICK_SECONDS = 0.05  # the timer's rate while a hub connection is open
SCAN_SECONDS = 1.0  # looking for deleted, restored and new nodes, unless the host says something changed
SAVE_DELAY_SECONDS = 0.2

# PrimState.state
PRESENT = "present"
ABSENT = "absent"  # the node vanished in a way that is not taken as a delete; the prim is kept
DELETED = "deleted"  # the node was deleted: the prim was deactivated, or removed if this 3ds Max added it

clock = hub.clock

_active = None


def active_link():
    return _active


def is_close(a, b):
    return all(abs(a[i][j] - b[i][j]) <= 1e-4 + 1e-5 * abs(b[i][j]) for i in range(4) for j in range(4))


def is_static_xformable(prim):
    """Animated transforms come in through the importer as animation and are not synced."""
    return prim.IsA(UsdGeom.Xformable) and not UsdGeom.Xformable(prim).TransformMightBeTimeVarying()


def local_transform(prim):
    m = UsdGeom.Xformable(prim).GetLocalTransformation()
    return m[0] if isinstance(m, tuple) else m


def depth(path):
    return path.count("/")


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


def axis_conversion(stage, meters_per_unit):
    """USD world to 3ds Max world from the stage's metadata, for a stage with no prim to measure it on: Y-up stages are
    turned Z-up the way the importer does it ((x, y, z) -> (x, -z, y)), and units scaled to the scene's."""
    scale = UsdGeom.GetStageMetersPerUnit(stage) / meters_per_unit if meters_per_unit > 0 else 1.0
    m = Gf.Matrix4d().SetScale(Gf.Vec3d(scale, scale, scale))
    if UsdGeom.GetStageUpAxis(stage) == UsdGeom.Tokens.y:
        m = m * Gf.Matrix4d().SetRotate(Gf.Rotation(Gf.Vec3d(1, 0, 0), 90.0))
    return m


class Host:
    """What the link needs from the application. max_host.MaxHost is the real one; the tests use a fake."""

    tool = "3ds Max"

    def document(self):
        """The open scene file, empty when it was never saved."""
        return ""

    def meters_per_unit(self):
        return 0.0254

    def read_tag(self):
        """The scene's link tag, {"stage": path, "conversion": 16 floats}, or None."""
        return None

    def write_tag(self, tag):
        pass

    def import_stage(self, path):
        """Imports a USD file as nodes; returns (id, name, parent id or None) of every node it made."""
        return []

    def nodes(self):
        """{id: prim path} of the scene's nodes that carry a prim path."""
        return {}

    def all_nodes(self):
        """Ids of all the scene's nodes."""
        return set()

    def exists(self, node):
        return False

    def name(self, node):
        return ""

    def parent(self, node):
        return None

    def tag_node(self, node, path):
        """Stores a prim path on a node; None takes it away."""

    def world(self, node):
        return Gf.Matrix4d(1)

    def set_world(self, node, m):
        pass

    def hidden(self, node):
        return False

    def set_hidden(self, node, hidden):
        pass

    def take_moved(self):
        """The nodes whose transform may have changed since the last call, or None when the host cannot tell, in which
        case every linked node is looked at."""
        return None

    def is_geometry(self, node):
        return False

    def geometry_key(self, node):
        """Something that changes when a node's geometry does; compared between scans."""
        return None

    def mesh(self, node):
        """meshes.MeshData of a geometry node, in the node's own space; None when it has none."""
        return None

    def start(self, link):
        """Starts calling link.tick() and telling the link about undo and scene changes."""

    def stop(self, link):
        pass

    def redraw(self):
        pass


class PrimState:
    """What this 3ds Max last knew about one linked prim. Transforms are USD-space local matrices."""

    __slots__ = ("node", "usd_local", "max_local", "hidden", "overruled", "state", "added", "geometry_key",
                 "reported")

    def __init__(self, node, hidden=False, state=PRESENT, added=False):
        self.node = node  # the node that drives the prim; copies of it are untagged
        self.usd_local = None  # composed value last applied to the node
        self.max_local = None  # node's value when last synced
        self.hidden = hidden
        self.overruled = False  # a stronger layer decides the transform; reported once
        self.state = state
        self.added = added  # this 3ds Max created the prim for a node added here
        self.geometry_key = None  # an added node's geometry_key when its mesh was last written
        self.reported = False  # a deleted prim is active again and that was reported


class LiveLink:
    def __init__(self, stage_path, session, participant, host):
        self.stage_path = os.path.abspath(stage_path).replace("\\", "/")
        self.session_name = session
        self.session_dir = sessions.session_folder(self.stage_path, session)
        self.session_root = self.session_dir + "/session.usda"
        self.participant = participant
        self.layer_path = self.session_dir + "/" + participant + ".usda"
        self.host = host
        self.tool = host.tool
        self.stage = None
        self.layer = None
        self.session_layer = None
        self.conversion = Gf.Matrix4d(1)  # 3ds Max world = USD world * conversion
        self.conversion_inv = Gf.Matrix4d(1)
        self.prims = {}
        self.mtimes = {}
        self.failed_reloads = {}
        self.dirty_since = None
        self.presence = None
        self.hub = hub.HubClient(self)
        self.local_nodes = set()  # nodes without a prim when joining stay local
        self.after_undo = False
        self.scene_changed = True  # the host saw nodes come or go; the next tick scans
        self.applied = {}  # path_key -> usda last applied from the hub
        self.unplaced = {}  # path_key -> hub layer message for a layer the stage does not use yet
        self.next_file_tick = 0.0
        self.next_scan = 0.0
        self.xforms = UsdGeom.XformCache()
        self.version = 0  # bumped on every change the panel shows
        self.log = []
        self.last_note = None
        self.note_repeats = 0

    # -- session ---------------------------------------------------------------------------------------------

    def start(self):
        """Joins the session, imports the stage (or adopts an earlier import in this scene) and starts syncing."""
        global _active
        if _active is not None:
            _active.leave()

        asset_layer = self.join()

        tag = self.host.read_tag()
        if tag is not None and sessions.same_path(tag.get("stage", ""), self.stage_path):
            self.adopt(tag)
        else:
            self.import_scene(asset_layer)

        self.pull()
        tagged = self.host.nodes()
        self.local_nodes = {node for node in self.host.all_nodes() if node not in tagged}

        _active = self
        self.host.start(self)

        self.presence = sessions.PresenceWriter({
            "version": 1,
            "tool": self.tool,
            "participant": self.participant,
            "stage": self.stage_path,
            "session": self.session_name,
            "sessionRoot": self.session_root,
            "document": self.host.document(),
            "pid": os.getpid(),
        })
        self.presence.refresh(force=True)

    def leave(self, quitting=False):
        """Saves pending edits, says goodbye to the hub and stops syncing. Quitting, it touches no 3ds Max data."""
        global _active
        if _active is self:
            _active = None
        if self.layer is not None:
            self.save_if_due(force=True)
        self.hub.close()
        if self.presence is not None:
            self.presence.remove()
            self.presence = None
        self.host.stop(self)
        self.stage = None
        self.layer = None
        self.session_layer = None
        self.prims.clear()
        self.version += 1
        if not quitting:
            self.host.redraw()

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

    def set_conversion(self, m):
        self.conversion = Gf.Matrix4d(m)
        self.conversion_inv = self.conversion.GetInverse()

    def import_scene(self, asset_layer):
        made = self.host.import_stage(self.stage_path)
        asset_stage = Usd.Stage.Open(asset_layer, Usd.Stage.LoadNone)

        names = {node: (name, parent) for node, name, parent in made}

        def chain(node):
            parts = []
            while node in names:
                name, parent = names[node]
                parts.append(Tf.MakeValidIdentifier(name))
                node = parent
            return list(reversed(parts))

        # The importer names nodes after their prims and keeps the hierarchy. Some versions put everything under a
        # node for the file, so a chain that names no prim is tried without its first name.
        mapped = {}
        for node in names:
            parts = chain(node)
            for skip in (0, 1):
                if len(parts) <= skip:
                    break
                path = "/" + "/".join(parts[skip:])
                if Sdf.Path.IsValidPathString(path) and asset_stage.GetPrimAtPath(path):
                    if path not in self.prims:
                        mapped[node] = path
                    break

        for node, path in mapped.items():
            self.host.tag_node(node, path)
            self.prims[path] = PrimState(node)

        # The importer applies the axis and unit conversion to the top nodes. Measure it on the shallowest mapped
        # node against the asset's own values, since the session layers may already move that prim.
        measured = False
        cache = UsdGeom.XformCache()
        for node, path in sorted(mapped.items(), key=lambda item: depth(item[1])):
            prim = asset_stage.GetPrimAtPath(path)
            if prim and prim.IsA(UsdGeom.Xformable):
                usd_world = cache.GetLocalToWorldTransform(prim)
                if abs(usd_world.GetDeterminant()) > 1e-12:
                    self.set_conversion(usd_world.GetInverse() * self.host.world(node))
                    measured = True
                    break
        if not measured:
            self.set_conversion(axis_conversion(asset_stage, self.host.meters_per_unit()))

        self.host.write_tag({"stage": self.stage_path, "conversion": [self.conversion[i][j]
                                                                      for i in range(4) for j in range(4)]})
        unmapped = len(made) - len(mapped)
        self.note("Imported {} nodes{}".format(len(mapped), ", {} without a prim stay local".format(unmapped)
                                                if unmapped else ""))

    def adopt(self, tag):
        values = list(tag.get("conversion") or [])
        if len(values) == 16:
            self.set_conversion(Gf.Matrix4d(*values))

        for node, path in self.host.nodes().items():
            if path in self.prims:
                continue
            added = self.is_own_def(path)
            if not added and not self.stage.GetPrimAtPath(path):
                continue
            self.prims[path] = PrimState(node, self.host.hidden(node), added=added)

        # Prims deactivated here earlier, so another layer activating them again is reported.
        def visit(path):
            if path.IsPrimPath() and str(path) not in self.prims:
                spec = self.layer.GetPrimAtPath(path)
                if spec is not None and spec.HasInfo("active") and not spec.GetInfo("active"):
                    self.prims[str(path)] = PrimState(None, True, state=DELETED)

        self.layer.Traverse(Sdf.Path.absoluteRootPath, visit)
        self.note("Using the {} nodes already in this scene".format(
            sum(st.state == PRESENT for st in self.prims.values())))

    # -- USD space -------------------------------------------------------------------------------------------

    def usd_parent_world(self, path):
        """USD world matrix of a prim's parent; None for prims at the root."""
        parent = Sdf.Path(path).GetParentPath()
        if parent.IsAbsoluteRootPath():
            return None
        prim = self.stage.GetPrimAtPath(parent)
        if not prim:
            return None
        return self.xforms.GetLocalToWorldTransform(prim)

    def max_local(self, node, path):
        """The node's transform as the local matrix of its prim, in USD space."""
        world = self.host.world(node) * self.conversion_inv
        parent_world = self.usd_parent_world(path)
        return world if parent_world is None else world * parent_world.GetInverse()

    def set_max_local(self, node, path, local):
        parent_world = self.usd_parent_world(path)
        world = local if parent_world is None else local * parent_world
        self.host.set_world(node, world * self.conversion)

    def stage_changed(self):
        """Composed transforms are cached per pull and push; anything that changes the stage clears them."""
        self.xforms.Clear()

    # -- USD -> 3ds Max --------------------------------------------------------------------------------------

    def linked(self, scope=None):
        """(node, prim path, state) of the present linked prims whose nodes exist, parents first; with a scope, only
        those whose prim or an ancestor of it is in the scope."""
        found = []
        for path, st in self.prims.items():
            if st.state != PRESENT or (scope is not None and not in_scope(path, scope)):
                continue
            if st.node is None or not self.host.exists(st.node):
                continue
            found.append((st.node, path, st))
        found.sort(key=lambda item: depth(item[1]))
        return found

    def pull(self, scope=None):
        """Applies the composed stage to the linked nodes whose prims changed since the last pull. A scope (prim paths)
        limits the nodes looked at to those prims and their descendants."""
        self.stage_changed()
        moved = 0

        for node, path, st in self.linked(scope):
            prim = self.stage.GetPrimAtPath(path)
            alive = bool(prim) and prim.IsActive()

            if alive and is_static_xformable(prim):
                local = local_transform(prim)
                if st.usd_local is None or not is_close(local, st.usd_local):
                    st.usd_local = local
                    if not is_close(self.max_local(node, path), local):
                        self.set_max_local(node, path, local)
                        moved += 1
                    st.max_local = self.max_local(node, path)

            hidden = not alive
            if alive and prim.IsA(UsdGeom.Imageable):
                hidden = UsdGeom.Imageable(prim).ComputeVisibility() == UsdGeom.Tokens.invisible

            if hidden != st.hidden:
                st.hidden = hidden
                self.host.set_hidden(node, hidden)

        if moved:
            self.note("Moved {} nodes from the session".format(moved))

        for path, st in self.prims.items():
            if st.state != DELETED or st.added:
                continue
            prim = self.stage.GetPrimAtPath(path)
            active = bool(prim) and prim.IsActive()
            if active and not st.reported:
                self.note("{} is active again in the session; 3ds Max cannot re-create the deleted node".format(
                    Sdf.Path(path).name))
            st.reported = active

        self.host.redraw()

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
                # A participant that joined after this 3ds Max read session.usda: applied once the file lists it.
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
            self.note("The Plasma hub speaks protocol {}, this connector {}".format(protocol, hub.PROTOCOL))
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
        self.version += 1

    # -- 3ds Max -> USD --------------------------------------------------------------------------------------

    def push_transforms(self, moved=None):
        """Writes the transforms of the nodes that moved since the last sync, parents first, so a child's local
        matrix is measured against its parent's new place. moved limits the nodes looked at."""
        self.stage_changed()
        for node, path, st in self.linked():
            if moved is not None and node not in moved and st.max_local is not None:
                continue
            local = self.max_local(node, path)
            if st.max_local is not None and is_close(local, st.max_local):
                continue
            st.max_local = local

            prim = self.stage.GetPrimAtPath(path)
            if not prim or not is_static_xformable(prim):
                continue

            prim.CreateAttribute("xformOp:transform", Sdf.ValueTypeNames.Matrix4d, False).Set(local)
            UsdGeom.Xformable(prim).CreateXformOpOrderAttr().Set(Vt.TokenArray(["xformOp:transform"]))
            self.stage_changed()

            composed = local_transform(prim)
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
        for node, path, st in self.linked():
            hidden = self.host.hidden(node)
            if hidden != st.hidden:
                self.push_visibility(path, st, hidden)

    # -- deleted and added nodes -----------------------------------------------------------------------------

    def is_own_def(self, path):
        spec = self.layer.GetPrimAtPath(path)
        return spec is not None and spec.specifier == Sdf.SpecifierDef

    def scan_nodes(self):
        """Handles linked nodes that vanished from or came back to the scene, copies of linked nodes, and geometry
        nodes to add to the stage."""
        after_undo, self.after_undo = self.after_undo, False
        self.scene_changed = False

        tagged = self.host.nodes()
        found = {}  # prim path -> node
        for node, path in tagged.items():
            st = self.prims.get(path)
            if st is None:
                if not self.stage.GetPrimAtPath(path) and not self.is_own_def(path):
                    self.host.tag_node(node, None)  # a tag from another stage or an old session
                continue
            if path in found:
                # A copy of a linked node carries its tag; the node the prim knows keeps it.
                other = found[path]
                keep, drop = (node, other) if node == st.node else (other, node)
                found[path] = keep
                self.host.tag_node(drop, None)
                continue
            found[path] = node

        present = [path for path, st in self.prims.items() if st.state == PRESENT]
        vanished = [path for path in present if path not in found]
        edited = False

        if vanished:
            if len(vanished) * 2 > len(present) and len(present) > 1:
                for path in vanished:
                    self.prims[path].state = ABSENT
                self.note("{} linked nodes vanished at once; the session keeps them".format(len(vanished)))
            else:
                removed = []
                for path in sorted(vanished, key=depth):
                    st = self.prims[path]
                    if st.state != PRESENT:
                        continue  # removed with its parent
                    if after_undo and not st.added:
                        st.state = ABSENT  # an undo is not a delete
                    else:
                        if self.delete(path, st):
                            removed.append(path)
                        edited = True
                self.release_children(removed)

        for path, node in found.items():
            st = self.prims.get(path)
            if st is None:
                continue
            if st.node != node:
                st.node = node
                st.max_local = None
            if st.state == ABSENT:
                st.state = PRESENT
            elif st.state == DELETED:
                self.restore(node, path, st)
                edited = True

        known = set(tagged) | self.local_nodes
        candidates = [node for node in self.host.all_nodes()
                      if node not in known and self.host.is_geometry(node)]
        for node in sorted(candidates, key=self.node_depth):
            if self.add_node(node):
                edited = True

        self.author_changed_meshes()

        if edited:
            self.pull()

    def node_depth(self, node):
        d = 0
        parent = self.host.parent(node)
        while parent is not None and d < 1000:
            d += 1
            parent = self.host.parent(parent)
        return d

    def delete(self, path, st):
        """The node of a prim was deleted. Returns True when the prim was removed rather than deactivated."""
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
        """Added nodes below a removed prim lost their prim with it; the next scan adds them where they are now."""
        for root in removed:
            prefix = root + "/"
            for path in [p for p in self.prims if p.startswith(prefix)]:
                st = self.prims.pop(path)
                if st.node is not None and self.host.exists(st.node):
                    self.host.tag_node(st.node, None)
                    self.local_nodes.discard(st.node)

    def restore(self, node, path, st):
        """A deleted node came back, by an undo: its prim is active, or defined, again."""
        if st.added:
            prim = self.stage.GetPrimAtPath(path)
            if not prim or not prim.IsDefined():
                self.author_mesh(path, node, st)
                st.max_local = None
        else:
            prim = self.stage.GetPrimAtPath(path)
            if prim:
                prim.ClearActive()
                self.remove_inert(path)
        st.state = PRESENT
        st.reported = False
        st.hidden = False
        self.note("{} restored".format(self.host.name(node)))
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

    def add_node(self, node):
        """Defines a Mesh prim for a new geometry node under the prim of its parent node, or the default prim."""
        parent_path = Sdf.Path.absoluteRootPath
        default_prim = self.stage.GetDefaultPrim()
        if default_prim:
            parent_path = default_prim.GetPath()
        parent = self.host.parent(node)
        if parent is not None:
            for path, st in self.prims.items():
                if st.node == parent and st.state == PRESENT:
                    parent_path = Sdf.Path(path)
                    break

        base = Tf.MakeValidIdentifier(self.host.name(node) or "Mesh")
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
        st = PrimState(node, added=True)
        if not self.author_mesh(key, node, st):
            self.local_nodes.add(node)  # no mesh to write; it stays local
            return False
        self.host.tag_node(node, key)
        self.prims[key] = st
        self.stage_changed()
        st.max_local = None
        self.push_transforms()
        self.note("{} added as {}".format(self.host.name(node), key))
        self.mark_dirty()
        return True

    def author_mesh(self, path, node, st):
        data = self.host.mesh(node)
        if data is None:
            return False
        meshes.author_mesh(self.stage, path, data)
        st.geometry_key = self.host.geometry_key(node)
        self.stage_changed()
        return True

    def author_changed_meshes(self):
        for path, st in self.prims.items():
            if not st.added or st.state != PRESENT or st.node is None or not self.host.exists(st.node):
                continue
            key = self.host.geometry_key(st.node)
            if key == st.geometry_key or not self.stage.GetPrimAtPath(path):
                continue
            if self.author_mesh(path, st.node, st):
                self.mark_dirty()

    # -- saving ----------------------------------------------------------------------------------------------

    def mark_dirty(self):
        if self.dirty_since is None:
            self.dirty_since = clock()
        self.version += 1
        self.hub.layer_changed()

    def save_if_due(self, force=False):
        if self.dirty_since is None:
            return
        if not force and clock() - self.dirty_since < SAVE_DELAY_SECONDS:
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
        """One step of the sync; the host calls it from its timer. Returns the seconds until the next call."""
        if _active is not self or self.stage is None:
            return None
        now = clock()
        try:
            self.hub.poll(now)
            if now >= self.next_file_tick:
                self.next_file_tick = now + TICK_SECONDS
                changed, scope = self.reload_changed_layers()
                if changed:
                    self.apply_unplaced()
                    self.pull(scope)
                full = self.scene_changed or self.after_undo or now >= self.next_scan
                if full:
                    self.next_scan = now + SCAN_SECONDS
                    self.scan_nodes()
                # Every scan looks at all nodes, as a safety net for changes the host's callbacks miss.
                moved = self.host.take_moved()
                self.push_transforms(None if full else moved)
                self.poll_hidden()
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
        self.version += 1


def join_session(stage, session, user, host):
    """Joins or starts a session on a USD stage as max-<user>; returns the live link."""
    session_root = sessions.session_folder(stage, session) + "/session.usda"
    link = LiveLink(stage, session, sessions.choose_participant(session_root, user), host)
    link.start()
    return link


def leave_active(quitting=False):
    if _active is not None:
        _active.leave(quitting)
