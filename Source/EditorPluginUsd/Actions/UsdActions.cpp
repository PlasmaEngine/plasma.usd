#include <EditorPluginUsd/EditorPluginUsdPCH.h>

#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorFramework/Document/GameObjectDocument.h>
#include <EditorFramework/EditorApp/EditorApp.h>
#include <EditorPluginUsd/Actions/UsdActions.h>
#include <EditorPluginUsd/LiveLink/UsdLiveManager.h>
#include <EditorPluginUsd/PartitionImport/UsdPartitionImportDialog.h>
#include <EditorPluginUsd/PartitionImport/UsdPartitionImporter.h>
#include <EditorPluginUsd/PartitionImport/UsdPartitionPanelExtension.h>
#include <EditorPluginUsd/UI/UsdApps.h>
#include <EditorPluginUsd/UI/UsdLivePanel.h>
#include <EditorUi/Platform/EditorPlatform.h>
#include <Foundation/Strings/TranslationLookup.h>
#include <GuiFoundation/Action/ActionManager.h>
#include <GuiFoundation/Action/ActionMapManager.h>
#include <GuiFoundation/UIServices/UIServices.h>
#include <ToolsFoundation/Document/Document.h>
#include <ToolsFoundation/Selection/SelectionManager.h>

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plUsdAction, 0, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;

plActionDescriptorHandle plUsdActions::s_hCategory;
plActionDescriptorHandle plUsdActions::s_hLinkStage;
plActionDescriptorHandle plUsdActions::s_hContextCategory;
plActionDescriptorHandle plUsdActions::s_hRevertPrim;
plActionDescriptorHandle plUsdActions::s_hDeactivatePrim;
plActionDescriptorHandle plUsdActions::s_hReactivatePrim;
plActionDescriptorHandle plUsdActions::s_hCopyPrimPath;
plActionDescriptorHandle plUsdActions::s_hStartSession;
plActionDescriptorHandle plUsdActions::s_hReload;
plActionDescriptorHandle plUsdActions::s_hSaveLayer;
plActionDescriptorHandle plUsdActions::s_hUnlink;
plActionDescriptorHandle plUsdActions::s_hShowPanel;
plActionDescriptorHandle plUsdActions::s_hOpenInBlender;
plActionDescriptorHandle plUsdActions::s_hBakeNow;
plActionDescriptorHandle plUsdActions::s_hOpenUsdPrefab;
plActionDescriptorHandle plUsdActions::s_hAppsInstall;
plActionDescriptorHandle plUsdActions::s_hAppsRemove;
plActionDescriptorHandle plUsdActions::s_hAppsOpen;
plActionDescriptorHandle plUsdActions::s_hImportPartition;

namespace
{
  constexpr const char* ContextCategory = "UsdContext";

  struct ActionText
  {
    const char* m_szName;
    const char* m_szText;
    const char* m_szTooltip;
  };

