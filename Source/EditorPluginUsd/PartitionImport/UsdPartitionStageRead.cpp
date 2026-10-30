#include <EditorPluginUsd/EditorPluginUsdPCH.h>

#include <EditorPluginUsd/PartitionImport/UsdPartitionStageRead.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Threading/DelegateTask.h>
#include <Foundation/Threading/TaskSystem.h>
#include <UsdBridge/UsdPartitionScan.h>

namespace
{
  /// Background reads, so the plugin can wait for them before it unloads.
  plDynamicArray<plTaskGroupID, plStaticsAllocatorWrapper>& GetRunningTasks()
  {
    static plDynamicArray<plTaskGroupID, plStaticsAllocatorWrapper> s_Tasks;
    return s_Tasks;
  }

  void StartTask(const plSharedPtr<plUsdPartitionStageRead>& pRead, const char* szName)
  {
    plSharedPtr<plDelegateTask<void>> pTask = PL_DEFAULT_NEW(plDelegateTask<void>, szName, plTaskNesting::Never, [pRead]()
      { pRead->Run(); });
    GetRunningTasks().PushBack(plTaskSystem::StartSingleTask(pTask, plTaskPriority::LongRunning));
  }
} // namespace

//////////////////////////////////////////////////////////////////////////
// Worker

plSharedPtr<plUsdPartitionStageRead> plUsdPartitionStageRead::StartListing(plStringView sStage, const plUsdPartitionImportOptions& options)
{
  plSharedPtr<plUsdPartitionStageRead> pRead = PL_DEFAULT_NEW(plUsdPartitionStageRead);
  pRead->m_sStage = sStage;
  pRead->m_Options = options;

  StartTask(pRead, "USD partition import: stage listing");
  return pRead;
}

plSharedPtr<plUsdPartitionStageRead> plUsdPartitionStageRead::StartScan(const plUsdPartitionImportOptions& options, plArrayPtr<const plString> roots)
{
  plSharedPtr<plUsdPartitionStageRead> pScan = PL_DEFAULT_NEW(plUsdPartitionStageRead);
  pScan->m_Roots = roots;
  pScan->m_sStage = options.m_sStageFile;
  pScan->m_bScan = true;
  pScan->m_Options = options;

  StartTask(pScan, "USD partition import: scan");
  return pScan;
}

void plUsdPartitionStageRead::SetProgress(plStringView sText)
{
  PL_LOCK(m_Mutex);
  m_sProgress = sText;
}

plString plUsdPartitionStageRead::GetProgress() const
{
  PL_LOCK(m_Mutex);
  return m_sProgress;
}

void plUsdPartitionStageRead::TakeSizes(plDynamicArray<Size>& out_sizes, plUInt32& out_uiMeasured, plUInt32& out_uiTotal)
{
  PL_LOCK(m_Mutex);
  out_sizes = std::move(m_Sizes);
  m_Sizes.Clear();
  out_uiMeasured = m_uiSizesMeasured;
  out_uiTotal = m_uiSizesToMeasure;
}

void plUsdPartitionStageRead::Run()
{
  const plTime start = plTime::Now();
  plUsdPartitionScan scan;

  SetProgress("Opening the stage");
  m_Result = scan.Open(m_sStage);
  if (m_Result.Succeeded())
  {
    const plUsdBasis& basis = scan.GetBasis();
    plStringBuilder s;
    s.SetFormat("{}-up, {} m per unit", basis.GetUpAxis() == plUsdBasis::UpAxis::Z ? "Z" : "Y", plArgF(basis.GetMetersPerUnit(), 4));
    m_sBasis = s;

    if (m_bScan)
      Scan(scan);
    else
      List(scan);
  }

  m_Duration = plTime::Now() - start;
  m_bListed.Set(true);

  if (m_Result.Succeeded() && !m_bScan)
    MeasureSizes(scan);

  m_bDone.Set(true);
}

