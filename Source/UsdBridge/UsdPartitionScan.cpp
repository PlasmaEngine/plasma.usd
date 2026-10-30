#include <UsdBridge/UsdBridgePCH.h>

#include <Foundation/Algorithm/HashingUtils.h>
#include <Foundation/Containers/Map.h>
#include <Foundation/Types/ScopeExit.h>
#include <GameEngine/WorldStreaming/WorldPartitionGrid.h>
#include <UsdBridge/UsdPartitionScan.h>
#include <UsdBridge/UsdStageAccess.h>

#pragma warning(push, 0)
#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/sdf/changeBlock.h>
#include <pxr/usd/usd/editContext.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/variantSets.h>
#include <pxr/usd/usdGeom/bboxCache.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/usd/usdGeom/pointInstancer.h>
#include <pxr/usd/usdGeom/subset.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#pragma warning(pop)

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>

using plUsdText::FromStd;
using plUsdText::HashString;
using plUsdText::ToStd;

namespace
{
  /// Every prim that exists, loaded or not, except inactive and abstract ones.
  const pxr::Usd_PrimFlagsConjunction s_Predicate = pxr::UsdPrimIsActive && pxr::UsdPrimIsDefined && !pxr::UsdPrimIsAbstract;

  /// The one time everything of a scan is read at, as plUsdStage::CaptureGeometry reads geometry.
  const pxr::UsdTimeCode s_Time = pxr::UsdTimeCode::EarliestTime();

  plMat4d ToPlasmaMatrix(const plUsdBasis& basis, const pxr::GfMatrix4d& m)
  {
    return basis.MatrixToPlasma(plUsdStageAccess::FromGfMatrix(m));
  }

  plBoundingBox ToPlasmaBounds(const plUsdBasis& basis, const pxr::GfBBox3d& box)
  {
    const pxr::GfRange3d range = box.ComputeAlignedRange();
    if (range.IsEmpty())
      return plBoundingBox::MakeInvalid();

    // The axis map only permutes and negates axes, so the converted corners still span the box.
    const pxr::GfVec3d& a = range.GetMin();
    const pxr::GfVec3d& b = range.GetMax();
    const plVec3d pa = basis.PointToPlasma(plVec3d(a[0], a[1], a[2]));
    const plVec3d pb = basis.PointToPlasma(plVec3d(b[0], b[1], b[2]));
    const plVec3d vMin = pa.CompMin(pb);
    const plVec3d vMax = pa.CompMax(pb);
    return plBoundingBox::MakeFromMinMax(plVec3((float)vMin.x, (float)vMin.y, (float)vMin.z), plVec3((float)vMax.x, (float)vMax.y, (float)vMax.z));
  }

  enum class Filtered : plUInt8
  {
    Keep,
    Purpose,
    Invisible,
  };

  using PurposeInfo = pxr::UsdGeomImageable::PurposeInfo;

  /// Default (also not computed yet) and render purposes are drawn.
  bool IsDrawn(const PurposeInfo& info)
  {
    return info.purpose.IsEmpty() || info.purpose == pxr::UsdGeomTokens->default_ || info.purpose == pxr::UsdGeomTokens->render;
  }

  /// Visibility and purpose of a prim, given its parent's purpose in inout_purpose, which becomes the prim's. Purpose
  /// follows UsdGeomImageable::ComputePurposeInfo: the topmost authored purpose wins. Non-imageable prims pass their
  /// parent's on. Both invisibility and an undrawn purpose hide the whole subtree.
  Filtered CheckPrim(const pxr::UsdPrim& prim, PurposeInfo& inout_purpose)
  {
    const pxr::UsdGeomImageable imageable(prim);
    if (imageable)
    {
      pxr::TfToken visibility;
      if (imageable.GetVisibilityAttr().Get(&visibility, s_Time) && visibility == pxr::UsdGeomTokens->invisible)
        return Filtered::Invisible;

      inout_purpose = imageable.ComputePurposeInfo(inout_purpose);
    }

    return IsDrawn(inout_purpose) ? Filtered::Keep : Filtered::Purpose;
  }

  /// Purpose a prim inherits: that of its nearest imageable ancestor.
  PurposeInfo InheritedPurpose(const pxr::UsdPrim& prim)
  {
    for (pxr::UsdPrim parent = prim.GetParent(); parent && !parent.IsPseudoRoot(); parent = parent.GetParent())
    {
      if (const pxr::UsdGeomImageable imageable(parent); imageable)
        return imageable.ComputePurposeInfo();
    }
    return {};
  }

  /// Typed prims that are not imageable (materials, shaders, subsets) have nothing to draw below them; typeless ones may.
  bool IsTraversable(const pxr::UsdPrim& prim)
  {
    return prim.IsA<pxr::UsdGeomImageable>() || prim.GetTypeName().IsEmpty();
  }

  /// A gprim of a prototype and its place in it.
  struct PrototypePart
  {
    pxr::UsdPrim m_Prim;
    pxr::GfMatrix4d m_mRelative;
  };

  /// The drawable gprims of a prototype, through nested instances. For an instance, relative to the instance prim; for a
  /// point instancer prototype, relative to the prototype prim, whose own transform the instance transforms include.
  void CollectParts(const pxr::UsdPrim& source, pxr::UsdGeomXformCache& ref_xforms, plDynamicArray<PrototypePart>& out_parts, plUInt32& inout_uiSkipped)
  {
    struct Visitor
    {
      const pxr::UsdPrim& m_Source;
      pxr::UsdGeomXformCache& m_Xforms;
      plDynamicArray<PrototypePart>& m_Parts;
      plUInt32& m_uiSkipped;

      void Visit(const pxr::UsdPrim& prim, PurposeInfo purpose)
      {
        if (!IsTraversable(prim) || CheckPrim(prim, purpose) != Filtered::Keep)
          return;

        const plUsdPrimKind kind = plUsdStageAccess::GetKind(prim);
        if (kind == plUsdPrimKind::Mesh || kind == plUsdPrimKind::Gprim)
        {
          PrototypePart& part = m_Parts.ExpandAndGetRef();
          part.m_Prim = prim;

          // A part that resets the transform stack is placed in the world, not below the source.
          bool bResetsXformStack = false;
          part.m_mRelative = prim == m_Source ? pxr::GfMatrix4d(1.0) : m_Xforms.ComputeRelativeTransform(prim, m_Source, &bResetsXformStack);
          if (bResetsXformStack)
            part.m_mRelative = m_Xforms.GetLocalToWorldTransform(prim) * m_Xforms.GetLocalToWorldTransform(m_Source).GetInverse();
          return;
        }

        if (kind == plUsdPrimKind::Instancer || prim.IsA<pxr::UsdGeomGprim>())
        {
          ++m_uiSkipped;
          return;
        }

        // Lights and cameras end here; groups go on.
        if (kind != plUsdPrimKind::Group)
          return;

        for (const pxr::UsdPrim& child : prim.GetFilteredChildren(pxr::UsdTraverseInstanceProxies(s_Predicate)))
          Visit(child, purpose);
      }
    };

    Visitor visitor{source, ref_xforms, out_parts, inout_uiSkipped};

    // The source's own visibility and purpose are checked again with those it inherits; an instance's children are below
    // its prototype, the instance itself is not a part.
    PurposeInfo purpose = InheritedPurpose(source);
    if (source.IsInstance())
    {
      if (CheckPrim(source, purpose) != Filtered::Keep)
        return;

      for (const pxr::UsdPrim& child : source.GetFilteredChildren(pxr::UsdTraverseInstanceProxies(s_Predicate)))
        visitor.Visit(child, purpose);
    }
    else
    {
      visitor.Visit(source, purpose);
    }
  }

  /// Bound material of a gprim (group 0) and of each of its materialBind subsets, as plUsdGeometrySource::Build groups.
  void GetGroupMaterials(const pxr::UsdPrim& prim, plDynamicArray<pxr::SdfPath>& out_materials)
  {
    out_materials.Clear();

    auto bound = [](const pxr::UsdPrim& p)
    {
      const pxr::UsdShadeMaterial material = pxr::UsdShadeMaterialBindingAPI(p).ComputeBoundMaterial();
      return material ? material.GetPath() : pxr::SdfPath();
    };

    out_materials.PushBack(bound(prim));

    if (pxr::UsdGeomMesh(prim))
    {
      for (const pxr::UsdGeomSubset& subset : pxr::UsdShadeMaterialBindingAPI(prim).GetMaterialBindSubsets())
        out_materials.PushBack(bound(subset.GetPrim()));
    }
  }

