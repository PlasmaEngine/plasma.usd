#pragma once

#include <EditorPluginUsd/PartitionImport/UsdPartitionImportJob.h>
#include <EditorUi/Runtime.h>
#include <Foundation/Communication/Event.h>
#include <Foundation/Configuration/Singleton.h>
#include <Foundation/Threading/TaskSystem.h>
#include <ToolsFoundation/Document/DocumentManager.h>

class plScene2Document;

struct plUsdPartitionImportEvent
{
  enum class Type
  {
    Started,
    Progress, ///< A few times a second while running.
    Finished, ///< m_Result says how; problems are in the partition's manifest.
  };

  Type m_Type = Type::Started;
  plUuid m_Partition;
  plStatus m_Result;
  bool m_bCancelled = false; ///< Finished by Cancel or by closing the scene; the import resumes, nothing failed.
};

/// \brief Runs USD world partition imports from the main thread: one at a time, for one scene.
///
/// Start creates the partition (or picks up the one of that name with an import manifest, which resumes an unfinished
/// import or updates a finished one from the stage), saves the scene and runs a plUsdPartitionImportJob on a worker.
/// While it runs, the files it writes are passed to the asset curator, so cells appear and loaded ones reload. When the
/// worker is done the materials are written once their textures are prepared; then, once the scene neither simulates
/// nor has an edit in progress, the lights and cameras go into the always loaded layer "<Partition> Globals",
/// oversized objects into "<Partition> Distant", cells left empty move to the backup folder, and the scene is saved
/// unless it has unsaved changes of the user's. Closing the scene stops the worker without waiting for it. Nothing
/// shows a dialog.
class PL_EDITORPLUGINUSD_DLL plUsdPartitionImporter
{
  PL_DECLARE_SINGLETON(plUsdPartitionImporter);

public:
  plUsdPartitionImporter();
  ~plUsdPartitionImporter();

  /// \brief Imports the stage into the partition options.m_sPartitionName of the scene. When that partition exists
  /// and has a manifest, its import resumes or updates with these options (the stage and cell size must match); roots
  /// whose plan the options change are scanned again. Fails without starting when another import runs.
  plStatus Start(plScene2Document* pScene, const plUsdPartitionImportOptions& options);

  /// \brief 'Update from USD': scans the roots whose layer files changed and rewrites the cells whose content changed.
  plStatus Update(plScene2Document* pScene, const plUuid& partition);

  /// \brief Stops the running import; it resumes with Start or Update.
  void Cancel();

  bool IsRunning() const { return m_pJob != nullptr; }
  const plScene2Document* GetScene() const { return m_pScene; }
  const plUuid& GetPartition() const { return m_Partition; }
  void GetProgress(plUsdPartitionImportProgress& out_progress) const;

  /// \brief The manifest of a partition made by an import; fails for other partitions.
  static plStatus LoadManifest(const plScene2Document* pScene, const plUuid& partition, plUsdPartitionImportManifest& out_manifest);

  /// \brief Runs an import from JSON (see plUsdPartitionImportOptions::ReadJson), for the action and scripts. "scene" names
  /// an open scene by path; without it the only open scene is used. For a partition imported before, the fields the
  /// JSON leaves out keep the earlier import's values. {"partition": name, "update": true} updates a partition from its
  /// stage; {"cancel": true} stops the running import.
  plStatus StartFromJson(plStringView sJson);

  plEvent<const plUsdPartitionImportEvent&> m_Events;

private:
  plStatus StartJob(plScene2Document* pScene, const plUuid& partition, plUsdPartitionImportManifest&& manifest, bool bSceneChanged);
  void Tick();
  void PassWrittenFiles();
  void SendReloads();
  bool WriteMaterials();
  void Finish();
  void FinishAndReport(const plStatus& result, bool bCancelled = false);
  plStatus WriteGlobals(plUsdPartitionImportManifest& ref_manifest, bool& ref_bSceneChanged);
  plStatus EnsureLayer(plStringView sFile, bool& ref_bSceneChanged);
  void RemoveEmptiedCells();
  void Abandon();
  void DocumentManagerEventHandler(const plDocumentManager::Event& e);

  plScene2Document* m_pScene = nullptr;
  plUuid m_Partition;
  plString m_sManifestFile;
  plString m_sGlobalsFile;
  plString m_sDistantFile;
  bool m_bSceneChanged = false;
  bool m_bWorkerDone = false;
  bool m_bWaitLogged = false;
  bool m_bMaterialsDone = false;
  bool m_bDetached = false;            ///< The scene closed; the cancelled worker is collected by Tick.
  bool m_bSceneModifiedAtStart = false; ///< The user's unsaved changes: the import then does not save the scene.
  plUInt64 m_uiHistoryStamp = 0;        ///< Of the scene's history; another value means the user edited the scene.
  plDynamicArray<plString> m_PendingReloads; ///< Mesh resources the engine reloads, a few per tick, from m_uiNextReload on.
  plUInt32 m_uiNextReload = 0;
  plUniquePtr<plUsdPartitionImportJob> m_pJob;
  plTaskGroupID m_Task;
  plEditorUiTimerGroup m_Timers; ///< drives Tick every 50 ms
  plEventSubscriptionID m_DocumentSubscription = 0;
  plTime m_LastProgressEvent;

  // The scene's header lists the cells, and the asset curator may report the last written cells after Finish: the scene
  // is saved again once it has them, for a few seconds.
  plScene2Document* m_pSaveScene = nullptr;
  plTime m_SaveUntil;
};