void plUsdPartitionStageRead::List(plUsdPartitionScan& scan)
{
  SetProgress("Reading the prims");

  plDynamicArray<plUInt32> level;
  level.PushBack(plInvalidIndex);

  for (plUInt32 uiDepth = 0; uiDepth < plUsdPartitionImportOptions::MaxRootDepth && !level.IsEmpty() && !m_bTruncated; ++uiDepth)
  {
    plDynamicArray<plUInt32> next;
    for (plUInt32 uiParent : level)
    {
      if (m_bCancel)
        return;

      // Subtree bounds are cached by the scan, so each level reuses what the level above computed.
      plDynamicArray<plUsdPartitionScan::ChildInfo> children;
      scan.ListChildren(uiParent == plInvalidIndex ? plStringView("/") : m_Prims[uiParent].m_sPath.GetView(), children, true, &m_bCancel);
      for (plUsdPartitionScan::ChildInfo& child : children)
      {
        if (m_Prims.GetCount() >= plUsdPartitionImportUi::MaxListedPrims)
        {
          m_bTruncated = true;
          break;
        }

        const plUInt32 uiIndex = m_Prims.GetCount();
        plUsdPartitionPrimEntry& entry = m_Prims.ExpandAndGetRef();
        entry.m_sPath = child.m_sPath;
        entry.m_sName = child.m_sName;
        entry.m_sType = child.m_sTypeName;
        entry.m_uiParent = uiParent;
        entry.m_bPayload = child.m_bHasPayload;
        entry.m_bExcluded = m_Options.IsExcluded(child.m_sPath);
        entry.m_VariantSets = std::move(child.m_VariantSets);
        entry.m_Bounds = child.m_WorldBounds;

        if (entry.m_bExcluded)
          continue;

        if (entry.m_bPayload)
        {
          for (plUInt32 p = uiParent; p != plInvalidIndex && !m_Prims[p].m_bPayloadBelow; p = m_Prims[p].m_uiParent)
            m_Prims[p].m_bPayloadBelow = true;
        }
        else
        {
          next.PushBack(uiIndex);
        }
      }
    }
    level = std::move(next);
  }

  for (const plUsdPartitionPrimEntry& entry : m_Prims)
    m_bAnyPayload |= entry.m_bPayload && !entry.m_bExcluded;

  // The roots the import finds when none are chosen, by its own rule; the listing may stop before some of them.
  plUsdPartitionImportOptions defaults = m_Options;
  defaults.m_Roots.Clear();
  plDynamicArray<plString> defaultRoots;
  defaults.ResolveRoots(scan, defaultRoots);
  plHashSet<plString> defaultSet;
  for (const plString& sRoot : defaultRoots)
    defaultSet.Insert(sRoot);

  // Parents come before their children.
  for (plUsdPartitionPrimEntry& entry : m_Prims)
  {
    if (!entry.m_bPayload && !entry.m_bPayloadBelow)
    {
      const plUsdPartitionPrimEntry* pParent = entry.m_uiParent != plInvalidIndex ? &m_Prims[entry.m_uiParent] : nullptr;
      entry.m_uiPlainDepth = pParent != nullptr && pParent->m_uiPlainDepth != plInvalidIndex ? pParent->m_uiPlainDepth + 1 : 0;
    }

    entry.m_bCandidate = entry.m_bPayload || entry.m_uiPlainDepth <= plUsdPartitionImportUi::MaxPlainRootDepth;
    entry.m_bDefault = !entry.m_bExcluded && defaultSet.Remove(entry.m_sPath);
  }

  for (const plString& sRoot : defaultRoots)
  {
    if (defaultSet.Contains(sRoot))
      m_UnlistedDefaults.PushBack(sRoot);
  }
}

void plUsdPartitionStageRead::MeasureSizes(plUsdPartitionScan& scan)
{
  plDynamicArray<plUInt32> prims;
  for (plUInt32 i = 0; i < m_Prims.GetCount(); ++i)
  {
    const plUsdPartitionPrimEntry& prim = m_Prims[i];
    if (!prim.m_bPayload || prim.m_bExcluded)
      continue;

    for (const plUsdVariantSet& set : prim.m_VariantSets)
    {
      if (!plUsdPartitionScan::GetProxyVariant(set).IsEmpty())
      {
        prims.PushBack(i);
        break;
      }
    }
  }

  {
    PL_LOCK(m_Mutex);
    m_uiSizesToMeasure = prims.GetCount();
  }

  for (plUInt32 uiPrim : prims)
  {
    if (m_bCancel)
      return;

    Size size;
    size.m_uiPrim = uiPrim;
    const bool bMeasured = scan.ComputeProxyBounds(m_Prims[uiPrim].m_sPath, size.m_Bounds).Succeeded() && size.m_Bounds.IsValid();

    PL_LOCK(m_Mutex);
    ++m_uiSizesMeasured;
    if (bMeasured)
      m_Sizes.PushBack(size);
  }
}