  /// Appends geometry transformed by a matrix; returns the index of its first vertex. Tangents are left to the caller, as
  /// Build leaves them.
  plUInt32 AppendVertices(const plGeometry& source, const plMat4& mTransform, plGeometry& ref_target)
  {
    const plUInt32 uiFirst = ref_target.GetVertices().GetCount();
    const plMat3 mNormal = plUsdBasis::NormalMatrix(mTransform);

    for (const plGeometry::Vertex& v : source.GetVertices())
    {
      plVec3 vNormal = mNormal * v.m_vNormal;
      vNormal.NormalizeIfNotZero(plVec3(0, 0, 1)).IgnoreResult();
      ref_target.AddVertex(mTransform.TransformPosition(v.m_vPosition), vNormal, v.m_vTexCoord, v.m_Color, v.m_BoneIndices, v.m_BoneWeights);
    }

    return uiFirst;
  }

  /// A prototype's gprims built as one mesh, in the frame of an instance, one group per distinct material.
  class plUsdMergedSource final : public plUsdGeometrySource
  {
  public:
    struct Part
    {
      plUniquePtr<plUsdGeometrySource> m_pSource;
      plMat4 m_mTransform;
      plDynamicArray<plUInt32> m_GroupSlots; ///< Material slot of each of the part's groups.
    };

    plDynamicArray<Part> m_Parts;
    plUInt32 m_uiSlots = 0;

    plStatus Build(plGeometry& out_geometry, plDynamicArray<plUInt32>* out_pGroupTriangleCounts) const override
    {
      struct Built
      {
        plUInt32 m_uiFirstVertex = 0;
        plDynamicArray<plUInt32> m_GroupStart;
        plDynamicArray<plUInt32> m_GroupCount;
        bool m_bFlip = false;
      };

      plDynamicArray<plGeometry> geometries;
      plDynamicArray<Built> built;
      geometries.SetCount(m_Parts.GetCount());
      built.SetCount(m_Parts.GetCount());

      for (plUInt32 p = 0; p < m_Parts.GetCount(); ++p)
      {
        plDynamicArray<plUInt32> counts;
        if (plStatus res = m_Parts[p].m_pSource->Build(geometries[p], &counts); res.Failed())
          return res;

        Built& b = built[p];
        b.m_uiFirstVertex = AppendVertices(geometries[p], m_Parts[p].m_mTransform, out_geometry);
        b.m_bFlip = plUsdBasis::IsMirror(m_Parts[p].m_mTransform);
        b.m_GroupCount = counts;
        b.m_GroupStart.SetCount(counts.GetCount());
        plUInt32 uiStart = 0;
        for (plUInt32 g = 0; g < counts.GetCount(); ++g)
        {
          b.m_GroupStart[g] = uiStart;
          uiStart += counts[g];
        }
      }

      if (out_pGroupTriangleCounts)
      {
        out_pGroupTriangleCounts->Clear();
        out_pGroupTriangleCounts->SetCount(m_uiSlots, 0);
      }

      plHybridArray<plUInt32, 8> polygon;
      for (plUInt32 uiSlot = 0; uiSlot < m_uiSlots; ++uiSlot)
      {
        for (plUInt32 p = 0; p < m_Parts.GetCount(); ++p)
        {
          const Built& b = built[p];
          const auto& polygons = geometries[p].GetPolygons();

          for (plUInt32 g = 0; g < b.m_GroupCount.GetCount() && g < m_Parts[p].m_GroupSlots.GetCount(); ++g)
          {
            if (m_Parts[p].m_GroupSlots[g] != uiSlot)
              continue;

            for (plUInt32 t = b.m_GroupStart[g]; t < b.m_GroupStart[g] + b.m_GroupCount[g]; ++t)
            {
              polygon.Clear();
              for (plUInt32 v : polygons[t].m_Vertices)
                polygon.PushBack(b.m_uiFirstVertex + v);
              out_geometry.AddPolygon(polygon, b.m_bFlip);
            }

            if (out_pGroupTriangleCounts)
              (*out_pGroupTriangleCounts)[uiSlot] += b.m_GroupCount[g];
          }
        }
      }

      return plStatus(PL_SUCCESS);
    }

    plUInt64 GetHash() const override
    {
      plUInt64 uiHash = m_uiSlots;
      for (const Part& part : m_Parts)
      {
        uiHash = plHashingUtils::xxHash64(&part.m_mTransform, sizeof(plMat4), part.m_pSource->GetHash() + uiHash);
        uiHash = plHashingUtils::xxHash64(part.m_GroupSlots.GetData(), part.m_GroupSlots.GetCount() * sizeof(plUInt32), uiHash);
      }
      return uiHash != 0 ? uiHash : 1;
    }

    plUInt64 GetPointCount() const override
    {
      plUInt64 uiPoints = 0;
      for (const Part& part : m_Parts)
        uiPoints += part.m_pSource->GetPointCount();
      return uiPoints;
    }
  };

  /// Captures a gprim into a merged mesh at a transform; its groups share the mesh's material slots by material. A gprim
  /// without usable geometry is counted in inout_mesh instead.
  void AppendMergedPart(const plUsdBasis& basis, const pxr::UsdPrim& prim, plStringView sPath, const plMat4& mTransform,
    plMap<pxr::SdfPath, plUInt32>& inout_slots, plUsdMergedSource& inout_merged, plUsdCapturedMesh& inout_mesh)
  {
    plStatus res("not loaded");
    plUniquePtr<plUsdGeometrySource> pSource;
    if (prim)
      pSource = plUsdStageAccess::CaptureGeometry(basis, prim, res);

    if (pSource == nullptr)
    {
      if (inout_mesh.m_uiSkipped++ == 0)
      {
        plStringBuilder s;
        s.SetFormat("'{}': {}", sPath, res.m_sMessage);
        inout_mesh.m_sSkipped = s;
      }
      return;
    }

    plUsdMergedSource::Part& merged = inout_merged.m_Parts.ExpandAndGetRef();
    merged.m_pSource = std::move(pSource);
    merged.m_mTransform = mTransform;

    plDynamicArray<pxr::SdfPath> materials;
    GetGroupMaterials(prim, materials);
    for (const pxr::SdfPath& material : materials)
    {
      auto it = inout_slots.Find(material);
      if (!it.IsValid())
      {
        it = inout_slots.Insert(material, inout_mesh.m_Materials.GetCount());
        inout_mesh.m_Materials.PushBack(material.IsEmpty() ? plString() : FromStd(material.GetString()));
      }
      merged.m_GroupSlots.PushBack(it.Value());
    }
  }
} // namespace

//////////////////////////////////////////////////////////////////////////

struct plUsdPartitionScan::Impl
{
  /// A variant set on a prim.
  using SelectionKey = std::pair<pxr::SdfPath, std::string>;

  struct Selection
  {
    std::string m_sVariant;
    plUInt32 m_uiUsers = 0; ///< Loaded roots that requested it.
  };

  /// Variant selections LoadRoot made in the session layer; removed when the last root using one is released.
  std::map<SelectionKey, Selection> m_Selections;

  /// Loaded roots and the selections each requested.
  std::map<pxr::SdfPath, std::vector<SelectionKey>> m_Roots;

  /// Payloads above roots that LoadRoot loaded; unloaded once no loaded root lies below them.
  std::set<pxr::SdfPath> m_LoadedAncestors;

  /// Bounds for ListChildren, so a listing walks each subtree once; dropped whenever payloads load or unload.
  std::unique_ptr<pxr::UsdGeomBBoxCache> m_pListBounds;

  pxr::UsdGeomBBoxCache& GetListBounds()
  {
    if (m_pListBounds == nullptr)
      m_pListBounds = std::make_unique<pxr::UsdGeomBBoxCache>(s_Time, pxr::TfTokenVector{pxr::UsdGeomTokens->default_, pxr::UsdGeomTokens->render}, true);
    return *m_pListBounds;
  }

  void Clear()
  {
    m_Selections.clear();
    m_Roots.clear();
    m_LoadedAncestors.clear();
    m_pListBounds.reset();
  }

  void UnloadUnusedAncestors(const pxr::UsdStageRefPtr& stage)
  {
    pxr::SdfPathSet unload;
    for (auto it = m_LoadedAncestors.begin(); it != m_LoadedAncestors.end();)
    {
      const pxr::SdfPath& path = *it;
      const bool bUsed = std::any_of(m_Roots.begin(), m_Roots.end(), [&](const auto& root) { return root.first.HasPrefix(path); });
      if (bUsed)
      {
        ++it;
        continue;
      }

      // Unloading a parent unloads its descendants too.
      if (std::none_of(unload.begin(), unload.end(), [&](const pxr::SdfPath& parent) { return path.HasPrefix(parent); }))
        unload.insert(path);
      it = m_LoadedAncestors.erase(it);
    }

    if (!unload.empty())
      stage->LoadAndUnload(pxr::SdfPathSet(), unload);
  }
};

