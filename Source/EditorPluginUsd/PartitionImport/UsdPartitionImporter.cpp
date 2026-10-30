#include <EditorPluginUsd/EditorPluginUsdPCH.h>

#include <Core/World/GameObject.h>
#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorPluginScene/Scene/Scene2Document.h>
#include <EditorPluginScene/WorldStreaming/LayerMoveUtils.h>
#include <EditorPluginScene/WorldStreaming/WorldPartitionRegistry.h>
#include <EditorPluginScene/WorldStreaming/WorldStreamingEditor.h>
#include <EditorPluginUsd/LiveLink/UsdBakeStore.h>
#include <EditorPluginUsd/LiveLink/UsdLiveManager.h>
#include <EditorPluginUsd/LiveLink/UsdLiveResources.h>
#include <EditorPluginUsd/LiveLink/UsdLiveTextures.h>
#include <EditorPluginUsd/LiveLink/UsdPrimComponents.h>
#include <EditorPluginUsd/PartitionImport/UsdPartitionCellWriter.h>
#include <EditorPluginUsd/PartitionImport/UsdPartitionImporter.h>
#include <EditorPluginUsd/Utils/UsdJson.h>
#include <Foundation/Algorithm/HashingUtils.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Serialization/AbstractObjectGraph.h>
#include <Foundation/Threading/DelegateTask.h>
#include <ToolsFoundation/CommandHistory/CommandHistory.h>
#include <ToolsFoundation/Object/DocumentObjectManager.h>
#include <ToolsFoundation/Object/ObjectDirectAccessor.h>
#include <ToolsFoundation/Serialization/DocumentObjectConverter.h>

PL_IMPLEMENT_SINGLETON(plUsdPartitionImporter);

namespace
{
  constexpr plTime ProgressInterval = plTime::MakeFromMilliseconds(250);

  /// Engine reload messages sent per tick; the rest wait for the next ticks.
  constexpr plUInt32 ReloadsPerTick = 256;

  plString GetLayerFile(const plScene2Document* pScene, plStringView sPartitionName, plStringView sSuffix)
  {
    plStringBuilder s = pScene->GetDocumentPath();
    s.RemoveFileExtension();
    s.Append("_data/", sPartitionName, " ", sSuffix, ".plSceneLayer");
    s.MakeCleanPath();
    return s;
  }

  const plSceneWorldPartition* FindPartition(const plScene2Document* pScene, plStringView sName)
  {
    plDynamicArray<const plSceneWorldPartition*> partitions;
    pScene->GetPartitions(partitions);
    for (const plSceneWorldPartition* pPartition : partitions)
    {
      if (pPartition->m_sName.IsEqual_NoCase(sName))
        return pPartition;
    }
    return nullptr;
  }

  /// Changes whenever a command is done or undone in the scene's own history.
  plUInt64 GetHistoryStamp(const plScene2Document* pScene)
  {
    const plCommandHistory* pHistory = pScene->GetSceneCommandHistory();
    const plUInt32 uiUndo = pHistory->GetUndoStackSize();
    const plCommandTransaction* pTop = uiUndo > 0 ? pHistory->GetUndoStackEntry(0) : nullptr;

    plUInt64 uiStamp = plHashingUtils::xxHash64(&uiUndo, sizeof(uiUndo));
    uiStamp = plHashingUtils::xxHash64(&pTop, sizeof(pTop), uiStamp);
    const plUInt32 uiRedo = pHistory->GetRedoStackSize();
    return plHashingUtils::xxHash64(&uiRedo, sizeof(uiRedo), uiStamp);
  }
} // namespace

plUsdPartitionImporter::plUsdPartitionImporter()
  : m_SingletonRegistrar(this)
{
  m_DocumentSubscription = plDocumentManager::s_Events.AddEventHandler(plMakeDelegate(&plUsdPartitionImporter::DocumentManagerEventHandler, this));

  m_Timers.Start(plTime::MakeFromMilliseconds(50), [this]()
    { Tick(); }, true);
}

plUsdPartitionImporter::~plUsdPartitionImporter()
{
  Abandon();

  m_Timers.StopAll();

  plDocumentManager::s_Events.RemoveEventHandler(m_DocumentSubscription);
}

