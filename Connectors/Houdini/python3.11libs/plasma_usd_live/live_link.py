"""The live link between this Houdini and one USD live session.

Joining builds (or finds again) the session's LOP setup (network.py): the session root sublayered, this Houdini's own
participant layer (``houdini-<user>.usda``) muted in Houdini's stage, a Layer Break, and the plasma_live node. The
artist edits with ordinary LOPs wired in between the layer break and plasma_live.

Houdini -> session: whenever plasma_live needs to cook or has cooked since the last look, the layers the network
authored after the break are flattened into one layer with Houdini's own data stripped (opinions.py). When that text
changes it replaces the content of this Houdini's layer, which goes to the hub (at most ~20 times a second, always the
final state) and is saved shortly after (with a ``saved`` notice to the hub), as the Blender connector does.

Session -> Houdini: the other participants' layers are the same SdfLayer objects Houdini's stage composes, kept loaded
by this link's own stage over session.usda. A layer from the hub replaces the content of that layer in memory
(ImportFromString) and a file that changed is reloaded; either way the plasma_session node's output is invalidated, so
the network cooks again and the viewport follows. Houdini's recook uses the layers in memory, it does not read the
files again. Received layers are never saved.

Two tiers carry the layers, as in the Blender connector:
  * the Plasma editor's live hub (hub.py): while connected, the hub decides the content of the layers it relays and
    their files are not polled. When the session owner commits, every layer is read from disk again.
  * the session files: this Houdini's layer is saved after each change, and the other layers are reloaded when their
    files change. This is all there is without a hub, and what remains after the connection drops.

Where a stronger participant's layer decides a value this Houdini's layer also sets, Houdini shows its own value (its
network sits above the session) while the session shows the stronger one; those opinions are listed in the panel.

In a Houdini with a UI the link ticks on the main thread from a single-shot Qt timer, re-armed with the interval tick()
returns (50 ms while a hub connection is open, 5 ms while a ping waits for its pong, 250 ms otherwise). Houdini's own
event loop callbacks (hou.ui.addEventLoopCallback) come only every ~50 ms while Houdini is idle, which would add up to
that much to every message in both directions and to the measured round trip. Without a UI (hython), the caller calls
tick() itself.
"""

import atexit
import os
import time
import traceback

import hou
from pxr import Sdf, Usd

from . import hub, network, opinions, sessions

TICK_SECONDS = 0.25  # file polling, presence and the network check without a hub
HUB_TICK_SECONDS = 0.05  # the tick's rate while a hub connection is open
SAVE_DELAY_SECONDS = 0.2

_active = None


def active_link():
    return _active


