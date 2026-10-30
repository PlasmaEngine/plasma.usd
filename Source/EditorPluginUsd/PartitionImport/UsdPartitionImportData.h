#pragma once

#include <EditorPluginUsd/EditorPluginUsdDLL.h>
#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Containers/HashSet.h>
#include <Foundation/Containers/HashTable.h>
#include <Foundation/Math/BoundingBox.h>
#include <Foundation/Math/Transform.h>
#include <Foundation/Math/Vec2.h>
#include <Foundation/Types/Status.h>
#include <Foundation/Types/Uuid.h>
#include <Foundation/Types/Variant.h>
#include <UsdBridge/UsdPartitionScan.h>

/// \brief What a world partition import reads from the stage and how it places it into cells.
struct PL_EDITORPLUGINUSD_DLL plUsdPartitionImportOptions
{
  plString m_sStageFile;
  plString m_sPartitionName;
  float m_fCellSize = 128.0f;

  /// Prims each scanned and placed on their own, so memory stays bounded by the largest. Empty: the highest prims with
  /// payloads (a few levels deep), else the top-level prims.
  plDynamicArray<plString> m_Roots;

  /// Variant selections made in the session layer. A set the list does not name gets its 'full', 'high' or 'render'
  /// variant when it has one.
  plDynamicArray<plUsdVariantOverride> m_Variants;

  /// Small instances of one prototype in one cell become one instanced object; small loose gprims of one parent prim in
  /// one cell become one merged mesh. Below this XY extent, in metres; 0 places every prim on its own.
  float m_fBatchSize = 8.0f;

  /// Loose meshes wider than this are cut along the cell borders, one piece per cell. 0: one cell; negative: never.
  float m_fSplitSize = 0.0f;

  bool m_bLightsAndCameras = true;

  /// Only placeables whose bounds centre lies inside this XY rectangle are imported; invalid: the whole stage.
  plBoundingBox m_Region = plBoundingBox::MakeInvalid();

  plDynamicArray<plString> m_ExcludePaths;

  plUsdPartitionImportOptions() { m_ExcludePaths.PushBack("/players"); }

  float GetSplitSize() const { return m_fSplitSize < 0.0f ? plMath::MaxValue<float>() : (m_fSplitSize == 0.0f ? m_fCellSize : m_fSplitSize); }

  /// How deep below the top level roots are searched for payload prims when none are chosen.
  static constexpr plUInt32 MaxRootDepth = 8;

  /// \brief Whether sPath lies strictly below sAncestor.
  static bool IsBelow(plStringView sPath, plStringView sAncestor);

  /// \brief Whether the prim is one of m_ExcludePaths or below one.
  bool IsExcluded(plStringView sPath) const;

  /// \brief Whether a bounds centre lies in m_Region; always true without a region.
  bool IsInRegion(const plVec3& vCentre) const;

  /// \brief The larger of the bounds' X and Y size; 0 for invalid bounds.
  static float GetExtentXY(const plBoundingBox& bounds);

  /// \brief The map width from the bounds of the cell objects' centres, at least four cells. Loose meshes wider than
  /// this go into the Distant layer.
  float GetMapExtent(const plBoundingBox& centres) const { return plMath::Max(GetExtentXY(centres), 4.0f * m_fCellSize); }

  enum class LooseMeshPlacement : plUInt8
  {
    Cell,    ///< Whole, into the cell under its centre (or batched when small).
    Split,   ///< Cut along the cell borders, one piece per cell.
    Distant, ///< Whole, into the Distant layer.
  };

  /// \brief Where a loose mesh of this XY extent goes on a map fMapExtent wide (see GetMapExtent).
  LooseMeshPlacement PlaceLooseMesh(float fExtent, float fMapExtent) const;

  /// \brief The scan options of one root: its variants, geometry keys, lights and cameras, and the excluded paths.
  void MakeScanOptions(plStringView sRoot, plUsdPartitionScan& scan, plUsdScanOptions& out_options) const;

  /// \brief Removes duplicates and roots below another root of the list, keeping the order.
  static void RemoveNestedRoots(plDynamicArray<plString>& ref_roots);

  /// \brief The roots the import scans: m_Roots without nested ones, else the highest payload prims down to
  /// MaxRootDepth, else the top-level prims; excluded prims never.
  void ResolveRoots(const plUsdPartitionScan& scan, plDynamicArray<plString>& out_roots) const;

  /// \brief The variant selections for one root: overrides for it, for every root or for prims below it, then the
  /// preferred variant of each of its sets no override names.
  void ResolveVariants(plStringView sRoot, plUsdPartitionScan& scan, plDynamicArray<plUsdVariantOverride>& out_variants) const;