  constexpr ActionText s_ActionTexts[] = {
    {"Usd.LinkStage", "Link USD Stage...", "Link a USD stage into this scene. Its prims appear under a new object."},
    {"Usd.RevertPrim", "Revert Plasma Opinions", "Remove your layer's opinions on the selected prims, so the other layers' values show again."},
    {"Usd.DeactivatePrim", "Deactivate Prim", "Deactivate the selected prims in your layer. Every tool in the session stops showing them."},
    {"Usd.ReactivatePrim", "Reactivate Prim", "Remove your layer's deactivation of the selected prims."},
    {"Usd.CopyPrimPath", "Copy Prim Path", "Copy the prim path of the selected object."},
    {"Usd.StartSession", "Start or Join Session...", "Start or join a live session, so your edits are written into a layer of your own."},
    {"Usd.Reload", "Reload Stage", "Reload every layer of the stage and resend meshes and materials."},
    {"Usd.SaveLayer", "Save Layer", "Save your session layer now."},
    {"Usd.Unlink", "Unlink Stage...", "Unlink the stage and remove the objects generated from it. Nothing is written to USD."},
    {"Usd.ShowPanel", "Show USD Live Panel", "Show the panel with this link's layers and participants."},
    {"Usd.OpenInBlender", "Open in Blender", "Open the stage in Blender, in a live session. A session is started first when there is none."},
    {"Usd.BakeNow", "Bake Now", "Rebuild every mesh and material of the stage and write its bake prefab, which exports and games use."},
    {"Usd.OpenUsdPrefab", "Open USD Prefab", "Open the prefab this object was placed from. It streams its USD stage live."},
    {"Usd.Apps.Install", "Install Connector", "Install or update a tool's USD Live connector. The value names the tool: Blender, Houdini or Maya."},
    {"Usd.Apps.Remove", "Remove Connector", "Remove the USD Live connector the Apps tab installed into a tool. The value names the tool."},
    {"Usd.Apps.Open", "Open in App", "Open the USD Live panel's stage in a tool, in a live session. The value names the tool."},
    {"UsdImport.ImportPartition", "World Partition from USD", "Import a USD stage as the cells of a world partition. The value holds the import options as JSON; without it the import dialog opens."},

    // The title of the USD Live application panel (plUsdLivePanel::PanelId).
    {"Panel.UsdLive", "USD Live", "The layers, participants and tools of the USD link that owns the selection."},

    // Import method of plUsdPrefabDocumentGenerator.
    {"UsdPrefabImport", "USD Prefab (live)", "A prefab whose content is a live link to the USD file: opening it streams the stage, which can be edited with Blender. Placed instances and games use its bake."},
  };

  /// The action names read as plain menu entries rather than as "Usd . Link Stage". A plain storage translator is
  /// used, not a subclass: translators cannot be removed again, and this one must outlive the package's code.
  void RegisterActionTexts()
  {
    plUniquePtr<plTranslatorStorage> pTexts = PL_DEFAULT_NEW(plTranslatorStorage);
    for (const ActionText& text : s_ActionTexts)
    {
      pTexts->StoreTranslation(text.m_szText, plHashingUtils::StringHash(text.m_szName), plTranslationUsage::Default);
      pTexts->StoreTranslation(text.m_szTooltip, plHashingUtils::StringHash(text.m_szName), plTranslationUsage::Tooltip);
    }

    plTranslationLookup::AddTranslator(std::move(pTexts));
  }

  /// The link the selection belongs to when every selected object is one of its prims, else nullptr.
  plUsdLiveLink* GetSelectedPrimsLink(const plDocument* pDocument)
  {
    plUsdLiveManager* pManager = plUsdLiveManager::GetSingleton();
    if (pManager == nullptr || pDocument == nullptr)
      return nullptr;

    const auto& selection = pDocument->GetSelectionManager()->GetSelection();
    if (selection.IsEmpty())
      return nullptr;

    plUsdLiveLink* pResult = nullptr;
    for (const plDocumentObject* pObject : selection)
    {
      plUsdLiveLink* pLink = pManager->FindLinkForObject(pDocument, pObject->GetGuid());
      if (pLink == nullptr || !pLink->IsGeneratedObject(pObject->GetGuid()) || (pResult != nullptr && pResult != pLink))
        return nullptr;

      pResult = pLink;
    }

    return pResult;
  }

  /// The link whose root object is the only selected object, else nullptr.
  plUsdLiveLink* GetSelectedRootLink(const plDocument* pDocument)
  {
    plUsdLiveManager* pManager = plUsdLiveManager::GetSingleton();
    if (pManager == nullptr || pDocument == nullptr)
      return nullptr;

    const auto& selection = pDocument->GetSelectionManager()->GetSelection();
    if (selection.GetCount() != 1)
      return nullptr;

    plUsdLiveLink* pLink = pManager->FindLinkForObject(pDocument, selection[0]->GetGuid());
    return pLink != nullptr && pLink->GetRootObject() == selection[0]->GetGuid() ? pLink : nullptr;
  }

  void GetSelectedGuids(const plDocument* pDocument, plDynamicArray<plUuid>& out_objects)
  {
    for (const plDocumentObject* pObject : pDocument->GetSelectionManager()->GetSelection())
      out_objects.PushBack(pObject->GetGuid());
  }

