#pragma once

#include <EditorPluginUsd/PartitionImport/UsdPartitionImportData.h>

class plScene2Document;

/// \brief Opens World Partition from USD (plUsdPartitionImportEngineDlg): the options of a USD world partition import,
/// which starts the import with plUsdPartitionImporter.
///
/// Not modal, one at a time: opening it again closes the one that is open. A dialog that closed is destroyed shortly
/// after.
class PL_EDITORPLUGINUSD_DLL plUsdPartitionImportDialog
{
public:
  /// \brief Opens the dialog for a new partition of the scene, with the stage filled in when sStageFile is not empty.
  static void ShowForNewImport(plScene2Document* pScene, plStringView sStageFile);

  /// \brief Opens the dialog with the options of the partition's import, to change them and import again.
  static void ShowForPartition(plScene2Document* pScene, const plUuid& partition);

  /// \brief Closes the dialog and waits for its worker; called before the plugin unloads.
  static void Shutdown();

private:
  /// Opens the dialog with the options; closes the open one first.
  static void Open(plScene2Document* pScene, const plUsdPartitionImportOptions& options, bool bExistingPartition);

  /// Destroys the dialog once it no longer shows.
  static void ReleaseClosedDialog();
};
