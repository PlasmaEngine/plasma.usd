"""The live link between this Maya and one USD live session. Needs only pxr and the standard library, no Maya.

The link works on a stage whose root layer is the session's ``session.usda``, and on that stage's own layer objects.
In Maya the host (maya_host.py) hands it the stage of the maya-usd proxy shape it made, or found again, on
session.usda; without Maya (tests, a plain Python with pxr) the base Host below opens a stage of its own.

Joining creates or opens the session files: session.usda beside the asset, and this Maya's participant layer
``maya-<user>.usda``, which is inserted into session.usda just above the asset (the weakest participant; whoever
started the session is strongest). The stage's edit target is that layer, so what the artist does to USD prims in Maya
(move, hide, deactivate, set values) is authored there by maya-usd.

Maya -> session: the link listens to the stage's Usd.Notice.ObjectsChanged, and to Sdf's per-layer change notice for
Maya's layer, which tells which of those changes touched it (a counter the tick compares). When the layer's text then
differs from what was last passed on, the whole text goes to the hub (at most ~20 times a second, always the final
state) and the file is saved shortly after, with a ``saved`` notice to the hub, as in the Blender and Houdini
connectors.

Session -> Maya: a layer from the hub replaces the content of the stage's own SdfLayer for that participant, in memory
(ImportFromString); a file that changed is reloaded. The stage recomposes, and maya-usd redraws from its
ObjectsChanged notice. Received layers are never saved: the host protects every other participant's layer from saving
(in Maya a maya-usd system lock, which both its save dialog and its save on Maya scene save skip), and leaving reads the
layers the hub filled from disk again, so none stays dirty.

Two tiers carry the layers, as in the other connectors:
  * the Plasma editor's live hub (hub.py): while connected, the hub decides the content of the layers it relays and
    their files are not polled. When the session owner commits, every layer is read from disk again.
  * the session files: this Maya's layer is saved after each change, and the other layers are reloaded when their
    files change. This is all there is without a hub, and what remains after the connection drops.

Where a stronger participant's layer decides a value Maya's layer also sets, Maya shows the stronger value (its stage is
the session); those opinions are listed in the panel.

The host ticks the link: in Maya a single-shot Qt timer on the main thread, re-armed with the interval tick() returns
(50 ms while a hub connection is open, 5 ms while a ping waits for its pong, 250 ms otherwise). Without a host timer the
caller calls tick() itself.
"""

import atexit
import os
import time
import traceback

from pxr import Sdf, Tf, Usd

from . import hub, opinions, sessions

TICK_SECONDS = 0.25  # file polling, presence, layer protection and the host's checks
HUB_TICK_SECONDS = 0.05  # the tick's rate while a hub connection is open
SAVE_DELAY_SECONDS = 0.2

_active = None


def active_link():
    return _active


class Host:
    """What the link needs from the application it runs in. This base needs no application: it opens a stage of its
    own on session.usda, sets the edit target on the stage, protects the other participants' layers only by taking away
    their permission to save, and leaves ticking to the caller. maya_host.MayaHost is the Maya one."""

    def open_stage(self, link):
        """The stage the link works on; its root layer is link.session_layer."""
        return Usd.Stage.Open(link.session_layer, Usd.Stage.LoadNone)

    def document(self):
        """The application's current document, for the presence file."""
        return ""

    def set_edit_target(self, link):
        link.stage.SetEditTarget(Usd.EditTarget(link.layer))

    def edit_target(self, link):
        """The layer the application's edits go to."""
        return link.stage.GetEditTarget().GetLayer()

    def is_protected(self, layer):
        return not layer.permissionToSave

    def protect_layer(self, link, layer):
        """Keeps the application from saving a layer this link does not own."""
        layer.SetPermissionToSave(False)

    def release_layer(self, link, layer):
        layer.SetPermissionToSave(True)

    def problem(self, link):
        """Why the application's edits cannot reach the session right now, or None; asked every file tick."""
        return None

    def start(self, link):
        """The link joined: tick it and watch the application."""

    def stop(self, link, quitting):
        """The link leaves: stop ticking it. Quitting, the application must not be touched anymore."""