  /// The link the selection belongs to: its root alone, or prims of it.
  plUsdLiveLink* GetSelectedLink(const plDocument* pDocument)
  {
    if (plUsdLiveLink* pLink = GetSelectedRootLink(pDocument))
      return pLink;

    return GetSelectedPrimsLink(pDocument);
  }

  /// The prefab a single selected object was placed from, when that prefab's content is a USD link: its absolute path,
  /// else empty. Objects inside the instance count too.
  plString GetSelectedUsdPrefab(const plDocument* pDocument)
  {
    const plGameObjectDocument* pGameObjectDocument = plDynamicCast<const plGameObjectDocument*>(pDocument);
    if (pGameObjectDocument == nullptr)
      return {};

    const auto& selection = pDocument->GetSelectionManager()->GetSelection();
    if (selection.GetCount() != 1)
      return {};

    for (const plDocumentObject* pObject = selection[0]; pObject != nullptr; pObject = pObject->GetParent())
    {
      const plUuid prefab = pGameObjectDocument->m_DocumentObjectMetaData->BeginReadMetaData(pObject->GetGuid())->m_CreateFromPrefab;
      pGameObjectDocument->m_DocumentObjectMetaData->EndReadMetaData();

      if (!prefab.IsValid())
        continue;

      auto pAsset = plAssetCurator::GetSingleton()->GetSubAsset(prefab);
      if (!pAsset)
        return {};

      const plString sPath = pAsset->m_pAssetInfo->m_Path.GetAbsolutePath();

      plHybridArray<plUsdLiveManager::SidecarLink, 1> links;
      plUsdLiveManager::ReadSidecar(sPath, links);
      return links.IsEmpty() ? plString() : sPath;
    }

    return {};
  }

  plDocument* OpenWithWindow(plStringView sPath)
  {
    return plEditorApp::GetSingleton()->OpenDocument(sPath, plDocumentFlags::RequestWindow | plDocumentFlags::AddToRecentFilesList);
  }
} // namespace

void plUsdActions::RegisterActions()
{
  RegisterActionTexts();

  s_hCategory = PL_REGISTER_CATEGORY("Usd");
  s_hLinkStage = PL_REGISTER_ACTION_1("Usd.LinkStage", plActionScope::Document, "USD", "", plUsdAction, plUsdAction::ActionType::LinkStage);

  s_hContextCategory = PL_REGISTER_CATEGORY(ContextCategory);
  s_hRevertPrim = PL_REGISTER_ACTION_1("Usd.RevertPrim", plActionScope::Document, "USD", "", plUsdAction, plUsdAction::ActionType::RevertPrim);
  s_hDeactivatePrim = PL_REGISTER_ACTION_1("Usd.DeactivatePrim", plActionScope::Document, "USD", "", plUsdAction, plUsdAction::ActionType::DeactivatePrim);
  s_hReactivatePrim = PL_REGISTER_ACTION_1("Usd.ReactivatePrim", plActionScope::Document, "USD", "", plUsdAction, plUsdAction::ActionType::ReactivatePrim);
  s_hCopyPrimPath = PL_REGISTER_ACTION_1("Usd.CopyPrimPath", plActionScope::Document, "USD", "", plUsdAction, plUsdAction::ActionType::CopyPrimPath);
  s_hStartSession = PL_REGISTER_ACTION_1("Usd.StartSession", plActionScope::Document, "USD", "", plUsdAction, plUsdAction::ActionType::StartSession);
  s_hReload = PL_REGISTER_ACTION_1("Usd.Reload", plActionScope::Document, "USD", "", plUsdAction, plUsdAction::ActionType::Reload);
  s_hSaveLayer = PL_REGISTER_ACTION_1("Usd.SaveLayer", plActionScope::Document, "USD", "", plUsdAction, plUsdAction::ActionType::SaveLayer);
  s_hUnlink = PL_REGISTER_ACTION_1("Usd.Unlink", plActionScope::Document, "USD", "", plUsdAction, plUsdAction::ActionType::Unlink);
  s_hShowPanel = PL_REGISTER_ACTION_1("Usd.ShowPanel", plActionScope::Document, "USD", "", plUsdAction, plUsdAction::ActionType::ShowPanel);
  s_hOpenInBlender = PL_REGISTER_ACTION_1("Usd.OpenInBlender", plActionScope::Document, "USD", "", plUsdAction, plUsdAction::ActionType::OpenInBlender);
  s_hBakeNow = PL_REGISTER_ACTION_1("Usd.BakeNow", plActionScope::Document, "USD", "", plUsdAction, plUsdAction::ActionType::BakeNow);
  s_hOpenUsdPrefab = PL_REGISTER_ACTION_1("Usd.OpenUsdPrefab", plActionScope::Document, "USD", "", plUsdAction, plUsdAction::ActionType::OpenUsdPrefab);

  s_hAppsInstall = PL_REGISTER_ACTION_1("Usd.Apps.Install", plActionScope::Global, "USD", "", plUsdAction, plUsdAction::ActionType::AppsInstall);
  s_hAppsRemove = PL_REGISTER_ACTION_1("Usd.Apps.Remove", plActionScope::Global, "USD", "", plUsdAction, plUsdAction::ActionType::AppsRemove);
  s_hAppsOpen = PL_REGISTER_ACTION_1("Usd.Apps.Open", plActionScope::Global, "USD", "", plUsdAction, plUsdAction::ActionType::AppsOpen);
  s_hImportPartition = PL_REGISTER_ACTION_1("UsdImport.ImportPartition", plActionScope::Global, "USD", "", plUsdAction, plUsdAction::ActionType::ImportPartition);
}

