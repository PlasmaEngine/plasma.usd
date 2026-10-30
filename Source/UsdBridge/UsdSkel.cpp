#include <UsdBridge/UsdBridgePCH.h>

#include <Core/Graphics/Geometry.h>
#include <Foundation/Algorithm/HashingUtils.h>
#include <Foundation/Containers/HashTable.h>
#include <Foundation/Containers/Set.h>
#include <Foundation/IO/OSFile.h>
#include <UsdBridge/UsdSkel.h>

#pragma warning(push, 0)
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdSkel/animQuery.h>
#include <pxr/usd/usdSkel/animation.h>
#include <pxr/usd/usdSkel/binding.h>
#include <pxr/usd/usdSkel/cache.h>
#include <pxr/usd/usdSkel/root.h>
#include <pxr/usd/usdSkel/skeleton.h>
#include <pxr/usd/usdSkel/skeletonQuery.h>
#include <pxr/usd/usdSkel/skinningQuery.h>
#include <pxr/usd/usdSkel/tokens.h>
#include <pxr/usd/usdSkel/topology.h>
#include <pxr/usd/usdSkel/utils.h>
#pragma warning(pop)

namespace
{
  std::string SkelToStd(plStringView s)
  {
    return std::string(s.GetStartPointer(), s.GetElementCount());
  }

  plString SkelFromStd(const std::string& s)
  {
    return plString(plStringView(s.data(), (plUInt32)s.size()));
  }

  /// OpenUSD matrices are row-vector (p' = p * M); plMat4d is column-vector. One is the transpose of the other.
  plMat4d SkelFromGf(const pxr::GfMatrix4d& gf)
  {
    plMat4d m;
    for (int c = 0; c < 4; ++c)
      for (int r = 0; r < 4; ++r)
        m.Element(c, r) = gf[c][r];
    return m;
  }

  /// USD's translate/rotate/scale components (scale, then rotate, then translate) in Plasma's frame.
  plTransform ComponentsToPlasma(const plUsdBasis& basis, const pxr::GfVec3f& vTranslation, const pxr::GfQuatf& qRotation, const pxr::GfVec3h& vScale)
  {
    plTransform t;
    t.m_vPosition.Set(vTranslation[0], vTranslation[1], vTranslation[2]);

    const pxr::GfVec3f& vImaginary = qRotation.GetImaginary();
    t.m_qRotation = plQuat(vImaginary[0], vImaginary[1], vImaginary[2], qRotation.GetReal());
    if (t.m_qRotation.Dot(t.m_qRotation) > 1e-12f)
      t.m_qRotation.Normalize();
    else
      t.m_qRotation = plQuat::MakeIdentity();

    t.m_vScale.Set((float)vScale[0], (float)vScale[1], (float)vScale[2]);

    return plUsdSkelScene::ChangeFrame(basis.GetAxisMap(), basis.GetMetersPerUnit(), t);
  }

  /// A USD joint-local matrix in Plasma's frame, through its components where it has a TRS form.
  plTransform LocalMatrixToPlasma(const plUsdBasis& basis, const pxr::GfMatrix4d& mUsd)
  {
    pxr::GfVec3f vTranslation;
    pxr::GfQuatf qRotation;
    pxr::GfVec3h vScale;
    if (pxr::UsdSkelDecomposeTransform(mUsd, &vTranslation, &qRotation, &vScale))
      return ComponentsToPlasma(basis, vTranslation, qRotation, vScale);

    // Shear or a degenerate axis: the closest TRS of the converted matrix.
    return plUsdBasis::MatrixToTransform(basis.MatrixToPlasma(SkelFromGf(mUsd)));
  }

