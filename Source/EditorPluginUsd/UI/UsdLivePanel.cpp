#include <EditorPluginUsd/EditorPluginUsdPCH.h>

#include <EditorFramework/Document/GameObjectDocument.h>
#include <EditorFramework/EngineShell/EngineShell.h>
#include <EditorPluginUsd/LiveLink/UsdLiveManager.h>
#include <EditorPluginUsd/PartitionImport/UsdPartitionImportDialog.h>
#include <EditorPluginUsd/PartitionImport/UsdPartitionPanelExtension.h>
#include <EditorPluginUsd/UI/EngineUsdLiveView.h>
#include <EditorPluginUsd/UI/UsdApps.h>
#include <EditorPluginUsd/UI/UsdLivePanel.h>
#include <EditorPluginUsd/UI/UsdLiveText.h>
#include <EditorUi/Platform/EditorPlatform.h>
#include <Foundation/IO/OSFile.h>
#include <GuiFoundation/Dialogs/FileDialog.h>
#include <GuiFoundation/DocumentWindow/DocumentWindow.h>
#include <GuiFoundation/EngineUi/ApplicationPanels.h>
#include <GuiFoundation/EngineUi/EnginePrompt.h>
#include <GuiFoundation/UIServices/UIServices.h>
#include <ToolsFoundation/Selection/SelectionManager.h>
#include <UsdBridge/UsdStage.h>

PL_IMPLEMENT_SINGLETON(plUsdLivePanel);

namespace
{
  constexpr const char* UsdIcon = ":/UsdPlugin/Icons/UsdLive.svg";

  using plUsdLiveText::IsInFolder;

  plGameObjectDocument* AsSceneDocument(const plDocument* pDocument)
  {
    // Documents are owned by their managers; the registry hands them out as const.
    return plDynamicCast<plGameObjectDocument*>(const_cast<plDocument*>(pDocument));
  }
} // namespace

plUsdLivePanel::plUsdLivePanel()
  : m_SingletonRegistrar(this)
{
  m_Timers.Start(plTime::MakeFromMilliseconds(250), [this]()
    { OnRefresh(); }, true);

  if (plUsdLiveManager* pManager = plUsdLiveManager::GetSingleton())
  {
    m_ManagerSubscription = pManager->m_Events.AddEventHandler(plMakeDelegate(&plUsdLivePanel::ManagerEventHandler, this));
  }

  // A tab beside the Log; it opens from the Panels menu and from the scene's USD menu.
  plEditorUiApplicationPanels::SetIcon(PanelId, UsdIcon);

  plEditorUiApplicationPanelDesc desc;
  desc.m_sId = PanelId;
  desc.m_sMenuIcon = UsdIcon;
  desc.m_CreateContent = []() -> plUniquePtr<plEditorUiContent>
  { return PL_DEFAULT_NEW(plEngineUsdLiveView, plUsdLivePanel::GetSingleton()); };
  plEditorUiApplicationPanels::Register(desc);
}

void plUsdLivePanel::SetView(plEngineUsdLiveView* pView)
{
  m_pView = pView;
  if (m_pView == nullptr)
    return;

  if (m_bAppsTabRequested)
  {
    m_bAppsTabRequested = false;
    m_pView->ShowAppsTab();
  }

  OnRefresh();
}

plUsdLivePanel::~plUsdLivePanel()
{
  // The shell destroys the content now, while the panel it talks to still exists.
  plEditorUiApplicationPanels::Unregister(PanelId);
  m_Timers.StopAll();

  if (plUsdLiveManager* pManager = plUsdLiveManager::GetSingleton())
  {
    pManager->m_Events.RemoveEventHandler(m_ManagerSubscription);
  }
}

void plUsdLivePanel::ManagerEventHandler(const plUsdLiveManagerEvent& e)
{
  OnRefresh();
}

void plUsdLivePanel::SaveShownLayer()
{
  if (plUsdLiveLink* pLink = GetShownLink())
  {
    const plStatus res = pLink->SaveNow();
    if (res.Failed())
      plEditorUiServices::MessageBoxStatus(res, "Saving your layer failed.");
  }
}