void plUsdActions::UnregisterActions()
{
  for (plActionDescriptorHandle* pHandle : {&s_hCategory, &s_hLinkStage, &s_hContextCategory, &s_hRevertPrim, &s_hDeactivatePrim, &s_hReactivatePrim,
         &s_hCopyPrimPath, &s_hStartSession, &s_hReload, &s_hSaveLayer, &s_hUnlink, &s_hShowPanel, &s_hOpenInBlender, &s_hBakeNow, &s_hOpenUsdPrefab,
         &s_hAppsInstall, &s_hAppsRemove, &s_hAppsOpen, &s_hImportPartition})
  {
    plActionManager::UnregisterAction(*pHandle);
  }
}

void plUsdActions::MapMenuActions(plStringView sMapping)
{
  plActionMap* pMap = plActionMapManager::GetActionMap(sMapping);
  if (pMap == nullptr)
    return;

  pMap->MapAction(s_hCategory, "G.Scene", 6.0f);
  pMap->MapAction(s_hLinkStage, "G.Scene", "Usd", 0.0f);
}

void plUsdActions::MapContextMenuActions(plStringView sMapping)
{
  plActionMap* pMap = plActionMapManager::GetActionMap(sMapping);
  if (pMap == nullptr)
    return;

  // Above the selection and edit entries: for USD objects these are the ones that matter.
  pMap->MapAction(s_hContextCategory, "", 0.5f);
  pMap->MapAction(s_hOpenUsdPrefab, ContextCategory, 0.1f);
  pMap->MapAction(s_hOpenInBlender, ContextCategory, 0.2f);
  pMap->MapAction(s_hBakeNow, ContextCategory, 0.3f);
  pMap->MapAction(s_hRevertPrim, ContextCategory, 1.0f);
  pMap->MapAction(s_hDeactivatePrim, ContextCategory, 2.0f);
  pMap->MapAction(s_hReactivatePrim, ContextCategory, 3.0f);
  pMap->MapAction(s_hCopyPrimPath, ContextCategory, 4.0f);
  pMap->MapAction(s_hStartSession, ContextCategory, 5.0f);
  pMap->MapAction(s_hReload, ContextCategory, 6.0f);
  pMap->MapAction(s_hSaveLayer, ContextCategory, 7.0f);
  pMap->MapAction(s_hShowPanel, ContextCategory, 8.0f);
  pMap->MapAction(s_hUnlink, ContextCategory, 9.0f);
}

