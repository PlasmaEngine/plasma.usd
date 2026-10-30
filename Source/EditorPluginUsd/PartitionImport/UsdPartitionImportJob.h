#pragma once

#include <EditorPluginUsd/PartitionImport/UsdPartitionImportData.h>
#include <Foundation/Threading/AtomicInteger.h>
#include <Foundation/Threading/Mutex.h>
#include <Foundation/Time/Time.h>

/// \brief Everything the worker of an import needs from the main thread.
struct plUsdPartitionImportInput
{
  plUsdPartitionImportManifest m_Manifest; ///< Options, partition and import id; roots and cells of earlier runs.
  plString m_sManifestFile;
  plString m_sPartitionFolder;
  plString m_sDistantFile;  ///< '<scene>_data/<Partition> Distant.plSceneLayer'.
  plUuid m_Scene;
  plHashTable<plUInt64, plUuid> m_CellLayers; ///< The partition's cells by plUsdPartitionScan::PackCellKey.
  plHashTable<plUInt64, plString> m_CellFiles;
  plHashSet<plUuid> m_ModifiedCellLayers; ///< Loaded cells with unsaved changes; rewriting one is a CellConflict problem.
};

/// \brief How far an import got; see plUsdPartitionImportJob::GetProgress.
struct plUsdPartitionImportProgress
{
  plString m_sPhase;
  plString m_sRoot;
  plUInt32 m_uiRootsDone = 0;
  plUInt32 m_uiRoots = 0;
  plUInt32 m_uiCellsWritten = 0;
  plUInt32 m_uiCells = 0;
  plUInt32 m_uiMeshesBuilt = 0;
  plTime m_Elapsed;

  /// \brief 0 to 1, by roots while scanning and by cells while writing them.
  float GetFraction() const;
};

/// \brief Imports a USD stage as the cells of a world partition, on a worker thread that owns the stage.
///
/// Roots are scanned one at a time (plUsdPartitionScan); while a root is loaded its placeables are planned into cell
/// objects and its geometry is captured and built into mesh files on worker tasks, unless plUsdImportMeshCache has the
/// mesh from an earlier run of the same files. Each root's plan is saved, so a cancelled or failed import resumes with
/// the roots it has not done, and an update scans only roots whose layer files changed. Then every cell whose generated
/// content changed is written (plUsdPartitionCellWriter) and the manifest is saved. The main thread tells the asset
/// curator about written files (TakeWrittenFiles) and writes the materials and the lights and cameras layer once the
/// worker is done.
class PL_EDITORPLUGINUSD_DLL plUsdPartitionImportJob
{
  PL_DISALLOW_COPY_AND_ASSIGN(plUsdPartitionImportJob);

public:
  plUsdPartitionImportJob(plUsdPartitionImportInput&& input);
  ~plUsdPartitionImportJob();

  /// \brief The worker's whole run.
  void Run();

  /// \brief Stops the run at the next root, mesh batch or cell; what was done so far stays and resumes.
  void Cancel() { m_bCancel.Set(true); }
  bool IsCancelled() const { return m_bCancel; }

  void GetProgress(plUsdPartitionImportProgress& out_progress) const;

  /// \brief Files written since the last call: cell and layer files, and mesh resources whose content changed.
  void TakeWrittenFiles(plDynamicArray<plString>& out_layerFiles, plDynamicArray<plString>& out_meshResources);

  // Results, read once Run returned.
  plStatus m_Result;
  plUsdPartitionImportManifest m_Manifest;
  plHashTable<plString, plUsdSurface> m_Materials;    ///< Material resource -> surface, with the source texture files.
  plDynamicArray<plUsdImportObject> m_GlobalObjects;  ///< Lights and cameras, for the Globals layer.
  plDynamicArray<plString> m_EmptiedCells;            ///< Cell files left with no root object.
  plUInt32 m_uiObjects = 0;                           ///< Generated objects in cells and the Distant layer.
  plUInt32 m_uiCellsWritten = 0;
  plUInt32 m_uiCellsUnchanged = 0;
  plUInt32 m_uiRootsScanned = 0;
  plUInt32 m_uiRootsSkipped = 0;
  plUInt32 m_uiDistantObjects = 0;
  plUInt32 m_uiMeshesReused = 0; ///< Taken from the mesh cache instead of built.

  /// \brief The GUIDs earlier runs generated (plUsdImportObjectIndex), for plUsdPartitionCellWriter::WriteLayer; nullptr
  /// when there is no index yet.
  const plHashSet<plUuid>* GetKnownGenerated() const { return m_bHasObjectIndex ? &m_KnownGenerated : nullptr; }

private:
  struct MeshTask;
  struct RootState;

  plStatus RunInternal();
  void ResolveRoots(plDynamicArray<plString>& out_roots);
  bool IsRootCurrent(plStringView sRoot, plUsdImportRootPlan& out_plan);
  plStatus ScanAndBuildRoot(plStringView sRoot, plUsdImportRootPlan& out_plan);
  plString ResolveMaterial(plStringView sMaterialPath, bool* out_pNormalMap);
  bool ReadMaterial(plStringView sMaterialPath, plString& out_sResource);
  bool ReuseMaterials(const plUsdImportMeshCache::Entry& entry);
  bool ReuseMesh(const plString& sMesh, plUInt64 uiFileStamp, plStringView sPath);
  void BuildTasks(plDynamicArray<MeshTask>& ref_tasks);
  plStatus WriteCells(plArrayPtr<plUsdImportRootPlan> plans);
  void RemoveUnusedMeshes(plArrayPtr<const plUsdImportRootPlan> plans);
  void AddProblem(plUsdImportProblem::Type type, plStringView sPath, plStringView sMessage);
  void AppendProblems(plArrayPtr<const plUsdImportProblem> problems);
  void SetPhase(plStringView sPhase, plStringView sRoot = {});

  plUsdPartitionImportInput m_Input;
  plString m_sImportId;
  plAtomicBool m_bCancel;
  plTime m_StartTime;

  plUniquePtr<RootState> m_pState;
  plBoundingBox m_MapCentres = plBoundingBox::MakeInvalid(); ///< Centres of the cell objects of the roots done so far.
  plHashSet<plUuid> m_KnownGenerated;
  bool m_bHasObjectIndex = false;

  mutable plMutex m_Mutex;
  plDynamicArray<plUsdImportProblem>* m_pRootProblems = nullptr; ///< While a root is scanned its problems go here.
  plUsdPartitionImportProgress m_Progress;
  plDynamicArray<plString> m_WrittenLayers;
  plDynamicArray<plString> m_WrittenMeshes;
};