  /// \brief Of everything that changes what is placed where; a root planned with other options is scanned again.
  plUInt64 GetPlanHash() const;

  /// \brief Writes the options as members of a JSON object (see plUsdJson).
  void WriteJson(plVariantDictionary& out_json) const;

  /// \brief Reads the options as WriteJson writes them; fields that are missing keep their values. Also takes the
  /// action's short forms: "stage", "partition", "cell", "batch", "split", "variants" as an object of set: variant.
  plStatus ReadJson(const plVariantDictionary& json);
};

/// \brief What an imported object is.
enum class plUsdImportObjectKind : plUInt8
{
  Mesh,      ///< One mesh, one plMeshComponent per mesh part.
  Instances, ///< One mesh drawn at m_Instances, one plInstancedMeshComponent per mesh part.
  Light,
  Camera,
};

/// \brief Where an imported object goes.
enum class plUsdImportTarget : plUInt8
{
  Cell,    ///< The cell m_Cell of the partition.
  Distant, ///< The always loaded layer "<Partition> Distant": larger than the map.
  Globals, ///< The always loaded layer "<Partition> Globals": lights and cameras.
};

/// \brief One object a root of an import puts into a layer.
struct PL_EDITORPLUGINUSD_DLL plUsdImportObject
{
  plString m_sKey;      ///< Unique within the import, the same in every import: its GUID comes from it.
  plString m_sName;
  plString m_sPrimPath; ///< The prim it stands for, or the parent prim of a merged or batched group.
  plUsdImportObjectKind m_Kind = plUsdImportObjectKind::Mesh;
  plUsdImportTarget m_Target = plUsdImportTarget::Cell;
  bool m_bOversized = false; ///< An instance wider than the split size, placed by its centre.
  plVec2I32 m_Cell = plVec2I32::MakeZero();
  plTransform m_Transform = plTransform::MakeIdentity(); ///< World.
  plBoundingBox m_Bounds = plBoundingBox::MakeInvalid(); ///< World.
  plString m_sMesh; ///< Resource of the mesh without the part suffix, see plUsdPartitionImportData::GetMeshResource.
  plUInt32 m_uiParts = 0;
  plDynamicArray<plTransform> m_Instances; ///< Relative to m_Transform.
  plUsdLight m_Light;
  plUsdCamera m_Camera;

  plUInt64 ComputeHash(plUInt64 uiSeed) const;
};

/// \brief Something an import could not place as asked; Problems lists them.
struct PL_EDITORPLUGINUSD_DLL plUsdImportProblem
{
  enum class Type : plUInt8
  {
    MeshFailed,    ///< No mesh could be built, or some gprims of a mesh were left out.
    NotSplit,      ///< Too large or broken to cut along the cells: placed whole in the Distant layer.
    Oversized,     ///< An instance wider than the split size, placed by its centre.
    Distant,       ///< Larger than the map: in the Distant layer.
    RootFailed,    ///< The root could not be scanned.
    CellConflict,  ///< A loaded cell with unsaved changes; its file changed under it.
  };

  Type m_Type = Type::MeshFailed;
  plString m_sPath;
  plString m_sMessage;
};

/// \brief What an import put out for one root; kept on disk so resuming and updating skip unchanged roots.
struct PL_EDITORPLUGINUSD_DLL plUsdImportRootPlan
{
  plString m_sRootPath;
  plString m_sRootName;
  plUInt64 m_uiOptionsHash = 0;
  plDynamicArray<plUsdImportObject> m_Objects;
  plHashTable<plString, plInt64> m_Layers;     ///< Layer file -> modification time in microseconds, when it was scanned.
  plDynamicArray<plString> m_Materials;          ///< Material resources its meshes use; a missing file rescans the root.
  plDynamicArray<plUsdImportProblem> m_Problems; ///< Found while scanning and building the root.

  plUInt32 m_uiPlaceables = 0;
  plUInt32 m_uiOutsideRegion = 0;
  plUInt32 m_uiWithoutBounds = 0;

  plStatus Save(plStringView sFile) const;
  plStatus Load(plStringView sFile);
};

/// \brief The state of an import, kept in '<scene>_data/<Partition>/usdimport.json'.
struct PL_EDITORPLUGINUSD_DLL plUsdPartitionImportManifest
{
  struct Root
  {
    plString m_sPath;
    bool m_bDone = false; ///< Its plan (with the layer time stamps) is saved, see plUsdPartitionImportData::GetRootPlanFile.
  };

  struct Cell
  {
    plUInt64 m_uiHash = 0; ///< Of the generated objects written into it.
    plUuid m_Layer;
    plUInt64 m_uiFileSize = 0; ///< Of the file as the import left it; another size or time means it was written since.
    plInt64 m_iFileTime = 0;   ///< Modification time in microseconds.
  };