void plUsdPartitionImporter::DocumentManagerEventHandler(const plDocumentManager::Event& e)
{
  if (e.m_Type == plDocumentManager::Event::Type::DocumentClosing && e.m_pDocument == m_pSaveScene)
    m_pSaveScene = nullptr;

  // The worker stops at its next cancel point; Tick collects it without waiting here.
  if (e.m_Type == plDocumentManager::Event::Type::DocumentClosing && e.m_pDocument == m_pScene && m_pScene != nullptr)
  {
    plLog::Warning("USD Import: the scene closes; the import stops and resumes when it is started again.");
    m_pJob->Cancel();
    m_pScene = nullptr;
    m_bDetached = true;
  }
}

void plUsdPartitionImporter::Abandon()
{
  if (m_pJob == nullptr)
    return;

  m_pJob->Cancel();
  if (!plTaskSystem::IsTaskGroupFinished(m_Task))
  {
    plLog::Info("USD Import: waiting for the import to stop");
    plTaskSystem::WaitForGroup(m_Task);
  }

  // What was written stays; the manifest knows it, so the import resumes.
  if (!m_pJob->m_Manifest.m_sImportId.IsEmpty())
    m_pJob->m_Manifest.Save(m_sManifestFile).LogFailure();

  m_pJob.Clear();
  m_pScene = nullptr;
  m_bDetached = false;
}

plStatus plUsdPartitionImporter::LoadManifest(const plScene2Document* pScene, const plUuid& partition, plUsdPartitionImportManifest& out_manifest)
{
  const plSceneWorldPartition* pPartition = pScene->GetPartition(partition);
  if (pPartition == nullptr)
    return plStatus("Unknown world partition.");

  PL_SUCCEED_OR_RETURN(out_manifest.Load(plUsdPartitionImportManifest::GetPath(pScene->GetPartitionFolder(*pPartition))));
  if (out_manifest.m_Partition != partition)
    return plStatus("The partition's import manifest belongs to another partition.");

  return plStatus(PL_SUCCESS);
}

plStatus plUsdPartitionImporter::Start(plScene2Document* pScene, const plUsdPartitionImportOptions& options)
{
  if (m_pJob != nullptr)
    return plStatus("Another USD import is running.");

  if (options.m_sStageFile.IsEmpty() || !plOSFile::ExistsFile(options.m_sStageFile))
    return plStatus(plFmt("The stage '{}' does not exist.", options.m_sStageFile));

  PL_SUCCEED_OR_RETURN(plLayerMoveUtils::CanRunBatchJob(pScene));

  const float fCellSize = plMath::Max(options.m_fCellSize, plWorldPartitionGrid::s_fMinCellSize);

  // An existing partition of that name continues its import with these options; the roots they affect scan again.
  if (const plSceneWorldPartition* pPartition = FindPartition(pScene, options.m_sPartitionName))
  {
    plUsdPartitionImportManifest manifest;
    const plStatus loaded = LoadManifest(pScene, pPartition->m_Partition, manifest);
    if (loaded.Failed())
      return plStatus(plFmt("The partition '{}' exists and was not made by a USD import ({}).", pPartition->m_sName, loaded.m_sMessage));

    plStringBuilder sStage = options.m_sStageFile, sManifestStage = manifest.m_Options.m_sStageFile;
    sStage.MakeCleanPath();
    sManifestStage.MakeCleanPath();
    if (!sStage.IsEqual_NoCase(sManifestStage))
      return plStatus(plFmt("The partition '{}' was imported from '{}'; choose another name for '{}'.", pPartition->m_sName, sManifestStage, sStage));

    if (fCellSize != manifest.m_Options.m_fCellSize)
      return plStatus(plFmt("The partition '{}' has {} m cells; its import cannot change to {} m. Choose another partition name.", pPartition->m_sName,
        manifest.m_Options.m_fCellSize, fCellSize));

    if (options.GetPlanHash() != manifest.m_Options.GetPlanHash() || !(options.m_Roots == manifest.m_Options.m_Roots))
      plLog::Info("USD Import: the options of '{}' changed; the roots they affect are scanned again.", pPartition->m_sName);

    manifest.m_Options = options;
    manifest.m_Options.m_fCellSize = fCellSize;
    return StartJob(pScene, pPartition->m_Partition, std::move(manifest), false);
  }

  plStringBuilder sName;
  PL_SUCCEED_OR_RETURN(pScene->CheckNewPartitionName(options.m_sPartitionName, sName));

  // Unsaved edits of the user are not saved for them; the import then saves nothing either.
  const bool bSceneWasModified = pScene->IsModified();

  plUuid partition;
  PL_SUCCEED_OR_RETURN(pScene->CreateWorldPartition(sName, fCellSize, partition));

  plUsdPartitionImportManifest manifest;
  manifest.m_Options = options;
  manifest.m_Options.m_sPartitionName = sName;
  manifest.m_Options.m_fCellSize = fCellSize;
  manifest.m_Partition = partition;
  manifest.m_sImportId = plUsdPartitionImportData::MakeImportId(partition);

  // Saved now, so the partition and its manifest survive a crash and the import resumes.
  const plString sFolder = pScene->GetPartitionFolder(*pScene->GetPartition(partition));
  if (plOSFile::CreateDirectoryStructure(sFolder).Failed())
    return plStatus(plFmt("Could not create '{}'.", sFolder));
  PL_SUCCEED_OR_RETURN(manifest.Save(plUsdPartitionImportManifest::GetPath(sFolder)));
  if (!bSceneWasModified)
    PL_SUCCEED_OR_RETURN(pScene->SaveDocument());

  return StartJob(pScene, partition, std::move(manifest), true);
}

