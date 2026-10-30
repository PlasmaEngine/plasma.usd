#pragma once

#include <Core/Graphics/Geometry.h>
#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Containers/HashTable.h>
#include <Foundation/Math/BoundingBox.h>
#include <Foundation/Math/Transform.h>
#include <Foundation/Math/Vec2.h>
#include <Foundation/Strings/String.h>
#include <Foundation/Threading/AtomicInteger.h>
#include <Foundation/Time/Time.h>
#include <Foundation/Types/Delegate.h>
#include <Foundation/Types/Status.h>
#include <UsdBridge/UsdStage.h>

/// \brief A variant selection a partition scan makes in the stage's session layer, so no file changes.
struct plUsdVariantOverride
{
  plString m_sPrimPath; ///< Empty: the scanned root.
  plString m_sSet;
  plString m_sVariant;
};

/// \brief Options of plUsdPartitionScan::ScanRoot.
struct plUsdScanOptions
{
  /// Applied before the root's payloads load. An override whose prim lacks the set or the variant is skipped.
  plDynamicArray<plUsdVariantOverride> m_Variants;

  /// Computes plUsdPlaceable::m_uiGeometryKey of loose gprims. Prototypes always get keys.
  bool m_bGeometryKeys = true;

  bool m_bLightsAndCameras = true;

  plUsdScanOptions() { m_ExcludePaths.PushBack("/players"); }

  /// Prims skipped with everything below them.
  plDynamicArray<plString> m_ExcludePaths;

  /// ScanRoot refuses a root with more unloaded payloads below it than this (0: no limit), so a root high up in a large
  /// stage does not load all of it. Choose the payload prims as roots instead.
  plUInt32 m_uiMaxRootPayloads = 1;

  /// Set from another thread to stop ScanRoot; it then fails with the root still loaded.
  const plAtomicBool* m_pCancel = nullptr;

  /// Called on the scanning thread: once before loading (0 prims), then every few thousand visited prims.
  plDelegate<void(bool bLoading, plUInt32 uiPrimsVisited)> m_OnProgress;
};

/// \brief What a placeable of a partition scan is.
enum class plUsdPlaceableKind : plUInt8
{
  Instance,      ///< An instanceable prim; draws its prototype.
  PointInstance, ///< One element of a UsdGeomPointInstancer; draws its prototype.
  Gprim,         ///< A Mesh or shape outside any instance.
  Light,
  Camera,
};

/// \brief One thing a partition places into a cell, in Plasma's frame and metres.
struct plUsdPlaceable
{
  plUInt32 m_uiPath = 0;    ///< The prim, see plUsdScanResult::GetPath; for PointInstance the instancer.
  plUInt32 m_uiElement = 0; ///< PointInstance: index into the instancer's arrays.
  plUsdPlaceableKind m_Kind = plUsdPlaceableKind::Gprim;
  plUInt32 m_uiRoot = 0;                   ///< Index into plUsdScanResult::m_Roots.
  plUInt32 m_uiPrototype = plInvalidIndex; ///< Instance and PointInstance: index into plUsdScanResult::m_Prototypes.
  plUInt64 m_uiGeometryKey = 0;            ///< Gprim: equal for gprims composed from the same specs; 0 when not computed.
  plUInt64 m_uiFileStamp = 0;              ///< Gprim with a key: of the files of those specs, their times and sizes.

  /// Of the prim; a light's or camera's without plUsdBasis::GetLookCorrection.
  plTransform m_WorldTransform = plTransform::MakeIdentity();
  plBoundingBox m_WorldBounds = plBoundingBox::MakeInvalid(); ///< Invalid when it draws nothing.
};

/// \brief Where a prototype's geometry can be captured from while a root is loaded.
struct plUsdPrototypeSource
{
  plUInt32 m_uiRoot = 0;
  plString m_sPath; ///< Instance prim (or point instancer prototype) in that root.
};