  /// The last path element, or the whole path for elements that more than one joint ends in.
  void MakeUniqueJointNames(const plDynamicArray<plString>& paths, plDynamicArray<plString>& out_names)
  {
    plDynamicArray<plString> leaves;
    leaves.SetCount(paths.GetCount());

    plHashTable<plString, plUInt32> counts;
    for (plUInt32 i = 0; i < paths.GetCount(); ++i)
    {
      plStringView sLeaf = paths[i];
      if (const char* szSlash = sLeaf.FindLastSubString("/"))
        sLeaf.SetStartPosition(szSlash + 1);

      leaves[i] = sLeaf;
      counts[leaves[i]] += 1;
    }

    out_names.SetCount(paths.GetCount());
    for (plUInt32 i = 0; i < paths.GetCount(); ++i)
    {
      plUInt32 uiCount = 0;
      counts.TryGetValue(leaves[i], uiCount);
      out_names[i] = uiCount > 1 ? paths[i] : leaves[i];
    }
  }

  /// Integer cell of a position, for finding the points near a vertex.
  struct CellKey
  {
    PL_DECLARE_POD_TYPE();

    plInt64 x;
    plInt64 y;
    plInt64 z;

    bool operator==(const CellKey& rhs) const { return x == rhs.x && y == rhs.y && z == rhs.z; }
  };

  struct CellKeyHash
  {
    static plUInt32 Hash(const CellKey& key) { return plHashingUtils::xxHash32(&key, sizeof(CellKey)); }
    static bool Equal(const CellKey& a, const CellKey& b) { return a == b; }
  };

  CellKey MakeCellKey(const plVec3& v, double fCell)
  {
    return CellKey{(plInt64)plMath::Floor(v.x / fCell), (plInt64)plMath::Floor(v.y / fCell), (plInt64)plMath::Floor(v.z / fCell)};
  }
} // namespace

/// A skinned Mesh's points and faces, converted into Plasma's frame the way plUsdGeometrySource::Build converts them.
struct plUsdSkinnedMeshData
{
  plDynamicArray<plVec3> m_Points;
  plDynamicArray<plInt32> m_FaceCounts;
  plDynamicArray<plInt32> m_FaceIndices;
};

struct plUsdSkelScene::Impl
{
  pxr::UsdStageRefPtr m_Stage;
  pxr::UsdSkelCache m_Cache;
  plUsdBasis m_Basis;

  plDynamicArray<plUsdSkeleton> m_Skeletons;
  plDynamicArray<plUsdSkinnedPrim> m_SkinnedPrims;
  plDynamicArray<plUsdSkelAnimation> m_Animations;

  std::vector<pxr::UsdPrim> m_AnimationPrims;     ///< Parallel to m_Animations.
  plDynamicArray<plUsdSkinnedMeshData> m_MeshData; ///< Parallel to m_SkinnedPrims.

  void ReadAnimation(const pxr::UsdPrim& prim);
  void ReadSkeleton(const pxr::UsdPrim& prim, pxr::UsdGeomXformCache& ref_xformCache);
  void ReadSkinnedPrims(const pxr::UsdSkelRoot& root, plSet<plString>& ref_seen);
};

void plUsdSkelScene::Impl::ReadAnimation(const pxr::UsdPrim& prim)
{
  const pxr::UsdSkelAnimQuery query = m_Cache.GetAnimQuery(pxr::UsdSkelAnimation(prim));
  if (!query)
  {
    plLog::Warning("SkelAnimation '{}' cannot be read.", prim.GetPath().GetString().c_str());
    return;
  }

  plUsdSkelAnimation& anim = m_Animations.ExpandAndGetRef();
  m_AnimationPrims.push_back(prim);

  anim.m_sPath = SkelFromStd(prim.GetPath().GetString());
  anim.m_sName = SkelFromStd(prim.GetName().GetString());
  anim.m_fTimeCodesPerSecond = m_Stage->GetTimeCodesPerSecond();

  for (const pxr::TfToken& joint : query.GetJointOrder())
    anim.m_JointPaths.PushBack(SkelFromStd(joint.GetString()));

  anim.m_SkeletonJoints.SetCount(anim.m_JointPaths.GetCount(), -1);

  std::vector<double> times;
  if (query.GetJointTransformTimeSamples(&times))
  {
    std::sort(times.begin(), times.end());
    for (double fTime : times)
      anim.m_TimeSamples.PushBack(fTime);
  }

  anim.m_bHasBlendShapeWeights = !query.GetBlendShapeOrder().empty();
}