void plUsdActions::UnmapContextMenuActions(plStringView sMapping)
{
  plActionMap* pMap = plActionMapManager::GetActionMap(sMapping);
  if (pMap == nullptr)
    return;

  for (plActionDescriptorHandle hAction : {s_hOpenUsdPrefab, s_hOpenInBlender, s_hBakeNow, s_hRevertPrim, s_hDeactivatePrim, s_hReactivatePrim, s_hCopyPrimPath,
         s_hStartSession, s_hReload, s_hSaveLayer, s_hShowPanel, s_hUnlink})
  {
    pMap->UnmapAction(hAction, ContextCategory).IgnoreResult();
  }

  pMap->UnmapAction(s_hContextCategory, "").IgnoreResult();
}

plUsdAction::plUsdAction(const plActionContext& context, const char* szName, ActionType type)
  : plButtonAction(context, szName, false, "")
  , m_Type(type)
{
  switch (m_Type)
  {
    case ActionType::LinkStage:
    case ActionType::ShowPanel:
      SetIconPath(":/UsdPlugin/Icons/UsdLive.svg");
      break;
    case ActionType::RevertPrim:
      SetIconPath(":/EditorPluginScene/Icons/PrefabRevert.svg");
      break;
    case ActionType::StartSession:
      SetIconPath(":/UsdPlugin/Icons/UsdSession.svg");
      break;
    case ActionType::Reload:
      SetIconPath(":/GuiFoundation/Icons/ReloadResources.svg");
      break;
    case ActionType::SaveLayer:
      SetIconPath(":/GuiFoundation/Icons/Save.svg");
      break;
    case ActionType::Unlink:
      SetIconPath(":/EditorPluginScene/Icons/PrefabUnlink.svg");
      break;
    case ActionType::OpenInBlender:
      SetIconPath(":/UsdPlugin/Icons/UsdOpenInBlender.svg");
      break;
    case ActionType::BakeNow:
      SetIconPath(":/UsdPlugin/Icons/UsdBake.svg");
      break;
    case ActionType::OpenUsdPrefab:
      SetIconPath(":/UsdPlugin/Icons/UsdLive.svg");
      break;
    default:
      break;
  }

  if (m_Type != ActionType::LinkStage && m_Context.m_pDocument != nullptr)
  {
    m_SelectionSubscription = m_Context.m_pDocument->GetSelectionManager()->m_Events.AddEventHandler(plMakeDelegate(&plUsdAction::SelectionEventHandler, this));
    UpdateState();
  }
}

plUsdAction::~plUsdAction()
{
  if (m_SelectionSubscription != 0)
  {
    m_Context.m_pDocument->GetSelectionManager()->m_Events.RemoveEventHandler(m_SelectionSubscription);
  }
}

void plUsdAction::SelectionEventHandler(const plSelectionManagerEvent& e)
{
  UpdateState();
}

