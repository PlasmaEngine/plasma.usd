#include <EditorPluginUsd/EditorPluginUsdPCH.h>

#include <EditorPluginUsd/LiveLink/UsdBakeStore.h>
#include <EditorPluginUsd/PartitionImport/UsdPartitionImportData.h>
#include <EditorPluginUsd/Utils/UsdJson.h>
#include <Foundation/Algorithm/HashingUtils.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Utilities/ConversionUtils.h>
#include <ToolsFoundation/Project/ToolsProject.h>

namespace
{
  /// Changes whenever the plan file layout or what a plan holds changes, so older plans are scanned again.
  constexpr plUInt32 PlanFileVersion = 5;

  /// Changes whenever what the import writes changes, so older plans and meshes are made again.
  constexpr plUInt64 ImportVersion = 2;

  /// Changes with ImportVersion or the mesh file layout, so cached meshes of other versions are built again.
  constexpr plUInt32 MeshCacheVersion = 2;

  /// Written last in plan and mesh cache files: a file cut short ends without it.
  constexpr plUInt32 EndMarker = 0x21444e45;

  /// Whether a count just read can be right: each element takes at least four of the bytes left.
  bool IsCountPossible(const plRawMemoryStreamReader& r, plUInt32 uiCount)
  {
    return (plUInt64)uiCount * 4 <= r.GetByteCount() - r.GetReadPosition();
  }

  /// Reads the end marker; false when the data ended early or goes on after it.
  bool ReadEndMarker(plRawMemoryStreamReader& r)
  {
    plUInt32 uiMarker = 0;
    return r.ReadBytes(&uiMarker, sizeof(uiMarker)) == sizeof(uiMarker) && uiMarker == EndMarker && r.GetReadPosition() == r.GetByteCount();
  }

  plVariantArray ToJson(const plDynamicArray<plString>& strings)
  {
    plVariantArray array;
    for (const plString& s : strings)
      array.PushBack(s);
    return array;
  }

  void FromJson(const plVariantArray& array, plDynamicArray<plString>& out_strings)
  {
    out_strings.Clear();
    for (const plVariant& item : array)
      out_strings.PushBack(plUsdJson::AsString(item));
  }

  /// Unchanged content leaves the file alone; with bAtomic it is written next to the target and moved over it.
  plStatus WriteWholeFile(plStringView sFile, const plContiguousMemoryStreamStorage& storage, bool bAtomic)
  {
    const plStringBuilder sTemp = bAtomic ? plStringBuilder(sFile, ".tmp") : plStringBuilder();
    const plArrayPtr<const plUInt8> content(storage.GetData(), storage.GetStorageSize32());
    if (plUsdBakeStore::WriteAbsoluteFile(sFile, content, nullptr, sTemp).Failed())
      return plStatus(plFmt("Could not write '{}'.", sFile));
    return plStatus(PL_SUCCESS);
  }

  plStatus ReadWholeFile(plStringView sFile, plDynamicArray<plUInt8>& out_data)
  {
    plOSFile file;
    if (file.Open(sFile, plFileOpenMode::Read).Failed())
      return plStatus(plFmt("Could not read '{}'.", sFile));

    out_data.SetCountUninitialized((plUInt32)file.GetFileSize());
    const plUInt64 uiRead = out_data.IsEmpty() ? 0 : file.Read(out_data.GetData(), out_data.GetCount());
    file.Close();
    return uiRead == out_data.GetCount() ? plStatus(PL_SUCCESS) : plStatus(plFmt("Could not read '{}'.", sFile));
  }
} // namespace

//////////////////////////////////////////////////////////////////////////

plUInt64 plUsdPartitionImportOptions::GetPlanHash() const
{
  plUInt64 uiHash = plHashingUtils::xxHash64(&ImportVersion, sizeof(ImportVersion));
  auto add = [&](const auto& value)
  { uiHash = plHashingUtils::xxHash64(&value, sizeof(value), uiHash); };
  auto addString = [&](plStringView s)
  { uiHash = plHashingUtils::xxHash64String(s, uiHash + 1); };

  add(m_fCellSize);
  add(m_fBatchSize);
  const float fSplit = GetSplitSize();
  add(fSplit);
  add(m_bLightsAndCameras);
  add(m_Region.IsValid());
  if (m_Region.IsValid())
  {
    add(m_Region.m_vMin.x);
    add(m_Region.m_vMin.y);
    add(m_Region.m_vMax.x);
    add(m_Region.m_vMax.y);
  }

  for (const plUsdVariantOverride& v : m_Variants)
  {
    addString(v.m_sPrimPath);
    addString(v.m_sSet);
    addString(v.m_sVariant);
  }

  for (const plString& s : m_ExcludePaths)
    addString(s);

  return uiHash;
}

