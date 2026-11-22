#include <UsdBridge/UsdBridgePCH.h>

#include <Core/Graphics/Geometry.h>
#include <Foundation/Containers/HashTable.h>
#include <Foundation/IO/JSONReader.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/IO/OSFile.h>
#include <ModelImporter2/FoliageCompositeSource.h>
#include <RendererCore/Meshes/FoliageSkeleton.h>
#include <RendererCore/Meshes/MeshResourceDescriptor.h>
#include <UsdBridge/UsdFoliageComposite.h>
#include <UsdBridge/UsdSkel.h>
#include <UsdBridge/UsdStageAccess.h>
#include <UsdBridge/UsdTangents.h>

#include <functional>

#pragma warning(push, 0)
#include <pxr/base/gf/quatd.h>
#include <pxr/base/gf/quath.h>
#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/usdGeom/pointInstancer.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdSkel/skeleton.h>
#pragma warning(pop)

namespace
{
  // File-format identifiers of the composite attributes.
  const pxr::TfToken& MeshTypeToken()
  {
    static const pxr::TfToken s_Token("unreal:naniteAssembly:meshType");
    return s_Token;
  }

  const pxr::TfToken& SkeletonRelToken()
  {
    static const pxr::TfToken s_Token("unreal:naniteAssembly:skeleton");
    return s_Token;
  }

  const pxr::TfToken& BindJointsToken()
  {
    static const pxr::TfToken s_Token("unreal:naniteAssembly:bindJoints");
    return s_Token;
  }

  const pxr::TfToken& BindWeightsToken()
  {
    static const pxr::TfToken s_Token("unreal:naniteAssembly:bindJointWeights");
    return s_Token;
  }

  plString LastPathElement(plStringView sPath)
  {
    const char* szSlash = sPath.FindLastSubString("/");
    if (szSlash == nullptr)
      return sPath;
    return plStringView(szSlash + 1, sPath.GetEndPointer());
  }

  /// One slot per distinct bound material across the tree.
  struct MaterialSlots
  {
    plHashTable<plString, plUInt32> m_Slots;
    plDynamicArray<plString> m_Names;

    plUInt32 GetSlot(const plString& sMaterialPath)
    {
      plUInt32 uiSlot = 0;
      if (m_Slots.TryGetValue(sMaterialPath, uiSlot))
        return uiSlot;

      uiSlot = m_Names.GetCount();
      m_Slots.Insert(sMaterialPath, uiSlot);
      m_Names.PushBack(sMaterialPath.IsEmpty() ? plString("Default") : LastPathElement(sMaterialPath));
      return uiSlot;
    }
  };

  /// The meshes of one part, merged in the part's space; triangles keep their material slot.
  struct PartBuilder
  {
    plGeometry m_Geometry;
    plDynamicArray<plUInt32> m_TriangleSlots;