class LiveLink:
    def __init__(self, stage_path, session, participant, tool, host=None):
        self.stage_path = os.path.abspath(stage_path).replace("\\", "/")
        self.session_name = session
        self.session_dir = sessions.session_folder(self.stage_path, session)
        self.session_root = self.session_dir + "/session.usda"
        self.participant = participant
        self.layer_path = self.session_dir + "/" + participant + ".usda"
        self.tool = tool
        self.host = host if host is not None else Host()
        self.stage = None  # the session as every participant composes it, with this Maya's layer as the edit target
        self.layer = None  # this Maya's participant layer
        self.session_layer = None
        self.last_text = None  # the layer's text as last passed on
        self.layer_changes = 0  # change notices for self.layer; the tick compares it with seen_changes
        self.seen_changes = 0
        self.notices = []
        self.protected = {}  # path_key -> layer this link protected from saving
        self.protect_failed = set()
        self.problem = None  # why Maya's edits cannot reach the session right now; reported once
        self.target_elsewhere = False  # the edit target is another layer; retarget() fixes that
        self.outvoted = []  # (prim path, what, layer name) of opinions a stronger layer decides
        self.outvoted_dirty = True
        self.mtimes = {}
        self.failed_reloads = {}
        self.dirty_since = None
        self.presence = None
        self.hub = hub.HubClient(self)
        self.applied = {}  # path_key -> usda last applied from the hub
        self.unplaced = {}  # path_key -> hub layer message for a layer the stage does not use yet
        self.next_file_tick = 0.0
        self.log = []
        self.last_note = None
        self.note_repeats = 0
        self.version = 0  # bumped whenever the panel has something new to show

    # -- session ---------------------------------------------------------------------------------------------

    def start(self):
        """Joins the session: prepares its files, takes the host's stage, targets this Maya's layer and starts
        syncing."""
        global _active
        if _active is not None:
            _active.leave()

        self.prepare()
        self.attach(self.host.open_stage(self))

        _active = self
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

        atexit.unregister(at_exit)
        atexit.register(at_exit)
        self.host.start(self)

        # The file holds the layer before the hub connection exists: the Plasma editor applies hub layers only once its
        # stage lists them, so the first layer message can arrive before it has read the new session.usda; it then
        # reads this file instead.
        self.check_layer(force=True)
        self.save_if_due(force=True)

    def leave(self, quitting=False):
        """Sends and saves the layer's last state, says goodbye to the hub and stops syncing. Not quitting, the layers
        the hub filled are read from disk again and the other participants' layers are released; the proxy shape
        stays, and joining again reuses it. Quitting, it touches nothing of Maya's."""
        global _active
        if _active is self:
            _active = None
        try:
            self.host.stop(self, quitting)
        except Exception:
            if not quitting:
                traceback.print_exc()
        if not quitting and self.layer is not None and self.stage is not None:
            try:
                self.check_layer()
            except Exception:
                traceback.print_exc()
        self.stop_listening()
        if self.layer is not None:
            self.save_if_due(force=True)
        filled = set(self.applied)
        self.hub.close()
        if not quitting and self.stage is not None:
            try:
                self.reload_from_disk(lambda layer: sessions.path_key(layer.realPath) in filled and layer.dirty)
                self.release_participants()
            except Exception:
                traceback.print_exc()
        if self.presence is not None:
            self.presence.remove()
            self.presence = None
        self.stage = None
        self.layer = None
        self.session_layer = None
        self.version += 1

    def prepare(self):
        """Creates or opens the session files and this Maya's layer, and lists the layer in session.usda."""
        os.makedirs(self.session_dir, exist_ok=True)

        asset_layer = Sdf.Layer.FindOrOpen(self.stage_path)
        if asset_layer is None:
            raise RuntimeError("Cannot open " + self.stage_path)

        session_path = self.session_root
        session = Sdf.Layer.Find(session_path)
        if session is not None:
            # Open already, in the proxy stage of a reopened Maya scene or from an earlier join: other participants may
            # have joined since it was read. Forced, because Sdf's own check compares file times in whole seconds on
            # Windows.
            if not session.dirty:
                session.Reload(True)
        elif os.path.exists(session_path):
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
        if session is None:
            raise RuntimeError("Cannot open " + session_path)

        own_path = self.layer_path
        own = Sdf.Layer.Find(own_path)
        if own is not None:
            # Open already: unsaved edits made in Maya since the last save stay and are sent.
            if not own.dirty:
                own.Reload(True)
        elif os.path.exists(own_path):
            own = Sdf.Layer.FindOrOpen(own_path)
        else:
            own = Sdf.Layer.CreateNew(own_path)
            own.documentation = "Opinions authored by {} during live session '{}'.".format(self.participant,
                                                                                         self.session_name)
            own.Save()
        if own is None:
            raise RuntimeError("Cannot open " + own_path)
        self.layer = own
        self.list_own_layer(session)
        self.session_layer = session

    def list_own_layer(self, session):
        """Lists this Maya's layer in session.usda and saves it. Whoever started the session is strongest; later
        participants sit just above the asset."""
        own_rel = "./" + self.participant + ".usda"
        if own_rel not in list(session.subLayerPaths):
            session.subLayerPaths.insert(max(len(session.subLayerPaths) - 1, 0), own_rel)
        session.Save()

    def attach(self, stage):
        """Works on stage from now on: checks that it composes the session with this Maya's layer, reads the other
        participants' layers from disk again where they hold unsaved content, targets this Maya's layer, listens to
        the stage and protects the other participants' layers."""
        root = stage.GetRootLayer()
        if root != self.session_layer:
            if not (root.realPath and sessions.same_path(root.realPath, self.session_root)):
                raise RuntimeError("The stage shows {}, not {}".format(root.identifier, self.session_root))
            # session.usda under another spelling of its path: the stage's layer objects are the ones to work on.
            if not root.dirty:
                root.Reload(True)
            self.list_own_layer(root)
            self.session_layer = root
        own_key = sessions.path_key(self.layer_path)
        own = next((layer for layer in stage.GetLayerStack(False)
                    if layer.realPath and sessions.path_key(layer.realPath) == own_key), None)
        if own is None:
            raise RuntimeError("{}.usda is {} in the stage".format(
                self.participant, "muted" if stage.IsLayerMuted(self.layer.identifier) else "missing"))
        self.layer = own
        self.stage = stage

        # What a reopened Maya scene brought back (maya-usd can store dirty layers in the scene file) or the hub sent
        # during an earlier join is not the participants' layer anymore; the hub sends its state again after welcome.
        others = self.participant_layers()
        self.reload_from_disk(lambda layer: layer.dirty and layer in others)

        self.host.set_edit_target(self)
        self.listen()
        self.protect_participants()
        self.check_problem()
        self.note("Joined '{}' as {}".format(self.session_name, self.participant))

    def participant_keys(self):
        """path_key of every participant layer session.usda lists (all its sublayers but the asset)."""
        return {sessions.path_key(self.session_layer.ComputeAbsolutePath(p))
                for p in list(self.session_layer.subLayerPaths)[:-1]}

    def participant_layers(self):
        """The other participants' layers the stage uses."""
        keys = self.participant_keys()
        keys.discard(sessions.path_key(self.layer_path))
        return [layer for layer in self.stage.GetUsedLayers() if not layer.anonymous and layer.realPath
                and sessions.path_key(layer.realPath) in keys]

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

    # -- Maya -> session -------------------------------------------------------------------------------------

    def listen(self):
        """Counts the changes to this Maya's layer (Sdf's notice for that layer alone, or, where a USD lacks it, the
        notice for all layers filtered to it), and marks the outvoted list for a new look on the stage's
        ObjectsChanged. The callbacks only count and mark; the tick does the work."""
        try:
            self.notices.append(Tf.Notice.Register(Sdf.Notice.LayersDidChangeSentPerLayer, self.on_layer_notice,
                                                   self.layer))
        except Exception:
            self.notices.append(Tf.Notice.RegisterGlobally(Sdf.Notice.LayersDidChange, self.on_layers_notice))
        self.notices.append(Tf.Notice.Register(Usd.Notice.ObjectsChanged, self.on_objects_changed, self.stage))

    def stop_listening(self):
        for key in self.notices:
            try:
                key.Revoke()
            except Exception:
                pass
        self.notices = []

    def on_layer_notice(self, notice, sender):
        self.layer_changes += 1

    def on_layers_notice(self, notice, sender):
        try:
            if not any(layer == self.layer for layer in notice.GetLayers()):
                return
        except Exception:
            pass
        self.layer_changes += 1

    def on_objects_changed(self, notice, sender):
        self.outvoted_dirty = True

    def check_layer(self, force=False):
        """Passes this Maya's layer on when it changed since the last look and its text differs from what was passed
        on."""
        changes = self.layer_changes
        if not force and changes == self.seen_changes:
            return
        self.seen_changes = changes
        text = self.layer.ExportToString()
        if text == self.last_text:
            return
        first = self.last_text is None
        self.last_text = text
        self.outvoted_dirty = True
        if not first:
            self.note("Maya's layer changed ({} prims)".format(len(opinion_paths(self.layer))))
        self.mark_dirty()

    def mark_dirty(self):
        if self.dirty_since is None:
            self.dirty_since = hub.clock()
        self.hub.layer_changed()

    def save_if_due(self, force=False):
        if self.dirty_since is None:
            return
        if not force and hub.clock() - self.dirty_since < SAVE_DELAY_SECONDS:
            return
        # The hub gets this state first, so its saved notice names the layer message the file holds. When the rate
        # limit holds the message back, the save waits for a later tick.
        self.hub.send_layer_if_due(force=force)
        if self.hub.layer_pending:
            return
        self.layer.Save()
        self.dirty_since = None
        self.hub.layer_saved()

    def retarget(self):
        """Points Maya's edits at this Maya's layer again (the panel's Target My Layer)."""
        if self.stage is None:
            return
        self.host.set_edit_target(self)
        self.check_problem()

    def set_problem(self, text):
        if text != self.problem:
            self.problem = text
            if text:
                self.note(text)
            self.version += 1

    def check_problem(self):
        """Reports what keeps Maya's edits from this Maya's layer: the layer muted or gone from the stage, the host's
        own problems (the proxy shape deleted, say), or another edit target."""
        self.target_elsewhere = False
        try:
            if not any(layer == self.layer for layer in self.stage.GetLayerStack(False)):
                muted = self.stage.IsLayerMuted(self.layer.identifier)
                text = "{}.usda is {} in Maya's stage; nothing you edit reaches the session".format(
                    self.participant, "muted" if muted else "missing")
            else:
                text = self.host.problem(self)
                if text is None:
                    target = self.host.edit_target(self)
                    if target is None or target != self.layer:
                        self.target_elsewhere = True
                        name = opinions.layer_name(target) if target is not None else "unknown"
                        text = ("Maya's edit target is {}, not {}.usda; those edits do not reach the session "
                                "(Target My Layer points it back)".format(name, self.participant))
        except Exception as e:
            text = "Cannot check Maya's stage: {!r}".format(e)
        self.set_problem(text)

    # -- other participants' layers ---------------------------------------------------------------------------

    def protect_participants(self):
        """Protects every other participant's layer the stage uses from being saved by Maya: the ones not protected
        yet, new participants, and layers something unprotected again (maya-usd refreshes its system locks from the
        files' write permissions when a layer or the stage is reloaded)."""
        for layer in self.participant_layers():
            if self.host.is_protected(layer):
                continue
            key = sessions.path_key(layer.realPath)
            try:
                self.host.protect_layer(self, layer)
            except Exception as e:
                if key not in self.protect_failed:
                    self.protect_failed.add(key)
                    self.note("Cannot protect {} from saving: {!r}".format(os.path.basename(layer.realPath), e))
                continue
            self.protected[key] = layer

    def release_participants(self):
        """Gives back the layers this link protected."""
        for layer in self.protected.values():
            try:
                if self.host.is_protected(layer):
                    self.host.release_layer(self, layer)
            except Exception:
                traceback.print_exc()
        self.protected = {}

    # -- session -> Maya -------------------------------------------------------------------------------------

    def used_layer(self, key):
        return next((layer for layer in self.stage.GetUsedLayers() if not layer.anonymous and layer.realPath
                     and sessions.path_key(layer.realPath) == key), None)

    def reload_changed_layers(self):
        """Reloads the layers whose files changed; True when any did."""
        changed = False
        own = self.layer.identifier

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

            # A file that fails to parse keeps its old time, so the next tick tries again. Forced, because Sdf's own
            # check compares file times in whole seconds on Windows and skips a second change within the same second.
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
            self.note(os.path.basename(layer.realPath) + " changed")

        return changed

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

    def changed_from_session(self):
        """The session's layers changed in memory: the outvoted list is looked at again and the panel redrawn."""
        self.outvoted_dirty = True
        self.version += 1

    # -- hub -------------------------------------------------------------------------------------------------

    def apply_hub_layers(self, messages):
        """Puts other participants' layers from the hub into the stage's layers in memory. They are never saved."""
        if self.stage is None:
            return
        own = sessions.path_key(self.layer_path)
        participants = self.participant_keys()
        applied = 0

        for message in messages:
            key = sessions.path_key(message["layer"])
            if key == own:
                continue
            layer = self.used_layer(key) if key in participants else None
            if layer is None:
                # A participant that joined after this Maya read session.usda: applied once the file lists it.
                self.unplaced[key] = message
                continue
            self.unplaced.pop(key, None)
            if self.applied.get(key) == message["usda"]:
                continue
            ok = import_text(layer, message["usda"])
            if not ok:
                self.note("Cannot read {} from the hub".format(os.path.basename(layer.realPath)))
                continue
            self.applied[key] = message["usda"]
            applied += 1

        if applied:
            self.changed_from_session()

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
        self.changed_from_session()

    def on_committed(self):
        """The owner folded every participant layer into the asset and cleared them: everything is read again,
        Maya's layer too, which is then what was last passed on. Maya's next edit goes out as usual."""
        if self.stage is None:
            return
        self.note("The session was committed to the asset; reloading all layers")
        self.dirty_since = None
        self.hub.layer_pending = False
        self.reload_from_disk(lambda layer: True)
        self.last_text = self.layer.ExportToString()
        self.seen_changes = self.layer_changes
        self.changed_from_session()

    def redraw(self):
        self.version += 1

    # -- outvoted opinions -----------------------------------------------------------------------------------

    def update_outvoted(self):
        self.outvoted_dirty = False
        found = opinions.outvoted(self.stage, self.layer)
        known = {(path, what) for path, what, _ in self.outvoted}
        for path, what, by in found:
            if (path, what) not in known:
                self.note("{}: {} decides its {}".format(Sdf.Path(path).name, by, what))
        if found != self.outvoted:
            self.outvoted = found
            self.version += 1

    # -- loop ------------------------------------------------------------------------------------------------

    def tick(self):
        """One pass of the link; returns the seconds until the next, or None once the link is gone."""
        if _active is not self or self.stage is None:
            return None
        now = hub.clock()
        try:
            self.hub.poll(now)
            self.check_layer()
            if now >= self.next_file_tick:
                self.next_file_tick = now + TICK_SECONDS
                if self.reload_changed_layers():
                    self.apply_unplaced()
                    self.changed_from_session()
                self.protect_participants()
                self.check_problem()
                if self.outvoted_dirty:
                    self.update_outvoted()
                if self.presence is not None:
                    self.presence.refresh()
            self.save_if_due()
            self.hub.send_layer_if_due()
        except Exception as e:  # keep the loop alive; the panel shows the error
            traceback.print_exc()
            self.note("Error: {!r}".format(e))
        return self.hub.poll_interval(HUB_TICK_SECONDS if self.hub.state != hub.OFF else TICK_SECONDS)

    def document_saved(self, path):
        """The host saved its document under path."""
        if self.presence is not None:
            self.presence.info["document"] = path
            self.presence.refresh(force=True)

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
            print("[Plasma USD Live]", text, flush=True)
        self.version += 1