bool plUsdPartitionImportOptions::IsBelow(plStringView sPath, plStringView sAncestor)
{
  return sPath.StartsWith(sAncestor) && sPath.GetElementCount() > sAncestor.GetElementCount() && sPath.GetStartPointer()[sAncestor.GetElementCount()] == '/';
}

bool plUsdPartitionImportOptions::IsExcluded(plStringView sPath) const
{
  for (const plString& sExcluded : m_ExcludePaths)
  {
    if (sPath == sExcluded || IsBelow(sPath, sExcluded))
      return true;
  }
  return false;
}

bool plUsdPartitionImportOptions::IsInRegion(const plVec3& vCentre) const
{
  return !m_Region.IsValid() || (vCentre.x >= m_Region.m_vMin.x && vCentre.y >= m_Region.m_vMin.y && vCentre.x <= m_Region.m_vMax.x && vCentre.y <= m_Region.m_vMax.y);
}

float plUsdPartitionImportOptions::GetExtentXY(const plBoundingBox& bounds)
{
  return bounds.IsValid() ? plMath::Max(bounds.m_vMax.x - bounds.m_vMin.x, bounds.m_vMax.y - bounds.m_vMin.y) : 0.0f;
}

plUsdPartitionImportOptions::LooseMeshPlacement plUsdPartitionImportOptions::PlaceLooseMesh(float fExtent, float fMapExtent) const
{
  if (fExtent > fMapExtent)
    return LooseMeshPlacement::Distant;
  if (fExtent > GetSplitSize())
    return LooseMeshPlacement::Split;
  return LooseMeshPlacement::Cell;
}

void plUsdPartitionImportOptions::MakeScanOptions(plStringView sRoot, plUsdPartitionScan& scan, plUsdScanOptions& out_options) const
{
  ResolveVariants(sRoot, scan, out_options.m_Variants);
  out_options.m_bGeometryKeys = true;
  out_options.m_bLightsAndCameras = m_bLightsAndCameras;
  out_options.m_ExcludePaths = m_ExcludePaths;
}

void plUsdPartitionImportOptions::RemoveNestedRoots(plDynamicArray<plString>& ref_roots)
{
  plHashSet<plString> all;
  for (const plString& sRoot : ref_roots)
    all.Insert(sRoot);

  plHashSet<plString> kept;
  plDynamicArray<plString> result;
  for (const plString& sRoot : ref_roots)
  {
    // Any ancestor in the list makes it nested.
    bool bNested = false;
    plStringView sParent = sRoot;
    while (!bNested)
    {
      const char* szSlash = sParent.FindLastSubString("/");
      if (szSlash == nullptr || szSlash == sParent.GetStartPointer())
        break;
      sParent = plStringView(sParent.GetStartPointer(), szSlash);
      bNested = all.Contains(sParent);
    }

    if (!bNested && !kept.Contains(sRoot))
    {
      kept.Insert(sRoot);
      result.PushBack(sRoot);
    }
  }

  ref_roots = std::move(result);
}

void plUsdPartitionImportOptions::ResolveRoots(const plUsdPartitionScan& scan, plDynamicArray<plString>& out_roots) const
{
  out_roots.Clear();
  if (!m_Roots.IsEmpty())
  {
    out_roots = m_Roots;
    RemoveNestedRoots(out_roots);
    return;
  }

  // The highest payload prims, a few levels deep; the top level when there are none.
  plDynamicArray<plString> level;
  level.PushBack("/");
  for (plUInt32 uiDepth = 0; uiDepth < MaxRootDepth && !level.IsEmpty(); ++uiDepth)
  {
    plDynamicArray<plString> next;
    for (const plString& sParent : level)
    {
      plDynamicArray<plUsdPartitionScan::ChildInfo> children;
      scan.ListChildren(sParent, children, false);
      for (const auto& child : children)
      {
        if (IsExcluded(child.m_sPath))
          continue;
        (child.m_bHasPayload ? out_roots : next).PushBack(child.m_sPath);
      }
    }
    level = std::move(next);
  }

  if (out_roots.IsEmpty())
  {
    plDynamicArray<plUsdPartitionScan::ChildInfo> children;
    scan.ListChildren("/", children, false);
    for (const auto& child : children)
    {
      if (!IsExcluded(child.m_sPath))
        out_roots.PushBack(child.m_sPath);
    }
  }
}