    /// Builds a mesh prim, keeps the joint each vertex carries in m_BoneIndices.x, and adds it moved by mTransform.
    /// pVertexJoints (optional) gives each built vertex its joint, before tangents are made.
    plStatus AddMesh(const plUsdStage& stage, const pxr::UsdPrim& prim, const plMat4& mTransform, MaterialSlots& ref_slots,
      const std::function<plStatus(const plGeometry&, plDynamicArray<plUInt16>&)>& vertexJoints = {})
    {
      const plUsdBasis& basis = stage.GetBasis();
      plStatus status;
      plUniquePtr<plUsdGeometrySource> pSource = plUsdStageAccess::CaptureGeometry(basis, prim, status);
      if (pSource == nullptr)
        return status;

      plGeometry geom;
      plDynamicArray<plUInt32> groupTriangles;
      PL_SUCCEED_OR_RETURN(pSource->Build(geom, &groupTriangles));

      if (vertexJoints)
      {
        plDynamicArray<plUInt16> joints;
        PL_SUCCEED_OR_RETURN(vertexJoints(geom, joints));
        for (plUInt32 v = 0; v < geom.GetVertices().GetCount(); ++v)
          geom.GetVertices()[v].m_BoneIndices.x = v < joints.GetCount() ? joints[v] : 0;
      }
      else
      {
        for (plGeometry::Vertex& vertex : geom.GetVertices())
          vertex.m_BoneIndices.x = 0;
      }

      // Material slot of each group, in Build's order.
      plHybridArray<plUInt32, 4> groupSlots;
      {
        plUsdPrim info;
        const plString sPath = plUsdText::FromStd(prim.GetPath().GetString());
        if (stage.ReadPrim(sPath, info).Succeeded())
        {
          // Only groups with triangles get a slot: group 0 is often empty when subsets cover every face.
          for (plUInt32 g = 0; g < info.m_MaterialGroups.GetCount(); ++g)
          {
            const plUsdMaterialGroup& group = info.m_MaterialGroups[g];
            const bool bUsed = g >= groupTriangles.GetCount() || groupTriangles[g] > 0;
            groupSlots.PushBack(bUsed ? ref_slots.GetSlot(group.m_bHasSurface ? group.m_Surface.m_sMaterialPath : plString()) : 0);
          }
        }
        for (plUInt32 g = groupSlots.GetCount(); g < groupTriangles.GetCount(); ++g)
          groupSlots.PushBack(groupTriangles[g] > 0 ? ref_slots.GetSlot(plString()) : 0);
      }

      // Into the part's space, then tangents on the final geometry (they keep the joint, which is part of the weld key).
      geom.Transform(mTransform, true);
      if (plUsdBasis::IsMirror(mTransform))
      {
        for (plGeometry::Polygon& poly : geom.GetPolygons())
        {
          if (poly.m_Vertices.GetCount() == 3)
            plMath::Swap(poly.m_Vertices[1], poly.m_Vertices[2]);
        }
      }

      if (plUsdTangents::Compute(geom).Failed())
        geom.ComputeTangents();

      const plUInt32 uiFirstVertex = m_Geometry.GetVertices().GetCount();
      for (const plGeometry::Vertex& vertex : geom.GetVertices())
        m_Geometry.GetVertices().PushBack(vertex);

      plUInt32 uiGroup = 0;
      plUInt32 uiGroupEnd = groupTriangles.IsEmpty() ? geom.GetPolygons().GetCount() : groupTriangles[0];
      plUInt32 uiTriangle = 0;
      for (const plGeometry::Polygon& poly : geom.GetPolygons())
      {
        while (uiTriangle >= uiGroupEnd && uiGroup + 1 < groupTriangles.GetCount())
        {
          ++uiGroup;
          uiGroupEnd += groupTriangles[uiGroup];
        }

        if (poly.m_Vertices.GetCount() != 3)
        {
          ++uiTriangle;
          continue;
        }

        plUInt32 tri[3] = {poly.m_Vertices[0] + uiFirstVertex, poly.m_Vertices[1] + uiFirstVertex, poly.m_Vertices[2] + uiFirstVertex};
        m_Geometry.AddPolygon(plArrayPtr<plUInt32>(tri), false);
        m_TriangleSlots.PushBack(uiGroup < groupSlots.GetCount() ? groupSlots[uiGroup] : 0);
        ++uiTriangle;
      }

      return plStatus(PL_SUCCESS);
    }