void plUsdAction::UpdateState()
{
  const plDocument* pDocument = m_Context.m_pDocument;

  switch (m_Type)
  {
    case ActionType::RevertPrim:
    case ActionType::DeactivatePrim:
    case ActionType::ReactivatePrim:
    case ActionType::CopyPrimPath:
    {
      plUsdLiveLink* pLink = GetSelectedPrimsLink(pDocument);
      SetVisible(pLink != nullptr);
      if (pLink == nullptr)
        return;

      if (m_Type == ActionType::CopyPrimPath)
      {
        const auto& selection = pDocument->GetSelectionManager()->GetSelection();
        SetEnabled(selection.GetCount() == 1);
        return;
      }

      // Deactivate is offered while any selected prim is active, Reactivate while any is inactive; Revert while this
      // editor's layer has something to remove.
      bool bAnyActive = false, bAnyInactive = false, bAnyOpinion = false;
      for (const plDocumentObject* pObject : pDocument->GetSelectionManager()->GetSelection())
      {
        plUsdLivePrimInfo info;
        if (pLink->GetPrimInfo(pObject->GetGuid(), info))
        {
          bAnyActive |= info.m_bActive;
          bAnyInactive |= !info.m_bActive;
          bAnyOpinion |= info.m_Opinions.HasEditOpinion();
        }
      }

      if (m_Type == ActionType::RevertPrim)
        SetEnabled(pLink->IsWritable() && bAnyOpinion);
      else if (m_Type == ActionType::DeactivatePrim)
      {
        SetVisible(bAnyActive || !bAnyInactive);
        SetEnabled(pLink->IsWritable() && bAnyActive);
      }
      else
      {
        SetVisible(bAnyInactive);
        SetEnabled(pLink->IsWritable() && bAnyInactive);
      }
      return;
    }

    case ActionType::StartSession:
    case ActionType::Reload:
    case ActionType::SaveLayer:
    case ActionType::Unlink:
    case ActionType::ShowPanel:
    {
      plUsdLiveLink* pLink = GetSelectedRootLink(pDocument);
      SetVisible(pLink != nullptr);
      if (pLink == nullptr)
        return;

      if (m_Type == ActionType::StartSession)
        SetEnabled(!pLink->IsWritable());
      else if (m_Type == ActionType::SaveLayer)
        SetEnabled(pLink->HasUnsavedChanges());
      else
        SetEnabled(true);
      return;
    }

    case ActionType::OpenInBlender:
    {
      const bool bLink = GetSelectedLink(pDocument) != nullptr;
      SetVisible(bLink || !GetSelectedUsdPrefab(pDocument).IsEmpty());
      SetEnabled(true);
      return;
    }

    case ActionType::BakeNow:
      SetVisible(GetSelectedLink(pDocument) != nullptr);
      SetEnabled(true);
      return;

    case ActionType::OpenUsdPrefab:
      SetVisible(!GetSelectedUsdPrefab(pDocument).IsEmpty());
      SetEnabled(true);
      return;

    default:
      return;
  }
}