void plUsdPartitionStageRead::Scan(plUsdPartitionScan& scan)
{
  const float fCellSize = m_Options.m_fCellSize;

  plUsdScanResult result;
  plHashSet<plUInt64> cells;
  plHashSet<plUInt64> looseKeys;
  plUInt64 uiKeylessGprims = 0;

  // Loose meshes that may be split or go into the Distant layer (the map is at least four cells wide); placed once
  // the map's extent is known.
  struct WideGprim
  {
    plBoundingBox m_Bounds;
    plUInt64 m_uiGeometryKey = 0;
  };
  plDynamicArray<WideGprim> wideGprims;
  plBoundingBox centres = plBoundingBox::MakeInvalid();
  const float fWide = plMath::Min(m_Options.GetSplitSize(), 4.0f * fCellSize);

  for (plUInt32 r = 0; r < m_Roots.GetCount(); ++r)
  {
    if (m_bCancel)
      return;

    const plString& sRoot = m_Roots[r];
    const plStringView sName = sRoot.GetFileName();

    plUsdScanOptions options;
    m_Options.MakeScanOptions(sRoot, scan, options);
    options.m_pCancel = &m_bCancel;
    options.m_OnProgress = [&](bool bLoading, plUInt32 uiPrims)
    {
      plStringBuilder s;
      if (bLoading)
        s.SetFormat("{}/{} {}: loading", r + 1, m_Roots.GetCount(), sName);
      else
        s.SetFormat("{}/{} {}: {} prims", r + 1, m_Roots.GetCount(), sName, uiPrims);
      SetProgress(s);
    };

    const plUInt32 uiRoot = result.m_Roots.GetCount();
    const plStatus res = scan.ScanRoot(sRoot, options, result);
    if (res.Failed())
    {
      scan.ReleaseRoot(sRoot);
      if (m_bCancel)
        return;

      if (m_sFailedRoot.IsEmpty())
      {
        plStringBuilder s;
        s.SetFormat("{}: {}", sRoot, res.m_sMessage);
        m_sFailedRoot = s;
      }
      m_iRootsDone.Increment();
      continue;
    }

    const plUsdScanRoot& root = result.m_Roots[uiRoot];
    for (plUInt32 i = root.m_uiFirstPlaceable; i < root.m_uiFirstPlaceable + root.m_uiPlaceableCount; ++i)
    {
      const plUsdPlaceable& placeable = result.m_Placeables[i];
      if (placeable.m_Kind == plUsdPlaceableKind::Light || placeable.m_Kind == plUsdPlaceableKind::Camera)
      {
        ++m_uiLightsAndCameras;
        continue;
      }

      if (!placeable.m_WorldBounds.IsValid())
        continue;

      const plVec3 vCentre = placeable.m_WorldBounds.GetCenter();
      if (!m_Options.IsInRegion(vCentre))
      {
        ++m_uiOutsideRegion;
        continue;
      }

      ++m_uiPlaceables;
      centres.ExpandToInclude(vCentre);

      if (placeable.m_Kind == plUsdPlaceableKind::Gprim)
      {
        ++m_uiGprims;
        if (plUsdPartitionImportOptions::GetExtentXY(placeable.m_WorldBounds) > fWide)
        {
          wideGprims.PushBack({placeable.m_WorldBounds, placeable.m_uiGeometryKey});
          continue;
        }

        if (placeable.m_uiGeometryKey != 0)
          looseKeys.Insert(placeable.m_uiGeometryKey);
        else
          ++uiKeylessGprims;
      }
      else
      {
        ++m_uiInstances;
      }

      cells.Insert(plUsdPartitionScan::PackCellKey(plUsdPartitionScan::CellOf(vCentre, fCellSize)));
    }

    ++m_uiRootsScanned;
    m_iRootsDone.Increment();
    scan.ReleaseRoot(sRoot);
    result.ReleasePlaceables();
  }

  // Split meshes count one piece and one mesh per cell their bounds touch; where they have no geometry the import
  // makes no piece, so this is an upper bound.
  const float fMap = m_Options.GetMapExtent(centres);
  for (const WideGprim& gprim : wideGprims)
  {
    const plUsdPartitionImportOptions::LooseMeshPlacement placement = m_Options.PlaceLooseMesh(plUsdPartitionImportOptions::GetExtentXY(gprim.m_Bounds), fMap);
    if (placement == plUsdPartitionImportOptions::LooseMeshPlacement::Split)
    {
      const plVec2I32 min = plUsdPartitionScan::CellOf(gprim.m_Bounds.m_vMin, fCellSize);
      const plVec2I32 max = plUsdPartitionScan::CellOf(gprim.m_Bounds.m_vMax, fCellSize);
      for (plInt32 y = min.y; y <= max.y; ++y)
      {
        for (plInt32 x = min.x; x <= max.x; ++x)
          cells.Insert(plUsdPartitionScan::PackCellKey(plVec2I32(x, y)));
      }
      ++m_uiSplit;
      m_uiSplitPieces += static_cast<plUInt64>(max.x - min.x + 1) * static_cast<plUInt64>(max.y - min.y + 1);
      continue;
    }

    if (placement == plUsdPartitionImportOptions::LooseMeshPlacement::Distant)
      ++m_uiDistant;
    else
      cells.Insert(plUsdPartitionScan::PackCellKey(plUsdPartitionScan::CellOf(gprim.m_Bounds.GetCenter(), fCellSize)));

    if (gprim.m_uiGeometryKey != 0)
      looseKeys.Insert(gprim.m_uiGeometryKey);
    else
      ++uiKeylessGprims;
  }

  m_uiCells = cells.GetCount();
  m_uiPrototypes = result.m_Prototypes.GetCount();
  m_uiLooseMeshes = looseKeys.GetCount() + uiKeylessGprims + m_uiSplitPieces;
}