void plUsdSkelScene::Impl::ReadSkeleton(const pxr::UsdPrim& prim, pxr::UsdGeomXformCache& ref_xformCache)
{
  const pxr::UsdSkelSkeleton skel(prim);
  const pxr::UsdSkelSkeletonQuery query = m_Cache.GetSkelQuery(skel);
  if (!query)
  {
    plLog::Warning("Skeleton '{}' cannot be read: its joints do not form a valid hierarchy.", prim.GetPath().GetString().c_str());
    return;
  }

  const plInt32 iSkeleton = (plInt32)m_Skeletons.GetCount();
  plUsdSkeleton& out = m_Skeletons.ExpandAndGetRef();
  out.m_sPath = SkelFromStd(prim.GetPath().GetString());

  const pxr::VtTokenArray joints = query.GetJointOrder();
  const pxr::UsdSkelTopology& topology = query.GetTopology();
  const plUInt32 uiNumJoints = (plUInt32)joints.size();

  out.m_JointPaths.SetCount(uiNumJoints);
  out.m_JointParents.SetCount(uiNumJoints);
  for (plUInt32 j = 0; j < uiNumJoints; ++j)
  {
    out.m_JointPaths[j] = SkelFromStd(joints[j].GetString());
    out.m_JointParents[j] = topology.GetParent(j);
  }

  MakeUniqueJointNames(out.m_JointPaths, out.m_JointNames);

  // Row-vector matrices here, as OpenUSD stores them: skeleton space = local * parent's skeleton space.
  pxr::VtMatrix4dArray rest, bind;
  skel.GetRestTransformsAttr().Get(&rest);
  skel.GetBindTransformsAttr().Get(&bind);

  out.m_bHasRestTransforms = rest.size() == uiNumJoints;
  out.m_bHasBindTransforms = bind.size() == uiNumJoints;

  if (!out.m_bHasRestTransforms && !out.m_bHasBindTransforms)
  {
    plLog::Warning("Skeleton '{}' has neither restTransforms nor bindTransforms for its {} joints; every joint is at its parent's origin.", out.m_sPath, uiNumJoints);
    rest.assign(uiNumJoints, pxr::GfMatrix4d(1.0));
    bind.assign(uiNumJoints, pxr::GfMatrix4d(1.0));
  }
  else if (!out.m_bHasRestTransforms)
  {
    rest.resize(uiNumJoints);
    for (plUInt32 j = 0; j < uiNumJoints; ++j)
    {
      const plInt32 iParent = out.m_JointParents[j];
      rest[j] = iParent >= 0 ? bind[j] * bind[iParent].GetInverse() : bind[j];
    }
  }
  else if (!out.m_bHasBindTransforms)
  {
    bind.resize(uiNumJoints);
    for (plUInt32 j = 0; j < uiNumJoints; ++j)
    {
      const plInt32 iParent = out.m_JointParents[j];
      bind[j] = iParent >= 0 ? rest[j] * bind[iParent] : rest[j];
    }
  }

  out.m_RestTransforms.SetCount(uiNumJoints);
  out.m_BindTransforms.SetCount(uiNumJoints);
  for (plUInt32 j = 0; j < uiNumJoints; ++j)
  {
    out.m_RestTransforms[j] = LocalMatrixToPlasma(m_Basis, rest[j]);
    out.m_BindTransforms[j] = m_Basis.MatrixToPlasma(SkelFromGf(bind[j]));
  }

  out.m_mSkeletonToStage = m_Basis.MatrixToPlasma(SkelFromGf(ref_xformCache.GetLocalToWorldTransform(prim)));

  if (const pxr::UsdSkelAnimQuery& animQuery = query.GetAnimQuery())
  {
    const plString sAnimPath = SkelFromStd(animQuery.GetPrim().GetPath().GetString());

    for (plUInt32 a = 0; a < m_Animations.GetCount(); ++a)
    {
      plUsdSkelAnimation& anim = m_Animations[a];
      if (anim.m_sPath != sAnimPath)
        continue;

      out.m_iAnimation = (plInt32)a;

      if (anim.m_iSkeleton < 0)
      {
        anim.m_iSkeleton = iSkeleton;

        plHashTable<plString, plInt32> jointIndex;
        for (plUInt32 j = 0; j < uiNumJoints; ++j)
          jointIndex[out.m_JointPaths[j]] = (plInt32)j;

        for (plUInt32 aj = 0; aj < anim.m_JointPaths.GetCount(); ++aj)
        {
          plInt32 iJoint = -1;
          jointIndex.TryGetValue(anim.m_JointPaths[aj], iJoint);
          anim.m_SkeletonJoints[aj] = iJoint;
        }
      }

      break;
    }
  }
}

