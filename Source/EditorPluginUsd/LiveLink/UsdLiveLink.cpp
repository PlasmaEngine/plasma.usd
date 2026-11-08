#include <EditorPluginUsd/EditorPluginUsdPCH.h>

#include <Core/Graphics/Geometry.h>
#include <Core/World/GameObject.h>
#include <EditorEngineProcessFramework/EngineProcess/EngineProcessMessages.h>
#include <EditorFramework/Assets/AssetDocument.h>
#include <EditorFramework/Document/GameObjectDocument.h>
#include <EditorFramework/DocumentWindow/EngineDocumentWindow.h>
#include <EditorFramework/DocumentWindow/EngineViewWidget.h>
#include <EditorFramework/GUI/ObjectRowDecorator.h>
#include <EditorPluginUsd/Bake/UsdBakePrefab.h>
#include <EditorPluginUsd/LiveLink/UsdBakeStore.h>
#include <EditorPluginUsd/LiveLink/UsdLiveLink.h>
#include <EditorPluginUsd/LiveLink/UsdLiveManager.h>
#include <EditorPluginUsd/LiveLink/UsdLiveResources.h>
#include <EditorPluginUsd/LiveLink/UsdLiveTextures.h>
#include <EditorPluginUsd/LiveLink/UsdMeshCache.h>
#include <EditorPluginUsd/LiveLink/UsdPrimComponents.h>
#include <Foundation/Algorithm/HashingUtils.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Profiling/Profiling.h>
#include <Foundation/System/SystemInformation.h>
#include <Foundation/Threading/DelegateTask.h>
#include <RendererCore/Meshes/InstancedMeshComponent.h>
#include <RendererCore/Meshes/MeshComponent.h>
#include <ToolsFoundation/CommandHistory/CommandHistory.h>
#include <ToolsFoundation/Document/Document.h>
#include <ToolsFoundation/Object/ObjectDirectAccessor.h>

#include <Foundation/Basics/Platform/Win/IncludeWindows.h>

namespace
{
  /// The local time of day as "HH:mm".
  plString GetLocalClockTime()
  {
    SYSTEMTIME time = {};
    GetLocalTime(&time);

    plStringBuilder s;
    s.SetFormat("{}:{}", plArgU(time.wHour, 2, true), plArgU(time.wMinute, 2, true));
    return s;
  }

  /// Leave room for the USD stage, engine and other tools. Dense meshes need substantial transient memory, so adding
  /// workers under memory pressure makes imports slower through paging.
  plUInt32 GetMaxConcurrentMeshJobs()
  {
    const auto& system = plSystemInformation::Get();
    const plUInt32 uiCpuLimit = plMath::Clamp<plUInt32>(system.GetCPUCoreCount() / 4, 2, 6);
    const plUInt64 available = system.GetAvailableMainMemory();
    constexpr plUInt64 GiB = 1024ull * 1024ull * 1024ull;
    const plUInt64 uiMemoryLimit = available > GiB ? plMath::Max<plUInt64>((available - GiB) / (2 * GiB), 1) : 1;
    return (plUInt32)plMath::Min<plUInt64>(uiCpuLimit, uiMemoryLimit);
  }

  /// How often the layer files are checked for changes saved by other tools.
  constexpr plTime PollInterval = plTime::MakeFromMilliseconds(250);

  /// Edits are collected and the participant layer saved this long after the last one, so a burst of edits is one save.
  constexpr plTime SaveDelay = plTime::MakeFromMilliseconds(500);

  /// The bake waits this long after the last change, so a burst of saves from another tool is one bake.
  constexpr plTime BakeDelay = plTime::MakeFromSeconds(2);

  bool IsTransformProperty(plStringView sName)
  {
    return sName == "LocalPosition" || sName == "LocalRotation" || sName == "LocalScaling" || sName == "LocalUniformScaling";
  }
} // namespace

plUsdLiveLink::plUsdLiveLink(plDocument* pDocument, const plUuid& rootObject, const Settings& settings)
  : m_pDocument(pDocument)
  , m_RootObject(rootObject)
  , m_Settings(settings)
{
  m_sLinkId = MakeLinkId(rootObject, settings.m_sStageFile);

  m_PropertySubscription = pDocument->GetObjectManager()->m_PropertyEvents.AddEventHandler(plMakeDelegate(&plUsdLiveLink::OnPropertyEvent, this));
  m_StructureSubscription = pDocument->GetObjectManager()->m_StructureEvents.AddEventHandler(plMakeDelegate(&plUsdLiveLink::OnStructureEvent, this));
  m_HistorySubscription = pDocument->GetCommandHistory()->m_Events.AddEventHandler(plMakeDelegate(&plUsdLiveLink::OnCommandHistoryEvent, this));

  m_pAssetDocument = plDynamicCast<plAssetDocument*>(pDocument);
  if (m_pAssetDocument)
  {
    m_EngineMessageSubscription = m_pAssetDocument->m_ProcessMessageEvent.AddEventHandler(plMakeDelegate(&plUsdLiveLink::OnEngineMessage, this));
  }
}

plUsdLiveLink::~plUsdLiveLink()
{
  Close(false);

  m_pDocument->GetObjectManager()->m_PropertyEvents.RemoveEventHandler(m_PropertySubscription);
  m_pDocument->GetObjectManager()->m_StructureEvents.RemoveEventHandler(m_StructureSubscription);
  m_pDocument->GetCommandHistory()->m_Events.RemoveEventHandler(m_HistorySubscription);

  if (m_pAssetDocument)
  {
    m_pAssetDocument->m_ProcessMessageEvent.RemoveEventHandler(m_EngineMessageSubscription);
  }
}

bool plUsdLiveLink::IsEngineReady() const
{
  return m_pAssetDocument == nullptr || m_pAssetDocument->GetEngineStatus() == plAssetDocument::EngineStatus::Loaded;
}

void plUsdLiveLink::OnEngineMessage(const plEditorEngineDocumentMsg* pMsg)
{
  if (!pMsg->GetDynamicRTTI()->IsDerivedFrom<plDocumentOpenResponseMsgToEditor>())
    return;

  // The engine (re)loaded the document: the mirror re-sends every object, whose meshes load from their files. Materials
  // sent before are gone with the old engine process; they are sent again.
  m_SentMaterials.Clear();
  m_bResendMaterials = true;
}

plString plUsdLiveLink::GetPrimPath(const plUuid& object) const
{
  plString sPath;
  m_ObjectToPrim.TryGetValue(object, sPath);
  return sPath;
}

plUuid plUsdLiveLink::GetObjectGuid(plStringView sPrimPath) const
{
  plStringBuilder s("usd:", m_sLinkId, sPrimPath);
  return plUuid::MakeStableUuidFromString(s);
}

plUuid plUsdLiveLink::GetMeshComponentGuid(plStringView sPrimPath, plUInt32 uiPart) const
{
  plStringBuilder s;
  s.SetFormat("usd:{}{}#mesh{}", m_sLinkId, sPrimPath, uiPart);
  return plUuid::MakeStableUuidFromString(s);
}

plUuid plUsdLiveLink::GetPrimComponentGuid(plStringView sPrimPath) const
{
  plStringBuilder s("usd:", m_sLinkId, sPrimPath, "#component");
  return plUuid::MakeStableUuidFromString(s);
}

plString plUsdLiveLink::GetInstanceGuidSeed(plStringView sInstancerPath, plStringView sMeshPath, plUInt32 uiPart) const
{
  plStringBuilder s;
  s.SetFormat("usd:{}{}#instances{}#{}", m_sLinkId, sInstancerPath, sMeshPath, uiPart);
  return s;
}

plString plUsdLiveLink::GetMeshResource(const PrimState& state, plUInt32 uiPart) const
{
  const Mesh* pMesh = m_Meshes.GetValue(state.m_uiAppliedMeshKey);
  if (pMesh == nullptr)
    return {};

  return pMesh->m_bPreview ? pMesh->m_sPreviewResource : plUsdBakeStore::GetMeshResource(pMesh->m_sName, uiPart);
}

plString plUsdLiveLink::GetMaterialResource(const plUsdMaterialGroup& group) const
{
  return plUsdBakeStore::GetMaterialResource(m_sLinkId, group);
}

plColor plUsdLiveLink::GetTint(const PrimState& state)
{
  return plUsdBakeStore::GetTint(state.m_MaterialGroups);
}

plString plUsdLiveLink::MakeLinkId(const plUuid& rootObject, plStringView sStageFile)
{
  // Resource ids and generated GUIDs carry this, so two links to the same stage in one scene never collide.
  plStringBuilder sGuid, sKey, sId;
  plConversionUtils::ToString(rootObject, sGuid);
  sKey.SetFormat("{}|{}", sGuid, sStageFile);
  sId.SetFormat("{}", plArgU(plHashingUtils::xxHash32String(sKey), 8, true, 16));
  return sId;
}

plStatus plUsdLiveLink::Open()
{
  Close(false);

  if (!m_Settings.m_sSessionName.IsEmpty())
  {
    plString sSessionRoot;
    plStatus res = plUsdStage::CreateSession(m_Settings.m_sStageFile, m_Settings.m_sSessionName, m_Settings.m_sParticipant, sSessionRoot, m_sParticipantLayer);
    if (res.Failed())
    {
      m_sStatus = res.m_sMessage;
      return res;
    }

    m_sOpenedFile = sSessionRoot;
  }
  else
  {
    m_sOpenedFile = m_Settings.m_sStageFile;
    m_sParticipantLayer.Clear();
  }

  plStatus res = m_Stage.Open(m_sOpenedFile);
  if (res.Failed())
  {
    m_sStatus = res.m_sMessage;
    return res;
  }

  if (!m_sParticipantLayer.IsEmpty())
  {
    res = m_Stage.SetEditLayer(m_sParticipantLayer);
    if (res.Failed())
    {
      m_sStatus = res.m_sMessage;
      return res;
    }
  }

  if (IsWritable())
    plLog::Info("USD Live: opened '{}', writing to '{}'", m_sOpenedFile, m_sParticipantLayer);
  else
    plLog::Info("USD Live: opened '{}' read-only", m_sOpenedFile);

  // A reopen, e.g. to start a session, keeps the manifest, which already lists what this session built.
  if (!m_bManifestLoaded)
  {
    plUsdBakeStore::LoadManifest(m_sLinkId, m_Manifest);
    m_bManifestLoaded = true;
  }

  m_bRefreshAllRows = true;
  Sync();
  m_NextPoll = plTime::Now() + PollInterval;

  // Checked once everything is built; the prefab is only written again when something differs from the last bake.
  MarkBakeDirty();
  return plStatus(PL_SUCCESS);
}

void plUsdLiveLink::MarkBakeDirty()
{
  m_bBakeCheckPending = true;
  m_BakeDue = plTime::Now() + BakeDelay;

  if (m_sBakeStatus.IsEmpty() || !m_sBakeStatus.StartsWith("Failed"))
    m_sBakeStatus = "Waiting for changes to settle";
}

