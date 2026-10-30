#include <EditorPluginUsd/EditorPluginUsdPCH.h>

#include <Core/Graphics/Geometry.h>
#include <EditorEngineProcessFramework/EngineProcess/EngineProcessMessages.h>
#include <EditorFramework/IPC/EngineProcessConnection.h>
#include <EditorPluginUsd/LiveLink/UsdLiveResources.h>
#include <Foundation/Algorithm/HashingUtils.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/Utilities/AssetFileHeader.h>
#include <ModelImporter2/MeshletLodGenerator.h>
#include <RendererCore/Meshes/MeshResourceDescriptor.h>

namespace
{
  void CopyStorage(const plContiguousMemoryStreamStorage& storage, plDynamicArray<plUInt8>& out_data)
  {
    out_data.SetCountUninitialized(storage.GetStorageSize32());
    if (!out_data.IsEmpty())
    {
      plMemoryUtils::Copy(out_data.GetData(), storage.GetData(), storage.GetStorageSize32());
    }
  }

  /// Consecutive triangles of one mesh part that share a material slot.
  struct SubMeshRange
  {
    plUInt32 m_uiMaterial = 0;
    plUInt32 m_uiFirstTriangle = 0;
    plUInt32 m_uiTriangles = 0;
  };

  void SerializeMesh(const plGeometry& geom, plArrayPtr<const plString> materials, plArrayPtr<const SubMeshRange> subMeshes, plUInt64 uiHash,
    bool bHighPrecision, plDynamicArray<plUInt8>& out_data)
  {
    plMeshResourceDescriptor desc;

    // Every part carries every slot, so slot i is group i in all of them.
    for (plUInt32 i = 0; i < materials.GetCount(); ++i)
      desc.SetMaterial(i, materials[i]);

    plMeshBufferResourceDescriptor& buffer = desc.MeshBufferDesc();
    buffer.AddCommonStreams(bHighPrecision);
    buffer.AllocateStreamsFromGeometry(geom, plGALPrimitiveTopology::Triangles);

    for (const SubMeshRange& subMesh : subMeshes)
      desc.AddSubMesh(subMesh.m_uiTriangles, subMesh.m_uiFirstTriangle, subMesh.m_uiMaterial);

    desc.ComputeBounds();

    // Meshlets with a LOD hierarchy, as mesh assets with virtual geometry get: the GPU culling picks a cut per instance,
    // so a distant copy of a million-triangle mesh draws a handful of clusters. A mesh too small for a hierarchy keeps
    // the classic whole-sub-mesh draw (Rendering.Culling.GPU.Meshlets.RequireLod).
    if (plModelImporter2::GenerateMeshletLods(desc).Failed())
      desc.ClearMeshlets();

    // A coarse proxy ahead of the full mesh: the engine shows it first and loads the full mesh only where a view needs
    // the detail (plMeshStreaming), so a scene of dense meshes neither waits for nor fills GPU memory with all of them.
    plMeshResourceDescriptor proxy;
    float fProxyLodError = 0.0f;
    const bool bProxy = !desc.GetMeshlets().IsEmpty() && plModelImporter2::GenerateMeshletProxy(desc, plUsdLiveResources::ProxyTriangleRatio, proxy, fProxyLodError).Succeeded();

    plContiguousMemoryStreamStorage storage;
    plMemoryStreamWriter writer(&storage);

    // A mesh file: the asset header, then the descriptor.
    plAssetFileHeader header;
    header.SetFileHashAndVersion(uiHash, 1);
    header.Write(writer).IgnoreResult();

    if (bProxy)
      desc.SaveWithProxy(writer, proxy, fProxyLodError);
    else
      desc.Save(writer);
    CopyStorage(storage, out_data);
  }
} // namespace

bool plUsdLiveResources::NeedsHighPrecision(const plGeometry& geom)
{
  for (const plGeometry::Vertex& vertex : geom.GetVertices())
  {
    if (plMath::Abs(vertex.m_vTexCoord.x) > 1.0f || plMath::Abs(vertex.m_vTexCoord.y) > 1.0f)
      return true;
  }

  return false;
}

