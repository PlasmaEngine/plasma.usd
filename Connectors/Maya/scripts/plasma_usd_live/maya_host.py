"""Maya's side of the live link: the maya-usd proxy shape on session.usda, the edit target, keeping the other
participants' layers out of maya-usd's saving, the Qt timer that ticks the link, and Maya's scene callbacks. MayaHost
implements the methods of live_link.Host.

Proxy shape: joining looks for a mayaUsdProxyShape tagged with the session (the string attribute plasmaUsdLiveSession
this connector adds), then for any proxy shape whose filePath is session.usda, and makes one only when there is none,
so joining again after reopening the Maya scene reuses it. A new one is made the way maya-usd's Create Stage from File
makes one (doCreateStage in mayaUsd_createStageFromFile.mel): a transform with a mayaUsdProxyShape under it, its load
rules set first to load everything (mayaUsd.lib.setLoadRulesAttribute), filePath set to session.usda as an absolute
path, time1.outTime connected to its time, and, where maya-usd has it (0.31 and later), the transform rotated and
scaled so the stage's up axis and units match Maya's (mayaUsdStageConversion, 'rotateScale'; the USD data is not
changed). The Create Stage from File options (prim path mask, paths relative to the Maya scene, overwriting Maya's
preferences) are not used. Making the proxy shape and setting the edit target is one undo step. The proxy shape is
found again by its UUID, so renaming or reparenting it does not matter.

Edit target: set with maya-usd's mayaUsdEditTarget command and read back with its query, which reports the layer edits
go to (with maya-usd's edit forwarding, its fallback layer). maya-usd stores the target in the Maya scene
(usdStageTargetLayer), so a reopened scene targets the layer again.

Saving: on Maya scene save maya-usd saves the dirty layers of each stage (optionVar mayaUsd_SerializedUsdEditsLocation:
1, the default, into their USD files, with its save dialog when mayaUsd_ConfirmExistingFileSave asks for it; 2 into the
Maya scene file; 3 not at all). This Maya's own layer is saved by the link within ~0.2 s of each change, so maya-usd
finds it clean or writes the same in-memory layer. The other participants' layers are system-locked
(mayaUsd.lib.systemLockLayer, maya-usd 0.28 and later): the save dialog leaves system-locked layers out, and the save
into USD files skips layers without permission to save. With an older maya-usd only the permission to save is taken
away, which the save into USD files skips too. Saving USD edits into the Maya scene file (option 2) stores dirty layers
whatever their lock; joining reads such restored content of other participants' layers from disk again.

Ticking: a single-shot QTimer on Maya's main thread, re-armed with the interval the link's tick returns; none in batch
mode, where the caller ticks.

Scene callbacks: File > New and File > Open leave the session first (MSceneMessage kBeforeNew, kBeforeOpen); a save
updates the presence file's document (kAfterSave); Maya exiting leaves without touching Maya (kMayaExiting, and
Python's atexit in live_link).
"""

import os
import re
import traceback

from maya import cmds

from . import sessions

PLUGIN = "mayaUsdPlugin"
PROXY_TYPE = "mayaUsdProxyShape"
TAG_ATTR = "plasmaUsdLiveSession"
NODE_PREFIX = "plasmaLive_"


def ensure_plugin():
    """Loads maya-usd; raises a RuntimeError saying what is missing when that fails."""
    try:
        if not cmds.pluginInfo(PLUGIN, query=True, loaded=True):
            cmds.loadPlugin(PLUGIN, quiet=True)
    except RuntimeError as e:
        raise RuntimeError("The maya-usd plug-in ({}) cannot be loaded ({}). Install maya-usd, or turn it on in "
                           "Windows > Settings/Preferences > Plug-in Manager.".format(PLUGIN, str(e).strip()))
    try:
        import mayaUsd.lib  # noqa: F401
        import mayaUsd.ufe  # noqa: F401
        from pxr import Usd  # noqa: F401
    except ImportError as e:
        raise RuntimeError("maya-usd is loaded, but its Python modules cannot be imported: {}".format(e))


def tool_name():
    name = "Maya {}".format(cmds.about(version=True))
    try:
        name += " (maya-usd {})".format(cmds.pluginInfo(PLUGIN, query=True, version=True))
    except RuntimeError:
        pass
    return name


def scene_name():
    return cmds.file(query=True, sceneName=True) or ""


# -- proxy shapes ----------------------------------------------------------------------------------------------------


def proxy_shapes():
    """Long names of the proxy shapes in the scene, but those from referenced files (maya-usd does not save those)."""
    shapes = cmds.ls(type=PROXY_TYPE, long=True) or []
    return [s for s in shapes if not cmds.referenceQuery(s, isNodeReferenced=True)]


