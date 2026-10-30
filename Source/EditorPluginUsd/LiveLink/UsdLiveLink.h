#pragma once

#include <EditorPluginUsd/EditorPluginUsdDLL.h>
#include <EditorPluginUsd/LiveLink/UsdBakeStore.h>
#include <Foundation/Containers/Deque.h>
#include <Foundation/Containers/HashTable.h>
#include <Foundation/Containers/Set.h>
#include <Foundation/Threading/TaskSystem.h>
#include <Foundation/Time/Time.h>
#include <Foundation/Types/Uuid.h>
#include <UsdBridge/UsdStage.h>

class plDocument;
class plAssetDocument;
class plDocumentObject;
class plEditorEngineDocumentMsg;
class plObjectDirectAccessor;
struct plDocumentObjectPropertyEvent;
struct plDocumentObjectStructureEvent;
struct plCommandHistoryEvent;

/// \brief What the UI shows about one generated object's prim.
struct plUsdLivePrimInfo
{
  plString m_sPath;
  plString m_sTypeName;
  plUsdPrimKind m_Kind = plUsdPrimKind::Group;
  bool m_bActive = true;
  bool m_bTransformAnimated = false; ///< The transform has time samples, so Plasma cannot edit it.
  bool m_bPrototype = false;         ///< Below a point instancer, so hidden: only the instancer draws it.
  plUInt32 m_uiInstances = 0;        ///< For point instancers: how many instances they draw.
  plUsdPrimOpinions m_Opinions;
  plHybridArray<plUsdMaterialGroup, 1> m_MaterialGroups; ///< For meshes: each group's surface, with its material path.
};

/// \brief One USD stage linked to one object of a scene document.
///
/// The stage's prims become child objects of the link's root object. They are marked transient, so they are mirrored
/// to the engine process and can be selected and edited, but are never saved into the scene: they are rebuilt from the
/// stage each time the scene opens. Meshes are tessellated on worker tasks into the link's bake folder (see
/// plUsdBakeStore), which the engine loads them from; prims with the same geometry and materials share one mesh, and a
/// mesh whose files are current from an earlier session is not built again. Only a rebuilt mesh the engine may already
/// have, and materials, are sent as resource updates. Once the link is idle it writes its bake prefab and keeps a USD
/// Bake object under the root (see plUsdBakePrefab), which exports and games use instead of the live objects.
///
/// Geometry is only read for meshes that need building: which prims changed comes from the stage's change notices
/// (see plUsdStage::ConsumeGeometryChanges), and which prims share a mesh from the specs they are composed from.
///
/// Changes other tools save into the stage's layers are picked up by polling the layer files. In a live session, moves
/// and visibility changes made in the editor, light, camera and material values, variant selections, and components
/// the user adds to generated objects (see plUsdComponent) are written into the session's participant layer and saved,
/// so every other tool sees them; without a session the link is read-only. Generated objects are never deleted by the user: deleting
/// one deactivates its prim (see plUsdObjectPolicy), and a deactivated prim keeps its object, hidden.
class PL_EDITORPLUGINUSD_DLL plUsdLiveLink
{
  PL_DISALLOW_COPY_AND_ASSIGN(plUsdLiveLink);

public:
  struct Settings
  {
    plString m_sStageFile;    ///< The asset's root layer, as the user linked it.
    plString m_sSessionName;  ///< Empty when the link is read-only.
    plString m_sParticipant;  ///< Name of this editor's layer in the session, e.g. "plasma-josh".
  };

  plUsdLiveLink(plDocument* pDocument, const plUuid& rootObject, const Settings& settings);
  ~plUsdLiveLink();

  /// \brief Opens the stage (the session root when a session is set) and generates the objects.
  plStatus Open();

  /// \brief Stops all work. With bRemoveObjects the generated objects are taken out of the document as well.
  void Close(bool bRemoveObjects);

  /// \brief Called regularly on the main thread: picks up changed layers, applies finished meshes, writes back edits
  /// and saves the participant layer.
  void Update();

  /// \brief Creates or joins a live session beside the asset and reopens the stage through it.
  plStatus StartSession(plStringView sSessionName, plStringView sParticipant);

  /// \brief Re-reads every prim and re-sends every mesh and material, e.g. after the engine process restarted.
  void RequestFullResync();