void plUsdSkelScene::Impl::ReadSkinnedPrims(const pxr::UsdSkelRoot& root, plSet<plString>& ref_seen)
{
  const pxr::Usd_PrimFlagsPredicate predicate = pxr::UsdPrimDefaultPredicate;
  m_Cache.Populate(root, predicate);

  std::vector<pxr::UsdSkelBinding> bindings;
  if (!m_Cache.ComputeSkelBindings(root, &bindings, predicate))
    return;

  for (const pxr::UsdSkelBinding& binding : bindings)
  {
    const plString sSkelPath = SkelFromStd(binding.GetSkeleton().GetPrim().GetPath().GetString());

    plInt32 iSkeleton = -1;
    for (plUInt32 s = 0; s < m_Skeletons.GetCount(); ++s)
    {
      if (m_Skeletons[s].m_sPath == sSkelPath)
      {
        iSkeleton = (plInt32)s;
        break;
      }
    }

    if (iSkeleton < 0)
      continue;

    const plUsdSkeleton& skeleton = m_Skeletons[iSkeleton];
    const plInt32 iNumSkelJoints = (plInt32)skeleton.m_JointPaths.GetCount();

    for (const pxr::UsdSkelSkinningQuery& skinning : binding.GetSkinningTargets())
    {
      const pxr::UsdPrim& prim = skinning.GetPrim();
      const plString sPath = SkelFromStd(prim.GetPath().GetString());

      if (!skinning.HasJointInfluences() || ref_seen.Contains(sPath))
        continue;

      ref_seen.Insert(sPath);

      pxr::VtIntArray indices;
      pxr::VtFloatArray weights;
      if (!skinning.ComputeJointInfluences(&indices, &weights) || indices.size() != weights.size())
      {
        plLog::Warning("'{}': its joint influences cannot be read, so it is not skinned.", sPath);
        continue;
      }

      plUsdSkinnedPrim out;
      out.m_sPath = sPath;
      out.m_iSkeleton = iSkeleton;
      out.m_uiInfluencesPerPoint = (plUInt32)plMath::Max(skinning.GetNumInfluencesPerComponent(), 0);
      out.m_bRigid = skinning.IsRigidlyDeformed();
      out.m_mGeomBindTransform = m_Basis.MatrixToPlasma(SkelFromGf(skinning.GetGeomBindTransform()));
      out.m_bHasBlendShapes = skinning.HasBlendShapes();
      out.m_bDualQuaternion = skinning.GetSkinningMethod() == pxr::UsdSkelTokens->dualQuaternion;

      if (out.m_uiInfluencesPerPoint == 0)
        continue;

      // Indices refer to the prim's own skel:joints order when it has one.
      plDynamicArray<plInt32> localToSkeleton;
      pxr::VtTokenArray localJoints;
      if (skinning.GetJointOrder(&localJoints))
      {
        plHashTable<plString, plInt32> jointIndex;
        for (plInt32 j = 0; j < iNumSkelJoints; ++j)
          jointIndex[skeleton.m_JointPaths[j]] = j;

        localToSkeleton.SetCount((plUInt32)localJoints.size(), -1);
        for (plUInt32 j = 0; j < localToSkeleton.GetCount(); ++j)
          jointIndex.TryGetValue(SkelFromStd(localJoints[j].GetString()), localToSkeleton[j]);
      }

      out.m_JointIndices.SetCountUninitialized((plUInt32)indices.size());
      out.m_JointWeights.SetCountUninitialized((plUInt32)weights.size());
      for (plUInt32 i = 0; i < (plUInt32)indices.size(); ++i)
      {
        plInt32 iJoint = indices[i];
        if (!localToSkeleton.IsEmpty())
          iJoint = (iJoint >= 0 && iJoint < (plInt32)localToSkeleton.GetCount()) ? localToSkeleton[iJoint] : -1;
        else if (iJoint >= iNumSkelJoints)
          iJoint = -1;

        out.m_JointIndices[i] = iJoint;
        out.m_JointWeights[i] = weights[i];
      }

      plUsdSkinnedMeshData meshData;

      if (const pxr::UsdGeomMesh mesh(prim); mesh)
      {
        // The values plUsdGeometrySource::Build reads, converted the way it converts them.
        pxr::VtVec3fArray points;
        pxr::VtIntArray faceCounts, faceIndices;
        mesh.GetPointsAttr().Get(&points, pxr::UsdTimeCode::EarliestTime());
        mesh.GetFaceVertexCountsAttr().Get(&faceCounts, pxr::UsdTimeCode::EarliestTime());
        mesh.GetFaceVertexIndicesAttr().Get(&faceIndices, pxr::UsdTimeCode::EarliestTime());

        meshData.m_Points.SetCountUninitialized((plUInt32)points.size());
        for (plUInt32 p = 0; p < (plUInt32)points.size(); ++p)
        {
          const plVec3d vPos = m_Basis.PointToPlasma(plVec3d(points[p][0], points[p][1], points[p][2]));
          meshData.m_Points[p].Set((float)vPos.x, (float)vPos.y, (float)vPos.z);
        }

        meshData.m_FaceCounts.SetCountUninitialized((plUInt32)faceCounts.size());
        for (plUInt32 f = 0; f < (plUInt32)faceCounts.size(); ++f)
          meshData.m_FaceCounts[f] = faceCounts[f];

        meshData.m_FaceIndices.SetCountUninitialized((plUInt32)faceIndices.size());
        for (plUInt32 c = 0; c < (plUInt32)faceIndices.size(); ++c)
          meshData.m_FaceIndices[c] = faceIndices[c];

        out.m_uiNumPoints = (plUInt32)points.size();
      }

      const plUInt32 uiExpected = out.m_bRigid ? out.m_uiInfluencesPerPoint : out.m_uiNumPoints * out.m_uiInfluencesPerPoint;
      if (out.m_JointIndices.GetCount() != uiExpected || (!out.m_bRigid && out.m_uiNumPoints == 0))
      {
        plLog::Warning("'{}': {} joint influences do not fit its {} points with {} influences each, so it is not skinned.", sPath, out.m_JointIndices.GetCount(),
          out.m_uiNumPoints, out.m_uiInfluencesPerPoint);
        continue;
      }

      m_SkinnedPrims.PushBack(std::move(out));
      m_MeshData.PushBack(std::move(meshData));
    }
  }
}

