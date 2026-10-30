#include <EditorPluginUsd/EditorPluginUsdPCH.h>

#include <Core/World/GameObject.h>
#include <EditorFramework/Document/GameObjectDocument.h>
#include <EditorFramework/IPC/EngineProcessConnection.h>
#include <EditorPluginUsd/LiveLink/UsdLiveManager.h>
#include <EditorPluginUsd/Utils/UsdJson.h>
#include <Foundation/Algorithm/HashingUtils.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Strings/StringConversion.h>
#include <Foundation/System/Process.h>
#include <ToolsFoundation/CommandHistory/CommandHistory.h>
#include <ToolsFoundation/Document/DocumentManager.h>
#include <ToolsFoundation/Object/ObjectAccessorBase.h>
#include <ToolsFoundation/Project/ToolsProject.h>

PL_IMPLEMENT_SINGLETON(plUsdLiveManager);

namespace
{
  // Readers treat a presence file older than a few refreshes as left behind by a tool that exited.
  constexpr plTime PresenceInterval = plTime::MakeFromSeconds(5);
} // namespace

plUsdLiveManager::plUsdLiveManager()
  : m_SingletonRegistrar(this)
{
  m_pTextures = PL_DEFAULT_NEW(plUsdLiveTextures);
  m_pBackgroundBaker = PL_DEFAULT_NEW(plUsdBackgroundBaker);
  m_pHub = PL_DEFAULT_NEW(plUsdLiveHub);

  m_DocumentSubscription = plDocumentManager::s_Events.AddEventHandler([this](const plDocumentManager::Event& e)
    {
      if (e.m_Type == plDocumentManager::Event::Type::DocumentOpened)
      {
        LoadLinks(e.m_pDocument);
      }
      else if (e.m_Type == plDocumentManager::Event::Type::DocumentClosing)
      {
        CloseLinks(e.m_pDocument);
      }
    });

  m_ProjectSubscription = plToolsProject::s_Events.AddEventHandler([this](const plToolsProjectEvent& e)
    {
      if (e.m_Type == plToolsProjectEvent::Type::ProjectOpened)
        m_pBackgroundBaker->ScanProject();
      else if (e.m_Type == plToolsProjectEvent::Type::ProjectClosing)
        m_pBackgroundBaker->Clear();
    });

  // The plugin can load into a project that is already open.
  if (plToolsProject::IsProjectOpen())
    m_pBackgroundBaker->ScanProject();

  // Every link of every open scene is driven from one timer on the main thread, which is where the documents live.
  m_Timers.Start(plTime::MakeFromMilliseconds(50), [this]()
    { Update(); }, true);
}

plUsdLiveManager::~plUsdLiveManager()
{
  m_Timers.StopAll();

  plDocumentManager::s_Events.RemoveEventHandler(m_DocumentSubscription);
  plToolsProject::s_Events.RemoveEventHandler(m_ProjectSubscription);

  m_pHub.Clear();
  m_Links.Clear();
  m_pBackgroundBaker.Clear();
  m_pTextures.Clear();
  UpdatePresence();
}

bool plUsdLiveManager::IsLinkOpen(plStringView sLinkId) const
{
  for (const auto& pLink : m_Links)
  {
    if (pLink->GetLinkId() == sLinkId)
      return true;
  }

  return false;
}

plStringView plUsdLiveManager::GetBackgroundBakeStatus() const
{
  return m_pBackgroundBaker->GetStatus();
}

plString plUsdLiveManager::GetDefaultParticipant()
{
  plStringBuilder sUser = "plasma";

  wchar_t szName[256] = {};
  size_t uiLength = 0;
  if (_wgetenv_s(&uiLength, szName, PL_ARRAY_SIZE(szName), L"USERNAME") == 0 && szName[0] != 0)
  {
    sUser.Append("-", plStringUtf8(szName).GetView());
  }

  sUser.ToLower();
  sUser.ReplaceAll(" ", "_");
  return sUser;
}