void plUsdLiveLink::RequestBake(bool bRebuild)
{
  if (bRebuild)
  {
    // Every mesh a prim needs is built and written again, and every material sent and written again.
    m_Manifest.m_uiContentHash = 0;

    plSet<plUInt64> needed;
    for (auto it = m_Prims.GetIterator(); it.IsValid(); ++it)
    {
      if (it.Value().m_uiMeshKey != 0)
        needed.Insert(it.Value().m_uiMeshKey);
    }

    for (plUInt64 uiMeshKey : needed)
      QueueMesh(uiMeshKey);

    m_SentMaterials.Clear();
    m_bResendMaterials = true;
  }

  m_bBakeCheckPending = true;
  m_BakeDue = plTime::Now();
  m_sBakeStatus = "Waiting for the meshes to be built";
}

void plUsdLiveLink::RunBake()
{
  m_bBakeCheckPending = false;

  plDynamicArray<plUsdPrim> prims;
  m_Stage.ReadPrims(prims);

  plDynamicArray<plUsdBakeNode> nodes;
  nodes.SetCount(prims.GetCount());
  for (plUInt32 i = 0; i < prims.GetCount(); ++i)
  {
    nodes[i].m_Prim = std::move(prims[i]);

    const PrimState* pState = nullptr;
    if (m_Prims.TryGetValue(nodes[i].m_Prim.m_sPath, pState))
    {
      if (const Mesh* pMesh = m_Meshes.GetValue(pState->m_uiAppliedMeshKey))
      {
        if (!pMesh->m_bComplete)
        {
          m_sBakeStatus = "Waiting for full-quality meshes";
          return;
        }

        nodes[i].m_uiMeshParts = pState->m_uiMeshParts;
        nodes[i].m_sMesh = pMesh->m_sName;
      }
    }
  }

  // Mesh prims that left the stage have no files the bake names any more.
  {
    plSet<plString> alive;
    for (const plUsdBakeNode& node : nodes)
      alive.Insert(node.m_Prim.m_sPath);

    plHybridArray<plString, 8> gone;
    for (auto it = m_Manifest.m_Prims.GetIterator(); it.IsValid(); ++it)
    {
      if (!alive.Contains(it.Key()))
        gone.PushBack(it.Key());
    }
    for (const plString& sPath : gone)
      m_Manifest.m_Prims.Remove(sPath);
  }

  const plString sPrefab = plUsdBakePrefab::GetPrefabPath(m_pDocument->GetDocumentPath(), m_Settings.m_sStageFile, m_sLinkId);
  const plUInt64 uiContent = plUsdBakePrefab::GetContentHash(nodes, m_SentMaterials);

  if (uiContent != m_Manifest.m_uiContentHash || !m_Manifest.m_Prefab.IsValid() || !plOSFile::ExistsFile(sPrefab))
  {
    plUuid prefabGuid;
    const plStatus res = plUsdBakePrefab::Write(sPrefab, m_sLinkId, nodes, prefabGuid);
    if (res.Failed())
    {
      m_sBakeStatus = plStringBuilder("Failed: ", res.m_sMessage);
      plLog::Error("USD Live: could not bake '{}': {}", m_Settings.m_sStageFile, res.m_sMessage);
      return;
    }

    m_Manifest.m_Prefab = prefabGuid;
    m_Manifest.m_uiContentHash = uiContent;
    plLog::Info("USD Live: baked '{}' into '{}'", m_Settings.m_sStageFile, sPrefab);
  }

  plUsdBakeStore::RecordLayers(m_Stage, m_Manifest.m_Layers);
  plUsdBakeStore::RemoveUnusedMeshes(m_sLinkId, m_Manifest);
  if (plUsdBakeStore::SaveManifest(m_sLinkId, m_Manifest).Failed())
    plLog::Warning("USD Live: could not save the bake manifest of '{}'", m_Settings.m_sStageFile);

  m_bApplyingRemote = true;
  plUsdBakePrefab::EnsureBakeObject(m_pDocument, m_RootObject, m_sLinkId, m_Manifest.m_Prefab);
  m_bApplyingRemote = false;

  m_sBakeStatus = plStringBuilder("Up to date, ", GetLocalClockTime());
}

void plUsdLiveLink::Close(bool bRemoveObjects)
{
  FinishMeshJobs(true);
  CheckpointMeshCache();
  m_MeshQueue.Clear();
  m_RefineQueue.Clear();
  m_MeshUsers.Clear();
  m_ApplyQueue.Clear();
  m_PendingApplications.Clear();

  if (m_Stage.IsOpen() && m_Stage.IsEditLayerDirty())
  {
    SaveNow().IgnoreResult();
  }

  if (bRemoveObjects)
  {
    plObjectDirectAccessor accessor(m_pDocument->GetObjectManager());
    m_bApplyingRemote = true;

    for (auto it = m_Prims.GetIterator(); it.IsValid(); ++it)
    {
      if (const plDocumentObject* pObject = accessor.GetObject(it.Value().m_Object))
      {
        plUsdPrimComponents::RemoveObject(m_pDocument->GetObjectManager(), pObject).IgnoreResult();
      }

      m_pDocument->GetObjectManager()->SetObjectTransient(it.Value().m_Object, false);
    }

    m_bApplyingRemote = false;

    if (!m_Prims.IsEmpty())
      ForgetUndoHistory("a USD link was removed");

    plObjectRowDecoratorRegistry::RequestRefresh(m_pDocument);
  }

  m_Prims.Clear();
  m_ObjectToPrim.Clear();
  m_Meshes.Clear();
  m_SourceContents.Clear();
  m_InstancersByMesh.Clear();
  m_bInstancersByMeshDirty = true;
  m_PendingMaterials.Clear();
  m_SentMaterials.Clear();
  m_DirtyTransforms.Clear();
  m_DirtyVisibility.Clear();
  m_bSaveScheduled = false;
  m_Stage.Close();
}

plStatus plUsdLiveLink::StartSession(plStringView sSessionName, plStringView sParticipant)
{
  m_Settings.m_sSessionName = sSessionName;
  m_Settings.m_sParticipant = sParticipant;

  // Reopening keeps the generated objects: their GUIDs come from the prim paths, so the resync updates them in place.
  return Open();
}

void plUsdLiveLink::RequestFullResync()
{
  m_SentMaterials.Clear();

  // The next sync gives every object its mesh components again; meshes already built are not built again.
  for (auto it = m_Prims.GetIterator(); it.IsValid(); ++it)
  {
    it.Value().m_uiAppliedMeshKey = 0;
  }

  m_bSyncPending = true;
}

plStatus plUsdLiveLink::SaveNow()
{
  m_bSaveScheduled = false;

  if (!m_Stage.IsOpen() || !m_Stage.IsEditLayerDirty())
    return plStatus(PL_SUCCESS);

  ++m_uiOwnLayerVersion;
  plStatus res = m_Stage.SaveEditLayer();
  if (res.Failed())
  {
    plLog::Error("USD Live: {}", res.m_sMessage);
    m_sStatus = res.m_sMessage;
  }

  // Our own save changes the file's time stamp; the stage already has these opinions, so it is not reloaded.
  plUsdChanges ignored;
  m_Stage.ConsumeChanges(ignored);
  return res;
}

void plUsdLiveLink::Update()
{
  if (!m_Stage.IsOpen())
    return;

  if (m_pDocument->GetObjectManager()->GetObject(m_RootObject) == nullptr)
  {
    m_sStatus = "The linked object was deleted.";
    m_bRootMissing = true;
    return;
  }

  if (m_bRootMissing)
  {
    // Undo or redo brought the root back without its generated children.
    m_bRootMissing = false;
    m_sStatus.Clear();
    RequestFullResync();
  }

  FinishMeshJobs(false);
  FlushMeshApplications();
  FlushInstancers();

  if (m_bResendMaterials && IsEngineReady())
  {
    m_bResendMaterials = false;

    for (auto it = m_Prims.GetIterator(); it.IsValid(); ++it)
    {
      if (it.Value().m_uiMeshParts > 0)
        SendMaterials(it.Key(), it.Value());
    }
  }

  const plUInt32 uiTexturesFinished = plUsdLiveManager::GetSingleton()->GetTextures()->GetFinishedCount();
  if (!m_PendingMaterials.IsEmpty() && IsEngineReady() && uiTexturesFinished != m_uiTexturesFinished)
  {
    m_uiTexturesFinished = uiTexturesFinished;

    plHybridArray<plString, 16> pending;
    for (const plString& sPath : m_PendingMaterials)
      pending.PushBack(sPath);

    for (const plString& sPath : pending)
    {
      PrimState* pState = nullptr;
      if (m_Prims.TryGetValue(sPath, pState))
        SendMaterials(sPath, *pState);
      else
        m_PendingMaterials.Remove(sPath);
    }
  }

  if (!m_DirtyTransforms.IsEmpty() || !m_DirtyVisibility.IsEmpty() || !m_DirtyComponents.IsEmpty())
  {
    if (!m_pDocument->GetCommandHistory()->IsInTransaction())
      FlushWriteBack();
  }

  const plTime now = plTime::Now();

  if (m_bManifestDirty && now >= m_NextManifestSave)
  {
    CheckpointMeshCache();
  }

  if (m_bSaveScheduled && now >= m_SaveDue)
  {
    SaveNow().IgnoreResult();
  }

  if (now >= m_NextPoll)
  {
    m_NextPoll = now + PollInterval;

    const plUInt32 uiReloaded = m_Stage.ReloadChangedLayers(m_RemoteLayers);
    if (uiReloaded > 0)
    {
      plLog::Dev("USD Live: {} layer(s) changed on disk", uiReloaded);
      m_bSyncPending = true;
    }
  }

  if (m_bSyncPending)
  {
    Sync();
  }

  LaunchMeshJobs();

  // The bake waits until the link is idle: every mesh built, every texture prepared, nothing left to sync.
  if (m_bBakeCheckPending && now >= m_BakeDue && GetPendingMeshCount() == 0 && m_ApplyQueue.IsEmpty() && m_PendingMaterials.IsEmpty() && !m_bSyncPending &&
      !m_pDocument->GetCommandHistory()->IsInTransaction() && IsEngineReady())
  {
    RunBake();
  }

  if (m_sStatus.IsEmpty() || m_sStatus.StartsWith("Building") || m_sStatus.StartsWith("Live") || m_sStatus.StartsWith("Read-only"))
  {
    plStringBuilder s;
    if (GetPendingMeshCount() > 0)
    {
      plUInt32 visible = 0, complete = 0, meshes = 0;
      for (auto it = m_Prims.GetIterator(); it.IsValid(); ++it)
      {
        if (it.Value().m_uiMeshKey == 0)
        {
          continue;
        }

        ++meshes;
        visible += it.Value().m_uiMeshParts > 0 ? 1 : 0;
        const Mesh* mesh = m_Meshes.GetValue(it.Value().m_uiMeshKey);
        complete += mesh != nullptr && mesh->m_bComplete ? 1 : 0;
      }

      s.SetFormat("Building: {} / {} mesh objects visible, {} refined, {} active builds", visible, meshes, complete, m_Jobs.GetCount());
    }
    else if (IsWritable())
      s.SetFormat("Live: {} prims, session '{}'", m_Prims.GetCount(), m_Settings.m_sSessionName);
    else
      s.SetFormat("Read-only: {} prims", m_Prims.GetCount());
    m_sStatus = s;
  }
}

void plUsdLiveLink::SetProperty(const plDocumentObject* pObject, plStringView sProperty, const plVariant& value)
{
  plDocumentObjectManager* pManager = m_pDocument->GetObjectManager();
  pManager->SetValue(pManager->GetObject(pObject->GetGuid()), sProperty, value).IgnoreResult();
}

