#pragma once

#include <EditorPluginUsd/EditorPluginUsdDLL.h>
#include <Foundation/Containers/HashTable.h>
#include <Foundation/Types/Uuid.h>
#include <UsdBridge/UsdStage.h>

/// \brief The files a link's bake consists of, and what they were built from.
///
/// Meshes and materials go to <project>/AssetCache/Generated/UsdLive/<link id>/, prepared textures to
/// <project>/AssetCache/Generated/UsdLive/Textures/. Resource ids are the files' ":project/..." paths, and the live link
/// writes each file when it sends the same bytes to the engine, so the editor and a game run from the same files.
/// Project export copies the Generated folder.
///
/// A mesh's files are named by its mesh key (see GetMeshKey), so prims with the same geometry and materials, e.g. the
/// copies of one referenced asset, share one set of files.
///
/// The manifest, bake.json beside a link's files, records which mesh each mesh prim shows, what each mesh's files were
/// built from, the layer files the stage was composed from with their time stamps, and the bake prefab. Meshes whose
/// layer files are unchanged are not built again, and a closed document's bake is out of date when a layer's time stamp
/// no longer matches.
class PL_EDITORPLUGINUSD_DLL plUsdBakeStore
{
public:
  struct PrimEntry
  {
    plString m_sMesh; ///< See GetMeshName.
    plUInt32 m_uiParts = 0;
  };

  struct MeshEntry
  {
    plUInt64 m_uiFileStamp = 0; ///< plUsdGeometryKey::m_uiFileStamp of the geometry the files were built from.
    plUInt32 m_uiParts = 0;
    plString m_sResourceMesh;    ///< Optional content-addressed geometry shared by links and material bindings.
    plString m_sPreviewResource; ///< A validated preview may be checkpointed before final parts exist.
    plDynamicArray<plUInt64> m_PartSizes;
    plUInt64 m_uiPreviewSize = 0;
  };

  struct Manifest
  {
    plHashTable<plString, PrimEntry> m_Prims;  ///< Mesh prim path -> the mesh it shows.
    plHashTable<plString, MeshEntry> m_Meshes; ///< Mesh name -> what its files were built from.
    plHashTable<plString, plInt64> m_Layers;   ///< Layer file -> modification time in microseconds, as of the last bake.
    plUuid m_Prefab;                           ///< The bake prefab's document GUID; invalid before the first bake.
    plUInt64 m_uiContentHash = 0;              ///< Hash of everything the bake prefab was written from; see plUsdBakePrefab::GetContentHash.
  };

  /// \brief <project>/AssetCache/Generated/UsdLive. Empty while no project is open.
  static plString GetRootFolder();

  static plString GetLinkFolder(plStringView sLinkId);

  /// \brief ":project/..." for a file inside the project; other paths unchanged.
  static plString ToResourceId(plStringView sAbsolutePath);
  static plString ToAbsolutePath(plStringView sResourceId);

  /// \brief The material resource of a material group; the engine's default material for groups without a material of
  /// their own. Double-sidedness comes from the geometry, so one USD material can need two engine materials.
  static plString GetMaterialResource(plStringView sLinkId, const plUsdMaterialGroup& group);

  /// \brief Identifies a mesh: the geometry's key and the material resources its parts name. A new binding changes the
  /// names, and so needs a new mesh even when the geometry is the same.
  static plUInt64 GetMeshKey(plStringView sLinkId, const plUsdGeometryKey& geometry, plArrayPtr<const plUsdMaterialGroup> groups);

  /// \brief ":project/..." path of a mesh's files without the part suffix; see GetMeshResource.
  static plString GetMeshName(plStringView sLinkId, const plUsdGeometryKey& geometry, plUInt64 uiMeshKey);

  /// \brief The resource of one part of a mesh named by GetMeshName.
  static plString GetMeshResource(plStringView sMesh, plUInt32 uiPart);

  /// \brief The mesh name of a GetMeshResource path (also absolute): without the part suffix and extension.
  static plStringView GetMeshOfResource(plStringView sResource);

  static bool HasMeshFiles(plStringView sMesh, plUInt32 uiParts);

  /// \brief Drops the meshes no prim of the manifest shows, and deletes every mesh file in the link's folder that is not
  /// one of the remaining meshes', e.g. files of geometry that changed since.
  static void RemoveUnusedMeshes(plStringView sLinkId, Manifest& inout_manifest);

  /// \brief The mesh components' colour. It tints the whole mesh, so it only carries displayColor when the prim has a
  /// single group and that group has no material.
  static plColor GetTint(plArrayPtr<const plUsdMaterialGroup> groups);

  /// \brief Writes a resource's file, unless it already holds exactly this content. Creates the folders.
  /// out_pWritten receives whether the file changed.
  static plResult WriteFile(plStringView sResourceId, plArrayPtr<const plUInt8> content, bool* out_pWritten = nullptr);

  /// \brief WriteFile for an absolute path. With sTempFile the content goes there first and then replaces the file in
  /// one step, so readers never see half a file.
  static plResult WriteAbsoluteFile(plStringView sFile, plArrayPtr<const plUInt8> content, bool* out_pWritten = nullptr, plStringView sTempFile = {});

  static void LoadManifest(plStringView sLinkId, Manifest& out_manifest);
  static plResult SaveManifest(plStringView sLinkId, const Manifest& manifest);

  /// \brief Manifest I/O with an explicit absolute path, independent of the open editor project.
  static void LoadManifestFile(plStringView sFile, Manifest& out_manifest);
  static plResult SaveManifestFile(plStringView sFile, const Manifest& manifest);

  /// \brief Replaces out_layers with the stage's layer files and their modification times in microseconds. Returns false
  /// when one was modified after iSince.
  static bool RecordLayers(const plUsdStage& stage, plHashTable<plString, plInt64>& out_layers, plInt64 iSince = plMath::MaxValue<plInt64>());

  /// \brief Whether every recorded layer file still has its recorded time stamp. False when none are recorded.
  static bool AreLayersCurrent(const plHashTable<plString, plInt64>& layers);
};
