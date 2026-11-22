#pragma once

#include <Foundation/Strings/String.h>
#include <Foundation/Types/Status.h>

namespace plModelImporter2
{
  struct FoliageCompositeSource;
}

/// \brief Reads composite trees from USD into plModelImporter2::FoliageCompositeSource.
///
/// A composite tree is a root prim that marks itself as one (the "unreal:naniteAssembly:meshType" attribute some tree
/// libraries author) with a relationship to a UsdSkel Skeleton, a skinned core mesh bound to that skeleton (trunk and
/// limbs), and a UsdGeomPointInstancer that places part meshes (branches, twigs) with per-instance bind joints and
/// weights. The result is in the root prim's space, in Plasma's frame and metres (plUsdBasis):
///
/// - Joints: the skeleton's joints at their bind pose; parents from the joint paths. Branch orders come from the
///   "<stage>_DynamicWind.json" file beside the stage (Joints[].SimulationGroupIndex, bIsGroundCover, GustAttenuation)
///   when there is one, else from the number of branch points between a joint and the root.
/// - Parts: one per instancer prototype, all of its meshes merged in the prototype's space, plus the core mesh as a part
///   with one instance whose vertices name their dominant joint in TexCoord1.x. A prototype's own skeleton is ignored:
///   parts are rigid.
/// - Instances: the instancer's transforms with the bind joints mapped to skeleton joint indices.
/// - Materials: one slot per distinct bound material across the tree, named after the material prim; every part's
///   sub-meshes index these slots. The slots' resource ids stay empty.
namespace plUsdFoliageComposite
{
  /// \brief Whether the stage's root layer holds a composite tree. Opens only the root layer, so it is cheap.
  bool IsCompositeTree(plStringView sFile);

  /// \brief Reads the tree. Fails with a message when the stage is no composite tree or a part cannot be built.
  plStatus Read(plStringView sFile, plModelImporter2::FoliageCompositeSource& out_source);
} // namespace plUsdFoliageComposite