void plUsdLiveLink::AddTag(const plDocumentObject* pObject, plStringView sTag)
{
  const plVariant tags = pObject->GetTypeAccessor().GetValue("Tags");
  if (tags.IsA<plVariantArray>())
  {
    for (const plVariant& tag : tags.Get<plVariantArray>())
    {
      if (tag.IsA<plString>() && tag.Get<plString>() == sTag)
        return;
    }
  }

  plDocumentObjectManager* pManager = m_pDocument->GetObjectManager();
  pManager->InsertValue(pManager->GetObject(pObject->GetGuid()), "Tags", plString(sTag), -1).IgnoreResult();
}

void plUsdLiveLink::SetObjectTransform(const plDocumentObject* pObject, const plTransform& t)
{
  SetProperty(pObject, "LocalPosition", t.m_vPosition);
  SetProperty(pObject, "LocalRotation", t.m_qRotation);
  SetProperty(pObject, "LocalScaling", t.m_vScale);
  SetProperty(pObject, "LocalUniformScaling", 1.0f);
}

void plUsdLiveLink::Sync()
{
  // Never interleave with a command the user has open, e.g. a gizmo drag.
  if (m_pDocument->GetCommandHistory()->IsInTransaction() || m_pDocument->GetCommandHistory()->IsInUndoRedo())
  {
    m_bSyncPending = true;
    return;
  }

  m_bSyncPending = false;

  plUsdChanges changes;
  m_Stage.ConsumeChanges(changes);

  plUsdGeometryChanges geometryChanges;
  m_Stage.ConsumeGeometryChanges(geometryChanges);

  // Whether the prim or one of its ancestors was resynced.
  const bool bRootResynced = geometryChanges.m_Resynced.Contains(plString("/"));
  auto isResynced = [&](plStringView sPath)
  {
    if (bRootResynced)
      return true;

    if (geometryChanges.m_Resynced.IsEmpty())
      return false;

    plStringView s = sPath;
    while (s.GetElementCount() > 1)
    {
      if (geometryChanges.m_Resynced.Contains(plString(s)))
        return true;

      const char* szSlash = s.FindLastSubString("/");
      if (szSlash == nullptr || szSlash == s.GetStartPointer())
        break;

      s = plStringView(s.GetStartPointer(), szSlash);
    }

    return false;
  };

  // Every participant layer sits in the session folder beside the session root layer.
  {
    const plUInt32 uiBefore = m_uiParticipantCount;
    m_uiParticipantCount = 0;

    if (IsWritable())
    {
      plStringBuilder sFolder = m_sParticipantLayer;
      sFolder.PathParentDirectory();

      plHybridArray<plUsdLayerInfo, 16> layers;
      m_Stage.GetLayerStack(layers);

      for (const plUsdLayerInfo& layer : layers)
      {
        plStringBuilder sLayerFolder = layer.m_sRealPath;
        sLayerFolder.PathParentDirectory();

        if (!layer.m_sRealPath.IsEmpty() && sLayerFolder.IsEqual_NoCase(sFolder) && layer.m_sRealPath.GetFileNameAndExtension() != "session.usda")
          ++m_uiParticipantCount;
      }
    }

    if (uiBefore != m_uiParticipantCount)
      m_bRefreshAllRows = true;
  }

  plDynamicArray<plUsdPrim> prims;
  m_Stage.ReadPrims(prims);

  plObjectDirectAccessor accessor(m_pDocument->GetObjectManager());
  const plDocumentObject* pRoot = accessor.GetObject(m_RootObject);
  if (pRoot == nullptr)
    return;

  m_bApplyingRemote = true;
  bool bContentChanged = false;

  // Prims that no longer exist.
  {
    plSet<plString> alive;
    plHybridArray<plString, 8> inactive;
    for (const plUsdPrim& prim : prims)
    {
      alive.Insert(prim.m_sPath);
      if (!prim.m_bActive)
        inactive.PushBack(plStringBuilder(prim.m_sPath, "/"));
    }

    // A deactivated prim's children are not on the stage, but their objects stay, hidden with the parent, so
    // reactivating brings them back as they were.
    auto isBelowInactive = [&](plStringView sPath)
    {
      for (const plString& sInactive : inactive)
      {
        if (sPath.StartsWith(sInactive))
          return true;
      }
      return false;
    };

    plHybridArray<plString, 16> removed;
    for (auto it = m_Prims.GetIterator(); it.IsValid(); ++it)
    {
      if (!alive.Contains(it.Key()) && !isBelowInactive(it.Key()))
        removed.PushBack(it.Key());
    }

    for (const plString& sPath : removed)
    {
      const plUuid object = m_Prims[sPath].m_Object;

      // Removing a parent takes its children with it, so a child may already be gone.
      if (const plDocumentObject* pObject = accessor.GetObject(object))
      {
        plUsdPrimComponents::RemoveObject(m_pDocument->GetObjectManager(), pObject).IgnoreResult();
      }

      m_pDocument->GetObjectManager()->SetObjectTransient(object, false);
      m_ObjectToPrim.Remove(object);
      m_Prims.Remove(sPath);
    }

    if (!removed.IsEmpty())
    {
      ForgetUndoHistory("prims were removed from the stage");
      bContentChanged = true;
    }
  }

  const plRTTI* pGameObjectType = plGetStaticRTTI<plGameObject>();
  plHybridArray<plUuid, 16> changedRows;

  for (const plUsdPrim& prim : prims)
  {
    const plUuid parentGuid = prim.m_iParent >= 0 ? GetObjectGuid(prims[prim.m_iParent].m_sPath) : m_RootObject;
    const plDocumentObject* pParent = accessor.GetObject(parentGuid);
    if (pParent == nullptr)
      continue;

    PrimState* pState = nullptr;
    const bool bNew = !m_Prims.TryGetValue(prim.m_sPath, pState);

    plUuid objectGuid = GetObjectGuid(prim.m_sPath);
    const plDocumentObject* pObject = accessor.GetObject(objectGuid);

    if (pObject == nullptr)
    {
      if (accessor.AddObjectByName(pParent, "Children", -1, pGameObjectType, objectGuid).Failed())
        continue;

      m_pDocument->GetObjectManager()->SetObjectTransient(objectGuid, true);
      pObject = accessor.GetObject(objectGuid);

      SetProperty(pObject, "Name", prim.m_sName);
      SetObjectTransform(pObject, prim.m_LocalTransform);

      // A deactivated prim keeps its object, hidden, so it can be reactivated from the scene tree. Prototypes are drawn
      // by their instancer only.
      SetProperty(pObject, "Active", prim.m_bActive && prim.m_bVisible && !prim.m_bPrototype);

      AddTag(pObject, LiveTag);
      AddTag(pObject, ExcludeFromExportTag);
    }

    plUsdPrimOpinions opinions;
    m_Stage.QueryPrimOpinions(prim.m_sPath, opinions);

    if (bNew)
    {
      pState = &m_Prims[prim.m_sPath];
      pState->m_Object = objectGuid;
      pState->m_Transform = prim.m_LocalTransform;
      pState->m_bVisible = prim.m_bVisible;
      pState->m_bActive = prim.m_bActive;
      pState->m_bPrototype = prim.m_bPrototype;
      m_ObjectToPrim[objectGuid] = prim.m_sPath;
      changedRows.PushBack(objectGuid);
      bContentChanged = true;
    }
    else
    {
      // Only values that changed in USD are pushed, so a local edit in a read-only link stays until USD disagrees.
      if (!pState->m_Transform.IsEqual(prim.m_LocalTransform, 1e-5f))
      {
        SetObjectTransform(pObject, prim.m_LocalTransform);
        pState->m_Transform = prim.m_LocalTransform;
        bContentChanged = true;
      }

      if (pState->m_bVisible != prim.m_bVisible || pState->m_bActive != prim.m_bActive || pState->m_bPrototype != prim.m_bPrototype)
      {
        bContentChanged = true;
        SetProperty(pObject, "Active", prim.m_bActive && prim.m_bVisible && !prim.m_bPrototype);

        if (pState->m_bActive != prim.m_bActive || pState->m_bPrototype != prim.m_bPrototype)
          changedRows.PushBack(objectGuid);

        pState->m_bVisible = prim.m_bVisible;
        pState->m_bActive = prim.m_bActive;
        pState->m_bPrototype = prim.m_bPrototype;
      }

      if (!(pState->m_Opinions == opinions))
        changedRows.PushBack(objectGuid);
    }

    const bool bKindChanged = pState->m_Kind != prim.m_Kind;
    pState->m_Kind = prim.m_Kind;
    pState->m_bTransformAnimated = prim.m_bTransformAnimated;
    pState->m_sTypeName = prim.m_sTypeName;
    pState->m_Opinions = opinions;

    // Lights and cameras get the matching Plasma component. An inactive prim's values are not composed, so its
    // component stays as it was, hidden with the object.
    if (prim.m_bActive && (plUsdPrimComponents::GetComponentType(prim) != nullptr || pState->m_bHasComponent))
    {
      if (bNew || bKindChanged || !pState->m_bHasComponent || !(pState->m_Light == prim.m_Light) || !(pState->m_Camera == prim.m_Camera))
      {
        plUsdPrimComponents::Apply(m_pDocument->GetObjectManager(), pObject, prim, GetPrimComponentGuid(prim.m_sPath));
        pState->m_Light = prim.m_Light;
        pState->m_Camera = prim.m_Camera;
        pState->m_bHasComponent = plUsdPrimComponents::GetComponentType(prim) != nullptr;
        bContentChanged = true;
      }
    }

    // Like the other components, an inactive instancer's stay as they were, hidden with the object.
    if (prim.m_bActive && (prim.m_Kind == plUsdPrimKind::Instancer || !pState->m_InstanceComponents.IsEmpty()) &&
        (bNew || pState->m_Instancer.m_uiHash != prim.m_Instancer.m_uiHash))
    {
      pState->m_Instancer = prim.m_Instancer;
      m_DirtyInstancers.Insert(prim.m_sPath);
      m_bInstancersByMeshDirty = true;
      bContentChanged = true;
    }

    // Plasma components the stage carries; see plUsdComponent. Only a change on the stage is pushed, so a component the
    // user just added in a read-only link stays until the scene closes.
    if (prim.m_bActive && !(pState->m_Components == prim.m_Components))
    {
      plHybridArray<plUuid, 8> generated;
      GetGeneratedComponents(prim.m_sPath, *pState, generated);

      plUsdPrimComponents::ApplyUserComponents(m_pDocument->GetObjectManager(), pObject, prim.m_Components, pState->m_Components, generated,
        plStringBuilder("usd:", m_sLinkId, prim.m_sPath));
      pState->m_Components = prim.m_Components;
      bContentChanged = true;
    }

    const bool bSurfaceChanged = !(pState->m_MaterialGroups == prim.m_MaterialGroups);
    pState->m_MaterialGroups = prim.m_MaterialGroups;
    bContentChanged |= bSurfaceChanged;

    // The property grid shows the prim's material values.
    if (bSurfaceChanged && !bNew)
      changedRows.PushBack(objectGuid);

    // An inactive prim's properties are not composed, so there is no geometry to build; its meshes stay hidden with it.
    // Geometry is never read here: the key only names the specs it comes from. Unchanged prims are skipped entirely.
    if (prim.m_bActive && (prim.m_Kind == plUsdPrimKind::Mesh || prim.m_Kind == plUsdPrimKind::Gprim))
    {
      if (bSurfaceChanged)
        RequestTextures(*pState);

      const bool bEdited = geometryChanges.m_Edited.Contains(prim.m_sPath);

      if (bNew || bSurfaceChanged || bEdited || pState->m_uiMeshKey == 0 || pState->m_uiAppliedMeshKey != pState->m_uiMeshKey ||
          isResynced(prim.m_sPath))
      {
        plUsdGeometryKey geometry;
        if (m_Stage.ComputeGeometryKey(prim.m_sPath, geometry).Succeeded())
        {
          const plUInt64 uiPreviousKey = pState->m_uiAppliedMeshKey;
          RequireMesh(prim.m_sPath, *pState, geometry, bEdited);

          if (bSurfaceChanged && uiPreviousKey != 0 && uiPreviousKey == pState->m_uiAppliedMeshKey && uiPreviousKey == pState->m_uiMeshKey)
          {
            // Same mesh, new values: only the materials and the component colour change.
            SendMaterials(prim.m_sPath, *pState);

            const plColor tint = GetTint(*pState);
            for (plUInt32 i = 0; i < pState->m_uiMeshParts; ++i)
            {
              if (const plDocumentObject* pComponent = accessor.GetObject(GetMeshComponentGuid(prim.m_sPath, i)))
              {
                SetProperty(pComponent, "Color", tint);
              }
            }

            MarkInstancersUsing(prim.m_sPath);
          }
        }
        else
        {
          plLog::Warning("USD Live: '{}' has no geometry the engine can use", prim.m_sPath);
        }
      }
    }
  }

  m_bApplyingRemote = false;

  FlushInstancers();

  if (bContentChanged)
    MarkBakeDirty();

  // The scene tree shows each prim's state; tell it which rows to repaint. After a reopen, the root row and every
  // prim row may look different, e.g. once a session started.
  if (m_bRefreshAllRows)
  {
    m_bRefreshAllRows = false;
    plObjectRowDecoratorRegistry::RequestRefresh(m_pDocument);
    plUsdLiveManager::GetSingleton()->BroadcastPrimStatesChanged(this);
  }
  else if (!changedRows.IsEmpty())
  {
    plObjectRowDecoratorRegistry::RequestRefresh(m_pDocument, changedRows);
    plUsdLiveManager::GetSingleton()->BroadcastPrimStatesChanged(this);
  }
}