//////////////////////////////////////////////////////////////////////////
// Results

void plUsdPartitionStageRead::CollectRoots(const plHashSet<plString>& checked, const plUsdPartitionImportOptions& initialOptions, plDynamicArray<plString>& out_roots, bool& out_bDefault) const
{
  out_roots.Clear();
  out_bDefault = true;

  plHashSet<plString> candidates;
  for (const plUsdPartitionPrimEntry& prim : m_Prims)
  {
    if (!prim.m_bCandidate)
      continue;

    candidates.Insert(prim.m_sPath);
    const bool bChecked = checked.Contains(prim.m_sPath);
    if (bChecked)
      out_roots.PushBack(prim.m_sPath);
    if (bChecked != prim.m_bDefault)
      out_bDefault = false;
  }

  if (initialOptions.m_Roots.IsEmpty())
  {
    // Default roots the listing does not show cannot be unchecked.
    out_roots.PushBackRange(m_UnlistedDefaults);
  }
  else
  {
    // Roots chosen by JSON that the listing does not show stay chosen.
    for (const plString& sRoot : initialOptions.m_Roots)
    {
      if (!candidates.Contains(sRoot))
      {
        out_roots.PushBack(sRoot);
        out_bDefault = false;
      }
    }
  }

  // A checked prim below another checked one is part of that root.
  plUsdPartitionImportOptions::RemoveNestedRoots(out_roots);
}

void plUsdPartitionStageRead::GatherVariantSets(plDynamicArray<plUsdVariantSet>& out_sets) const
{
  out_sets.Clear();

  for (const plUsdPartitionPrimEntry& prim : m_Prims)
  {
    if (!prim.m_bCandidate || prim.m_bExcluded)
      continue;

    for (const plUsdVariantSet& set : prim.m_VariantSets)
    {
      plUsdVariantSet* pSet = nullptr;
      for (plUsdVariantSet& known : out_sets)
      {
        if (known.m_sName == set.m_sName)
          pSet = &known;
      }

      if (pSet == nullptr)
      {
        out_sets.PushBack(set);
        continue;
      }

      for (const plString& sVariant : set.m_Variants)
      {
        if (!pSet->m_Variants.Contains(sVariant))
          pSet->m_Variants.PushBack(sVariant);
      }
      if (pSet->m_sSelection.IsEmpty())
        pSet->m_sSelection = set.m_sSelection;
    }
  }
}

