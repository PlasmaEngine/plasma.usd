#pragma once

#include <Foundation/Containers/Deque.h>
#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Types/UniquePtr.h>
#include <ModelImporter2/Importer/Importer.h>

/// \brief Reads OpenUSD stages (.usd, .usda, .usdc, .usdz) for the model importer registry, so the Asset Import
/// dialog's Mesh import and every later transform of the resulting mesh assets go through the same code.
///
/// The stage is read through UsdBridge, which already converts into Plasma's frame and metres from the stage's own
/// upAxis and metersPerUnit (a reflection, with triangle winding corrected). Geometry comes out exactly as the live
/// link builds it, and each prim is placed by the same local transforms the live link gives its objects.
///
/// Frame: the axis part of ImportOptions::m_RootTransform is ignored, because applying the mesh asset's axis preset
/// on top of the bridge's conversion would convert twice. Only its uniform scale is applied. A whole-stage import is
/// in the stage's root frame. An exact-name selection (ImportOptions::m_MeshExactIncludeNames) that picks the geometry
/// of a single prim is in that prim's own frame instead, so the prefab that "split by scene node" builds places it
/// with the prim's transforms, as the live link does. Any other selection stays in the root frame.
///
/// What is read: every Mesh and implicit shape (Cube, Sphere, Cylinder, Cone, Capsule) that is active and visible,
/// with visibility inherited from ancestors. Each one is a mesh named by its prim path, and a node named by its prim
/// name. Faces that share a surface, across all prims, form one sub-mesh; a prim's own binding and each of its
/// materialBind GeomSubsets are separate face groups. Nodes are every imported prim with geometry somewhere below it.
///
/// Point instancers: prims below an instancer are drawn only through it, as OpenUSD's renderers do. Each instance adds
/// a copy of its prototype's meshes, placed by the instance's transform, to the instancer's mesh (named by the
/// instancer's path, on the instancer's node), until instances make up 10 million triangles. Name filters pick an
/// instancer by its own path or name; material tags also select instance faces. Skinned imports leave instancers out.
///
/// Name filters: exact names match a prim's path or name. Include and exclude tags match the start or end of a prim's
/// path or name. An include tag equal to a material name instead selects the faces that use that material, which is
/// how "split by sub-mesh" asks for one material's faces.
///
/// Vertex data is high precision when the asset asks for it, and also when the texture coordinates form a UDIM layout
/// or reach beyond one tile, where half floats would cost tiled textures their detail.
///
/// Animated values are read at their earliest time sample. Textures are the absolute files the stage resolves to;
/// textures packed inside a .usdz are handed over as embedded textures.
///
/// Skeletal data comes from UsdSkel, through plUsdSkelScene:
/// - The skeleton is one UsdSkel Skeleton: the first that deforms a prim, else the first. Its joints keep their order
///   and hierarchy, are named by the last element of their joint path, and rest in the Skeleton's rest pose. Root
///   joints include where the Skeleton prim sits in the stage.
/// - A skinned import keeps only the Meshes and shapes that skeleton deforms, placed by their geomBindTransform in
///   skeleton space rather than by their own transforms, as UsdSkel skins them. Each vertex keeps its point's four
///   strongest influences; bones are the joints that influence a vertex, bound by the Skeleton's bindTransforms.
/// - Every SkelAnimation is a clip, named after its prim. A clip has one track per animated joint of the skeleton that
///   binds it, keyed at the animation's time samples, starting at its first sample. Keyframe options count time codes.
/// - Skeleton, skinned mesh and clips are delivered in the frame the skeleton asset's default import transform maps
///   onto the stage's Plasma frame (see plUsdSkeletalFrame), since the skeleton resource applies that transform to all
///   three at runtime.
///
/// Not imported: blend shapes, dual quaternion skinning (skinned linearly), more than one skeleton per stage, and
/// skeletons inside instances.
class plUsdModelImporter : public plModelImporter2::Importer
{
  PL_DISALLOW_COPY_AND_ASSIGN(plUsdModelImporter);

public:
  plUsdModelImporter();
  ~plUsdModelImporter();

  /// \brief The provider id the importer is registered under, which is also the package id.
  static constexpr plStringView s_sProviderId = "plasma.usd"_plsv;

protected:
  virtual plResult DoImport() override;

private:
  struct PackageFile;

  /// \brief Adds a texture packed inside a .usdz to m_OutputTextures, keyed by the path the material refers to it by.
  /// Does nothing for a texture that is an ordinary file.
  void AddPackagedTexture(plStringView sTexturePath);

  /// Packages that textures were read from. Kept open, because m_OutputTextures points into their mapped memory.
  plDynamicArray<plUniquePtr<PackageFile>> m_Packages;

  /// Uncompressed copies of packed textures that were stored compressed, which a conforming .usdz never does.
  plDeque<plDynamicArray<plUInt8>> m_UnpackedTextures;
};
