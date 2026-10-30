#include <EditorPluginUsd/EditorPluginUsdPCH.h>

#include <EditorFramework/EditorApp/EditorApp.h>
#include <EditorPluginUsd/Actions/UsdActions.h>
#include <EditorPluginUsd/EditorPluginUsdDLL.h>
#include <EditorPluginUsd/Import/UsdModelImporter.h>
#include <EditorPluginUsd/LiveLink/UsdLiveManager.h>
#include <EditorPluginUsd/LiveLink/UsdObjectPolicy.h>
#include <EditorPluginUsd/PartitionImport/UsdPartitionImportDialog.h>
#include <EditorPluginUsd/PartitionImport/UsdPartitionImporter.h>
#include <EditorPluginUsd/PartitionImport/UsdPartitionPanelExtension.h>
#include <EditorPluginUsd/UI/UsdApps.h>
#include <EditorPluginUsd/UI/UsdLivePanel.h>
#include <EditorPluginUsd/UI/UsdPropertyGrid.h>
#include <EditorPluginUsd/UI/UsdRowDecorator.h>
#include <EditorUi/Widgets.h>
#include <Foundation/Configuration/Plugin.h>
#include <ModelImporter2/ModelImporter.h>
#include <UsdBridge/UsdRuntime.h>
#include <UsdBridge/UsdStage.h>

/// The package's icon folder: the icons under their ids (":/UsdPlugin/Icons/UsdLive.svg" is
/// "EditorIcons/UsdPlugin/Icons/UsdLive.svg"). The plugin binary lives in <package>/Bin/<configuration>.
static plString s_sIconFolder;

static void RegisterIconFolder()
{
  plStringBuilder sPlugin, sCopy;
  plPlugin::GetPluginPaths("plEditorPluginUsd", sPlugin, sCopy, 0);

  plStringBuilder sFolder = sPlugin.GetFileDirectory();
  sFolder.AppendPath("..", "..", "EditorIcons");
  sFolder.MakeCleanPath();

  s_sIconFolder = sFolder;
  plEditorUi::AddIconFolder(s_sIconFolder);
}

static void UnregisterIconFolder()
{
  plEditorUi::RemoveIconFolder(s_sIconFolder);
  s_sIconFolder.Clear();
}

static plUsdLiveManager* s_pManager = nullptr;
static plUsdApps* s_pApps = nullptr;
static plUsdPartitionImporter* s_pPartitionImporter = nullptr;
static plUsdPartitionPanelExtension* s_pPartitionPanelExtension = nullptr;
static plUsdObjectPolicy* s_pObjectPolicy = nullptr;
static plUsdRowDecorator* s_pRowDecorator = nullptr;
static plUsdPropertyRowDecorator* s_pPropertyDecorator = nullptr;

static constexpr const char* ScenegraphContextMenu = "EditorPluginScene_ScenegraphContextMenu";

static void OnLoadPlugin()
{
  plDynamicArray<plString> searched;
  const plStatus res = plUsdRuntime::Initialize(&searched);
  if (res.Failed())
  {
    plStringBuilder sTried;
    for (const plString& sPath : searched)
      sTried.AppendFormat("\n  {}", sPath);

    plLog::Error("USD Live is unavailable: {} Looked in:{}", res.m_sMessage, sTried);
    return;
  }

  plLog::Info("USD Live: OpenUSD {} from '{}'", plUsdStage::GetOpenUsdVersion(), plUsdRuntime::GetOpenUsdDirectory());

  // Registered in headless processes too: they transform the mesh, skeleton and animation assets it creates.
  {
    plModelImporter2::ImporterRegistration importer;
    for (plStringView sExtension : plUsdStage::GetFileExtensions())
      importer.m_Extensions.PushBack(sExtension);
    importer.m_sDisplayName = "OpenUSD";
    importer.m_sProviderId = plUsdModelImporter::s_sProviderId;
    importer.m_bSupportsAnimations = true;
    importer.m_Factory = []() -> plUniquePtr<plModelImporter2::Importer> { return PL_DEFAULT_NEW(plUsdModelImporter); };
    plModelImporter2::RegisterImporter(std::move(importer));
  }

  // Headless editor processes (asset transforms) must not bring stages in: what a shipped scene contains comes from
  // Bake, never from a live link.
  if (plEditorApp::GetSingleton()->IsInHeadlessMode())
    return;

  RegisterIconFolder();

  s_pManager = PL_DEFAULT_NEW(plUsdLiveManager);
  s_pApps = PL_DEFAULT_NEW(plUsdApps);
  s_pPartitionImporter = PL_DEFAULT_NEW(plUsdPartitionImporter);

  // '+ New > World partition from USD...', partition details, problems and progress in the World Streaming panel.
  s_pPartitionPanelExtension = PL_DEFAULT_NEW(plUsdPartitionPanelExtension);

  plUsdActions::RegisterActions();
  plUsdActions::MapMenuActions("EditorPluginScene_DocumentMenuBar");
  plUsdActions::MapMenuActions("EditorPluginScene_Scene2MenuBar");
  plUsdActions::MapContextMenuActions(ScenegraphContextMenu);

  // Generated objects are deactivated instead of deleted, and are shown with their USD state in the scene tree and
  // the property grid.
  s_pObjectPolicy = PL_DEFAULT_NEW(plUsdObjectPolicy);
  plDocumentObjectPolicy::Register(s_pObjectPolicy);

  s_pRowDecorator = PL_DEFAULT_NEW(plUsdRowDecorator);
  plObjectRowDecoratorRegistry::Register(s_pRowDecorator);

  s_pPropertyDecorator = PL_DEFAULT_NEW(plUsdPropertyRowDecorator);
  plPropertyGridRowDecorator::Register(s_pPropertyDecorator);
  plUsdPropertyGridHeaders::Register();

  // The panel registers with the engine shell's application panels, which lists it in the Panels menu; it starts closed.
  PL_DEFAULT_NEW(plUsdLivePanel);
}

static void OnUnloadPlugin()
{
  // The registry holds a factory that points into this DLL.
  plModelImporter2::UnregisterImporterProvider(plUsdModelImporter::s_sProviderId);

  // Unregistering the panel destroys its content in the shell.
  if (plUsdLivePanel* pPanel = plUsdLivePanel::GetSingleton())
  {
    PL_DEFAULT_DELETE(pPanel);
  }

  if (s_pManager != nullptr)
  {
    plUsdPropertyGridHeaders::Unregister();
    plPropertyGridRowDecorator::Unregister(s_pPropertyDecorator);
    PL_DEFAULT_DELETE(s_pPropertyDecorator);

    plObjectRowDecoratorRegistry::Unregister(s_pRowDecorator);
    PL_DEFAULT_DELETE(s_pRowDecorator);

    plDocumentObjectPolicy::Unregister(s_pObjectPolicy);
    PL_DEFAULT_DELETE(s_pObjectPolicy);

    plUsdActions::UnmapContextMenuActions(ScenegraphContextMenu);
    plUsdActions::UnregisterActions();
    plUsdPartitionImportDialog::Shutdown();
    PL_DEFAULT_DELETE(s_pPartitionPanelExtension);
    PL_DEFAULT_DELETE(s_pPartitionImporter);
    PL_DEFAULT_DELETE(s_pApps);
    PL_DEFAULT_DELETE(s_pManager);

    UnregisterIconFolder();
  }
}

PL_PLUGIN_ON_LOADED()
{
  OnLoadPlugin();
}

PL_PLUGIN_ON_UNLOADED()
{
  OnUnloadPlugin();
}