plUsdPartitionScan::plUsdPartitionScan()
  : m_pImpl(PL_DEFAULT_NEW(Impl))
{
}

plUsdPartitionScan::~plUsdPartitionScan()
{
  Close();
}

plStatus plUsdPartitionScan::Open(plStringView sRootLayerFile)
{
  Close();
  return m_Stage.Open(sRootLayerFile, plUsdStage::Load::None);
}

void plUsdPartitionScan::Close()
{
  m_pImpl->Clear();
  m_Stage.Close();
}

void plUsdPartitionScan::ListChildren(plStringView sPrimPath, plDynamicArray<ChildInfo>& out_children, bool bBounds, const plAtomicBool* pCancel) const
{
  out_children.Clear();

  const pxr::UsdStageRefPtr stage = plUsdStageAccess::GetStage(m_Stage);
  if (!stage)
    return;

  const pxr::UsdPrim parent = sPrimPath.IsEmpty() || sPrimPath == "/" ? stage->GetPseudoRoot() : stage->GetPrimAtPath(pxr::SdfPath(ToStd(sPrimPath)));
  if (!parent)
    return;

  for (const pxr::UsdPrim& prim : parent.GetFilteredChildren(s_Predicate))
  {
    if (pCancel != nullptr && *pCancel)
      return;

    ChildInfo& info = out_children.ExpandAndGetRef();
    info.m_sPath = FromStd(prim.GetPath().GetString());
    info.m_sName = FromStd(prim.GetName().GetString());
    info.m_sTypeName = FromStd(prim.GetTypeName().GetString());
    info.m_bHasPayload = prim.HasAuthoredPayloads();
    info.m_bLoaded = prim.IsLoaded();
    m_Stage.GetVariantSets(info.m_sPath, info.m_VariantSets);

    if (bBounds && prim.IsA<pxr::UsdGeomImageable>())
      info.m_WorldBounds = ToPlasmaBounds(GetBasis(), m_pImpl->GetListBounds().ComputeWorldBound(prim));
  }
}

plStatus plUsdPartitionScan::ComputeProxyBounds(plStringView sPrimPath, plBoundingBox& out_bounds)
{
  out_bounds = plBoundingBox::MakeInvalid();

  plDynamicArray<plUsdVariantSet> sets;
  m_Stage.GetVariantSets(sPrimPath, sets);

  plHybridArray<plUsdVariantOverride, 2> variants;
  for (const plUsdVariantSet& set : sets)
  {
    const plStringView sProxy = GetProxyVariant(set);
    if (!sProxy.IsEmpty())
      variants.PushBack({plString(), set.m_sName, plString(sProxy)});
  }

  if (variants.IsEmpty())
    return plStatus("no proxy variant");

  PL_SCOPE_EXIT(ReleaseRoot(sPrimPath));
  PL_SUCCEED_OR_RETURN(LoadRoot(sPrimPath, variants));

  const pxr::UsdStageRefPtr stage = plUsdStageAccess::GetStage(m_Stage);
  const pxr::UsdPrim prim = stage->GetPrimAtPath(pxr::SdfPath(ToStd(sPrimPath)));
  if (!prim || !prim.IsA<pxr::UsdGeomImageable>())
    return plStatus("not imageable");

  // Proxy representations are often authored with the proxy purpose.
  pxr::UsdGeomBBoxCache bounds(s_Time, {pxr::UsdGeomTokens->default_, pxr::UsdGeomTokens->render, pxr::UsdGeomTokens->proxy}, true);
  out_bounds = ToPlasmaBounds(GetBasis(), bounds.ComputeWorldBound(prim));
  return plStatus(PL_SUCCESS);
}

namespace
{
  plStringView FindVariant(const plUsdVariantSet& set, std::initializer_list<const char*> names)
  {
    for (const char* szName : names)
    {
      for (const plString& sVariant : set.m_Variants)
      {
        if (sVariant.IsEqual_NoCase(szName))
          return sVariant;
      }
    }
    return {};
  }
} // namespace

plStringView plUsdPartitionScan::GetPreferredVariant(const plUsdVariantSet& set)
{
  return FindVariant(set, {"full", "high", "render"});
}

plStringView plUsdPartitionScan::GetProxyVariant(const plUsdVariantSet& set)
{
  return FindVariant(set, {"proxy", "low", "preview"});
}

plStatus plUsdPartitionScan::LoadRoot(plStringView sRootPath, plArrayPtr<const plUsdVariantOverride> variants)
{
  const pxr::UsdStageRefPtr stage = plUsdStageAccess::GetStage(m_Stage);
  if (!stage)
    return plStatus("no stage is open");

  const pxr::SdfPath rootPath(ToStd(sRootPath));
  if (!rootPath.IsAbsolutePath() || !rootPath.IsPrimPath())
    return plStatus(plFmt("'{}' is not an absolute prim path", sRootPath));

  Impl& impl = *m_pImpl;
  impl.m_pListBounds.reset();
  const pxr::SdfPathSet loadedBefore = stage->GetLoadSet();

  // Payloads above the root that this call loaded; Load loads a path's ancestors too.
  auto noteAncestors = [&]()
  {
    for (pxr::SdfPath path = rootPath.GetParentPath(); !path.IsEmpty() && !path.IsAbsoluteRootPath(); path = path.GetParentPath())
    {
      const pxr::UsdPrim prim = stage->GetPrimAtPath(path);
      if (prim && prim.HasAuthoredPayloads() && prim.IsLoaded() && loadedBefore.count(path) == 0)
        impl.m_LoadedAncestors.insert(path);
    }
  };

  // A root below an unloaded payload appears once its ancestors are loaded.
  if (!stage->GetPrimAtPath(rootPath))
    stage->Load(rootPath, pxr::UsdLoadWithoutDescendants);
  if (!stage->GetPrimAtPath(rootPath))
  {
    noteAncestors();
    impl.UnloadUnusedAncestors(stage);
    return plStatus(plFmt("the stage has no prim '{}'", sRootPath));
  }

  std::vector<Impl::SelectionKey>& requested = impl.m_Roots[rootPath];
  plHybridArray<bool, 4> applied;
  applied.SetCount(variants.GetCount(), false);

  auto apply = [&]()
  {
    pxr::UsdEditContext context(stage, stage->GetSessionLayer());

    for (plUInt32 i = 0; i < variants.GetCount(); ++i)
    {
      if (applied[i])
        continue;

      const plUsdVariantOverride& v = variants[i];
      const pxr::SdfPath path = v.m_sPrimPath.IsEmpty() ? rootPath : pxr::SdfPath(ToStd(v.m_sPrimPath));
      const pxr::UsdPrim prim = stage->GetPrimAtPath(path);
      const std::string sSet = ToStd(v.m_sSet);
      if (!prim || !prim.GetVariantSets().HasVariantSet(sSet))
        continue;

      pxr::UsdVariantSet set = prim.GetVariantSet(sSet);
      const std::string sVariant = ToStd(v.m_sVariant);
      applied[i] = true;
      if (!set.HasAuthoredVariant(sVariant))
        continue;

      const Impl::SelectionKey key(path, sSet);
      if (auto it = impl.m_Selections.find(key); it != impl.m_Selections.end())
      {
        // Another loaded root made this selection; changing it would recompose that root under its scan.
        if (it->second.m_sVariant != sVariant)
        {
          plLog::Warning("'{}': variant set '{}' stays '{}', a loaded root uses it", path.GetString().c_str(), sSet.c_str(), it->second.m_sVariant.c_str());
          continue;
        }

        if (std::find(requested.begin(), requested.end(), key) == requested.end())
        {
          ++it->second.m_uiUsers;
          requested.push_back(key);
        }
        continue;
      }

      if (set.GetVariantSelection() == sVariant)
        continue;

      if (set.SetVariantSelection(sVariant))
      {
        impl.m_Selections[key] = {sVariant, 1};
        requested.push_back(key);
      }
    }
  };

  // Overrides on prims inside the root's payloads apply once those are loaded.
  apply();
  stage->Load(rootPath, pxr::UsdLoadWithDescendants);
  apply();
  noteAncestors();

  return plStatus(PL_SUCCESS);
}