void plUsdPartitionImportOptions::ResolveVariants(plStringView sRoot, plUsdPartitionScan& scan, plDynamicArray<plUsdVariantOverride>& out_variants) const
{
  out_variants.Clear();

  for (const plUsdVariantOverride& v : m_Variants)
  {
    if (v.m_sPrimPath.IsEmpty() || v.m_sPrimPath == sRoot)
    {
      plUsdVariantOverride& o = out_variants.ExpandAndGetRef();
      o.m_sSet = v.m_sSet;
      o.m_sVariant = v.m_sVariant;
    }
    else if (IsBelow(v.m_sPrimPath, sRoot))
    {
      out_variants.PushBack(v);
    }
  }

  scan.AddPreferredVariants(sRoot, out_variants);
}

void plUsdPartitionImportOptions::WriteJson(plVariantDictionary& out_json) const
{
  out_json.Insert("stage", m_sStageFile);
  out_json.Insert("partition", m_sPartitionName);
  out_json.Insert("cell", m_fCellSize);
  out_json.Insert("roots", ToJson(m_Roots));

  plVariantArray variants;
  for (const plUsdVariantOverride& v : m_Variants)
  {
    plVariantDictionary variant;
    variant.Insert("prim", v.m_sPrimPath);
    variant.Insert("set", v.m_sSet);
    variant.Insert("variant", v.m_sVariant);
    variants.PushBack(variant);
  }
  out_json.Insert("variants", variants);

  out_json.Insert("batch", m_fBatchSize);
  out_json.Insert("split", m_fSplitSize);
  out_json.Insert("lights", m_bLightsAndCameras);
  out_json.Insert("exclude", ToJson(m_ExcludePaths));

  if (m_Region.IsValid())
  {
    plVariantArray region;
    region.PushBack(m_Region.m_vMin.x);
    region.PushBack(m_Region.m_vMin.y);
    region.PushBack(m_Region.m_vMax.x);
    region.PushBack(m_Region.m_vMax.y);
    out_json.Insert("region", region);
  }
}

plStatus plUsdPartitionImportOptions::ReadJson(const plVariantDictionary& json)
{
  if (json.Contains("stage"))
    m_sStageFile = plUsdJson::GetString(json, "stage");
  if (json.Contains("partition"))
    m_sPartitionName = plUsdJson::GetString(json, "partition");
  if (json.Contains("cell"))
    m_fCellSize = (float)plUsdJson::GetNumber(json, "cell", m_fCellSize);
  if (json.Contains("roots"))
    FromJson(plUsdJson::GetArray(json, "roots"), m_Roots);
  if (json.Contains("batch"))
    m_fBatchSize = plMath::Max(0.0f, (float)plUsdJson::GetNumber(json, "batch", m_fBatchSize));
  if (json.Contains("split"))
    m_fSplitSize = (float)plUsdJson::GetNumber(json, "split", m_fSplitSize);
  if (json.Contains("lights"))
    m_bLightsAndCameras = plUsdJson::GetBool(json, "lights", m_bLightsAndCameras);
  if (json.Contains("exclude"))
    FromJson(plUsdJson::GetArray(json, "exclude"), m_ExcludePaths);

  if (json.Contains("variants"))
  {
    m_Variants.Clear();
    const plVariant& value = *json.GetValue("variants");
    if (value.IsA<plVariantDictionary>())
    {
      // { "set": "variant" } for every root.
      for (const auto& it : value.Get<plVariantDictionary>())
      {
        plUsdVariantOverride& v = m_Variants.ExpandAndGetRef();
        v.m_sSet = it.Key();
        v.m_sVariant = plUsdJson::AsString(it.Value());
      }
    }
    else
    {
      for (const plVariant& item : plUsdJson::GetArray(json, "variants"))
      {
        const plVariantDictionary& variant = plUsdJson::AsObject(item);
        plUsdVariantOverride& v = m_Variants.ExpandAndGetRef();
        v.m_sPrimPath = plUsdJson::GetString(variant, "prim");
        v.m_sSet = plUsdJson::GetString(variant, "set");
        v.m_sVariant = plUsdJson::GetString(variant, "variant");
      }
    }
  }

  if (json.Contains("region"))
  {
    const plVariantArray& region = plUsdJson::GetArray(json, "region");
    if (region.GetCount() != 4)
      return plStatus("'region' expects [minX, minY, maxX, maxY] in metres.");

    auto coordinate = [&](plUInt32 i)
    { return region[i].IsNumber() ? region[i].ConvertTo<float>() : 0.0f; };

    const plVec3 vMin(coordinate(0), coordinate(1), -plMath::HighValue<float>());
    const plVec3 vMax(coordinate(2), coordinate(3), plMath::HighValue<float>());
    if (vMin.x >= vMax.x || vMin.y >= vMax.y)
      return plStatus("'region' is empty.");

    m_Region = plBoundingBox::MakeFromMinMax(vMin, vMax);
  }

  if (!(m_fCellSize > 0.0f))
    return plStatus("'cell' must be a positive size in metres.");

  return plStatus(PL_SUCCESS);
}