plStatus plUsdPartitionImporter::Update(plScene2Document* pScene, const plUuid& partition)
{
  if (m_pJob != nullptr)
    return plStatus("Another USD import is running.");

  PL_SUCCEED_OR_RETURN(plLayerMoveUtils::CanRunBatchJob(pScene));

  plUsdPartitionImportManifest manifest;
  PL_SUCCEED_OR_RETURN(LoadManifest(pScene, partition, manifest));
  return StartJob(pScene, partition, std::move(manifest), false);
}

plStatus plUsdPartitionImporter::StartJob(plScene2Document* pScene, const plUuid& partition, plUsdPartitionImportManifest&& manifest, bool bSceneChanged)
{
  const plSceneWorldPartition* pPartition = pScene->GetPartition(partition);
  if (pPartition == nullptr)
    return plStatus("Unknown world partition.");

  if (pPartition->m_fCellSize != manifest.m_Options.m_fCellSize)
    return plStatus(plFmt("The partition has {} m cells, its import {} m; rebuild the partition with the import's cell size first.", pPartition->m_fCellSize,
      manifest.m_Options.m_fCellSize));

  plUsdPartitionImportInput input;
  input.m_sPartitionFolder = pScene->GetPartitionFolder(*pPartition);
  input.m_sManifestFile = plUsdPartitionImportManifest::GetPath(input.m_sPartitionFolder);
  input.m_Scene = pScene->GetGuid();
  manifest.m_Options.m_sPartitionName = pPartition->m_sName;
  input.m_sDistantFile = GetLayerFile(pScene, pPartition->m_sName, "Distant");

  if (const plWorldPartitionRegistry* pRegistry = pScene->GetWorldStreaming()->GetRegistry(partition))
  {
    for (auto it : pRegistry->GetCells())
    {
      input.m_CellLayers.Insert(it.Key(), it.Value().m_Layer);
      input.m_CellFiles.Insert(it.Key(), it.Value().m_sPath);

      if (const plSceneDocument* pCell = pScene->GetLayerDocument(it.Value().m_Layer); pCell != nullptr && pCell->IsModified())
        input.m_ModifiedCellLayers.Insert(it.Value().m_Layer);
    }
  }

  const plString sStage = manifest.m_Options.m_sStageFile;
  input.m_Manifest = std::move(manifest);

  m_pScene = pScene;
  m_Partition = partition;
  m_sManifestFile = input.m_sManifestFile;
  m_sDistantFile = input.m_sDistantFile;
  m_sGlobalsFile = GetLayerFile(pScene, pPartition->m_sName, "Globals");
  m_bSceneChanged = bSceneChanged;
  m_bWorkerDone = false;
  m_bWaitLogged = false;
  m_bMaterialsDone = false;
  m_pSaveScene = nullptr;
  m_bSceneModifiedAtStart = pScene->IsModified();
  m_uiHistoryStamp = GetHistoryStamp(pScene);

  m_pJob = PL_DEFAULT_NEW(plUsdPartitionImportJob, std::move(input));

  plUsdPartitionImportJob* pJob = m_pJob.Borrow();
  // Maybe: the job waits for its mesh builds.
  plSharedPtr<plDelegateTask<void>> pTask = PL_DEFAULT_NEW(plDelegateTask<void>, "USD partition import", plTaskNesting::Maybe, [pJob]()
    { pJob->Run(); });
  m_Task = plTaskSystem::StartSingleTask(pTask, plTaskPriority::LongRunning);

  plLog::Info("USD Import: importing '{}' into the world partition '{}'", sStage, pPartition->m_sName);

  plUsdPartitionImportEvent e;
  e.m_Type = plUsdPartitionImportEvent::Type::Started;
  e.m_Partition = partition;
  m_Events.Broadcast(e);
  return plStatus(PL_SUCCESS);
}