bool plUsdLiveResources::NeedsTangents(plArrayPtr<const plUsdMaterialGroup> groups)
{
  for (const plUsdMaterialGroup& group : groups)
  {
    if (group.m_bHasSurface && !group.m_Surface.m_sNormalTexture.IsEmpty())
      return true;
  }

  return false;
}

plStatus plUsdLiveResources::BuildMeshParts(const plUsdGeometrySource& source, plArrayPtr<const plString> materialResources, plUInt64 uiContentHash,
  bool bTangents, plDynamicArray<plDynamicArray<plUInt8>>& out_parts)
{
  out_parts.Clear();

  plGeometry geom;
  plDynamicArray<plUInt32> groupTriangles;
  const plStatus res = source.Build(geom, &groupTriangles);
  if (res.Failed())
    return res;

  return BuildMeshPartsFromGeometry(geom, groupTriangles, materialResources, uiContentHash, bTangents, out_parts);
}

plStatus plUsdLiveResources::BuildMeshPartsFromGeometry(plGeometry& geom, plArrayPtr<const plUInt32> groupTriangles, plArrayPtr<const plString> materialResources,
  plUInt64 uiContentHash, bool bTangents, plDynamicArray<plDynamicArray<plUInt8>>& out_parts)
{
  out_parts.Clear();

  if (geom.GetPolygons().IsEmpty())
    return plStatus("the geometry has no triangles");

  if (groupTriangles.GetCount() != materialResources.GetCount())
    return plStatus(plFmt("the geometry has {} material groups but {} materials were given", groupTriangles.GetCount(), materialResources.GetCount()));

  if (bTangents)
  {
    geom.ComputeTangents();
  }
  else
  {
    for (plGeometry::Vertex& vertex : geom.GetVertices())
    {
      vertex.m_vTangent = vertex.m_vNormal.GetOrthogonalVector().GetNormalized();
      vertex.m_fBiTangentSign = 1.0f;
    }
  }

  const plDeque<plGeometry::Vertex>& vertices = geom.GetVertices();
  const plDeque<plGeometry::Polygon>& polygons = geom.GetPolygons();

  // Every part of a mesh uses the same vertex format.
  const bool bHighPrecision = NeedsHighPrecision(geom);
  const plUInt32 uiMaxVertices = bHighPrecision ? MaxVerticesPerPartHighPrecision : MaxVerticesPerPart;

  // Build orders the triangles by group, so each group is one range.
  if (vertices.GetCount() <= uiMaxVertices)
  {
    plHybridArray<SubMeshRange, 8> subMeshes;
    plUInt32 uiFirst = 0;
    for (plUInt32 uiGroup = 0; uiGroup < groupTriangles.GetCount(); ++uiGroup)
    {
      if (groupTriangles[uiGroup] > 0)
        subMeshes.PushBack({uiGroup, uiFirst, groupTriangles[uiGroup]});

      uiFirst += groupTriangles[uiGroup];
    }

    SerializeMesh(geom, materialResources, subMeshes, uiContentHash, bHighPrecision, out_parts.ExpandAndGetRef());
    return plStatus(PL_SUCCESS);
  }

  // Too large for one message: split by triangles into parts that each stay under the vertex budget. A vertex shared
  // by triangles in different parts is duplicated into each. A group that straddles two parts gets a range in both.
  plDynamicArray<plUInt32> vertexPart;
  plDynamicArray<plUInt32> vertexIndexInPart;
  vertexPart.SetCount(vertices.GetCount(), 0xFFFFFFFF);
  vertexIndexInPart.SetCountUninitialized(vertices.GetCount());

  plGeometry part;
  plHybridArray<SubMeshRange, 8> subMeshes;
  plUInt32 uiPart = 0;

  auto flush = [&]()
  {
    SerializeMesh(part, materialResources, subMeshes, uiContentHash + uiPart, bHighPrecision, out_parts.ExpandAndGetRef());
    part.Clear();
    subMeshes.Clear();
    ++uiPart;
  };

  plUInt32 uiPolygon = 0;
  for (plUInt32 uiGroup = 0; uiGroup < groupTriangles.GetCount(); ++uiGroup)
  {
    const plUInt32 uiGroupEnd = plMath::Min(uiPolygon + groupTriangles[uiGroup], polygons.GetCount());

    for (; uiPolygon < uiGroupEnd; ++uiPolygon)
    {
      const plGeometry::Polygon& poly = polygons[uiPolygon];

      plUInt32 uiNew = 0;
      for (plUInt32 v : poly.m_Vertices)
      {
        if (vertexPart[v] != uiPart)
          ++uiNew;
      }

      if (part.GetVertices().GetCount() + uiNew > uiMaxVertices)
      {
        flush();
      }

      if (subMeshes.IsEmpty() || subMeshes.PeekBack().m_uiMaterial != uiGroup)
        subMeshes.PushBack({uiGroup, part.GetPolygons().GetCount(), 0});

      ++subMeshes.PeekBack().m_uiTriangles;

      plGeometry::Polygon& newPoly = part.GetPolygons().ExpandAndGetRef();
      newPoly.m_vNormal = poly.m_vNormal;

      for (plUInt32 v : poly.m_Vertices)
      {
        if (vertexPart[v] != uiPart)
        {
          vertexPart[v] = uiPart;
          vertexIndexInPart[v] = part.GetVertices().GetCount();
          part.GetVertices().PushBack(vertices[v]);
        }

        newPoly.m_Vertices.PushBack(vertexIndexInPart[v]);
      }
    }
  }

  if (!part.GetPolygons().IsEmpty())
  {
    flush();
  }

  return plStatus(PL_SUCCESS);
}

