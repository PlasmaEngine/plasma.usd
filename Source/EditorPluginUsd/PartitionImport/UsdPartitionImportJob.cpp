#include <EditorPluginUsd/EditorPluginUsdPCH.h>

#include <Core/Graphics/Geometry.h>
#include <EditorPluginUsd/LiveLink/UsdBakeStore.h>
#include <EditorPluginUsd/LiveLink/UsdLiveResources.h>
#include <EditorPluginUsd/PartitionImport/UsdPartitionCellWriter.h>
#include <EditorPluginUsd/PartitionImport/UsdPartitionImportJob.h>
#include <Foundation/Algorithm/HashingUtils.h>
#include <Foundation/Containers/HashSet.h>
#include <Foundation/Containers/Map.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Threading/TaskSystem.h>
#include <Foundation/Time/Timestamp.h>
#include <GameEngine/WorldStreaming/WorldPartitionGrid.h>

namespace
{
  /// Captured meshes built together on worker tasks; bounds the geometry held in memory at once.
  constexpr plUInt32 TasksPerBatch = 32;

  /// Captured points before a batch is built, whatever its task count.
  constexpr plUInt64 PointsPerBatch = 4'000'000;

  /// A split task holds its whole geometry and all its pieces while it builds: its points count this many times.
  constexpr plUInt64 SplitPointWeight = 3;

  /// A mesh covering more cells than this is not cut; it goes whole into the Distant layer.
  constexpr plUInt32 MaxSplitCells = 4096;

  constexpr plUInt32 MaxProblems = 2000;

  float ExtentXY(const plBoundingBox& bounds)
  {
    return plUsdPartitionImportOptions::GetExtentXY(bounds);
  }

  plString CellText(const plVec2I32& cell)
  {
    plStringBuilder s;
    s.SetFormat("{}_{}", cell.x, cell.y);
    return s;
  }

  plStringView LastElement(plStringView sPath)
  {
    const char* szSlash = sPath.FindLastSubString("/");
    return szSlash != nullptr ? plStringView(szSlash + 1, sPath.GetEndPointer()) : sPath;
  }

  plStringView ParentPath(plStringView sPath)
  {
    const char* szSlash = sPath.FindLastSubString("/");
    return szSlash != nullptr && szSlash > sPath.GetStartPointer() ? plStringView(sPath.GetStartPointer(), szSlash) : plStringView("/");
  }
} // namespace

float plUsdPartitionImportProgress::GetFraction() const
{
  // Scanning and building take most of an import; writing the cells the rest.
  const float fRoots = m_uiRoots > 0 ? (float)m_uiRootsDone / (float)m_uiRoots : 0.0f;
  const float fCells = m_uiCells > 0 ? (float)m_uiCellsWritten / (float)m_uiCells : 0.0f;
  return plMath::Clamp(0.9f * fRoots + 0.1f * fCells, 0.0f, 1.0f);
}

/// One mesh to build: a prototype, a loose gprim, merged gprims, or a gprim cut along the cells.
struct plUsdPartitionImportJob::MeshTask
{
  struct Piece
  {
    plVec2I32 m_Cell;
    plVec3 m_vOrigin;
    plString m_sMesh;
    plUInt32 m_uiParts = 0;
    plBoundingBox m_Bounds = plBoundingBox::MakeInvalid();
  };

  bool m_bSplit = false;
  plString m_sMesh; ///< The whole mesh; for a split task the mesh used when it cannot be cut.
  plString m_sPath;
  plString m_sName;
  plUsdCapturedMesh m_Captured;
  plDynamicArray<plString> m_Materials; ///< Material resource of each of m_Captured's groups.
  bool m_bTangents = false;
  plMat4 m_mWorld = plMat4::MakeIdentity();
  plUInt32 m_uiObject = plInvalidIndex; ///< Split: the object the pieces replace.
  plUInt64 m_uiFileStamp = 0;           ///< Kept in the mesh cache with the mesh.

  plStatus m_Result;
  bool m_bNotRun = false; ///< Cancelled before it started: nothing was written.
  plUInt32 m_uiParts = 0;
  plDynamicArray<Piece> m_Pieces;
  plString m_sNotSplit;    ///< Why a split task's mesh stayed whole.
  bool m_bDistant = false; ///< The split failed: the whole mesh goes into the Distant layer.
};

/// The worker's stage and what one run built.
struct plUsdPartitionImportJob::RootState
{
  plUsdPartitionScan m_Scan;
  plUsdScanResult m_Result;
  plHashTable<plString, plUInt32> m_MeshParts; ///< Meshes built (or failed: 0 parts) in this run.
  plHashSet<plString> m_PendingMeshes;
  plHashTable<plString, plString> m_MaterialResources; ///< Material prim path -> resource.
  plHashSet<plString> m_NormalMapped;                  ///< Material resources with a normal map.
  plDynamicArray<MeshTask> m_Tasks;
  plHashTable<plString, plDynamicArray<plString>> m_MeshMaterials; ///< Material resources of the meshes in m_MeshParts.
  plDynamicArray<plString> m_GprimMaterials;                       ///< Scratch for plUsdPartitionScan::GetGprimMaterials.

  plUsdImportMeshCache m_MeshCache;
  bool m_bMeshCacheChanged = false;
};

plUsdPartitionImportJob::plUsdPartitionImportJob(plUsdPartitionImportInput&& input)
  : m_Input(std::move(input))
{
  m_sImportId = m_Input.m_Manifest.m_sImportId;
}

plUsdPartitionImportJob::~plUsdPartitionImportJob() = default;

void plUsdPartitionImportJob::GetProgress(plUsdPartitionImportProgress& out_progress) const
{
  PL_LOCK(m_Mutex);
  out_progress = m_Progress;
  out_progress.m_Elapsed = m_StartTime.IsPositive() ? plTime::Now() - m_StartTime : plTime();
}

void plUsdPartitionImportJob::TakeWrittenFiles(plDynamicArray<plString>& out_layerFiles, plDynamicArray<plString>& out_meshResources)
{
  PL_LOCK(m_Mutex);
  out_layerFiles = std::move(m_WrittenLayers);
  out_meshResources = std::move(m_WrittenMeshes);
  m_WrittenLayers.Clear();
  m_WrittenMeshes.Clear();
}

void plUsdPartitionImportJob::SetPhase(plStringView sPhase, plStringView sRoot)
{
  PL_LOCK(m_Mutex);
  m_Progress.m_sPhase = sPhase;
  m_Progress.m_sRoot = sRoot;
}

void plUsdPartitionImportJob::AddProblem(plUsdImportProblem::Type type, plStringView sPath, plStringView sMessage)
{
  PL_LOCK(m_Mutex);
  plDynamicArray<plUsdImportProblem>& problems = m_pRootProblems != nullptr ? *m_pRootProblems : m_Manifest.m_Problems;
  if (problems.GetCount() >= MaxProblems)
    return;

  plUsdImportProblem& problem = problems.ExpandAndGetRef();
  problem.m_Type = type;
  problem.m_sPath = sPath;
  problem.m_sMessage = sMessage;
}

void plUsdPartitionImportJob::AppendProblems(plArrayPtr<const plUsdImportProblem> problems)
{
  PL_LOCK(m_Mutex);
  for (const plUsdImportProblem& problem : problems)
  {
    if (m_Manifest.m_Problems.GetCount() >= MaxProblems)
      return;
    m_Manifest.m_Problems.PushBack(problem);
  }
}

void plUsdPartitionImportJob::Run()
{
  m_StartTime = plTime::Now();
  m_Manifest = m_Input.m_Manifest;
  m_Manifest.m_Problems.Clear();
  m_Manifest.m_bComplete = false;

  // Tagged objects the earlier runs made; without the index every tagged object counts as theirs.
  m_bHasObjectIndex = plUsdImportObjectIndex::LoadGuids(plUsdImportObjectIndex::GetPath(m_Input.m_sPartitionFolder), m_KnownGenerated).Succeeded();

  m_Result = RunInternal();

  // The stage and everything captured from it go now, on the thread that owns them.
  m_pState.Clear();
}

