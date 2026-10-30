#include <EditorPluginUsd/EditorPluginUsdPCH.h>

#include <Core/Graphics/Geometry.h>
#include <EditorPluginUsd/Import/UsdSkelImport.h>
#include <Foundation/Containers/HashTable.h>
#include <RendererCore/AnimationSystem/AnimationClipResource.h>
#include <RendererCore/AnimationSystem/EditableSkeleton.h>
#include <RendererCore/Declarations.h>
#include <RendererCore/Meshes/MeshResourceDescriptor.h>
#include <UsdBridge/UsdSkel.h>

namespace
{
  /// Where the skeleton's space sits in the stage, as a transform that root joints are placed by.
  plTransform GetSkeletonPlacement(const plUsdSkeleton& skeleton)
  {
    return plUsdBasis::MatrixToTransform(skeleton.m_mSkeletonToStage);
  }

  /// Names for the joints of an animation no skeleton binds: the last path element, or the path where those collide.
  void MakeAnimationJointNames(const plDynamicArray<plString>& paths, plDynamicArray<plString>& out_names)
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

  /// One point's influences reduced to the four strongest.
  plUsdSkinVertex ReduceInfluences(const plUsdSkinnedPrim& prim, plUInt32 uiFirst, bool bNormalize, bool& out_bTruncated)
  {
    struct Influence
    {
      PL_DECLARE_POD_TYPE();

      plUInt16 m_uiJoint;
      float m_fWeight;
    };

    plHybridArray<Influence, 8> influences;
    float fTotal = 0.0f;

    for (plUInt32 k = 0; k < prim.m_uiInfluencesPerPoint; ++k)
    {
      const plInt32 iJoint = prim.m_JointIndices[uiFirst + k];
      const float fWeight = prim.m_JointWeights[uiFirst + k];
      if (iJoint < 0 || iJoint > 0xFFFF || !(fWeight > 0.0f))
        continue;

      fTotal += fWeight;

      bool bMerged = false;
      for (Influence& existing : influences)
      {
        if (existing.m_uiJoint == (plUInt16)iJoint)
        {
          existing.m_fWeight += fWeight;
          bMerged = true;
          break;
        }
      }

      if (!bMerged)
        influences.PushBack({(plUInt16)iJoint, fWeight});
    }

    influences.Sort([](const Influence& a, const Influence& b) { return a.m_fWeight > b.m_fWeight; });

    out_bTruncated = influences.GetCount() > 4;
    const plUInt32 uiKept = plMath::Min(influences.GetCount(), 4u);

    float fKept = 0.0f;
    for (plUInt32 i = 0; i < uiKept; ++i)
      fKept += influences[i].m_fWeight;

    // Dropped influences hand their weight to the ones that stay, so the vertex keeps its overall weighting.
    float fFactor = 1.0f;
    if (fKept > 0.0f && (bNormalize || out_bTruncated))
      fFactor = (bNormalize ? 1.0f : fTotal) / fKept;

    plUsdSkinVertex result;
    for (plUInt32 i = 0; i < uiKept; ++i)
    {
      result.m_uiJoints[i] = influences[i].m_uiJoint;
      result.m_fWeights[i] = influences[i].m_fWeight * fFactor;
    }

    return result;
  }

  /// Rounds weights to the stream's precision so that weights meant to sum to 1 still do after encoding.
  plVec4 QuantizeWeights(const float* pWeights, float fSteps)
  {
    float fSum = 0.0f;
    plInt32 iQuantized[4];
    plInt32 iSum = 0;
    plUInt32 uiLargest = 0;

    for (plUInt32 i = 0; i < 4; ++i)
    {
      fSum += pWeights[i];
      iQuantized[i] = (plInt32)plMath::Round(plMath::Clamp(pWeights[i], 0.0f, 1.0f) * fSteps);
      iSum += iQuantized[i];
      if (pWeights[i] > pWeights[uiLargest])
        uiLargest = i;
    }

    if (plMath::Abs(fSum - 1.0f) < 1e-3f)
      iQuantized[uiLargest] = plMath::Clamp(iQuantized[uiLargest] + ((plInt32)fSteps - iSum), 0, (plInt32)fSteps);

    return plVec4(iQuantized[0] / fSteps, iQuantized[1] / fSteps, iQuantized[2] / fSteps, iQuantized[3] / fSteps);
  }