//////////////////////////////////////////////////////////////////////////

plUInt64 plUsdImportObject::ComputeHash(plUInt64 uiSeed) const
{
  plUInt64 uiHash = plHashingUtils::xxHash64String(m_sKey, uiSeed);
  auto add = [&](const auto& value)
  { uiHash = plHashingUtils::xxHash64(&value, sizeof(value), uiHash); };

  uiHash = plHashingUtils::xxHash64String(m_sName, uiHash);
  uiHash = plHashingUtils::xxHash64String(m_sMesh, uiHash);
  add(m_Kind);
  add(m_Target);
  add(m_Cell);
  add(m_Transform.m_vPosition);
  add(m_Transform.m_qRotation);
  add(m_Transform.m_vScale);
  add(m_uiParts);

  if (!m_Instances.IsEmpty())
    uiHash = plHashingUtils::xxHash64(m_Instances.GetData(), m_Instances.GetCount() * sizeof(plTransform), uiHash);

  if (m_Kind == plUsdImportObjectKind::Light)
    uiHash = m_Light.ComputeHash(uiHash);
  else if (m_Kind == plUsdImportObjectKind::Camera)
    uiHash = m_Camera.ComputeHash(uiHash);

  return uiHash;
}

//////////////////////////////////////////////////////////////////////////

plStatus plUsdImportRootPlan::Save(plStringView sFile) const
{
  plContiguousMemoryStreamStorage storage;
  plMemoryStreamWriter w(&storage);

  w << PlanFileVersion;
  w << m_sRootPath;
  w << m_sRootName;
  w << m_uiOptionsHash;
  w << m_uiPlaceables;
  w << m_uiOutsideRegion;
  w << m_uiWithoutBounds;

  w << m_Objects.GetCount();
  for (const plUsdImportObject& o : m_Objects)
  {
    w << o.m_sKey;
    w << o.m_sName;
    w << o.m_sPrimPath;
    w << (plUInt8)o.m_Kind;
    w << (plUInt8)o.m_Target;
    w << o.m_bOversized;
    w << o.m_Cell.x;
    w << o.m_Cell.y;
    w << o.m_Transform;
    w << o.m_Bounds;
    w << o.m_sMesh;
    w << o.m_uiParts;

    w << o.m_Instances.GetCount();
    for (const plTransform& t : o.m_Instances)
      w << t;

    auto writeField = [&](const auto& value)
    { w.WriteBytes(&value, sizeof(value)).IgnoreResult(); };
    if (o.m_Kind == plUsdImportObjectKind::Light)
      plUsdLight::VisitFields(o.m_Light, writeField);
    else if (o.m_Kind == plUsdImportObjectKind::Camera)
      plUsdCamera::VisitFields(o.m_Camera, writeField);
  }

  w << m_Layers.GetCount();
  for (auto it = m_Layers.GetIterator(); it.IsValid(); ++it)
  {
    w << it.Key();
    w << it.Value();
  }

  w << m_Materials.GetCount();
  for (const plString& sMaterial : m_Materials)
    w << sMaterial;

  w << m_Problems.GetCount();
  for (const plUsdImportProblem& problem : m_Problems)
  {
    w << (plUInt8)problem.m_Type;
    w << problem.m_sPath;
    w << problem.m_sMessage;
  }

  w << EndMarker;

  // Atomic: a plan cut short by a crash would put a part of the root into the cells.
  return WriteWholeFile(sFile, storage, true);
}