void plUsdLiveManager::ReadSidecar(plStringView sDocumentPath, plDynamicArray<SidecarLink>& out_links)
{
  out_links.Clear();

  plVariantDictionary sidecar;
  if (!plUsdJson::ReadFile(plStringBuilder(sDocumentPath, ".usdlinks"), sidecar))
    return;

  for (const plVariant& value : plUsdJson::GetArray(sidecar, "links"))
  {
    const plVariantDictionary& json = plUsdJson::AsObject(value);

    SidecarLink& link = out_links.ExpandAndGetRef();
    link.m_Root = plConversionUtils::ConvertStringToUuid(plUsdJson::GetString(json, "root"));
    link.m_Settings.m_sStageFile = plUsdJson::GetString(json, "stage");
    link.m_Settings.m_sSessionName = plUsdJson::GetString(json, "session");
    link.m_Settings.m_sParticipant = plUsdJson::GetString(json, "participant");
  }
}

plString plUsdLiveManager::GetPresenceFolder()
{
  return plOSFile::GetUserDataFolder("PlasmaUsdLive/Presence");
}

plString plUsdLiveManager::GetSidecarPath(const plDocument* pDocument)
{
  plStringBuilder s(pDocument->GetDocumentPath(), ".usdlinks");
  return s;
}

void plUsdLiveManager::Broadcast(const plDocument* pDocument)
{
  plUsdLiveManagerEvent e;
  e.m_Type = plUsdLiveManagerEvent::Type::LinksChanged;
  e.m_pDocument = pDocument;
  m_Events.Broadcast(e);
}

void plUsdLiveManager::BroadcastPrimStatesChanged(plUsdLiveLink* pLink)
{
  plUsdLiveManagerEvent e;
  e.m_Type = plUsdLiveManagerEvent::Type::PrimStatesChanged;
  e.m_pDocument = pLink->GetDocument();
  e.m_pLink = pLink;
  m_Events.Broadcast(e);
}

void plUsdLiveManager::SetHighlightedLayer(plUsdLiveLink* pLink, plStringView sLayerPath)
{
  if (pLink->GetHighlightedLayer() == sLayerPath)
    return;

  pLink->SetHighlightedLayer(sLayerPath);

  plUsdLiveManagerEvent e;
  e.m_Type = plUsdLiveManagerEvent::Type::HighlightChanged;
  e.m_pDocument = pLink->GetDocument();
  e.m_pLink = pLink;
  m_Events.Broadcast(e);
}

void plUsdLiveManager::Update()
{
  m_pTextures->Update();

  for (auto& pLink : m_Links)
  {
    pLink->Update();
  }

  m_pBackgroundBaker->Update();
  m_pHub->Update();

  if (plTime::Now() >= m_NextPresenceUpdate)
  {
    m_NextPresenceUpdate = plTime::Now() + PresenceInterval;
    UpdatePresence();
  }
}

void plUsdLiveManager::UpdatePresence()
{
  const plString sFolder = GetPresenceFolder();
  const plOsProcessID pid = plProcess::GetCurrentProcessID();
  plHashSet<plString> written;

  for (const auto& pLink : m_Links)
  {
    if (!pLink->IsWritable())
      continue;

    const plUsdLiveLink::Settings& settings = pLink->GetSettings();

    plStringBuilder sSessionRoot = pLink->GetParticipantLayer();
    sSessionRoot.PathParentDirectory();
    sSessionRoot.AppendPath("session.usda");

    plStringBuilder sFile = sFolder;
    sFile.AppendFormat("/{}-{}-{}.json", settings.m_sParticipant, pid, plHashingUtils::xxHash32String(sSessionRoot));

    if (written.Contains(sFile))
      continue;

    if (written.IsEmpty() && plOSFile::CreateDirectoryStructure(sFolder).Failed())
    {
      static bool s_bReported = false;
      if (!s_bReported)
        plLog::Warning("USD Live: could not create '{}'; other tools will not see this editor's sessions.", sFolder);
      s_bReported = true;
      return;
    }

    plVariantDictionary json;
    json.Insert("version", 1);
    json.Insert("tool", plString("Plasma Editor"));
    json.Insert("participant", settings.m_sParticipant);
    json.Insert("stage", settings.m_sStageFile);
    json.Insert("session", settings.m_sSessionName);
    json.Insert("sessionRoot", plString(sSessionRoot));
    json.Insert("document", plString(pLink->GetDocument()->GetDocumentPath()));
    json.Insert("pid", static_cast<plInt64>(pid));
    if (m_pHub != nullptr && m_pHub->GetPort() != 0)
      json.Insert("hubPort", static_cast<plInt32>(m_pHub->GetPort()));

    // Rewriting the file is the heartbeat: readers go by its modification time.
    if (plUsdJson::WriteFile(sFile, json).Succeeded())
      written.Insert(sFile);
  }

  for (const plString& sOld : m_PresenceFiles)
  {
    if (!written.Contains(sOld))
      plOSFile::DeleteFile(sOld).IgnoreResult();
  }

  m_PresenceFiles = std::move(written);
}