  struct AnimationTrack
  {
    plUInt32 m_uiAnimationJoint = 0;
    plString m_sName;
    bool m_bRoot = false;
  };
} // namespace

//////////////////////////////////////////////////////////////////////////

plUsdSkeletalFrame plUsdSkeletalFrame::Make(float fImportScale)
{
  // The same axes SkeletonAsset builds its root transform from; its mirror handling folds into the same matrix.
  const plMeshImportTransform::Enum preset = plMeshImportTransform::Default;
  const plBasisAxis::Enum rightDir = plMeshImportTransform::GetRightDir(preset, plBasisAxis::PositiveY);
  const plBasisAxis::Enum upDir = plMeshImportTransform::GetUpDir(preset, plBasisAxis::PositiveZ);
  const bool bFlipForward = plMeshImportTransform::GetFlipForward(preset, false);
  const plBasisAxis::Enum forwardDir = plBasisAxis::GetOrthogonalAxis(rightDir, upDir, !bFlipForward);
  const plMat3 mPreset = plBasisAxis::CalculateTransformationMatrix(forwardDir, rightDir, upDir);

  // The preset is orthonormal, so its transpose is its inverse.
  plUsdSkeletalFrame frame;
  for (plUInt32 c = 0; c < 3; ++c)
    for (plUInt32 r = 0; r < 3; ++r)
      frame.m_mAxes.Element(c, r) = mPreset.Element(r, c);

  frame.m_fScale = fImportScale > 0.0f ? fImportScale : 1.0f;
  return frame;
}

plMat4d plUsdSkeletalFrame::GetMatrix() const
{
  return plMat4d(m_mAxes * m_fScale, plVec3d::MakeZero());
}

plTransform plUsdSkeletalFrame::Convert(const plTransform& transform) const
{
  return plUsdSkelScene::ChangeFrame(m_mAxes, m_fScale, transform);
}

plMat4d plUsdSkeletalFrame::Convert(const plMat4d& mTransform) const
{
  const plMat4d mTo = GetMatrix();
  const plMat4d mFrom(m_mAxes.GetTranspose() * (1.0 / m_fScale), plVec3d::MakeZero());
  return mTo * mTransform * mFrom;
}

//////////////////////////////////////////////////////////////////////////

plInt32 plUsdSkelImport::PickSkeleton(const plUsdSkelScene& scene)
{
  const plDynamicArray<plUsdSkeleton>& skeletons = scene.GetSkeletons();
  if (skeletons.IsEmpty())
    return -1;

  plInt32 iPicked = 0;
  for (const plUsdSkinnedPrim& prim : scene.GetSkinnedPrims())
  {
    if (prim.m_iSkeleton >= 0)
    {
      iPicked = prim.m_iSkeleton;
      break;
    }
  }

  if (skeletons.GetCount() > 1)
  {
    plLog::Warning("The stage has {} Skeletons; only '{}' is imported, with the prims it deforms.", skeletons.GetCount(), skeletons[iPicked].m_sPath);
  }

  return iPicked;
}

void plUsdSkelImport::ComputeRestPose(const plUsdSkelScene& scene, plInt32 iSkeleton, const plUsdSkeletalFrame& frame, plDynamicArray<plTransform>& out_localTransforms)
{
  const plUsdSkeleton& skeleton = scene.GetSkeletons()[iSkeleton];
  const plTransform placement = GetSkeletonPlacement(skeleton);

  out_localTransforms.SetCount(skeleton.m_RestTransforms.GetCount());
  for (plUInt32 j = 0; j < skeleton.m_RestTransforms.GetCount(); ++j)
  {
    plTransform t = skeleton.m_RestTransforms[j];
    if (skeleton.m_JointParents[j] < 0)
      t = plTransform::MakeGlobalTransform(placement, t);

    out_localTransforms[j] = frame.Convert(t);
  }
}