/// \brief Geometry shared by instances: every gprim of an instance's prototype, or of a point instancer's prototype.
struct plUsdScanPrototype
{
  /// The same in every session and for every root: from the specs of its gprims, their place in it and their materials.
  plUInt64 m_uiKey = 0;
  plUInt64 m_uiFileStamp = 0;                    ///< Of the files of its gprims' specs, their times and sizes, when first met.
  plString m_sName;                              ///< Readable, from the prim the instances reference.
  plDynamicArray<plUsdPrototypeSource> m_Sources; ///< One per root that uses the prototype, in scan order.
  bool m_bPointInstancer = false;
  plBoundingBox m_LocalBounds = plBoundingBox::MakeInvalid(); ///< In the frame of an instance.
  plUInt32 m_uiGprims = 0;
  plUInt64 m_uiPoints = 0;
  plUInt32 m_uiUseCount = 0;

  /// The source in a root; nullptr when the root does not use the prototype.
  const plUsdPrototypeSource* FindSource(plUInt32 uiRoot) const
  {
    for (const plUsdPrototypeSource& source : m_Sources)
    {
      if (source.m_uiRoot == uiRoot)
        return &source;
    }
    return nullptr;
  }
};

/// \brief One scanned root and what its traversal saw.
struct plUsdScanRoot
{
  plString m_sPath;
  plBoundingBox m_WorldBounds = plBoundingBox::MakeInvalid();
  plUInt32 m_uiFirstPlaceable = 0;
  plUInt32 m_uiPlaceableCount = 0;
  plUInt32 m_uiNewPrototypes = 0; ///< Prototypes no earlier root had.
  plUInt32 m_uiPrims = 0;         ///< Visited prims, not counting prototype contents.
  plUInt32 m_uiSkippedPurpose = 0;   ///< Subtrees of guide or proxy purpose.
  plUInt32 m_uiSkippedInvisible = 0; ///< Invisible subtrees.
  plUInt32 m_uiSkippedExcluded = 0; ///< Prims in plUsdScanOptions::m_ExcludePaths.
  plUInt32 m_uiSkippedUnsupported = 0; ///< Points, curves, volumes, empty prototypes and instancers inside prototypes.
  plTime m_LoadTime;
  plTime m_ScanTime;
  plTime m_BoundsTime;    ///< Part of m_ScanTime: world bounds of the placeables.
  plTime m_PrototypeTime; ///< Part of m_ScanTime: collecting and keying prototypes.
  plTime m_KeyTime;       ///< Part of m_ScanTime: geometry keys of loose gprims.
};

/// \brief Everything plUsdPartitionScan::ScanRoot collected, over all roots scanned into it.
struct plUsdScanResult
{
  plDynamicArray<plUsdScanRoot> m_Roots;
  plDynamicArray<plUsdPlaceable> m_Placeables;
  plDynamicArray<plUsdScanPrototype> m_Prototypes;
  plHashTable<plUInt64, plUInt32> m_PrototypeByKey;

  /// Stores a prim path in one shared buffer; returns its index.
  plUInt32 AddPath(plStringView sPath)
  {
    m_PathStarts.PushBack(m_PathText.GetCount());
    m_PathText.PushBackRange(plArrayPtr<const char>(sPath.GetStartPointer(), sPath.GetElementCount()));
    return m_PathStarts.GetCount() - 1;
  }

  plStringView GetPath(plUInt32 uiPath) const
  {
    const plUInt32 uiEnd = uiPath + 1 < m_PathStarts.GetCount() ? m_PathStarts[uiPath + 1] : m_PathText.GetCount();
    return plStringView(m_PathText.GetData() + m_PathStarts[uiPath], m_PathText.GetData() + uiEnd);
  }

  plStringView GetPath(const plUsdPlaceable& placeable) const { return GetPath(placeable.m_uiPath); }

  /// \brief Drops the placeables and paths of the roots scanned so far; prototypes and roots stay, so prototypes are
  /// still shared with later roots. The dropped roots' placeable ranges are empty afterwards.
  void ReleasePlaceables()
  {
    for (plUsdScanRoot& root : m_Roots)
    {
      root.m_uiFirstPlaceable = 0;
      root.m_uiPlaceableCount = 0;
    }
    m_Placeables.Clear();
    m_PathText.Clear();
    m_PathStarts.Clear();
  }