plStatus plUsdLiveManager::LinkStage(plGameObjectDocument* pDocument, plStringView sStageFile)
{
  plStringBuilder sStage = sStageFile;
  sStage.MakeCleanPath();

  // The root object is an ordinary object created as one undoable step; only its children come from the stage.
  plCommandHistory* pHistory = pDocument->GetCommandHistory();
  plObjectAccessorBase* pAccessor = pDocument->GetObjectAccessor();

  plUuid rootGuid = plUuid::MakeUuid();
  pHistory->StartTransaction("Link USD Stage");

  // A prefab keeps its content under its "<Prefab-Root>" object, which placing it replaces with the placed object.
  const plDocumentObject* pParent = pDocument->GetObjectManager()->GetRootObject();
  for (const plDocumentObject* pChild : pParent->GetChildren())
  {
    if (pChild->GetParentProperty() == "Children" && pChild->GetTypeAccessor().GetValue("Name").ConvertTo<plString>() == "<Prefab-Root>")
    {
      pParent = pChild;
      break;
    }
  }

  plStatus res = pAccessor->AddObjectByName(pParent, "Children", -1, plGetStaticRTTI<plGameObject>(), rootGuid);
  if (res.Succeeded())
  {
    plStringBuilder sName("USD ", sStage.GetFileName());
    res = pAccessor->SetValueByName(pAccessor->GetObject(rootGuid), "Name", plString(sName));
  }

  if (res.Failed())
  {
    pHistory->CancelTransaction();
    return res;
  }

  pHistory->FinishTransaction();

  plUsdLiveLink::Settings settings;
  settings.m_sStageFile = sStage;

  m_Links.PushBack(PL_DEFAULT_NEW(plUsdLiveLink, pDocument, rootGuid, settings));
  res = m_Links.PeekBack()->Open();

  SaveLinks(pDocument);
  Broadcast(pDocument);
  return res;
}

void plUsdLiveManager::Unlink(plUsdLiveLink* pLink, bool bRemoveObjects)
{
  for (plUInt32 i = 0; i < m_Links.GetCount(); ++i)
  {
    if (m_Links[i].Borrow() != pLink)
      continue;

    const plDocument* pDocument = pLink->GetDocument();
    pLink->Close(bRemoveObjects);
    m_Links.RemoveAtAndCopy(i);
    m_NextPresenceUpdate = plTime();

    SaveLinks(pDocument);
    Broadcast(pDocument);
    return;
  }
}

plStatus plUsdLiveManager::StartSession(plUsdLiveLink* pLink, plStringView sSessionName, plStringView sParticipant)
{
  plStatus res = pLink->StartSession(sSessionName, sParticipant);
  m_NextPresenceUpdate = plTime();
  SaveLinks(pLink->GetDocument());
  Broadcast(pLink->GetDocument());
  return res;
}

plStatus plUsdLiveManager::CommitSession(plUsdLiveLink* pLink, plUInt32& out_uiOpinions)
{
  const plString sSessionRoot = pLink->GetSessionRoot();
  PL_SUCCEED_OR_RETURN(pLink->CommitSession(out_uiOpinions));

  m_pHub->NotifyCommitted(sSessionRoot);
  Broadcast(pLink->GetDocument());
  return plStatus(PL_SUCCESS);
}

void plUsdLiveManager::GetAllLinks(plDynamicArray<plUsdLiveLink*>& out_links) const
{
  out_links.Clear();
  for (const auto& pLink : m_Links)
    out_links.PushBack(pLink.Borrow());
}

void plUsdLiveManager::BroadcastPeersChanged()
{
  plUsdLiveManagerEvent e;
  e.m_Type = plUsdLiveManagerEvent::Type::PeersChanged;
  m_Events.Broadcast(e);
}