plResult plUsdSkelImport::ImportSkeleton(const plUsdSkelScene& scene, plInt32 iSkeleton, const plUsdSkeletalFrame& frame, plEditableSkeleton& out_skeleton)
{
  const plUsdSkeleton& skeleton = scene.GetSkeletons()[iSkeleton];
  const plUInt32 uiNumJoints = skeleton.m_JointPaths.GetCount();

  if (uiNumJoints == 0)
  {
    plLog::Error("Skeleton '{}' has no joints.", skeleton.m_sPath);
    return PL_FAILURE;
  }

  const plTransform placement = GetSkeletonPlacement(skeleton);
  if (!placement.m_vScale.IsEqual(plVec3(placement.m_vScale.x), 0.0001f))
  {
    plLog::Warning("Skeleton '{}' is placed in the stage with non-uniform or mirrored scale; its root joints only approximate it.", skeleton.m_sPath);
  }

  if (!skeleton.m_bHasRestTransforms)
  {
    plLog::Info("Skeleton '{}' has no restTransforms; the rest pose is its bind pose.", skeleton.m_sPath);
  }

  plDynamicArray<plTransform> rest;
  ComputeRestPose(scene, iSkeleton, frame, rest);

  plDynamicArray<plEditableSkeletonJoint*> joints;
  joints.SetCount(uiNumJoints);

  // UsdSkel lists parents before their children.
  for (plUInt32 j = 0; j < uiNumJoints; ++j)
  {
    plEditableSkeletonJoint* pJoint = PL_DEFAULT_NEW(plEditableSkeletonJoint);
    pJoint->m_sName.Assign(skeleton.m_JointNames[j]);
    pJoint->m_LocalTransform = rest[j];
    joints[j] = pJoint;

    const plInt32 iParent = skeleton.m_JointParents[j];
    if (iParent >= 0 && (plUInt32)iParent < j)
      joints[iParent]->m_Children.PushBack(pJoint);
    else
      out_skeleton.m_Children.PushBack(pJoint);
  }

  return PL_SUCCESS;
}