void plUsdLivePanel::RevertSelection()
{
  plUsdLiveLink* pLink = GetShownLink();
  if (pLink == nullptr)
    return;

  plHybridArray<plUuid, 32> objects;
  for (const plDocumentObject* pObject : pLink->GetDocument()->GetSelectionManager()->GetSelection())
    objects.PushBack(pObject->GetGuid());

  const plStatus res = pLink->Revert(objects);
  if (res.Failed())
    plEditorUiServices::MessageBoxStatus(res, "Reverting failed.");
}

void plUsdLivePanel::ImportPartition()
{
  plScene2Document* pScene = plUsdPartitionPanelExtension::GetTargetScene();
  if (pScene == nullptr)
  {
    plEditorUiServices::MessageBoxInformation("Open a scene first: the stage becomes a world partition of it.");
    return;
  }

  plUsdLiveLink* pLink = GetShownLink();
  plUsdPartitionImportDialog::ShowForNewImport(pScene, pLink != nullptr ? pLink->GetSettings().m_sStageFile.GetView() : plStringView());
}

void plUsdLivePanel::EnsureVisible()
{
  if (plEditorUiEngineShell* pShell = plEditorUiEngineShell::GetSingleton())
  {
    pShell->SetPanelOpen(PanelId, true);
    pShell->GetDock().SetCurrentPanel(PanelId);
  }

  OnRefresh();
}

void plUsdLivePanel::ShowAppsTab()
{
  // The content may only come with the panel; it shows the Apps tab when it does.
  m_bAppsTabRequested = m_pView == nullptr;
  if (m_pView != nullptr)
    m_pView->ShowAppsTab();

  EnsureVisible();
}

plDocument* plUsdLivePanel::GetActiveSceneDocument() const
{
  // The scene of the active document tab. While another kind of tab is active the scene active last is kept; failing
  // both, the first open scene.
  static const plDocument* s_pLastActive = nullptr;

  const plDocument* pActive = nullptr;
  if (plEditorUiEngineShell* pShell = plEditorUiEngineShell::GetSingleton())
  {
    if (plEditorUiDocumentFrame* pFrame = pShell->GetActiveFrame())
      pActive = pFrame->GetDocument();
  }

  plDocument* pFirst = nullptr;
  bool bLastActiveStillOpen = false;

  for (plDocumentWindow* pWindow : plDocumentWindow::GetAllDocumentWindows())
  {
    plGameObjectDocument* pScene = AsSceneDocument(pWindow->GetDocument());
    if (pScene == nullptr)
      continue;

    if (pScene == pActive)
    {
      s_pLastActive = pScene;
      return pScene;
    }

    if (pScene == s_pLastActive)
      bLastActiveStillOpen = true;

    if (pFirst == nullptr)
      pFirst = pScene;
  }

  if (bLastActiveStillOpen)
    return const_cast<plDocument*>(s_pLastActive);

  return pFirst;
}

plUsdLiveLink* plUsdLivePanel::GetCurrentLink(plDocument* pDocument, const plDynamicArray<plUsdLiveLink*>& links)
{
  if (pDocument == nullptr || links.IsEmpty())
  {
    m_pShownLink = nullptr;
    return nullptr;
  }

  auto show = [&](plUsdLiveLink* pLink)
  {
    m_pShownDocument = pDocument;
    m_ShownRoot = pLink->GetRootObject();
    m_pShownLink = pLink;
    return pLink;
  };

  for (const plDocumentObject* pObject : pDocument->GetSelectionManager()->GetSelection())
  {
    for (plUsdLiveLink* pLink : links)
    {
      if (pLink->GetRootObject() == pObject->GetGuid() || pLink->IsGeneratedObject(pObject->GetGuid()))
        return show(pLink);
    }
  }

  if (m_pShownDocument == pDocument)
  {
    for (plUsdLiveLink* pLink : links)
    {
      if (pLink->GetRootObject() == m_ShownRoot)
        return show(pLink);
    }
  }

  return show(links[0]);
}

plUsdLiveLink* plUsdLivePanel::GetShownLink() const
{
  plUsdLiveManager* pManager = plUsdLiveManager::GetSingleton();
  if (pManager == nullptr || m_pShownDocument == nullptr)
    return nullptr;

  plHybridArray<plUsdLiveLink*, 8> links;
  pManager->GetLinks(m_pShownDocument, links);

  for (plUsdLiveLink* pLink : links)
  {
    if (pLink == m_pShownLink && pLink->GetRootObject() == m_ShownRoot)
      return pLink;
  }

  return nullptr;
}