  plUsdPartitionImportOptions m_Options;
  plUuid m_Partition;
  plString m_sImportId;
  bool m_bComplete = false; ///< Every root and cell was written.
  plDynamicArray<Root> m_Roots;
  plHashTable<plUInt64, Cell> m_Cells; ///< By plUsdPartitionScan::PackCellKey.
  plUInt64 m_uiDistantHash = 0;
  plUInt64 m_uiGlobalsHash = 0;
  plDynamicArray<plUsdImportProblem> m_Problems;

  static plString GetPath(plStringView sPartitionFolder);

  plStatus Save(plStringView sFile) const;
  plStatus Load(plStringView sFile);

  Root* FindRoot(plStringView sPath);
};

/// \brief The game objects an import generated, '<partition folder>/usdimport.objects': one line per object with its
/// GUID, key and prim path. Leads objects back to their prims, and tells the cell writer which tagged objects the import
/// made, so user copies of them are kept.
struct PL_EDITORPLUGINUSD_DLL plUsdImportObjectIndex
{
  struct Entry
  {
    plUuid m_Guid;
    plString m_sKey;
    plString m_sPrimPath;
  };

  plDynamicArray<Entry> m_Entries;

  static plString GetPath(plStringView sPartitionFolder);

  /// \brief Rewrites the file when its content changed.
  plStatus Save(plStringView sFile) const;

  /// \brief The GUIDs of a saved index; fails when there is none.
  static plStatus LoadGuids(plStringView sFile, plHashSet<plUuid>& out_guids);
};

/// \brief The meshes an import built, '<work folder>/meshes.cache', so resuming and updating reuse them. A mesh is
/// reused while its files exist and the files of its geometry's specs have the same times and sizes as when it was built.
struct PL_EDITORPLUGINUSD_DLL plUsdImportMeshCache
{
  struct Entry
  {
    plUInt32 m_uiParts = 0;
    plUInt64 m_uiFileStamp = 0; ///< plUsdPlaceable::m_uiFileStamp or plUsdScanPrototype::m_uiFileStamp it was built from.
    plUInt32 m_uiSkipped = 0;   ///< Gprims left out of it, see plUsdCapturedMesh.
    plString m_sSkipped;
    plDynamicArray<plString> m_MaterialPaths;     ///< Material prim paths of its groups, see plUsdCapturedMesh::m_Materials.
    plDynamicArray<plString> m_MaterialResources; ///< What each of m_MaterialPaths resolved to.
  };

  plHashTable<plString, Entry> m_Meshes; ///< By mesh name, see plUsdPartitionImportData::GetMeshName.

  static plString GetPath(plStringView sImportId);

  plStatus Save(plStringView sFile) const;

  /// \brief Fails, with no entries, when there is no cache or another version wrote it.
  plStatus Load(plStringView sFile);
};

/// \brief Names and folders of what an import writes.
///
/// Meshes and materials go to <project>/AssetCache/Generated/UsdImport/<import id>/, which project export ships; the
/// per-root plans and staging files to <project>/AssetCache/UsdImport/<import id>/, which it does not.
class PL_EDITORPLUGINUSD_DLL plUsdPartitionImportData
{
public:
  /// \brief The import id of a partition: a short hash of its GUID.
  static plString MakeImportId(const plUuid& partition);

  /// \brief ":project/AssetCache/Generated/UsdImport/<id>/Meshes/<name>-<key>" without part suffix and extension.
  static plString GetMeshName(plStringView sImportId, plStringView sName, plUInt64 uiKey);

  /// \brief The resource of one part of a mesh named by GetMeshName.
  static plString GetMeshResource(plStringView sMesh, plUInt32 uiPart);

  /// \brief The absolute folder of the meshes GetMeshName names; empty without a project.
  static plString GetMeshFolder(plStringView sImportId);

  /// \brief The material resource of a UsdShade material prim.
  static plString GetMaterialResource(plStringView sImportId, plStringView sMaterialPath);

  /// \brief The neutral grey material of geometry without a material.
  static plString GetDefaultMaterialResource(plStringView sImportId);

  /// \brief <project>/AssetCache/UsdImport/<id>; empty without a project.
  static plString GetWorkFolder(plStringView sImportId);

  static plString GetRootPlanFile(plStringView sImportId, plStringView sRootPath);

  /// \brief A GUID made from the import id, a key and a salt, the same in every import.
  static plUuid MakeGuid(plStringView sImportId, plStringView sKey, plUInt64 uiSalt = 0);

  /// \brief Keeps letters, digits, '_' and '-' of a prim name for file names.
  static plString MakeFileName(plStringView sName);
};
