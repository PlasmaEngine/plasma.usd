#pragma once

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Math/Mat3.h>
#include <Foundation/Math/Mat4.h>
#include <Foundation/Math/Transform.h>
#include <Foundation/Strings/String.h>
#include <Foundation/Types/Status.h>
#include <Foundation/Types/UniquePtr.h>
#include <UsdBridge/UsdBasis.h>

class plGeometry;

/// \brief A UsdSkel Skeleton, in Plasma's frame and metres (see plUsdBasis).
///
/// Transforms convert like every other transform the bridge reads, M_plasma = (S C) M_usd (C S^-1), so they stay
/// rotations while the geometry they deform is reflected. Skeleton space is the Skeleton prim's own space.
struct plUsdSkeleton
{
  plString m_sPath;

  /// The joints as the Skeleton lists them: paths from a root joint, such as "Hips/Spine/Chest". Parents come before
  /// their children.
  plDynamicArray<plString> m_JointPaths;

  /// One name per joint, unique within the skeleton: the last element of the joint's path, or the whole path for
  /// joints whose last elements collide.
  plDynamicArray<plString> m_JointNames;

  plDynamicArray<plInt32> m_JointParents; ///< Index of each joint's parent, -1 for root joints.

  /// Each joint's rest pose relative to its parent, or to skeleton space for root joints. From restTransforms; derived
  /// from bindTransforms when restTransforms is not authored.
  plDynamicArray<plTransform> m_RestTransforms;

  /// Each joint's bind pose in skeleton space. From bindTransforms; the concatenated rest pose when bindTransforms is
  /// not authored. UsdSkel's skinning treats these as skeleton space, and so does everything reading them.
  plDynamicArray<plMat4d> m_BindTransforms;

  bool m_bHasRestTransforms = false;
  bool m_bHasBindTransforms = false;

  /// The Skeleton prim's local-to-world transform at the stage's earliest time: where skeleton space sits in the
  /// stage's root frame.
  plMat4d m_mSkeletonToStage = plMat4d::MakeIdentity();

  /// The SkelAnimation its skel:animationSource binds (index into plUsdSkelScene::GetAnimations()), or -1.
  plInt32 m_iAnimation = -1;
};

/// \brief A Mesh or shape that a Skeleton deforms: it has UsdSkelBindingAPI joint influences and sits under a SkelRoot.
struct plUsdSkinnedPrim
{
  plString m_sPath;
  plInt32 m_iSkeleton = -1; ///< Index into plUsdSkelScene::GetSkeletons().

  /// Influences per point: jointIndices' and jointWeights' elementSize.
  plUInt32 m_uiInfluencesPerPoint = 0;

  /// One set of influences for every point (constant interpolation). Otherwise one set per point.
  bool m_bRigid = false;

  /// m_uiInfluencesPerPoint entries per point, or just one set when rigid. Indices are into the skeleton's joints; a
  /// skel:joints order of the prim is resolved already. -1 marks an influence on a joint the skeleton does not have.
  plDynamicArray<plInt32> m_JointIndices;
  plDynamicArray<float> m_JointWeights;

  /// geomBindTransform in Plasma's frame: takes the prim's geometry, as plUsdGeometrySource::Build makes it, into
  /// skeleton space at bind time. The prim's own transform plays no part in skinning.
  plMat4d m_mGeomBindTransform = plMat4d::MakeIdentity();

  plUInt32 m_uiNumPoints = 0; ///< Points of a Mesh; 0 for implicit shapes.
  bool m_bHasBlendShapes = false;
  bool m_bDualQuaternion = false; ///< skinningMethod asks for dual quaternion skinning.
};

/// \brief A UsdSkel SkelAnimation: joint transforms over time.
struct plUsdSkelAnimation
{
  plString m_sPath;

  /// The prim's name, or its path with '/' turned into '_' when another SkelAnimation has the same name.
  plString m_sName;

  /// The first Skeleton whose skel:animationSource binds this animation, or -1.
  plInt32 m_iSkeleton = -1;

  plDynamicArray<plString> m_JointPaths; ///< The animation's joints, in the order SampleAnimation reports them.

  /// Per animation joint: index into the bound skeleton's joints, or -1 when unbound or not in the skeleton.
  plDynamicArray<plInt32> m_SkeletonJoints;

  /// Every time code at which some joint transform has a sample, ascending. Empty for a pose without samples.
  plDynamicArray<double> m_TimeSamples;

  double m_fTimeCodesPerSecond = 24.0;
  bool m_bHasBlendShapeWeights = false;
};

/// \brief The UsdSkel content of a stage: skeletons, the prims they deform, and animations, read through Plasma types.
///
/// Opens the root layer file on a stage of its own, as saved on disk, so a plUsdStage's edit layer or muted layers are
/// not part of what this reads. The basis must be the one plUsdStage reports for the same file, so joints and the
/// geometry plUsdGeometrySource builds share one frame.
///
/// Static values are read at the stage's earliest time, like plUsdStage reads them. Only prims that are active, and
/// not inside instances, are read.
class plUsdSkelScene
{
  PL_DISALLOW_COPY_AND_ASSIGN(plUsdSkelScene);

public:
  plUsdSkelScene();
  ~plUsdSkelScene();

  plStatus Open(plStringView sRootLayerFile, const plUsdBasis& basis);
  bool IsOpen() const;

  const plUsdBasis& GetBasis() const;

  /// \brief Skeletons, in stage traversal order.
  const plDynamicArray<plUsdSkeleton>& GetSkeletons() const;

  /// \brief Deformed prims, in the order their SkelRoots and bindings list them.
  const plDynamicArray<plUsdSkinnedPrim>& GetSkinnedPrims() const;

  /// \brief SkelAnimations, in stage traversal order.
  const plDynamicArray<plUsdSkelAnimation>& GetAnimations() const;

  /// \brief Index into GetSkinnedPrims() of the prim with this path, or -1.
  plInt32 FindSkinnedPrim(plStringView sPrimPath) const;

  /// \brief Joint transforms of an animation at a time code, relative to each joint's parent (skeleton space for root
  /// joints), in the order of the animation's joints. Components the animation does not author are identity.
  plStatus SampleAnimation(plUInt32 uiAnimation, double fTimeCode, plDynamicArray<plTransform>& out_transforms) const;

  /// \brief For the geometry of a skinned Mesh as plUsdGeometrySource::Build made it, before any change: the point each
  /// vertex was made from.
  ///
  /// Build turns every point into one or more vertices at the point's position. Points that share a position are told
  /// apart by their neighbours only when their influences differ.
  plStatus FindVertexPoints(plUInt32 uiSkinnedPrim, const plGeometry& geometry, plDynamicArray<plUInt32>& out_vertexPoints) const;

  /// \brief Re-expresses a transform in a frame whose axes are a signed permutation of the current ones, scaled
  /// uniformly: the TRS form of (s A) T (s A)^-1.
  ///
  /// A may be a reflection; the rotation stays a rotation, and the scale's components are permuted. This is how the
  /// bridge converts joint transforms (A = the basis' axis map, s = metersPerUnit), and how further frame changes
  /// on converted data stay consistent with it.
  static plTransform ChangeFrame(const plMat3d& mAxes, double fScale, const plTransform& transform);

private:
  struct Impl;
  plUniquePtr<Impl> m_pImpl;
};