  /// \brief Removes this editor's opinions for the given objects, so the other layers' values show again.
  plStatus Revert(plArrayPtr<const plUuid> objects);

  plStatus SaveNow();

  plDocument* GetDocument() const { return m_pDocument; }
  const plUuid& GetRootObject() const { return m_RootObject; }
  const Settings& GetSettings() const { return m_Settings; }
  bool IsOpen() const { return m_Stage.IsOpen(); }
  bool IsWritable() const { return m_Stage.HasEditLayer(); }
  bool HasUnsavedChanges() const { return m_Stage.IsOpen() && m_Stage.IsEditLayerDirty(); }
  plStringView GetStatus() const { return m_sStatus; }
  plUInt32 GetPrimCount() const { return m_Prims.GetCount(); }
  plUInt32 GetPendingMeshCount() const { return m_MeshQueue.GetCount() + m_Jobs.GetCount(); }
  const plUsdBasis& GetBasis() const { return m_Stage.GetBasis(); }
  void GetLayers(plDynamicArray<plUsdLayerInfo>& out_layers) const { m_Stage.GetLayerStack(out_layers); }
  plUInt32 CountLayerOpinions(plStringView sLayerIdentifier) const { return m_Stage.CountLayerOpinions(sLayerIdentifier); }

  /// \brief The layer this editor writes into; empty when the link is read-only.
  plStringView GetParticipantLayer() const { return m_sParticipantLayer; }

  /// \brief How many participant layers the session has, including this editor's, as of the last sync. 0 without a
  /// session.
  plUInt32 GetParticipantCount() const { return m_uiParticipantCount; }

  /// \brief Mutes or unmutes a layer in this editor only and resyncs the objects.
  plStatus SetLayerMuted(plStringView sLayerIdentifier, bool bMuted);

  /// \brief Whether this editor's layer holds any opinion for the object's prim.
  bool HasOpinions(const plUuid& object) const;

  /// \brief Real path of a layer whose decisions the UI highlights, e.g. when a participant is picked in the panel.
  /// Empty for none.
  plStringView GetHighlightedLayer() const { return m_sHighlightedLayer; }
  void SetHighlightedLayer(plStringView sLayerPath) { m_sHighlightedLayer = sLayerPath; }

  bool IsGeneratedObject(const plUuid& object) const { return m_ObjectToPrim.Contains(object); }
  plString GetPrimPath(const plUuid& object) const;

  /// \brief The cached state of a generated object's prim, as of the last sync. False for other objects.
  bool GetPrimInfo(const plUuid& object, plUsdLivePrimInfo& out_info) const;

  /// \brief Deactivates the objects' prims in this editor's layer, or removes that opinion again.
  plStatus SetPrimsActive(plArrayPtr<const plUuid> objects, bool bActive);

  /// \brief Removes this editor's opinions of one kind from the objects' prims: "xform", "visibility", "active", "values"
  /// (a light's or camera's settings), "variants" (variant selections), or empty for all.
  plStatus RevertProperty(plArrayPtr<const plUuid> objects, plStringView sWhat);

  /// \brief The variant sets of the prim an object stands for, with the variant the stage composes.
  void GetVariantSets(const plUuid& object, plDynamicArray<plUsdVariantSet>& out_sets) const;

  /// \brief Selects a variant in this editor's layer and saves it. The prim's objects below are rebuilt on the next sync.
  plStatus SetVariantSelection(const plUuid& object, plStringView sVariantSet, plStringView sVariant);

  /// \brief Writes one value of a material's surface into this editor's layer. The material is resent and rebaked.
  plStatus SetSurfaceValue(plStringView sMaterialPath, plUsdStage::SurfaceValue value, const plVariant& data);

  /// \brief Removes this editor's opinions on a material's surface.
  plStatus RevertSurface(plStringView sMaterialPath);

  /// \brief Whether this editor's layer sets any of a material's surface values.
  bool HasSurfaceOpinion(plStringView sMaterialPath) const;

  /// \brief True while the link itself changes the document, e.g. adding or removing generated objects.
  bool IsApplyingRemote() const { return m_bApplyingRemote; }

  // --- Live hub -----------------------------------------------------------------------------------------------------