def shape_session(shape):
    """The session root a proxy shape is tagged with, or None."""
    if cmds.attributeQuery(TAG_ATTR, node=shape, exists=True):
        return cmds.getAttr(shape + "." + TAG_ATTR) or None
    return None


def shape_file(shape):
    """The file a proxy shape shows, as an absolute path; maya-usd reads a relative one from the Maya scene's
    folder."""
    path = cmds.getAttr(shape + ".filePath") or ""
    if path and not os.path.isabs(path):
        scene = scene_name()
        if scene:
            path = os.path.join(os.path.dirname(scene), path)
    return path


def find_proxy(session_root):
    """The proxy shape showing a session: one tagged with it, else any on its session.usda, else None."""
    tagged = []
    showing = []
    for shape in proxy_shapes():
        tag = shape_session(shape)
        if tag and sessions.same_path(tag, session_root):
            tagged.append(shape)
        elif shape_file(shape) and sessions.same_path(shape_file(shape), session_root):
            showing.append(shape)
    found = tagged + showing
    return found[0] if found else None


def sessions_in_scene():
    """Session roots with a proxy shape in the scene."""
    roots = []
    for shape in proxy_shapes():
        root = shape_session(shape) or shape_file(shape)
        if root and os.path.basename(root).lower() == "session.usda":
            roots.append(root)
    return roots


def tag(shape, session_root):
    if not cmds.attributeQuery(TAG_ATTR, node=shape, exists=True):
        cmds.addAttr(shape, longName=TAG_ATTR, dataType="string")
    cmds.setAttr(shape + "." + TAG_ATTR, session_root, type="string")


def create_proxy(session_root, session_name):
    """A new transform and proxy shape on session.usda, as Create Stage from File makes them; returns the shape."""
    import mayaUsd.lib

    name = NODE_PREFIX + (re.sub(r"[^A-Za-z0-9_]", "_", session_name) or "session")
    transform = cmds.createNode("transform", name=name, skipSelect=True)
    shape = cmds.createNode(PROXY_TYPE, name=transform.split("|")[-1] + "Shape", parent=transform, skipSelect=True)
    shape = cmds.ls(shape, long=True)[0]

    # Load rules first, so the stage opens in the right state (what doCreateStage does).
    set_load_rules = getattr(mayaUsd.lib, "setLoadRulesAttribute", None)
    if set_load_rules is not None:
        set_load_rules(shape, True)
    cmds.setAttr(shape + ".filePath", session_root, type="string")
    if cmds.attributeQuery("filePathRelative", node=shape, exists=True):
        cmds.setAttr(shape + ".filePathRelative", False)
    cmds.connectAttr("time1.outTime", shape + ".time")
    tag(shape, session_root)

    try:
        import mayaUsdStageConversion
    except ImportError:
        mayaUsdStageConversion = None  # maya-usd before 0.31: the stage shows in its own up axis and units
    if mayaUsdStageConversion is not None:
        try:
            mayaUsdStageConversion.convertUpAxisAndUnit(shape, True, True, "rotateScale")
        except Exception:
            traceback.print_exc()
    return shape


def proxy_stage(shape):
    """The UsdStage maya-usd opened for a proxy shape."""
    import mayaUsd.lib
    import mayaUsd.ufe

    stage = None
    try:
        stage = mayaUsd.ufe.getStage(shape)
    except Exception:
        traceback.print_exc()
    if not stage:
        try:
            prim = mayaUsd.lib.GetPrim(shape)
            stage = prim.GetStage() if prim else None
        except Exception:
            traceback.print_exc()
    if not stage:
        raise RuntimeError("maya-usd opened no stage for {} (filePath {})".format(
            shape, cmds.getAttr(shape + ".filePath")))
    return stage


def lock_api():
    """mayaUsd.lib when it can system-lock layers (maya-usd 0.28 and later), else None."""
    try:
        import mayaUsd.lib as lib
    except ImportError:
        return None
    return lib if hasattr(lib, "systemLockLayer") and hasattr(lib, "isLayerSystemLocked") else None