plStatus plUsdImportRootPlan::Load(plStringView sFile)
{
  plDynamicArray<plUInt8> data;
  PL_SUCCEED_OR_RETURN(ReadWholeFile(sFile, data));

  plRawMemoryStreamReader r(data.GetData(), data.GetCount());

  plUInt32 uiVersion = 0;
  r >> uiVersion;
  if (uiVersion != PlanFileVersion)
    return plStatus("The plan was written by another version.");

  r >> m_sRootPath;
  r >> m_sRootName;
  r >> m_uiOptionsHash;
  r >> m_uiPlaceables;
  r >> m_uiOutsideRegion;
  r >> m_uiWithoutBounds;

  plUInt32 uiObjects = 0;
  r >> uiObjects;
  m_Objects.Clear();
  if (!IsCountPossible(r, uiObjects))
    return plStatus("The plan is truncated.");
  m_Objects.Reserve(uiObjects);

  for (plUInt32 i = 0; i < uiObjects; ++i)
  {
    plUsdImportObject& o = m_Objects.ExpandAndGetRef();
    plUInt8 uiKind = 0, uiTarget = 0;
    r >> o.m_sKey;
    r >> o.m_sName;
    r >> o.m_sPrimPath;
    r >> uiKind;
    r >> uiTarget;
    r >> o.m_bOversized;
    r >> o.m_Cell.x;
    r >> o.m_Cell.y;
    r >> o.m_Transform;
    r >> o.m_Bounds;
    r >> o.m_sMesh;
    r >> o.m_uiParts;
    o.m_Kind = (plUsdImportObjectKind)uiKind;
    o.m_Target = (plUsdImportTarget)uiTarget;

    plUInt32 uiInstances = 0;
    r >> uiInstances;
    if (!IsCountPossible(r, uiInstances))
      return plStatus("The plan is truncated.");
    o.m_Instances.SetCountUninitialized(uiInstances);
    for (plTransform& t : o.m_Instances)
      r >> t;

    auto readField = [&](auto& value)
    { r.ReadBytes(&value, sizeof(value)); };
    if (o.m_Kind == plUsdImportObjectKind::Light)
      plUsdLight::VisitFields(o.m_Light, readField);
    else if (o.m_Kind == plUsdImportObjectKind::Camera)
      plUsdCamera::VisitFields(o.m_Camera, readField);
  }

  plUInt32 uiLayers = 0;
  r >> uiLayers;
  m_Layers.Clear();
  if (!IsCountPossible(r, uiLayers))
    return plStatus("The plan is truncated.");
  for (plUInt32 i = 0; i < uiLayers; ++i)
  {
    plString sLayer;
    plInt64 iTime = 0;
    r >> sLayer;
    r >> iTime;
    m_Layers.Insert(sLayer, iTime);
  }

  plUInt32 uiMaterials = 0;
  r >> uiMaterials;
  m_Materials.Clear();
  if (!IsCountPossible(r, uiMaterials))
    return plStatus("The plan is truncated.");
  m_Materials.SetCount(uiMaterials);
  for (plString& sMaterial : m_Materials)
    r >> sMaterial;

  plUInt32 uiProblems = 0;
  r >> uiProblems;
  m_Problems.Clear();
  if (!IsCountPossible(r, uiProblems))
    return plStatus("The plan is truncated.");
  for (plUInt32 i = 0; i < uiProblems; ++i)
  {
    plUInt8 uiType = 0;
    plUsdImportProblem& problem = m_Problems.ExpandAndGetRef();
    r >> uiType;
    r >> problem.m_sPath;
    r >> problem.m_sMessage;
    problem.m_Type = (plUsdImportProblem::Type)uiType;
  }

  if (!ReadEndMarker(r))
    return plStatus("The plan is truncated.");

  return plStatus(PL_SUCCESS);
}

//////////////////////////////////////////////////////////////////////////

plString plUsdPartitionImportManifest::GetPath(plStringView sPartitionFolder)
{
  plStringBuilder s(sPartitionFolder, "/usdimport.json");
  s.MakeCleanPath();
  return s;
}

plUsdPartitionImportManifest::Root* plUsdPartitionImportManifest::FindRoot(plStringView sPath)
{
  for (Root& root : m_Roots)
  {
    if (root.m_sPath == sPath)
      return &root;
  }
  return nullptr;
}

