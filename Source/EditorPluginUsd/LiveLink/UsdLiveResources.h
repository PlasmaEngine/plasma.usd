#pragma once

#include <EditorPluginUsd/EditorPluginUsdDLL.h>
#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Types/Status.h>
#include <UsdBridge/UsdStage.h>

/// \brief Turns USD geometry and surfaces into mesh and material files, and sends them to the editor's engine process as
/// resource updates.
///
/// The bytes are a file's content, as written to the bake folder (see plUsdBakeStore). A resource id only has to be
/// referenced by a component in the engine process before its update arrives; the engine then replaces whatever it
/// loaded with the bytes sent here.
class PL_EDITORPLUGINUSD_DLL plUsdLiveResources
{
public:
  /// \brief The most vertices in one mesh part. Each part is its own file and resource, culled on its own, and keeps the
  /// memory one build and one engine load need bounded.
  static constexpr plUInt32 MaxVerticesPerPart = 250000;

  /// \brief The same for meshes with high-precision vertex data, whose vertices take half as much again.
  static constexpr plUInt32 MaxVerticesPerPartHighPrecision = 160000;

  /// \brief The share of a mesh's triangles its proxy keeps, see plModelImporter2::GenerateMeshletProxy.
  static constexpr float ProxyTriangleRatio = 1.0f / 16.0f;

  /// \brief Whether a mesh needs high-precision vertex data: when any texture coordinate lies outside [-1, 1]. Half
  /// floats step by 1/1024 above 1 and by 1/64 above 16, which on a 4096 texture is 4 and 64 texels: tiled textures
  /// would lose their detail.
  static bool NeedsHighPrecision(const plGeometry& geom);

  /// \brief The engine's standard lit material, used for geometry without a material of its own.
  static constexpr const char* DefaultMaterial = "{ 7c71fc2d-2a1a-4938-9305-f3a0f26d73cc }";

  /// \brief Whether any group's material has a normal map, the only thing the mesh's tangents are used for.
  static bool NeedsTangents(plArrayPtr<const plUsdMaterialGroup> groups);

  /// \brief Tessellates the source and serialises it as one or more mesh resources. Runs on any thread.
  ///
  /// materialResources holds one material per material group of the source (see plUsdMaterialGroup); material slot i of
  /// every part is group i. Without bTangents each vertex gets an arbitrary tangent perpendicular to its normal, which
  /// takes a fraction of the time MikkTSpace does on a large mesh and is all a material without a normal map needs.
  static plStatus BuildMeshParts(const plUsdGeometrySource& source, plArrayPtr<const plString> materialResources, plUInt64 uiContentHash,
    bool bTangents, plDynamicArray<plDynamicArray<plUInt8>>& out_parts);

  /// \brief BuildMeshParts for geometry already built: triangles ordered by material group, groupTriangles counting
  /// each group's triangles. Writes the geometry's tangents.
  static plStatus BuildMeshPartsFromGeometry(plGeometry& geom, plArrayPtr<const plUInt32> groupTriangles, plArrayPtr<const plString> materialResources,
    plUInt64 uiContentHash, bool bTangents, plDynamicArray<plDynamicArray<plUInt8>>& out_parts);

  /// \brief Serialises a material that derives from DefaultMaterial and overrides colour, metallic, roughness and
  /// textures from the surface. The surface's texture paths must already be ones the engine can open; an opacity mask
  /// is expected to be merged into the base colour texture's alpha. bBlendOpacityMask chooses how that mask is used:
  /// blended for soft coverage such as grime decals, otherwise as a cut-out.
  static void BuildMaterial(const plUsdSurface& surface, bool bBlendOpacityMask, plDynamicArray<plUInt8>& out_data);

  /// \brief Sends one resource update with a file's content. sAssetType is "Mesh" or "Material". Returns false when no
  /// engine process runs.
  static bool Send(plStringView sAssetType, plStringView sResourceId, const plDynamicArray<plUInt8>& data);

  /// \brief Has the engine load a resource again from its file, e.g. after the file was rewritten, in the background.
  /// Resources the engine has not loaded are not affected. Returns false when no engine process runs.
  static bool Reload(plStringView sAssetType, plStringView sResourceId);
};