void plUsdPartitionImporter::Cancel()
{
  if (m_pJob != nullptr)
    m_pJob->Cancel();
}

void plUsdPartitionImporter::GetProgress(plUsdPartitionImportProgress& out_progress) const
{
  if (m_pJob == nullptr)
  {
    out_progress = plUsdPartitionImportProgress();
    return;
  }

  m_pJob->GetProgress(out_progress);
  if (m_bWorkerDone)
    out_progress.m_sPhase = m_bWaitLogged ? "Waiting for the simulation or an edit to end" : "Writing materials and layers";
}

void plUsdPartitionImporter::PassWrittenFiles()
{
  plDynamicArray<plString> layers, meshes;
  m_pJob->TakeWrittenFiles(layers, meshes);

  if (!layers.IsEmpty())
  {
    // Cells appear in the registry, and loaded unmodified ones reload (World Streaming's changed-on-disk check).
    for (const plString& sFile : layers)
      plAssetCurator::GetSingleton()->NotifyOfFileChange(sFile);
    plAssetCurator::GetSingleton()->MainThreadTick(false);
  }

  m_PendingReloads.PushBackRange(meshes);
}

void plUsdPartitionImporter::SendReloads()
{
  const plUInt32 uiEnd = plMath::Min(m_PendingReloads.GetCount(), m_uiNextReload + ReloadsPerTick);
  for (plUInt32 i = m_uiNextReload; i < uiEnd; ++i)
    plUsdLiveResources::Reload("Mesh", m_PendingReloads[i]);
  m_uiNextReload = uiEnd;

  // Sent entries are dropped when all are sent, or in one go once they are the larger part.
  if (m_uiNextReload == m_PendingReloads.GetCount())
  {
    m_PendingReloads.Clear();
    m_uiNextReload = 0;
  }
  else if (m_uiNextReload > 4096 && m_uiNextReload > m_PendingReloads.GetCount() / 2)
  {
    m_PendingReloads.RemoveAtAndCopy(0, m_uiNextReload);
    m_uiNextReload = 0;
  }
}