plStatus plUsdPartitionImportManifest::Save(plStringView sFile) const
{
  plVariantDictionary options;
  m_Options.WriteJson(options);

  plVariantArray roots;
  for (const Root& root : m_Roots)
  {
    plVariantDictionary r;
    r.Insert("path", root.m_sPath);
    r.Insert("done", root.m_bDone);
    roots.PushBack(r);
  }

  plVariantDictionary cells;
  for (auto it = m_Cells.GetIterator(); it.IsValid(); ++it)
  {
    const plVec2I32 cell = plUsdPartitionScan::UnpackCellKey(it.Key());

    plStringBuilder sGuid;
    plConversionUtils::ToString(it.Value().m_Layer, sGuid);

    // Numbers as strings: JSON numbers are doubles, which cannot hold every 64-bit value exactly.
    plVariantDictionary c;
    c.Insert("hash", plUsdJson::HexString(it.Value().m_uiHash));
    c.Insert("layer", plString(sGuid));
    c.Insert("size", plUsdJson::DecimalString(static_cast<plInt64>(it.Value().m_uiFileSize)));
    c.Insert("time", plUsdJson::DecimalString(it.Value().m_iFileTime));

    plStringBuilder sKey;
    sKey.SetFormat("{},{}", cell.x, cell.y);
    cells.Insert(sKey, c);
  }

  plVariantArray problems;
  for (const plUsdImportProblem& problem : m_Problems)
  {
    plVariantDictionary p;
    p.Insert("type", static_cast<plInt32>(problem.m_Type));
    p.Insert("path", problem.m_sPath);
    p.Insert("message", problem.m_sMessage);
    problems.PushBack(p);
  }

  plStringBuilder sPartition;
  plConversionUtils::ToString(m_Partition, sPartition);

  plVariantDictionary root;
  root.Insert("version", 1);
  root.Insert("partition", plString(sPartition));
  root.Insert("importId", m_sImportId);
  root.Insert("complete", m_bComplete);
  root.Insert("options", options);
  root.Insert("roots", roots);
  root.Insert("cells", cells);
  root.Insert("distant", plUsdJson::HexString(m_uiDistantHash));
  root.Insert("globals", plUsdJson::HexString(m_uiGlobalsHash));
  root.Insert("problems", problems);

  const plString sText = plUsdJson::Write(root, true);

  plContiguousMemoryStreamStorage storage;
  plMemoryStreamWriter w(&storage);
  w.WriteBytes(sText.GetData(), sText.GetElementCount()).IgnoreResult();

  // Atomic, so a crash never leaves half a manifest.
  return WriteWholeFile(sFile, storage, true);
}

plStatus plUsdPartitionImportManifest::Load(plStringView sFile)
{
  *this = plUsdPartitionImportManifest();

  if (!plOSFile::ExistsFile(sFile))
    return plStatus(plFmt("Could not read '{}'.", sFile));

  plVariantDictionary root;
  if (!plUsdJson::ReadFile(sFile, root) || plUsdJson::GetNumber(root, "version") != 1.0)
    return plStatus(plFmt("'{}' is no import manifest this version reads.", sFile));

  m_Partition = plConversionUtils::ConvertStringToUuid(plUsdJson::GetString(root, "partition"));
  m_sImportId = plUsdJson::GetString(root, "importId");
  m_bComplete = plUsdJson::GetBool(root, "complete");
  PL_SUCCEED_OR_RETURN(m_Options.ReadJson(plUsdJson::GetObject(root, "options")));

  for (const plVariant& item : plUsdJson::GetArray(root, "roots"))
  {
    const plVariantDictionary& r = plUsdJson::AsObject(item);
    Root& entry = m_Roots.ExpandAndGetRef();
    entry.m_sPath = plUsdJson::GetString(r, "path");
    entry.m_bDone = plUsdJson::GetBool(r, "done");
  }

  for (const auto& it : plUsdJson::GetObject(root, "cells"))
  {
    plHybridArray<plStringView, 2> xy;
    it.Key().Split(false, xy, ",");

    plInt32 x = 0, y = 0;
    if (xy.GetCount() != 2 || plConversionUtils::StringToInt(xy[0], x).Failed() || plConversionUtils::StringToInt(xy[1], y).Failed())
      continue;

    const plVariantDictionary& c = plUsdJson::AsObject(it.Value());
    Cell cell;
    cell.m_uiHash = plUsdJson::ParseHex(plUsdJson::GetString(c, "hash"));
    cell.m_Layer = plConversionUtils::ConvertStringToUuid(plUsdJson::GetString(c, "layer"));
    cell.m_uiFileSize = static_cast<plUInt64>(plUsdJson::ParseDecimal(plUsdJson::GetString(c, "size")));
    cell.m_iFileTime = plUsdJson::ParseDecimal(plUsdJson::GetString(c, "time"));
    m_Cells.Insert(plUsdPartitionScan::PackCellKey(plVec2I32(x, y)), cell);
  }

  m_uiDistantHash = plUsdJson::ParseHex(plUsdJson::GetString(root, "distant"));
  m_uiGlobalsHash = plUsdJson::ParseHex(plUsdJson::GetString(root, "globals"));

  for (const plVariant& item : plUsdJson::GetArray(root, "problems"))
  {
    const plVariantDictionary& p = plUsdJson::AsObject(item);
    plUsdImportProblem& problem = m_Problems.ExpandAndGetRef();
    problem.m_Type = (plUsdImportProblem::Type)static_cast<int>(plUsdJson::GetNumber(p, "type"));
    problem.m_sPath = plUsdJson::GetString(p, "path");
    problem.m_sMessage = plUsdJson::GetString(p, "message");
  }

  return plStatus(PL_SUCCESS);
}