  /// \brief The session root layer the stage was opened through; empty without a session.
  plStringView GetSessionRoot() const { return IsWritable() ? plStringView(m_sOpenedFile) : plStringView(); }

  /// \brief This editor's layer as usda text, for the hub to send to the other participants.
  plString ExportOwnLayer() const { return m_Stage.ExportEditLayer(); }

  /// \brief Counts changes to this editor's layer; the hub sends the layer whenever it moved.
  plUInt64 GetOwnLayerVersion() const { return m_uiOwnLayerVersion; }

  /// \brief Replaces another participant's layer with content that arrived through the hub, and stops reloading that
  /// layer from disk until ReleaseRemoteLayer: while its tool is connected, the hub is the newer source.
  plStatus ApplyRemoteLayer(plStringView sLayerFile, plStringView sUsda);

  /// \brief The tool of a remote layer disconnected: the layer's file is watched again.
  void ReleaseRemoteLayer(plStringView sLayerFile);

  /// \brief Whether this editor's layer is the session's strongest, whose participant may commit the session.
  bool IsSessionOwner() const;

  /// \brief Folds every participant layer into the asset and empties them (see plUsdStage::CommitSession). Owner only.
  plStatus CommitSession(plUInt32& out_uiOpinions);

  /// \brief Identifies a link by its root object and stage; part of its resource ids and generated GUIDs.
  static plString MakeLinkId(const plUuid& rootObject, plStringView sStageFile);
  plStringView GetLinkId() const { return m_sLinkId; }

  /// \brief The tags every generated object carries: UsdLive, which the editor-engine plugin looks for, and
  /// "Exclude From Export", which keeps the live objects out of exported scenes and prefabs.
  static constexpr const char* LiveTag = "UsdLive";
  static constexpr const char* ExcludeFromExportTag = "Exclude From Export";

  // --- Bake ---------------------------------------------------------------------------------------------------------

  /// \brief Bakes the link once it is idle: about two seconds after the last change, with every mesh built and every
  /// texture prepared. With bRebuild, every mesh and material is built and written again first.
  void RequestBake(bool bRebuild);

  /// \brief One line for the UI: "Up to date", "Waiting for changes to settle", "Failed: ...".
  plStringView GetBakeStatus() const { return m_sBakeStatus; }
  bool IsBakePending() const { return m_bBakeCheckPending; }

private:
  struct PrimState
  {
    plUuid m_Object;
    plUsdPrimKind m_Kind = plUsdPrimKind::Group;
    plString m_sTypeName;
    plTransform m_Transform = plTransform::MakeIdentity();
    bool m_bVisible = true;
    bool m_bActive = true;
    plUsdPrimOpinions m_Opinions;
    plHybridArray<plUsdMaterialGroup, 1> m_MaterialGroups;
    plUInt64 m_uiMeshKey = 0;        ///< The mesh (see plUsdBakeStore::GetMeshKey) the prim's geometry and materials need.
    plUInt64 m_uiAppliedMeshKey = 0; ///< The mesh the object's mesh components show; 0 while there are none.
    plUInt32 m_uiMeshParts = 0;      ///< How many mesh components the object has.
    plUsdLight m_Light;           ///< As last applied to the light component.
    plUsdCamera m_Camera;         ///< As last applied to the camera component.
    bool m_bHasComponent = false; ///< The object carries a light or camera component; see plUsdPrimComponents.
    bool m_bTransformAnimated = false;
    bool m_bPrototype = false;
    plUsdInstancer m_Instancer;                    ///< As last read, for point instancers.
    plHybridArray<plUuid, 2> m_InstanceComponents; ///< The instanced mesh components ApplyInstancer put on the object.
    plDynamicArray<plUsdComponent> m_Components;   ///< Plasma components as the stage last had them; see plUsdComponent.
  };

  /// \brief The files of one mesh, shared by every prim with the same geometry and materials, e.g. the copies of one
  /// referenced asset.
  struct Mesh
  {
    plString m_sName;            ///< See plUsdBakeStore::GetMeshName.
    plUInt64 m_uiFileStamp = 0;  ///< plUsdGeometryKey::m_uiFileStamp as of the last time a prim asked for the mesh.
    plUInt32 m_uiParts = 0;      ///< Of the files that exist; 0 until the first build finished.
    plUInt32 m_uiGeneration = 0; ///< Counts build requests; a build that finishes after a newer request is dropped.
    bool m_bQueued = false;
  };

