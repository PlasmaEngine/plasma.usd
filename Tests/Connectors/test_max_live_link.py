"""Tests the 3ds Max connector's live link (Connectors/3dsMax/Contents/python/plasma_usd_live/live_link.py) against a
fake host that behaves like 3ds Max's scene and USD importer: nodes with world matrices that children follow, a Y-up
centimetre stage imported into a Z-up inch scene. Needs pxr, so run it with a Python that has it, such as Houdini's:

    hython Tests/Connectors/test_max_live_link.py

Prints one line per check and exits with the number of failures.
"""

import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "Connectors", "3dsMax", "Contents", "python"))

from pxr import Gf, Sdf, Usd, UsdGeom  # noqa: E402

from plasma_usd_live import live_link, meshes, sessions  # noqa: E402

STAGE = """#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 0.01
    upAxis = "Y"
)

def Xform "World"
{
    def Cube "Box"
    {
        double3 xformOp:translate = (10, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }

    def Xform "Group"
    {
        double3 xformOp:translate = (0, 5, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]

        def Cube "Child"
        {
            double3 xformOp:translate = (1, 2, 3)
            uniform token[] xformOpOrder = ["xformOp:translate"]
        }
    }
}
"""

# Y-up centimetres to Z-up inches, as 3ds Max's importer would put it on the top nodes.
TRUE_CONVERSION = (Gf.Matrix4d().SetScale(Gf.Vec3d(1 / 2.54, 1 / 2.54, 1 / 2.54))
                   * Gf.Matrix4d().SetRotate(Gf.Rotation(Gf.Vec3d(1, 0, 0), 90.0)))

failures = 0


def check(what, ok):
    global failures
    print(("PASS " if ok else "FAIL ") + what, flush=True)
    if not ok:
        failures += 1


def close(a, b, eps=1e-4):
    return all(abs(a[i][j] - b[i][j]) <= eps for i in range(4) for j in range(4))


class Node:
    def __init__(self, name, parent, local):
        self.name = name
        self.parent = parent  # node id or None
        self.local = Gf.Matrix4d(local)  # relative to the parent, like 3ds Max keeps it
        self.hidden = False
        self.tag = None
        self.mesh = None


class FakeHost(live_link.Host):
    tool = "3ds Max (fake)"

    def __init__(self):
        self.scene = {}
        self.next_id = 100
        self.file_tag = None
        self.started = False

    # scene helpers for the test
    def add(self, name, parent=None, world=None, mesh=None):
        node_id = self.next_id
        self.next_id += 1
        node = Node(name, parent, Gf.Matrix4d(1))
        node.mesh = mesh
        self.scene[node_id] = node
        if world is not None:
            self.set_world(node_id, world)
        return node_id

    def remove(self, node_id):
        """Deletes a node and its children; returns them for an undo."""
        removed = {node_id: self.scene.pop(node_id)}
        for child in [n for n, node in self.scene.items() if node.parent == node_id]:
            removed.update(self.remove(child))
        return removed

    def find(self, name):
        return next(n for n, node in self.scene.items() if node.name == name)

    # live_link.Host
    def meters_per_unit(self):
        return 0.0254

    def read_tag(self):
        return self.file_tag

    def write_tag(self, tag):
        self.file_tag = tag

    def import_stage(self, path):
        stage = Usd.Stage.Open(path)
        cache = UsdGeom.XformCache()
        made = []
        ids = {}
        for prim in stage.Traverse():
            world = cache.GetLocalToWorldTransform(prim) * TRUE_CONVERSION
            parent = ids.get(str(prim.GetPath().GetParentPath()))
            node_id = self.add(prim.GetName(), parent, world)
            ids[str(prim.GetPath())] = node_id
            made.append((node_id, prim.GetName(), parent))
        return made

    def nodes(self):
        return {n: node.tag for n, node in self.scene.items() if node.tag}

    def all_nodes(self):
        return set(self.scene)

    def exists(self, node):
        return node in self.scene

    def name(self, node):
        return self.scene[node].name

    def parent(self, node):
        return self.scene[node].parent

    def tag_node(self, node, path):
        self.scene[node].tag = path

    def world(self, node):
        n = self.scene[node]
        return n.local if n.parent is None else n.local * self.world(n.parent)

    def set_world(self, node, m):
        n = self.scene[node]
        n.local = Gf.Matrix4d(m) if n.parent is None else Gf.Matrix4d(m) * self.world(n.parent).GetInverse()

    def hidden(self, node):
        return self.scene[node].hidden

    def set_hidden(self, node, hidden):
        self.scene[node].hidden = hidden

    def is_geometry(self, node):
        return self.scene[node].mesh is not None

    def geometry_key(self, node):
        data = self.scene[node].mesh
        return (len(data.points), tuple(data.points[0])) if data else None

    def mesh(self, node):
        return self.scene[node].mesh

    def start(self, link):
        self.started = True

    def stop(self, link):
        self.started = False