plUsdSkelScene::plUsdSkelScene()
  : m_pImpl(PL_DEFAULT_NEW(Impl))
{
}

plUsdSkelScene::~plUsdSkelScene() = default;

plStatus plUsdSkelScene::Open(plStringView sRootLayerFile, const plUsdBasis& basis)
{
  m_pImpl = PL_DEFAULT_NEW(Impl);
  Impl& impl = *m_pImpl;

  plStringBuilder sFile = sRootLayerFile;
  sFile.MakeCleanPath();
  if (!plOSFile::ExistsFile(sFile))
    return plStatus(plFmt("USD file not found: '{}'", sFile));

  pxr::TfErrorMark mark;
  impl.m_Stage = pxr::UsdStage::Open(SkelToStd(sFile), pxr::UsdStage::LoadAll);
  mark.Clear();

  if (!impl.m_Stage)
    return plStatus(plFmt("OpenUSD could not open '{}'", sFile));

  impl.m_Basis = basis;

  std::vector<pxr::UsdPrim> skeletonPrims;
  std::vector<pxr::UsdSkelRoot> roots;

  for (const pxr::UsdPrim& prim : impl.m_Stage->Traverse())
  {
    if (prim.IsA<pxr::UsdSkelAnimation>())
      impl.ReadAnimation(prim);
    else if (prim.IsA<pxr::UsdSkelSkeleton>())
      skeletonPrims.push_back(prim);
    else if (prim.IsA<pxr::UsdSkelRoot>())
      roots.push_back(pxr::UsdSkelRoot(prim));
  }

  // Animation names become asset file names and are how a clip is picked, so they must be unique.
  {
    plHashTable<plString, plUInt32> counts;
    for (const plUsdSkelAnimation& anim : impl.m_Animations)
      counts[anim.m_sName] += 1;

    for (plUsdSkelAnimation& anim : impl.m_Animations)
    {
      plUInt32 uiCount = 0;
      counts.TryGetValue(anim.m_sName, uiCount);
      if (uiCount > 1)
      {
        plStringBuilder sName = anim.m_sPath;
        sName.TrimWordStart("/");
        sName.ReplaceAll("/", "_");
        anim.m_sName = sName;
      }
    }
  }

  pxr::UsdGeomXformCache xformCache(pxr::UsdTimeCode::EarliestTime());
  for (const pxr::UsdPrim& prim : skeletonPrims)
    impl.ReadSkeleton(prim, xformCache);

  plSet<plString> seen;
  for (const pxr::UsdSkelRoot& root : roots)
    impl.ReadSkinnedPrims(root, seen);

  return plStatus(PL_SUCCESS);
}