void plUsdLiveLink::ForgetUndoHistory(plStringView sReason)
{
  // Undo steps may still point at the objects just removed; replaying one would write into a detached object.
  plCommandHistory* pHistory = m_pDocument->GetCommandHistory();
  if (pHistory->GetUndoStackSize() == 0 && pHistory->GetRedoStackSize() == 0)
    return;

  pHistory->ClearUndoHistory();
  pHistory->ClearRedoHistory();
  plLog::Info("USD Live: cleared the undo history of '{}' because {}.", m_pDocument->GetDocumentPath(), sReason);
}

void plUsdLiveLink::RequireMesh(plStringView sPrimPath, PrimState& ref_state, const plUsdGeometryKey& geometry, bool bRebuild)
{
  const plUInt64 sourceKey = plHashingUtils::xxHash64(&geometry.m_uiFileStamp, sizeof(geometry.m_uiFileStamp), geometry.m_uiIdentity);
  if (bRebuild)
  {
    m_SourceContents.Remove(sourceKey);
  }

  const plUInt64 uiMeshKey = plUsdBakeStore::GetMeshKey(m_sLinkId, geometry, ref_state.m_MaterialGroups);
  if (ref_state.m_uiMeshKey != uiMeshKey)
  {
    if (auto* users = m_MeshUsers.GetValue(ref_state.m_uiMeshKey))
    {
      users->RemoveAndSwap(plString(sPrimPath));
    }
  }

  auto& users = m_MeshUsers[uiMeshKey];
  if (!users.Contains(sPrimPath))
  {
    users.PushBack(sPrimPath);
  }
  ref_state.m_uiMeshKey = uiMeshKey;

  Mesh* pMesh = nullptr;
  if (!m_Meshes.TryGetValue(uiMeshKey, pMesh))
  {
    pMesh = &m_Meshes[uiMeshKey];
    pMesh->m_sName = plUsdBakeStore::GetMeshName(m_sLinkId, geometry, uiMeshKey);
    pMesh->m_sCacheName = pMesh->m_sName;

    // Built in an earlier session from the same layer files: the files are current.
    const plUsdBakeStore::MeshEntry* pEntry = m_Manifest.m_Meshes.GetValue(pMesh->m_sName);
    const plString resourceMesh = pEntry != nullptr && !pEntry->m_sResourceMesh.IsEmpty() ? pEntry->m_sResourceMesh : pMesh->m_sName;
    if (!bRebuild && pEntry != nullptr && pEntry->m_uiFileStamp == geometry.m_uiFileStamp && plUsdBakeStore::HasMeshFiles(resourceMesh, pEntry->m_uiParts))
    {
      pMesh->m_sName = resourceMesh;
      pMesh->m_uiParts = pEntry->m_uiParts;
      pMesh->m_bComplete = true;
    }
    else
    {
      if (!bRebuild && pEntry != nullptr && pEntry->m_uiFileStamp == geometry.m_uiFileStamp &&
          pEntry->m_sPreviewResource == plUsdMeshCache::GetPreviewResource(resourceMesh) && plOSFile::ExistsFile(plUsdBakeStore::ToAbsolutePath(pEntry->m_sPreviewResource)))
      {
        pMesh->m_sName = resourceMesh;
        pMesh->m_sPreviewResource = pEntry->m_sPreviewResource;
        pMesh->m_uiParts = 1;
        pMesh->m_bPreview = true;
      }
      bRebuild = true;
    }
  }

  pMesh->m_uiFileStamp = geometry.m_uiFileStamp;
  pMesh->m_sSourcePrim = sPrimPath;
  pMesh->m_uiSourceKey = sourceKey;

  // Until the new build is in, the prim shows the mesh's current files.
  if (pMesh->m_uiParts > 0 && ref_state.m_uiAppliedMeshKey != uiMeshKey)
    ApplyMeshUsers(uiMeshKey, pMesh->m_uiParts);

  if (bRebuild)
    QueueMesh(uiMeshKey);
}

void plUsdLiveLink::QueueMesh(plUInt64 uiMeshKey)
{
  Mesh* pMesh = nullptr;
  if (!m_Meshes.TryGetValue(uiMeshKey, pMesh))
    return;

  ++pMesh->m_uiGeneration;
  pMesh->m_bComplete = false;
  pMesh->m_sBuildName.Clear();
  pMesh->m_uiContentKey = 0;
  pMesh->m_uiEstimatedPoints = plMath::MaxValue<plUInt64>();

  for (const auto& job : m_Jobs)
  {
    if (job->m_uiMeshKey == uiMeshKey)
    {
      job->m_Cancelled = true;
    }
  }

  if (!pMesh->m_bQueued)
  {
    pMesh->m_bQueued = true;
    pMesh->m_bPreviewPending = pMesh->m_uiParts == 0;
    (pMesh->m_bPreviewPending ? m_MeshQueue : m_RefineQueue).PushBack(uiMeshKey);
  }
}

