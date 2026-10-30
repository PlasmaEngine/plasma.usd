#pragma once

#include <EditorPluginUsd/EditorPluginUsdDLL.h>
#include <EditorPluginUsd/LiveLink/UsdBakeStore.h>
#include <Foundation/Types/Status.h>
#include <Foundation/Types/Uuid.h>
#include <UsdBridge/UsdStage.h>

class plDocument;

/// \brief One prim of a bake.
struct plUsdBakeNode
{
  plUsdPrim m_Prim;           ///< m_Prim.m_iParent indexes the same array of nodes.
  plUInt32 m_uiMeshParts = 0; ///< For meshes: how many mesh files the prim's geometry is split into.
  plString m_sMesh;           ///< For meshes: the files' name, see plUsdBakeStore::GetMeshName.
};

/// \brief The bake prefab of a link, and the object that references it.
///
/// A link's bake prefab, <project>/UsdBake/<document name>/<stage name>-<link id>.plPrefab, holds ordinary, saved objects
/// that mirror the link's prims: names, transforms, active state, mesh components naming the bake's mesh files (see
/// plUsdBakeStore), instanced mesh components for point instancers, light and camera components, and the Plasma
/// components the prims carry (see plUsdComponent). It is a normal asset, transformed and exported by the normal
/// pipeline, and nothing in it needs this package.
///
/// Under each link root sits one saved object, "USD Bake", tagged UsdBake, whose prefab reference instantiates the bake
/// prefab. The editor-engine plugin hides it while the link's live objects are shown and activates it on export, so
/// exports, the Player and prefab instances without a link use the bake.
class PL_EDITORPLUGINUSD_DLL plUsdBakePrefab
{
public:
  static constexpr const char* BakeTag = "UsdBake";

  static plString GetPrefabPath(plStringView sDocumentPath, plStringView sStageFile, plStringView sLinkId);

  /// \brief Replaces the prefab's objects with the nodes and saves it, creating the prefab when it does not exist yet.
  /// Opens it without a window when it is not open, and closes it again. Main thread.
  static plStatus Write(plStringView sPrefabPath, plStringView sLinkId, plArrayPtr<const plUsdBakeNode> nodes, plUuid& out_prefabGuid);

  /// \brief Hash of what a bake is written from: the nodes, the mesh each mesh prim shows and the content of every
  /// material file. Two bakes with the same hash write the same prefab.
  static plUInt64 GetContentHash(plArrayPtr<const plUsdBakeNode> nodes, const plHashTable<plString, plUInt64>& materialHashes);

  static plUuid GetBakeObjectGuid(plStringView sLinkId);

  /// \brief Adds the USD Bake object under the link root, or points it at the prefab. No undo step; the document is
  /// marked modified when anything changed.
  static void EnsureBakeObject(plDocument* pDocument, const plUuid& rootObject, plStringView sLinkId, const plUuid& prefabGuid);

  /// \brief Whether the object is some link's USD Bake object.
  static bool IsBakeObject(const plDocument* pDocument, const plUuid& object);
};