bool plUsdSkelScene::IsOpen() const
{
  return m_pImpl->m_Stage != nullptr;
}

const plUsdBasis& plUsdSkelScene::GetBasis() const
{
  return m_pImpl->m_Basis;
}

const plDynamicArray<plUsdSkeleton>& plUsdSkelScene::GetSkeletons() const
{
  return m_pImpl->m_Skeletons;
}

const plDynamicArray<plUsdSkinnedPrim>& plUsdSkelScene::GetSkinnedPrims() const
{
  return m_pImpl->m_SkinnedPrims;
}

const plDynamicArray<plUsdSkelAnimation>& plUsdSkelScene::GetAnimations() const
{
  return m_pImpl->m_Animations;
}

plInt32 plUsdSkelScene::FindSkinnedPrim(plStringView sPrimPath) const
{
  for (plUInt32 i = 0; i < m_pImpl->m_SkinnedPrims.GetCount(); ++i)
  {
    if (m_pImpl->m_SkinnedPrims[i].m_sPath == sPrimPath)
      return (plInt32)i;
  }

  return -1;
}

plStatus plUsdSkelScene::SampleAnimation(plUInt32 uiAnimation, double fTimeCode, plDynamicArray<plTransform>& out_transforms) const
{
  const Impl& impl = *m_pImpl;
  if (uiAnimation >= impl.m_Animations.GetCount())
    return plStatus("animation index out of range");

  const pxr::UsdSkelAnimation anim(impl.m_AnimationPrims[uiAnimation]);
  const plUInt32 uiNumJoints = impl.m_Animations[uiAnimation].m_JointPaths.GetCount();
  const pxr::UsdTimeCode time(fTimeCode);

  pxr::VtVec3fArray translations;
  pxr::VtQuatfArray rotations;
  pxr::VtVec3hArray scales;
  anim.GetTranslationsAttr().Get(&translations, time);
  anim.GetRotationsAttr().Get(&rotations, time);
  anim.GetScalesAttr().Get(&scales, time);

  out_transforms.SetCount(uiNumJoints);
  for (plUInt32 j = 0; j < uiNumJoints; ++j)
  {
    const pxr::GfVec3f vTranslation = j < translations.size() ? translations[j] : pxr::GfVec3f(0.0f);
    const pxr::GfQuatf qRotation = j < rotations.size() ? rotations[j] : pxr::GfQuatf::GetIdentity();
    const pxr::GfVec3h vScale = j < scales.size() ? scales[j] : pxr::GfVec3h(1.0f);

    out_transforms[j] = ComponentsToPlasma(impl.m_Basis, vTranslation, qRotation, vScale);
  }

  return plStatus(PL_SUCCESS);
}