def tick(link):
    link.next_file_tick = 0.0
    link.next_scan = 0.0
    link.tick()
    link.save_if_due(force=True)


def own_spec(link, path):
    return Sdf.Layer.FindOrOpen(link.layer_path).GetPrimAtPath(path)


def run(folder):
    stage_path = os.path.join(folder, "Asset.usda")
    with open(stage_path, "w", encoding="utf-8") as f:
        f.write(STAGE)

    host = FakeHost()
    host.add("Teapot", world=Gf.Matrix4d(1), mesh=meshes.MeshData([(0, 0, 0), (1, 0, 0), (0, 1, 0)], [(0, 1, 2)]))
    link = live_link.join_session(stage_path, "test", "tester", host)

    check("joins as max-tester", link.participant == "max-tester")
    check("imports and tags every prim", sorted(link.prims) == ["/World", "/World/Box", "/World/Group",
                                                                  "/World/Group/Child"])
    check("measures the importer's conversion", close(link.conversion, TRUE_CONVERSION))
    check("writes the scene tag", host.file_tag and sessions.same_path(host.file_tag["stage"], stage_path))
    check("keeps nodes from before the join local", host.find("Teapot") in link.local_nodes)
    tick(link)
    check("an idle tick writes nothing", own_spec(link, "/World/Box") is None)

    # 3ds Max -> USD: move a node 10 inches up (Z), which is +Y 25.4 cm in the stage.
    box = host.find("Box")
    host.set_world(box, host.world(box) * Gf.Matrix4d().SetTranslate(Gf.Vec3d(0, 0, 10)))
    tick(link)
    spec = own_spec(link, "/World/Box")
    value = spec.attributes["xformOp:transform"].default if spec and "xformOp:transform" in spec.attributes else None
    check("a moved node writes its transform", value is not None
          and Gf.IsClose(value.ExtractTranslation(), Gf.Vec3d(10, 25.4, 0), 1e-3))

    # Moving the parent writes only the parent; the child follows in both.
    group = host.find("Group")
    host.set_world(group, host.world(group) * Gf.Matrix4d().SetTranslate(Gf.Vec3d(5, 0, 0)))
    tick(link)
    check("moving a parent writes the parent", own_spec(link, "/World/Group") is not None)
    check("its child keeps its local transform", own_spec(link, "/World/Group/Child") is None)

    # USD -> 3ds Max: a stronger participant moves the child.
    other = Sdf.Layer.CreateNew(os.path.join(link.session_dir, "plasma-other.usda"))
    child_spec = Sdf.CreatePrimInLayer(other, "/World/Group/Child")
    child_spec.specifier = Sdf.SpecifierOver
    attr = Sdf.AttributeSpec(child_spec, "xformOp:transform", Sdf.ValueTypeNames.Matrix4d)
    attr.default = Gf.Matrix4d().SetTranslate(Gf.Vec3d(0, 0, 50))
    order = Sdf.AttributeSpec(child_spec, "xformOpOrder", Sdf.ValueTypeNames.TokenArray,
                              variability=Sdf.VariabilityUniform)
    order.default = ["xformOp:transform"]
    other.Save()
    session = Sdf.Layer.FindOrOpen(link.session_root)
    session.subLayerPaths.insert(0, "./plasma-other.usda")
    session.Save()
    os.utime(link.session_root, (os.path.getmtime(link.session_root) + 2,) * 2)
    tick(link)
    child = host.find("Child")
    expected = (Gf.Matrix4d().SetTranslate(Gf.Vec3d(0, 0, 50))
                * UsdGeom.Xformable(link.stage.GetPrimAtPath("/World/Group")).ComputeLocalToWorldTransform(0)
                * TRUE_CONVERSION)
    check("a stronger layer's move reaches the node", close(host.world(child), expected))
    tick(link)
    check("applying it is not written back", own_spec(link, "/World/Group/Child") is None)

    # The other layer changes again, through its file.
    attr.default = Gf.Matrix4d().SetTranslate(Gf.Vec3d(0, 0, 70))
    other.Save()
    os.utime(other.realPath, (os.path.getmtime(other.realPath) + 4,) * 2)
    tick(link)
    world_z = (host.world(child) * TRUE_CONVERSION.GetInverse()).ExtractTranslation()[2]
    check("a participant's file change is pulled", abs(world_z - 70) < 1e-3)

    # Hub layers.
    hub_usda = other.ExportToString().replace("(0, 0, 70, 1)", "(0, 0, 90, 1)")
    link.apply_hub_layers([{"layer": other.realPath, "usda": hub_usda}])
    world_z = (host.world(child) * TRUE_CONVERSION.GetInverse()).ExtractTranslation()[2]
    check("a hub layer message is applied", abs(world_z - 90) < 1e-3)

    # Visibility both ways.
    host.set_hidden(box, True)
    tick(link)
    spec = own_spec(link, "/World/Box")
    check("hiding a node makes its prim invisible",
          spec.attributes["visibility"].default == UsdGeom.Tokens.invisible)
    host.set_hidden(box, False)
    tick(link)
    check("unhiding makes it inherited again",
          own_spec(link, "/World/Box").attributes["visibility"].default == UsdGeom.Tokens.inherited)

    # Delete deactivates; the undo of it activates again.
    removed = host.remove(box)
    tick(link)
    spec = own_spec(link, "/World/Box")
    check("deleting a node deactivates its prim", spec is not None and spec.HasInfo("active") and not spec.active)
    host.scene.update(removed)
    link.after_undo = True
    tick(link)
    spec = own_spec(link, "/World/Box")
    check("undoing the delete activates it again", spec is None or not spec.HasInfo("active"))

    # An undo that takes a node away is not a delete.
    removed = host.remove(box)
    link.after_undo = True
    tick(link)
    spec = own_spec(link, "/World/Box")
    check("a node an undo takes away is kept", (spec is None or not spec.HasInfo("active"))
          and link.prims["/World/Box"].state == live_link.ABSENT)
    host.scene.update(removed)
    tick(link)
    check("it is linked again when it comes back", link.prims["/World/Box"].state == live_link.PRESENT)

    # A copy carries the tag and is untagged.
    copy = host.add("Box001", world=host.world(box))
    host.scene[copy].tag = "/World/Box"
    tick(link)
    check("a copy of a linked node is untagged", host.scene[copy].tag is None
          and host.scene[box].tag == "/World/Box")
    host.remove(copy)

    # A geometry node made in the session becomes a Mesh prim under its parent's prim.
    tri = meshes.MeshData([(0, 0, 0), (10, 0, 0), (0, 10, 0)], [(0, 1, 2)], [(0, 0), (1, 0), (0, 1)], [(0, 1, 2)])
    added = host.add("New Mesh", parent=group, world=host.world(group), mesh=tri)
    tick(link)
    path = host.scene[added].tag
    spec = own_spec(link, path) if path else None
    check("a new geometry node is added as a Mesh prim", path == "/World/Group/New_Mesh" and spec is not None
          and spec.specifier == Sdf.SpecifierDef and spec.typeName == "Mesh")
    check("it gets points, faces and st", spec is not None and len(spec.attributes["points"].default) == 3
          and "primvars:st" in spec.attributes)
    host.scene[added].mesh = meshes.MeshData([(0, 0, 0), (20, 0, 0), (0, 20, 0), (20, 20, 0)],
                                             [(0, 1, 2), (1, 3, 2)])
    tick(link)
    spec = own_spec(link, path)
    check("editing its geometry rewrites the mesh", len(spec.attributes["points"].default) == 4
          and "primvars:st" not in spec.attributes)
    host.remove(added)
    tick(link)
    check("deleting it removes the prim", own_spec(link, path) is None)

    # Leaving and joining again adopts the tagged nodes instead of importing.
    count = len(host.scene)
    link.leave()
    check("leaving stops the host", not host.started)
    link = live_link.join_session(stage_path, "test", "tester", host)
    check("joining again imports nothing", len(host.scene) == count)
    check("joining again adopts the tagged nodes", link.prims["/World/Box"].node == box
          and close(link.conversion, TRUE_CONVERSION))
    tick(link)
    check("joining again writes nothing new", own_spec(link, "/World/Group/Child") is None)
    link.leave()

    composed = Usd.Stage.Open(link.session_root)
    check("the session composes with this layer", bool(composed.GetPrimAtPath("/World/Box")))


def main():
    folder = tempfile.mkdtemp(prefix="plasma_max_live_")
    try:
        run(folder)
    finally:
        live_link.leave_active(quitting=True)
        shutil.rmtree(folder, ignore_errors=True)
    print("{} failure(s)".format(failures))
    return failures


if __name__ == "__main__":
    sys.exit(main())