void plUsdPartitionImporter::Tick()
{
  SendReloads();

  if (m_pJob == nullptr)
  {
    // Cells the asset curator reported late; saved only while nobody else changed the scene.
    if (m_pSaveScene != nullptr)
    {
      if (plTime::Now() > m_SaveUntil || GetHistoryStamp(m_pSaveScene) != m_uiHistoryStamp)
        m_pSaveScene = nullptr;
      else if (m_pSaveScene->GetUnsavedCellCount() > 0 && plLayerMoveUtils::CanRunBatchJob(m_pSaveScene).Succeeded())
        m_pSaveScene->SaveDocument().LogFailure();
    }
    return;
  }

  PassWrittenFiles();

  // The scene closed: wait for the worker to stop, then keep what it did for the next start.
  if (m_bDetached)
  {
    if (!plTaskSystem::IsTaskGroupFinished(m_Task))
      return;

    PassWrittenFiles();
    if (!m_pJob->m_Manifest.m_sImportId.IsEmpty())
      m_pJob->m_Manifest.Save(m_sManifestFile).LogFailure();

    m_bDetached = false;
    FinishAndReport(plStatus("The scene was closed; the import resumes when it is started again."), true);
    return;
  }

  if (plTime::Now() - m_LastProgressEvent > ProgressInterval)
  {
    m_LastProgressEvent = plTime::Now();
    plUsdPartitionImportEvent e;
    e.m_Type = plUsdPartitionImportEvent::Type::Progress;
    e.m_Partition = m_Partition;
    m_Events.Broadcast(e);
  }

  if (!m_bWorkerDone)
  {
    if (!plTaskSystem::IsTaskGroupFinished(m_Task))
      return;

    m_bWorkerDone = true;
    PassWrittenFiles();

    if (m_pJob->m_Result.Failed())
    {
      // What was written stays; the manifest knows it, so Start or Update resumes.
      m_pJob->m_Manifest.Save(m_sManifestFile).LogFailure();
      FinishAndReport(m_pJob->m_Result, m_pJob->IsCancelled());
      return;
    }
  }

  // Materials wait for their textures; the texture cache is main-thread only.
  if (!m_bMaterialsDone)
  {
    if (!WriteMaterials())
      return;
    m_bMaterialsDone = true;
  }

  // Layers are added and cells removed only while the scene does not simulate and no edit is in progress.
  if (const plStatus ready = plLayerMoveUtils::CanRunBatchJob(m_pScene); ready.Failed())
  {
    if (!m_bWaitLogged)
      plLog::Info("USD Import: finishing waits: {}", ready.m_sMessage);
    m_bWaitLogged = true;
    return;
  }

  Finish();
}

bool plUsdPartitionImporter::WriteMaterials()
{
  plUsdLiveManager* pManager = plUsdLiveManager::GetSingleton();
  plUsdLiveTextures* pTextures = pManager != nullptr ? pManager->GetTextures() : nullptr;

  auto& materials = m_pJob->m_Materials;
  if (pTextures != nullptr)
  {
    for (auto it = materials.GetIterator(); it.IsValid(); ++it)
    {
      plUsdSurface surface = it.Value();
      if (!pTextures->Resolve(surface))
        return false;
    }
  }

  auto write = [](plStringView sResource, const plUsdSurface& surface, bool bSoftOpacity)
  {
    plDynamicArray<plUInt8> data;
    plUsdLiveResources::BuildMaterial(surface, bSoftOpacity, data);

    bool bWritten = false;
    if (plUsdBakeStore::WriteFile(sResource, data, &bWritten).Failed())
      plLog::Warning("USD Import: could not write the material '{}'", sResource);
    else if (bWritten)
      plUsdLiveResources::Reload("Material", sResource);
  };

  // Geometry without a material: neutral grey.
  {
    plUsdSurface grey;
    grey.m_BaseColor = plColor(0.5f, 0.5f, 0.5f);
    grey.m_fRoughness = 0.7f;
    write(plUsdPartitionImportData::GetDefaultMaterialResource(m_pJob->m_Manifest.m_sImportId), grey, false);
  }

  for (auto it = materials.GetIterator(); it.IsValid(); ++it)
  {
    plUsdSurface surface = it.Value();
    bool bSoftOpacity = false;
    if (pTextures != nullptr)
    {
      pTextures->Resolve(surface, &bSoftOpacity);
    }
    else
    {
      surface.m_sBaseColorTexture.Clear();
      surface.m_sNormalTexture.Clear();
      surface.m_sRoughnessTexture.Clear();
      surface.m_sMetallicTexture.Clear();
      surface.m_sOpacityTexture.Clear();
    }

    write(it.Key(), surface, bSoftOpacity);
  }

  materials.Clear();
  return true;
}