void plUsdPartitionScan::ReleaseRoot(plStringView sRootPath)
{
  const pxr::UsdStageRefPtr stage = plUsdStageAccess::GetStage(m_Stage);
  if (!stage)
    return;

  Impl& impl = *m_pImpl;
  impl.m_pListBounds.reset();
  const pxr::SdfPath rootPath(ToStd(sRootPath));
  if (!rootPath.IsAbsolutePath() || !rootPath.IsPrimPath())
    return;

  // Selections go before the unload, in one change block: on Caldera's capital this releases in 5.9 s, unloading first
  // takes 8.4 s.
  if (auto it = impl.m_Roots.find(rootPath); it != impl.m_Roots.end())
  {
    {
      const pxr::SdfLayerHandle session = stage->GetSessionLayer();
      pxr::SdfChangeBlock block;

      for (const Impl::SelectionKey& key : it->second)
      {
        auto selection = impl.m_Selections.find(key);
        if (selection == impl.m_Selections.end() || --selection->second.m_uiUsers > 0)
          continue;

        if (const pxr::SdfPrimSpecHandle spec = session->GetPrimAtPath(key.first))
          spec->GetVariantSelections().erase(key.second);
        impl.m_Selections.erase(selection);
      }
    }

    impl.m_Roots.erase(it);
  }

  stage->Unload(rootPath);
  impl.UnloadUnusedAncestors(stage);
}

void plUsdPartitionScan::AddPreferredVariants(plStringView sRootPath, plDynamicArray<plUsdVariantOverride>& inout_variants)
{
  plDynamicArray<plUsdVariantSet> sets;
  m_Stage.GetVariantSets(sRootPath, sets);

  for (const plUsdVariantSet& set : sets)
  {
    bool bNamed = false;
    for (const plUsdVariantOverride& v : inout_variants)
      bNamed |= (v.m_sPrimPath.IsEmpty() || v.m_sPrimPath == sRootPath) && v.m_sSet == set.m_sName;
    if (bNamed)
      continue;

    const plStringView sPreferred = GetPreferredVariant(set);
    if (!sPreferred.IsEmpty())
    {
      plUsdVariantOverride& o = inout_variants.ExpandAndGetRef();
      o.m_sSet = set.m_sName;
      o.m_sVariant = sPreferred;
    }
  }
}

namespace
{
  /// Collects one root's placeables.
  struct RootWalker
  {
    const plUsdBasis& m_Basis;
    const plUsdScanOptions& m_Options;
    plUsdScanResult& m_Result;
    plUsdScanRoot& m_Root;
    plUInt32 m_uiRoot = 0;
    plUInt64 m_uiSeed = 0;
    pxr::UsdGeomXformCache m_Xforms;
    pxr::UsdGeomBBoxCache m_Bounds;
    std::vector<pxr::SdfPath> m_Excluded;
    bool m_bCancelled = false;

    /// A USD prototype (or point instancer prototype) and the material bindings its instance inherits from outside it.
    using SessionKey = std::pair<pxr::SdfPath, plUInt64>;

    struct SessionKeyHash
    {
      size_t operator()(const SessionKey& key) const { return pxr::SdfPath::Hash()(key.first) ^ (size_t)(key.second * 0x9e3779b97f4a7c15ull); }
    };

    /// This root's prototypes -> index into m_Result.m_Prototypes, or plInvalidIndex when they draw nothing.
    std::unordered_map<SessionKey, plUInt32, SessionKeyHash> m_SessionPrototypes;

    /// BindingSignature of the ancestors of instances.
    std::unordered_map<pxr::SdfPath, plUInt64, pxr::SdfPath::Hash> m_AncestorBindings;

    /// Layer files read for file stamps during this scan.
    plUsdStageAccess::LayerStamps m_LayerStamps;

    RootWalker(const plUsdBasis& basis, const plUsdScanOptions& options, plUsdScanResult& ref_result, plUInt32 uiRoot)
      : m_Basis(basis)
      , m_Options(options)
      , m_Result(ref_result)
      , m_Root(ref_result.m_Roots[uiRoot])
      , m_uiRoot(uiRoot)
      , m_uiSeed(plUsdStageAccess::BasisSeed(basis))
      , m_Xforms(s_Time)
      , m_Bounds(s_Time, {pxr::UsdGeomTokens->default_, pxr::UsdGeomTokens->render}, false)
    {
      for (const plString& sPath : options.m_ExcludePaths)
      {
        const pxr::SdfPath path(ToStd(sPath));
        if (path.IsAbsolutePath() && path.IsPrimPath())
          m_Excluded.push_back(path);
      }
    }

    bool IsExcluded(const pxr::SdfPath& path, bool bWithDescendants) const
    {
      return std::any_of(m_Excluded.begin(), m_Excluded.end(), [&](const pxr::SdfPath& excluded) { return bWithDescendants ? path.HasPrefix(excluded) : path == excluded; });
    }

    /// Checks for cancellation and reports progress; called every few thousand prims.
    bool Cancelled()
    {
      if (m_Options.m_pCancel != nullptr && *m_Options.m_pCancel)
        m_bCancelled = true;
      else if (m_Options.m_OnProgress.IsValid())
        m_Options.m_OnProgress(false, m_Root.m_uiPrims);
      return m_bCancelled;
    }

    plUInt32 AddPath(const pxr::SdfPath& path)
    {
      const std::string& sPath = path.GetString();
      return m_Result.AddPath(plStringView(sPath.data(), (plUInt32)sPath.size()));
    }

    plUsdPlaceable& AddPlaceable(const pxr::UsdPrim& prim, plUsdPlaceableKind kind)
    {
      plUsdPlaceable& p = m_Result.m_Placeables.ExpandAndGetRef();
      p.m_uiPath = AddPath(prim.GetPath());
      p.m_Kind = kind;
      p.m_uiRoot = m_uiRoot;
      return p;
    }

    /// Hash of the material bindings authored on a prim itself: direct, collection and purpose-specific ones.
    static plUInt64 OwnBindings(const pxr::UsdPrim& prim, plUInt64 uiSeed)
    {
      plUInt64 uiHash = uiSeed;
      for (const pxr::UsdProperty& property : prim.GetAuthoredPropertiesInNamespace("material"))
      {
        const pxr::UsdRelationship rel = property.As<pxr::UsdRelationship>();
        if (!rel || !pxr::TfStringStartsWith(rel.GetName().GetString(), "material:binding"))
          continue;

        uiHash = HashString(rel.GetName().GetString(), uiHash);
        uiHash = HashString(pxr::UsdShadeMaterialBindingAPI::GetMaterialBindingStrength(rel).GetString(), uiHash);

        pxr::SdfPathVector targets;
        rel.GetTargets(&targets);
        for (const pxr::SdfPath& target : targets)
          uiHash = HashString(target.GetString(), uiHash);
      }
      return uiHash;
    }

    /// Hash of the material bindings authored on an instance and its ancestors. Bindings inside a prototype are the same
    /// for all its instances; these may differ per instance and change what its gprims are bound to.
    plUInt64 BindingSignature(const pxr::UsdPrim& instance)
    {
      // Ancestors first, cached: siblings share them.
      std::vector<pxr::UsdPrim> uncached;
      plUInt64 uiHash = 0;
      for (pxr::UsdPrim parent = instance.GetParent(); parent && !parent.IsPseudoRoot(); parent = parent.GetParent())
      {
        if (auto it = m_AncestorBindings.find(parent.GetPath()); it != m_AncestorBindings.end())
        {
          uiHash = it->second;
          break;
        }
        uncached.push_back(parent);
      }

      for (auto it = uncached.rbegin(); it != uncached.rend(); ++it)
      {
        uiHash = OwnBindings(*it, uiHash);
        m_AncestorBindings[it->GetPath()] = uiHash;
      }

      return OwnBindings(instance, uiHash);
    }

    plTransform WorldTransform(const pxr::UsdPrim& prim)
    {
      return plUsdBasis::MatrixToTransform(ToPlasmaMatrix(m_Basis, m_Xforms.GetLocalToWorldTransform(prim)));
    }