plStatus plUsdPartitionImportJob::RunInternal()
{
  const plUsdPartitionImportOptions& options = m_Manifest.m_Options;
  m_pState = PL_DEFAULT_NEW(RootState);
  RootState& st = *m_pState;

  SetPhase("Opening the stage");
  const plString sMeshCacheFile = plUsdImportMeshCache::GetPath(m_sImportId);
  st.m_MeshCache.Load(sMeshCacheFile).IgnoreResult();
  PL_SUCCEED_OR_RETURN(st.m_Scan.Open(options.m_sStageFile));

  plDynamicArray<plString> roots;
  ResolveRoots(roots);
  if (roots.IsEmpty())
    return plStatus("The stage has no prims to import.");

  {
    PL_LOCK(m_Mutex);
    m_Progress.m_uiRoots = roots.GetCount();
  }

  // Roots of earlier runs that the stage no longer has drop out with their objects.
  {
    plDynamicArray<plUsdPartitionImportManifest::Root> kept;
    for (const plString& sRoot : roots)
    {
      plUsdPartitionImportManifest::Root* pRoot = m_Manifest.FindRoot(sRoot);
      plUsdPartitionImportManifest::Root& entry = kept.ExpandAndGetRef();
      if (pRoot != nullptr)
        entry = *pRoot;
      entry.m_sPath = sRoot;
    }
    m_Manifest.m_Roots = std::move(kept);
  }

  plDynamicArray<plUsdImportRootPlan> plans;
  plans.SetCount(roots.GetCount());
  bool bAllRoots = true;
  plString sBlockingRoot; ///< A root that failed with no earlier plan while cells exist: its content in them is unknown.

  for (plUInt32 r = 0; r < roots.GetCount(); ++r)
  {
    if (m_bCancel)
      return plStatus("The import was cancelled; it resumes where it stopped.");

    const plString& sRoot = roots[r];
    plUsdPartitionImportManifest::Root& entry = m_Manifest.m_Roots[r];
    const plString sPlanFile = plUsdPartitionImportData::GetRootPlanFile(m_sImportId, sRoot);

    if (IsRootCurrent(sRoot, plans[r]))
    {
      ++m_uiRootsSkipped;
      AppendProblems(plans[r].m_Problems);
    }
    else
    {
      entry.m_bDone = false;
      SetPhase("Scanning", sRoot);

      plDynamicArray<plUsdImportProblem> rootProblems;
      {
        PL_LOCK(m_Mutex);
        m_pRootProblems = &rootProblems;
      }

      const plInt64 iScanStart = plTimestamp::CurrentTimestamp().GetInt64(plSIUnitOfTime::Microsecond);
      const plStatus res = ScanAndBuildRoot(sRoot, plans[r]);
      bool bLayersUnchanged = true;
      if (res.Succeeded())
        bLayersUnchanged = plUsdBakeStore::RecordLayers(st.m_Scan.GetStage(), plans[r].m_Layers, iScanStart);

      st.m_Scan.ReleaseRoot(sRoot);
      st.m_Result.ReleasePlaceables();

      {
        PL_LOCK(m_Mutex);
        m_pRootProblems = nullptr;
      }

      // Also after a failed root: the meshes it built are on disk.
      if (st.m_bMeshCacheChanged)
      {
        st.m_bMeshCacheChanged = false;
        st.m_MeshCache.Save(sMeshCacheFile).LogFailure();
      }

      if (res.Failed())
      {
        if (m_bCancel)
          return plStatus("The import was cancelled; it resumes where it stopped.");

        plLog::Error("USD Import: '{}' could not be imported: {}", sRoot, res.m_sMessage);
        AddProblem(plUsdImportProblem::Type::RootFailed, sRoot, res.m_sMessage);
        bAllRoots = false;

        // Its earlier plan stays in the cells until the root scans again; entry.m_bDone stays false, so the next run retries.
        plUsdImportRootPlan& plan = plans[r];
        if (plan.Load(sPlanFile).Succeeded() && plan.m_sRootPath == sRoot)
        {
          plLog::Warning("USD Import: '{}' keeps what an earlier import made of it.", sRoot);
          AppendProblems(plan.m_Problems);
        }
        else
        {
          plan = plUsdImportRootPlan();
          if (!m_Manifest.m_Cells.IsEmpty() && sBlockingRoot.IsEmpty())
            sBlockingRoot = sRoot;
        }
        continue;
      }

      plans[r].m_Problems = std::move(rootProblems);
      AppendProblems(plans[r].m_Problems);

      const plStatus saved = plans[r].Save(sPlanFile);
      if (saved.Failed())
        plLog::Warning("USD Import: {}", saved.m_sMessage);
      if (!bLayersUnchanged)
        plLog::Warning("USD Import: a layer of '{}' changed while it was imported; the next update scans it again.", sRoot);

      entry.m_bDone = saved.Succeeded() && bLayersUnchanged;
      ++m_uiRootsScanned;
      m_Manifest.Save(m_Input.m_sManifestFile).LogFailure();
    }

    // The map so far, for keeping map-sized meshes of later roots whole.
    for (const plUsdImportObject& o : plans[r].m_Objects)
    {
      if (o.m_Target == plUsdImportTarget::Cell && o.m_Bounds.IsValid())
        m_MapCentres.ExpandToInclude(o.m_Bounds.GetCenter());
    }

    PL_LOCK(m_Mutex);
    ++m_Progress.m_uiRootsDone;
  }

  if (m_bCancel)
    return plStatus("The import was cancelled; it resumes where it stopped.");

  if (!sBlockingRoot.IsEmpty())
  {
    m_Manifest.Save(m_Input.m_sManifestFile).LogFailure();
    return plStatus(plFmt("'{}' could not be scanned and no earlier plan of it is left, so no cell was changed. Fix or exclude it and update again.",
      sBlockingRoot));
  }

  PL_SUCCEED_OR_RETURN(WriteCells(plans));

  if (bAllRoots)
    RemoveUnusedMeshes(plans);

  return plStatus(PL_SUCCESS);
}

void plUsdPartitionImportJob::ResolveRoots(plDynamicArray<plString>& out_roots)
{
  m_Manifest.m_Options.ResolveRoots(m_pState->m_Scan, out_roots);
}

bool plUsdPartitionImportJob::IsRootCurrent(plStringView sRoot, plUsdImportRootPlan& out_plan)
{
  plUsdPartitionImportManifest::Root* pEntry = m_Manifest.FindRoot(sRoot);
  if (pEntry == nullptr || !pEntry->m_bDone)
    return false;

  if (out_plan.Load(plUsdPartitionImportData::GetRootPlanFile(m_sImportId, sRoot)).Failed())
    return false;

  if (out_plan.m_sRootPath != sRoot || out_plan.m_uiOptionsHash != m_Manifest.m_Options.GetPlanHash() || !plUsdBakeStore::AreLayersCurrent(out_plan.m_Layers))
    return false;

  // Materials are written when an import finishes; one that stopped earlier left them out.
  const plString sDefault = plUsdPartitionImportData::GetDefaultMaterialResource(m_sImportId);
  for (const plString& sMaterial : out_plan.m_Materials)
  {
    if (sMaterial != sDefault && !plOSFile::ExistsFile(plUsdBakeStore::ToAbsolutePath(sMaterial)))
    {
      plLog::Info("USD Import: materials of '{}' are missing; it is scanned again.", sRoot);
      return false;
    }
  }

  // Its meshes must still be there; a cleared mesh folder rebuilds them.
  plHashSet<plString> checked;
  for (const plUsdImportObject& o : out_plan.m_Objects)
  {
    if (o.m_sMesh.IsEmpty() || checked.Contains(o.m_sMesh))
      continue;

    checked.Insert(o.m_sMesh);
    if (!plUsdBakeStore::HasMeshFiles(o.m_sMesh, o.m_uiParts))
    {
      plLog::Info("USD Import: meshes of '{}' are missing; it is scanned again.", sRoot);
      return false;
    }
  }

  return true;
}

