#include <EditorPluginUsd/EditorPluginUsdPCH.h>

#include <EditorPluginUsd/LiveLink/UsdBakeStore.h>
#include <EditorPluginUsd/LiveLink/UsdLiveResources.h>
#include <EditorPluginUsd/Utils/UsdJson.h>
#include <Foundation/Algorithm/HashingUtils.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Utilities/ConversionUtils.h>
#include <ToolsFoundation/Project/ToolsProject.h>

namespace
{
  constexpr plStringView ProjectRoot = ":project/";

  plString MakeFileName(plStringView sPath, plStringView sSuffix)
  {
    // The last path element keeps the file recognisable; the hash of the whole path keeps it unique and short.
    plStringView sName = sPath.GetFileNameAndExtension();
    if (sName.IsEmpty())
      sName = "root";

    plStringBuilder s;
    s.SetFormat("{}-{}{}", sName, plArgU(plHashingUtils::xxHash32String(sPath), 8, true, 16), sSuffix);
    return s;
  }
} // namespace

plString plUsdBakeStore::GetRootFolder()
{
  if (!plToolsProject::IsProjectOpen())
    return {};

  plStringBuilder s(plToolsProject::GetSingleton()->GetProjectDirectory(), "/AssetCache/Generated/UsdLive");
  s.MakeCleanPath();
  return s;
}

plString plUsdBakeStore::GetLinkFolder(plStringView sLinkId)
{
  plStringBuilder s(GetRootFolder(), "/", sLinkId);
  return s;
}

plString plUsdBakeStore::ToResourceId(plStringView sAbsolutePath)
{
  if (!plToolsProject::IsProjectOpen())
    return sAbsolutePath;

  plStringBuilder sProject = plToolsProject::GetSingleton()->GetProjectDirectory();
  sProject.MakeCleanPath();
  if (!sProject.EndsWith("/"))
    sProject.Append("/");

  plStringBuilder sPath = sAbsolutePath;
  sPath.MakeCleanPath();

  if (!sPath.StartsWith_NoCase(sProject))
    return sPath;

  sPath.Shrink(sProject.GetCharacterCount(), 0);
  return plStringBuilder(ProjectRoot, sPath);
}

plString plUsdBakeStore::ToAbsolutePath(plStringView sResourceId)
{
  if (!sResourceId.StartsWith(ProjectRoot) || !plToolsProject::IsProjectOpen())
    return sResourceId;

  plStringBuilder s = plToolsProject::GetSingleton()->GetProjectDirectory();
  s.AppendPath(sResourceId.GetSubString(ProjectRoot.GetElementCount(), plInvalidIndex));
  s.MakeCleanPath();
  return s;
}

plUInt64 plUsdBakeStore::GetMeshKey(plStringView sLinkId, const plUsdGeometryKey& geometry, plArrayPtr<const plUsdMaterialGroup> groups)
{
  // Changes whenever plUsdLiveResources writes mesh files differently, so bakes made before are rebuilt.
  constexpr plUInt64 MeshFileVersion = 7;

  plUInt64 uiHash = plHashingUtils::xxHash64(&MeshFileVersion, sizeof(MeshFileVersion), geometry.m_uiIdentity);
  for (const plUsdMaterialGroup& group : groups)
    uiHash = plHashingUtils::xxHash64String(GetMaterialResource(sLinkId, group), uiHash);

  const bool bTangents = plUsdLiveResources::NeedsTangents(groups);
  uiHash = plHashingUtils::xxHash64(&bTangents, sizeof(bTangents), uiHash);

  return uiHash != 0 ? uiHash : 1;
}

plString plUsdBakeStore::GetMeshName(plStringView sLinkId, const plUsdGeometryKey& geometry, plUInt64 uiMeshKey)
{
  plStringBuilder s;
  s.SetFormat("{}AssetCache/Generated/UsdLive/{}/{}-{}", ProjectRoot, sLinkId, geometry.m_sName.IsEmpty() ? plStringView("mesh") : geometry.m_sName.GetView(),
    plArgU(uiMeshKey, 16, true, 16));
  return s;
}

plString plUsdBakeStore::GetMeshResource(plStringView sMesh, plUInt32 uiPart)
{
  plStringBuilder s;
  s.SetFormat("{}_{}.plBinMesh", sMesh, uiPart);
  return s;
}