  struct MeshJob
  {
    plUInt64 m_uiMeshKey = 0;
    plUInt32 m_uiGeneration = 0;
    plString m_sMesh;
    plUInt64 m_uiFileStamp = 0;
    bool m_bTangents = false;
    plDynamicArray<plString> m_MaterialResources; ///< One per material group.
    plUniquePtr<plUsdGeometrySource> m_pSource;
    plDynamicArray<plDynamicArray<plUInt8>> m_Parts; ///< Freed once the task wrote them.
    plUInt32 m_uiPartCount = 0;
    plDynamicArray<bool> m_ReplacedParts; ///< Per part: the task overwrote an older file, which the engine may have loaded.
    plStatus m_Result;
    plTaskGroupID m_TaskGroup;
  };

  void Sync();

  /// \brief Points the prim at the mesh its geometry and materials need. A mesh built before, in this session or in an
  /// earlier one from the same layer files, is shown right away; otherwise, or with bRebuild, the mesh is queued.
  void RequireMesh(plStringView sPrimPath, PrimState& ref_state, const plUsdGeometryKey& geometry, bool bRebuild);
  void QueueMesh(plUInt64 uiMeshKey);
  void LaunchMeshJobs();
  void FinishMeshJobs(bool bWait);
  void ApplyMeshJob(MeshJob& job);

  /// \brief Records a finished build's files in the manifest. False when the build failed or was superseded.
  bool RecordMeshJob(const MeshJob& job);

  /// \brief Gives the prim's object one mesh component per part of its mesh, and sends its materials.
  void ApplyMesh(plStringView sPrimPath, PrimState& ref_state);

  /// \brief Sends the material of each of a prim's material groups, with the textures prepared so far. While any are
  /// still being prepared the prim stays in m_PendingMaterials and is sent again. Identical bytes are not sent twice.
  void SendMaterials(plStringView sPrimPath, const PrimState& state);

  /// \brief Starts preparing a prim's textures without sending anything, so they are ready by the time its mesh is.
  void RequestTextures(const PrimState& state) const;

  /// \brief Gives a point instancer's object one instanced mesh component per part of each prototype mesh built so far,
  /// and removes the ones it no longer needs. The prototype meshes' files are shared with their own, hidden, objects.
  void ApplyInstancer(plStringView sPrimPath);

  /// \brief Queues every instancer that draws the mesh for ApplyInstancer, e.g. once the mesh was built.
  void MarkInstancersUsing(plStringView sMeshPath);
  void FlushInstancers();

  plString GetMaterialResource(const plUsdMaterialGroup& group) const;

  /// \brief The mesh components' colour. It tints the whole mesh, so it only carries displayColor when the prim has a
  /// single group and that group has no material.
  static plColor GetTint(const PrimState& state);

  /// \brief One part of the mesh the prim's components show; empty while they show none.
  plString GetMeshResource(const PrimState& state, plUInt32 uiPart) const;
  plUuid GetObjectGuid(plStringView sPrimPath) const;
  plUuid GetMeshComponentGuid(plStringView sPrimPath, plUInt32 uiPart) const;
  plUuid GetPrimComponentGuid(plStringView sPrimPath) const;
  plString GetInstanceGuidSeed(plStringView sInstancerPath, plStringView sMeshPath, plUInt32 uiPart) const;

  /// \brief Sets a property without an undo step. Goes through the object manager rather than plObjectDirectAccessor,
  /// whose SetValue writes the value silently: no property event, so neither the engine mirror nor the property grid
  /// would ever see it.
  void SetProperty(const plDocumentObject* pObject, plStringView sProperty, const plVariant& value);
  void SetObjectTransform(const plDocumentObject* pObject, const plTransform& t);

  /// \brief Adds a tag to a game object's Tags unless it is there already. Without an undo step, like SetProperty.
  void AddTag(const plDocumentObject* pObject, plStringView sTag);

  /// \brief Whether the engine process has this document loaded. Resource updates sent before then are dropped.
  bool IsEngineReady() const;
  void OnEngineMessage(const plEditorEngineDocumentMsg* pMsg);

  void OnPropertyEvent(const plDocumentObjectPropertyEvent& e);
  void OnStructureEvent(const plDocumentObjectStructureEvent& e);
  void OnCommandHistoryEvent(const plCommandHistoryEvent& e);

