#include <EditorPluginUsd/EditorPluginUsdPCH.h>

#include <EditorPluginUsd/Bake/UsdBackgroundBaker.h>
#include <EditorPluginUsd/Bake/UsdBakePrefab.h>
#include <EditorPluginUsd/LiveLink/UsdBakeStore.h>
#include <EditorPluginUsd/LiveLink/UsdLiveLink.h>
#include <EditorPluginUsd/LiveLink/UsdLiveManager.h>
#include <EditorPluginUsd/LiveLink/UsdLiveResources.h>
#include <EditorPluginUsd/LiveLink/UsdLiveTextures.h>
#include <Foundation/Algorithm/HashingUtils.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Threading/DelegateTask.h>
#include <ToolsFoundation/Document/DocumentManager.h>
#include <ToolsFoundation/Project/ToolsProject.h>
#include <UsdBridge/UsdStage.h>

namespace
{
  /// How often closed links are checked for changed layers.
  constexpr plTime CheckInterval = plTime::MakeFromSeconds(3);

  /// A link whose bake failed is left alone this long, so a missing stage does not fail every few seconds.
  constexpr plTime RetryDelay = plTime::MakeFromSeconds(60);

  constexpr plStringView SidecarExtension = ".usdlinks";
} // namespace

struct plUsdBackgroundBaker::Job
{
  Link m_Link;
  plUsdBakeStore::Manifest m_Manifest;
  plDynamicArray<plUsdBakeNode> m_Nodes;
  plHashTable<plString, plUsdSurface> m_Materials; ///< Material resource -> surface, with the source texture files.
  plHashTable<plString, plUInt64> m_MaterialHashes;
  plStatus m_Result;
  plTaskGroupID m_Task;
  bool m_bWorkerDone = false;
};

plUsdBackgroundBaker::plUsdBackgroundBaker() = default;

plUsdBackgroundBaker::~plUsdBackgroundBaker()
{
  Clear();
}

void plUsdBackgroundBaker::Clear()
{
  if (m_pJob != nullptr)
  {
    plTaskSystem::WaitForGroup(m_pJob->m_Task);
    m_pJob.Clear();
  }

  m_Links.Clear();
  m_RetryAfter.Clear();
  m_sStatus.Clear();
}

void plUsdBackgroundBaker::ScanProject()
{
  m_Links.Clear();

  if (!plToolsProject::IsProjectOpen())
    return;

  plStringBuilder sProject = plToolsProject::GetSingleton()->GetProjectDirectory();

  plFileSystemIterator it;
  for (it.StartSearch(sProject, plFileSystemIteratorFlags::ReportFilesAndFoldersRecursive); it.IsValid();)
  {
    const plFileStats& stats = it.GetStats();

    if (stats.m_bIsDirectory)
    {
      // Generated and third-party folders hold no documents.
      const plStringView sName = stats.m_sName;
      if (sName == "AssetCache" || sName == "Plugins" || sName == ".live" || sName == ".git" || sName == "UsdBake")
      {
        it.SkipFolder();
        continue;
      }
    }
    else if (stats.m_sName.EndsWith_NoCase(SidecarExtension))
    {
      plStringBuilder sSidecar;
      stats.GetFullPath(sSidecar);
      sSidecar.Shrink(0, SidecarExtension.GetElementCount());
      ReadSidecar(sSidecar);
    }

    it.Next();
  }

  plLog::Dev("USD Live: {} link(s) in the project", m_Links.GetCount());
}

void plUsdBackgroundBaker::UpdateDocument(plStringView sDocumentPath)
{
  for (plUInt32 i = m_Links.GetCount(); i-- > 0;)
  {
    if (m_Links[i].m_sDocument == sDocumentPath)
      m_Links.RemoveAtAndCopy(i);
  }

  ReadSidecar(sDocumentPath);
}

void plUsdBackgroundBaker::ReadSidecar(plStringView sDocumentPath)
{
  plHybridArray<plUsdLiveManager::SidecarLink, 4> links;
  plUsdLiveManager::ReadSidecar(sDocumentPath, links);

  for (const plUsdLiveManager::SidecarLink& sidecarLink : links)
  {
    Link& link = m_Links.ExpandAndGetRef();
    link.m_sDocument = sDocumentPath;
    link.m_Root = sidecarLink.m_Root;
    link.m_sStageFile = sidecarLink.m_Settings.m_sStageFile;
    link.m_sSession = sidecarLink.m_Settings.m_sSessionName;
    link.m_sLinkId = plUsdLiveLink::MakeLinkId(link.m_Root, link.m_sStageFile);
  }
}

bool plUsdBackgroundBaker::IsDocumentOpen(const Link& link) const
{
  if (plUsdLiveManager::GetSingleton()->IsLinkOpen(link.m_sLinkId))
    return true;

  for (plDocumentManager* pManager : plDocumentManager::GetAllDocumentManagers())
  {
    if (pManager->GetDocumentByPath(link.m_sDocument) != nullptr)
      return true;
  }

  return false;
}