plStringView plUsdBakeStore::GetMeshOfResource(plStringView sResource)
{
  if (!sResource.EndsWith_NoCase(".plBinMesh"))
    return sResource;

  const char* szPart = sResource.FindLastSubString("_");
  return szPart != nullptr ? plStringView(sResource.GetStartPointer(), szPart) : sResource;
}

bool plUsdBakeStore::HasMeshFiles(plStringView sMesh, plUInt32 uiParts)
{
  if (uiParts == 0)
    return false;

  for (plUInt32 uiPart = 0; uiPart < uiParts; ++uiPart)
  {
    if (!plOSFile::ExistsFile(ToAbsolutePath(GetMeshResource(sMesh, uiPart))))
      return false;
  }

  return true;
}

void plUsdBakeStore::RemoveUnusedMeshes(plStringView sLinkId, Manifest& inout_manifest)
{
  plSet<plString> used;
  for (auto it = inout_manifest.m_Prims.GetIterator(); it.IsValid(); ++it)
    used.Insert(it.Value().m_sMesh);

  plHybridArray<plString, 16> unused;
  for (auto it = inout_manifest.m_Meshes.GetIterator(); it.IsValid(); ++it)
  {
    if (!used.Contains(it.Value().m_sResourceMesh.IsEmpty() ? it.Key() : it.Value().m_sResourceMesh))
    {
      unused.PushBack(it.Key());
    }
  }
  for (const plString& sMesh : unused)
    inout_manifest.m_Meshes.Remove(sMesh);

  // The folder's mesh files, by the absolute path of their mesh name.
  plSet<plString> keep;
  for (auto it = inout_manifest.m_Meshes.GetIterator(); it.IsValid(); ++it)
  {
    plStringBuilder s = ToAbsolutePath(it.Value().m_sResourceMesh.IsEmpty() ? it.Key() : it.Value().m_sResourceMesh);
    s.MakeCleanPath();
    keep.Insert(s);
  }

  plDynamicArray<plString> remove;
  plFileSystemIterator files;
  for (files.StartSearch(GetLinkFolder(sLinkId), plFileSystemIteratorFlags::ReportFiles); files.IsValid(); files.Next())
  {
    const plFileStats& stats = files.GetStats();
    if (!stats.m_sName.EndsWith_NoCase(".plBinMesh"))
      continue;

    plStringBuilder sFile;
    stats.GetFullPath(sFile);
    sFile.MakeCleanPath();

    const plStringBuilder sMesh = GetMeshOfResource(sFile);
    if (!keep.Contains(sMesh))
      remove.PushBack(sFile);
  }

  for (const plString& sFile : remove)
  {
    if (plOSFile::DeleteFile(sFile).Failed())
      plLog::Dev("USD Live: could not delete the unused mesh file '{}'", sFile);
  }

  if (!remove.IsEmpty())
    plLog::Dev("USD Live: removed {} unused mesh file(s) of link {}", remove.GetCount(), sLinkId);
}

plString plUsdBakeStore::GetMaterialResource(plStringView sLinkId, const plUsdMaterialGroup& group)
{
  // displayColor alone needs no material of its own: the mesh component's colour tints the standard material.
  if (!group.m_bHasSurface || group.m_Surface.m_sMaterialPath.IsEmpty())
    return plUsdLiveResources::DefaultMaterial;

  plStringBuilder s(ProjectRoot, "AssetCache/Generated/UsdLive/", sLinkId, "/");
  s.Append(MakeFileName(group.m_Surface.m_sMaterialPath, group.m_Surface.m_bDoubleSided ? "_2s.plBinMaterial" : ".plBinMaterial"));
  return s;
}

plColor plUsdBakeStore::GetTint(plArrayPtr<const plUsdMaterialGroup> groups)
{
  if (groups.GetCount() == 1 && groups[0].m_bHasSurface && groups[0].m_Surface.m_sMaterialPath.IsEmpty())
    return groups[0].m_Surface.m_BaseColor;

  return plColor::White;
}

plResult plUsdBakeStore::WriteFile(plStringView sResourceId, plArrayPtr<const plUInt8> content, bool* out_pWritten)
{
  return WriteAbsoluteFile(ToAbsolutePath(sResourceId), content, out_pWritten);
}