void plUsdLiveResources::BuildMaterial(const plUsdSurface& surface, bool bBlendOpacityMask, plDynamicArray<plUInt8>& out_data)
{
  plContiguousMemoryStreamStorage storage;
  plMemoryStreamWriter w(&storage);

  plUInt64 uiHash = plHashingUtils::xxHash64String(surface.m_sMaterialPath);
  const float values[] = {surface.m_BaseColor.r, surface.m_BaseColor.g, surface.m_BaseColor.b, surface.m_fMetallic, surface.m_fRoughness, surface.m_fOpacity,
    surface.m_fOpacityThreshold, surface.m_bDoubleSided ? 1.0f : 0.0f, bBlendOpacityMask ? 1.0f : 0.0f};
  uiHash = plHashingUtils::xxHash64(values, sizeof(values), uiHash);
  for (const plString* pFile : {&surface.m_sBaseColorTexture, &surface.m_sNormalTexture, &surface.m_sRoughnessTexture, &surface.m_sMetallicTexture, &surface.m_sOpacityTexture})
    uiHash = plHashingUtils::xxHash64String(*pFile, uiHash);

  plAssetFileHeader header;
  header.SetFileHashAndVersion(uiHash, 1);
  header.Write(w).IgnoreResult();

  w << (plUInt8)8; // plBinMaterial version, as written by plMaterialAssetDocument::WriteMaterialAsset
  w << (plUInt8)0; // uncompressed

  w << plString(DefaultMaterial); // base material: supplies the render state; the constants are all written below
  w << plString();                // surface
  w << plString("Shaders/Materials/DefaultMaterial.plShader");

  // Permutations. An opacity mask wins over constant opacity: it cuts foliage cards and fences, and blends grime decals,
  // which a cut would turn into solid sheets. Constant opacity below one is glass: transparent.
  plHybridArray<std::pair<const char*, const char*>, 2> permutations;
  if (!surface.m_sOpacityTexture.IsEmpty())
    permutations.PushBack({"BLEND_MODE", bBlendOpacityMask ? "BLEND_MODE_TRANSPARENT" : "BLEND_MODE_MASKED"});
  else if (surface.m_fOpacity < 0.999f)
    permutations.PushBack({"BLEND_MODE", "BLEND_MODE_TRANSPARENT"});

  if (surface.m_bDoubleSided)
    permutations.PushBack({"TWO_SIDED", "TRUE"});

  w << (plUInt16)permutations.GetCount();
  for (const auto& perm : permutations)
  {
    w << plString(perm.first);
    w << plString(perm.second);
  }

  struct TextureSlot
  {
    const char* m_szTexture;
    const char* m_szUseFlag;
    const plString* m_pFile;
  };

  // Not called "slots": that is a Qt keyword macro, and Qt's headers are in scope here.
  const TextureSlot textureSlots[] = {
    {"BaseTexture", "UseBaseTexture", &surface.m_sBaseColorTexture},
    {"NormalTexture", "UseNormalTexture", &surface.m_sNormalTexture},
    {"RoughnessTexture", "UseRoughnessTexture", &surface.m_sRoughnessTexture},
    {"MetallicTexture", "UseMetallicTexture", &surface.m_sMetallicTexture},
  };

  // 2D textures, bound by the prepared files' resource ids: the texture loader reads DDS directly.
  plUInt16 uiTextures = 0;
  for (const TextureSlot& slot : textureSlots)
  {
    if (!slot.m_pFile->IsEmpty())
      ++uiTextures;
  }

  w << uiTextures;
  for (const TextureSlot& slot : textureSlots)
  {
    if (!slot.m_pFile->IsEmpty())
    {
      w << plString(slot.m_szTexture);
      w << *slot.m_pFile;
    }
  }

  w << (plUInt16)0; // cube textures

  // Constants. The base material's values do not reach a material streamed in at runtime: a constant left out here is
  // zero, not the shader's default. So every constant whose default is not zero is written, or UVs collapse to one
  // texel (Scale), normal maps do nothing (NormalIntensity) and masks cut everything or nothing (MaskThreshold).
  plColor baseColor = surface.m_BaseColor;
  baseColor.a = surface.m_fOpacity;

  w << (plUInt16)(7 + PL_ARRAY_SIZE(textureSlots));
  w << plString("Scale");
  w << plVariant(plVec2(1.0f, 1.0f));
  w << plString("NormalIntensity");
  w << plVariant(1.0f);
  w << plString("MaskThreshold");
  w << plVariant(surface.m_fOpacityThreshold > 0.0f ? surface.m_fOpacityThreshold : 0.5f);
  w << plString("EmissiveLighting");
  w << plVariant(true);
  w << plString("BaseColor");
  w << plVariant(baseColor);
  w << plString("MetallicValue");
  w << plVariant(surface.m_fMetallic);
  w << plString("RoughnessValue");
  w << plVariant(surface.m_fRoughness);

  for (const TextureSlot& slot : textureSlots)
  {
    w << plString(slot.m_szUseFlag);
    w << plVariant(!slot.m_pFile->IsEmpty());
  }

  w << plString("<Invalid>"); // render data category: keep the base material's
  w << plString();            // no embedded low-resolution textures

  CopyStorage(storage, out_data);
}