plString plUsdPartitionImportJob::ResolveMaterial(plStringView sMaterialPath, bool* out_pNormalMap)
{
  RootState& st = *m_pState;

  plString sResource;
  if (!st.m_MaterialResources.TryGetValue(sMaterialPath, sResource))
  {
    if (!ReadMaterial(sMaterialPath, sResource))
      sResource = plUsdPartitionImportData::GetDefaultMaterialResource(m_sImportId);

    st.m_MaterialResources.Insert(sMaterialPath, sResource);
  }

  if (out_pNormalMap != nullptr)
    *out_pNormalMap |= st.m_NormalMapped.Contains(sResource);

  return sResource;
}

bool plUsdPartitionImportJob::ReadMaterial(plStringView sMaterialPath, plString& out_sResource)
{
  RootState& st = *m_pState;

  plUsdSurface surface;
  if (sMaterialPath.IsEmpty() || st.m_Scan.GetStage().ReadMaterial(sMaterialPath, surface).Failed())
    return false;

  out_sResource = plUsdPartitionImportData::GetMaterialResource(m_sImportId, sMaterialPath);
  if (!surface.m_sNormalTexture.IsEmpty())
    st.m_NormalMapped.Insert(out_sResource);
  m_Materials[out_sResource] = surface;
  return true;
}

bool plUsdPartitionImportJob::ReuseMaterials(const plUsdImportMeshCache::Entry& entry)
{
  RootState& st = *m_pState;
  if (entry.m_MaterialPaths.GetCount() != entry.m_MaterialResources.GetCount())
    return false;

  const plString sDefault = plUsdPartitionImportData::GetDefaultMaterialResource(m_sImportId);
  for (plUInt32 i = 0; i < entry.m_MaterialPaths.GetCount(); ++i)
  {
    const plString& sPath = entry.m_MaterialPaths[i];
    const plString& sBuiltWith = entry.m_MaterialResources[i];

    // Resolved again where the stage has it, so its file is written with this import's materials.
    plString sResource;
    if (!st.m_MaterialResources.TryGetValue(sPath, sResource) && ReadMaterial(sPath, sResource))
      st.m_MaterialResources.Insert(sPath, sResource);

    if (!sResource.IsEmpty())
    {
      if (sResource != sBuiltWith)
        return false;
      continue;
    }

    // Not on the loaded stage: what the mesh was built with must be on disk already.
    if (sBuiltWith != sDefault && !plOSFile::ExistsFile(plUsdBakeStore::ToAbsolutePath(sBuiltWith)))
      return false;
  }
  return true;
}

bool plUsdPartitionImportJob::ReuseMesh(const plString& sMesh, plUInt64 uiFileStamp, plStringView sPath)
{
  RootState& st = *m_pState;

  // Mesh names come from the specs the geometry is composed from; the file stamp tells whether those files changed.
  const plUsdImportMeshCache::Entry* pEntry = st.m_MeshCache.m_Meshes.GetValue(sMesh);
  if (pEntry == nullptr || pEntry->m_uiFileStamp != uiFileStamp || !plUsdBakeStore::HasMeshFiles(sMesh, pEntry->m_uiParts) || !ReuseMaterials(*pEntry))
    return false;

  st.m_MeshParts[sMesh] = pEntry->m_uiParts;
  st.m_MeshMaterials[sMesh] = pEntry->m_MaterialResources;
  ++m_uiMeshesReused;

  if (pEntry->m_uiSkipped > 0)
  {
    plStringBuilder sMessage;
    sMessage.SetFormat("{} gprim(s) left out of the mesh, first {}", pEntry->m_uiSkipped, pEntry->m_sSkipped);
    AddProblem(plUsdImportProblem::Type::MeshFailed, sPath, sMessage);
  }
  return true;
}

void plUsdPartitionImportJob::BuildTasks(plDynamicArray<MeshTask>& ref_tasks)
{
  if (ref_tasks.IsEmpty())
    return;

  {
    PL_LOCK(m_Mutex);
    m_Progress.m_sPhase = "Building meshes";
  }

  const float fCellSize = m_Manifest.m_Options.m_fCellSize;
  const plString sImportId = m_sImportId;

  // Files that changed are reloaded by the engine; new ones it has never loaded.
  auto writeParts = [this](plStringView sMesh, const plDynamicArray<plDynamicArray<plUInt8>>& parts)
  {
    for (plUInt32 uiPart = 0; uiPart < parts.GetCount(); ++uiPart)
    {
      const plString sResource = plUsdPartitionImportData::GetMeshResource(sMesh, uiPart);
      const bool bExisted = plOSFile::ExistsFile(plUsdBakeStore::ToAbsolutePath(sResource));
      bool bWritten = false;
      if (plUsdBakeStore::WriteFile(sResource, parts[uiPart], &bWritten).Failed())
        return plStatus(plFmt("could not write '{}'", sResource));

      if (bWritten && bExisted)
      {
        PL_LOCK(m_Mutex);
        m_WrittenMeshes.PushBack(sResource);
      }
    }
    return plStatus(PL_SUCCESS);
  };

  plParallelForParams params;
  params.m_uiBinSize = 1;
  params.m_uiMaxTasksPerThread = 2;
  params.m_NestingMode = plTaskNesting::Maybe;

  plTaskSystem::ParallelForSingle(
    ref_tasks.GetArrayPtr(),
    [&](MeshTask& task)
    {
      if (m_bCancel)
      {
        task.m_Result = plStatus("cancelled");
        task.m_bNotRun = true;
        return;
      }

      plDynamicArray<plDynamicArray<plUInt8>> parts;

      if (!task.m_bSplit)
      {
        task.m_Result = plUsdLiveResources::BuildMeshParts(*task.m_Captured.m_pSource, task.m_Materials, plHashingUtils::xxHash64String(task.m_sMesh), task.m_bTangents, parts);
        if (task.m_Result.Succeeded())
          task.m_Result = writeParts(task.m_sMesh, parts);
        task.m_uiParts = task.m_Result.Succeeded() ? parts.GetCount() : 0;
        return;
      }

      plGeometry geom;
      plDynamicArray<plUInt32> groups;
      task.m_Result = task.m_Captured.m_pSource->Build(geom, &groups);
      if (task.m_Result.Failed())
        return;

      plDynamicArray<plUsdCellPiece> pieces;
      const plStatus split = plUsdPartitionScan::SplitByCell(geom, groups, task.m_mWorld, fCellSize, MaxSplitCells, pieces);
      if (split.Failed() || pieces.IsEmpty())
      {
        // Whole, at the gprim's own transform; one that cannot be cut goes into the Distant layer.
        task.m_sNotSplit = split.Failed() ? split.m_sMessage : plString("nothing to cut");
        task.m_bDistant = split.Failed();
        task.m_Result = plUsdLiveResources::BuildMeshPartsFromGeometry(geom, groups, task.m_Materials, plHashingUtils::xxHash64String(task.m_sMesh), task.m_bTangents, parts);
        if (task.m_Result.Succeeded())
          task.m_Result = writeParts(task.m_sMesh, parts);
        task.m_uiParts = task.m_Result.Succeeded() ? parts.GetCount() : 0;
        return;
      }

      geom.Clear();
      plStringBuilder sPieceName;
      for (plUsdCellPiece& piece : pieces)
      {
        // Clipping can leave a cell with only degenerate slivers; that cell simply gets no piece.
        if (piece.m_Geometry.GetPolygons().IsEmpty())
          continue;

        MeshTask::Piece& out = task.m_Pieces.ExpandAndGetRef();
        out.m_Cell = piece.m_Cell;
        out.m_vOrigin = piece.m_vOrigin;

        for (const plGeometry::Vertex& v : piece.m_Geometry.GetVertices())
          out.m_Bounds.ExpandToInclude(v.m_vPosition + piece.m_vOrigin);

        sPieceName.SetFormat("{}_{}_{}", task.m_sName, piece.m_Cell.x, piece.m_Cell.y);
        plStringBuilder sKey(task.m_sMesh, "#", CellText(piece.m_Cell));
        out.m_sMesh = plUsdPartitionImportData::GetMeshName(sImportId, sPieceName, plHashingUtils::xxHash64String(sKey));

        parts.Clear();
        plStatus res = plUsdLiveResources::BuildMeshPartsFromGeometry(piece.m_Geometry, piece.m_GroupTriangleCounts, task.m_Materials,
          plHashingUtils::xxHash64String(out.m_sMesh), task.m_bTangents, parts);
        if (res.Succeeded())
          res = writeParts(out.m_sMesh, parts);

        out.m_uiParts = res.Succeeded() ? parts.GetCount() : 0;
        if (res.Failed())
          task.m_Result = res;

        piece.m_Geometry.Clear();
      }
    },
    "USD Import Meshes", params);

  RootState& st = *m_pState;
  for (MeshTask& task : ref_tasks)
  {
    st.m_PendingMeshes.Remove(task.m_sMesh);
    st.m_MeshParts[task.m_sMesh] = task.m_uiParts;
    st.m_MeshMaterials[task.m_sMesh] = task.m_Materials;

    // Split pieces are not cached: their names come from the cut, which runs whenever the gprim is planned. A failed
    // build may have overwritten some parts, so its entry goes.
    if (!task.m_bSplit && task.m_Result.Succeeded() && task.m_uiParts > 0)
    {
      plUsdImportMeshCache::Entry& entry = st.m_MeshCache.m_Meshes[task.m_sMesh];
      entry.m_uiParts = task.m_uiParts;
      entry.m_uiFileStamp = task.m_uiFileStamp;
      entry.m_uiSkipped = task.m_Captured.m_uiSkipped;
      entry.m_sSkipped = task.m_Captured.m_sSkipped;
      entry.m_MaterialPaths = task.m_Captured.m_Materials;
      entry.m_MaterialResources = task.m_Materials;
      st.m_bMeshCacheChanged = true;
    }
    else if (!task.m_bSplit && !task.m_bNotRun && st.m_MeshCache.m_Meshes.Remove(task.m_sMesh))
    {
      st.m_bMeshCacheChanged = true;
    }

    if (task.m_Result.Failed() && !m_bCancel)
      AddProblem(plUsdImportProblem::Type::MeshFailed, task.m_sPath, task.m_Result.m_sMessage);
    if (!task.m_sNotSplit.IsEmpty())
      AddProblem(plUsdImportProblem::Type::NotSplit, task.m_sPath, task.m_sNotSplit);
    if (task.m_bDistant)
      AddProblem(plUsdImportProblem::Type::Distant, task.m_sPath, "could not be cut along the cells");

    for (const MeshTask::Piece& piece : task.m_Pieces)
    {
      st.m_MeshParts[piece.m_sMesh] = piece.m_uiParts;
      st.m_MeshMaterials[piece.m_sMesh] = task.m_Materials;
    }

    PL_LOCK(m_Mutex);
    m_Progress.m_uiMeshesBuilt += task.m_bSplit && task.m_sNotSplit.IsEmpty() ? task.m_Pieces.GetCount() : 1;
  }

  // The captured geometry goes now; the split results are read by the caller.
  for (MeshTask& task : ref_tasks)
    task.m_Captured = {};
}

