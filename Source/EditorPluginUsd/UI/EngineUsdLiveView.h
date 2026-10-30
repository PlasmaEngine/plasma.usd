#pragma once

#include <EditorPluginUsd/EditorPluginUsdDLL.h>
#include <EditorPluginUsd/UI/UsdApps.h>
#include <EditorUi/Content.h>
#include <EditorUi/Tabs.h>
#include <RendererCore/Textures/Texture2DResource.h>

class plDocument;
class plUsdLivePanel;
class plUiElement;
class plUsdLiveLink;
struct plUsdLayerInfo;

/// \brief The USD Live panel's content, drawn by the engine UI: a header with the stage, the session line, the link
/// picker and the state dot; a toolbar row with the link's actions; Layers, Participants and Apps tabs; and a status
/// row.
///
/// The engine shell makes it while the USD Live panel is open; it tells plUsdLivePanel that it shows, and the panel
/// picks the link and calls Refresh every 250 ms. The view gathers what it shows from the link and from plUsdApps and
/// rebuilds its document only when that changed. Actions go through the panel and its dialogs.
class PL_EDITORPLUGINUSD_DLL plEngineUsdLiveView : public plEditorUiContent
{
public:
  explicit plEngineUsdLiveView(plUsdLivePanel* pPanel);
  ~plEngineUsdLiveView() override;

  void Refresh(plDocument* pDocument, plUsdLiveLink* pLink, const plDynamicArray<plUsdLiveLink*>& links, const plDynamicArray<plUsdLayerInfo>& layers);

  enum Tab : plUInt8
  {
    Layers,
    Participants,
    Apps,
  };

  void SetCurrentTab(Tab tab);
  Tab GetCurrentTab() const { return static_cast<Tab>(m_Tabs.GetCurrent()); }
  void ShowAppsTab() { SetCurrentTab(Apps); }

  /// The toolbar's actions, in toolbar order.
  enum Action : plUInt8
  {
    Link,
    Session,
    Reload,
    Save,
    Revert,
    Unlink,
    OpenIn,
    Bake,
    Commit,
    Partition,
    ActionCount
  };

  /// Does what clicking a toolbar button does.
  void RunAction(Action action);

protected:
  void OnUiUpdate() override;
  void OnUiEvent(const plMsgUiEvent& event) override;

private:
  enum class Tone : plUInt8
  {
    Good,
    Attention,
    Idle,
  };

  struct ActionState
  {
    bool m_bEnabled = false;
    plString m_sTooltip;
  };

  struct LayerRow
  {
    plString m_sFile;
    plString m_sPath;
    plString m_sIdentifier;
    plString m_sOpinions;
    plString m_sState;
    bool m_bEditTarget = false;
    bool m_bMutable = false; ///< another participant's layer: it can be muted here
    bool m_bMuted = false;
  };

  struct ParticipantRow
  {
    plString m_sTool;
    plString m_sUser; ///< with " (you)" for this editor's layer
    plString m_sState;
    plString m_sOpinions;
    plString m_sPath;
    Tone m_Tone = Tone::Idle;
    bool m_bHighlighted = false;
  };

  struct AppLine
  {
    plString m_sText;
    Tone m_Tone = Tone::Idle;
  };

  struct AppRow
  {
    plUsdApps::App m_App = plUsdApps::App::Blender;
    plString m_sName;
    plHybridArray<plString, 4> m_VersionTexts;
    plHybridArray<plString, 4> m_VersionPaths; ///< executables; empty for "Locate another..."
    plInt32 m_iSelectedVersion = -1;
    bool m_bVersionsEnabled = true;
    plString m_sExecutable; ///< the selected installation's, for its icon
    plString m_sConnector;
    plString m_sConnectorTooltip;
    Tone m_ConnectorTone = Tone::Idle;
    bool m_bShowInstall = false;
    bool m_bInstallEnabled = true;
    plString m_sInstallLabel;
    plString m_sInstallTooltip;
    bool m_bShowRemove = false;
    bool m_bRemoveEnabled = true;
    plString m_sRemoveTooltip;
    bool m_bShowLocate = false;
    plString m_sLocateTooltip;
    bool m_bOpenEnabled = false;
    plString m_sOpenTooltip;
    AppLine m_Message;
    plHybridArray<AppLine, 2> m_Instances;
  };

  // gathering
  void GatherHeader(plDocument* pDocument, plUsdLiveLink* pLink, const plDynamicArray<plUsdLiveLink*>& links);
  void GatherToolbar(plDocument* pDocument, plUsdLiveLink* pLink);
  void GatherLayers(plUsdLiveLink* pLink, const plDynamicArray<plUsdLayerInfo>& layers);
  void GatherParticipants(plUsdLiveLink* pLink, const plDynamicArray<plUsdLayerInfo>& layers);
  void GatherApps(plUsdLiveLink* pLink);
  void GatherStatus(plUsdLiveLink* pLink);
  plString DescribeState() const;

  // document
  void BuildDocument();
  float BuildHeader(plUiElement* pRoot, float fWidth);
  float BuildToolbar(plUiElement* pRoot, float y, float fWidth);
  float BuildLayers(plUiElement* pBody, float fWidth);
  float BuildParticipants(plUiElement* pBody, float fWidth);
  float BuildApps(plUiElement* pBody, float fWidth);
  float BuildEmpty(plUiElement* pBody, float fWidth, plStringView sLine, bool bOfferLink);
  float BuildNote(plUiElement* pBody, plStringView sText, float y, float fWidth);
  void UpdateTabs();

  // menus
  void OpenLinkPicker();
  void OpenOpenInMenu();
  void OpenVersionMenu(plUInt32 uiApp);
  plVec2 GetBelow(plStringView sElement) const;

  /// The executable's own icon as a texture, from the platform; invalid when there is none.
  plTexture2DResourceHandle GetExecutableIcon(plStringView sExecutable);

  static plColorGammaUB ToneColor(Tone tone);

  plUsdLivePanel* m_pPanel = nullptr;
  plEditorUiTabs m_Tabs;

  bool m_bHasDocument = false;
  bool m_bHasLink = false;
  plString m_sStage;
  plString m_sStagePath;
  plString m_sDetail;
  plHybridArray<plString, 4> m_LinkNames;
  plInt32 m_iShownLink = -1;
  bool m_bShowState = false;
  plString m_sState;
  Tone m_StateTone = Tone::Idle;

  ActionState m_Actions[ActionCount];

  plDynamicArray<LayerRow> m_Layers;
  plString m_sLayersNote;
  bool m_bSession = false;
  plDynamicArray<ParticipantRow> m_Participants;
  plString m_sParticipantsNote;
  plDynamicArray<AppRow> m_Apps;
  plString m_sAppsNote;
  plString m_sStatusLeft;
  plString m_sStatusRight;

  plHashTable<plString, plTexture2DResourceHandle> m_ExecutableIcons; ///< executable -> its icon (invalid when it has none)

  plString m_sBuiltState;
  float m_fBuiltWidth = -1.0f;
  bool m_bRebuild = true;
};
