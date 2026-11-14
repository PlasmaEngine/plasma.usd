"""3ds Max for the live link: USD for 3ds Max (its importer and its pxr), nodes, the scene tag, the timer and the scene
callbacks.

Nodes are named by their anim handles (getHandleByAnim), which stay the same while 3ds Max runs, also across the undo
of a delete. A node's prim path is its user property ``usd_prim_path``, so it survives saving the scene, copying the
node (a copy is untagged by the link) and merging. The scene's link tag is a custom file property, ``plasmaUsdLive``.

Bulk reads run as MAXScript functions defined once (MXS below), so a node costs one call from Python rather than one per
value. What the session does to nodes is not undoable and never animated: undoing in 3ds Max takes back only 3ds Max's
own edits.

pxr comes with USD for 3ds Max (from the 3ds Max installer or the Autodesk App Store). Its Python packages are on 3ds
Max's path once the plug-in loads; ensure_usd() checks it is loaded, and looks for its python folder when pxr is still
not importable.
"""

import glob
import json
import os
import sys
import traceback

import pymxs
from pymxs import runtime as rt

PRIM_PATH_PROP = "usd_prim_path"
TAG_PROPERTY = "plasmaUsdLive"
CALLBACK_ID = "plasmaUsdLive"

MXS = r"""
global plasmaUsdLive_tagged, plasmaUsdLive_all, plasmaUsdLive_tm, plasmaUsdLive_geoKey, plasmaUsdLive_mesh

fn plasmaUsdLive_tagged = (
    local handles = #()
    local paths = #()
    for n in objects do (
        local p = getUserProp n "usd_prim_path"
        if p != undefined and (p as string) != "" do (
            append handles (getHandleByAnim n)
            append paths (p as string)
        )
    )
    #(handles, paths)
)

fn plasmaUsdLive_all = (
    for n in objects collect (getHandleByAnim n)
)

fn plasmaUsdLive_tm n = (
    local m = n.transform
    #(m.row1.x, m.row1.y, m.row1.z, m.row2.x, m.row2.y, m.row2.z, m.row3.x, m.row3.y, m.row3.z, m.row4.x, m.row4.y, m.row4.z)
)

fn plasmaUsdLive_geoKey n = (
    local counts = try (getPolygonCount n) catch #(0, 0)
    local box = try (nodeLocalBoundingBox n) catch #([0,0,0], [0,0,0])
    #(counts[1], counts[2], box[1].x, box[1].y, box[1].z, box[2].x, box[2].y, box[2].z)
)

fn plasmaUsdLive_mesh n = (
    local m = snapshotAsMesh n
    local toNode = inverse n.transform
    local points = #()
    local faces = #()
    local uvs = #()
    local uvFaces = #()
    for i = 1 to m.numverts do (
        local p = (getVert m i) * toNode
        append points p.x; append points p.y; append points p.z
    )
    for i = 1 to m.numfaces do (
        local f = getFace m i
        append faces (f.x as integer); append faces (f.y as integer); append faces (f.z as integer)
    )
    if meshop.getMapSupport m 1 do (
        for i = 1 to (meshop.getNumMapVerts m 1) do (
            local uv = meshop.getMapVert m 1 i
            append uvs uv.x; append uvs uv.y
        )
        for i = 1 to m.numfaces do (
            local f = meshop.getMapFace m 1 i
            append uvFaces (f.x as integer); append uvFaces (f.y as integer); append uvFaces (f.z as integer)
        )
    )
    delete m
    #(points, faces, uvs, uvFaces)
)
"""

UNIT_METERS = {
    "inches": 0.0254, "feet": 0.3048, "miles": 1609.344, "millimeters": 0.001, "centimeters": 0.01, "meters": 1.0,
    "kilometers": 1000.0,
}

_defined = False


def define_helpers():
    global _defined
    if not _defined:
        rt.execute(MXS)
        _defined = True


def max_year():
    """The product year, 2025 say; 0 when 3ds Max does not say."""
    try:
        return int(rt.maxVersion()[7])
    except Exception:
        return 0


def tool_name():
    year = max_year()
    return "3ds Max {}".format(year) if year else "3ds Max"


def _python_folders():
    """Folders holding a pxr package, from the places USD for 3ds Max installs to; this version's first."""
    roots = []
    for variable in ("ProgramData", "ProgramFiles", "APPDATA"):
        base = os.environ.get(variable)
        if base:
            roots.append(os.path.join(base, "Autodesk", "ApplicationPlugins"))
    try:
        roots.append(rt.getDir(rt.Name("maxroot")))
    except Exception:
        pass

    found = []
    for root in roots:
        for pattern in ("*USD*/**/pxr/__init__.py", "**/USD*/**/pxr/__init__.py"):
            for init in glob.glob(os.path.join(root, pattern), recursive=True):
                folder = os.path.dirname(os.path.dirname(init))
                if folder not in found:
                    found.append(folder)
    year = str(max_year())
    found.sort(key=lambda folder: year not in folder)
    return found