    /// Index of the prototype a source prim draws, adding it the first time any root meets its key, and the source to the
    /// prototype the first time this root meets it.
    plUInt32 GetPrototype(const pxr::UsdPrim& source, const SessionKey& sessionKey, bool bPointInstancer)
    {
      if (auto it = m_SessionPrototypes.find(sessionKey); it != m_SessionPrototypes.end())
        return it->second;

      const plTime start = plTime::Now();
      PL_SCOPE_EXIT(m_Root.m_PrototypeTime += plTime::Now() - start);

      plDynamicArray<PrototypePart> parts;
      CollectParts(source, m_Xforms, parts, m_Root.m_uiSkippedUnsupported);

      plUInt32 uiIndex = plInvalidIndex;
      if (!parts.IsEmpty())
      {
        const std::string& sSource = source.GetPath().GetString();
        plUInt64 uiKey = plHashingUtils::xxHash64(&bPointInstancer, sizeof(bool), m_uiSeed);
        plUInt64 uiFileStamp = 0;
        plUInt64 uiPoints = 0;
        plDynamicArray<pxr::SdfPath> materials;

        for (const PrototypePart& part : parts)
        {
          // Paths inside the prototype relative to it: instance proxy paths differ per instance.
          const std::string& sPart = part.m_Prim.GetPath().GetString();
          uiKey = HashString(sPart.substr(plMath::Min(sSource.size(), sPart.size())), uiKey);
          uiKey = plUsdStageAccess::HashGeometrySpecs(part.m_Prim, uiKey, m_LayerStamps, uiFileStamp);
          uiKey = plHashingUtils::xxHash64(part.m_mRelative.GetArray(), 16 * sizeof(double), uiKey);

          GetGroupMaterials(part.m_Prim, materials);
          for (const pxr::SdfPath& material : materials)
          {
            const std::string& sMaterial = material.GetString();
            uiKey = HashString(sMaterial.compare(0, sSource.size(), sSource) == 0 ? sMaterial.substr(sSource.size()) : sMaterial, uiKey + 1);
          }

          pxr::VtVec3fArray points;
          if (pxr::UsdGeomMesh mesh(part.m_Prim); mesh && mesh.GetPointsAttr().Get(&points, s_Time))
            uiPoints += points.size();
        }

        uiKey = uiKey != 0 ? uiKey : 1;

        if (!m_Result.m_PrototypeByKey.TryGetValue(uiKey, uiIndex))
        {
          uiIndex = m_Result.m_Prototypes.GetCount();
          m_Result.m_PrototypeByKey.Insert(uiKey, uiIndex);
          ++m_Root.m_uiNewPrototypes;

          plUsdScanPrototype& proto = m_Result.m_Prototypes.ExpandAndGetRef();
          proto.m_uiKey = uiKey;
          proto.m_uiFileStamp = uiFileStamp;
          proto.m_sName = FromStd(source.GetName().GetString());
          proto.m_bPointInstancer = bPointInstancer;
          proto.m_LocalBounds = ToPlasmaBounds(m_Basis, m_Bounds.ComputeUntransformedBound(source));
          proto.m_uiGprims = parts.GetCount();
          proto.m_uiPoints = uiPoints;
        }

        plUsdScanPrototype& proto = m_Result.m_Prototypes[uiIndex];
        if (proto.FindSource(m_uiRoot) == nullptr)
          proto.m_Sources.PushBack({m_uiRoot, FromStd(sSource)});
      }
      else
      {
        ++m_Root.m_uiSkippedUnsupported;
      }

      m_SessionPrototypes[sessionKey] = uiIndex;
      return uiIndex;
    }

    void AddInstance(const pxr::UsdPrim& prim)
    {
      const plUInt32 uiPrototype = GetPrototype(prim, SessionKey(prim.GetPrototype().GetPath(), BindingSignature(prim)), false);
      if (uiPrototype == plInvalidIndex)
        return;

      plUsdPlaceable& p = AddPlaceable(prim, plUsdPlaceableKind::Instance);
      p.m_uiPrototype = uiPrototype;
      p.m_WorldTransform = WorldTransform(prim);
      p.m_WorldBounds = WorldBounds(prim);
      ++m_Result.m_Prototypes[uiPrototype].m_uiUseCount;
    }

    plBoundingBox WorldBounds(const pxr::UsdPrim& prim)
    {
      const plTime start = plTime::Now();
      const plBoundingBox bounds = ToPlasmaBounds(m_Basis, m_Bounds.ComputeWorldBound(prim));
      m_Root.m_BoundsTime += plTime::Now() - start;
      return bounds;
    }

    void AddGprim(const pxr::UsdPrim& prim)
    {
      plUsdPlaceable& p = AddPlaceable(prim, plUsdPlaceableKind::Gprim);
      p.m_WorldTransform = WorldTransform(prim);
      p.m_WorldBounds = WorldBounds(prim);

      if (m_Options.m_bGeometryKeys)
      {
        const plTime start = plTime::Now();
        p.m_uiGeometryKey = plUsdStageAccess::HashGeometrySpecs(prim, m_uiSeed, m_LayerStamps, p.m_uiFileStamp) | 1;
        m_Root.m_KeyTime += plTime::Now() - start;
      }
    }

    void AddPointInstances(const pxr::UsdPrim& prim)
    {
      const pxr::UsdGeomPointInstancer instancer(prim);

      pxr::SdfPathVector prototypes;
      instancer.GetPrototypesRel().GetForwardedTargets(&prototypes);

      // The prototypes' own transforms are in the instance transforms, as OpenUSD applies them.
      pxr::VtIntArray protoIndices;
      pxr::VtArray<pxr::GfMatrix4d> instances;
      if (prototypes.empty() || !instancer.GetProtoIndicesAttr().Get(&protoIndices, s_Time) ||
          !instancer.ComputeInstanceTransformsAtTime(&instances, s_Time, s_Time, pxr::UsdGeomPointInstancer::IncludeProtoXform, pxr::UsdGeomPointInstancer::IgnoreMask) ||
          instances.size() != protoIndices.size())
      {
        ++m_Root.m_uiSkippedUnsupported;
        return;
      }

      const std::vector<bool> mask = instancer.ComputeMaskAtTime(s_Time);

      plDynamicArray<plUInt32> protoSlots;
      protoSlots.SetCount((plUInt32)prototypes.size(), plInvalidIndex);
      for (size_t i = 0; i < prototypes.size(); ++i)
      {
        if (const pxr::UsdPrim proto = prim.GetStage()->GetPrimAtPath(prototypes[i]); proto && proto.IsActive())
          protoSlots[(plUInt32)i] = GetPrototype(proto, SessionKey(prototypes[i], 0), true);
      }

      std::vector<int64_t> ids(instances.size());
      for (size_t i = 0; i < ids.size(); ++i)
        ids[i] = (int64_t)i;

      std::vector<pxr::GfBBox3d> bounds(instances.size());
      if (!ids.empty())
        m_Bounds.ComputePointInstanceWorldBounds(instancer, ids.data(), ids.size(), bounds.data());

      const pxr::GfMatrix4d mWorld = m_Xforms.GetLocalToWorldTransform(prim);
      const plUInt32 uiPath = AddPath(prim.GetPath());

      for (size_t i = 0; i < instances.size(); ++i)
      {
        const int iProto = protoIndices[i];
        if ((!mask.empty() && !mask[i]) || iProto < 0 || (size_t)iProto >= prototypes.size() || protoSlots[iProto] == plInvalidIndex)
          continue;

        plUsdPlaceable& p = m_Result.m_Placeables.ExpandAndGetRef();
        p.m_uiPath = uiPath;
        p.m_uiElement = (plUInt32)i;
        p.m_Kind = plUsdPlaceableKind::PointInstance;
        p.m_uiRoot = m_uiRoot;
        p.m_uiPrototype = protoSlots[iProto];
        p.m_WorldTransform = plUsdBasis::MatrixToTransform(ToPlasmaMatrix(m_Basis, instances[i] * mWorld));
        p.m_WorldBounds = ToPlasmaBounds(m_Basis, bounds[i]);
        ++m_Result.m_Prototypes[p.m_uiPrototype].m_uiUseCount;
      }
    }

    void Visit(const pxr::UsdPrim& prim, PurposeInfo purpose)
    {
      if ((++m_Root.m_uiPrims & 4095) == 0 && Cancelled())
        return;

      if (!IsTraversable(prim))
        return;

      if (!m_Excluded.empty() && IsExcluded(prim.GetPath(), false))
      {
        ++m_Root.m_uiSkippedExcluded;
        return;
      }

      const Filtered filtered = CheckPrim(prim, purpose);
      if (filtered == Filtered::Invisible)
      {
        ++m_Root.m_uiSkippedInvisible;
        return;
      }

      if (filtered == Filtered::Purpose)
      {
        ++m_Root.m_uiSkippedPurpose;
        return;
      }

      const plUsdPrimKind kind = plUsdStageAccess::GetKind(prim);

      if (prim.IsInstance())
      {
        AddInstance(prim);
        return;
      }

      switch (kind)
      {
        case plUsdPrimKind::Mesh:
        case plUsdPrimKind::Gprim:
          AddGprim(prim);
          return;

        case plUsdPrimKind::Instancer:
          AddPointInstances(prim);
          return;

        case plUsdPrimKind::Light:
        case plUsdPrimKind::Camera:
          if (m_Options.m_bLightsAndCameras)
          {
            plUsdPlaceable& p = AddPlaceable(prim, kind == plUsdPrimKind::Light ? plUsdPlaceableKind::Light : plUsdPlaceableKind::Camera);
            p.m_WorldTransform = WorldTransform(prim);
          }
          return;

        case plUsdPrimKind::Group:
          break;
      }

      // Points, curves, volumes.
      if (prim.IsA<pxr::UsdGeomGprim>())
      {
        ++m_Root.m_uiSkippedUnsupported;
        return;
      }

      VisitChildren(prim, purpose);
    }