plResult plUsdSkelImport::ImportAnimation(const plUsdSkelScene& scene, const plModelImporter2::ImportOptions& options, const plUsdSkeletalFrame& frame, plAnimationClipResourceDescriptor& out_clip)
{
  const plDynamicArray<plUsdSkelAnimation>& animations = scene.GetAnimations();
  if (animations.IsEmpty())
  {
    plLog::Error("The stage has no SkelAnimation.");
    return PL_FAILURE;
  }

  plUInt32 uiAnimation = 0;
  if (!options.m_sAnimationToImport.IsEmpty())
  {
    uiAnimation = plInvalidIndex;
    for (plUInt32 a = 0; a < animations.GetCount(); ++a)
    {
      if (animations[a].m_sName == options.m_sAnimationToImport || animations[a].m_sPath == options.m_sAnimationToImport)
      {
        uiAnimation = a;
        break;
      }
    }

    if (uiAnimation == plInvalidIndex)
    {
      plLog::Error("The stage has no SkelAnimation named '{}'.", options.m_sAnimationToImport);
      return PL_FAILURE;
    }
  }

  const plUsdSkelAnimation& anim = animations[uiAnimation];
  const plUsdSkeleton* pSkeleton = anim.m_iSkeleton >= 0 ? &scene.GetSkeletons()[anim.m_iSkeleton] : nullptr;

  if (anim.m_bHasBlendShapeWeights)
  {
    plLog::Warning("SkelAnimation '{}' animates blend shapes, which are not imported.", anim.m_sPath);
  }

  // --- Tracks: every joint of the animation that its skeleton has, named the way the skeleton names it. ---
  plDynamicArray<AnimationTrack> tracks;
  {
    plDynamicArray<plString> unboundNames;
    if (pSkeleton == nullptr)
    {
      plLog::Warning("SkelAnimation '{}' is not bound to a Skeleton; its tracks are named after its joints and are not placed in the stage.", anim.m_sPath);
      MakeAnimationJointNames(anim.m_JointPaths, unboundNames);
    }

    plUInt32 uiUnmapped = 0;
    for (plUInt32 aj = 0; aj < anim.m_JointPaths.GetCount(); ++aj)
    {
      AnimationTrack track;
      track.m_uiAnimationJoint = aj;

      if (pSkeleton != nullptr)
      {
        const plInt32 iJoint = anim.m_SkeletonJoints[aj];
        if (iJoint < 0)
        {
          ++uiUnmapped;
          continue;
        }

        track.m_sName = pSkeleton->m_JointNames[iJoint];
        track.m_bRoot = pSkeleton->m_JointParents[iJoint] < 0;
      }
      else
      {
        track.m_sName = unboundNames[aj];
      }

      tracks.PushBack(track);
    }

    if (uiUnmapped > 0)
    {
      plLog::Warning("SkelAnimation '{}' animates {} joints that Skeleton '{}' does not have; they are left out.", anim.m_sPath, uiUnmapped, pSkeleton->m_sPath);
    }
  }

  if (tracks.IsEmpty())
  {
    plLog::Error("SkelAnimation '{}' animates no joint.", anim.m_sPath);
    return PL_FAILURE;
  }

  const plTransform placement = pSkeleton != nullptr ? GetSkeletonPlacement(*pSkeleton) : plTransform::MakeIdentity();

  // --- Time range, in time codes. Keyframe options count time codes from the first sample. ---
  const double fClipStart = anim.m_TimeSamples.IsEmpty() ? 0.0 : anim.m_TimeSamples[0];
  const double fClipEnd = anim.m_TimeSamples.IsEmpty() ? 0.0 : anim.m_TimeSamples.PeekBack();

  const double fRangeStart = plMath::Min(fClipStart + (double)options.m_uiFirstAnimKeyframe, fClipEnd);
  const double fRangeEnd = options.m_uiNumAnimKeyframes > 0 ? plMath::Min(fRangeStart + (double)options.m_uiNumAnimKeyframes, fClipEnd) : fClipEnd;

  plDynamicArray<double> times;
  times.PushBack(fRangeStart);
  for (double fTime : anim.m_TimeSamples)
  {
    if (fTime > fRangeStart && fTime < fRangeEnd)
      times.PushBack(fTime);
  }
  if (fRangeEnd > fRangeStart)
    times.PushBack(fRangeEnd);

  if (times.GetCount() > 0xFFFF)
  {
    plLog::Error("SkelAnimation '{}' has {} keys in the imported range; at most 65535 are supported.", anim.m_sPath, times.GetCount());
    return PL_FAILURE;
  }

  const double fTimeCodesPerSecond = anim.m_fTimeCodesPerSecond > 0.0 ? anim.m_fTimeCodesPerSecond : 24.0;

  // Joint transforms in the skeletal frame, root joints placed like the skeleton's.
  plDynamicArray<plTransform> sampled;
  auto Sample = [&](double fTime, plDynamicArray<plTransform>& out_tracks) -> plResult {
    const plStatus res = scene.SampleAnimation(uiAnimation, fTime, sampled);
    if (res.Failed())
    {
      plLog::Error("SkelAnimation '{}' cannot be sampled: {}", anim.m_sPath, res.m_sMessage);
      return PL_FAILURE;
    }

    out_tracks.SetCount(tracks.GetCount());
    for (plUInt32 t = 0; t < tracks.GetCount(); ++t)
    {
      plTransform transform = sampled[tracks[t].m_uiAnimationJoint];
      if (tracks[t].m_bRoot)
        transform = plTransform::MakeGlobalTransform(placement, transform);

      transform = frame.Convert(transform);
      transform.m_vPosition *= options.m_fAnimationPositionScale;
      out_tracks[t] = transform;
    }

    return PL_SUCCESS;
  };

  // keys[time][track]
  plDynamicArray<plDynamicArray<plTransform>> keys;
  keys.SetCount(times.GetCount());
  for (plUInt32 k = 0; k < times.GetCount(); ++k)
    PL_SUCCEED_OR_RETURN(Sample(times[k], keys[k]));

  // Additive clips hold each key relative to a reference key of the whole clip, the way ozz's additive builder makes them.
  if (options.m_bAdditiveAnimation)
  {
    plDynamicArray<plTransform> reference;
    PL_SUCCEED_OR_RETURN(Sample(options.m_AdditiveReference == plModelImporter2::AdditiveReference::LastKeyFrame ? fClipEnd : fClipStart, reference));

    for (plDynamicArray<plTransform>& key : keys)
    {
      for (plUInt32 t = 0; t < tracks.GetCount(); ++t)
      {
        const plTransform& ref = reference[t];
        plTransform& value = key[t];

        value.m_vPosition -= ref.m_vPosition;
        value.m_qRotation = ref.m_qRotation.GetInverse() * value.m_qRotation;
        value.m_qRotation.Normalize();

        for (plUInt32 c = 0; c < 3; ++c)
        {
          const float fRef = ref.m_vScale.GetData()[c];
          value.m_vScale.GetData()[c] = plMath::Abs(fRef) > 1e-6f ? value.m_vScale.GetData()[c] / fRef : 1.0f;
        }
      }
    }
  }

  // Neighbouring keys on the same hemisphere, so interpolation takes the short way.
  for (plUInt32 k = 1; k < keys.GetCount(); ++k)
  {
    for (plUInt32 t = 0; t < tracks.GetCount(); ++t)
    {
      plQuat& q = keys[k][t].m_qRotation;
      if (q.Dot(keys[k - 1][t].m_qRotation) < 0.0f)
        q = plQuat(-q.x, -q.y, -q.z, -q.w);
    }
  }

  // --- Output. A channel that never changes is one key. ---
  out_clip.m_bAdditive = options.m_bAdditiveAnimation;

  const plUInt32 uiNumKeys = keys.GetCount();
  plDynamicArray<plUInt16> positionCounts, rotationCounts, scaleCounts;
  positionCounts.SetCount(tracks.GetCount());
  rotationCounts.SetCount(tracks.GetCount());
  scaleCounts.SetCount(tracks.GetCount());

  for (plUInt32 t = 0; t < tracks.GetCount(); ++t)
  {
    bool bPositionConstant = true;
    bool bRotationConstant = true;
    bool bScaleConstant = true;

    const plTransform& first = keys[0][t];
    for (plUInt32 k = 1; k < uiNumKeys; ++k)
    {
      const plTransform& value = keys[k][t];
      bPositionConstant = bPositionConstant && value.m_vPosition.IsEqual(first.m_vPosition, 1e-6f);
      bRotationConstant = bRotationConstant && plMath::Abs(value.m_qRotation.Dot(first.m_qRotation)) > 1.0f - 1e-7f;
      bScaleConstant = bScaleConstant && value.m_vScale.IsEqual(first.m_vScale, 1e-6f);
    }

    positionCounts[t] = bPositionConstant ? 1 : (plUInt16)uiNumKeys;
    rotationCounts[t] = bRotationConstant ? 1 : (plUInt16)uiNumKeys;
    scaleCounts[t] = bScaleConstant ? 1 : (plUInt16)uiNumKeys;

    plHashedString sName;
    sName.Assign(tracks[t].m_sName);
    out_clip.CreateJoint(sName, positionCounts[t], rotationCounts[t], scaleCounts[t]);
  }

  out_clip.AllocateJointTransforms();

  for (plUInt32 t = 0; t < tracks.GetCount(); ++t)
  {
    const plAnimationClipResourceDescriptor::JointInfo* pJoint = out_clip.GetJointInfo(plTempHashedString(tracks[t].m_sName));
    if (pJoint == nullptr)
      continue;

    plArrayPtr<plAnimationClipResourceDescriptor::KeyframeVec3> positions = out_clip.GetPositionKeyframes(*pJoint);
    plArrayPtr<plAnimationClipResourceDescriptor::KeyframeQuat> rotations = out_clip.GetRotationKeyframes(*pJoint);
    plArrayPtr<plAnimationClipResourceDescriptor::KeyframeVec3> scales = out_clip.GetScaleKeyframes(*pJoint);

    for (plUInt32 k = 0; k < positions.GetCount(); ++k)
    {
      positions[k].m_fTimeInSec = (float)((times[k] - fRangeStart) / fTimeCodesPerSecond);
      positions[k].m_Value = keys[k][t].m_vPosition;
    }

    for (plUInt32 k = 0; k < rotations.GetCount(); ++k)
    {
      rotations[k].m_fTimeInSec = (float)((times[k] - fRangeStart) / fTimeCodesPerSecond);
      rotations[k].m_Value = keys[k][t].m_qRotation;
    }

    for (plUInt32 k = 0; k < scales.GetCount(); ++k)
    {
      scales[k].m_fTimeInSec = (float)((times[k] - fRangeStart) / fTimeCodesPerSecond);
      scales[k].m_Value = keys[k][t].m_vScale;
    }
  }

  out_clip.SetDuration(plTime::MakeFromSeconds(plMath::Max((fRangeEnd - fRangeStart) / fTimeCodesPerSecond, 1.0 / 60.0)));

  // The rest pose the tracks were authored against, so the runtime can match them to a skeleton from another source.
  // It is the same pose the skeleton import gives the joints, which makes playback on that skeleton exact.
  if (pSkeleton != nullptr)
  {
    plDynamicArray<plTransform> rest;
    ComputeRestPose(scene, anim.m_iSkeleton, frame, rest);

    plDynamicArray<plQuat> globalRotations;
    globalRotations.SetCount(rest.GetCount());

    for (plUInt32 j = 0; j < rest.GetCount(); ++j)
    {
      const plInt32 iParent = pSkeleton->m_JointParents[j];
      globalRotations[j] = (iParent >= 0 && (plUInt32)iParent < j) ? globalRotations[iParent] * rest[j].m_qRotation : rest[j].m_qRotation;

      plAnimationClipResourceDescriptor::SourceJoint joint;
      if (iParent >= 0)
        joint.m_sParent.Assign(pSkeleton->m_JointNames[iParent]);
      joint.m_LocalRestPose = rest[j];
      joint.m_GlobalRestRotation = globalRotations[j];

      if (!joint.m_LocalRestPose.IsValid() || !joint.m_GlobalRestRotation.IsValid())
        continue;

      plHashedString sName;
      sName.Assign(pSkeleton->m_JointNames[j]);
      out_clip.m_SourceBindPose.Insert(sName, joint);
    }
  }

  return PL_SUCCESS;
}