plStatus plUsdPartitionImporter::EnsureLayer(plStringView sFile, bool& ref_bSceneChanged)
{
  plAssetCurator::GetSingleton()->NotifyOfFileChange(sFile);
  plAssetCurator::GetSingleton()->MainThreadTick(false);

  {
    auto pAsset = plAssetCurator::GetSingleton()->FindSubAsset(sFile);
    if (pAsset.isValid() && m_pScene->HasLayer(pAsset->m_Data.m_Guid))
      return plStatus(PL_SUCCESS);
  }

  // CreateLayer adds the existing file as an always loaded layer.
  plUuid layer;
  const plStringBuilder sName = sFile.GetFileName();
  PL_SUCCEED_OR_RETURN(m_pScene->CreateLayer(sName, layer, plLayerStreamingMode::AlwaysLoaded));
  ref_bSceneChanged = true;
  return plStatus(PL_SUCCESS);
}

plStatus plUsdPartitionImporter::WriteGlobals(plUsdPartitionImportManifest& ref_manifest, bool& ref_bSceneChanged)
{
  const plDynamicArray<plUsdImportObject>& objects = m_pJob->m_GlobalObjects;

  plUInt64 uiHash = objects.IsEmpty() ? 0 : 1;
  for (const plUsdImportObject& o : objects)
    uiHash = o.ComputeHash(uiHash);

  // Unchanged: the layer only has to be in the scene, also after its 'Add Layer' was undone.
  if (uiHash == ref_manifest.m_uiGlobalsHash && (objects.IsEmpty() || plOSFile::ExistsFile(m_sGlobalsFile)))
    return objects.IsEmpty() ? plStatus(PL_SUCCESS) : EnsureLayer(m_sGlobalsFile, ref_bSceneChanged);

  if (objects.IsEmpty() && !plOSFile::ExistsFile(m_sGlobalsFile))
  {
    ref_manifest.m_uiGlobalsHash = 0;
    return plStatus(PL_SUCCESS);
  }

  const plString& sImportId = ref_manifest.m_sImportId;

  // Light and camera components are made as the live link makes them, in a manager of their own, then written out.
  plDocumentObjectManager manager;
  plObjectDirectAccessor accessor(&manager);
  plAbstractObjectGraph graph;
  plUsdLayerExtraObjects extra;
  extra.m_pGraph = &graph;

  {
    plDocumentObjectConverterWriter writer(&graph, &manager);

    for (const plUsdImportObject& o : objects)
    {
      plUuid guid = plUsdPartitionImportData::MakeGuid(sImportId, o.m_sKey);
      if (accessor.AddObjectByName(manager.GetRootObject(), "Children", -1, plGetStaticRTTI<plGameObject>(), guid).Failed())
        continue;

      plDocumentObject* pObject = manager.GetObject(guid);
      manager.SetValue(pObject, "Name", o.m_sName).IgnoreResult();
      manager.SetValue(pObject, "LocalPosition", o.m_Transform.m_vPosition).IgnoreResult();
      manager.SetValue(pObject, "LocalRotation", o.m_Transform.m_qRotation).IgnoreResult();
      manager.SetValue(pObject, "LocalScaling", o.m_Transform.m_vScale).IgnoreResult();
      manager.InsertValue(pObject, "Tags", plString(plUsdPartitionCellWriter::GeneratedTag), -1).IgnoreResult();

      plUsdPrim prim;
      prim.m_sPath = o.m_sPrimPath;
      prim.m_sName = o.m_sName;
      prim.m_Kind = o.m_Kind == plUsdImportObjectKind::Light ? plUsdPrimKind::Light : plUsdPrimKind::Camera;
      prim.m_Light = o.m_Light;
      prim.m_Camera = o.m_Camera;
      plUsdPrimComponents::Apply(&manager, pObject, prim, plUsdPartitionImportData::MakeGuid(sImportId, plStringBuilder(o.m_sKey, "#component")));

      writer.AddObjectToGraph(pObject);
      extra.m_Roots.PushBack(guid);
    }
  }

  plUsdLayerIdentity identity;
  identity.m_Scene = m_pScene->GetGuid();
  identity.m_Layer = plUsdPartitionImportData::MakeGuid(sImportId, "#globals");

  plUsdLayerWriteResult result;
  const plString sStaging = plStringBuilder(plUsdPartitionImportData::GetWorkFolder(sImportId), "/Staging");
  PL_SUCCEED_OR_RETURN(plUsdPartitionCellWriter::WriteLayer(m_sGlobalsFile, sStaging, sImportId, identity, {}, &extra, m_pJob->GetKnownGenerated(), result));

  ref_manifest.m_uiGlobalsHash = uiHash;
  if (!objects.IsEmpty())
    PL_SUCCEED_OR_RETURN(EnsureLayer(m_sGlobalsFile, ref_bSceneChanged));
  else
    plAssetCurator::GetSingleton()->NotifyOfFileChange(m_sGlobalsFile);

  return plStatus(PL_SUCCESS);
}