  plDynamicArray<char> m_PathText;
  plDynamicArray<plUInt32> m_PathStarts;
};

/// \brief Geometry captured for building on another thread.
struct plUsdCapturedMesh
{
  plUniquePtr<plUsdGeometrySource> m_pSource; ///< Build groups triangles by m_Materials.
  plDynamicArray<plString> m_Materials;       ///< Bound material per group: a prim path, empty for none.
  plUInt32 m_uiSkipped = 0;                   ///< Gprims left out because their geometry could not be captured.
  plString m_sSkipped;                        ///< The first of them and why.
};

/// \brief One piece of a mesh split along the cells of a partition grid.
struct plUsdCellPiece
{
  plVec2I32 m_Cell;    ///< As plUsdPartitionScan::CellOf.
  plVec3 m_vOrigin;    ///< World position of the piece's geometry origin: its cell's centre at height 0.
  plGeometry m_Geometry;
  plDynamicArray<plUInt32> m_GroupTriangleCounts; ///< Per material group, as plUsdGeometrySource::Build reports them.
};

/// \brief Reads a stage piece by piece to place it into the cells of a world partition.
///
/// The stage opens with no payload loaded. ScanRoot loads one root's payloads, under variant selections made in the
/// session layer, and collects its placeables; ReleaseRoot unloads it again, so memory is bounded by the largest root.
/// Not thread-safe: use it from one thread.
class plUsdPartitionScan
{
  PL_DISALLOW_COPY_AND_ASSIGN(plUsdPartitionScan);

public:
  plUsdPartitionScan();
  ~plUsdPartitionScan();

  plStatus Open(plStringView sRootLayerFile);
  void Close();

  /// \brief The stage, for reading loaded prims (ReadPrim, GetVariantSets, ...).
  plUsdStage& GetStage() { return m_Stage; }
  const plUsdBasis& GetBasis() const { return m_Stage.GetBasis(); }

  /// \brief A child prim, for choosing roots.
  struct ChildInfo
  {
    plString m_sPath;
    plString m_sName;
    plString m_sTypeName;
    bool m_bHasPayload = false;
    bool m_bLoaded = false;
    plDynamicArray<plUsdVariantSet> m_VariantSets;
    plBoundingBox m_WorldBounds = plBoundingBox::MakeInvalid(); ///< Of what is composed now; invalid when unknown.
  };

  /// \brief The children of a prim ("/" for the top level) as the stage is composed now. With bBounds their world bounds
  /// too; subtree bounds are cached until payloads load or unload, so listing level by level walks each prim once. Stops
  /// early, with the children so far, once *pCancel is set.
  void ListChildren(plStringView sPrimPath, plDynamicArray<ChildInfo>& out_children, bool bBounds = true, const plAtomicBool* pCancel = nullptr) const;

  /// \brief Bounds of a payload prim with its low detail variants selected ('proxy', 'low' or 'preview'): loads that
  /// prim alone, measures it and unloads it again. Fails when the prim has no such variant.
  plStatus ComputeProxyBounds(plStringView sPrimPath, plBoundingBox& out_bounds);

  /// \brief The variant an import picks for a set no override names: 'full', 'high' or 'render', in that order; empty
  /// when the set has none of them.
  static plStringView GetPreferredVariant(const plUsdVariantSet& set);

  /// \brief The set's 'proxy', 'low' or 'preview' variant, in that order; empty when it has none.
  static plStringView GetProxyVariant(const plUsdVariantSet& set);

  /// \brief Applies the variant overrides and loads the root's payloads, with everything below it. Payloads of ancestors
  /// that had to load for it are unloaded again by ReleaseRoot once no loaded root lies below them.
  plStatus LoadRoot(plStringView sRootPath, plArrayPtr<const plUsdVariantOverride> variants);