plStatus plUsdSkelScene::FindVertexPoints(plUInt32 uiSkinnedPrim, const plGeometry& geometry, plDynamicArray<plUInt32>& out_vertexPoints) const
{
  const Impl& impl = *m_pImpl;
  if (uiSkinnedPrim >= impl.m_SkinnedPrims.GetCount())
    return plStatus("skinned prim index out of range");

  const plUsdSkinnedPrim& prim = impl.m_SkinnedPrims[uiSkinnedPrim];
  const plUsdSkinnedMeshData& mesh = impl.m_MeshData[uiSkinnedPrim];
  const plDeque<plGeometry::Vertex>& vertices = geometry.GetVertices();

  if (mesh.m_Points.IsEmpty())
    return plStatus(plFmt("'{}' has no points", prim.m_sPath));

  // Build gives every vertex its point's position exactly; the tolerance only absorbs float rounding.
  float fExtent = 0.0f;
  for (const plVec3& p : mesh.m_Points)
    fExtent = plMath::Max(fExtent, plMath::Max(plMath::Abs(p.x), plMath::Max(plMath::Abs(p.y), plMath::Abs(p.z))));

  const double fCell = plMath::Max(1e-7, (double)fExtent * 1e-6);
  const float fTolerance = (float)fCell;

  plHashTable<CellKey, plHybridArray<plUInt32, 2>, CellKeyHash> grid;
  grid.Reserve(mesh.m_Points.GetCount());
  for (plUInt32 p = 0; p < mesh.m_Points.GetCount(); ++p)
    grid[MakeCellKey(mesh.m_Points[p], fCell)].PushBack(p);

  const plUInt32 n = prim.m_uiInfluencesPerPoint;
  auto SameInfluences = [&](plUInt32 a, plUInt32 b) -> bool {
    return plMemoryUtils::IsEqual(prim.m_JointIndices.GetData() + a * n, prim.m_JointIndices.GetData() + b * n, n) &&
           plMemoryUtils::IsEqual(prim.m_JointWeights.GetData() + a * n, prim.m_JointWeights.GetData() + b * n, n);
  };

  auto FindCandidates = [&](const plVec3& vPos, plHybridArray<plUInt32, 4>& out_candidates) {
    out_candidates.Clear();
    const CellKey center = MakeCellKey(vPos, fCell);

    for (plInt64 dx = -1; dx <= 1; ++dx)
    {
      for (plInt64 dy = -1; dy <= 1; ++dy)
      {
        for (plInt64 dz = -1; dz <= 1; ++dz)
        {
          const plHybridArray<plUInt32, 2>* pCell = grid.GetValue(CellKey{center.x + dx, center.y + dy, center.z + dz});
          if (pCell == nullptr)
            continue;

          for (plUInt32 p : *pCell)
          {
            if ((mesh.m_Points[p] - vPos).GetLengthSquared() <= fTolerance * fTolerance)
              out_candidates.PushBack(p);
          }
        }
      }
    }
  };

  out_vertexPoints.Clear();
  out_vertexPoints.SetCount(vertices.GetCount(), plInvalidIndex);

  plDynamicArray<plUInt32> ambiguous;
  plHybridArray<plUInt32, 4> candidates;

  for (plUInt32 v = 0; v < vertices.GetCount(); ++v)
  {
    FindCandidates(vertices[v].m_vPosition, candidates);

    if (candidates.IsEmpty())
      return plStatus(plFmt("vertex {} of '{}' is not at any of its points", v, prim.m_sPath));

    out_vertexPoints[v] = candidates[0];

    for (plUInt32 c = 1; c < candidates.GetCount(); ++c)
    {
      if (!SameInfluences(candidates[0], candidates[c]))
      {
        ambiguous.PushBack(v);
        break;
      }
    }
  }

  if (ambiguous.IsEmpty())
    return plStatus(PL_SUCCESS);

  // Coincident points that are skinned differently, such as closed lips: pick the point whose faces reach the
  // positions the vertex's triangles reach.
  plDynamicArray<plHybridArray<plUInt32, 8>> vertexNeighbours;
  vertexNeighbours.SetCount(vertices.GetCount());
  for (const plGeometry::Polygon& poly : geometry.GetPolygons())
  {
    for (plUInt32 a : poly.m_Vertices)
    {
      for (plUInt32 b : poly.m_Vertices)
      {
        if (a != b && !vertexNeighbours[a].Contains(b))
          vertexNeighbours[a].PushBack(b);
      }
    }
  }

  plDynamicArray<plHybridArray<plUInt32, 8>> pointNeighbours;
  pointNeighbours.SetCount(mesh.m_Points.GetCount());
  {
    plUInt32 uiCorner = 0;
    for (plInt32 iCount : mesh.m_FaceCounts)
    {
      if (iCount < 0 || uiCorner + iCount > mesh.m_FaceIndices.GetCount())
        break;

      for (plInt32 i = 0; i < iCount; ++i)
      {
        const plInt32 a = mesh.m_FaceIndices[uiCorner + i];
        for (plInt32 k = 0; k < iCount; ++k)
        {
          const plInt32 b = mesh.m_FaceIndices[uiCorner + k];
          if (a != b && a >= 0 && b >= 0 && (plUInt32)a < pointNeighbours.GetCount() && !pointNeighbours[a].Contains((plUInt32)b))
            pointNeighbours[a].PushBack((plUInt32)b);
        }
      }

      uiCorner += iCount;
    }
  }

  for (plUInt32 v : ambiguous)
  {
    FindCandidates(vertices[v].m_vPosition, candidates);

    plUInt32 uiBest = candidates[0];
    plUInt32 uiBestScore = 0;

    for (plUInt32 p : candidates)
    {
      plUInt32 uiScore = 0;
      for (plUInt32 nv : vertexNeighbours[v])
      {
        const plVec3& vNeighbour = vertices[nv].m_vPosition;
        for (plUInt32 np : pointNeighbours[p])
        {
          if ((mesh.m_Points[np] - vNeighbour).GetLengthSquared() <= fTolerance * fTolerance)
          {
            ++uiScore;
            break;
          }
        }
      }

      if (uiScore > uiBestScore)
      {
        uiBest = p;
        uiBestScore = uiScore;
      }
    }

    out_vertexPoints[v] = uiBest;
  }

  return plStatus(PL_SUCCESS);
}