plStatus plUsdPartitionImportJob::ScanAndBuildRoot(plStringView sRoot, plUsdImportRootPlan& out_plan)
{
  RootState& st = *m_pState;
  const plUsdPartitionImportOptions& options = m_Manifest.m_Options;

  out_plan = plUsdImportRootPlan();
  out_plan.m_sRootPath = sRoot;
  out_plan.m_sRootName = LastElement(sRoot);
  out_plan.m_uiOptionsHash = options.GetPlanHash();

  plUsdScanOptions scanOptions;
  options.MakeScanOptions(sRoot, st.m_Scan, scanOptions);
  scanOptions.m_pCancel = &m_bCancel;
  scanOptions.m_OnProgress = [this](bool bLoading, plUInt32 uiPrims)
  {
    plStringBuilder s;
    if (bLoading)
      s = "Loading";
    else
      s.SetFormat("Scanning ({} prims)", uiPrims);
    PL_LOCK(m_Mutex);
    m_Progress.m_sPhase = s;
  };

  const plUInt32 uiRoot = st.m_Result.m_Roots.GetCount();
  PL_SUCCEED_OR_RETURN(st.m_Scan.ScanRoot(sRoot, scanOptions, st.m_Result));

  const plUsdScanResult& result = st.m_Result;
  const plUsdScanRoot& root = result.m_Roots[uiRoot];
  out_plan.m_uiPlaceables = root.m_uiPlaceableCount;

  const float fCellSize = options.m_fCellSize;
  const float fSplitSize = options.GetSplitSize();
  const float fBatchSize = options.m_fBatchSize;
  const plUsdBasis& basis = st.m_Scan.GetBasis();

  plDynamicArray<MeshTask>& tasks = st.m_Tasks;
  tasks.Clear();
  plUInt64 uiPendingPoints = 0; ///< Captured by the tasks, see SplitPointWeight.

  auto flush = [&](bool bForce)
  {
    if (tasks.GetCount() >= TasksPerBatch || uiPendingPoints >= PointsPerBatch || (bForce && !tasks.IsEmpty()))
    {
      uiPendingPoints = 0;
      BuildTasks(tasks);

      // Split results replace their placeholder objects.
      for (MeshTask& task : tasks)
      {
        if (task.m_bSplit && task.m_bDistant)
          out_plan.m_Objects[task.m_uiObject].m_Target = plUsdImportTarget::Distant;

        if (!task.m_bSplit || !task.m_sNotSplit.IsEmpty() || task.m_Pieces.IsEmpty())
          continue;

        for (const MeshTask::Piece& piece : task.m_Pieces)
        {
          plUsdImportObject& o = out_plan.m_Objects.ExpandAndGetRef();
          plUsdImportObject& source = out_plan.m_Objects[task.m_uiObject];
          o.m_sKey = plStringBuilder(source.m_sKey, "#piece/", CellText(piece.m_Cell));
          o.m_sName = source.m_sName;
          o.m_sPrimPath = source.m_sPrimPath;
          o.m_Cell = piece.m_Cell;
          o.m_Transform = plTransform::Make(piece.m_vOrigin);
          o.m_Bounds = piece.m_Bounds;
          o.m_sMesh = piece.m_sMesh;
        }
        // Marked for removal: no mesh.
        out_plan.m_Objects[task.m_uiObject].m_sMesh.Clear();
      }

      tasks.Clear();
    }
  };

  // Resolves the captured materials and counts the captured points towards the batch.
  auto captureMaterials = [&](MeshTask& task)
  {
    for (const plString& sMaterial : task.m_Captured.m_Materials)
      task.m_Materials.PushBack(ResolveMaterial(sMaterial, &task.m_bTangents));

    if (task.m_Captured.m_uiSkipped > 0)
    {
      plStringBuilder sMessage;
      sMessage.SetFormat("{} gprim(s) left out of the mesh, first {}", task.m_Captured.m_uiSkipped, task.m_Captured.m_sSkipped);
      AddProblem(plUsdImportProblem::Type::MeshFailed, task.m_sPath, sMessage);
    }

    const plUInt64 uiPoints = task.m_Captured.m_pSource != nullptr ? task.m_Captured.m_pSource->GetPointCount() : 0;
    uiPendingPoints += task.m_bSplit ? SplitPointWeight * uiPoints : uiPoints;
  };

  // The name of a loose gprim mesh: its geometry key, or path, and the materials of its groups.
  auto gprimMeshKey = [&](plArrayPtr<const plStringView> paths, plUInt64 uiKey)
  {
    st.m_Scan.GetGprimMaterials(paths, st.m_GprimMaterials);
    for (const plString& sMaterial : st.m_GprimMaterials)
      uiKey = plHashingUtils::xxHash64String(ResolveMaterial(sMaterial, nullptr), uiKey);
    return uiKey;
  };

  // The map known so far: object centres of earlier roots and placeable centres of this one. A loose mesh wider than
  // that goes whole into the Distant layer instead of being cut into cells.
  float fMapEstimate = 0.0f;
  {
    plBoundingBox centres = m_MapCentres;
    for (plUInt32 i = root.m_uiFirstPlaceable; i < root.m_uiFirstPlaceable + root.m_uiPlaceableCount; ++i)
    {
      const plUsdPlaceable& p = result.m_Placeables[i];
      if (p.m_Kind != plUsdPlaceableKind::Light && p.m_Kind != plUsdPlaceableKind::Camera && p.m_WorldBounds.IsValid())
        centres.ExpandToInclude(p.m_WorldBounds.GetCenter());
    }
    fMapEstimate = options.GetMapExtent(centres);
  }

  struct Batch
  {
    plVec2I32 m_Cell;
    plUInt32 m_uiPrototype = plInvalidIndex;
    plString m_sParent;
    plDynamicArray<plUInt32> m_Placeables;
    plBoundingBox m_Bounds = plBoundingBox::MakeInvalid();
  };
  plMap<plString, Batch> instanceBatches;
  plMap<plString, Batch> gprimBatches;

  plStringBuilder sKey;

  for (plUInt32 i = root.m_uiFirstPlaceable; i < root.m_uiFirstPlaceable + root.m_uiPlaceableCount; ++i)
  {
    if (m_bCancel)
      return plStatus("cancelled");

    const plUsdPlaceable& p = result.m_Placeables[i];
    const plStringView sPath = result.GetPath(p);

    if (p.m_Kind == plUsdPlaceableKind::Light || p.m_Kind == plUsdPlaceableKind::Camera)
    {
      plUsdPrim prim;
      if (st.m_Scan.GetStage().ReadPrim(sPath, prim).Failed())
        continue;

      plUsdImportObject& o = out_plan.m_Objects.ExpandAndGetRef();
      o.m_sKey = sPath;
      o.m_sName = LastElement(sPath);
      o.m_sPrimPath = sPath;
      o.m_Kind = p.m_Kind == plUsdPlaceableKind::Light ? plUsdImportObjectKind::Light : plUsdImportObjectKind::Camera;
      o.m_Target = plUsdImportTarget::Globals;
      o.m_Light = prim.m_Light;
      o.m_Camera = prim.m_Camera;

      // Faces the way the prim does, as the live link's light and camera objects.
      const plMat4d mLook(basis.GetLookCorrection(), plVec3d::MakeZero());
      o.m_Transform = plUsdBasis::MatrixToTransform(plUsdBasis::TransformToMatrix(p.m_WorldTransform) * mLook);
      o.m_Bounds = plBoundingBox::MakeFromCenterAndHalfExtents(o.m_Transform.m_vPosition, plVec3(0.5f));
      continue;
    }

    if (!p.m_WorldBounds.IsValid())
    {
      ++out_plan.m_uiWithoutBounds;
      continue;
    }

    const plVec3 vCentre = p.m_WorldBounds.GetCenter();
    if (!options.IsInRegion(vCentre))
    {
      ++out_plan.m_uiOutsideRegion;
      continue;
    }

    const plVec2I32 cell = plUsdPartitionScan::CellOf(vCentre, fCellSize);
    const float fExtent = ExtentXY(p.m_WorldBounds);

    if (p.m_Kind == plUsdPlaceableKind::Instance || p.m_Kind == plUsdPlaceableKind::PointInstance)
    {
      const plUsdScanPrototype& proto = result.m_Prototypes[p.m_uiPrototype];

      if (fBatchSize > 0.0f && fExtent < fBatchSize)
      {
        sKey.SetFormat("{}/{}", plArgU(proto.m_uiKey, 16, true, 16), CellText(cell));
        Batch& batch = instanceBatches[sKey];
        batch.m_Cell = cell;
        batch.m_uiPrototype = p.m_uiPrototype;
        batch.m_Placeables.PushBack(i);
        batch.m_Bounds.ExpandToInclude(p.m_WorldBounds);
        continue;
      }

      plUsdImportObject& o = out_plan.m_Objects.ExpandAndGetRef();
      if (p.m_Kind == plUsdPlaceableKind::PointInstance)
      {
        sKey.SetFormat("{}#{}", sPath, p.m_uiElement);
        o.m_sKey = sKey;
      }
      else
        o.m_sKey = sPath;
      o.m_sName = LastElement(sPath);
      o.m_sPrimPath = sPath;
      o.m_Cell = cell;
      o.m_Transform = p.m_WorldTransform;
      o.m_Bounds = p.m_WorldBounds;
      o.m_sMesh = plUsdPartitionImportData::GetMeshName(m_sImportId, proto.m_sName, proto.m_uiKey);

      if (fExtent > fSplitSize)
      {
        o.m_bOversized = true;
        sKey.SetFormat("{} m wide, placed by its centre", plArgF(fExtent, 0));
        AddProblem(plUsdImportProblem::Type::Oversized, sPath, sKey);
      }
      continue;
    }

    // A loose gprim.
    const plUsdPartitionImportOptions::LooseMeshPlacement placement = options.PlaceLooseMesh(fExtent, fMapEstimate);
    const bool bDistant = placement == plUsdPartitionImportOptions::LooseMeshPlacement::Distant;
    if (placement == plUsdPartitionImportOptions::LooseMeshPlacement::Split)
    {
      MeshTask& task = tasks.ExpandAndGetRef();
      task.m_bSplit = true;
      task.m_sPath = sPath;
      task.m_sName = LastElement(sPath);
      task.m_mWorld = p.m_WorldTransform.GetAsMat4();

      const plStatus res = st.m_Scan.CaptureGprim(sPath, task.m_Captured);
      if (res.Failed())
      {
        AddProblem(plUsdImportProblem::Type::MeshFailed, sPath, res.m_sMessage);
        tasks.PopBack();
        continue;
      }
      captureMaterials(task);

      sKey.SetFormat("{}#split", sPath);
      task.m_sMesh = plUsdPartitionImportData::GetMeshName(m_sImportId, task.m_sName, plHashingUtils::xxHash64String(sKey, p.m_uiGeometryKey));

      task.m_uiObject = out_plan.m_Objects.GetCount();
      plUsdImportObject& o = out_plan.m_Objects.ExpandAndGetRef();
      o.m_sKey = sPath;
      o.m_sName = task.m_sName;
      o.m_sPrimPath = sPath;
      o.m_Cell = cell;
      o.m_Transform = p.m_WorldTransform;
      o.m_Bounds = p.m_WorldBounds;
      o.m_sMesh = task.m_sMesh;
      st.m_PendingMeshes.Insert(task.m_sMesh);

      flush(false);
      continue;
    }

    if (fBatchSize > 0.0f && fExtent < fBatchSize)
    {
      const plStringView sParent = ParentPath(sPath);
      sKey.SetFormat("{}#merged/{}", sParent, CellText(cell));
      Batch& batch = gprimBatches[sKey];
      batch.m_Cell = cell;
      batch.m_sParent = sParent;
      batch.m_Placeables.PushBack(i);
      batch.m_Bounds.ExpandToInclude(p.m_WorldBounds);
      continue;
    }

    // On its own: shares its mesh with every gprim of the same geometry and materials; captured only when it is built.
    {
      const plUInt64 uiKey = gprimMeshKey(plArrayPtr<const plStringView>(&sPath, 1), p.m_uiGeometryKey != 0 ? p.m_uiGeometryKey : plHashingUtils::xxHash64String(sPath));
      const plString sMesh = plUsdPartitionImportData::GetMeshName(m_sImportId, LastElement(sPath), uiKey);

      plUsdImportObject& o = out_plan.m_Objects.ExpandAndGetRef();
      o.m_sKey = sPath;
      o.m_sName = LastElement(sPath);
      o.m_sPrimPath = sPath;
      o.m_Cell = cell;
      o.m_Transform = p.m_WorldTransform;
      o.m_Bounds = p.m_WorldBounds;
      o.m_sMesh = sMesh;

      if (bDistant)
      {
        o.m_Target = plUsdImportTarget::Distant;
        sKey.SetFormat("{} m wide, larger than the map ({} m so far)", plArgF(fExtent, 0), plArgF(fMapEstimate, 0));
        AddProblem(plUsdImportProblem::Type::Distant, sPath, sKey);
      }

      // A name from the path alone does not change with the composition, so only keyed meshes are reused.
      if (st.m_MeshParts.Contains(sMesh) || st.m_PendingMeshes.Contains(sMesh) || (p.m_uiGeometryKey != 0 && ReuseMesh(sMesh, p.m_uiFileStamp, sPath)))
        continue;

      MeshTask task;
      task.m_sPath = sPath;
      task.m_sMesh = sMesh;
      task.m_uiFileStamp = p.m_uiFileStamp;
      const plStatus res = st.m_Scan.CaptureGprim(sPath, task.m_Captured);
      if (res.Failed())
      {
        AddProblem(plUsdImportProblem::Type::MeshFailed, sPath, res.m_sMessage);
        st.m_MeshParts[sMesh] = 0;
        continue;
      }

      captureMaterials(task);
      st.m_PendingMeshes.Insert(sMesh);
      tasks.PushBack(std::move(task));
      flush(false);
    }
  }

  // Prototypes this root draws, built once per run from their source in this root.
  {
    plHashSet<plUInt32> used;
    for (plUInt32 i = root.m_uiFirstPlaceable; i < root.m_uiFirstPlaceable + root.m_uiPlaceableCount; ++i)
    {
      if (result.m_Placeables[i].m_uiPrototype != plInvalidIndex)
        used.Insert(result.m_Placeables[i].m_uiPrototype);
    }

    plDynamicArray<plUInt32> sorted;
    for (plUInt32 uiProto : used)
      sorted.PushBack(uiProto);
    sorted.Sort();

    for (plUInt32 uiProto : sorted)
    {
      if (m_bCancel)
        return plStatus("cancelled");

      const plUsdScanPrototype& proto = result.m_Prototypes[uiProto];
      const plString sMesh = plUsdPartitionImportData::GetMeshName(m_sImportId, proto.m_sName, proto.m_uiKey);
      if (st.m_MeshParts.Contains(sMesh) || st.m_PendingMeshes.Contains(sMesh))
        continue;

      const plString sSourcePath = proto.FindSource(uiRoot) != nullptr ? proto.FindSource(uiRoot)->m_sPath : proto.m_sName;
      if (ReuseMesh(sMesh, proto.m_uiFileStamp, sSourcePath))
        continue;

      MeshTask& task = tasks.ExpandAndGetRef();
      task.m_sMesh = sMesh;
      task.m_sPath = sSourcePath;
      task.m_uiFileStamp = proto.m_uiFileStamp;

      const plStatus res = st.m_Scan.CapturePrototype(proto, uiRoot, task.m_Captured);
      if (res.Failed())
      {
        AddProblem(plUsdImportProblem::Type::MeshFailed, task.m_sPath, res.m_sMessage);
        st.m_MeshParts[sMesh] = 0;
        tasks.PopBack();
        continue;
      }

      captureMaterials(task);
      st.m_PendingMeshes.Insert(sMesh);
      flush(false);
    }
  }

  // Small instances: one instanced object per prototype and cell.
  for (auto it = instanceBatches.GetIterator(); it.IsValid(); ++it)
  {
    const Batch& batch = it.Value();
    const plUsdScanPrototype& proto = result.m_Prototypes[batch.m_uiPrototype];

    plVec3 vCentre = plVec3::MakeZero();
    for (plUInt32 i : batch.m_Placeables)
      vCentre += result.m_Placeables[i].m_WorldTransform.m_vPosition;
    vCentre /= (float)batch.m_Placeables.GetCount();

    plUsdImportObject& o = out_plan.m_Objects.ExpandAndGetRef();
    o.m_sKey = plStringBuilder(sRoot, "#batch/", it.Key());
    plStringBuilder sName;
    sName.SetFormat("{} x {}", proto.m_sName, batch.m_Placeables.GetCount());
    o.m_sName = sName;
    o.m_sPrimPath = sRoot;
    o.m_Kind = plUsdImportObjectKind::Instances;
    o.m_Cell = batch.m_Cell;
    o.m_Transform = plTransform::Make(vCentre);
    o.m_Bounds = batch.m_Bounds;
    o.m_sMesh = plUsdPartitionImportData::GetMeshName(m_sImportId, proto.m_sName, proto.m_uiKey);

    o.m_Instances.Reserve(batch.m_Placeables.GetCount());
    for (plUInt32 i : batch.m_Placeables)
    {
      plTransform t = result.m_Placeables[i].m_WorldTransform;
      t.m_vPosition -= vCentre;
      o.m_Instances.PushBack(t);
    }
  }

  // Small loose gprims: one merged mesh per parent prim and cell, around the group's centre.
  for (auto it = gprimBatches.GetIterator(); it.IsValid(); ++it)
  {
    if (m_bCancel)
      return plStatus("cancelled");

    const Batch& batch = it.Value();
    const plVec3 vCentre = batch.m_Bounds.GetCenter();
    const plMat4 mToFrame = plMat4::MakeTranslation(-vCentre);

    plDynamicArray<plStringView> paths;
    plDynamicArray<plMat4> transforms;
    plUInt64 uiKey = plHashingUtils::xxHash64String(it.Key());
    plUInt64 uiFileStamp = 0;
    bool bAllKeyed = true;
    for (plUInt32 i : batch.m_Placeables)
    {
      const plUsdPlaceable& p = result.m_Placeables[i];
      bAllKeyed &= p.m_uiGeometryKey != 0;
      paths.PushBack(result.GetPath(p));
      transforms.PushBack(mToFrame * p.m_WorldTransform.GetAsMat4());
      uiKey = plHashingUtils::xxHash64(&p.m_uiGeometryKey, sizeof(plUInt64), uiKey);
      uiKey = plHashingUtils::xxHash64(&p.m_WorldTransform, sizeof(plTransform), uiKey);
      uiFileStamp = plHashingUtils::xxHash64(&p.m_uiFileStamp, sizeof(plUInt64), uiFileStamp);
    }
    uiKey = gprimMeshKey(paths, uiKey);

    const plStringView sParentName = LastElement(batch.m_sParent);
    const plString sMesh = plUsdPartitionImportData::GetMeshName(m_sImportId, sParentName, uiKey);

    plUsdImportObject& o = out_plan.m_Objects.ExpandAndGetRef();
    o.m_sKey = it.Key();
    o.m_sName = sParentName;
    o.m_sPrimPath = batch.m_sParent;
    o.m_Cell = batch.m_Cell;
    o.m_Transform = plTransform::Make(vCentre);
    o.m_Bounds = batch.m_Bounds;
    o.m_sMesh = sMesh;

    if (st.m_MeshParts.Contains(sMesh) || st.m_PendingMeshes.Contains(sMesh) || (bAllKeyed && ReuseMesh(sMesh, uiFileStamp, batch.m_sParent)))
      continue;

    MeshTask task;
    task.m_sPath = batch.m_sParent;
    task.m_sMesh = sMesh;
    task.m_uiFileStamp = uiFileStamp;
    const plStatus res = st.m_Scan.CaptureGprims(paths, transforms, task.m_Captured);
    if (res.Failed())
    {
      AddProblem(plUsdImportProblem::Type::MeshFailed, batch.m_sParent, res.m_sMessage);
      st.m_MeshParts[sMesh] = 0;
      continue;
    }

    captureMaterials(task);
    st.m_PendingMeshes.Insert(sMesh);
    tasks.PushBack(std::move(task));
    flush(false);
  }

  flush(true);

  if (m_bCancel)
    return plStatus("cancelled");

  // Mesh parts for every object, and the materials of the meshes; objects whose mesh failed are left out.
  plDynamicArray<plUsdImportObject> objects;
  objects.Reserve(out_plan.m_Objects.GetCount());
  plHashSet<plString> materials;
  for (plUsdImportObject& o : out_plan.m_Objects)
  {
    if (o.m_Kind == plUsdImportObjectKind::Light || o.m_Kind == plUsdImportObjectKind::Camera)
    {
      objects.PushBack(std::move(o));
      continue;
    }

    plUInt32 uiParts = 0;
    if (o.m_sMesh.IsEmpty() || !st.m_MeshParts.TryGetValue(o.m_sMesh, uiParts) || uiParts == 0)
      continue;

    if (const plDynamicArray<plString>* pMaterials = st.m_MeshMaterials.GetValue(o.m_sMesh))
    {
      for (const plString& sMaterial : *pMaterials)
        materials.Insert(sMaterial);
    }

    o.m_uiParts = uiParts;
    objects.PushBack(std::move(o));
  }
  out_plan.m_Objects = std::move(objects);

  for (const plString& sMaterial : materials)
    out_plan.m_Materials.PushBack(sMaterial);

  plLog::Info("USD Import: '{}': {} placeables -> {} objects ({} outside the region, {} without bounds)", sRoot, out_plan.m_uiPlaceables,
    out_plan.m_Objects.GetCount(), out_plan.m_uiOutsideRegion, out_plan.m_uiWithoutBounds);
  return plStatus(PL_SUCCESS);
}