void plUsdLivePanel::OnRefresh()
{
  if (m_pView == nullptr)
    return;

  plUsdLiveManager* pManager = plUsdLiveManager::GetSingleton();
  plDocument* pDocument = GetActiveSceneDocument();

  plHybridArray<plUsdLiveLink*, 8> links;
  if (pManager && pDocument)
    pManager->GetLinks(pDocument, links);

  plUsdLiveLink* pLink = GetCurrentLink(pDocument, links);

  plHybridArray<plUsdLayerInfo, 16> layers;
  if (pLink && pLink->IsOpen())
    pLink->GetLayers(layers);

  m_pView->Refresh(pDocument, pLink, links, layers);
}

void plUsdLivePanel::OpenIn(plUInt8 uiApp, bool bReportInDialog)
{
  const plUsdApps::App app = (plUsdApps::App)uiApp;
  const plStatus res = plUsdApps::GetSingleton()->Open(app, GetShownLink());

  if (res.Failed() && bReportInDialog)
  {
    plStringBuilder sFailure;
    sFailure.SetFormat("Opening the stage in {} failed.", plUsdApps::GetName(app));
    plEditorUiServices::MessageBoxStatus(res, sFailure.GetData());
  }

  OnRefresh();
}

void plUsdLivePanel::OnLinkPicked(int iIndex)
{
  plUsdLiveManager* pManager = plUsdLiveManager::GetSingleton();
  plDocument* pDocument = GetActiveSceneDocument();
  if (pManager == nullptr || pDocument == nullptr)
    return;

  plHybridArray<plUsdLiveLink*, 8> links;
  pManager->GetLinks(pDocument, links);
  if (iIndex < 0 || iIndex >= (int)links.GetCount())
    return;

  plUsdLiveLink* pLink = links[iIndex];
  m_pShownDocument = pDocument;
  m_ShownRoot = pLink->GetRootObject();
  m_pShownLink = pLink;

  // The panel follows the selection, so a selection inside another link would switch it straight back.
  bool bSelectionElsewhere = false;
  for (const plDocumentObject* pObject : pDocument->GetSelectionManager()->GetSelection())
  {
    for (plUsdLiveLink* pOther : links)
    {
      if (pOther != pLink && (pOther->GetRootObject() == pObject->GetGuid() || pOther->IsGeneratedObject(pObject->GetGuid())))
        bSelectionElsewhere = true;
    }
  }

  if (bSelectionElsewhere)
  {
    if (const plDocumentObject* pRoot = pDocument->GetObjectManager()->GetObject(pLink->GetRootObject()))
      pDocument->GetSelectionManager()->SetSelection(pRoot);
  }

  OnRefresh();
}

void plUsdLivePanel::LinkStageInteractive(plDocument* pDocument)
{
  plGameObjectDocument* pScene = AsSceneDocument(pDocument);
  plUsdLiveManager* pManager = plUsdLiveManager::GetSingleton();
  if (pScene == nullptr || pManager == nullptr)
    return;

  static plString s_sLastFolder;
  const plString sFile = plFileDialog::GetOpenFileName("Link USD Stage", s_sLastFolder, plUsdStage::GetFileDialogFilter());
  if (sFile.IsEmpty())
    return;

  s_sLastFolder = plStringView(sFile).GetFileDirectory();

  const plStatus res = pManager->LinkStage(pScene, sFile);
  if (res.Failed())
  {
    plEditorUiServices::MessageBoxStatus(res, "Linking the USD stage failed.");
  }
}