def import_text(layer, usda):
    """Replaces a layer's content with usda text in memory; True when it worked. A layer without permission to edit
    (maya-usd's system lock takes it away) is allowed the import alone, should a USD refuse it."""
    try:
        if layer.ImportFromString(usda):
            return True
    except Exception:
        traceback.print_exc()
    if layer.permissionToEdit:
        return False
    layer.SetPermissionToEdit(True)
    try:
        return bool(layer.ImportFromString(usda))
    except Exception:
        traceback.print_exc()
        return False
    finally:
        layer.SetPermissionToEdit(False)


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


def at_exit():
    """Python exits with a session still joined: save pending edits and take this Maya off the open session list,
    without touching Maya, which may be gone already."""
    link = _active
    if link is None:
        return
    try:
        link.leave(quitting=True)
    except Exception:
        pass


def join_session(stage, session, user, tool, host=None):
    """Joins or starts a session on a stage as maya-<user> (numbered when that name is taken); returns the link."""
    session_root = sessions.session_folder(stage, session) + "/session.usda"
    participant = sessions.choose_participant(session_root, user)
    link = LiveLink(stage, session, participant, tool, host)
    try:
        link.start()
    except Exception:
        link.leave()
        raise
    return link


def leave_active():
    if _active is not None:
        _active.leave()