  /// \brief The components the link itself put on a prim's object: mesh parts, instanced meshes, light or camera.
  void GetGeneratedComponents(plStringView sPrimPath, const PrimState& state, plDynamicArray<plUuid>& out_components) const;

  /// \brief The prim a component the user put on a generated object belongs to, or empty.
  plString GetUserComponentPrim(const plDocumentObject* pComponent) const;
  void FlushWriteBack();
  void ScheduleSave();

  /// \brief Re-reads the opinions of prims this editor just wrote to and repaints the rows that changed.
  void RefreshOpinions(const plSet<plString>& primPaths);

  void CollectPrimPaths(plArrayPtr<const plUuid> objects, plDynamicArray<plString>& out_paths) const;

  /// \brief Clears the document's undo and redo history after generated objects were removed, since undo steps may
  /// still point at them.
  void ForgetUndoHistory(plStringView sReason);

  /// \brief Something the bake is made from may have changed; the bake is checked once the link is idle.
  void MarkBakeDirty();

  /// \brief Writes the bake prefab when its content hash differs from the last bake's, and adds the USD Bake object.
  void RunBake();

  plDocument* m_pDocument = nullptr;
  plAssetDocument* m_pAssetDocument = nullptr;
  plUuid m_RootObject;
  bool m_bRootMissing = false;
  Settings m_Settings;
  plString m_sLinkId;
  plString m_sStatus;

  plUsdStage m_Stage;
  plString m_sOpenedFile;
  plString m_sParticipantLayer;
  plString m_sHighlightedLayer;
  plUInt32 m_uiParticipantCount = 0;

  plHashTable<plString, PrimState> m_Prims;
  plHashTable<plUuid, plString> m_ObjectToPrim;

  plHashTable<plUInt64, Mesh> m_Meshes; ///< By mesh key.
  plDeque<plUInt64> m_MeshQueue;        ///< Mesh keys waiting for a build.
  plDynamicArray<plUniquePtr<MeshJob>> m_Jobs;

  /// Mesh prim -> the point instancers that draw it; see MarkInstancersUsing. Rebuilt when an instancer changed.
  plHashTable<plString, plHybridArray<plString, 1>> m_InstancersByMesh;
  bool m_bInstancersByMeshDirty = true;

  plSet<plString> m_PendingMaterials;              ///< Prim paths whose material waits for textures.
  plUInt32 m_uiTexturesFinished = 0;               ///< plUsdLiveTextures::GetFinishedCount when they were last retried.
  plHashTable<plString, plUInt64> m_SentMaterials; ///< Material resource -> hash of the bytes last sent.
  bool m_bResendMaterials = false;                 ///< The engine lost what was sent; every material is sent again.

  bool m_bApplyingRemote = false;
  bool m_bSyncPending = false;
  bool m_bRefreshAllRows = false; ///< The next sync repaints every row of the scene tree, e.g. after a reopen.
  bool m_bReportedReadOnly = false;
  plSet<plString> m_DirtyTransforms;
  plSet<plString> m_DirtyVisibility;
  plSet<plString> m_DirtyComponents; ///< Prims whose light or camera component the user edited.
  plSet<plString> m_DirtyInstancers; ///< Point instancers waiting for ApplyInstancer.
  plSet<plString> m_DirtyUserComponents; ///< Prims whose objects got user components added, changed or removed.

  plTime m_NextPoll;
  plTime m_SaveDue;
  bool m_bSaveScheduled = false;

  plUInt64 m_uiOwnLayerVersion = 0;
  plHybridArray<plString, 4> m_RemoteLayers; ///< Layers whose content comes through the hub, not their files.

  plUsdBakeStore::Manifest m_Manifest; ///< Loaded on the first Open and kept up to date as meshes are built.
  bool m_bManifestLoaded = false;
  bool m_bBakeCheckPending = false;
  plTime m_BakeDue;
  plString m_sBakeStatus;

  plEventSubscriptionID m_PropertySubscription = 0;
  plEventSubscriptionID m_StructureSubscription = 0;
  plEventSubscriptionID m_HistorySubscription = 0;
  plEventSubscriptionID m_EngineMessageSubscription = 0;
};