    /// One sub-mesh per material slot the part uses. With bJoints, every vertex's joint goes into TexCoord1.x.
    plStatus Finish(plMeshResourceDescriptor& out_desc, bool bJoints)
    {
      const plUInt32 uiTriangles = m_Geometry.GetPolygons().GetCount();
      if (uiTriangles == 0)
        return plStatus("The part has no triangles.");

      // Triangles sorted by slot, so each slot is one sub-mesh.
      plDynamicArray<plUInt32> order;
      order.SetCount(uiTriangles);
      for (plUInt32 i = 0; i < uiTriangles; ++i)
        order[i] = i;
      order.Sort([this](plUInt32 a, plUInt32 b)
        { return m_TriangleSlots[a] != m_TriangleSlots[b] ? m_TriangleSlots[a] < m_TriangleSlots[b] : a < b; });

      plGeometry sorted;
      sorted.GetVertices() = m_Geometry.GetVertices();
      for (plUInt32 i : order)
      {
        const plGeometry::Polygon& poly = m_Geometry.GetPolygons()[i];
        plUInt32 tri[3] = {poly.m_Vertices[0], poly.m_Vertices[1], poly.m_Vertices[2]};
        sorted.AddPolygon(plArrayPtr<plUInt32>(tri), false);
      }

      plMeshBufferResourceDescriptor& mbd = out_desc.MeshBufferDesc();
      mbd.AddCommonStreams();
      mbd.AllocateStreamsFromGeometry(sorted, plGALPrimitiveTopology::Triangles);

      plUInt32 uiFirst = 0;
      while (uiFirst < uiTriangles)
      {
        const plUInt32 uiSlot = m_TriangleSlots[order[uiFirst]];
        plUInt32 uiEnd = uiFirst;
        while (uiEnd < uiTriangles && m_TriangleSlots[order[uiEnd]] == uiSlot)
          ++uiEnd;
        out_desc.AddSubMesh(uiEnd - uiFirst, uiFirst, uiSlot);
        uiFirst = uiEnd;
      }

      if (bJoints)
      {
        plDynamicArray<plUInt16> joints;
        joints.SetCountUninitialized(sorted.GetVertices().GetCount());
        for (plUInt32 v = 0; v < joints.GetCount(); ++v)
          joints[v] = sorted.GetVertices()[v].m_BoneIndices.x;

        plFoliageSkeleton::AddTexCoord1Stream(mbd);
        if (plFoliageSkeleton::BindVertices(mbd, joints).Failed())
          return plStatus("Binding the core's vertices to their joints failed.");
      }

      out_desc.ComputeBounds();
      return plStatus(PL_SUCCESS);
    }
  };

  plMat4d ToPlasma(const plUsdBasis& basis, const pxr::GfMatrix4d& mUsd)
  {
    return basis.MatrixToPlasma(plUsdStageAccess::FromGfMatrix(mUsd));
  }

  /// The wind side file: joint orders by joint name, ground cover and gust attenuation.
  struct WindSideFile
  {
    bool m_bFound = false;
    plHashTable<plString, plUInt8> m_JointOrders;
    bool m_bGroundCover = false;
    float m_fGustAttenuation = 0.0f;
  };

  plStatus ReadWindSideFile(plStringView sStageFile, WindSideFile& out_wind)
  {
    plStringBuilder sJson = sStageFile;
    sJson.RemoveFileExtension();
    sJson.Append("_DynamicWind.json");

    plOSFile file;
    if (file.Open(sJson, plFileOpenMode::Read).Failed())
      return plStatus(PL_SUCCESS); // optional

    plDynamicArray<plUInt8> content;
    file.ReadAll(content);
    file.Close();

    plMemoryStreamContainerWrapperStorage<plDynamicArray<plUInt8>> storage(&content);
    plMemoryStreamReader reader(&storage);

    plJSONReader json;
    if (json.Parse(reader).Failed() || json.GetTopLevelElementType() != plJSONReader::ElementType::Dictionary)
      return plStatus(plFmt("'{}' is no valid wind file.", sJson));

    const plVariantDictionary& root = json.GetTopLevelObject();
    out_wind.m_bFound = true;

    if (const plVariant* pJoints = root.GetValue("Joints"); pJoints != nullptr && pJoints->IsA<plVariantArray>())
    {
      for (const plVariant& joint : pJoints->Get<plVariantArray>())
      {
        if (!joint.IsA<plVariantDictionary>())
          continue;

        const plVariantDictionary& dict = joint.Get<plVariantDictionary>();
        const plVariant* pName = dict.GetValue("JointName");
        const plVariant* pGroup = dict.GetValue("SimulationGroupIndex");
        if (pName == nullptr || pGroup == nullptr || !pName->IsA<plString>())
          continue;

        const plInt32 iGroup = pGroup->ConvertTo<plInt32>();
        out_wind.m_JointOrders.Insert(pName->Get<plString>(), static_cast<plUInt8>(plMath::Clamp(iGroup, 0, 3)));
      }
    }

    if (const plVariant* pGround = root.GetValue("bIsGroundCover"))
      out_wind.m_bGroundCover = pGround->ConvertTo<bool>();
    if (const plVariant* pGust = root.GetValue("GustAttenuation"))
      out_wind.m_fGustAttenuation = pGust->ConvertTo<float>();

    return plStatus(PL_SUCCESS);
  }

