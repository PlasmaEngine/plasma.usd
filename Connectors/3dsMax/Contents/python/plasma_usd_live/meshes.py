"""Mesh prims for geometry nodes added in 3ds Max.

The host reads a node's mesh as triangles in the node's own space, object offset included (max_host.MaxHost.mesh), and
this module writes it as a Mesh prim: points, face vertex counts and indices, extent, and the first map channel as a
face-varying ``primvars:st`` when the node has one. 3ds Max triangles wind counter-clockwise like USD's default, so the
orientation stays rightHanded. Smoothing groups and normals are not written; the prim is a plain polygonal mesh
(subdivisionScheme none). pxr only.
"""

from pxr import Gf, Sdf, UsdGeom, Vt


class MeshData:
    """Triangles of a node: points (x, y, z), faces (a, b, c) indexing points, and optionally uvs (u, v) with
    uv_faces (a, b, c) indexing uvs."""

    __slots__ = ("points", "faces", "uvs", "uv_faces")

    def __init__(self, points, faces, uvs=None, uv_faces=None):
        self.points = points
        self.faces = faces
        self.uvs = uvs
        self.uv_faces = uv_faces


def author_mesh(stage, path, data):
    """Defines (or redefines) a Mesh prim at path in the stage's edit target from MeshData."""
    mesh = UsdGeom.Mesh.Define(stage, path)
    points = Vt.Vec3fArray([Gf.Vec3f(*p) for p in data.points])
    indices = Vt.IntArray([i for face in data.faces for i in face])
    mesh.CreatePointsAttr().Set(points)
    mesh.CreateFaceVertexCountsAttr().Set(Vt.IntArray([3] * len(data.faces)))
    mesh.CreateFaceVertexIndicesAttr().Set(indices)
    mesh.CreateSubdivisionSchemeAttr().Set(UsdGeom.Tokens.none)
    mesh.CreateOrientationAttr().Set(UsdGeom.Tokens.rightHanded)
    extent = UsdGeom.Mesh.ComputeExtent(points)
    if extent is not None and len(extent) == 2:
        mesh.CreateExtentAttr().Set(extent)

    primvars = UsdGeom.PrimvarsAPI(mesh.GetPrim())
    if data.uvs and data.uv_faces and len(data.uv_faces) == len(data.faces):
        st = primvars.CreatePrimvar("st", Sdf.ValueTypeNames.TexCoord2fArray, UsdGeom.Tokens.faceVarying)
        st.Set(Vt.Vec2fArray([Gf.Vec2f(*uv) for uv in data.uvs]))
        st.SetIndices(Vt.IntArray([i for face in data.uv_faces for i in face]))
    elif primvars.HasPrimvar("st"):
        primvars.RemovePrimvar("st")
    return mesh