plResult plUsdBakeStore::WriteAbsoluteFile(plStringView sFile, plArrayPtr<const plUInt8> content, bool* out_pWritten, plStringView sTempFile)
{
  if (out_pWritten)
    *out_pWritten = false;

  // An unchanged file keeps its time stamp, so nothing that depends on it is rebuilt.
  {
    plFileStats stats;
    if (plOSFile::GetFileStats(sFile, stats).Succeeded() && stats.m_uiFileSize == content.GetCount())
    {
      plOSFile file;
      if (file.Open(sFile, plFileOpenMode::Read).Succeeded())
      {
        plDynamicArray<plUInt8> existing;
        existing.SetCountUninitialized(content.GetCount());
        const plUInt64 uiRead = content.IsEmpty() ? 0 : file.Read(existing.GetData(), content.GetCount());
        file.Close();

        if (uiRead == content.GetCount() && (content.IsEmpty() || plMemoryUtils::IsEqual(existing.GetData(), content.GetPtr(), content.GetCount())))
          return PL_SUCCESS;
      }
    }
  }

  plStringBuilder sFolder = sFile;
  sFolder.PathParentDirectory();
  PL_SUCCEED_OR_RETURN(plOSFile::CreateDirectoryStructure(sFolder));

  const plStringView sWrite = sTempFile.IsEmpty() ? sFile : sTempFile;
  if (!sTempFile.IsEmpty())
  {
    sFolder = sTempFile;
    sFolder.PathParentDirectory();
    PL_SUCCEED_OR_RETURN(plOSFile::CreateDirectoryStructure(sFolder));
  }

  plOSFile file;
  PL_SUCCEED_OR_RETURN(file.Open(sWrite, plFileOpenMode::Write));
  plResult res = content.IsEmpty() ? PL_SUCCESS : file.Write(content.GetPtr(), content.GetCount());
  file.Close();

  if (!sTempFile.IsEmpty())
  {
    if (res.Succeeded())
      res = plOSFile::AtomicReplaceFile(sTempFile, sFile);
    if (res.Failed())
      plOSFile::DeleteFile(sTempFile).IgnoreResult();
  }

  if (out_pWritten)
    *out_pWritten = res.Succeeded();
  return res;
}

void plUsdBakeStore::LoadManifest(plStringView sLinkId, Manifest& out_manifest)
{
  LoadManifestFile(plStringBuilder(GetLinkFolder(sLinkId), "/bake.json"), out_manifest);
}

void plUsdBakeStore::LoadManifestFile(plStringView sFile, Manifest& out_manifest)
{
  out_manifest = {};

  plVariantDictionary root;
  if (!plUsdJson::ReadFile(sFile, root))
  {
    return;
  }

  const plString sPrefab = plUsdJson::GetString(root, "prefab");
  if (plConversionUtils::IsStringUuid(sPrefab))
  {
    out_manifest.m_Prefab = plConversionUtils::ConvertStringToUuid(sPrefab);
  }
  out_manifest.m_uiContentHash = plUsdJson::ParseHex(plUsdJson::GetString(root, "content"));

  // Version 1 named mesh files by prim; its meshes are simply built again.
  if (plUsdJson::GetNumber(root, "version") >= 2.0)
  {
    for (const auto& it : plUsdJson::GetObject(root, "prims"))
    {
      const plVariantDictionary& prim = plUsdJson::AsObject(it.Value());
      PrimEntry entry;
      entry.m_sMesh = plUsdJson::GetString(prim, "mesh");
      entry.m_uiParts = static_cast<plUInt32>(plUsdJson::GetNumber(prim, "parts"));
      out_manifest.m_Prims.Insert(it.Key(), entry);
    }

    for (const auto& it : plUsdJson::GetObject(root, "meshes"))
    {
      const plVariantDictionary& mesh = plUsdJson::AsObject(it.Value());
      MeshEntry entry;
      entry.m_uiFileStamp = plUsdJson::ParseHex(plUsdJson::GetString(mesh, "files"));
      entry.m_uiParts = static_cast<plUInt32>(plUsdJson::GetNumber(mesh, "parts"));
      entry.m_sResourceMesh = plUsdJson::GetString(mesh, "resourceMesh");
      entry.m_sPreviewResource = plUsdJson::GetString(mesh, "preview");
      entry.m_uiPreviewSize = plUsdJson::ParseHex(plUsdJson::GetString(mesh, "previewSize"));
      for (const auto& size : plUsdJson::GetArray(mesh, "partSizes"))
      {
        entry.m_PartSizes.PushBack(plUsdJson::ParseHex(plUsdJson::AsString(size)));
      }
      out_manifest.m_Meshes.Insert(it.Key(), entry);
    }
  }

  for (const auto& it : plUsdJson::GetObject(root, "layers"))
  {
    out_manifest.m_Layers.Insert(it.Key(), plUsdJson::ParseDecimal(plUsdJson::AsString(it.Value())));
  }
}