void plUsdPartitionStageRead::GetUsedVariantSets(plArrayPtr<const plString> roots, plHashSet<plString>& out_sets) const
{
  out_sets.Clear();

  plHashSet<plString> rootSet;
  for (const plString& sRoot : roots)
    rootSet.Insert(sRoot);

  // A set is offered while a checked root has it.
  for (const plUsdPartitionPrimEntry& prim : m_Prims)
  {
    if (!prim.m_bCandidate || prim.m_VariantSets.IsEmpty() || !rootSet.Contains(prim.m_sPath))
      continue;

    for (const plUsdVariantSet& set : prim.m_VariantSets)
      out_sets.Insert(set.m_sName);
  }
}

plString plUsdPartitionStageRead::FormatStageInfo() const
{
  plStringBuilder sInfo;
  sInfo.SetFormat("{}, read in {} s without payloads", m_sBasis, plArgF(m_Duration.GetSeconds(), 1));
  if (m_bTruncated)
    sInfo.AppendFormat("; only the first {} prims are listed", plUsdPartitionImportUi::MaxListedPrims);
  if (!m_UnlistedDefaults.IsEmpty())
    sInfo.AppendFormat("; {} more payload prim(s) not listed are imported too", m_UnlistedDefaults.GetCount());
  return sInfo;
}

plString plUsdPartitionStageRead::FormatScanResult() const
{
  using namespace plUsdPartitionImportUi;

  plStringBuilder s;
  s.SetFormat("{} prim(s) scanned in {} s: ", m_uiRootsScanned, plArgF(m_Duration.GetSeconds(), 1));
  s.AppendFormat("{} objects ({} instances, {} meshes)", FormatCount(m_uiPlaceables), FormatCount(m_uiInstances), FormatCount(m_uiGprims));
  s.AppendFormat(", {} cells", FormatCount(m_uiCells));
  s.AppendFormat(", {} unique meshes", FormatCount(m_uiPrototypes + m_uiLooseMeshes));
  if (m_uiSplit > 0)
    s.AppendFormat(", {} split along cells into up to {} pieces", FormatCount(m_uiSplit), FormatCount(m_uiSplitPieces));
  if (m_uiDistant > 0)
    s.AppendFormat(", {} larger than the map, in the Distant layer", FormatCount(m_uiDistant));
  if (m_uiLightsAndCameras > 0)
    s.AppendFormat(", {} lights and cameras", FormatCount(m_uiLightsAndCameras));
  if (m_uiOutsideRegion > 0)
    s.AppendFormat(", {} outside the region", FormatCount(m_uiOutsideRegion));

  s.Append(". Objects before batching.");
  return s;
}

plString plUsdPartitionStageRead::FormatNotScanned() const
{
  if (m_sFailedRoot.IsEmpty())
    return {};

  return plStringBuilder("Not scanned: ", m_sFailedRoot);
}

//////////////////////////////////////////////////////////////////////////
// Shared

plString plUsdPartitionImportUi::GetDefaultVariant(const plUsdVariantSet& set)
{
  const plStringView sPreferred = plUsdPartitionScan::GetPreferredVariant(set);
  if (!sPreferred.IsEmpty())
    return sPreferred;
  if (!set.m_sSelection.IsEmpty())
    return set.m_sSelection;
  return set.m_Variants.IsEmpty() ? plString() : set.m_Variants[0];
}

plString plUsdPartitionImportUi::GetChosenVariant(const plUsdVariantSet& set, const plUsdPartitionImportOptions& initialOptions)
{
  // The earlier choice for every root, else what the import picks on its own.
  plString sChosen = GetDefaultVariant(set);
  for (const plUsdVariantOverride& v : initialOptions.m_Variants)
  {
    if (v.m_sPrimPath.IsEmpty() && v.m_sSet == set.m_sName)
      sChosen = v.m_sVariant;
  }
  return sChosen;
}

plString plUsdPartitionImportUi::FormatSize(const plBoundingBox& bounds)
{
  if (!bounds.IsValid())
    return "-";

  // "x × y m", the multiplication sign as UTF-8
  const plVec3 vSize = bounds.GetExtents();
  plStringBuilder s;
  s.SetFormat("{} \xC3\x97 {} m", plArgF(vSize.x, 0), plArgF(vSize.y, 0));
  return s;
}