void plUsdLiveLink::LaunchMeshJobs()
{
  PL_PROFILE_SCOPE("USD admit meshes");
  PrioritizeMeshes();
  constexpr plUInt64 MaxPointsInFlight = 4'000'000;
  const plTime captureDeadline = plTime::Now() + plTime::MakeFromMilliseconds(8);
  plUInt64 uiPointsInFlight = 0;
  for (const auto& job : m_Jobs)
  {
    uiPointsInFlight += job->m_uiPointCount;
  }

  // Preview work has its own queue and gets the next available slot before refinement.
  const plUInt32 maxJobs = GetMaxConcurrentMeshJobs();
  for (plUInt32 checked = m_MeshQueue.GetCount() + m_RefineQueue.GetCount(); checked > 0 && m_Jobs.GetCount() < maxJobs; --checked)
  {
    if (plTime::Now() >= captureDeadline)
    {
      break;
    }

    const bool previewQueue = !m_MeshQueue.IsEmpty();
    auto& queue = previewQueue ? m_MeshQueue : m_RefineQueue;
    if (queue.IsEmpty())
    {
      break;
    }

    const plUInt64 uiMeshKey = queue.PeekFront();
    queue.PopFront();

    Mesh* pMesh = nullptr;
    if (!m_Meshes.TryGetValue(uiMeshKey, pMesh) || !pMesh->m_bQueued || pMesh->m_bComplete || pMesh->m_bPreviewPending != previewQueue)
    {
      continue;
    }

    bool bRunning = false;
    for (const plUniquePtr<MeshJob>& pJob : m_Jobs)
      bRunning |= pJob->m_uiMeshKey == uiMeshKey;

    if (bRunning)
    {
      queue.PushBack(uiMeshKey);
      continue;
    }

    const PrimState* pSourceState = m_Prims.GetValue(pMesh->m_sSourcePrim);
    if (pSourceState == nullptr || pSourceState->m_uiMeshKey != uiMeshKey)
    {
      pSourceState = nullptr;
      if (const auto* users = m_MeshUsers.GetValue(uiMeshKey))
      {
        for (const auto& path : *users)
        {
          const auto* state = m_Prims.GetValue(path);
          if (state != nullptr && state->m_uiMeshKey == uiMeshKey)
          {
            pMesh->m_sSourcePrim = path;
            pSourceState = state;
            break;
          }
        }
      }
    }

    if (pSourceState == nullptr)
    {
      pMesh->m_bQueued = false;
      continue;
    }

    if (pMesh->m_sBuildName.IsEmpty())
    {
      if (const auto* known = m_SourceContents.GetValue(pMesh->m_uiSourceKey))
      {
        pMesh->m_sBuildName = known->m_sMesh;
        pMesh->m_uiContentKey = known->m_uiKey;
        pMesh->m_uiEstimatedPoints = known->m_uiPoints;
      }
    }

    // Published files are immutable and the completion marker is replaced atomically. Reusing them
    // needs no build slot, even while another placement is refining the same geometry.
    if (!pMesh->m_sBuildName.IsEmpty())
    {
      const auto cached = plUsdMeshCache::Read(pMesh->m_sBuildName);
      if (cached.m_uiParts > 0 || (previewQueue && !cached.m_sPreviewResource.IsEmpty()))
      {
        pMesh->m_bQueued = false;
        AdoptCachedMesh(uiMeshKey, pMesh->m_sBuildName, cached);
        continue;
      }
    }

    // A blocked job retains its cheap estimate and content identity, never a large captured source.
    if (pMesh->m_uiEstimatedPoints == plMath::MaxValue<plUInt64>())
    {
      PL_PROFILE_SCOPE("USD preflight points");
      pMesh->m_uiEstimatedPoints = m_Stage.GetGeometryPointCount(pMesh->m_sSourcePrim);
    }

    const plUInt64 uiPoints = pMesh->m_uiEstimatedPoints;
    if ((!m_Jobs.IsEmpty() && uiPointsInFlight + uiPoints > MaxPointsInFlight) || !plUsdMeshCache::CanAdmit(uiPoints))
    {
      queue.PushBack(uiMeshKey);
      continue;
    }

    const plTime captureStart = plTime::Now();
    plUniquePtr<plUsdGeometrySource> source;
    plStatus res;
    plUInt64 contentKey = pMesh->m_uiContentKey;
    if (pMesh->m_sBuildName.IsEmpty())
    {
      PL_PROFILE_SCOPE("USD capture and hash");
      source = m_Stage.CaptureGeometry(pMesh->m_sSourcePrim, res);
      if (source == nullptr)
      {
        pMesh->m_bQueued = false;
        plLog::Warning("USD Live: '{}': {}", pMesh->m_sSourcePrim, res.m_sMessage);
        continue;
      }

      contentKey = plUsdMeshCache::GetKey(source->GetHash(), pSourceState->m_MaterialGroups.GetCount());
      pMesh->m_sBuildName = plUsdMeshCache::GetMeshName(contentKey);
      pMesh->m_uiContentKey = contentKey;
      m_SourceContents[pMesh->m_uiSourceKey] = {pMesh->m_sBuildName, contentKey, uiPoints};
    }

    // Different source prims and material bindings can resolve to the same immutable geometry.
    // Acquire before checking the marker: a worker may currently be publishing it.
    if (!plUsdMeshCache::TryAcquire(pMesh->m_sBuildName, uiPoints))
    {
      queue.PushBack(uiMeshKey);
      continue;
    }

    const auto cached = plUsdMeshCache::Read(pMesh->m_sBuildName);
    if (cached.m_uiParts > 0 || (previewQueue && !cached.m_sPreviewResource.IsEmpty()))
    {
      plUsdMeshCache::Release(pMesh->m_sBuildName);
      pMesh->m_bQueued = false;
      AdoptCachedMesh(uiMeshKey, pMesh->m_sBuildName, cached);
      continue;
    }

    if (source == nullptr)
    {
      PL_PROFILE_SCOPE("USD capture refinement");
      source = m_Stage.CaptureGeometry(pMesh->m_sSourcePrim, res);
    }

    if (source == nullptr)
    {
      plUsdMeshCache::Release(pMesh->m_sBuildName);
      pMesh->m_bQueued = false;
      plLog::Warning("USD Live: '{}': {}", pMesh->m_sSourcePrim, res.m_sMessage);
      continue;
    }

    if (contentKey == 0)
    {
      contentKey = plUsdMeshCache::GetKey(source->GetHash(), pSourceState->m_MaterialGroups.GetCount());
    }

    plUniquePtr<MeshJob> pJob = PL_DEFAULT_NEW(MeshJob);
    pJob->m_bPreviewOnly = previewQueue;
    pJob->m_uiContentKey = contentKey;
    pJob->m_CaptureTime = plTime::Now() - captureStart;
    pJob->m_Started = plTime::Now();
    pJob->m_sPreviewResource = plUsdMeshCache::GetPreviewResource(pMesh->m_sBuildName);
    pJob->m_uiPointCount = uiPoints;
    pJob->m_uiMeshKey = uiMeshKey;
    pJob->m_uiGeneration = pMesh->m_uiGeneration;
    pJob->m_sMesh = pMesh->m_sBuildName;
    pJob->m_uiFileStamp = pMesh->m_uiFileStamp;
    pJob->m_bTangents = true;
    for (const plUsdMaterialGroup& group : pSourceState->m_MaterialGroups)
      pJob->m_MaterialResources.PushBack(plUsdLiveResources::DefaultMaterial);
    pJob->m_pSource = std::move(source);
    pMesh->m_bQueued = false;

    // The files are written by the task too, so the main thread never waits for a large write.
    MeshJob* pRaw = pJob.Borrow();
    plSharedPtr<plDelegateTask<void>> pTask = PL_DEFAULT_NEW(plDelegateTask<void>, previewQueue ? "USD preview" : "USD refinement", plTaskNesting::Never, [pRaw]()
      {
        MeshJob& job = *pRaw;
        if (job.m_Cancelled)
        {
          return;
        }

        if (job.m_bPreviewOnly)
        {
          plGeometry geometry;
          plDynamicArray<plUInt32> groups;
          {
            PL_PROFILE_SCOPE("USD preview tessellation");
            job.m_Result = job.m_pSource->Build(geometry, &groups);
          }
          job.m_pSource.Clear();

          if (job.m_Result.Failed() || job.m_Cancelled)
          {
            return;
          }

          plDynamicArray<plUInt8> preview;
          job.m_Result = plUsdLiveResources::BuildMeshPreview(geometry, groups, job.m_MaterialResources, job.m_uiContentKey, preview);
          if (job.m_Result.Failed() || job.m_Cancelled)
          {
            return;
          }

          job.m_Result = plStatus(plUsdBakeStore::WriteFile(job.m_sPreviewResource, preview));
          if (job.m_Result.Succeeded())
          {
            job.m_Result = plStatus(plUsdMeshCache::Publish(job.m_sMesh, 0, true));
          }
          return;
        }

        auto publish = [&job](plUInt32 i, plArrayPtr<const plUInt8> bytes) -> plStatus
        {
          if (job.m_Cancelled)
          {
            return plStatus("Superseded USD geometry.");
          }

          PL_PROFILE_SCOPE("USD write mesh part");
          const plString sResource = plUsdBakeStore::GetMeshResource(job.m_sMesh, i);
          if (plUsdBakeStore::WriteFile(sResource, bytes).Failed())
          {
            return plStatus(plFmt("could not write '{}'", sResource));
          }

          return plStatus(PL_SUCCESS);
        };

        job.m_Result = plUsdLiveResources::BuildMeshParts(*job.m_pSource, job.m_MaterialResources, job.m_uiContentKey, job.m_bTangents, job.m_Parts, publish,
          {}, [&job]() { return (bool)job.m_Cancelled; });
        job.m_pSource.Clear();

        if (job.m_Result.Failed())
        {
          return;
        }

        job.m_uiPartCount = job.m_Parts.GetCount();
        job.m_Parts.Clear();
        job.m_Parts.Compact();
        if (!job.m_Cancelled)
        {
          job.m_Result = plStatus(plUsdMeshCache::Publish(job.m_sMesh, job.m_uiPartCount, true));
        }
      });

    pJob->m_TaskGroup = plTaskSystem::StartSingleTask(pTask, plTaskPriority::LongRunning);
    m_Jobs.PushBack(std::move(pJob));
    uiPointsInFlight += uiPoints;
  }
}

void plUsdLiveLink::PrioritizeMeshes()
{
  if (plTime::Now() < m_NextPriorityUpdate || (m_MeshQueue.IsEmpty() && m_RefineQueue.IsEmpty()))
  {
    return;
  }

  m_NextPriorityUpdate = plTime::Now() + plTime::MakeFromMilliseconds(500);
  const auto* view = plEngineViewWidget::GetInteractionContext().m_pLastHoveredViewWidget;
  auto* document = plDynamicCast<plGameObjectDocument*>(m_pDocument);
  if (view == nullptr || document == nullptr || view->GetDocumentWindow()->GetDocument() != m_pDocument)
  {
    return;
  }

  PL_PROFILE_SCOPE("USD prioritize visible geometry");
  const auto& camera = view->m_pViewConfig->m_Camera;
  const plVec3 cameraPosition = camera.GetCenterPosition();
  const plVec3 cameraForward = camera.GetCenterDirForwards();
  plHashTable<plUInt64, float> scores;
  for (auto it = m_MeshUsers.GetIterator(); it.IsValid(); ++it)
  {
    float score = plMath::MaxValue<float>();
    for (const auto& path : it.Value())
    {
      const auto* state = m_Prims.GetValue(path);
      if (state == nullptr || state->m_uiMeshKey != it.Key())
      {
        continue;
      }

      const auto* object = m_pDocument->GetObjectManager()->GetObject(state->m_Object);
      if (object == nullptr)
      {
        continue;
      }

      const plVec3 delta = document->GetGlobalTransform(object).m_vPosition - cameraPosition;
      float candidate = delta.GetLengthSquared();
      if (delta.Dot(cameraForward) < 0)
      {
        candidate = candidate * 4.0f + 100.0f;
      }
      if (!state->m_bVisible || !state->m_bActive || state->m_bPrototype)
      {
        candidate += 1.0e12f;
      }
      score = plMath::Min(score, candidate);
    }
    scores.Insert(it.Key(), score);
  }

  auto sort = [&](plDeque<plUInt64>& queue)
  {
    plDynamicArray<plUInt64> keys;
    plSet<plUInt64> seen;
    for (plUInt64 key : queue)
    {
      if (!seen.Contains(key))
      {
        seen.Insert(key);
        keys.PushBack(key);
      }
    }
    keys.Sort([&](plUInt64 a, plUInt64 b)
      {
        const float sa = scores.Contains(a) ? scores[a] : plMath::MaxValue<float>();
        const float sb = scores.Contains(b) ? scores[b] : plMath::MaxValue<float>();
        return sa == sb ? a < b : sa < sb; });
    queue.Clear();
    for (plUInt64 key : keys)
    {
      queue.PushBack(key);
    }
  };
  sort(m_MeshQueue);
  sort(m_RefineQueue);
}

void plUsdLiveLink::FinishMeshJobs(bool bWait)
{
  for (plUInt32 i = 0; i < m_Jobs.GetCount();)
  {
    MeshJob& job = *m_Jobs[i];

    if (bWait)
    {
      // Closing: the objects go, but the files are written and current.
      plTaskSystem::WaitForGroup(job.m_TaskGroup);
      RecordMeshJob(job);
    }
    else if (!plTaskSystem::IsTaskGroupFinished(job.m_TaskGroup))
    {
      ++i;
      continue;
    }
    else
    {
      ApplyMeshJob(job);
    }

    plUsdMeshCache::Release(job.m_sMesh);
    m_Jobs.RemoveAtAndSwap(i);
  }
}

bool plUsdLiveLink::RecordMeshJob(const MeshJob& job)
{
  if (job.m_Cancelled)
  {
    return false;
  }

  if (job.m_Result.Failed())
  {
    plLog::Warning("USD Live: mesh '{}' could not be built: {}", job.m_sMesh, job.m_Result.m_sMessage);
    return false;
  }

  const auto entry = plUsdMeshCache::Read(job.m_sMesh);
  plUInt32 users = 0;
  for (auto it = m_Meshes.GetIterator(); it.IsValid(); ++it)
  {
    if (it.Value().m_sBuildName == job.m_sMesh)
    {
      AdoptCachedMesh(it.Key(), job.m_sMesh, entry);
      ++users;
    }
  }

  plLog::Info("USD Live: {} ready in {} s (capture {} ms, {} points, {} bindings): '{}'",
    job.m_bPreviewOnly ? "preview" : "refinement", (plTime::Now() - job.m_Started).GetSeconds(), job.m_CaptureTime.GetMilliseconds(),
    job.m_uiPointCount, users, job.m_sMesh);
  return true;
}