  pxr::UsdPrim FindTreeRoot(const pxr::UsdStageRefPtr& pStage)
  {
    if (pxr::UsdPrim defaultPrim = pStage->GetDefaultPrim(); defaultPrim && defaultPrim.HasAttribute(MeshTypeToken()))
      return defaultPrim;

    for (const pxr::UsdPrim& prim : pStage->Traverse())
    {
      if (prim.HasAttribute(MeshTypeToken()))
        return prim;
    }
    return pxr::UsdPrim();
  }
} // namespace

bool plUsdFoliageComposite::IsCompositeTree(plStringView sFile)
{
  pxr::TfErrorMark errors;
  pxr::SdfLayerRefPtr pLayer = pxr::SdfLayer::FindOrOpen(plUsdText::ToStd(sFile));
  errors.Clear();
  if (!pLayer)
    return false;

  for (const pxr::SdfPrimSpecHandle& spec : pLayer->GetRootPrims())
  {
    if (pLayer->GetAttributeAtPath(spec->GetPath().AppendProperty(MeshTypeToken())))
      return true;
  }
  return false;
}

plStatus plUsdFoliageComposite::Read(plStringView sFile, plModelImporter2::FoliageCompositeSource& out_source)
{
  out_source = plModelImporter2::FoliageCompositeSource();

  plUsdStage stage;
  PL_SUCCEED_OR_RETURN(stage.Open(sFile));

  const plUsdBasis& basis = stage.GetBasis();
  pxr::UsdStageRefPtr pStage = plUsdStageAccess::GetStage(stage);
  const pxr::UsdTimeCode time(stage.GetStartTimeCode());
  pxr::UsdGeomXformCache xformCache(time);

  const pxr::UsdPrim root = FindTreeRoot(pStage);
  if (!root)
    return plStatus(plFmt("'{}' holds no composite tree: no prim has the composite mesh type attribute.", sFile));

  // Everything ends up in the root prim's space.
  const plMat4d mObjectFromStage = ToPlasma(basis, xformCache.GetLocalToWorldTransform(root)).GetInverse(0.0);

  // The skeleton the root names, else the first one below it.
  pxr::SdfPath skeletonPath;
  {
    pxr::SdfPathVector targets;
    if (pxr::UsdRelationship rel = root.GetRelationship(SkeletonRelToken()); rel && rel.GetForwardedTargets(&targets) && !targets.empty())
      skeletonPath = targets[0];

    if (skeletonPath.IsEmpty())
    {
      for (const pxr::UsdPrim& prim : pxr::UsdPrimRange(root))
      {
        if (prim.IsA<pxr::UsdSkelSkeleton>())
        {
          skeletonPath = prim.GetPath();
          break;
        }
      }
    }
  }

  plUsdSkelScene skel;
  PL_SUCCEED_OR_RETURN(skel.Open(sFile, basis));

  plInt32 iSkeleton = -1;
  for (plUInt32 s = 0; s < skel.GetSkeletons().GetCount(); ++s)
  {
    if (skel.GetSkeletons()[s].m_sPath == plUsdText::FromStd(skeletonPath.GetString()))
      iSkeleton = static_cast<plInt32>(s);
  }
  if (iSkeleton < 0)
    return plStatus(plFmt("The tree's skeleton '{}' was not found.", skeletonPath.GetString().c_str()));

  const plUsdSkeleton& skeleton = skel.GetSkeletons()[iSkeleton];
  const plUInt32 uiJoints = skeleton.m_JointPaths.GetCount();

  // Joints: bind pose positions in object space, parents from the joint paths.
  plHashTable<plString, plUInt32> jointByPath;
  plHashTable<plString, plUInt32> jointByName;
  {
    out_source.m_JointPositions.SetCount(uiJoints);
    out_source.m_JointParents.SetCount(uiJoints);
    const plMat4d mObjectFromSkeleton = mObjectFromStage * skeleton.m_mSkeletonToStage;
    for (plUInt32 j = 0; j < uiJoints; ++j)
    {
      const plMat4d mJoint = mObjectFromSkeleton * skeleton.m_BindTransforms[j];
      const plVec3d vPos = mJoint.GetTranslationVector();
      out_source.m_JointPositions[j] = plVec3(static_cast<float>(vPos.x), static_cast<float>(vPos.y), static_cast<float>(vPos.z));
      out_source.m_JointParents[j] = skeleton.m_JointParents[j] < 0 ? 0xFFFFFFFFu : static_cast<plUInt32>(skeleton.m_JointParents[j]);
      jointByPath.Insert(skeleton.m_JointPaths[j], j);
      jointByName.Insert(LastPathElement(skeleton.m_JointPaths[j]), j);
    }
  }

  // Branch orders: the wind file's simulation groups, else branch points passed on the way from the root.
  {
    WindSideFile wind;
    PL_SUCCEED_OR_RETURN(ReadWindSideFile(sFile, wind));
    out_source.m_bGroundCover = wind.m_bGroundCover;
    out_source.m_fGustAttenuation = wind.m_fGustAttenuation;

    plDynamicArray<plUInt32> children;
    children.SetCount(uiJoints);
    for (plUInt32 j = 0; j < uiJoints; ++j)
    {
      if (out_source.m_JointParents[j] < uiJoints)
        ++children[out_source.m_JointParents[j]];
    }

    out_source.m_JointOrders.SetCount(uiJoints);
    plUInt32 uiFromWind = 0;
    for (plUInt32 j = 0; j < uiJoints; ++j)
    {
      plUInt8 uiOrder = 0;
      if (wind.m_JointOrders.TryGetValue(LastPathElement(skeleton.m_JointPaths[j]), uiOrder))
      {
        ++uiFromWind;
      }
      else
      {
        // parents come before children (UsdSkel orders joints that way)
        const plUInt32 uiParent = out_source.m_JointParents[j];
        if (uiParent < j)
          uiOrder = static_cast<plUInt8>(plMath::Min<plUInt32>(out_source.m_JointOrders[uiParent] + (children[uiParent] > 1 ? 1 : 0), 3));
      }
      out_source.m_JointOrders[j] = uiOrder;
    }

    if (wind.m_bFound && uiFromWind < uiJoints)
      plLog::Warning("Foliage composite: the wind file names {} of {} joints; the others take their order from the branch points.", uiFromWind, uiJoints);
  }

  MaterialSlots slots;

  // Parts: one per instancer prototype.
  pxr::UsdGeomPointInstancer instancer;
  for (const pxr::UsdPrim& prim : pxr::UsdPrimRange(root))
  {
    if (prim.IsA<pxr::UsdGeomPointInstancer>())
    {
      instancer = pxr::UsdGeomPointInstancer(prim);
      break;
    }
  }

  if (instancer)
  {
    pxr::SdfPathVector prototypes;
    instancer.GetPrototypesRel().GetForwardedTargets(&prototypes);

    const pxr::GfMatrix4d mInstancerToStage = xformCache.GetLocalToWorldTransform(instancer.GetPrim());

    // Prototype -> part, for the prototypes that have meshes.
    plDynamicArray<plUInt32> protoToPart;
    std::vector<pxr::GfMatrix4d> protoLocal(prototypes.size(), pxr::GfMatrix4d(1.0));
    protoToPart.SetCount(static_cast<plUInt32>(prototypes.size()), plInvalidIndex);

    for (plUInt32 p = 0; p < prototypes.size(); ++p)
    {
      const pxr::UsdPrim proto = pStage->GetPrimAtPath(prototypes[p]);
      if (!proto)
        continue;

      bool bResets = false;
      protoLocal[p] = xformCache.GetLocalTransformation(proto, &bResets);
      const pxr::GfMatrix4d mProtoToStageInv = xformCache.GetLocalToWorldTransform(proto).GetInverse();

      PartBuilder part;
      for (const pxr::UsdPrim& meshPrim : pxr::UsdPrimRange(proto, pxr::UsdTraverseInstanceProxies()))
      {
        if (!meshPrim.IsA<pxr::UsdGeomMesh>())
          continue;

        // In the prototype's own space: the instance transforms below include the prototype's transform.
        const pxr::GfMatrix4d mMeshToProto = xformCache.GetLocalToWorldTransform(meshPrim) * mProtoToStageInv;
        const plStatus res = part.AddMesh(stage, meshPrim, plUsdBasis::ToFloat(ToPlasma(basis, mMeshToProto)), slots);
        if (res.Failed())
          plLog::Warning("Foliage composite: mesh '{}' skipped: {}", meshPrim.GetPath().GetString().c_str(), res.m_sMessage);
      }

      if (part.m_Geometry.GetPolygons().IsEmpty())
        continue;

      plMeshResourceDescriptor& desc = out_source.m_Parts.ExpandAndGetRef();
      const plStatus res = part.Finish(desc, false);
      if (res.Failed())
        return plStatus(plFmt("Part '{}': {}", proto.GetName().GetString().c_str(), res.m_sMessage));

      protoToPart[p] = out_source.m_Parts.GetCount() - 1;
      out_source.m_PartNames.PushBack(plUsdText::FromStd(proto.GetName().GetString()));
    }

    // Instances.
    pxr::VtIntArray protoIndices;
    pxr::VtVec3fArray positions;
    pxr::VtQuathArray orientations;
    pxr::VtVec3fArray scales;
    instancer.GetProtoIndicesAttr().Get(&protoIndices, time);
    instancer.GetPositionsAttr().Get(&positions, time);
    instancer.GetOrientationsAttr().Get(&orientations, time);
    instancer.GetScalesAttr().Get(&scales, time);
    const std::vector<bool> mask = instancer.ComputeMaskAtTime(time);

    const size_t uiInstances = protoIndices.size();

    pxr::VtTokenArray bindJoints;
    pxr::VtFloatArray bindWeights;
    {
      pxr::UsdGeomPrimvarsAPI primvars(instancer.GetPrim());
      if (pxr::UsdGeomPrimvar pv = primvars.GetPrimvar(BindJointsToken()))
        pv.ComputeFlattened(&bindJoints, time);
      if (pxr::UsdGeomPrimvar pv = primvars.GetPrimvar(BindWeightsToken()))
        pv.ComputeFlattened(&bindWeights, time);
    }
    const size_t uiPerInstance = uiInstances > 0 ? bindJoints.size() / uiInstances : 0;
    plUInt32 uiUnknownJoints = 0;

    for (size_t i = 0; i < uiInstances; ++i)
    {
      if (!mask.empty() && !mask[i])
        continue;

      const plInt32 iProto = protoIndices[i];
      if (iProto < 0 || static_cast<plUInt32>(iProto) >= protoToPart.GetCount() || protoToPart[iProto] == plInvalidIndex)
        continue;

      pxr::GfMatrix4d mInstance(1.0);
      if (i < scales.size())
        mInstance.SetScale(pxr::GfVec3d(scales[i]));
      if (i < orientations.size())
      {
        pxr::GfMatrix4d mRotate(1.0);
        mRotate.SetRotate(pxr::GfQuatd(orientations[i]));
        mInstance = mInstance * mRotate;
      }
      if (i < positions.size())
      {
        pxr::GfMatrix4d mTranslate(1.0);
        mTranslate.SetTranslate(pxr::GfVec3d(positions[i]));
        mInstance = mInstance * mTranslate;
      }

      const pxr::GfMatrix4d mToStage = protoLocal[iProto] * mInstance * mInstancerToStage;

      plModelImporter2::FoliageCompositeSourceInstance& inst = out_source.m_Instances.ExpandAndGetRef();
      inst.m_uiPart = protoToPart[iProto];
      inst.m_Transform = plUsdBasis::MatrixToTransform(mObjectFromStage * ToPlasma(basis, mToStage));

      for (size_t k = 0; k < uiPerInstance; ++k)
      {
        const size_t uiEntry = i * uiPerInstance + k;
        const plString sJoint = plUsdText::FromStd(bindJoints[uiEntry].GetString());
        plUInt32 uiJoint = 0;
        if (!jointByPath.TryGetValue(sJoint, uiJoint) && !jointByName.TryGetValue(LastPathElement(sJoint), uiJoint))
        {
          ++uiUnknownJoints;
          continue;
        }

        const float fWeight = uiEntry < bindWeights.size() ? bindWeights[uiEntry] : 1.0f;
        if (fWeight <= 0.0f)
          continue;

        inst.m_Joints.PushBack(uiJoint);
        inst.m_Weights.PushBack(fWeight);
      }
    }

    if (uiUnknownJoints > 0)
      plLog::Warning("Foliage composite: {} instance bind joints name no joint of the skeleton.", uiUnknownJoints);
  }

  // The core: the meshes the skeleton deforms, as one part placed once, every vertex on its dominant joint.
  {
    PartBuilder core;
    const plMat4d mObjectFromSkeleton = mObjectFromStage * skeleton.m_mSkeletonToStage;

    for (plUInt32 uiSkinned = 0; uiSkinned < skel.GetSkinnedPrims().GetCount(); ++uiSkinned)
    {
      const plUsdSkinnedPrim& skinned = skel.GetSkinnedPrims()[uiSkinned];
      if (skinned.m_iSkeleton != iSkeleton || skinned.m_uiInfluencesPerPoint == 0)
        continue;

      const pxr::UsdPrim prim = pStage->GetPrimAtPath(pxr::SdfPath(plUsdText::ToStd(skinned.m_sPath)));
      if (!prim || !prim.IsA<pxr::UsdGeomMesh>())
        continue;

      auto vertexJoints = [&](const plGeometry& geom, plDynamicArray<plUInt16>& out_joints) -> plStatus
      {
        plDynamicArray<plUInt32> vertexPoints;
        PL_SUCCEED_OR_RETURN(skel.FindVertexPoints(uiSkinned, geom, vertexPoints));

        const plUInt32 uiInfluences = skinned.m_uiInfluencesPerPoint;
        out_joints.SetCount(geom.GetVertices().GetCount());
        for (plUInt32 v = 0; v < out_joints.GetCount(); ++v)
        {
          const plUInt32 uiPoint = skinned.m_bRigid ? 0 : (v < vertexPoints.GetCount() ? vertexPoints[v] : 0);
          plInt32 iBest = 0;
          float fBest = -1.0f;
          for (plUInt32 k = 0; k < uiInfluences; ++k)
          {
            const plUInt32 uiEntry = uiPoint * uiInfluences + k;
            if (uiEntry >= skinned.m_JointIndices.GetCount())
              break;
            if (skinned.m_JointIndices[uiEntry] >= 0 && skinned.m_JointWeights[uiEntry] > fBest)
            {
              fBest = skinned.m_JointWeights[uiEntry];
              iBest = skinned.m_JointIndices[uiEntry];
            }
          }
          out_joints[v] = static_cast<plUInt16>(plMath::Clamp<plInt32>(iBest, 0, 0xFFFF));
        }
        return plStatus(PL_SUCCESS);
      };

      // Rest pose: the bind transform takes the geometry into skeleton space.
      const plMat4 mTransform = plUsdBasis::ToFloat(mObjectFromSkeleton * skinned.m_mGeomBindTransform);
      const plStatus res = core.AddMesh(stage, prim, mTransform, slots, vertexJoints);
      if (res.Failed())
        plLog::Warning("Foliage composite: core mesh '{}' skipped: {}", skinned.m_sPath, res.m_sMessage);
    }

    if (!core.m_Geometry.GetPolygons().IsEmpty())
    {
      plMeshResourceDescriptor& desc = out_source.m_Parts.ExpandAndGetRef();
      const plStatus res = core.Finish(desc, true);
      if (res.Failed())
        return plStatus(plFmt("Core: {}", res.m_sMessage));

      plStringBuilder sName = plUsdText::FromStd(root.GetName().GetString());
      sName.Append("_Core");
      out_source.m_PartNames.PushBack(sName);
      plModelImporter2::FoliageCompositeSourceInstance& inst = out_source.m_Instances.ExpandAndGetRef();
      inst.m_uiPart = out_source.m_Parts.GetCount() - 1;
    }
  }

  if (out_source.m_Parts.IsEmpty() || out_source.m_Instances.IsEmpty())
    return plStatus(plFmt("'{}' has no parts: neither instanced part meshes nor a skinned core.", sFile));

  out_source.m_MaterialNames = slots.m_Names;
  out_source.m_Materials.SetCount(slots.m_Names.GetCount());
  return plStatus(PL_SUCCESS);
}