def ensure_usd():
    """Loads USD for 3ds Max and makes pxr importable; raises RuntimeError saying what is missing."""
    importer = getattr(rt, "USDImporter", None)
    if importer is None:
        raise RuntimeError("USD for 3ds Max is not installed or not loaded. Install it with the 3ds Max installer or "
                           "from the Autodesk App Store, and check it is loaded in the Plug-in Manager.")
    try:
        import pxr  # noqa: F401
        return
    except ImportError:
        pass

    for folder in _python_folders():
        if folder not in sys.path:
            sys.path.append(folder)
        # pxr's modules load the USD libraries beside them, in bin or lib a few levels up.
        parent = folder
        for _ in range(4):
            parent = os.path.dirname(parent)
            for name in ("bin", "lib"):
                candidate = os.path.join(parent, name)
                if os.path.isdir(candidate) and hasattr(os, "add_dll_directory"):
                    try:
                        os.add_dll_directory(candidate)
                    except OSError:
                        pass
        try:
            import pxr  # noqa: F401,F811
            return
        except ImportError:
            continue
    raise RuntimeError("USD for 3ds Max is loaded, but its Python packages (pxr) were not found.")


def meters_per_unit():
    try:
        unit = str(rt.units.SystemType).lower()
        return UNIT_METERS.get(unit, 0.0254) * float(rt.units.SystemScale)
    except Exception:
        return 0.0254


def node_of(handle):
    if handle is None:
        return None
    try:
        node = rt.getAnimByHandle(handle)
    except Exception:
        return None
    if node is None or not rt.isValidNode(node):
        return None
    return node


def handle_of(node):
    return int(rt.getHandleByAnim(node))


def scene_file():
    path = str(rt.maxFilePath or "") + str(rt.maxFileName or "")
    return path.replace("\\", "/")