void plUsdLiveLink::AdoptCachedMesh(plUInt64 uiMeshKey, plStringView sMesh, const plUsdBakeStore::MeshEntry& entry)
{
  Mesh* mesh = m_Meshes.GetValue(uiMeshKey);
  if (mesh == nullptr || (entry.m_uiParts == 0 && entry.m_sPreviewResource.IsEmpty()))
  {
    return;
  }

  mesh->m_sName = sMesh;
  mesh->m_sPreviewResource = entry.m_sPreviewResource;
  mesh->m_bComplete = entry.m_uiParts > 0;
  mesh->m_bPreview = !mesh->m_bComplete;
  mesh->m_uiParts = mesh->m_bComplete ? entry.m_uiParts : 1;
  mesh->m_bQueued = false;
  mesh->m_bPreviewPending = false;

  auto saved = entry;
  saved.m_uiFileStamp = mesh->m_uiFileStamp;
  saved.m_sResourceMesh = sMesh;
  m_Manifest.m_Meshes[mesh->m_sCacheName] = saved;
  m_bManifestDirty = true;

  if (!mesh->m_bComplete)
  {
    mesh->m_bQueued = true;
    m_RefineQueue.PushBack(uiMeshKey);
  }

  ApplyMeshUsers(uiMeshKey, mesh->m_uiParts, true);
}

void plUsdLiveLink::CheckpointMeshCache()
{
  if (!m_bManifestLoaded || !m_bManifestDirty)
  {
    return;
  }

  if (plUsdBakeStore::SaveManifest(m_sLinkId, m_Manifest).Succeeded())
  {
    m_bManifestDirty = false;
  }
  else
  {
    plLog::Warning("USD Live: could not checkpoint completed meshes for '{}'.", m_sOpenedFile);
  }

  m_NextManifestSave = plTime::Now() + plTime::MakeFromSeconds(2);
}

void plUsdLiveLink::ApplyMeshJob(MeshJob& job)
{
  RecordMeshJob(job);
}

void plUsdLiveLink::ApplyMeshUsers(plUInt64 uiMeshKey, plUInt32 uiParts, bool bForce)
{
  const auto* users = m_MeshUsers.GetValue(uiMeshKey);
  if (users == nullptr)
  {
    return;
  }

  for (const auto& path : *users)
  {
    const auto* state = m_Prims.GetValue(path);
    if (state != nullptr && state->m_uiMeshKey == uiMeshKey &&
        (bForce || state->m_uiAppliedMeshKey != uiMeshKey || state->m_uiMeshParts != uiParts) && !m_PendingApplications.Contains(path))
    {
      m_PendingApplications.Insert(path);
      m_ApplyQueue.PushBack(path);
    }
  }

  MarkBakeDirty();
}

void plUsdLiveLink::FlushMeshApplications()
{
  if (!IsEngineReady() || m_pDocument->GetCommandHistory()->IsInTransaction() || m_pDocument->GetCommandHistory()->IsInUndoRedo())
  {
    return;
  }

  PL_PROFILE_SCOPE("USD apply mesh objects");
  const plTime deadline = plTime::Now() + plTime::MakeFromMilliseconds(4);
  while (!m_ApplyQueue.IsEmpty() && plTime::Now() < deadline)
  {
    const plString path = m_ApplyQueue.PeekFront();
    m_ApplyQueue.PopFront();
    m_PendingApplications.Remove(path);
    if (auto* state = m_Prims.GetValue(path))
    {
      ApplyMesh(path, *state);
    }
  }
}

void plUsdLiveLink::ApplyMesh(plStringView sPrimPath, PrimState& ref_state)
{
  const Mesh* pMesh = m_Meshes.GetValue(ref_state.m_uiMeshKey);
  if (pMesh == nullptr || pMesh->m_uiParts == 0)
    return;

  // Existing geometry stays visible while an edited mesh is refined. New placements can use its preview.
  if (pMesh->m_bPreview && ref_state.m_uiAppliedMeshKey != 0 && ref_state.m_uiAppliedMeshKey != ref_state.m_uiMeshKey)
  {
    return;
  }

  plObjectDirectAccessor accessor(m_pDocument->GetObjectManager());
  const plDocumentObject* pObject = accessor.GetObject(ref_state.m_Object);
  if (pObject == nullptr)
    return;

  const plRTTI* pMeshType = plGetStaticRTTI<plMeshComponent>();
  const plUInt32 uiParts = pMesh->m_uiParts;
  const plColor color = GetTint(ref_state);

  // Also called from Sync, which already has this set.
  const bool bWasApplyingRemote = m_bApplyingRemote;
  m_bApplyingRemote = true;

  // Geometry casts shadows unless someone turns it off, as it would in the tool it came from.
  AddTag(pObject, "CastShadow");

  // One mesh component per part. Only values that differ are set: each set is an event and a message to the engine.
  for (plUInt32 i = 0; i < uiParts; ++i)
  {
    plUuid componentGuid = GetMeshComponentGuid(sPrimPath, i);
    const plDocumentObject* pComponent = accessor.GetObject(componentGuid);

    if (pComponent == nullptr)
    {
      if (accessor.AddObjectByName(pObject, "Components", -1, pMeshType, componentGuid).Failed())
        continue;

      pComponent = accessor.GetObject(componentGuid);
    }

    const plString sResource = pMesh->m_bPreview ? pMesh->m_sPreviewResource : plUsdBakeStore::GetMeshResource(pMesh->m_sName, i);
    if (pComponent->GetTypeAccessor().GetValue("Mesh").ConvertTo<plString>() != sResource)
      SetProperty(pComponent, "Mesh", sResource);

    if (pComponent->GetTypeAccessor().GetValue("Color").ConvertTo<plColor>() != color)
      SetProperty(pComponent, "Color", color);

    plUsdPrimComponents::ApplyMaterials(m_pDocument->GetObjectManager(), pComponent, m_sLinkId, ref_state.m_MaterialGroups);
  }

  for (plUInt32 i = uiParts; i < ref_state.m_uiMeshParts; ++i)
  {
    if (const plDocumentObject* pComponent = accessor.GetObject(GetMeshComponentGuid(sPrimPath, i)))
    {
      plUsdPrimComponents::RemoveObject(m_pDocument->GetObjectManager(), pComponent).IgnoreResult();
    }
  }

  m_bApplyingRemote = bWasApplyingRemote;

  ref_state.m_uiMeshParts = uiParts;
  ref_state.m_uiAppliedMeshKey = ref_state.m_uiMeshKey;
  if (!pMesh->m_bPreview)
  {
    m_Manifest.m_Prims[sPrimPath] = {pMesh->m_sName, uiParts};
  }
  MarkBakeDirty();
  MarkInstancersUsing(sPrimPath);

  // The mesh names the materials; the engine asks for them once it loads the mesh.
  SendMaterials(sPrimPath, ref_state);
}

void plUsdLiveLink::RequestTextures(const PrimState& state) const
{
  plUsdLiveTextures* pTextures = plUsdLiveManager::GetSingleton()->GetTextures();

  for (const plUsdMaterialGroup& group : state.m_MaterialGroups)
  {
    if (group.m_bHasSurface && !group.m_Surface.m_sMaterialPath.IsEmpty())
    {
      plUsdSurface ignored = group.m_Surface;
      pTextures->Resolve(ignored);
    }
  }
}

void plUsdLiveLink::MarkInstancersUsing(plStringView sMeshPath)
{
  // Called for every mesh prim that gets its mesh, so the instancers are looked up rather than searched.
  if (m_bInstancersByMeshDirty)
  {
    m_bInstancersByMeshDirty = false;
    m_InstancersByMesh.Clear();

    for (auto it = m_Prims.GetIterator(); it.IsValid(); ++it)
    {
      for (const plUsdInstancedMesh& mesh : it.Value().m_Instancer.m_Meshes)
      {
        plHybridArray<plString, 1>& instancers = m_InstancersByMesh[mesh.m_sMeshPath];
        if (!instancers.Contains(it.Key()))
          instancers.PushBack(it.Key());
      }
    }
  }

  if (const plHybridArray<plString, 1>* pInstancers = m_InstancersByMesh.GetValue(sMeshPath))
  {
    for (const plString& sInstancer : *pInstancers)
      m_DirtyInstancers.Insert(sInstancer);
  }
}

void plUsdLiveLink::FlushInstancers()
{
  if (m_DirtyInstancers.IsEmpty() || !IsEngineReady())
    return;

  // Like Sync, never inside a command the user has open.
  if (m_pDocument->GetCommandHistory()->IsInTransaction() || m_pDocument->GetCommandHistory()->IsInUndoRedo())
    return;

  plHybridArray<plString, 8> dirty;
  for (const plString& sPath : m_DirtyInstancers)
    dirty.PushBack(sPath);
  m_DirtyInstancers.Clear();

  m_bApplyingRemote = true;
  for (const plString& sPath : dirty)
    ApplyInstancer(sPath);
  m_bApplyingRemote = false;

  MarkBakeDirty();
}

void plUsdLiveLink::ApplyInstancer(plStringView sPrimPath)
{
  PrimState* pState = nullptr;
  if (!m_Prims.TryGetValue(sPrimPath, pState))
    return;

  plObjectDirectAccessor accessor(m_pDocument->GetObjectManager());
  const plDocumentObject* pObject = accessor.GetObject(pState->m_Object);
  if (pObject == nullptr)
    return;

  const plRTTI* pInstancedType = plGetStaticRTTI<plInstancedMeshComponent>();
  plHybridArray<plUuid, 2> components;
  plUInt32 uiBudget = plUsdPrimComponents::MaxInstances;
  plUInt32 uiLeftOut = 0;

  for (const plUsdInstancedMesh& mesh : pState->m_Instancer.m_Meshes)
  {
    // Prototype meshes are built like any other mesh, for their own hidden objects; until then there is nothing to draw.
    const PrimState* pMesh = nullptr;
    if (!m_Prims.TryGetValue(mesh.m_sMeshPath, pMesh))
      continue;

    for (plUInt32 uiPart = 0; uiPart < pMesh->m_uiMeshParts; ++uiPart)
    {
      const plUInt32 uiCount = plMath::Min(mesh.m_Instances.GetCount(), uiBudget);
      uiLeftOut += mesh.m_Instances.GetCount() - uiCount;
      uiBudget -= uiCount;

      if (uiCount == 0)
        continue;

      const plString sSeed = GetInstanceGuidSeed(sPrimPath, mesh.m_sMeshPath, uiPart);
      plUuid componentGuid = plUuid::MakeStableUuidFromString(sSeed);

      if (accessor.GetObject(componentGuid) == nullptr && accessor.AddObjectByName(pObject, "Components", -1, pInstancedType, componentGuid).Failed())
        continue;

      plUsdPrimComponents::ApplyInstances(m_pDocument->GetObjectManager(), accessor.GetObject(componentGuid), GetMeshResource(*pMesh, uiPart),
        GetTint(*pMesh), mesh.m_Instances.GetArrayPtr().GetSubArray(0, uiCount), sSeed);
      plUsdPrimComponents::ApplyMaterials(m_pDocument->GetObjectManager(), accessor.GetObject(componentGuid), m_sLinkId, pMesh->m_MaterialGroups);
      components.PushBack(componentGuid);
    }
  }

  for (const plUuid& guid : pState->m_InstanceComponents)
  {
    if (components.Contains(guid))
      continue;

    if (const plDocumentObject* pComponent = accessor.GetObject(guid))
      plUsdPrimComponents::RemoveObject(m_pDocument->GetObjectManager(), pComponent).IgnoreResult();
  }

  if (!components.IsEmpty())
    AddTag(pObject, "CastShadow");

  if (uiLeftOut > 0)
    plLog::Warning("USD Live: '{}' has more instances than the {} an instancer shows; {} are left out.", sPrimPath, plUsdPrimComponents::MaxInstances, uiLeftOut);

  pState->m_InstanceComponents = components;
}