bool plUsdBackgroundBaker::NeedsBake(const Link& link) const
{
  if (!plOSFile::ExistsFile(link.m_sStageFile))
    return false;

  plUsdBakeStore::Manifest manifest;
  plUsdBakeStore::LoadManifest(link.m_sLinkId, manifest);

  if (!manifest.m_Prefab.IsValid() || !plOSFile::ExistsFile(plUsdBakePrefab::GetPrefabPath(link.m_sDocument, link.m_sStageFile, link.m_sLinkId)))
    return true;

  return !plUsdBakeStore::AreLayersCurrent(manifest.m_Layers);
}

void plUsdBackgroundBaker::Update()
{
  if (m_pJob != nullptr)
  {
    UpdateJob();
    return;
  }

  const plTime now = plTime::Now();
  if (now < m_NextCheck || m_Links.IsEmpty())
    return;

  m_NextCheck = now + CheckInterval;

  // Round robin, so one link that keeps changing does not starve the others.
  for (plUInt32 i = 0; i < m_Links.GetCount(); ++i)
  {
    const Link& link = m_Links[(m_uiNextLink + i) % m_Links.GetCount()];

    plTime retry;
    if (m_RetryAfter.TryGetValue(link.m_sLinkId, retry) && now < retry)
      continue;

    if (IsDocumentOpen(link) || !NeedsBake(link))
      continue;

    m_uiNextLink = (m_uiNextLink + i + 1) % m_Links.GetCount();
    StartJob(link);
    return;
  }
}

void plUsdBackgroundBaker::StartJob(const Link& link)
{
  m_pJob = PL_DEFAULT_NEW(Job);
  m_pJob->m_Link = link;
  plUsdBakeStore::LoadManifest(link.m_sLinkId, m_pJob->m_Manifest);

  m_sStatus = plStringBuilder("Baking '", link.m_sDocument.GetFileNameAndExtension(), "' in the background");
  plLog::Info("USD Live: {} ('{}' is closed and its stage changed)", m_sStatus, link.m_sStageFile);

  Job* pJob = m_pJob.Borrow();
  plSharedPtr<plDelegateTask<void>> pTask = PL_DEFAULT_NEW(plDelegateTask<void>, "USD background bake", plTaskNesting::Never, [pJob]()
    { RunWorker(*pJob); });

  m_pJob->m_Task = plTaskSystem::StartSingleTask(pTask, plTaskPriority::LongRunning);
}

void plUsdBackgroundBaker::UpdateJob()
{
  Job& job = *m_pJob;

  if (!job.m_bWorkerDone)
  {
    if (!plTaskSystem::IsTaskGroupFinished(job.m_Task))
      return;

    job.m_bWorkerDone = true;

    if (job.m_Result.Failed())
    {
      plLog::Warning("USD Live: the background bake of '{}' failed: {}", job.m_Link.m_sStageFile, job.m_Result.m_sMessage);
      m_RetryAfter[job.m_Link.m_sLinkId] = plTime::Now() + RetryDelay;
      m_pJob.Clear();
      m_sStatus.Clear();
      return;
    }
  }

  // Materials, once every texture they use is prepared. The texture cache is main-thread only.
  plUsdLiveTextures* pTextures = plUsdLiveManager::GetSingleton()->GetTextures();
  bool bPending = false;

  for (auto it = job.m_Materials.GetIterator(); it.IsValid(); ++it)
  {
    plUsdSurface surface = it.Value();
    if (!pTextures->Resolve(surface))
      bPending = true;
  }

  if (bPending)
    return;

  for (auto it = job.m_Materials.GetIterator(); it.IsValid(); ++it)
  {
    plUsdSurface surface = it.Value();
    bool bSoftOpacity = false;
    pTextures->Resolve(surface, &bSoftOpacity);

    plDynamicArray<plUInt8> data;
    plUsdLiveResources::BuildMaterial(surface, bSoftOpacity, data);
    plUsdBakeStore::WriteFile(it.Key(), data).IgnoreResult();
    job.m_MaterialHashes[it.Key()] = plHashingUtils::xxHash64(data.GetData(), data.GetCount());
  }

  const Link& link = job.m_Link;
  const plString sPrefab = plUsdBakePrefab::GetPrefabPath(link.m_sDocument, link.m_sStageFile, link.m_sLinkId);
  const plUInt64 uiContent = plUsdBakePrefab::GetContentHash(job.m_Nodes, job.m_MaterialHashes);

  if (uiContent != job.m_Manifest.m_uiContentHash || !job.m_Manifest.m_Prefab.IsValid() || !plOSFile::ExistsFile(sPrefab))
  {
    plUuid prefabGuid;
    const plStatus res = plUsdBakePrefab::Write(sPrefab, link.m_sLinkId, job.m_Nodes, prefabGuid);
    if (res.Failed())
    {
      plLog::Warning("USD Live: could not write the bake prefab '{}': {}", sPrefab, res.m_sMessage);
      m_RetryAfter[link.m_sLinkId] = plTime::Now() + RetryDelay;
      m_pJob.Clear();
      m_sStatus.Clear();
      return;
    }

    job.m_Manifest.m_Prefab = prefabGuid;
    job.m_Manifest.m_uiContentHash = uiContent;
  }

  plUsdBakeStore::RemoveUnusedMeshes(link.m_sLinkId, job.m_Manifest);
  plUsdBakeStore::SaveManifest(link.m_sLinkId, job.m_Manifest).IgnoreResult();
  plLog::Success("USD Live: baked '{}' in the background into '{}'", link.m_sStageFile, sPrefab);

  m_pJob.Clear();
  m_sStatus.Clear();
}