class MaxHost:
    """live_link.Host for 3ds Max."""

    def __init__(self):
        from pxr import Gf  # importable once ensure_usd() ran
        self.Gf = Gf
        self.tool = tool_name()
        self.link = None
        self.timer = None
        self.events = None  # the NodeEventCallback; None when it could not be made
        self.moved = set()
        self.callbacks = False
        define_helpers()

    # -- scene -----------------------------------------------------------------------------------------------

    def document(self):
        return scene_file()

    def meters_per_unit(self):
        return meters_per_unit()

    def read_tag(self):
        custom = rt.Name("custom")
        index = rt.fileProperties.findProperty(custom, TAG_PROPERTY)
        if not index:
            return None
        try:
            return json.loads(str(rt.fileProperties.getPropertyValue(custom, index)))
        except ValueError:
            return None

    def write_tag(self, tag):
        rt.fileProperties.addProperty(rt.Name("custom"), TAG_PROPERTY, json.dumps(tag))

    def import_stage(self, path):
        before = set(self.all_nodes())
        importer = rt.USDImporter
        options = None
        try:
            options = importer.CreateOptions()
        except Exception:
            options = None
        with pymxs.undo(False):
            if options is not None:
                importer.ImportFile(path, importOptions=options)
            else:
                importer.ImportFile(path)
        made = []
        for handle in self.all_nodes():
            if handle in before:
                continue
            node = node_of(handle)
            if node is None:
                continue
            parent = node.parent
            made.append((handle, str(node.name), handle_of(parent) if parent is not None else None))
        try:
            rt.execute("max zoomext sel all")
        except Exception:
            pass
        return made

    def nodes(self):
        handles, paths = rt.plasmaUsdLive_tagged()
        return {int(h): str(p) for h, p in zip(handles, paths)}

    def all_nodes(self):
        return {int(h) for h in rt.plasmaUsdLive_all()}

    def exists(self, node):
        return node_of(node) is not None

    def name(self, node):
        n = node_of(node)
        return str(n.name) if n is not None else ""

    def parent(self, node):
        n = node_of(node)
        if n is None or n.parent is None:
            return None
        return handle_of(n.parent)

    def tag_node(self, node, path):
        n = node_of(node)
        if n is None:
            return
        with pymxs.undo(False):
            rt.setUserProp(n, PRIM_PATH_PROP, path or "")

    # -- transforms and visibility ---------------------------------------------------------------------------

    def world(self, node):
        n = node_of(node)
        if n is None:
            return self.Gf.Matrix4d(1)
        v = [float(x) for x in rt.plasmaUsdLive_tm(n)]
        return self.Gf.Matrix4d(v[0], v[1], v[2], 0, v[3], v[4], v[5], 0, v[6], v[7], v[8], 0, v[9], v[10], v[11], 1)

    def set_world(self, node, m):
        n = node_of(node)
        if n is None:
            return
        rows = [rt.Point3(m[i][0], m[i][1], m[i][2]) for i in range(4)]
        with pymxs.undo(False), pymxs.animate(False):
            n.transform = rt.Matrix3(rows[0], rows[1], rows[2], rows[3])

    def hidden(self, node):
        n = node_of(node)
        return bool(n.isHidden) if n is not None else False

    def set_hidden(self, node, hidden):
        n = node_of(node)
        if n is not None:
            with pymxs.undo(False):
                n.isHidden = hidden

    def take_moved(self):
        if self.events is None:
            return None
        moved, self.moved = self.moved, set()
        return moved

    # -- geometry --------------------------------------------------------------------------------------------

    def is_geometry(self, node):
        n = node_of(node)
        if n is None:
            return False
        try:
            if rt.superClassOf(n) != rt.GeometryClass:
                return False
            # Targets, bones and biped parts are geometry to 3ds Max, but not meshes anyone adds to a scene.
            skipped = [getattr(rt, name, None) for name in ("TargetObject", "BoneGeometry", "Biped_Object")]
            if rt.classOf(n) in [c for c in skipped if c is not None]:
                return False
            return bool(rt.canConvertTo(n, rt.TriMeshGeometry))
        except Exception:
            return False

    def geometry_key(self, node):
        n = node_of(node)
        if n is None:
            return None
        return tuple(round(float(x), 5) for x in rt.plasmaUsdLive_geoKey(n))

    def mesh(self, node):
        from .meshes import MeshData
        n = node_of(node)
        if n is None:
            return None
        try:
            points, faces, uvs, uv_faces = rt.plasmaUsdLive_mesh(n)
        except Exception:
            traceback.print_exc()
            return None
        p = [float(x) for x in points]
        f = [int(x) - 1 for x in faces]
        if not f:
            return None
        u = [float(x) for x in uvs]
        uf = [int(x) - 1 for x in uv_faces]
        return MeshData([tuple(p[i:i + 3]) for i in range(0, len(p), 3)],
                        [tuple(f[i:i + 3]) for i in range(0, len(f), 3)],
                        [tuple(u[i:i + 2]) for i in range(0, len(u), 2)] or None,
                        [tuple(uf[i:i + 3]) for i in range(0, len(uf), 3)] or None)

    # -- loop and callbacks ----------------------------------------------------------------------------------

    def start(self, link):
        from .qt import QtCore
        self.link = link
        self.timer = QtCore.QTimer()
        self.timer.setSingleShot(True)
        self.timer.timeout.connect(self.on_timer)
        self.timer.start(50)

        # Python functions as MAXScript callbacks; without them the link looks at every node on every tick.
        try:
            self.events = rt.NodeEventCallback(controllerOtherEvent=self.on_moved, added=self.on_scene_changed,
                                               deleted=self.on_scene_changed, geometryChanged=self.on_scene_changed,
                                               topologyChanged=self.on_scene_changed, mouseUp=False, delay=0)
        except Exception:
            traceback.print_exc()
            self.events = None
        try:
            add = rt.callbacks.addScript
            cid = rt.Name(CALLBACK_ID)
            for event, handler in (("sceneUndo", self.on_undo), ("sceneRedo", self.on_undo),
                                   ("systemPreReset", self.on_scene_replaced), ("systemPreNew", self.on_scene_replaced),
                                   ("filePreOpen", self.on_scene_replaced), ("preSystemShutdown", self.on_shutdown),
                                   ("filePostSave", self.on_saved)):
                add(rt.Name(event), handler, id=cid)
            self.callbacks = True
        except Exception:
            traceback.print_exc()
            self.callbacks = False
            link.note("3ds Max callbacks are unavailable; undo is treated as editing")

    def stop(self, link):
        if self.timer is not None:
            self.timer.stop()
            self.timer = None
        if self.events is not None:
            try:
                self.events.enabled = False
            except Exception:
                pass
            self.events = None
        if self.callbacks:
            try:
                rt.callbacks.removeScripts(id=rt.Name(CALLBACK_ID))
            except Exception:
                pass
            self.callbacks = False
        self.link = None

    def on_timer(self):
        link = self.link
        if link is None or self.timer is None:
            return
        seconds = link.tick()
        if seconds is not None and self.timer is not None:
            self.timer.start(max(1, int(seconds * 1000)))

    def on_moved(self, event, handles):
        for handle in handles:
            self.moved.add(int(handle))

    def on_scene_changed(self, event, handles):
        if self.link is not None:
            self.link.scene_changed = True

    def on_undo(self):
        if self.link is not None:
            self.link.after_undo = True

    def on_scene_replaced(self):
        """Reset, New and Open replace the linked nodes, so the session is left first."""
        if self.link is not None:
            self.link.leave()

    def on_shutdown(self):
        if self.link is not None:
            self.link.leave(quitting=True)

    def on_saved(self):
        link = self.link
        if link is not None and link.presence is not None:
            link.presence.info["document"] = scene_file()
            link.presence.refresh(force=True)

    def redraw(self):
        try:
            rt.redrawViews()
        except Exception:
            pass


def join_session(stage, session, user):
    """Joins or starts a session on a USD stage as max-<user>; returns the live link."""
    ensure_usd()
    from . import live_link
    return live_link.join_session(stage, session, user, MaxHost())


def select_prim(link, prim_path):
    st = link.prims.get(prim_path)
    node = node_of(st.node) if st is not None else None
    if node is not None:
        rt.select(node)