    void VisitChildren(const pxr::UsdPrim& prim, const PurposeInfo& purpose)
    {
      for (const pxr::UsdPrim& child : prim.GetFilteredChildren(s_Predicate))
      {
        if (m_bCancelled)
          return;
        Visit(child, purpose);
      }
    }
  };

  /// Unloaded payloads at and below a prim, counting up to uiLimit + 1.
  plUInt32 CountUnloadedPayloads(const pxr::UsdPrim& root, plUInt32 uiLimit)
  {
    plUInt32 uiCount = 0;
    for (const pxr::UsdPrim& prim : pxr::UsdPrimRange(root, s_Predicate))
    {
      if (prim.HasAuthoredPayloads() && !prim.IsLoaded() && ++uiCount > uiLimit)
        break;
    }
    return uiCount;
  }
} // namespace

plStatus plUsdPartitionScan::ScanRoot(plStringView sRootPath, const plUsdScanOptions& options, plUsdScanResult& inout_result)
{
  const pxr::UsdStageRefPtr stage = plUsdStageAccess::GetStage(m_Stage);
  if (!stage)
    return plStatus("no stage is open");

  auto isCancelled = [&]() { return options.m_pCancel != nullptr && *options.m_pCancel; };

  const pxr::SdfPath rootPath(ToStd(sRootPath));
  if (options.m_uiMaxRootPayloads > 0 && rootPath.IsAbsolutePath() && rootPath.IsPrimPath())
  {
    if (const pxr::UsdPrim prim = stage->GetPrimAtPath(rootPath); prim && CountUnloadedPayloads(prim, options.m_uiMaxRootPayloads) > options.m_uiMaxRootPayloads)
    {
      return plStatus(plFmt("'{}' has more than {} unloaded payloads below it; choose prims further down as roots", sRootPath, options.m_uiMaxRootPayloads));
    }
  }

  if (options.m_OnProgress.IsValid())
    options.m_OnProgress(true, 0);

  plTime start = plTime::Now();
  PL_SUCCEED_OR_RETURN(LoadRoot(sRootPath, options.m_Variants));
  const plTime loadTime = plTime::Now() - start;

  if (isCancelled())
    return plStatus("cancelled");

  const pxr::UsdPrim root = stage->GetPrimAtPath(rootPath);

  // To leave inout_result as it was when cancelled.
  const plUInt32 uiFirstPrototype = inout_result.m_Prototypes.GetCount();
  const plUInt32 uiFirstPath = inout_result.m_PathStarts.GetCount();
  const plUInt32 uiPathText = inout_result.m_PathText.GetCount();

  const plUInt32 uiRoot = inout_result.m_Roots.GetCount();
  plUsdScanRoot& scanRoot = inout_result.m_Roots.ExpandAndGetRef();
  scanRoot.m_sPath = sRootPath;
  scanRoot.m_uiFirstPlaceable = inout_result.m_Placeables.GetCount();
  scanRoot.m_LoadTime = loadTime;

  start = plTime::Now();
  bool bCancelled = false;
  {
    RootWalker walker(GetBasis(), options, inout_result, uiRoot);

    // Purpose and visibility are inherited from above the root as well, decided top down.
    std::vector<pxr::UsdPrim> ancestors;
    for (pxr::UsdPrim parent = root.GetParent(); parent && !parent.IsPseudoRoot(); parent = parent.GetParent())
      ancestors.push_back(parent);

    PurposeInfo purpose;
    bool bHidden = false;
    for (auto it = ancestors.rbegin(); it != ancestors.rend() && !bHidden; ++it)
      bHidden = CheckPrim(*it, purpose) == Filtered::Invisible;

    const bool bExcluded = walker.IsExcluded(rootPath, true);
    if (bExcluded)
      ++scanRoot.m_uiSkippedExcluded;
    else if (!bHidden)
      walker.Visit(root, purpose);

    bCancelled = walker.m_bCancelled;
    if (!bCancelled && !bExcluded)
      scanRoot.m_WorldBounds = ToPlasmaBounds(GetBasis(), walker.m_Bounds.ComputeWorldBound(root));
  }

  if (bCancelled)
  {
    for (plUInt32 i = scanRoot.m_uiFirstPlaceable; i < inout_result.m_Placeables.GetCount(); ++i)
    {
      const plUsdPlaceable& p = inout_result.m_Placeables[i];
      if (p.m_uiPrototype != plInvalidIndex)
        --inout_result.m_Prototypes[p.m_uiPrototype].m_uiUseCount;
    }

    for (plUInt32 i = uiFirstPrototype; i < inout_result.m_Prototypes.GetCount(); ++i)
      inout_result.m_PrototypeByKey.Remove(inout_result.m_Prototypes[i].m_uiKey);

    inout_result.m_Prototypes.SetCount(uiFirstPrototype);
    for (plUsdScanPrototype& proto : inout_result.m_Prototypes)
    {
      if (!proto.m_Sources.IsEmpty() && proto.m_Sources.PeekBack().m_uiRoot == uiRoot)
        proto.m_Sources.PopBack();
    }

    inout_result.m_Placeables.SetCount(scanRoot.m_uiFirstPlaceable);
    inout_result.m_PathStarts.SetCount(uiFirstPath);
    inout_result.m_PathText.SetCount(uiPathText);
    inout_result.m_Roots.PopBack();
    return plStatus("cancelled");
  }

  scanRoot.m_uiPlaceableCount = inout_result.m_Placeables.GetCount() - scanRoot.m_uiFirstPlaceable;
  scanRoot.m_ScanTime = plTime::Now() - start;
  return plStatus(PL_SUCCESS);
}

plStatus plUsdPartitionScan::CapturePrototype(const plUsdScanPrototype& prototype, plUInt32 uiRoot, plUsdCapturedMesh& out_mesh) const
{
  out_mesh = {};

  const plUsdPrototypeSource* pSourceInRoot = prototype.FindSource(uiRoot);
  if (pSourceInRoot == nullptr)
    return plStatus(plFmt("prototype '{}' is not used in root {}", prototype.m_sName, uiRoot));

  const plString& sSourcePath = pSourceInRoot->m_sPath;
  const pxr::UsdStageRefPtr stage = plUsdStageAccess::GetStage(m_Stage);
  const pxr::UsdPrim source = stage ? stage->GetPrimAtPath(pxr::SdfPath(ToStd(sSourcePath))) : pxr::UsdPrim();
  if (!source || (!prototype.m_bPointInstancer && !source.IsInstance()))
    return plStatus(plFmt("'{}' is not loaded", sSourcePath));

  pxr::UsdGeomXformCache xforms(s_Time);
  plDynamicArray<PrototypePart> parts;
  plUInt32 uiSkipped = 0;
  CollectParts(source, xforms, parts, uiSkipped);
  if (parts.IsEmpty())
    return plStatus(plFmt("'{}' draws nothing", sSourcePath));

  plUniquePtr<plUsdMergedSource> pMerged = PL_DEFAULT_NEW(plUsdMergedSource);
  plMap<pxr::SdfPath, plUInt32> slots;

  for (const PrototypePart& part : parts)
  {
    const std::string& sPart = part.m_Prim.GetPath().GetString();
    AppendMergedPart(GetBasis(), part.m_Prim, plStringView(sPart.data(), (plUInt32)sPart.size()), plUsdBasis::ToFloat(ToPlasmaMatrix(GetBasis(), part.m_mRelative)),
      slots, *pMerged, out_mesh);
  }

  if (pMerged->m_Parts.IsEmpty())
    return plStatus(plFmt("no gprim of '{}' has usable geometry ({})", sSourcePath, out_mesh.m_sSkipped));

  pMerged->m_uiSlots = out_mesh.m_Materials.GetCount();
  out_mesh.m_pSource = std::move(pMerged);
  return plStatus(PL_SUCCESS);
}

plStatus plUsdPartitionScan::CaptureGprim(plStringView sPrimPath, plUsdCapturedMesh& out_mesh) const
{
  out_mesh = {};

  const pxr::UsdStageRefPtr stage = plUsdStageAccess::GetStage(m_Stage);
  const pxr::UsdPrim prim = stage ? stage->GetPrimAtPath(pxr::SdfPath(ToStd(sPrimPath))) : pxr::UsdPrim();
  if (!prim)
    return plStatus(plFmt("'{}' is not loaded", sPrimPath));

  plStatus res;
  out_mesh.m_pSource = plUsdStageAccess::CaptureGeometry(GetBasis(), prim, res);
  if (out_mesh.m_pSource == nullptr)
    return res;

  plDynamicArray<pxr::SdfPath> materials;
  GetGroupMaterials(prim, materials);
  for (const pxr::SdfPath& material : materials)
    out_mesh.m_Materials.PushBack(material.IsEmpty() ? plString() : FromStd(material.GetString()));

  return plStatus(PL_SUCCESS);
}