void plUsdLiveLink::SendMaterials(plStringView sPrimPath, const PrimState& state)
{
  plUsdLiveTextures* pTextures = plUsdLiveManager::GetSingleton()->GetTextures();
  bool bPending = false;

  for (const plUsdMaterialGroup& group : state.m_MaterialGroups)
  {
    // Groups without a material of their own use the default material; see GetMaterialResource.
    if (!group.m_bHasSurface || group.m_Surface.m_sMaterialPath.IsEmpty())
      continue;

    // A material whose textures are still being prepared is sent with their previews, and without the ones that have no
    // preview yet, so the surface shows its values rather than the engine's missing-material placeholder. It is sent
    // again as previews and then final versions come in.
    plUsdSurface resolved = group.m_Surface;
    bool bSoftOpacity = false;
    if (!pTextures->Resolve(resolved, &bSoftOpacity))
    {
      bPending = true;

      // Without its mask a decal or a foliage card would show as a solid sheet, so it stays hidden until the mask is in.
      if (!resolved.m_sOpacityTexture.IsEmpty() && resolved.m_sBaseColorTexture.IsEmpty())
        resolved.m_fOpacity = 0.0f;
    }

    const plString sResource = GetMaterialResource(group);

    plDynamicArray<plUInt8> data;
    plUsdLiveResources::BuildMaterial(resolved, bSoftOpacity, data);

    // Many prims share one material; the engine needs it once.
    const plUInt64 uiHash = plHashingUtils::xxHash64(data.GetData(), data.GetCount());
    plUInt64 uiSent = 0;
    if (m_SentMaterials.TryGetValue(sResource, uiSent) && uiSent == uiHash)
      continue;

    if (plUsdBakeStore::WriteFile(sResource, data).Failed())
      plLog::Warning("USD Live: could not write the bake file of material '{}'", group.m_Surface.m_sMaterialPath);

    if (plUsdLiveResources::Send("Material", sResource, data))
    {
      m_SentMaterials[sResource] = uiHash;
      MarkBakeDirty();
    }
  }

  if (bPending)
    m_PendingMaterials.Insert(sPrimPath);
  else
    m_PendingMaterials.Remove(sPrimPath);
}

plStatus plUsdLiveLink::SetLayerMuted(plStringView sLayerIdentifier, bool bMuted)
{
  const plStatus res = m_Stage.SetLayerMuted(sLayerIdentifier, bMuted);
  if (res.Succeeded())
    m_bSyncPending = true;

  return res;
}

bool plUsdLiveLink::HasOpinions(const plUuid& object) const
{
  plString sPath;
  return IsWritable() && m_ObjectToPrim.TryGetValue(object, sPath) && m_Stage.HasEditLayerOpinion(sPath, "");
}

plStatus plUsdLiveLink::Revert(plArrayPtr<const plUuid> objects)
{
  if (!IsWritable())
    return plStatus("This link is read-only; start a live session to have opinions of your own.");

  plUInt32 uiReverted = 0;
  for (const plUuid& object : objects)
  {
    plString sPath;
    if (!m_ObjectToPrim.TryGetValue(object, sPath))
      continue;

    if (m_Stage.HasEditLayerOpinion(sPath, ""))
    {
      m_Stage.ClearEditLayerOpinions(sPath, "").IgnoreResult();
      ++uiReverted;
    }
  }

  if (uiReverted > 0)
  {
    plStatus res = SaveNow();
    m_bSyncPending = true;
    return res;
  }

  return plStatus(PL_SUCCESS);
}

//////////////////////////////////////////////////////////////////////////
// Writing back

void plUsdLiveLink::OnPropertyEvent(const plDocumentObjectPropertyEvent& e)
{
  if (m_bApplyingRemote || e.m_pObject == nullptr)
    return;

  plString sPath;
  if (!m_ObjectToPrim.TryGetValue(e.m_pObject->GetGuid(), sPath))
  {
    // The light or camera component of a generated object, or one the user added.
    const plDocumentObject* pOwner = e.m_pObject->GetParent();
    if (pOwner != nullptr && m_ObjectToPrim.TryGetValue(pOwner->GetGuid(), sPath) && e.m_pObject->GetGuid() == GetPrimComponentGuid(sPath))
      m_DirtyComponents.Insert(sPath);
    else if (plString sUserPrim = GetUserComponentPrim(e.m_pObject); !sUserPrim.IsEmpty())
      m_DirtyUserComponents.Insert(sUserPrim);

    return;
  }

  if (IsTransformProperty(e.m_sProperty))
  {
    m_DirtyTransforms.Insert(sPath);
  }
  else if (e.m_sProperty == "Active")
  {
    // A deactivated prim's object is hidden by the link, so its Active property says nothing about visibility.
    PrimState* pState = nullptr;
    if (m_Prims.TryGetValue(sPath, pState) && pState->m_bActive)
      m_DirtyVisibility.Insert(sPath);
  }
}

void plUsdLiveLink::OnStructureEvent(const plDocumentObjectStructureEvent& e)
{
  if (m_bApplyingRemote)
    return;

  // Moving an object there is refused by plUsdObjectPolicy; creating one there can't be, so it is pointed out.
  if (e.m_EventType == plDocumentObjectStructureEvent::Type::AfterObjectAdded && e.m_sParentProperty == "Children" && e.m_pNewParent != nullptr &&
      m_ObjectToPrim.Contains(e.m_pNewParent->GetGuid()))
  {
    plLog::Warning("USD Live: an object was added under '{}', which is generated from a USD stage. It is not saved with the scene; put it under "
                   "the link's root object instead.",
      GetPrimPath(e.m_pNewParent->GetGuid()));
    return;
  }

  if (e.m_sParentProperty != "Components")
    return;

  // Removal is seen after the fact, when the component is no longer among the object's; addition once it is.
  const plDocumentObject* pOwner = nullptr;
  if (e.m_EventType == plDocumentObjectStructureEvent::Type::AfterObjectAdded)
    pOwner = e.m_pNewParent;
  else if (e.m_EventType == plDocumentObjectStructureEvent::Type::AfterObjectRemoved)
    pOwner = e.m_pPreviousParent;

  plString sPath;
  if (pOwner != nullptr && m_ObjectToPrim.TryGetValue(pOwner->GetGuid(), sPath))
    m_DirtyUserComponents.Insert(sPath);
}

void plUsdLiveLink::GetGeneratedComponents(plStringView sPrimPath, const PrimState& state, plDynamicArray<plUuid>& out_components) const
{
  out_components.Clear();
  out_components.PushBack(GetPrimComponentGuid(sPrimPath));
  for (plUInt32 i = 0; i < state.m_uiMeshParts; ++i)
    out_components.PushBack(GetMeshComponentGuid(sPrimPath, i));
  out_components.PushBackRange(state.m_InstanceComponents);
}

plString plUsdLiveLink::GetUserComponentPrim(const plDocumentObject* pComponent) const
{
  const plDocumentObject* pOwner = pComponent->GetParent();
  if (pOwner == nullptr || pComponent->GetParentProperty() != "Components")
    return {};

  plString sPath;
  const PrimState* pState = nullptr;
  if (!m_ObjectToPrim.TryGetValue(pOwner->GetGuid(), sPath) || !m_Prims.TryGetValue(sPath, pState))
    return {};

  plHybridArray<plUuid, 8> generated;
  GetGeneratedComponents(sPath, *pState, generated);
  return generated.Contains(pComponent->GetGuid()) ? plString() : sPath;
}

void plUsdLiveLink::OnCommandHistoryEvent(const plCommandHistoryEvent& e)
{
  switch (e.m_Type)
  {
    case plCommandHistoryEvent::Type::TransactionEnded:
    case plCommandHistoryEvent::Type::UndoEnded:
    case plCommandHistoryEvent::Type::RedoEnded:
      FlushWriteBack();
      break;

    default:
      break;
  }
}

void plUsdLiveLink::FlushWriteBack()
{
  if (m_DirtyTransforms.IsEmpty() && m_DirtyVisibility.IsEmpty() && m_DirtyComponents.IsEmpty() && m_DirtyUserComponents.IsEmpty())
    return;

  if (!IsWritable())
  {
    if (!m_bReportedReadOnly)
    {
      plLog::Warning("USD Live: '{}' is linked read-only, so edits to its objects are not written back, and components added to them "
                     "are lost when the scene closes. Start a live session to write them into your own layer.",
        m_Settings.m_sStageFile);
      m_bReportedReadOnly = true;
    }

    m_DirtyTransforms.Clear();
    m_DirtyVisibility.Clear();
    m_DirtyComponents.Clear();
    m_DirtyUserComponents.Clear();
    return;
  }

  plUInt32 uiAuthored = 0;
  const plDocumentObjectManager* pManager = m_pDocument->GetObjectManager();
  plSet<plString> touched;

  for (const plString& sPath : m_DirtyTransforms)
  {
    PrimState* pState = nullptr;
    if (!m_Prims.TryGetValue(sPath, pState))
      continue;

    const plDocumentObject* pObject = pManager->GetObject(pState->m_Object);
    if (pObject == nullptr)
      continue;

    // An animated transform has no single value to write; a default would be hidden by the time samples anyway. The
    // object goes back to the animation's first sample.
    if (pState->m_bTransformAnimated)
    {
      m_bApplyingRemote = true;
      SetObjectTransform(pObject, pState->m_Transform);
      m_bApplyingRemote = false;

      plLog::Warning("USD Live: '{}' is animated in USD, so its transform cannot be edited in Plasma.", sPath);
      continue;
    }

    const plIReflectedTypeAccessor& props = pObject->GetTypeAccessor();

    plTransform t;
    t.m_vPosition = props.GetValue("LocalPosition").ConvertTo<plVec3>();
    t.m_qRotation = props.GetValue("LocalRotation").ConvertTo<plQuat>();
    t.m_vScale = props.GetValue("LocalScaling").ConvertTo<plVec3>() * props.GetValue("LocalUniformScaling").ConvertTo<float>();

    const plStatus res = m_Stage.AuthorLocalTransform(sPath, t);
    if (res.Failed())
    {
      plLog::Warning("USD Live: {}", res.m_sMessage);
      continue;
    }

    // What USD now holds, so the next sync does not see a difference and push it back.
    plUsdPrim prim;
    if (m_Stage.ReadPrim(sPath, prim).Succeeded())
      pState->m_Transform = prim.m_LocalTransform;

    touched.Insert(sPath);
    ++uiAuthored;
  }

  for (const plString& sPath : m_DirtyVisibility)
  {
    PrimState* pState = nullptr;
    if (!m_Prims.TryGetValue(sPath, pState))
      continue;

    const plDocumentObject* pObject = pManager->GetObject(pState->m_Object);
    if (pObject == nullptr)
      continue;

    const bool bVisible = pObject->GetTypeAccessor().GetValue("Active").ConvertTo<bool>();
    if (m_Stage.AuthorVisibility(sPath, bVisible).Succeeded())
    {
      pState->m_bVisible = bVisible;
      touched.Insert(sPath);
      ++uiAuthored;
    }
  }

  for (const plString& sPath : m_DirtyComponents)
  {
    PrimState* pState = nullptr;
    if (!m_Prims.TryGetValue(sPath, pState) || !pState->m_bHasComponent)
      continue;

    plUsdPrim applied;
    applied.m_Kind = pState->m_Kind;
    applied.m_Light = pState->m_Light;
    applied.m_Camera = pState->m_Camera;

    plUsdLight light;
    plUsdCamera camera;
    if (!plUsdPrimComponents::ReadBack(pManager->GetObject(GetPrimComponentGuid(sPath)), applied, light, camera))
      continue;

    const plStatus res = pState->m_Kind == plUsdPrimKind::Camera ? m_Stage.AuthorCamera(sPath, camera) : m_Stage.AuthorLight(sPath, light);
    if (res.Failed())
    {
      plLog::Warning("USD Live: {}", res.m_sMessage);
      continue;
    }

    // What USD now holds, so the next sync does not see a difference and push it back.
    plUsdPrim prim;
    if (m_Stage.ReadPrim(sPath, prim).Succeeded())
    {
      pState->m_Light = prim.m_Light;
      pState->m_Camera = prim.m_Camera;
    }

    touched.Insert(sPath);
    ++uiAuthored;
  }

  for (const plString& sPath : m_DirtyUserComponents)
  {
    PrimState* pState = nullptr;
    if (!m_Prims.TryGetValue(sPath, pState) || !pState->m_bActive)
      continue;

    const plDocumentObject* pObject = pManager->GetObject(pState->m_Object);
    plUsdPrim current;
    if (pObject == nullptr || m_Stage.ReadPrim(sPath, current).Failed())
      continue;

    plHybridArray<plUuid, 8> generated;
    GetGeneratedComponents(sPath, *pState, generated);

    plDynamicArray<plUsdComponent> components;
    plUsdPrimComponents::ReadUserComponents(pObject, generated, current.m_Components, components);

    // Components of types this editor doesn't know are on the stage but can't be on the object; they stay.
    for (const plUsdComponent& component : current.m_Components)
    {
      if (plRTTI::FindTypeByName(component.m_sType) == nullptr)
        components.PushBack(component);
    }

    if (components == current.m_Components)
      continue;

    const plStatus res = m_Stage.AuthorComponents(sPath, components);
    if (res.Failed())
    {
      plLog::Warning("USD Live: {}", res.m_sMessage);
      continue;
    }

    // What USD now holds, so the next sync does not see a difference and push it back.
    if (m_Stage.ReadPrim(sPath, current).Succeeded())
      pState->m_Components = current.m_Components;

    touched.Insert(sPath);
    ++uiAuthored;
  }

  m_DirtyTransforms.Clear();
  m_DirtyVisibility.Clear();
  m_DirtyComponents.Clear();
  m_DirtyUserComponents.Clear();

  if (uiAuthored > 0)
  {
    plLog::Dev("USD Live: wrote {} change(s) into '{}'", uiAuthored, m_sParticipantLayer);
    ScheduleSave();
    RefreshOpinions(touched);

    // The bake reads the composed stage, which now has these opinions.
    MarkBakeDirty();
  }

  // Our own edits notify the stage too; nothing needs re-reading for them.
  plUsdChanges ignored;
  m_Stage.ConsumeChanges(ignored);
}