void plUsdAction::Execute(const plVariant& value)
{
  plDocument* pDocument = GetContext().m_pDocument;

  auto report = [](const plStatus& res, const char* szFailure)
  {
    if (res.Failed())
      plEditorUiServices::MessageBoxStatus(res, szFailure);
  };

  switch (m_Type)
  {
    case ActionType::LinkStage:
      plUsdLivePanel::LinkStageInteractive(pDocument);
      if (plUsdLivePanel* pPanel = plUsdLivePanel::GetSingleton())
        pPanel->EnsureVisible();
      return;

    case ActionType::RevertPrim:
    case ActionType::DeactivatePrim:
    case ActionType::ReactivatePrim:
    {
      plUsdLiveLink* pLink = GetSelectedPrimsLink(pDocument);
      if (pLink == nullptr)
        return;

      plHybridArray<plUuid, 32> objects;
      GetSelectedGuids(pDocument, objects);

      if (m_Type == ActionType::RevertPrim)
        report(pLink->Revert(objects), "Reverting failed.");
      else
        report(pLink->SetPrimsActive(objects, m_Type == ActionType::ReactivatePrim), "Changing whether the prims are active failed.");

      UpdateState();
      return;
    }

    case ActionType::CopyPrimPath:
    {
      const auto& selection = pDocument->GetSelectionManager()->GetSelection();
      plUsdLiveLink* pLink = GetSelectedPrimsLink(pDocument);
      if (pLink != nullptr && selection.GetCount() == 1)
        plEditorPlatform::SetClipboardText(pLink->GetPrimPath(selection[0]->GetGuid()));
      return;
    }

    case ActionType::StartSession:
      if (plUsdLiveLink* pLink = GetSelectedRootLink(pDocument))
        plUsdLivePanel::StartSessionInteractive(pLink);
      return;

    case ActionType::Reload:
      if (plUsdLiveLink* pLink = GetSelectedRootLink(pDocument))
        pLink->RequestFullResync();
      return;

    case ActionType::SaveLayer:
      if (plUsdLiveLink* pLink = GetSelectedRootLink(pDocument))
        report(pLink->SaveNow(), "Saving your layer failed.");
      return;

    case ActionType::Unlink:
      if (plUsdLiveLink* pLink = GetSelectedRootLink(pDocument))
        plUsdLivePanel::UnlinkInteractive(pLink);
      return;

    case ActionType::ShowPanel:
      if (plUsdLivePanel* pPanel = plUsdLivePanel::GetSingleton())
        pPanel->EnsureVisible();
      return;

    case ActionType::OpenInBlender:
    {
      plUsdLiveLink* pLink = GetSelectedLink(pDocument);

      // A placed USD prefab: its prefab is where the link lives, so it opens first and then goes to Blender.
      if (pLink == nullptr)
      {
        const plString sPrefab = GetSelectedUsdPrefab(pDocument);
        plDocument* pPrefab = sPrefab.IsEmpty() ? nullptr : OpenWithWindow(sPrefab);
        if (pPrefab == nullptr)
          return;

        plHybridArray<plUsdLiveLink*, 1> links;
        plUsdLiveManager::GetSingleton()->GetLinks(pPrefab, links);
        if (links.IsEmpty())
          return;

        pLink = links[0];
      }

      const plStatus res = plUsdApps::GetSingleton()->Open(plUsdApps::App::Blender, pLink);
      report(res, "Opening the stage in Blender failed.");

      // Finding Blender and installing its connector happen in the Apps tab.
      if (res.Failed())
      {
        if (plUsdLivePanel* pPanel = plUsdLivePanel::GetSingleton())
          pPanel->ShowAppsTab();
      }
      return;
    }

    case ActionType::BakeNow:
      if (plUsdLiveLink* pLink = GetSelectedLink(pDocument))
        pLink->RequestBake(true);
      return;

    case ActionType::OpenUsdPrefab:
    {
      const plString sPrefab = GetSelectedUsdPrefab(pDocument);
      if (!sPrefab.IsEmpty())
        OpenWithWindow(sPrefab);
      return;
    }

    case ActionType::AppsInstall:
    case ActionType::AppsRemove:
    case ActionType::AppsOpen:
    {
      plUsdApps* pApps = plUsdApps::GetSingleton();
      const plString sApp = value.IsA<plString>() ? value.Get<plString>() : value.ConvertTo<plString>();

      plUsdApps::App app;
      if (pApps == nullptr || !plUsdApps::FindApp(sApp, app))
      {
        plLog::Error("USD Live: '{}' is not a tool with a connector; use Blender, Houdini or Maya.", sApp);
        return;
      }

      // Failures are logged and shown in the tool's row; a dialog would stop a script.
      if (m_Type == ActionType::AppsInstall)
        pApps->InstallConnector(app).IgnoreResult();
      else if (m_Type == ActionType::AppsRemove)
        pApps->RemoveConnector(app).IgnoreResult();
      else
      {
        plUsdLivePanel* pPanel = plUsdLivePanel::GetSingleton();
        pApps->Open(app, pPanel != nullptr ? pPanel->GetShownLink() : nullptr).IgnoreResult();
      }
      return;
    }

    case ActionType::ImportPartition:
    {
      plUsdPartitionImporter* pImporter = plUsdPartitionImporter::GetSingleton();
      const plString sJson = value.IsValid() ? (value.IsA<plString>() ? value.Get<plString>() : value.ConvertTo<plString>()) : plString();
      if (pImporter == nullptr)
        return;

      // Without options the dialog opens; it is not modal, so nothing waits for it.
      if (sJson.IsEmpty())
      {
        if (plScene2Document* pScene = plUsdPartitionPanelExtension::GetTargetScene())
          plUsdPartitionImportDialog::ShowForNewImport(pScene, {});
        else
          plLog::Error("USD Import: open a scene first, or pass the import options as JSON, e.g. {\"stage\": \"<file>\", \"partition\": \"<name>\"}.");
        return;
      }

      const plStatus res = pImporter->StartFromJson(sJson);
      if (res.Failed())
        plLog::Error("USD Import: {}", res.m_sMessage);
      return;
    }
  }
}
