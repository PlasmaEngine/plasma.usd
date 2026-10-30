#include <EditorPluginUsd/EditorPluginUsdPCH.h>

#include <EditorPluginScene/Scene/Scene2Document.h>
#include <EditorPluginUsd/PartitionImport/UsdPartitionImportDialog.h>
#include <EditorPluginUsd/PartitionImport/UsdPartitionImportEngineDlg.h>
#include <EditorPluginUsd/PartitionImport/UsdPartitionImporter.h>
#include <EditorPluginUsd/PartitionImport/UsdPartitionStageRead.h>
#include <EditorUi/Runtime.h>

namespace
{
  /// The dialog that shows, or closed and waits to be destroyed.
  plUniquePtr<plUsdPartitionImportEngineDlg> s_pDialog;

  /// Checks a few times a second whether the dialog closed; made with the first dialog, destroyed at Shutdown.
  plUniquePtr<plEditorUiTimerGroup> s_pTimers;
} // namespace

void plUsdPartitionImportDialog::ShowForNewImport(plScene2Document* pScene, plStringView sStageFile)
{
  plUsdPartitionImportOptions options;
  options.m_sStageFile = sStageFile;
  if (!sStageFile.IsEmpty())
    options.m_sPartitionName = sStageFile.GetFileName();

  Open(pScene, options, false);
}

void plUsdPartitionImportDialog::ShowForPartition(plScene2Document* pScene, const plUuid& partition)
{
  plUsdPartitionImportManifest manifest;
  const plStatus res = plUsdPartitionImporter::LoadManifest(pScene, partition, manifest);
  if (res.Failed())
  {
    plLog::Error("USD Import: {}", res.m_sMessage);
    return;
  }

  Open(pScene, manifest.m_Options, true);
}

void plUsdPartitionImportDialog::Open(plScene2Document* pScene, const plUsdPartitionImportOptions& options, bool bExistingPartition)
{
  s_pDialog.Clear();

  if (!plEngineDialog::IsEngineEnabled())
  {
    plLog::Error("USD Import: the import dialog needs the editor UI renderer.");
    return;
  }

  s_pDialog = PL_DEFAULT_NEW(plUsdPartitionImportEngineDlg, pScene, options, bExistingPartition);
  if (!s_pDialog->Show())
  {
    s_pDialog.Clear();
    return;
  }

  if (s_pTimers == nullptr)
  {
    s_pTimers = PL_DEFAULT_NEW(plEditorUiTimerGroup);
    s_pTimers->Start(plTime::MakeFromMilliseconds(250), &plUsdPartitionImportDialog::ReleaseClosedDialog, true);
  }
}

void plUsdPartitionImportDialog::ReleaseClosedDialog()
{
  if (s_pDialog != nullptr && !s_pDialog->IsShown())
    s_pDialog.Clear();
}

void plUsdPartitionImportDialog::Shutdown()
{
  s_pTimers.Clear();
  s_pDialog.Clear();

  plUsdPartitionImportUi::WaitForTasks();
}
