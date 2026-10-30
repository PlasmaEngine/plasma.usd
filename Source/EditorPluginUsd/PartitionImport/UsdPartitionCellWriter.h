#pragma once

#include <EditorPluginUsd/PartitionImport/UsdPartitionImportData.h>

class plAbstractObjectGraph;

/// \brief Which layer a file is: a cell of a partition, or an ordinary layer of the scene.
struct plUsdLayerIdentity
{
  plUuid m_Layer;     ///< Document GUID of a new file; an existing file keeps its own.
  plUuid m_Scene;
  plUuid m_Partition; ///< Invalid for layers that are no cell.
  plInt32 m_iX = 0;
  plInt32 m_iY = 0;
};

/// \brief The generated objects of one import root in one layer, under one group object.
struct plUsdLayerGroup
{
  plString m_sKey;  ///< GUID seed of the group object.
  plString m_sName;
  plVec3 m_vPosition = plVec3::MakeZero(); ///< Of the group object: a cell's group sits in its cell, so its pivot does.
  plDynamicArray<const plUsdImportObject*> m_Objects;
};

/// \brief Generated objects made elsewhere (e.g. lights through a document object manager): root game objects and
/// everything below them.
struct plUsdLayerExtraObjects
{
  const plAbstractObjectGraph* m_pGraph = nullptr;
  plDynamicArray<plUuid> m_Roots;
};

struct plUsdLayerWriteResult
{
  plUInt32 m_uiRoots = 0;     ///< Root objects in the written file.
  plUInt32 m_uiUserRoots = 0; ///< Of those, kept from the file because the import did not make them.
  plUInt32 m_uiObjects = 0;   ///< Generated game objects written.
  bool m_bChanged = false;    ///< The file's content changed.
};

/// \brief Writes scene layer files with an import's objects, without opening documents.
///
/// Every generated game object carries the tag UsdImport; its GUID comes from the import id and its key. A file that
/// exists keeps its document GUID, its settings and every object the import did not make: untagged ones, and tagged
/// copies whose GUIDs the import never made. Objects the user put under a generated object move under the new object
/// with the same GUID, or become roots when there is none. The file is written into a staging folder and moved over
/// the target in one step, so readers never see half a file; unchanged content leaves the file alone.
class PL_EDITORPLUGINUSD_DLL plUsdPartitionCellWriter
{
public:
  static constexpr const char* GeneratedTag = "UsdImport";

  /// \brief Replaces the generated objects of a layer file with the groups' objects. Creates the file when there is
  /// none; then with an empty layer's settings and the identity. Runs on any thread.
  ///
  /// pKnownGenerated holds the GUIDs the import made before (plUsdImportObjectIndex); without it every tagged object
  /// counts as generated.
  static plStatus WriteLayer(plStringView sTargetFile, plStringView sStagingFolder, plStringView sImportId, const plUsdLayerIdentity& identity,
    plArrayPtr<const plUsdLayerGroup> groups, const plUsdLayerExtraObjects* pExtra, const plHashSet<plUuid>* pKnownGenerated,
    plUsdLayerWriteResult& out_result);
};