void plUsdBackgroundBaker::RunWorker(Job& job)
{
  {
    const Link& link = job.m_Link;

    // Through the session, when the link has one, so the bake shows what every participant sees.
    plStringBuilder sFile = link.m_sStageFile;
    if (!link.m_sSession.IsEmpty())
    {
      plStringBuilder sSession(plUsdStage::GetSessionsFolder(link.m_sStageFile), "/", link.m_sSession, "/session.usda");
      if (plOSFile::ExistsFile(sSession))
        sFile = sSession;
    }

    plUsdStage stage;
    job.m_Result = stage.Open(sFile);
    if (job.m_Result.Failed())
      return;

    plDynamicArray<plUsdPrim> prims;
    stage.ReadPrims(prims);
    job.m_Nodes.SetCount(prims.GetCount());
    job.m_Manifest.m_Prims.Clear();

    // Meshes built or checked by this bake: prims with the same geometry and materials share one.
    plHashTable<plString, plUInt32> meshParts;

    for (plUInt32 i = 0; i < prims.GetCount(); ++i)
    {
      plUsdBakeNode& node = job.m_Nodes[i];
      node.m_Prim = std::move(prims[i]);
      const plUsdPrim& prim = node.m_Prim;

      if (!prim.m_bActive || (prim.m_Kind != plUsdPrimKind::Mesh && prim.m_Kind != plUsdPrimKind::Gprim))
        continue;

      plDynamicArray<plString> materials;
      for (const plUsdMaterialGroup& group : prim.m_MaterialGroups)
      {
        const plString sMaterial = plUsdBakeStore::GetMaterialResource(link.m_sLinkId, group);
        materials.PushBack(sMaterial);

        if (group.m_bHasSurface && !group.m_Surface.m_sMaterialPath.IsEmpty())
          job.m_Materials[sMaterial] = group.m_Surface;
      }

      plUsdGeometryKey geometry;
      if (stage.ComputeGeometryKey(prim.m_sPath, geometry).Failed())
        continue;

      const plUInt64 uiMeshKey = plUsdBakeStore::GetMeshKey(link.m_sLinkId, geometry, prim.m_MaterialGroups);
      const plString sMesh = plUsdBakeStore::GetMeshName(link.m_sLinkId, geometry, uiMeshKey);

      plUInt32 uiParts = 0;
      if (!meshParts.TryGetValue(sMesh, uiParts))
      {
        // Built from the same layer files as last time, and its files still there: nothing to build.
        const plUsdBakeStore::MeshEntry* pEntry = job.m_Manifest.m_Meshes.GetValue(sMesh);
        if (pEntry != nullptr && pEntry->m_uiFileStamp == geometry.m_uiFileStamp && plUsdBakeStore::HasMeshFiles(sMesh, pEntry->m_uiParts))
        {
          uiParts = pEntry->m_uiParts;
        }
        else
        {
          plStatus res;
          plUniquePtr<plUsdGeometrySource> pSource = stage.CaptureGeometry(prim.m_sPath, res);

          plDynamicArray<plDynamicArray<plUInt8>> parts;
          if (pSource != nullptr)
            res = plUsdLiveResources::BuildMeshParts(*pSource, materials, uiMeshKey, plUsdLiveResources::NeedsTangents(prim.m_MaterialGroups), parts);

          if (res.Failed())
          {
            plLog::Warning("USD Live: '{}' could not be baked: {}", prim.m_sPath, res.m_sMessage);
          }
          else
          {
            for (plUInt32 uiPart = 0; uiPart < parts.GetCount(); ++uiPart)
              plUsdBakeStore::WriteFile(plUsdBakeStore::GetMeshResource(sMesh, uiPart), parts[uiPart]).IgnoreResult();

            uiParts = parts.GetCount();
            job.m_Manifest.m_Meshes[sMesh] = {geometry.m_uiFileStamp, uiParts};
          }
        }

        meshParts[sMesh] = uiParts;
      }

      if (uiParts == 0)
        continue;

      node.m_uiMeshParts = uiParts;
      node.m_sMesh = sMesh;
      job.m_Manifest.m_Prims[prim.m_sPath] = {sMesh, uiParts};
    }

    plUsdBakeStore::RecordLayers(stage, job.m_Manifest.m_Layers);
  }
}