plStatus plUsdPartitionScan::CaptureGprims(plArrayPtr<const plStringView> primPaths, plArrayPtr<const plMat4> transforms, plUsdCapturedMesh& out_mesh) const
{
  out_mesh = {};

  if (primPaths.GetCount() != transforms.GetCount())
    return plStatus("one transform per gprim is needed");

  const pxr::UsdStageRefPtr stage = plUsdStageAccess::GetStage(m_Stage);
  if (!stage)
    return plStatus("no stage is open");

  plUniquePtr<plUsdMergedSource> pMerged = PL_DEFAULT_NEW(plUsdMergedSource);
  plMap<pxr::SdfPath, plUInt32> slots;

  for (plUInt32 i = 0; i < primPaths.GetCount(); ++i)
  {
    const pxr::UsdPrim prim = stage->GetPrimAtPath(pxr::SdfPath(ToStd(primPaths[i])));
    AppendMergedPart(GetBasis(), prim, primPaths[i], transforms[i], slots, *pMerged, out_mesh);
  }

  if (pMerged->m_Parts.IsEmpty())
    return plStatus(plFmt("none of the gprims has usable geometry ({})", out_mesh.m_sSkipped));

  pMerged->m_uiSlots = out_mesh.m_Materials.GetCount();
  out_mesh.m_pSource = std::move(pMerged);
  return plStatus(PL_SUCCESS);
}

void plUsdPartitionScan::GetGprimMaterials(plArrayPtr<const plStringView> primPaths, plDynamicArray<plString>& out_materials) const
{
  out_materials.Clear();

  const pxr::UsdStageRefPtr stage = plUsdStageAccess::GetStage(m_Stage);
  plDynamicArray<pxr::SdfPath> materials;
  for (const plStringView sPath : primPaths)
  {
    const pxr::UsdPrim prim = stage ? stage->GetPrimAtPath(pxr::SdfPath(ToStd(sPath))) : pxr::UsdPrim();
    if (!prim)
    {
      out_materials.PushBack(plString());
      continue;
    }

    GetGroupMaterials(prim, materials);
    for (const pxr::SdfPath& material : materials)
      out_materials.PushBack(material.IsEmpty() ? plString() : FromStd(material.GetString()));
  }
}

plVec2I32 plUsdPartitionScan::CellOf(const plVec3& vPosition, float fCellSize)
{
  // Clamped so the cell index fits an int32; non-finite coordinates go to cell 0.
  const float fLimit = plMath::Max(fCellSize, plWorldPartitionGrid::s_fMinCellSize) * 1.0e9f;
  auto clamp = [&](float f) { return plMath::IsFinite(f) ? plMath::Clamp(f, -fLimit, fLimit) : 0.0f; };

  plVec2I32 cell;
  plWorldPartitionGrid::CellOf(plVec3(clamp(vPosition.x), clamp(vPosition.y), 0.0f), fCellSize, cell.x, cell.y);
  return cell;
}

plBoundingBox plUsdPartitionScan::CellRect(const plVec2I32& cell, float fCellSize)
{
  return plWorldPartitionGrid::CellRect(cell.x, cell.y, fCellSize);
}

plUInt64 plUsdPartitionScan::PackCellKey(const plVec2I32& cell)
{
  return plWorldPartitionGrid::PackKey(cell.x, cell.y);
}

plVec2I32 plUsdPartitionScan::UnpackCellKey(plUInt64 uiKey)
{
  plVec2I32 cell;
  plWorldPartitionGrid::UnpackKey(uiKey, cell.x, cell.y);
  return cell;
}

