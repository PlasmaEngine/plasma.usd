#pragma once

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Math/Mat3.h>
#include <Foundation/Math/Mat4.h>
#include <Foundation/Math/Transform.h>
#include <Foundation/Types/Status.h>
#include <ModelImporter2/Importer/Importer.h>

class plEditableSkeleton;
class plGeometry;
class plMeshResourceDescriptor;
class plUsdSkelScene;
struct plAnimationClipResourceDescriptor;

/// \brief The frame skeletons, skinned meshes and animation clips from USD are delivered in.
///
/// A skeleton resource applies its asset's import transform at runtime, to the skeleton, the skinned mesh and every
/// clip played on it, and the importer is never told what that transform is. Skeleton assets start with the default
/// preset (plMeshImportTransform::Default), so skeletal data is delivered in the frame that preset maps onto the
/// stage's Plasma frame: with default skeleton settings a character lands where the live link and a static import of
/// the stage place it.
///
/// The frame is Plasma's, taken through the preset's inverse (a reflection) and the uniform import scale. Joint
/// transforms are conjugated by it and stay rotations. Geometry is reflected with it, so its winding is reversed; the
/// renderer reverses it again for the mirrored root transform.
struct plUsdSkeletalFrame
{
  plMat3d m_mAxes = plMat3d::MakeIdentity(); ///< Plasma's frame -> the frame the default preset expects.
  double m_fScale = 1.0;

  static plUsdSkeletalFrame Make(float fImportScale);

  /// \brief Positions in Plasma's frame -> positions in this frame.
  plMat4d GetMatrix() const;

  /// \brief A transform in Plasma's frame, expressed in this one.
  plTransform Convert(const plTransform& transform) const;
  plMat4d Convert(const plMat4d& mTransform) const;
};

/// \brief One vertex's four strongest influences, as skeleton joint indices.
struct plUsdSkinVertex
{
  PL_DECLARE_POD_TYPE();

  plUInt16 m_uiJoints[4] = {0, 0, 0, 0};
  float m_fWeights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

/// \brief Turns a plUsdSkelScene into the skeleton, skinned mesh and animation clip outputs of the model importer.
namespace plUsdSkelImport
{
  /// \brief The skeleton a stage is imported with: the first one that deforms a prim, else the first one. -1 when the
  /// stage has none. Warns when the stage has more than one.
  plInt32 PickSkeleton(const plUsdSkelScene& scene);

  /// \brief Joint rest poses relative to their parents, in the skeletal frame. Root joints include the Skeleton prim's
  /// placement in the stage.
  void ComputeRestPose(const plUsdSkelScene& scene, plInt32 iSkeleton, const plUsdSkeletalFrame& frame, plDynamicArray<plTransform>& out_localTransforms);

  plResult ImportSkeleton(const plUsdSkelScene& scene, plInt32 iSkeleton, const plUsdSkeletalFrame& frame, plEditableSkeleton& out_skeleton);

  /// \brief Fills a clip from the SkelAnimation ImportOptions::m_sAnimationToImport names (by name or prim path), or
  /// from the first one. Honours the keyframe range, additive and position scale options.
  plResult ImportAnimation(const plUsdSkelScene& scene, const plModelImporter2::ImportOptions& options, const plUsdSkeletalFrame& frame, plAnimationClipResourceDescriptor& out_clip);

  /// \brief Takes a skinned prim's geometry, as plUsdGeometrySource::Build makes it, to where it is bound: through its
  /// geomBindTransform into skeleton space, into the stage by the Skeleton's placement, and into the skeletal frame.
  plMat4 GetSkinnedGeometryTransform(const plUsdSkelScene& scene, plUInt32 uiSkinnedPrim, const plUsdSkeletalFrame& frame);

  /// \brief The influences of every vertex of a skinned prim's geometry, as plUsdGeometrySource::Build made it and
  /// before any change. More than four influences keep the four strongest, scaled to the weight they had together;
  /// bNormalize scales every vertex's weights to a sum of 1. out_uiTruncatedPoints counts points that had more than
  /// four.
  plStatus ComputeSkinVertices(const plUsdSkelScene& scene, plUInt32 uiSkinnedPrim, const plGeometry& geometry, bool bNormalize,
    plDynamicArray<plUsdSkinVertex>& out_vertices, plUInt32& inout_uiTruncatedPoints);

  /// \brief Adds the bones the vertices use to the mesh, and writes its bone index and weight stream, whose vertices
  /// match skinVertices one to one. The mesh buffer must have the SkinningData stream allocated.
  void WriteSkinning(const plUsdSkelScene& scene, plInt32 iSkeleton, const plUsdSkeletalFrame& frame, const plDynamicArray<plUsdSkinVertex>& skinVertices,
    plMeshResourceDescriptor& ref_mesh);
} // namespace plUsdSkelImport