plResult plUsdBakeStore::SaveManifest(plStringView sLinkId, const Manifest& manifest)
{
  return SaveManifestFile(plStringBuilder(GetLinkFolder(sLinkId), "/bake.json"), manifest);
}

plResult plUsdBakeStore::SaveManifestFile(plStringView sFile, const Manifest& manifest)
{
  plVariantDictionary prims;
  for (auto it = manifest.m_Prims.GetIterator(); it.IsValid(); ++it)
  {
    plVariantDictionary prim;
    prim.Insert("mesh", it.Value().m_sMesh);
    prim.Insert("parts", static_cast<plInt32>(it.Value().m_uiParts));
    prims.Insert(it.Key(), prim);
  }

  plVariantDictionary meshes;
  for (auto it = manifest.m_Meshes.GetIterator(); it.IsValid(); ++it)
  {
    plVariantDictionary mesh;
    mesh.Insert("files", plUsdJson::HexString(it.Value().m_uiFileStamp));
    mesh.Insert("parts", static_cast<plInt32>(it.Value().m_uiParts));
    mesh.Insert("resourceMesh", it.Value().m_sResourceMesh);
    mesh.Insert("preview", it.Value().m_sPreviewResource);
    mesh.Insert("previewSize", plUsdJson::HexString(it.Value().m_uiPreviewSize));
    plVariantArray sizes;
    for (plUInt64 size : it.Value().m_PartSizes)
    {
      sizes.PushBack(plUsdJson::HexString(size));
    }
    mesh.Insert("partSizes", sizes);
    meshes.Insert(it.Key(), mesh);
  }

  // Time stamps as strings: JSON numbers are doubles, which cannot hold every microsecond count exactly.
  plVariantDictionary layers;
  for (auto it = manifest.m_Layers.GetIterator(); it.IsValid(); ++it)
  {
    layers.Insert(it.Key(), plUsdJson::DecimalString(it.Value()));
  }

  plStringBuilder sPrefab;
  if (manifest.m_Prefab.IsValid())
    plConversionUtils::ToString(manifest.m_Prefab, sPrefab);

  plVariantDictionary root;
  root.Insert("version", 3);
  root.Insert("prefab", plString(sPrefab));
  root.Insert("content", plUsdJson::HexString(manifest.m_uiContentHash));
  root.Insert("prims", prims);
  root.Insert("meshes", meshes);
  root.Insert("layers", layers);

  PL_SUCCEED_OR_RETURN(plOSFile::CreateDirectoryStructure(sFile.GetFileDirectory()));

  const plString json = plUsdJson::Write(root, true);
  return WriteAbsoluteFile(sFile,
    plArrayPtr<const plUInt8>(reinterpret_cast<const plUInt8*>(json.GetData()), json.GetElementCount()), nullptr,
    plStringBuilder(sFile, ".tmp"));
}

bool plUsdBakeStore::RecordLayers(const plUsdStage& stage, plHashTable<plString, plInt64>& out_layers, plInt64 iSince)
{
  out_layers.Clear();

  plDynamicArray<plString> files;
  stage.GetUsedLayerFiles(files);

  bool bUnchanged = true;
  for (const plString& sFile : files)
  {
    plFileStats stats;
    if (plOSFile::GetFileStats(sFile, stats).Succeeded())
    {
      const plInt64 iTime = stats.m_LastModificationTime.GetInt64(plSIUnitOfTime::Microsecond);
      out_layers.Insert(sFile, iTime);
      bUnchanged &= iTime <= iSince;
    }
  }
  return bUnchanged;
}

bool plUsdBakeStore::AreLayersCurrent(const plHashTable<plString, plInt64>& layers)
{
  if (layers.IsEmpty())
    return false;

  for (auto it = layers.GetIterator(); it.IsValid(); ++it)
  {
    plFileStats stats;
    if (plOSFile::GetFileStats(it.Key(), stats).Failed() || stats.m_LastModificationTime.GetInt64(plSIUnitOfTime::Microsecond) != it.Value())
      return false;
  }

  return true;
}
