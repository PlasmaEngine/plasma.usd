#pragma once

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Strings/String.h>
#include <Foundation/Types/Status.h>

/// \brief Process-wide OpenUSD setup.
class plUsdRuntime
{
public:
  /// \brief Registers the plugInfo trees shipped beside usd_ms.dll, defines the .mtlx file format (plUsdMtlx) and checks
  /// that the core schemas and file formats resolve.
  ///
  /// OpenUSD finds "usd/" next to its own DLL by itself, but not the package's "plugin/usd/" folder, and a plugin
  /// that is load-copied into loaded/<n>/ cannot rely on its own location either. So the directory is taken from
  /// the module usd_ms.dll was actually mapped from. Safe to call more than once; out_pSearchedPaths lists every
  /// folder looked at, for the error message.
  static plStatus Initialize(plDynamicArray<plString>* out_pSearchedPaths = nullptr);

  /// \brief The folder usd_ms.dll was loaded from, or empty if it is not loaded.
  static plString GetOpenUsdDirectory();

  /// \brief Whether OpenUSD's schema registry knows an API schema, e.g. the package's own PlasmaComponentAPI.
  static bool IsApiSchemaRegistered(plStringView sSchema);

  /// \brief Layers whose identifier contains sPattern are read completely into memory when they open ("detached"),
  /// so other tools can save over their files while a stage uses them. Affects layers opened afterwards.
  ///
  /// Every session layer (a ".live/" folder) is detached, and plUsdStage::Open adds the root layer it opens. Other
  /// layers, e.g. the files an asset references, stay memory-mapped: opening a large stage then reads only what is used,
  /// but on Windows those files cannot be replaced while a stage that uses them is open.
  static void DetachLayers(plStringView sPattern);
};