plString plUsdPartitionImportUi::FormatCount(plUInt64 uiCount)
{
  plStringBuilder sDigits;
  sDigits.SetFormat("{}", uiCount);

  // a comma before every group of three digits from the right
  plStringBuilder s;
  const plUInt32 uiDigits = sDigits.GetElementCount();
  for (plUInt32 i = 0; i < uiDigits; ++i)
  {
    if (i > 0 && (uiDigits - i) % 3 == 0)
      s.Append(",");
    s.Append(sDigits.GetData()[i]);
  }
  return s;
}

plString plUsdPartitionImportUi::FormatCellSize(float fSize)
{
  plStringBuilder s;
  s.SetFormat("{} m", plArgF(fSize, 0));
  return s;
}

bool plUsdPartitionImportUi::BuildOptions(const plUsdPartitionImportOptions& initialOptions, const plUsdPartitionImportChoices& choices, plUsdPartitionImportOptions& out_options, plString& out_sError)
{
  out_options = initialOptions;
  out_options.m_sStageFile = choices.m_sStageFile;
  out_options.m_sPartitionName = choices.m_sPartitionName;
  out_options.m_fCellSize = choices.m_fCellSize;

  if (out_options.m_sStageFile.IsEmpty() || !plOSFile::ExistsFile(out_options.m_sStageFile))
  {
    out_sError = "Choose an existing USD stage.";
    return false;
  }

  if (out_options.m_sPartitionName.IsEmpty())
  {
    out_sError = "Enter a name for the partition.";
    return false;
  }

  out_options.m_Roots = choices.m_Roots;
  if (out_options.m_Roots.IsEmpty())
  {
    out_sError = "Check at least one prim to import.";
    return false;
  }

  // The import finds the same roots on its own, also ones a later version of the stage adds.
  if (choices.m_bDefaultRoots)
    out_options.m_Roots.Clear();

  // Overrides for a prim of their own stay; sets for every root only where they differ from what the import picks.
  out_options.m_Variants.Clear();
  for (const plUsdVariantOverride& v : initialOptions.m_Variants)
  {
    if (!v.m_sPrimPath.IsEmpty())
      out_options.m_Variants.PushBack(v);
  }
  for (const plUsdPartitionImportChoices::Variant& variant : choices.m_Variants)
  {
    if (variant.m_sChosen != variant.m_sDefault)
      out_options.m_Variants.PushBack({plString(), variant.m_sSet, variant.m_sChosen});
  }

  out_options.m_fBatchSize = choices.m_bBatch ? choices.m_fBatchSize : 0.0f;

  if (!choices.m_bSplit)
    out_options.m_fSplitSize = -1.0f;
  else if (choices.m_fSplitSize == out_options.m_fCellSize)
    out_options.m_fSplitSize = 0.0f;
  else
    out_options.m_fSplitSize = choices.m_fSplitSize;

  out_options.m_bLightsAndCameras = choices.m_bLightsAndCameras;

  out_options.m_Region = plBoundingBox::MakeInvalid();
  if (choices.m_bRegion)
  {
    const plVec3 vMin(choices.m_fRegion[0], choices.m_fRegion[1], -plMath::HighValue<float>());
    const plVec3 vMax(choices.m_fRegion[2], choices.m_fRegion[3], plMath::HighValue<float>());
    if (vMin.x >= vMax.x || vMin.y >= vMax.y)
    {
      out_sError = "The region is empty: its 'to' values must be larger than its 'from' values.";
      return false;
    }
    out_options.m_Region = plBoundingBox::MakeFromMinMax(vMin, vMax);
  }

  return true;
}

void plUsdPartitionImportUi::ForgetFinishedTasks()
{
  for (plUInt32 i = GetRunningTasks().GetCount(); i-- > 0;)
  {
    if (plTaskSystem::IsTaskGroupFinished(GetRunningTasks()[i]))
      GetRunningTasks().RemoveAtAndSwap(i);
  }
}

void plUsdPartitionImportUi::WaitForTasks()
{
  for (const plTaskGroupID& task : GetRunningTasks())
    plTaskSystem::WaitForGroup(task);
  GetRunningTasks().Clear();
  GetRunningTasks().Compact();
}