  /// \brief For each variant set of the root that inout_variants does not name for the root, adds its 'full', 'high' or
  /// 'render' variant, in that order, when it has one.
  void AddPreferredVariants(plStringView sRootPath, plDynamicArray<plUsdVariantOverride>& inout_variants);

  /// \brief Unloads a root's payloads and removes the variant selections LoadRoot made for it that no other loaded root
  /// shares.
  void ReleaseRoot(plStringView sRootPath);

  /// \brief LoadRoot, then collects the root's placeables and prototypes into inout_result. The root stays loaded, so
  /// its geometry can be captured, until ReleaseRoot.
  ///
  /// Default and render purposes only, as UsdGeomImageable::ComputePurpose decides them; invisible, inactive and excluded
  /// prims are skipped. Traversal stops at instances, point instancers and gprims. When cancelled, inout_result is left
  /// as before the call and the root stays loaded.
  plStatus ScanRoot(plStringView sRootPath, const plUsdScanOptions& options, plUsdScanResult& inout_result);

  /// \brief Captures a prototype as one mesh from its source in a root, which must be loaded: all its gprims, placed
  /// relative to the instance, one group per material.
  plStatus CapturePrototype(const plUsdScanPrototype& prototype, plUInt32 uiRoot, plUsdCapturedMesh& out_mesh) const;

  /// \brief Captures a loose Mesh or shape; its groups as plUsdStage::CaptureGeometry reports them.
  plStatus CaptureGprim(plStringView sPrimPath, plUsdCapturedMesh& out_mesh) const;

  /// \brief Captures loose gprims as one mesh, each placed by its matrix (Plasma frame), one group per material. Gprims
  /// without usable geometry are skipped and counted in out_mesh; fails when none is left.
  plStatus CaptureGprims(plArrayPtr<const plStringView> primPaths, plArrayPtr<const plMat4> transforms, plUsdCapturedMesh& out_mesh) const;

  /// \brief The bound material of every material group of loose gprims, in order: per prim its own binding, then its
  /// materialBind subsets'; empty for none. Reads no geometry. Unloaded prims add one empty entry.
  void GetGprimMaterials(plArrayPtr<const plStringView> primPaths, plDynamicArray<plString>& out_materials) const;

  /// \brief Splits geometry along the cells of a grid, one piece per cell. Triangles inside one cell go whole; triangles
  /// that span cells are cut at the grid lines. Every cut point is computed from the source edge it lies on, so the
  /// pieces on both sides of a cell border share bit-identical vertices and stay inside their cells.
  ///
  /// The geometry is transformed by mTransform first (normals by plUsdBasis::NormalMatrix, winding flipped for mirrors).
  /// groupTriangleCounts is the Build output of the geometry; pieces keep the group order. Fails, with no pieces, for
  /// non-finite positions or when the geometry's bounds cover more than uiMaxCells cells.
  static plStatus SplitByCell(const plGeometry& geometry, plArrayPtr<const plUInt32> groupTriangleCounts, const plMat4& mTransform, float fCellSize,
    plUInt32 uiMaxCells, plDynamicArray<plUsdCellPiece>& out_pieces);

  /// \brief The cell under a point, as plWorldPartitionGrid::CellOf; coordinates out of range clamp to the outermost cells.
  static plVec2I32 CellOf(const plVec3& vPosition, float fCellSize);

  /// \brief The cell's square on the ground plane, as plWorldPartitionGrid::CellRect.
  static plBoundingBox CellRect(const plVec2I32& cell, float fCellSize);

  /// \brief A cell as one number, as plWorldPartitionGrid::PackKey.
  static plUInt64 PackCellKey(const plVec2I32& cell);

  /// \brief The cell of a PackCellKey number, as plWorldPartitionGrid::UnpackKey.
  static plVec2I32 UnpackCellKey(plUInt64 uiKey);

private:
  struct Impl;

  plUsdStage m_Stage;
  plUniquePtr<Impl> m_pImpl;
};