plTransform plUsdSkelScene::ChangeFrame(const plMat3d& mAxes, double fScale, const plTransform& transform)
{
  // Conjugating by a reflection A is the same as conjugating by the rotation -A.
  const plMat3d mRotation = mAxes.GetDeterminant() < 0.0 ? mAxes * -1.0 : mAxes;
  plQuatd qAxesD = plQuatd::MakeFromMat3(mRotation);
  qAxesD.Normalize();

  const plQuat qAxes((float)qAxesD.x, (float)qAxesD.y, (float)qAxesD.z, (float)qAxesD.w);
  plQuat qOut = qAxes * transform.m_qRotation * qAxes.GetInverse();
  qOut.Normalize();

  const plVec3d vPosition = mAxes * plVec3d(transform.m_vPosition.x, transform.m_vPosition.y, transform.m_vPosition.z) * fScale;

  // A diag(s) A^T is diagonal for a signed permutation: row i takes the scale of the axis A maps onto it.
  const double fScaleIn[3] = {transform.m_vScale.x, transform.m_vScale.y, transform.m_vScale.z};
  double fScaleOut[3] = {1.0, 1.0, 1.0};
  for (plUInt32 r = 0; r < 3; ++r)
  {
    plUInt32 uiColumn = 0;
    for (plUInt32 c = 1; c < 3; ++c)
    {
      if (plMath::Abs(mAxes.Element(c, r)) > plMath::Abs(mAxes.Element(uiColumn, r)))
        uiColumn = c;
    }

    fScaleOut[r] = fScaleIn[uiColumn];
  }

  plTransform result;
  result.m_vPosition.Set((float)vPosition.x, (float)vPosition.y, (float)vPosition.z);
  result.m_qRotation = qOut;
  result.m_vScale.Set((float)fScaleOut[0], (float)fScaleOut[1], (float)fScaleOut[2]);
  return result;
}