class MayaHost:
    """The live link's host in Maya (see live_link.Host)."""

    def __init__(self):
        self.uuid = None  # of the proxy shape
        self.link = None
        self.timer = None
        self.callbacks = []

    def shape(self):
        """The proxy shape's long name now, or None once it was deleted."""
        found = cmds.ls(self.uuid, long=True) if self.uuid else None
        return found[0] if found else None

    # -- live_link.Host --------------------------------------------------------------------------------------

    def open_stage(self, link):
        shape = find_proxy(link.session_root)
        if shape is None:
            shape = create_proxy(link.session_root, link.session_name)
            link.note("Made the proxy shape " + shape)
        else:
            if not shape_session(shape):
                tag(shape, link.session_root)
            link.note("Using the proxy shape " + shape)
        self.uuid = cmds.ls(shape, uuid=True)[0]
        return proxy_stage(shape)

    def document(self):
        return scene_name()

    def set_edit_target(self, link):
        try:
            cmds.mayaUsdEditTarget(self.shape(), edit=True, editTarget=link.layer.identifier)
            return
        except (AttributeError, RuntimeError, TypeError) as e:
            link.note("mayaUsdEditTarget failed ({}); setting the stage's edit target directly".format(
                str(e).strip() or type(e).__name__))
        from pxr import Usd
        link.stage.SetEditTarget(Usd.EditTarget(link.layer))

    def edit_target(self, link):
        try:
            found = cmds.mayaUsdEditTarget(self.shape(), query=True, editTarget=True) or []
        except (AttributeError, RuntimeError, TypeError):
            found = []
        if found:
            from pxr import Sdf
            layer = Sdf.Layer.Find(found[0])
            if layer is not None:
                return layer
        return link.stage.GetEditTarget().GetLayer()

    def is_protected(self, layer):
        lib = lock_api()
        if lib is not None:
            return bool(lib.isLayerSystemLocked(layer))
        return not layer.permissionToSave

    def protect_layer(self, link, layer):
        lib = lock_api()
        if lib is not None:
            lib.systemLockLayer(self.shape() or "", layer)
        else:
            layer.SetPermissionToSave(False)

    def release_layer(self, link, layer):
        lib = lock_api()
        if lib is not None:
            lib.unlockLayer(self.shape() or "", layer)
        else:
            layer.SetPermissionToSave(True)

    def problem(self, link):
        shape = self.shape()
        if shape is None:
            return "The session's proxy shape was deleted; join again to make a new one"
        path = shape_file(shape)
        if not sessions.same_path(path or "?", link.session_root):
            return "{} shows {} now, not the session; join again".format(shape.split("|")[-1], path or "nothing")
        return None

    def start(self, link):
        self.link = link
        import maya.api.OpenMaya as om

        scene = om.MSceneMessage
        self.callbacks = [
            scene.addCallback(scene.kBeforeNew, self.on_before_new_or_open),
            scene.addCallback(scene.kBeforeOpen, self.on_before_new_or_open),
            scene.addCallback(scene.kAfterSave, self.on_after_save),
            scene.addCallback(scene.kMayaExiting, self.on_exiting),
        ]
        if cmds.about(batch=True):
            return
        from .qt import QtCore

        self.timer = QtCore.QTimer()
        self.timer.setSingleShot(True)
        self.timer.setTimerType(QtCore.Qt.PreciseTimer)
        self.timer.timeout.connect(self.on_timer)
        self.timer.start(0)

    def stop(self, link, quitting):
        """Stops the timer and removes the scene callbacks; quitting, Maya takes both down itself."""
        self.link = None
        if quitting:
            return
        if self.timer is not None:
            self.timer.stop()
            self.timer = None
        callbacks, self.callbacks = self.callbacks, []
        if callbacks:
            # Possibly called from one of these callbacks; they do nothing once self.link is None.
            import maya.api.OpenMaya as om
            import maya.utils

            maya.utils.executeDeferred(om.MMessage.removeCallbacks, callbacks)

    # -- Maya events -----------------------------------------------------------------------------------------

    def on_timer(self):
        link = self.link
        interval = link.tick() if link is not None else None
        if interval is None:
            if self.timer is not None:
                self.timer.stop()
        elif self.timer is not None:
            self.timer.start(max(1, int(interval * 1000.0)))

    def on_before_new_or_open(self, *_):
        """Another scene replaces this one and its proxy shape: leave first."""
        if self.link is not None:
            self.link.leave()

    def on_after_save(self, *_):
        if self.link is not None:
            self.link.document_saved(scene_name())

    def on_exiting(self, *_):
        if self.link is not None:
            self.link.leave(quitting=True)


def join_session(stage, session, user):
    """Joins or starts a session on a USD stage as maya-<user>, in one undo step; returns the live link."""
    ensure_plugin()
    from . import live_link

    undo = cmds.undoInfo(query=True, state=True)
    if undo:
        cmds.undoInfo(openChunk=True, chunkName="Join Plasma USD Live session '{}'".format(session))
    try:
        return live_link.join_session(stage, session, user, tool_name(), MayaHost())
    finally:
        if undo:
            cmds.undoInfo(closeChunk=True)


def select_proxy(link):
    shape = link.host.shape() if isinstance(link.host, MayaHost) else None
    if shape:
        cmds.select(shape, replace=True)


def select_prim(link, prim_path):
    """Selects a USD prim of the session's stage in Maya (the proxy shape's UFE path, then the prim's)."""
    shape = link.host.shape() if isinstance(link.host, MayaHost) else None
    if shape:
        try:
            cmds.select(shape + "," + prim_path, replace=True)
        except RuntimeError:
            traceback.print_exc()