void plUsdLiveLink::ScheduleSave()
{
  // Every change to this editor's layer goes through here or SaveNow; the hub sends the layer when this moves.
  ++m_uiOwnLayerVersion;
  m_bSaveScheduled = true;
  m_SaveDue = plTime::Now() + SaveDelay;
}

plStatus plUsdLiveLink::ApplyRemoteLayer(plStringView sLayerFile, plStringView sUsda)
{
  PL_SUCCEED_OR_RETURN(m_Stage.ImportLayerContent(sLayerFile, sUsda));

  bool bKnown = false;
  for (const plString& sLayer : m_RemoteLayers)
    bKnown |= sLayer.IsEqual_NoCase(sLayerFile);
  if (!bKnown)
    m_RemoteLayers.PushBack(sLayerFile);

  m_bSyncPending = true;
  return plStatus(PL_SUCCESS);
}

void plUsdLiveLink::ReleaseRemoteLayer(plStringView sLayerFile)
{
  for (plUInt32 i = 0; i < m_RemoteLayers.GetCount(); ++i)
  {
    if (m_RemoteLayers[i].IsEqual_NoCase(sLayerFile))
    {
      m_RemoteLayers.RemoveAtAndCopy(i);
      return;
    }
  }
}

bool plUsdLiveLink::IsSessionOwner() const
{
  if (!IsWritable())
    return false;

  return plUsdStage::GetSessionOwnerLayer(m_sOpenedFile).IsEqual_NoCase(m_sParticipantLayer);
}

plStatus plUsdLiveLink::CommitSession(plUInt32& out_uiOpinions)
{
  out_uiOpinions = 0;

  if (!IsSessionOwner())
    return plStatus("Only the session's owner, whose layer is strongest, can commit it.");

  PL_SUCCEED_OR_RETURN(SaveNow());
  PL_SUCCEED_OR_RETURN(plUsdStage::CommitSession(m_sOpenedFile, out_uiOpinions));

  // The stage shares the layers the commit changed, so it already composes the result.
  ++m_uiOwnLayerVersion;
  m_bSyncPending = true;
  MarkBakeDirty();
  return plStatus(PL_SUCCESS);
}

void plUsdLiveLink::RefreshOpinions(const plSet<plString>& primPaths)
{
  plHybridArray<plUuid, 16> changedRows;

  for (const plString& sPath : primPaths)
  {
    PrimState* pState = nullptr;
    if (!m_Prims.TryGetValue(sPath, pState))
      continue;

    plUsdPrimOpinions opinions;
    m_Stage.QueryPrimOpinions(sPath, opinions);

    if (!(opinions == pState->m_Opinions))
    {
      pState->m_Opinions = opinions;
      changedRows.PushBack(pState->m_Object);
    }
  }

  if (!changedRows.IsEmpty())
  {
    plObjectRowDecoratorRegistry::RequestRefresh(m_pDocument, changedRows);
    plUsdLiveManager::GetSingleton()->BroadcastPrimStatesChanged(this);
  }
}

bool plUsdLiveLink::GetPrimInfo(const plUuid& object, plUsdLivePrimInfo& out_info) const
{
  plString sPath;
  if (!m_ObjectToPrim.TryGetValue(object, sPath))
    return false;

  const PrimState* pState = nullptr;
  if (!m_Prims.TryGetValue(sPath, pState))
    return false;

  out_info.m_sPath = sPath;
  out_info.m_sTypeName = pState->m_sTypeName;
  out_info.m_Kind = pState->m_Kind;
  out_info.m_bActive = pState->m_bActive;
  out_info.m_bTransformAnimated = pState->m_bTransformAnimated;
  out_info.m_bPrototype = pState->m_bPrototype;
  out_info.m_uiInstances = pState->m_Instancer.m_uiInstanceCount;
  out_info.m_Opinions = pState->m_Opinions;
  out_info.m_MaterialGroups = pState->m_MaterialGroups;
  return true;
}

plStatus plUsdLiveLink::SetSurfaceValue(plStringView sMaterialPath, plUsdStage::SurfaceValue value, const plVariant& data)
{
  if (!IsWritable())
    return plStatus("This link is read-only; start a live session to have opinions of your own.");

  PL_SUCCEED_OR_RETURN(m_Stage.AuthorSurfaceValue(sMaterialPath, value, data));

  // The next sync reads the new surface, resends the material and rebakes.
  ScheduleSave();
  m_bSyncPending = true;
  return plStatus(PL_SUCCESS);
}

plStatus plUsdLiveLink::RevertSurface(plStringView sMaterialPath)
{
  if (!IsWritable())
    return plStatus("This link is read-only; start a live session to have opinions of your own.");

  PL_SUCCEED_OR_RETURN(m_Stage.ClearSurfaceValues(sMaterialPath));

  ScheduleSave();
  m_bSyncPending = true;
  return plStatus(PL_SUCCESS);
}

bool plUsdLiveLink::HasSurfaceOpinion(plStringView sMaterialPath) const
{
  return IsWritable() && m_Stage.HasSurfaceOpinion(sMaterialPath);
}

void plUsdLiveLink::CollectPrimPaths(plArrayPtr<const plUuid> objects, plDynamicArray<plString>& out_paths) const
{
  for (const plUuid& object : objects)
  {
    plString sPath;
    if (m_ObjectToPrim.TryGetValue(object, sPath))
      out_paths.PushBack(sPath);
  }
}

plStatus plUsdLiveLink::SetPrimsActive(plArrayPtr<const plUuid> objects, bool bActive)
{
  if (!IsWritable())
    return plStatus("This link is read-only; start a live session to have opinions of your own.");

  plHybridArray<plString, 16> paths;
  CollectPrimPaths(objects, paths);

  plUInt32 uiChanged = 0;
  for (const plString& sPath : paths)
  {
    // Reactivating removes this editor's opinion rather than authoring active = true, so the prim follows the other
    // layers again.
    const plStatus res = bActive ? m_Stage.ClearEditLayerOpinions(sPath, "active") : m_Stage.AuthorActive(sPath, false);
    if (res.Failed())
      return res;

    ++uiChanged;
  }

  if (uiChanged > 0)
  {
    plLog::Dev("USD Live: {} {} prim(s) in '{}'", bActive ? "reactivated" : "deactivated", uiChanged, m_sParticipantLayer);
    ScheduleSave();

    // Activation changes which prims exist below these, so the tree is re-read rather than patched.
    m_bSyncPending = true;
  }

  return plStatus(PL_SUCCESS);
}

void plUsdLiveLink::GetVariantSets(const plUuid& object, plDynamicArray<plUsdVariantSet>& out_sets) const
{
  out_sets.Clear();

  plString sPath;
  if (m_ObjectToPrim.TryGetValue(object, sPath))
    m_Stage.GetVariantSets(sPath, out_sets);
}

plStatus plUsdLiveLink::SetVariantSelection(const plUuid& object, plStringView sVariantSet, plStringView sVariant)
{
  if (!IsWritable())
    return plStatus("This link is read-only; start a live session to have opinions of your own.");

  plString sPath;
  if (!m_ObjectToPrim.TryGetValue(object, sPath))
    return plStatus("The object does not belong to this link.");

  PL_SUCCEED_OR_RETURN(m_Stage.AuthorVariantSelection(sPath, sVariantSet, sVariant));

  // Saved right away rather than with the next burst of edits: the switch recomposes a whole subtree for everyone.
  const plStatus res = SaveNow();
  m_bSyncPending = true;
  return res;
}

plStatus plUsdLiveLink::RevertProperty(plArrayPtr<const plUuid> objects, plStringView sWhat)
{
  if (!IsWritable())
    return plStatus("This link is read-only; start a live session to have opinions of your own.");

  plHybridArray<plString, 16> paths;
  CollectPrimPaths(objects, paths);

  plUInt32 uiReverted = 0;
  for (const plString& sPath : paths)
  {
    if (m_Stage.HasEditLayerOpinion(sPath, sWhat))
    {
      m_Stage.ClearEditLayerOpinions(sPath, sWhat).IgnoreResult();
      ++uiReverted;
    }
  }

  if (uiReverted > 0)
  {
    const plStatus res = SaveNow();
    m_bSyncPending = true;
    return res;
  }

  return plStatus(PL_SUCCESS);
}