plMat4 plUsdSkelImport::GetSkinnedGeometryTransform(const plUsdSkelScene& scene, plUInt32 uiSkinnedPrim, const plUsdSkeletalFrame& frame)
{
  const plUsdSkinnedPrim& prim = scene.GetSkinnedPrims()[uiSkinnedPrim];
  const plUsdSkeleton& skeleton = scene.GetSkeletons()[prim.m_iSkeleton];
  return plUsdBasis::ToFloat(frame.GetMatrix() * skeleton.m_mSkeletonToStage * prim.m_mGeomBindTransform);
}

plStatus plUsdSkelImport::ComputeSkinVertices(const plUsdSkelScene& scene, plUInt32 uiSkinnedPrim, const plGeometry& geometry, bool bNormalize,
  plDynamicArray<plUsdSkinVertex>& out_vertices, plUInt32& inout_uiTruncatedPoints)
{
  const plUsdSkinnedPrim& prim = scene.GetSkinnedPrims()[uiSkinnedPrim];
  const plUInt32 uiNumVertices = geometry.GetVertices().GetCount();

  out_vertices.Clear();

  if (prim.m_bRigid)
  {
    bool bTruncated = false;
    const plUsdSkinVertex influences = ReduceInfluences(prim, 0, bNormalize, bTruncated);
    if (bTruncated)
      ++inout_uiTruncatedPoints;

    out_vertices.SetCount(uiNumVertices, influences);
    return plStatus(PL_SUCCESS);
  }

  plDynamicArray<plUInt32> vertexPoints;
  PL_SUCCEED_OR_RETURN(scene.FindVertexPoints(uiSkinnedPrim, geometry, vertexPoints));

  plDynamicArray<plUsdSkinVertex> pointInfluences;
  pointInfluences.SetCount(prim.m_uiNumPoints);
  for (plUInt32 p = 0; p < prim.m_uiNumPoints; ++p)
  {
    bool bTruncated = false;
    pointInfluences[p] = ReduceInfluences(prim, p * prim.m_uiInfluencesPerPoint, bNormalize, bTruncated);
    if (bTruncated)
      ++inout_uiTruncatedPoints;
  }

  out_vertices.SetCountUninitialized(uiNumVertices);
  for (plUInt32 v = 0; v < uiNumVertices; ++v)
    out_vertices[v] = pointInfluences[vertexPoints[v]];

  return plStatus(PL_SUCCESS);
}

