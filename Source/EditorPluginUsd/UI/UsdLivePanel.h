#pragma once

#include <EditorPluginUsd/EditorPluginUsdDLL.h>
#include <EditorUi/Runtime.h>
#include <Foundation/Communication/Event.h>
#include <Foundation/Configuration/Singleton.h>
#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Types/Uuid.h>

class plDocument;
class plEngineUsdLiveView;
class plUsdLiveLink;
struct plUsdLayerInfo;
struct plUsdLiveManagerEvent;

/// \brief Shows the session side of the USD link that owns the selection: the stage, an icon toolbar with the link's
/// actions, its layer stack, the participants of its session, and the tools that can join it (the Apps tab, over
/// plUsdApps: their installations, their connectors, and the instances launched into the session).
///
/// The prims themselves are not listed here; they are the link root's children in the Scenegraph. With several links
/// in a scene, the panel follows the selection and otherwise keeps the link it showed last; a picker in its header lets
/// the user switch explicitly.
///
/// An application panel of the engine shell ("Panel.UsdLive", in the Panels menu): the shell makes its content,
/// plEngineUsdLiveView, while the panel is open. This object picks the link, refreshes the content every 250 ms and runs
/// the actions.
class PL_EDITORPLUGINUSD_DLL plUsdLivePanel
{
  PL_DECLARE_SINGLETON(plUsdLivePanel);

public:
  static constexpr const char* PanelId = "Panel.UsdLive";

  plUsdLivePanel();
  ~plUsdLivePanel();

  /// \brief Asks for a USD file and links it into the given scene. Shared with the scene menu action.
  static void LinkStageInteractive(plDocument* pDocument);

  /// \brief Asks for a session and a participant name, offering the sessions that already exist beside the asset, and
  /// starts or joins the session.
  static void StartSessionInteractive(plUsdLiveLink* pLink);

  /// \brief Asks for confirmation, then removes the link and its generated objects.
  static void UnlinkInteractive(plUsdLiveLink* pLink);

  /// \brief Shows what committing would fold into the asset, asks for confirmation, then commits the link's session.
  static void CommitInteractive(plUsdLiveLink* pLink);

  /// \brief Opens the panel if it is closed and brings its tab to the front.
  void EnsureVisible();

  /// \brief EnsureVisible(), with the Apps tab current.
  void ShowAppsTab();

  /// \brief The link the panel shows, if it still exists. The Apps actions work on it.
  plUsdLiveLink* GetShownLink() const;

private:
  friend class plEngineUsdLiveView;

  /// \brief Gathers the scene, its links and the shown link's layers and hands them to the content, when it shows.
  void OnRefresh();

  /// \brief Shows the link at iIndex among the active scene's links.
  void OnLinkPicked(int iIndex);

  /// \brief The content tells the panel that it shows (pView) or goes (nullptr).
  void SetView(plEngineUsdLiveView* pView);

  void ManagerEventHandler(const plUsdLiveManagerEvent& e);

  /// \brief The scene of the active document tab; when another kind of tab is active, the scene that was active last,
  /// else the first open scene.
  plDocument* GetActiveSceneDocument() const;

  /// \brief The toolbar's Save, Revert and world partition import.
  void SaveShownLayer();
  void RevertSelection();
  void ImportPartition();

  /// \brief The link that owns the selection, else the one shown last, else the scene's first link.
  plUsdLiveLink* GetCurrentLink(plDocument* pDocument, const plDynamicArray<plUsdLiveLink*>& links);

  /// \brief Launches a tool into the shown link's session. Failures show in the tool's row, and in a message box when
  /// bReportInDialog is set (the toolbar menu, where the row is not in view).
  void OpenIn(plUInt8 uiApp, bool bReportInDialog);

  plEngineUsdLiveView* m_pView = nullptr; ///< the shell's content while the panel is open
  bool m_bAppsTabRequested = false;       ///< ShowAppsTab ran before the content existed
  plEditorUiTimerGroup m_Timers;          ///< refreshes the content every 250 ms

  const plDocument* m_pShownDocument = nullptr; ///< The scene m_ShownRoot belongs to.
  plUuid m_ShownRoot;                           ///< Root object of the link shown, kept while nothing linked is selected.
  plUsdLiveLink* m_pShownLink = nullptr;        ///< Only compared against, never dereferenced without a lookup.
  plEventSubscriptionID m_ManagerSubscription = 0;
};