void plUsdLiveManager::GetLinks(const plDocument* pDocument, plDynamicArray<plUsdLiveLink*>& out_links) const
{
  out_links.Clear();
  for (const auto& pLink : m_Links)
  {
    if (pLink->GetDocument() == pDocument)
      out_links.PushBack(pLink.Borrow());
  }
}

plUsdLiveLink* plUsdLiveManager::FindLinkForObject(const plDocument* pDocument, const plUuid& object) const
{
  for (const auto& pLink : m_Links)
  {
    if (pLink->GetDocument() == pDocument && (pLink->IsGeneratedObject(object) || pLink->GetRootObject() == object))
      return pLink.Borrow();
  }

  return nullptr;
}

void plUsdLiveManager::CloseLinks(const plDocument* pDocument)
{
  bool bAny = false;

  for (plUInt32 i = m_Links.GetCount(); i-- > 0;)
  {
    if (m_Links[i]->GetDocument() == pDocument)
    {
      // The document is going away with its objects; only the stage and pending work need stopping.
      m_Links[i]->Close(false);
      m_Links.RemoveAtAndCopy(i);
      bAny = true;
    }
  }

  if (bAny)
  {
    m_NextPresenceUpdate = plTime();
    Broadcast(pDocument);
  }
}

void plUsdLiveManager::LoadLinks(plDocument* pDocument)
{
  if (plDynamicCast<plGameObjectDocument*>(pDocument) == nullptr)
    return;

  plVariantDictionary sidecar;
  if (!plUsdJson::ReadFile(GetSidecarPath(pDocument), sidecar))
    return;

  for (const plVariant& value : plUsdJson::GetArray(sidecar, "links"))
  {
    const plVariantDictionary& link = plUsdJson::AsObject(value);

    const plUuid root = plConversionUtils::ConvertStringToUuid(plUsdJson::GetString(link, "root"));
    if (!root.IsValid() || pDocument->GetObjectManager()->GetObject(root) == nullptr)
    {
      plLog::Warning("USD Live: '{}' links an object that is not in the scene any more; skipped.", GetSidecarPath(pDocument));
      continue;
    }

    plUsdLiveLink::Settings settings;
    settings.m_sStageFile = plUsdJson::GetString(link, "stage");
    settings.m_sSessionName = plUsdJson::GetString(link, "session");
    settings.m_sParticipant = plUsdJson::GetString(link, "participant");

    if (!settings.m_sSessionName.IsEmpty() && settings.m_sParticipant.IsEmpty())
      settings.m_sParticipant = GetDefaultParticipant();

    m_Links.PushBack(PL_DEFAULT_NEW(plUsdLiveLink, pDocument, root, settings));

    const plStatus res = m_Links.PeekBack()->Open();
    if (res.Failed())
    {
      plLog::Error("USD Live: could not open '{}': {}", settings.m_sStageFile, res.m_sMessage);
    }
  }

  Broadcast(pDocument);
}

void plUsdLiveManager::SaveLinks(const plDocument* pDocument) const
{
  plVariantArray links;

  for (const auto& pLink : m_Links)
  {
    if (pLink->GetDocument() != pDocument)
      continue;

    plStringBuilder sGuid;
    plConversionUtils::ToString(pLink->GetRootObject(), sGuid);

    plVariantDictionary link;
    link.Insert("root", plString(sGuid));
    link.Insert("stage", pLink->GetSettings().m_sStageFile);
    link.Insert("session", pLink->GetSettings().m_sSessionName);
    link.Insert("participant", pLink->GetSettings().m_sParticipant);
    links.PushBack(link);
  }

  const plString sPath = GetSidecarPath(pDocument);

  // No links, no sidecar: a scene that never used the package carries no trace of it.
  if (links.IsEmpty())
  {
    plOSFile::DeleteFile(sPath).IgnoreResult();
    m_pBackgroundBaker->UpdateDocument(pDocument->GetDocumentPath());
    return;
  }

  plVariantDictionary root;
  root.Insert("version", 1);
  root.Insert("links", links);

  if (plUsdJson::WriteFile(sPath, root).Failed())
  {
    plLog::Error("USD Live: could not write '{}'", sPath);
    return;
  }

  m_pBackgroundBaker->UpdateDocument(pDocument->GetDocumentPath());
}