void plUsdSkelImport::WriteSkinning(const plUsdSkelScene& scene, plInt32 iSkeleton, const plUsdSkeletalFrame& frame, const plDynamicArray<plUsdSkinVertex>& skinVertices,
  plMeshResourceDescriptor& ref_mesh)
{
  const plUsdSkeleton& skeleton = scene.GetSkeletons()[iSkeleton];
  const plUInt32 uiNumJoints = skeleton.m_JointPaths.GetCount();

  // Only joints that influence a vertex become bones, numbered in joint order.
  plDynamicArray<plUInt16> jointToBone;
  jointToBone.SetCount(uiNumJoints, plInvalidJointIndex);

  for (const plUsdSkinVertex& vertex : skinVertices)
  {
    for (plUInt32 i = 0; i < 4; ++i)
    {
      if (vertex.m_fWeights[i] > 0.0f && vertex.m_uiJoints[i] < uiNumJoints)
        jointToBone[vertex.m_uiJoints[i]] = 0;
    }
  }

  // Bone space from the mesh's space: the bind pose placed in the stage, in the skeletal frame.
  const plMat4d mPlacement = skeleton.m_mSkeletonToStage;
  plDynamicArray<plVec3> bindPositions;
  bindPositions.SetCount(uiNumJoints, plVec3::MakeZero());

  plUInt16 uiNextBone = 0;
  for (plUInt32 j = 0; j < uiNumJoints; ++j)
  {
    if (jointToBone[j] == plInvalidJointIndex)
      continue;

    jointToBone[j] = uiNextBone++;
    const plMat4d mBind = frame.Convert(mPlacement * skeleton.m_BindTransforms[j]);
    bindPositions[j].Set((float)mBind.Element(3, 0), (float)mBind.Element(3, 1), (float)mBind.Element(3, 2));

    plMeshResourceDescriptor::BoneData bone;
    bone.m_GlobalInverseRestPoseMatrix = plUsdBasis::ToFloat(mBind.GetInverse(0.0));
    bone.m_uiBoneIndex = jointToBone[j];

    plHashedString sName;
    sName.Assign(skeleton.m_JointNames[j]);
    ref_mesh.m_Bones.Insert(sName, bone);
  }

  plMeshBufferResourceDescriptor& buffer = ref_mesh.MeshBufferDesc();
  const float fSteps = buffer.GetVertexStreamConfig().GetBoneWeightsFormat() == plGALResourceFormat::RGBAUShortNormalized ? 65535.0f : 255.0f;

  plUInt32 uiZeroWeights = 0;
  float fMaxOffset = ref_mesh.m_fMaxBoneVertexOffset;

  for (plUInt32 v = 0; v < skinVertices.GetCount(); ++v)
  {
    const plUsdSkinVertex& vertex = skinVertices[v];

    plVec4U16 indices = plVec4U16::MakeZero();
    for (plUInt32 i = 0; i < 4; ++i)
    {
      if (vertex.m_fWeights[i] > 0.0f && vertex.m_uiJoints[i] < uiNumJoints)
        indices.GetData()[i] = jointToBone[vertex.m_uiJoints[i]];
    }

    const plVec4 weights = QuantizeWeights(vertex.m_fWeights, fSteps);
    buffer.SetBoneIndices(v, indices);
    buffer.SetBoneWeights(v, weights);

    if (weights.IsZero(0.001f))
    {
      ++uiZeroWeights;
      continue;
    }

    // How far a vertex strays from the bones that move it. The runtime pads the bounds of the posed joint positions by
    // this, in the mesh's space, so it is measured there rather than in the bones' possibly scaled spaces.
    const plVec3 vPosition = buffer.GetPosition(v);
    for (plUInt32 i = 0; i < 4; ++i)
    {
      if (vertex.m_fWeights[i] < 0.2f || vertex.m_uiJoints[i] >= uiNumJoints)
        continue;

      fMaxOffset = plMath::Max(fMaxOffset, (vPosition - bindPositions[vertex.m_uiJoints[i]]).GetLength());
    }
  }

  ref_mesh.m_fMaxBoneVertexOffset = fMaxOffset;

  if (uiZeroWeights > 0)
  {
    plLog::Error("Mesh has {} vertices with bone weights that are zero. These will not show up!", uiZeroWeights);
  }
}