//////////////////////////////////////////////////////////////////////////

plString plUsdImportObjectIndex::GetPath(plStringView sPartitionFolder)
{
  plStringBuilder s(sPartitionFolder, "/usdimport.objects");
  s.MakeCleanPath();
  return s;
}

plStatus plUsdImportObjectIndex::Save(plStringView sFile) const
{
  // One line per object: GUID, key and prim path, separated by tabs.
  plContiguousMemoryStreamStorage storage;
  plMemoryStreamWriter w(&storage);
  plStringBuilder sLine, sGuid;
  for (const Entry& entry : m_Entries)
  {
    plConversionUtils::ToString(entry.m_Guid, sGuid);
    sLine.SetFormat("{}\t{}\t{}\n", sGuid, entry.m_sKey, entry.m_sPrimPath);
    w.WriteBytes(sLine.GetData(), sLine.GetElementCount()).IgnoreResult();
  }

  return WriteWholeFile(sFile, storage, true);
}

plStatus plUsdImportObjectIndex::LoadGuids(plStringView sFile, plHashSet<plUuid>& out_guids)
{
  out_guids.Clear();

  plDynamicArray<plUInt8> data;
  PL_SUCCEED_OR_RETURN(ReadWholeFile(sFile, data));

  const char* szPos = reinterpret_cast<const char*>(data.GetData());
  const char* szEnd = szPos + data.GetCount();
  while (szPos < szEnd)
  {
    const char* szLineEnd = szPos;
    while (szLineEnd < szEnd && *szLineEnd != '\n')
      ++szLineEnd;

    const plStringView sLine(szPos, szLineEnd);
    const char* szTab = sLine.FindSubString("\t");
    if (szTab != nullptr)
    {
      plUuid guid;
      if (plConversionUtils::TryConvertStringToUuid(plStringView(szPos, szTab), guid))
        out_guids.Insert(guid);
    }

    szPos = szLineEnd + 1;
  }

  return plStatus(PL_SUCCESS);
}

//////////////////////////////////////////////////////////////////////////

plString plUsdImportMeshCache::GetPath(plStringView sImportId)
{
  plStringBuilder s(plUsdPartitionImportData::GetWorkFolder(sImportId), "/meshes.cache");
  s.MakeCleanPath();
  return s;
}

plStatus plUsdImportMeshCache::Save(plStringView sFile) const
{
  plContiguousMemoryStreamStorage storage;
  plMemoryStreamWriter w(&storage);

  w << MeshCacheVersion;
  w << ImportVersion;
  w << m_Meshes.GetCount();
  for (auto it = m_Meshes.GetIterator(); it.IsValid(); ++it)
  {
    const Entry& entry = it.Value();
    w << it.Key();
    w << entry.m_uiParts;
    w << entry.m_uiFileStamp;
    w << entry.m_uiSkipped;
    w << entry.m_sSkipped;

    w << entry.m_MaterialPaths.GetCount();
    for (plUInt32 i = 0; i < entry.m_MaterialPaths.GetCount(); ++i)
    {
      w << entry.m_MaterialPaths[i];
      w << entry.m_MaterialResources[i];
    }
  }

  w << EndMarker;
  return WriteWholeFile(sFile, storage, true);
}

plStatus plUsdImportMeshCache::Load(plStringView sFile)
{
  m_Meshes.Clear();

  plDynamicArray<plUInt8> data;
  PL_SUCCEED_OR_RETURN(ReadWholeFile(sFile, data));

  plRawMemoryStreamReader r(data.GetData(), data.GetCount());

  plUInt32 uiVersion = 0;
  plUInt64 uiImportVersion = 0;
  r >> uiVersion;
  r >> uiImportVersion;
  if (uiVersion != MeshCacheVersion || uiImportVersion != ImportVersion)
    return plStatus("The mesh cache was written by another version.");

  plUInt32 uiCount = 0;
  r >> uiCount;
  bool bTruncated = !IsCountPossible(r, uiCount);
  if (!bTruncated)
    m_Meshes.Reserve(uiCount);

  for (plUInt32 i = 0; i < uiCount && !bTruncated; ++i)
  {
    plString sMesh;
    Entry entry;
    r >> sMesh;
    r >> entry.m_uiParts;
    r >> entry.m_uiFileStamp;
    r >> entry.m_uiSkipped;
    r >> entry.m_sSkipped;

    plUInt32 uiMaterials = 0;
    r >> uiMaterials;
    bTruncated = !IsCountPossible(r, uiMaterials);
    if (bTruncated)
      break;

    entry.m_MaterialPaths.SetCount(uiMaterials);
    entry.m_MaterialResources.SetCount(uiMaterials);
    for (plUInt32 m = 0; m < uiMaterials; ++m)
    {
      r >> entry.m_MaterialPaths[m];
      r >> entry.m_MaterialResources[m];
    }
    m_Meshes.Insert(sMesh, std::move(entry));
  }

  if (bTruncated || !ReadEndMarker(r))
  {
    m_Meshes.Clear();
    return plStatus("The mesh cache is truncated.");
  }

  return plStatus(PL_SUCCESS);
}

