#include <EditorPluginUsd/EditorPluginUsdPCH.h>

#include <EditorPluginUsd/LiveLink/UsdMeshCache.h>
#include <Foundation/Algorithm/HashingUtils.h>
#include <Foundation/Application/SdkVersion.h>
#include <Foundation/Containers/Set.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Threading/Mutex.h>

namespace
{
  plMutex s_Mutex;
  plHashTable<plString, plUInt64> s_BuildingMeshes;
  plUInt64 s_uiPoints = 0;

  bool HasCapacity(plUInt64 uiPoints)
  {
    return s_BuildingMeshes.IsEmpty() || (s_BuildingMeshes.GetCount() < 6 && s_uiPoints + uiPoints <= 4'000'000);
  }

  plUInt64 GetFileSize(plStringView sResource)
  {
    plFileStats stats;
    return plOSFile::GetFileStats(plUsdBakeStore::ToAbsolutePath(sResource), stats).Succeeded() ? stats.m_uiFileSize : 0;
  }
} // namespace

plUInt64 plUsdMeshCache::GetKey(plUInt64 uiGeometryHash, plUInt32 uiMaterialSlots)
{
  // All final meshes include MikkTSpace tangents, regardless of the current material's textures.
  // A binding or normal-map edit therefore never changes this key.
  constexpr plUInt32 ConversionVersion = 1;
  plUInt64 hash = plHashingUtils::xxHash64(&ConversionVersion, sizeof(ConversionVersion), uiGeometryHash);
  hash = plHashingUtils::xxHash64(&uiMaterialSlots, sizeof(uiMaterialSlots), hash);
  hash = plHashingUtils::xxHash64String(plSdkVersion::GetString(), hash);
  return hash != 0 ? hash : 1;
}

plString plUsdMeshCache::GetMeshName(plUInt64 uiKey)
{
  plStringBuilder name;
  name.SetFormat(":project/AssetCache/Generated/UsdLive/Geometry/{}", plArgU(uiKey, 16, true, 16));
  return name;
}

plString plUsdMeshCache::GetPreviewResource(plStringView sMesh)
{
  if (sMesh.StartsWith(":project/AssetCache/Generated/UsdLive/Geometry/"))
  {
    return plStringBuilder(":project/AssetCache/UsdLivePreviews/Geometry/", sMesh.GetFileNameAndExtension(), ".v2.preview.plBinMesh");
  }

  return plStringBuilder(sMesh, ".v2.preview.plBinMesh");
}

plUsdBakeStore::MeshEntry plUsdMeshCache::Read(plStringView sMesh)
{
  plUsdBakeStore::Manifest manifest;
  plUsdBakeStore::LoadManifestFile(plUsdBakeStore::ToAbsolutePath(plStringBuilder(sMesh, ".cache.json")), manifest);
  plUsdBakeStore::MeshEntry result;
  if (const auto* entry = manifest.m_Meshes.GetValue(sMesh))
  {
    result = *entry;
    if (result.m_PartSizes.GetCount() != result.m_uiParts)
    {
      result.m_uiParts = 0;
    }

    for (plUInt32 i = 0; i < result.m_uiParts; ++i)
    {
      if (result.m_PartSizes[i] <= 32 || GetFileSize(plUsdBakeStore::GetMeshResource(sMesh, i)) != result.m_PartSizes[i])
      {
        result.m_uiParts = 0;
        break;
      }
    }

    if (!result.m_sPreviewResource.IsEmpty() &&
        (result.m_sPreviewResource != GetPreviewResource(sMesh) || result.m_uiPreviewSize <= 32 || GetFileSize(result.m_sPreviewResource) != result.m_uiPreviewSize))
    {
      result.m_sPreviewResource.Clear();
    }
  }

  return result;
}

plResult plUsdMeshCache::Publish(plStringView sMesh, plUInt32 uiParts, bool bPreview)
{
  plUsdBakeStore::Manifest manifest;
  auto& entry = manifest.m_Meshes[sMesh];
  entry.m_sResourceMesh = sMesh;
  entry.m_uiParts = uiParts;
  for (plUInt32 i = 0; i < uiParts; ++i)
  {
    const plUInt64 size = GetFileSize(plUsdBakeStore::GetMeshResource(sMesh, i));
    if (size <= 32)
    {
      return PL_FAILURE;
    }

    entry.m_PartSizes.PushBack(size);
  }
  if (bPreview)
  {
    entry.m_sPreviewResource = GetPreviewResource(sMesh);
    entry.m_uiPreviewSize = GetFileSize(entry.m_sPreviewResource);
    if (entry.m_uiPreviewSize <= 32)
    {
      entry.m_sPreviewResource.Clear();
      if (uiParts == 0)
      {
        return PL_FAILURE;
      }
    }
  }

  return plUsdBakeStore::SaveManifestFile(plUsdBakeStore::ToAbsolutePath(plStringBuilder(sMesh, ".cache.json")), manifest);
}

bool plUsdMeshCache::CanAdmit(plUInt64 uiPoints)
{
  PL_LOCK(s_Mutex);
  return HasCapacity(uiPoints);
}

bool plUsdMeshCache::TryAcquire(plStringView sMesh, plUInt64 uiPoints)
{
  PL_LOCK(s_Mutex);
  const plString key = plUsdBakeStore::ToAbsolutePath(sMesh);
  if (s_BuildingMeshes.Contains(key) || !HasCapacity(uiPoints))
  {
    return false;
  }

  s_BuildingMeshes.Insert(key, uiPoints);
  s_uiPoints += uiPoints;
  return true;
}

void plUsdMeshCache::Release(plStringView sMesh)
{
  PL_LOCK(s_Mutex);
  plUInt64 points = 0;
  if (s_BuildingMeshes.Remove(plUsdBakeStore::ToAbsolutePath(sMesh), &points))
  {
    s_uiPoints -= points;
  }
}