plStatus plUsdPartitionImportJob::WriteCells(plArrayPtr<plUsdImportRootPlan> plans)
{
  const plUsdPartitionImportOptions& options = m_Manifest.m_Options;
  const float fCellSize = options.m_fCellSize;

  SetPhase("Writing cells");

  // Larger than the map: the Distant layer. The map is where the objects' centres are.
  {
    plBoundingBox centres = plBoundingBox::MakeInvalid();
    for (const plUsdImportRootPlan& plan : plans)
    {
      for (const plUsdImportObject& o : plan.m_Objects)
      {
        if (o.m_Target == plUsdImportTarget::Cell && o.m_Bounds.IsValid())
          centres.ExpandToInclude(o.m_Bounds.GetCenter());
      }
    }

    const float fMap = options.GetMapExtent(centres);
    for (plUsdImportRootPlan& plan : plans)
    {
      for (plUsdImportObject& o : plan.m_Objects)
      {
        if (o.m_Target == plUsdImportTarget::Cell && o.m_Kind == plUsdImportObjectKind::Mesh && ExtentXY(o.m_Bounds) > fMap)
        {
          o.m_Target = plUsdImportTarget::Distant;
          plStringBuilder sMessage;
          sMessage.SetFormat("{} m wide, larger than the map ({} m)", plArgF(ExtentXY(o.m_Bounds), 0), plArgF(fMap, 0));
          AddProblem(plUsdImportProblem::Type::Distant, o.m_sPrimPath, sMessage);
        }
      }
    }
  }

  // Cell -> one group per root.
  plMap<plUInt64, plDynamicArray<plUsdLayerGroup>> cells;
  plDynamicArray<plUsdLayerGroup> distant;
  m_GlobalObjects.Clear();

  for (plUInt32 r = 0; r < plans.GetCount(); ++r)
  {
    const plUsdImportRootPlan& plan = plans[r];
    plHashTable<plUInt64, plUInt32> groupOfCell;

    for (const plUsdImportObject& o : plan.m_Objects)
    {
      if (o.m_Target == plUsdImportTarget::Globals)
      {
        m_GlobalObjects.PushBack(o);
        continue;
      }

      if (o.m_Target == plUsdImportTarget::Distant)
      {
        if (distant.IsEmpty() || distant.PeekBack().m_sKey != plStringBuilder(plan.m_sRootPath, "#distant"))
        {
          plUsdLayerGroup& group = distant.ExpandAndGetRef();
          group.m_sKey = plStringBuilder(plan.m_sRootPath, "#distant");
          group.m_sName = plan.m_sRootName;
        }
        distant.PeekBack().m_Objects.PushBack(&o);
        ++m_uiDistantObjects;
        continue;
      }

      const plUInt64 uiCell = plUsdPartitionScan::PackCellKey(o.m_Cell);
      plDynamicArray<plUsdLayerGroup>& groups = cells[uiCell];

      plUInt32 uiGroup = 0;
      if (!groupOfCell.TryGetValue(uiCell, uiGroup))
      {
        uiGroup = groups.GetCount();
        groupOfCell.Insert(uiCell, uiGroup);

        plUsdLayerGroup& group = groups.ExpandAndGetRef();
        group.m_sKey = plStringBuilder(plan.m_sRootPath, "#group/", CellText(o.m_Cell));
        group.m_sName = plan.m_sRootName;
        const plVec3 vCentre = plUsdPartitionScan::CellRect(o.m_Cell, fCellSize).GetCenter();
        group.m_vPosition = plVec3(vCentre.x, vCentre.y, 0.0f);
      }
      groups[uiGroup].m_Objects.PushBack(&o);
    }
  }

  auto hashGroups = [](plArrayPtr<const plUsdLayerGroup> groups)
  {
    plUInt64 uiHash = 1;
    for (const plUsdLayerGroup& group : groups)
    {
      uiHash = plHashingUtils::xxHash64String(group.m_sKey, uiHash);
      uiHash = plHashingUtils::xxHash64String(group.m_sName, uiHash);
      for (const plUsdImportObject* pObject : group.m_Objects)
        uiHash = pObject->ComputeHash(uiHash);
    }
    return uiHash;
  };

  const plString sStaging = plStringBuilder(plUsdPartitionImportData::GetWorkFolder(m_sImportId), "/Staging");
  const plString& sPartitionName = options.m_sPartitionName;

  {
    PL_LOCK(m_Mutex);
    m_Progress.m_uiCells = cells.GetCount();
  }

  auto cellFile = [&](plUInt64 uiKey, const plVec2I32& cell)
  {
    plString sFile;
    if (m_Input.m_CellFiles.TryGetValue(uiKey, sFile))
      return sFile;

    plStringBuilder sName;
    plWorldPartitionGrid::GetCellName(sPartitionName, cell.x, cell.y, sName);
    plStringBuilder sPath(m_Input.m_sPartitionFolder, "/", sName, ".plSceneLayer");
    sPath.MakeCleanPath();
    return plString(sPath);
  };

  auto writeOne = [&](plUInt64 uiKey, plArrayPtr<const plUsdLayerGroup> groups, plUInt64 uiHash) -> plStatus
  {
    plVec2I32 cell;
    plWorldPartitionGrid::UnpackKey(uiKey, cell.x, cell.y);

    plUsdLayerIdentity identity;
    identity.m_Scene = m_Input.m_Scene;
    identity.m_Partition = m_Manifest.m_Partition;
    identity.m_iX = cell.x;
    identity.m_iY = cell.y;
    if (!m_Input.m_CellLayers.TryGetValue(uiKey, identity.m_Layer))
      identity.m_Layer = plUsdPartitionImportData::MakeGuid(m_sImportId, plStringBuilder("#cell/", CellText(cell)));

    const plString sFile = cellFile(uiKey, cell);

    // Unchanged only while the file is as the import left it; a file saved or restored since is written again.
    plFileStats stats;
    const bool bExists = plOSFile::GetFileStats(sFile, stats).Succeeded();
    const plUsdPartitionImportManifest::Cell* pOld = m_Manifest.m_Cells.GetValue(uiKey);
    if (!groups.IsEmpty() && pOld != nullptr && pOld->m_uiHash == uiHash && bExists && pOld->m_uiFileSize == stats.m_uiFileSize &&
        pOld->m_iFileTime == stats.m_LastModificationTime.GetInt64(plSIUnitOfTime::Microsecond))
    {
      ++m_uiCellsUnchanged;
      return plStatus(PL_SUCCESS);
    }

    plUsdLayerWriteResult res;
    PL_SUCCEED_OR_RETURN(plUsdPartitionCellWriter::WriteLayer(sFile, sStaging, m_sImportId, identity, groups, nullptr, GetKnownGenerated(), res));

    if (res.m_bChanged)
    {
      ++m_uiCellsWritten;
      if (m_Input.m_ModifiedCellLayers.Contains(identity.m_Layer))
        AddProblem(plUsdImportProblem::Type::CellConflict, sFile, "had unsaved changes in the editor");

      PL_LOCK(m_Mutex);
      m_WrittenLayers.PushBack(sFile);
    }

    if (groups.IsEmpty())
    {
      m_Manifest.m_Cells.Remove(uiKey);
      if (res.m_uiRoots == 0)
        m_EmptiedCells.PushBack(sFile);
    }
    else
    {
      plUsdPartitionImportManifest::Cell& entry = m_Manifest.m_Cells[uiKey];
      entry.m_uiHash = uiHash;
      entry.m_Layer = identity.m_Layer;
      if (plOSFile::GetFileStats(sFile, stats).Succeeded())
      {
        entry.m_uiFileSize = stats.m_uiFileSize;
        entry.m_iFileTime = stats.m_LastModificationTime.GetInt64(plSIUnitOfTime::Microsecond);
      }
    }
    return plStatus(PL_SUCCESS);
  };

  plUInt32 uiSinceSave = 0;
  for (auto it = cells.GetIterator(); it.IsValid(); ++it)
  {
    if (m_bCancel)
      return plStatus("The import was cancelled; it resumes where it stopped.");

    for (const plUsdLayerGroup& group : it.Value())
      m_uiObjects += group.m_Objects.GetCount();

    const plStatus res = writeOne(it.Key(), it.Value(), hashGroups(it.Value()));
    if (res.Failed())
      return res;

    {
      PL_LOCK(m_Mutex);
      ++m_Progress.m_uiCellsWritten;
    }

    if (++uiSinceSave >= 32)
    {
      uiSinceSave = 0;
      m_Manifest.Save(m_Input.m_sManifestFile).LogFailure();
    }
  }

  // Cells of earlier runs that no longer get objects lose their generated ones.
  {
    plDynamicArray<plUInt64> gone;
    for (auto it = m_Manifest.m_Cells.GetIterator(); it.IsValid(); ++it)
    {
      if (!cells.Contains(it.Key()))
        gone.PushBack(it.Key());
    }

    for (plUInt64 uiKey : gone)
    {
      plVec2I32 cell;
      plWorldPartitionGrid::UnpackKey(uiKey, cell.x, cell.y);
      if (!plOSFile::ExistsFile(cellFile(uiKey, cell)))
      {
        m_Manifest.m_Cells.Remove(uiKey);
        continue;
      }

      PL_SUCCEED_OR_RETURN(writeOne(uiKey, {}, 0));
    }
  }

  // The Distant layer: written when it gets objects, or emptied when it had some.
  {
    const plUInt64 uiHash = distant.IsEmpty() ? 0 : hashGroups(distant);
    if (uiHash != m_Manifest.m_uiDistantHash || (!distant.IsEmpty() && !plOSFile::ExistsFile(m_Input.m_sDistantFile)))
    {
      if (!distant.IsEmpty() || plOSFile::ExistsFile(m_Input.m_sDistantFile))
      {
        plUsdLayerIdentity identity;
        identity.m_Scene = m_Input.m_Scene;
        identity.m_Layer = plUsdPartitionImportData::MakeGuid(m_sImportId, "#distant");

        plUsdLayerWriteResult res;
        PL_SUCCEED_OR_RETURN(plUsdPartitionCellWriter::WriteLayer(m_Input.m_sDistantFile, sStaging, m_sImportId, identity, distant, nullptr, GetKnownGenerated(), res));
        if (res.m_bChanged)
        {
          PL_LOCK(m_Mutex);
          m_WrittenLayers.PushBack(m_Input.m_sDistantFile);
        }
      }
      m_Manifest.m_uiDistantHash = uiHash;
    }
  }

  m_uiObjects += m_uiDistantObjects;
  m_Manifest.Save(m_Input.m_sManifestFile).LogFailure();

  // Every game object the import makes, with its prim: groups, their objects, and the lights and cameras.
  {
    plUsdImportObjectIndex index;
    auto add = [&](plStringView sKey, plStringView sPrimPath)
    {
      plUsdImportObjectIndex::Entry& entry = index.m_Entries.ExpandAndGetRef();
      entry.m_Guid = plUsdPartitionImportData::MakeGuid(m_sImportId, sKey);
      entry.m_sKey = sKey;
      entry.m_sPrimPath = sPrimPath;
    };

    auto addGroups = [&](plArrayPtr<const plUsdLayerGroup> groups)
    {
      for (const plUsdLayerGroup& group : groups)
      {
        // A group's key starts with its root's path.
        const char* szHash = group.m_sKey.FindSubString("#");
        add(group.m_sKey, szHash != nullptr ? plStringView(group.m_sKey.GetData(), szHash) : group.m_sKey.GetView());
        for (const plUsdImportObject* pObject : group.m_Objects)
          add(pObject->m_sKey, pObject->m_sPrimPath);
      }
    };

    for (auto it = cells.GetIterator(); it.IsValid(); ++it)
      addGroups(it.Value());
    addGroups(distant);
    for (const plUsdImportObject& o : m_GlobalObjects)
      add(o.m_sKey, o.m_sPrimPath);

    index.Save(plUsdImportObjectIndex::GetPath(m_Input.m_sPartitionFolder)).LogFailure();
  }

  return plStatus(PL_SUCCESS);
}