bool plUsdLiveResources::Reload(plStringView sAssetType, plStringView sResourceId)
{
  plEditorEngineProcessConnection* pConnection = plEditorEngineProcessConnection::GetSingleton();
  if (pConnection == nullptr || pConnection->IsProcessCrashed() || !pConnection->IsEngineSetup())
    return false;

  plRestoreResourceMsgToEngine msg;
  msg.m_sResourceType = sAssetType;
  msg.m_sResourceID = sResourceId;
  return pConnection->SendMessage(&msg);
}

bool plUsdLiveResources::Send(plStringView sAssetType, plStringView sResourceId, const plDynamicArray<plUInt8>& data)
{
  plEditorEngineProcessConnection* pConnection = plEditorEngineProcessConnection::GetSingleton();
  if (pConnection == nullptr || pConnection->IsProcessCrashed() || !pConnection->IsEngineSetup())
    return false;

  // The resource's UpdateContent expects what its file loader produces: the path, then the file's content. The material
  // loader only takes the binary layout when that path ends in .plBinMaterial, which the resource ids do.
  plContiguousMemoryStreamStorage storage;
  plMemoryStreamWriter writer(&storage);
  writer << plString(sResourceId);
  writer.WriteBytes(data.GetData(), data.GetCount()).IgnoreResult();

  plDynamicArray<plUInt8> message;
  CopyStorage(storage, message);

  plResourceUpdateMsgToEngine msg;
  msg.m_sResourceType = sAssetType;
  msg.m_sResourceID = sResourceId;
  msg.m_Data = plArrayPtr<const plUInt8>(message.GetData(), message.GetCount());

  return pConnection->SendMessage(&msg);
}