void plUsdPartitionImporter::RemoveEmptiedCells()
{
  plUInt32 uiRemoved = 0;
  plString sBackup;
  for (const plString& sFile : m_pJob->m_EmptiedCells)
  {
    auto pAsset = plAssetCurator::GetSingleton()->FindSubAsset(sFile);
    const plUuid layer = pAsset.isValid() ? pAsset->m_Data.m_Guid : plUuid();

    // A cell of the scene goes the way the scene removes cells; a file it does not know is only moved away.
    if (layer.IsValid() && m_pScene->HasLayer(layer))
    {
      const plStatus res = m_pScene->DeleteCellLayer(layer);
      if (res.Failed())
        plLog::Warning("USD Import: '{}' has no objects left but stays: {}", sFile, res.m_sMessage);
      else
        ++uiRemoved;
      continue;
    }

    if (sBackup.IsEmpty())
      sBackup = plLayerMoveUtils::GetBackupFolder(m_pScene);

    if (plLayerMoveUtils::MoveFileToFolder(sFile, sBackup).Succeeded())
    {
      plAssetCurator::GetSingleton()->NotifyOfFileChange(sFile);
      ++uiRemoved;
    }
  }

  if (!sBackup.IsEmpty())
    plAssetCurator::GetSingleton()->MainThreadTick(false);

  if (uiRemoved > 0)
    plLog::Info("USD Import: moved {} cell(s) without objects to the backup folder", uiRemoved);
}

void plUsdPartitionImporter::Finish()
{
  plUsdPartitionImportJob& job = *m_pJob;
  plUsdPartitionImportManifest& manifest = job.m_Manifest;
  bool bSceneChanged = m_bSceneChanged;

  // Decided before the import adds its layers, which are commands of the scene's history too.
  const bool bUserChanges = m_bSceneModifiedAtStart || GetHistoryStamp(m_pScene) != m_uiHistoryStamp;

  plStatus result = WriteGlobals(manifest, bSceneChanged);
  if (result.Succeeded() && job.m_uiDistantObjects > 0)
    result = EnsureLayer(m_sDistantFile, bSceneChanged);

  if (result.Failed())
  {
    manifest.Save(m_sManifestFile).LogFailure();
    FinishAndReport(result);
    return;
  }

  RemoveEmptiedCells();

  manifest.m_bComplete = true;
  manifest.Save(m_sManifestFile).LogFailure();

  // The scene is saved for the import's layers and cells, but never with edits of the user's. The layers it added are
  // ordinary 'Add Layer' steps of the undo history.
  if (bUserChanges)
  {
    if (bSceneChanged || m_pScene->IsModified())
      plLog::Warning("USD Import: the scene has other unsaved changes, so it was not saved; save it to keep the import's layers and cells.");
  }
  else
  {
    if (bSceneChanged || m_pScene->IsModified())
      m_pScene->SaveDocument().LogFailure();

    m_pSaveScene = m_pScene;
    m_SaveUntil = plTime::Now() + plTime::MakeFromSeconds(10);
    m_uiHistoryStamp = GetHistoryStamp(m_pScene);
  }

  plUsdPartitionImportProgress progress;
  job.GetProgress(progress);
  plLog::Success("USD Import: '{}' into '{}' in {} s: {} roots scanned, {} unchanged; {} meshes built, {} reused", manifest.m_Options.m_sStageFile,
    manifest.m_Options.m_sPartitionName, plArgF(progress.m_Elapsed.GetSeconds(), 1), job.m_uiRootsScanned, job.m_uiRootsSkipped, progress.m_uiMeshesBuilt,
    job.m_uiMeshesReused);
  plLog::Info("USD Import: {} objects in {} cells ({} written, {} unchanged), {} distant, {} lights and cameras; {} problem(s) listed in '{}'", job.m_uiObjects,
    manifest.m_Cells.GetCount(), job.m_uiCellsWritten, job.m_uiCellsUnchanged, job.m_uiDistantObjects, job.m_GlobalObjects.GetCount(), manifest.m_Problems.GetCount(),
    m_sManifestFile);

  FinishAndReport(plStatus(PL_SUCCESS));
}