void plUsdPartitionImportJob::RemoveUnusedMeshes(plArrayPtr<const plUsdImportRootPlan> plans)
{
  plHashSet<plString> used;
  for (const plUsdImportRootPlan& plan : plans)
  {
    for (const plUsdImportObject& o : plan.m_Objects)
    {
      if (!o.m_sMesh.IsEmpty())
      {
        plStringBuilder s = plUsdBakeStore::ToAbsolutePath(o.m_sMesh);
        s.MakeCleanPath();
        s.ToLower();
        used.Insert(s);
      }
    }
  }

  const plString sFolder = plUsdPartitionImportData::GetMeshFolder(m_sImportId);
  if (sFolder.IsEmpty())
    return;

  plDynamicArray<plString> remove;
  plFileSystemIterator files;
  for (files.StartSearch(sFolder, plFileSystemIteratorFlags::ReportFiles); files.IsValid(); files.Next())
  {
    const plFileStats& stats = files.GetStats();
    if (!stats.m_sName.EndsWith_NoCase(".plBinMesh"))
      continue;

    plStringBuilder sFile;
    stats.GetFullPath(sFile);
    sFile.MakeCleanPath();

    plStringBuilder sMesh = plUsdBakeStore::GetMeshOfResource(sFile);
    sMesh.ToLower();

    if (!used.Contains(sMesh))
      remove.PushBack(sFile);
  }

  for (const plString& sFile : remove)
    plOSFile::DeleteFile(sFile).IgnoreResult();

  if (!remove.IsEmpty())
    plLog::Info("USD Import: removed {} mesh file(s) no object uses any more", remove.GetCount());

  // The cache keeps the meshes that stay.
  plUsdImportMeshCache& cache = m_pState->m_MeshCache;
  plDynamicArray<plString> dropped;
  for (auto it = cache.m_Meshes.GetIterator(); it.IsValid(); ++it)
  {
    plStringBuilder s = plUsdBakeStore::ToAbsolutePath(it.Key());
    s.MakeCleanPath();
    s.ToLower();
    if (!used.Contains(s))
      dropped.PushBack(it.Key());
  }

  for (const plString& sMesh : dropped)
    cache.m_Meshes.Remove(sMesh);

  if (!dropped.IsEmpty() || m_pState->m_bMeshCacheChanged)
  {
    m_pState->m_bMeshCacheChanged = false;
    cache.Save(plUsdImportMeshCache::GetPath(m_sImportId)).LogFailure();
  }
}