void plUsdLivePanel::StartSessionInteractive(plUsdLiveLink* pLink)
{
  const plUsdLiveLink::Settings& settings = pLink->GetSettings();

  // The sessions beside the asset come first in the list; a typed name starts a new one.
  plHybridArray<plString, 8> sessions;
  plUsdStage::ListSessions(settings.m_sStageFile, sessions);

  const plString sCurrent = settings.m_sSessionName.IsEmpty() ? (sessions.IsEmpty() ? plString("live") : sessions[0]) : settings.m_sSessionName;
  plUInt32 uiCurrent = sessions.IndexOf(sCurrent);
  if (uiCurrent == plInvalidIndex)
  {
    sessions.InsertAt(0, sCurrent);
    uiCurrent = 0;
  }

  const char* szSessionLabel = sessions.GetCount() <= 1 && settings.m_sSessionName.IsEmpty()
                                 ? "No sessions exist beside this asset yet. A new session puts your layer strongest; other tools join by opening session.usda in its folder.\n\nSession"
                                 : "Pick a session to join it, or type a new name to start one. A new session puts your layer strongest; other tools join by opening session.usda in its folder.\n\nSession";

  bool bOk = false;
  plStringBuilder sSession = plEnginePrompt::GetItem("Start or Join a USD Session", szSessionLabel, sessions, uiCurrent, true, &bOk);
  sSession.Trim();
  if (!bOk || sSession.IsEmpty())
    return;

  const plString sDefaultParticipant = settings.m_sParticipant.IsEmpty() ? plUsdLiveManager::GetDefaultParticipant() : settings.m_sParticipant;
  plStringBuilder sParticipantName = plEnginePrompt::GetText("Start or Join a USD Session", "Your layer", sDefaultParticipant, &bOk);
  sParticipantName.Trim();
  if (!bOk || sParticipantName.IsEmpty())
    return;

  const plStatus res = plUsdLiveManager::GetSingleton()->StartSession(pLink, sSession, sParticipantName);
  if (res.Failed())
  {
    plEditorUiServices::MessageBoxStatus(res, "Starting the session failed.");
  }
}

void plUsdLivePanel::CommitInteractive(plUsdLiveLink* pLink)
{
  plStringBuilder sSessionFolder = pLink->GetParticipantLayer();
  sSessionFolder.PathParentDirectory();

  plHybridArray<plUsdLayerInfo, 8> layers;
  pLink->GetLayers(layers);

  plUInt32 uiLayers = 0, uiOpinions = 0;
  for (const plUsdLayerInfo& layer : layers)
  {
    if (IsInFolder(layer.m_sRealPath, sSessionFolder) && layer.m_sRealPath.GetFileNameAndExtension() != "session.usda")
    {
      ++uiLayers;
      uiOpinions += pLink->CountLayerOpinions(layer.m_sIdentifier);
    }
  }

  const plString& sAsset = pLink->GetSettings().m_sStageFile;
  plFileStats stats;
  const double fMegabytes = plOSFile::GetFileStats(sAsset, stats).Succeeded() ? stats.m_uiFileSize / (1024.0 * 1024.0) : 0.0;

  plStringBuilder sQuestion;
  sQuestion.SetFormat("Commit session '{}'?\n\n{} changes from {} participant layers are folded into {} ({} MB), which is rewritten and saved.",
    pLink->GetSettings().m_sSessionName, uiOpinions, uiLayers, sAsset.GetFileNameAndExtension(), plArgF(fMegabytes, 1));
  sQuestion.Append("\n\nEvery participant layer is then emptied, and the tools on the live hub reload. This cannot be undone.");

  if (plEditorUiServices::MessageBoxQuestion(sQuestion, plEditorUiMessageButton::Ok | plEditorUiMessageButton::Cancel, plEditorUiMessageButton::Cancel) != plEditorUiMessageButton::Ok)
    return;

  plEditorPlatform::SetOverrideCursor(plEditorPlatformCursor::Wait);
  plUInt32 uiCommitted = 0;
  const plStatus res = plUsdLiveManager::GetSingleton()->CommitSession(pLink, uiCommitted);
  plEditorPlatform::RestoreOverrideCursor();

  if (res.Failed())
  {
    plEditorUiServices::MessageBoxStatus(res, "Committing the session failed.");
    return;
  }

  plEditorUiServices::MessageBoxInformation(plFmt("Committed {} changes into {}.", uiCommitted, sAsset.GetFileNameAndExtension()));
}

void plUsdLivePanel::UnlinkInteractive(plUsdLiveLink* pLink)
{
  const plEditorUiMessageButton::Enum answer = plEditorUiServices::MessageBoxQuestion(
    plFmt("Stop the live link to '{}' and remove its generated objects? The root object stays in the scene, and nothing is written to USD.",
      pLink->GetSettings().m_sStageFile),
    plEditorUiMessageButton::Yes | plEditorUiMessageButton::Cancel, plEditorUiMessageButton::Cancel);

  if (answer == plEditorUiMessageButton::Yes)
  {
    plUsdLiveManager::GetSingleton()->Unlink(pLink, true);
  }
}
