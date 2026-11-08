#pragma once

#include <EditorPluginUsd/LiveLink/UsdBakeStore.h>

/// Immutable, material-independent geometry. Components supply their own material bindings.
/// The key includes the geometry values, basis, material partition and conversion version.
class PL_EDITORPLUGINUSD_DLL plUsdMeshCache
{
public:
  static plUInt64 GetKey(plUInt64 uiGeometryHash, plUInt32 uiMaterialSlots);

  static plString GetMeshName(plUInt64 uiKey);

  static plString GetPreviewResource(plStringView sMesh);

  /// Completion markers are published only after every referenced file is fully written.
  static plUsdBakeStore::MeshEntry Read(plStringView sMesh);

  static plResult Publish(plStringView sMesh, plUInt32 uiParts, bool bPreview);

  /// Shared admission budget across links. A single oversized mesh is allowed to make progress.
  static bool CanAdmit(plUInt64 uiPoints);

  /// Leases coalesce simultaneous requests from different live links.
  static bool TryAcquire(plStringView sMesh, plUInt64 uiPoints = 0);

  static void Release(plStringView sMesh);
};
