"""Mesh prims for mesh objects added in Blender.

The object's evaluated mesh (modifiers applied, edit-mode changes included) is written in its local space, which is
also the prim's local space: points, faceVertexCounts, faceVertexIndices, face-varying normals, the active UV map as
face-varying primvars:st, primvars:displayColor from the first material's base colour, and the extent.
"""

import numpy as np

import bpy

bpy.utils.expose_bundled_modules()
from pxr import Gf, Sdf, UsdGeom, Vt  # noqa: E402

from . import materials  # noqa: E402


def display_color(obj):
    for slot in obj.material_slots:
        if slot.material is not None:
            return materials.base_color(slot.material)
    return None


def author_mesh(stage, path, obj, depsgraph):
    """Writes the object's mesh into the prim at path in the stage's edit target, defining a Mesh prim there when no
    layer defines one yet."""
    prim = stage.GetPrimAtPath(path)
    mesh = UsdGeom.Mesh(prim) if prim and prim.IsDefined() else UsdGeom.Mesh.Define(stage, path)

    evaluated = obj.evaluated_get(depsgraph) if depsgraph is not None else obj
    data = evaluated.to_mesh()
    try:
        points = np.empty(len(data.vertices) * 3, np.float32)
        data.vertices.foreach_get("co", points)
        counts = np.empty(len(data.polygons), np.int32)
        data.polygons.foreach_get("loop_total", counts)
        indices = np.empty(len(data.loops), np.int32)
        data.loops.foreach_get("vertex_index", indices)
        normals = np.empty(len(data.loops) * 3, np.float32)
        data.corner_normals.foreach_get("vector", normals)
        uv_layer = data.uv_layers.active
        uvs = None
        if uv_layer is not None:
            uvs = np.empty(len(data.loops) * 2, np.float32)
            uv_layer.data.foreach_get("uv", uvs)
    finally:
        evaluated.to_mesh_clear()

    usd_points = Vt.Vec3fArray.FromNumpy(points.reshape(-1, 3))
    mesh.CreatePointsAttr().Set(usd_points)
    mesh.CreateFaceVertexCountsAttr().Set(Vt.IntArray.FromNumpy(counts))
    mesh.CreateFaceVertexIndicesAttr().Set(Vt.IntArray.FromNumpy(indices))
    mesh.CreateNormalsAttr().Set(Vt.Vec3fArray.FromNumpy(normals.reshape(-1, 3)))
    mesh.SetNormalsInterpolation(UsdGeom.Tokens.faceVarying)
    mesh.CreateSubdivisionSchemeAttr().Set(UsdGeom.Tokens.none)
    mesh.CreateExtentAttr().Set(UsdGeom.PointBased.ComputeExtent(usd_points))

    primvars = UsdGeom.PrimvarsAPI(mesh)
    if uvs is not None:
        st = primvars.CreatePrimvar("st", Sdf.ValueTypeNames.TexCoord2fArray, UsdGeom.Tokens.faceVarying)
        st.Set(Vt.Vec2fArray.FromNumpy(uvs.reshape(-1, 2)))

    color = display_color(obj)
    if color is not None:
        mesh.CreateDisplayColorPrimvar(UsdGeom.Tokens.constant).Set(Vt.Vec3fArray([Gf.Vec3f(*color)]))

    return mesh.GetPrim()