//////////////////////////////////////////////////////////////////////////

plString plUsdPartitionImportData::MakeImportId(const plUuid& partition)
{
  plStringBuilder sGuid;
  plConversionUtils::ToString(partition, sGuid);

  plStringBuilder s;
  s.SetFormat("{}", plArgU(plHashingUtils::xxHash64String(sGuid), 16, true, 16));
  return s;
}

plString plUsdPartitionImportData::MakeFileName(plStringView sName)
{
  plStringBuilder s;
  for (auto it = sName.GetIteratorFront(); it.IsValid(); ++it)
  {
    const plUInt32 c = it.GetCharacter();
    const bool bKeep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    s.Append(bKeep ? c : '_');
  }

  // Long names cost path length without telling more.
  if (s.GetCharacterCount() > 48)
    s.Shrink(0, s.GetCharacterCount() - 48);

  if (s.IsEmpty())
    s = "mesh";

  return s;
}

plString plUsdPartitionImportData::GetMeshName(plStringView sImportId, plStringView sName, plUInt64 uiKey)
{
  plStringBuilder s;
  s.SetFormat(":project/AssetCache/Generated/UsdImport/{}/Meshes/{}-{}", sImportId, MakeFileName(sName), plArgU(uiKey, 16, true, 16));
  return s;
}

plString plUsdPartitionImportData::GetMeshResource(plStringView sMesh, plUInt32 uiPart)
{
  return plUsdBakeStore::GetMeshResource(sMesh, uiPart);
}

plString plUsdPartitionImportData::GetMeshFolder(plStringView sImportId)
{
  if (!plToolsProject::IsProjectOpen())
    return {};

  plStringBuilder s(plToolsProject::GetSingleton()->GetProjectDirectory(), "/AssetCache/Generated/UsdImport/", sImportId, "/Meshes");
  s.MakeCleanPath();
  return s;
}

plString plUsdPartitionImportData::GetMaterialResource(plStringView sImportId, plStringView sMaterialPath)
{
  if (sMaterialPath.IsEmpty())
    return GetDefaultMaterialResource(sImportId);

  plStringBuilder s;
  s.SetFormat(":project/AssetCache/Generated/UsdImport/{}/Materials/{}-{}.plBinMaterial", sImportId, MakeFileName(sMaterialPath.GetFileName()),
    plArgU(plHashingUtils::xxHash32String(sMaterialPath), 8, true, 16));
  return s;
}

plString plUsdPartitionImportData::GetDefaultMaterialResource(plStringView sImportId)
{
  plStringBuilder s;
  s.SetFormat(":project/AssetCache/Generated/UsdImport/{}/Materials/Default.plBinMaterial", sImportId);
  return s;
}

plString plUsdPartitionImportData::GetWorkFolder(plStringView sImportId)
{
  if (!plToolsProject::IsProjectOpen())
    return {};

  plStringBuilder s(plToolsProject::GetSingleton()->GetProjectDirectory(), "/AssetCache/UsdImport/", sImportId);
  s.MakeCleanPath();
  return s;
}

plString plUsdPartitionImportData::GetRootPlanFile(plStringView sImportId, plStringView sRootPath)
{
  plStringBuilder s;
  s.SetFormat("{}/Roots/{}-{}.plan", GetWorkFolder(sImportId), MakeFileName(sRootPath.GetFileName()), plArgU(plHashingUtils::xxHash64String(sRootPath), 16, true, 16));
  return s;
}

plUuid plUsdPartitionImportData::MakeGuid(plStringView sImportId, plStringView sKey, plUInt64 uiSalt)
{
  const plUInt64 uiSeed = plHashingUtils::xxHash64String(sImportId, uiSalt);
  const plUInt64 uiLow = plHashingUtils::xxHash64String(sKey, uiSeed);
  const plUInt64 uiHigh = plHashingUtils::xxHash64String(sKey, uiSeed ^ 0x9e3779b97f4a7c15ull);
  return plUuid(uiLow, uiHigh);
}