class LiveLink:
    def __init__(self, stage_path, session, participant):
        self.stage_path = os.path.abspath(stage_path).replace("\\", "/")
        self.session_name = session
        self.session_dir = sessions.session_folder(self.stage_path, session)
        self.session_root = self.session_dir + "/session.usda"
        self.participant = participant
        self.layer_path = self.session_dir + "/" + participant + ".usda"
        self.tool = "Houdini " + hou.applicationVersionString()
        self.stage = None  # the session as every participant composes it; also keeps its layers loaded
        self.layer = None  # this Houdini's participant layer: the network's opinions as sent and saved
        self.session_layer = None
        self.setup = None
        self.last_text = None  # the network's opinions last put into self.layer
        self.last_cook_count = None
        self.network_problem = None  # why the network's opinions cannot be read right now; reported once
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
        self.timer = None
        self.log = []
        self.last_note = None
        self.note_repeats = 0
        self.version = 0  # bumped whenever the panel has something new to show

    # -- session ---------------------------------------------------------------------------------------------

    def start(self):
        """Joins the session, builds or finds its LOP setup and starts syncing."""
        global _active
        if _active is not None:
            _active.leave()

        previous = self.join()
        self.setup, fresh = network.build(self.session_root, self.layer_path, self.session_name, self.participant,
                                          previous_usda=previous)
        if fresh and previous:
            self.note("{}'s opinions from before are in plasma_previous_edits".format(self.participant))
        self.refresh_houdini()

        _active = self
        self.presence = sessions.PresenceWriter({
            "version": 1,
            "tool": self.tool,
            "participant": self.participant,
            "stage": self.stage_path,
            "session": self.session_name,
            "sessionRoot": self.session_root,
            "document": hou.hipFile.path(),
            "pid": os.getpid(),
        })
        self.presence.refresh(force=True)

        hou.hipFile.addEventCallback(self.on_hip_event)
        atexit.unregister(at_exit)
        atexit.register(at_exit)
        if hou.isUIAvailable():
            from PySide6 import QtCore
            self.timer = QtCore.QTimer()
            self.timer.setSingleShot(True)
            self.timer.setTimerType(QtCore.Qt.PreciseTimer)
            self.timer.timeout.connect(self.on_timer)
            self.timer.start(0)

        # The file holds the network's opinions before the hub connection exists: the Plasma editor applies hub
        # layers only once its stage lists them, so the first layer message can arrive before it has read the new
        # session.usda; it then reads this file instead.
        self.check_network(force=True)
        self.save_if_due(force=True)

    def leave(self, quitting=False):
        """Sends and saves the network's last state, says goodbye to the hub and stops syncing. The LOP setup stays,
        and joining again reuses it. Quitting, it touches nothing of Houdini's."""
        global _active
        if _active is self:
            _active = None
        if not quitting:
            self.stop_callbacks()
            if self.layer is not None and self.setup is not None:
                try:
                    self.check_network()
                except Exception:
                    traceback.print_exc()
        if self.layer is not None:
            self.save_if_due(force=True)
        self.hub.close()
        if self.presence is not None:
            self.presence.remove()
            self.presence = None
        self.stage = None
        self.layer = None
        self.session_layer = None
        self.version += 1

    def stop_callbacks(self):
        if self.timer is not None:
            self.timer.stop()
            self.timer = None
        try:
            hou.hipFile.removeEventCallback(self.on_hip_event)
        except hou.Error:
            pass

    def join(self):
        """Creates or opens the session files and this Houdini's layer; returns the layer's earlier opinions as usda
        text, or None when it holds none."""
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
            self.layer.documentation = self.documentation()
            self.layer.Save()
        previous = self.layer.ExportToString() if len(self.layer.rootPrims) else None

        # Whoever started the session is strongest; later participants sit just above the asset.
        own_rel = "./" + self.participant + ".usda"
        if own_rel not in list(session.subLayerPaths):
            session.subLayerPaths.insert(max(len(session.subLayerPaths) - 1, 0), own_rel)
        session.Save()

        self.session_layer = session
        self.stage = Usd.Stage.Open(session, Usd.Stage.LoadNone)
        self.note("Joined '{}' as {}".format(self.session_name, self.participant))
        return previous

    def documentation(self):
        return "Opinions authored by {} during live session '{}'.".format(self.participant, self.session_name)

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

    def live_node(self):
        return self.setup.node(network.LIVE) if self.setup is not None else None

    # -- Houdini -> session ----------------------------------------------------------------------------------

    def is_session_layer(self, identifier):
        """True for the layers of the session itself: session.usda, the participants' layers and the asset's."""
        if Sdf.Layer.IsAnonymousLayerIdentifier(identifier):
            return False
        key = sessions.path_key(identifier)
        return any(not layer.anonymous and sessions.path_key(layer.identifier) == key
                   for layer in self.stage.GetUsedLayers())

    def problem(self, text):
        if text != self.network_problem:
            self.network_problem = text
            if text:
                self.note(text)
            self.version += 1

    def network_opinions(self, node):
        """The opinions plasma_live's network authors after the layer break, as the usda text of this Houdini's layer;
        None when they cannot be read."""
        try:
            stage = node.stage()
        except hou.Error as e:
            self.problem("plasma_live does not cook: {}".format(str(e).strip()[-160:]))
            return None
        if stage is None or node.errors():
            self.problem("plasma_live has errors; nothing is sent until they are fixed")
            return None

        upstream = set(node.layersAboveLayerBreak())
        if not any(sessions.same_path(i, self.session_root) for i in upstream
                   if not Sdf.Layer.IsAnonymousLayerIdentifier(i)):
            self.problem("plasma_layer_break is missing, bypassed or not above plasma_live; nothing is sent")
            return None

        root = stage.GetRootLayer()
        mine = [p for p in root.subLayerPaths if p not in upstream and not self.is_session_layer(p)]
        merged = opinions.merge(mine)
        opinions.strip(merged, self.documentation())
        self.problem(None)
        return merged.ExportToString()

    def check_network(self, force=False):
        """Reads the network's opinions when plasma_live changed, and passes them on when they differ."""
        node = self.live_node()
        if node is None:
            self.problem("plasma_live is gone; join again to rebuild the session's nodes")
            return
        if not force and self.last_cook_count == node.cookCount() and not node.needsToCook():
            return
        text = self.network_opinions(node)
        self.last_cook_count = node.cookCount()
        if text is None or text == self.last_text:
            return
        if not self.layer.ImportFromString(text):
            self.note("Cannot store the network's opinions in {}.usda".format(self.participant))
            return
        first = self.last_text is None
        self.last_text = text
        self.outvoted_dirty = True
        if not first:
            self.note("Houdini's opinions changed ({} prims)".format(len(opinion_paths(self.layer))))
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

    # -- session -> Houdini ----------------------------------------------------------------------------------

    def refresh_houdini(self):
        """The session's layers changed in memory: the network cooks again from plasma_session down."""
        self.outvoted_dirty = True
        self.version += 1
        node = self.setup.node(network.SESSION) if self.setup is not None else None
        if node is not None:
            node.invalidateOutput()

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

    # -- hub -------------------------------------------------------------------------------------------------

    def apply_hub_layers(self, messages):
        """Puts other participants' layers from the hub into the session's layers in memory. They are never saved."""
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
                # A participant that joined after this Houdini read session.usda: applied once the file lists it.
                self.unplaced[key] = message
                continue
            self.unplaced.pop(key, None)
            if self.applied.get(key) == message["usda"]:
                continue
            try:
                ok = layer.ImportFromString(message["usda"])
            except Exception:
                ok = False
                traceback.print_exc()
            if not ok:
                self.note("Cannot read {} from the hub".format(os.path.basename(layer.realPath)))
                continue
            self.applied[key] = message["usda"]
            applied += 1

        if applied:
            self.refresh_houdini()

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
        self.refresh_houdini()

    def on_committed(self):
        """The owner folded every participant layer into the asset and cleared them: everything is read again. The
        network still holds its edits; they go out again with its next change."""
        if self.stage is None:
            return
        self.note("The session was committed to the asset; reloading all layers")
        self.dirty_since = None
        self.hub.layer_pending = False
        self.reload_from_disk(lambda layer: True)
        self.refresh_houdini()

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
            self.check_network()
            if now >= self.next_file_tick:
                self.next_file_tick = now + TICK_SECONDS
                if self.reload_changed_layers():
                    self.apply_unplaced()
                    self.refresh_houdini()
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

    def on_timer(self):
        interval = self.tick()
        if interval is None:
            self.stop_callbacks()
        elif self.timer is not None:
            self.timer.start(max(1, int(interval * 1000.0)))

    def on_hip_event(self, event_type):
        """A new or another .hip replaces the scene, so the session is left first; a save moves the document."""
        if event_type in (hou.hipFileEventType.BeforeClear, hou.hipFileEventType.BeforeLoad):
            if _active is self:
                self.leave()
        elif event_type == hou.hipFileEventType.AfterSave and self.presence is not None:
            self.presence.info["document"] = hou.hipFile.path()
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
    """Python exits with a session still joined: save pending edits and take this Houdini off the open session list,
    without touching Houdini, which may be gone already."""
    link = _active
    if link is None:
        return
    try:
        link.leave(quitting=True)
    except Exception:
        pass


def join_session(stage, session, user):
    """Joins or starts a session on a stage as houdini-<user>; returns the link."""
    session_root = sessions.session_folder(stage, session) + "/session.usda"
    participant = sessions.choose_participant(session_root, user)
    link = LiveLink(stage, session, participant)
    try:
        link.start()
    except Exception:
        link.leave()
        raise
    return link


def leave_active():
    if _active is not None:
        _active.leave()