void plUsdPartitionImporter::FinishAndReport(const plStatus& result, bool bCancelled)
{
  if (bCancelled)
    plLog::Info("USD Import: {}", result.m_sMessage);
  else if (result.Failed())
    plLog::Error("USD Import: {}", result.m_sMessage);

  plUsdPartitionImportEvent e;
  e.m_Type = plUsdPartitionImportEvent::Type::Finished;
  e.m_Partition = m_Partition;
  e.m_Result = result;
  e.m_bCancelled = bCancelled;

  m_pJob.Clear();
  m_pScene = nullptr;
  m_Events.Broadcast(e);
}

plStatus plUsdPartitionImporter::StartFromJson(plStringView sJson)
{
  plVariantDictionary json;
  if (!plUsdJson::Parse(sJson, json))
    return plStatus("The import options are no JSON object.");

  if (plUsdJson::GetBool(json, "cancel"))
  {
    if (m_pJob == nullptr)
      return plStatus("No USD import is running.");
    Cancel();
    return plStatus(PL_SUCCESS);
  }

  // The scene: named by path, else the only open one.
  plScene2Document* pScene = nullptr;
  const plString sScene = plUsdJson::GetString(json, "scene");
  for (plDocumentManager* pManager : plDocumentManager::GetAllDocumentManagers())
  {
    for (plDocument* pDocument : pManager->GetAllOpenDocuments())
    {
      plScene2Document* pCandidate = plDynamicCast<plScene2Document*>(pDocument);
      if (pCandidate == nullptr)
        continue;

      if (!sScene.IsEmpty())
      {
        plStringBuilder a = pCandidate->GetDocumentPath(), b = sScene;
        a.MakeCleanPath();
        b.MakeCleanPath();
        if (!a.IsEqual_NoCase(b))
          continue;
      }
      else if (pScene != nullptr)
      {
        return plStatus("Several scenes are open; name one with \"scene\".");
      }

      pScene = pCandidate;
    }
  }

  if (pScene == nullptr)
  {
    if (sScene.IsEmpty())
      return plStatus("No scene is open.");
    return plStatus(plFmt("The scene '{}' is not open.", sScene));
  }

  // Update of a partition by name.
  if (plUsdJson::GetBool(json, "update"))
  {
    const plString sName = plUsdJson::GetString(json, "partition");
    if (const plSceneWorldPartition* pPartition = FindPartition(pScene, sName))
      return Update(pScene, pPartition->m_Partition);
    return plStatus(plFmt("The scene has no partition '{}'.", sName));
  }

  plUsdPartitionImportOptions options;
  PL_SUCCEED_OR_RETURN(options.ReadJson(json));

  if (options.m_sPartitionName.IsEmpty())
    options.m_sPartitionName = plStringView(options.m_sStageFile).GetFileName();

  // An earlier import of that partition keeps the options the request does not give.
  if (const plSceneWorldPartition* pPartition = FindPartition(pScene, options.m_sPartitionName))
  {
    plUsdPartitionImportManifest manifest;
    if (LoadManifest(pScene, pPartition->m_Partition, manifest).Succeeded())
    {
      const plString sName = options.m_sPartitionName;
      options = manifest.m_Options;
      PL_SUCCEED_OR_RETURN(options.ReadJson(json));
      options.m_sPartitionName = sName;
    }
  }

  return Start(pScene, options);
}
