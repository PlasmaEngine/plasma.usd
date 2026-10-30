#pragma once

#include <EditorPluginUsd/Bake/UsdBackgroundBaker.h>
#include <EditorPluginUsd/Hub/UsdLiveHub.h>
#include <EditorPluginUsd/LiveLink/UsdLiveLink.h>
#include <EditorPluginUsd/LiveLink/UsdLiveTextures.h>
#include <Foundation/Communication/Event.h>
#include <Foundation/Configuration/Singleton.h>
#include <Foundation/Containers/HashSet.h>
#include <EditorUi/Runtime.h>
#include <Foundation/Time/Time.h>

class plDocument;
class plGameObjectDocument;

struct plUsdLiveManagerEvent
{
  enum class Type
  {
    LinksChanged,
    HighlightChanged,  ///< A link's highlighted layer changed; m_pLink says which.
    PrimStatesChanged, ///< Prims of m_pLink changed their activation or where their values come from.
    PeersChanged,      ///< A tool joined or left the live hub, or its latency was measured again.
  };

  Type m_Type = Type::LinksChanged;
  const plDocument* m_pDocument = nullptr;
  plUsdLiveLink* m_pLink = nullptr;
};

/// \brief Owns every live link of every open scene and drives them from the main thread.
///
/// Which objects of a scene are linked to which stage is kept in a sidecar next to the scene file
/// ("<scene>.usdlinks"), not in the scene itself, so a scene never depends on this package: without it the link's root
/// object is an ordinary empty object. The links reopen when the scene does.
///
/// While a link is in a live session, the manager keeps a presence file for it in GetPresenceFolder() and rewrites it
/// every few seconds, so other tools (the Blender add-on) can list the open sessions and join them.
class PL_EDITORPLUGINUSD_DLL plUsdLiveManager
{
  PL_DECLARE_SINGLETON(plUsdLiveManager);

public:
  plUsdLiveManager();
  ~plUsdLiveManager();

  /// \brief Creates a new object in the document (one undoable step) and links the stage to it.
  plStatus LinkStage(plGameObjectDocument* pDocument, plStringView sStageFile);

  /// \brief Stops a link. With bRemoveObjects the generated objects are removed too; the root object stays.
  void Unlink(plUsdLiveLink* pLink, bool bRemoveObjects);

  plStatus StartSession(plUsdLiveLink* pLink, plStringView sSessionName, plStringView sParticipant);

  /// \brief Commits the link's session (owner only) and tells the tools on the live hub to reload.
  plStatus CommitSession(plUsdLiveLink* pLink, plUInt32& out_uiOpinions);

  void GetLinks(const plDocument* pDocument, plDynamicArray<plUsdLiveLink*>& out_links) const;
  void GetAllLinks(plDynamicArray<plUsdLiveLink*>& out_links) const;

  /// \brief The live hub other tools connect to; see plUsdLiveHub.
  plUsdLiveHub* GetHub() { return m_pHub.Borrow(); }

  /// \brief Tells the listeners the hub's participants changed. Called by the hub.
  void BroadcastPeersChanged();

  /// \brief The link that generated an object or has it as its root, or nullptr.
  plUsdLiveLink* FindLinkForObject(const plDocument* pDocument, const plUuid& object) const;

  /// \brief Tells the listeners that prims of a link changed state. Called by the link.
  void BroadcastPrimStatesChanged(plUsdLiveLink* pLink);

  /// \brief Sets the layer (by real path) whose decisions the UI highlights for a link, empty for none, and tells the
  /// listeners.
  void SetHighlightedLayer(plUsdLiveLink* pLink, plStringView sLayerPath);

  /// \brief The participant name this editor uses by default: "plasma-" and the OS user name.
  static plString GetDefaultParticipant();

  struct SidecarLink
  {
    plUuid m_Root;
    plUsdLiveLink::Settings m_Settings;
  };

  /// \brief Reads the links a document's sidecar lists, whether or not the document is open.
  static void ReadSidecar(plStringView sDocumentPath, plDynamicArray<SidecarLink>& out_links);

  /// \brief Where every tool in a live session keeps its presence file: "%APPDATA%/PlasmaUsdLive/Presence".
  static plString GetPresenceFolder();

  /// \brief The texture preparation cache every link shares.
  plUsdLiveTextures* GetTextures() { return m_pTextures.Borrow(); }

  /// \brief Whether a link with this id (see plUsdLiveLink::MakeLinkId) is open in any document.
  bool IsLinkOpen(plStringView sLinkId) const;

  /// \brief The background bake of closed documents' links; its status is empty while it is idle.
  plStringView GetBackgroundBakeStatus() const;

  plEvent<const plUsdLiveManagerEvent&> m_Events;

private:
  void Update();
  void LoadLinks(plDocument* pDocument);
  void SaveLinks(const plDocument* pDocument) const;
  static plString GetSidecarPath(const plDocument* pDocument);
  void Broadcast(const plDocument* pDocument);
  void CloseLinks(const plDocument* pDocument);

  /// \brief Writes a presence file for every link in a session and removes the files of links that left one.
  void UpdatePresence();

  plDynamicArray<plUniquePtr<plUsdLiveLink>> m_Links;
  plUniquePtr<plUsdLiveTextures> m_pTextures;
  plUniquePtr<plUsdBackgroundBaker> m_pBackgroundBaker;
  plUniquePtr<plUsdLiveHub> m_pHub;
  plEditorUiTimerGroup m_Timers; ///< drives Update every 50 ms
  plEventSubscriptionID m_DocumentSubscription = 0;
  plEventSubscriptionID m_ProjectSubscription = 0;
  plTime m_NextPresenceUpdate;
  plHashSet<plString> m_PresenceFiles;
};