plStatus plUsdPartitionScan::SplitByCell(const plGeometry& geometry, plArrayPtr<const plUInt32> groupTriangleCounts, const plMat4& mTransform, float fCellSize,
  plUInt32 uiMaxCells, plDynamicArray<plUsdCellPiece>& out_pieces)
{
  out_pieces.Clear();

  plGeometry world;
  AppendVertices(geometry, mTransform, world);
  const bool bFlip = plUsdBasis::IsMirror(mTransform);
  const auto& vertices = world.GetVertices();
  const float fSize = plMath::Max(fCellSize, plWorldPartitionGrid::s_fMinCellSize);

  // Bounded work: every triangle is clipped against at most the cells of the whole geometry.
  plBoundingBox bounds = plBoundingBox::MakeInvalid();
  for (const plGeometry::Vertex& v : vertices)
  {
    if (!v.m_vPosition.IsValid())
      return plStatus("the geometry has non-finite positions");
    bounds.ExpandToInclude(v.m_vPosition);
  }

  if (!bounds.IsValid())
    return plStatus(PL_SUCCESS);

  const plVec2I32 b0 = CellOf(bounds.m_vMin, fSize);
  const plVec2I32 b1 = CellOf(bounds.m_vMax, fSize);
  const plUInt64 uiCells = static_cast<plUInt64>((plInt64)b1.x - b0.x + 1) * static_cast<plUInt64>((plInt64)b1.y - b0.y + 1);
  if (uiCells > uiMaxCells)
    return plStatus(plFmt("the geometry covers {} cells, more than {}", uiCells, uiMaxCells));

  /// A vertex of a clipped triangle and the source triangle's edges it lies on.
  struct ClipVertex
  {
    plGeometry::Vertex m_Vertex;
    plUInt8 m_uiEdges = 0; ///< Bit e: on the edge from corner e to corner (e + 1) % 3; none for a grid corner inside.
  };

  using Poly = plHybridArray<ClipVertex, 8>;

  /// What makes two piece vertices one.
  struct VertexKey
  {
    PL_DECLARE_POD_TYPE();

    plVec3 m_vPosition;
    plVec3 m_vNormal;
    plVec2 m_vTexCoord;
    plColor m_Color;
  };

  struct VertexKeyHash
  {
    static plUInt32 Hash(const VertexKey& key) { return plHashingUtils::xxHash32(&key, sizeof(VertexKey)); }
    static bool Equal(const VertexKey& a, const VertexKey& b) { return plMemoryUtils::IsEqual(&a, &b); }
  };

  plHashTable<plUInt64, plUInt32> pieceByCell;
  plDynamicArray<plHashTable<VertexKey, plUInt32, VertexKeyHash>> welds;

  // Where a source edge crosses a grid line, from its endpoints in a fixed order: every piece and every triangle sharing
  // the edge gets the same vertex.
  auto crossing = [](const plGeometry::Vertex& v0, const plGeometry::Vertex& v1, plUInt32 uiAxis, float fLine)
  {
    const plVec3& p0 = v0.m_vPosition;
    const plVec3& p1 = v1.m_vPosition;
    const bool bSwap = p1.x < p0.x || (p1.x == p0.x && (p1.y < p0.y || (p1.y == p0.y && p1.z < p0.z)));
    const plGeometry::Vertex& a = bSwap ? v1 : v0;
    const plGeometry::Vertex& b = bSwap ? v0 : v1;

    const float fA = a.m_vPosition.GetData()[uiAxis];
    const float fB = b.m_vPosition.GetData()[uiAxis];
    const float t = fB != fA ? (fLine - fA) / (fB - fA) : 0.0f;

    plGeometry::Vertex v = a;
    v.m_vPosition = plMath::Lerp(a.m_vPosition, b.m_vPosition, t);
    v.m_vPosition.GetData()[uiAxis] = fLine;
    v.m_vNormal = plMath::Lerp(a.m_vNormal, b.m_vNormal, t);
    v.m_vNormal.NormalizeIfNotZero(plVec3(0, 0, 1)).IgnoreResult();
    v.m_vTexCoord = plMath::Lerp(a.m_vTexCoord, b.m_vTexCoord, t);
    v.m_Color = plMath::Lerp(a.m_Color, b.m_Color, t);
    return v;
  };

  // The source triangle being split.
  const plGeometry::Vertex* triangle[3] = {};

  // Where two grid lines meet inside the source triangle, from the triangle alone: every piece meeting there gets the
  // same vertex. a and b lie on the grid line of the other axis.
  auto corner = [&](const ClipVertex& a, const ClipVertex& b, plUInt32 uiAxis, float fLine)
  {
    plVec2 vAt;
    vAt.GetData()[uiAxis] = fLine;
    vAt.GetData()[1 - uiAxis] = a.m_Vertex.m_vPosition.GetData()[1 - uiAxis];

    const plVec3& p0 = triangle[0]->m_vPosition;
    const plVec3& p1 = triangle[1]->m_vPosition;
    const plVec3& p2 = triangle[2]->m_vPosition;
    const float fDet = (p1.x - p0.x) * (p2.y - p0.y) - (p2.x - p0.x) * (p1.y - p0.y);

    ClipVertex result;
    if (fDet == 0.0f)
    {
      result.m_Vertex = crossing(a.m_Vertex, b.m_Vertex, uiAxis, fLine);
      return result;
    }

    const float w1 = ((vAt.x - p0.x) * (p2.y - p0.y) - (p2.x - p0.x) * (vAt.y - p0.y)) / fDet;
    const float w2 = ((p1.x - p0.x) * (vAt.y - p0.y) - (vAt.x - p0.x) * (p1.y - p0.y)) / fDet;
    const float w0 = 1.0f - w1 - w2;

    plGeometry::Vertex& v = result.m_Vertex;
    v = *triangle[0];
    v.m_vPosition = p0 * w0 + p1 * w1 + p2 * w2;
    v.m_vPosition.x = vAt.x;
    v.m_vPosition.y = vAt.y;
    v.m_vNormal = triangle[0]->m_vNormal * w0 + triangle[1]->m_vNormal * w1 + triangle[2]->m_vNormal * w2;
    v.m_vNormal.NormalizeIfNotZero(plVec3(0, 0, 1)).IgnoreResult();
    v.m_vTexCoord = triangle[0]->m_vTexCoord * w0 + triangle[1]->m_vTexCoord * w1 + triangle[2]->m_vTexCoord * w2;
    v.m_Color = triangle[0]->m_Color * w0 + triangle[1]->m_Color * w1 + triangle[2]->m_Color * w2;
    return result;
  };

  // Keeps the part of a convex polygon with the coordinate >= fLine, or <= fLine when bBelow. New vertices are computed
  // from the source triangle, never from clipped vertices.
  auto clip = [&](const Poly& in, plUInt32 uiAxis, float fLine, bool bBelow, Poly& out)
  {
    out.Clear();
    for (plUInt32 i = 0; i < in.GetCount(); ++i)
    {
      const ClipVertex& a = in[i];
      const ClipVertex& b = in[(i + 1) % in.GetCount()];
      const float fA = a.m_Vertex.m_vPosition.GetData()[uiAxis];
      const float fB = b.m_Vertex.m_vPosition.GetData()[uiAxis];
      const bool bInA = bBelow ? fA <= fLine : fA >= fLine;
      const bool bInB = bBelow ? fB <= fLine : fB >= fLine;

      if (bInA)
        out.PushBack(a);
      if (bInA == bInB)
        continue;

      // On a source edge, or along a grid line inside the triangle.
      const plUInt8 uiShared = a.m_uiEdges & b.m_uiEdges;
      if (uiShared != 0)
      {
        const plUInt32 e = (uiShared & 1) ? 0 : ((uiShared & 2) ? 1 : 2);
        ClipVertex& v = out.ExpandAndGetRef();
        v.m_Vertex = crossing(*triangle[e], *triangle[(e + 1) % 3], uiAxis, fLine);
        v.m_uiEdges = static_cast<plUInt8>(1u << e);
      }
      else
      {
        out.PushBack(corner(a, b, uiAxis, fLine));
      }
    }
  };

  // Adds a convex polygon that lies in one cell, fanned into triangles.
  auto emit = [&](const Poly& poly, plUInt32 uiGroup)
  {
    if (poly.GetCount() < 3)
      return;

    plVec3 vCentroid = plVec3::MakeZero();
    for (const ClipVertex& v : poly)
      vCentroid += v.m_Vertex.m_vPosition;
    vCentroid /= (float)poly.GetCount();

    const plVec2I32 cell = CellOf(vCentroid, fSize);
    const plUInt64 uiCellKey = PackCellKey(cell);

    plUInt32 uiPiece = 0;
    if (!pieceByCell.TryGetValue(uiCellKey, uiPiece))
    {
      uiPiece = out_pieces.GetCount();
      pieceByCell.Insert(uiCellKey, uiPiece);
      plUsdCellPiece& piece = out_pieces.ExpandAndGetRef();
      piece.m_Cell = cell;
      piece.m_GroupTriangleCounts.SetCount(plMath::Max(groupTriangleCounts.GetCount(), 1u), 0);
      welds.ExpandAndGetRef();
    }

    plUsdCellPiece& piece = out_pieces[uiPiece];
    auto& weld = welds[uiPiece];

    plHybridArray<plUInt32, 8> indices;
    for (const ClipVertex& clipped : poly)
    {
      const plGeometry::Vertex& v = clipped.m_Vertex;
      const VertexKey key = {v.m_vPosition, v.m_vNormal, v.m_vTexCoord, v.m_Color};
      plUInt32 uiIndex = 0;
      if (!weld.TryGetValue(key, uiIndex))
      {
        uiIndex = piece.m_Geometry.AddVertex(v.m_vPosition, v.m_vNormal, v.m_vTexCoord, v.m_Color, v.m_BoneIndices, v.m_BoneWeights);
        weld.Insert(key, uiIndex);
      }
      indices.PushBack(uiIndex);
    }

    for (plUInt32 i = 1; i + 1 < indices.GetCount(); ++i)
    {
      plUInt32 fan[3] = {indices[0], indices[i], indices[i + 1]};
      if (fan[0] == fan[1] || fan[1] == fan[2] || fan[0] == fan[2])
        continue;

      // Slivers from vertices lying on a grid line.
      const plVec3 e0 = poly[i].m_Vertex.m_vPosition - poly[0].m_Vertex.m_vPosition;
      const plVec3 e1 = poly[i + 1].m_Vertex.m_vPosition - poly[0].m_Vertex.m_vPosition;
      if (e0.CrossRH(e1).GetLengthSquared() == 0.0f)
        continue;

      // AppendVertices does not reorder; the winding flips here for mirroring transforms.
      piece.m_Geometry.AddPolygon(plArrayPtr<plUInt32>(fan), bFlip);
      ++piece.m_GroupTriangleCounts[uiGroup];
    }
  };

  const auto& polygons = geometry.GetPolygons();
  plUInt32 uiGroup = 0;
  plUInt32 uiGroupEnd = groupTriangleCounts.IsEmpty() ? polygons.GetCount() : groupTriangleCounts[0];
  Poly poly, strip, stripLow, part, partLow;

  for (plUInt32 t = 0; t < polygons.GetCount(); ++t)
  {
    while (t >= uiGroupEnd && uiGroup + 1 < groupTriangleCounts.GetCount())
      uiGroupEnd += groupTriangleCounts[++uiGroup];

    // Polygons as fans of triangles: a fan diagonal is a source edge of both triangles beside it.
    const auto& polyVertices = polygons[t].m_Vertices;
    for (plUInt32 f = 1; f + 1 < polyVertices.GetCount(); ++f)
    {
      triangle[0] = &vertices[polyVertices[0]];
      triangle[1] = &vertices[polyVertices[f]];
      triangle[2] = &vertices[polyVertices[f + 1]];

      // Corner c lies on the edges c and c - 1.
      poly.SetCount(3);
      plVec2I32 c0(plMath::MaxValue<plInt32>()), c1(plMath::MinValue<plInt32>());
      for (plUInt32 c = 0; c < 3; ++c)
      {
        poly[c].m_Vertex = *triangle[c];
        poly[c].m_uiEdges = static_cast<plUInt8>((1u << c) | (1u << ((c + 2) % 3)));

        const plVec2I32 cell = CellOf(triangle[c]->m_vPosition, fSize);
        c0.Set(plMath::Min(c0.x, cell.x), plMath::Min(c0.y, cell.y));
        c1.Set(plMath::Max(c1.x, cell.x), plMath::Max(c1.y, cell.y));
      }

      if (c0 == c1)
      {
        emit(poly, uiGroup);
        continue;
      }

      // Spans cells: cut at every grid line it crosses, first into columns, then each column into cells.
      for (plInt32 x = c0.x; x <= c1.x; ++x)
      {
        clip(poly, 0, x * fSize, false, stripLow);
        clip(stripLow, 0, (x + 1) * fSize, true, strip);

        for (plInt32 y = c0.y; y <= c1.y; ++y)
        {
          clip(strip, 1, y * fSize, false, partLow);
          clip(partLow, 1, (y + 1) * fSize, true, part);
          emit(part, uiGroup);
        }
      }
    }
  }

  // Origins on the grid: a border vertex moves by the same exact offsets in the pieces on both sides.
  for (plUsdCellPiece& piece : out_pieces)
  {
    const plVec3 vCentre = CellRect(piece.m_Cell, fSize).GetCenter();
    piece.m_vOrigin.Set(vCentre.x, vCentre.y, 0.0f);
    for (plGeometry::Vertex& v : piece.m_Geometry.GetVertices())
      v.m_vPosition -= piece.m_vOrigin;
  }

  return plStatus(PL_SUCCESS);
}
