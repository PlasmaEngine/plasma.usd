#include <EditorPluginUsd/EditorPluginUsdPCH.h>

#include <EditorFramework/Document/GameObjectDocument.h>
#include <EditorPluginUsd/LiveLink/UsdLiveManager.h>
#include <EditorPluginUsd/UI/EngineUsdLiveView.h>
#include <EditorPluginUsd/UI/UsdLivePanel.h>
#include <EditorPluginUsd/UI/UsdLiveText.h>
#include <EditorUi/Menu.h>
#include <EditorUi/Surface.h>
#include <EditorUi/Widgets.h>
#include <Foundation/Math/ColorScheme.h>
#include <Foundation/Strings/StringConversion.h>
#include <Foundation/Utilities/ConversionUtils.h>
#include <GameUiPlugin/Elements/UiElements.h>
#include <GuiFoundation/UIServices/UIServices.h>
#include <ToolsFoundation/Selection/SelectionManager.h>

#include <Foundation/Basics/Platform/Win/IncludeWindows.h>
#include <shellapi.h>
#include <shlobj.h>

namespace
{
  using plUsdLiveText::Sep;

  /// The pixels of a bitmap of an icon, top row first, as BGRA; false when they cannot be read.
  bool ReadBitmap(HBITMAP hBitmap, plUInt32& out_uiWidth, plUInt32& out_uiHeight, plDynamicArray<plUInt8>& out_bgra)
  {
    BITMAP bitmap = {};
    if (hBitmap == nullptr || GetObjectW(hBitmap, sizeof(bitmap), &bitmap) == 0 || bitmap.bmWidth <= 0 || bitmap.bmHeight <= 0)
      return false;

    out_uiWidth = static_cast<plUInt32>(bitmap.bmWidth);
    out_uiHeight = static_cast<plUInt32>(bitmap.bmHeight);

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = bitmap.bmWidth;
    info.bmiHeader.biHeight = -bitmap.bmHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    out_bgra.SetCount(out_uiWidth * out_uiHeight * 4);

    HDC hDC = GetDC(nullptr);
    const int iLines = GetDIBits(hDC, hBitmap, 0, out_uiHeight, out_bgra.GetData(), &info, DIB_RGB_COLORS);
    ReleaseDC(nullptr, hDC);
    return iLines == static_cast<int>(out_uiHeight);
  }

  /// The executable's own icon at about uiSize pixels, as the platform shows it for the file; false when it has none.
  bool GetExecutableIconPixels(plStringView sExecutable, plUInt32 uiSize, plUInt32& out_uiWidth, plUInt32& out_uiHeight, plDynamicArray<plColorGammaUB>& out_pixels)
  {
    HICON hIcon = nullptr;
    if (SHDefExtractIconW(plStringWChar(sExecutable).GetData(), 0, 0, &hIcon, nullptr, uiSize) != S_OK || hIcon == nullptr)
      return false;

    ICONINFO icon = {};
    plDynamicArray<plUInt8> color, mask;
    plUInt32 uiMaskWidth = 0, uiMaskHeight = 0;
    bool bRead = false;

    if (GetIconInfo(hIcon, &icon))
    {
      bRead = ReadBitmap(icon.hbmColor, out_uiWidth, out_uiHeight, color);
      if (bRead && !ReadBitmap(icon.hbmMask, uiMaskWidth, uiMaskHeight, mask))
        mask.Clear();

      DeleteObject(icon.hbmColor);
      DeleteObject(icon.hbmMask);
    }

    DestroyIcon(hIcon);

    if (!bRead)
      return false;

    // Icons without an alpha channel are cut out by their mask, where set bits are transparent.
    bool bHasAlpha = false;
    for (plUInt32 i = 3; i < color.GetCount(); i += 4)
      bHasAlpha |= color[i] != 0;

    const bool bUseMask = !bHasAlpha && uiMaskWidth == out_uiWidth && uiMaskHeight >= out_uiHeight;

    out_pixels.SetCountUninitialized(out_uiWidth * out_uiHeight);
    for (plUInt32 i = 0; i < out_pixels.GetCount(); ++i)
    {
      const plUInt8 uiAlpha = bHasAlpha ? color[i * 4 + 3] : (bUseMask && mask[i * 4] != 0 ? 0 : 255);
      out_pixels[i] = plColorGammaUB(color[i * 4 + 2], color[i * 4 + 1], color[i * 4 + 0], uiAlpha);
    }

    return true;
  }

  constexpr float s_fSide = 10.0f;
  constexpr float s_fHeaderHeight = 48.0f;
  constexpr float s_fToolbarHeight = 30.0f;
  constexpr float s_fTableHeader = 26.0f;
  constexpr float s_fLine = 16.0f;
  constexpr float s_fIconButton = 24.0f;
  constexpr plUInt32 s_uiLabelSize = plEditorUi::FontSize - 1;
  constexpr plUInt32 s_uiNumberSize = plEditorUi::FontSize - 2;
  constexpr plUInt32 s_uiTagSize = plEditorUi::MinFontSize;

  constexpr const char* s_szDisabled = "Usd.Disabled"; ///< always true; disabled buttons bind it

  /// The package's line icons for the engine UI.
  plString LineIcon(plStringView sName)
  {
    return plStringBuilder(":/UsdPlugin/Line/", sName, ".svg");
  }

  plString Name(plStringView sKind, plUInt32 uiIndex)
  {
    plStringBuilder sName;
    sName.SetFormat("Usd.{}.{}", sKind, uiIndex);
    return sName;
  }

  /// The index after a prefix ("Usd.mute.2" with "Usd.mute."); false when the identifier has another prefix.
  bool ParseIndex(plStringView sIdentifier, plStringView sPrefix, plUInt32& out_uiIndex)
  {
    if (!sIdentifier.StartsWith(sPrefix))
      return false;

    plStringView sRest(sIdentifier.GetStartPointer() + sPrefix.GetElementCount(), sIdentifier.GetEndPointer());
    if (const char* szDot = sRest.FindSubString("."))
      sRest = plStringView(sRest.GetStartPointer(), szDot);

    return plConversionUtils::StringToUInt(sRest, out_uiIndex).Succeeded();
  }

  void Place(plUiElement* pElement, float x, float y, float fWidth, float fHeight)
  {
    pElement->m_vAnchorMin = pElement->m_vAnchorMax = plVec2::MakeZero();
    pElement->m_vOffsets = plVec4(x, y, fWidth, fHeight);
  }

  /// An opaque mix of two theme colours, as the style pass tints plates and lines.
  plColorGammaUB Mix(const plColorGammaUB& a, const plColorGammaUB& b, float t)
  {
    auto channel = [t](plUInt8 x, plUInt8 y)
    { return static_cast<plUInt8>(plMath::Clamp(x + (y - x) * t + 0.5f, 0.0f, 255.0f)); };

    return plColorGammaUB(channel(a.r, b.r), channel(a.g, b.g), channel(a.b, b.b), 255);
  }

  plColorGammaUB Hover()
  {
    return Mix(plEditorUi::Plate(), plEditorUi::Raised(), 0.5f);
  }

  /// Row separators: the hairline at 60 % over the panel.
  plColorGammaUB RowLine()
  {
    return Mix(plEditorUi::Surface(), plEditorUi::Line(), 0.6f);
  }

  /// Greedy word wrap at fWidth; line breaks in the text start new lines.
  void WrapLines(plStringView sText, plUInt32 uiFontSize, float fWidth, plDynamicArray<plString>& out_lines)
  {
    out_lines.Clear();

    plHybridArray<plStringView, 4> paragraphs;
    sText.Split(true, paragraphs, "\n");

    for (plStringView sParagraph : paragraphs)
    {
      plHybridArray<plStringView, 32> words;
      sParagraph.Split(false, words, " ");

      plStringBuilder sLine;
      for (plStringView sWord : words)
      {
        plStringBuilder sCandidate = sLine;
        if (!sCandidate.IsEmpty())
          sCandidate.Append(" ");
        sCandidate.Append(sWord);

        if (!sLine.IsEmpty() && plEditorUi::MeasureText(sCandidate, uiFontSize) > fWidth)
        {
          out_lines.PushBack(sLine);
          sLine = sWord;
        }
        else
        {
          sLine = sCandidate;
        }
      }

      out_lines.PushBack(sLine);
    }
  }

  plUiText* AddText(plUiElement* pParent, plStringView sText, float x, float y, float fWidth, float fHeight, plUInt32 uiFontSize, const plColorGammaUB& color, bool bCode = false)
  {
    plUiText* pText = plEditorUi::Text(pParent, sText);
    Place(pText, x, y, fWidth, fHeight);
    pText->m_uiFontSize = uiFontSize;
    pText->m_Color = color;
    if (bCode)
      pText->m_hFont = plEditorUi::GetCodeFont();
    return pText;
  }

  /// Wrapped lines; returns their height.
  float AddLines(plUiElement* pParent, plStringView sText, float x, float y, float fWidth, plUInt32 uiFontSize, const plColorGammaUB& color)
  {
    plHybridArray<plString, 8> lines;
    WrapLines(sText, uiFontSize, fWidth, lines);

    for (plUInt32 i = 0; i < lines.GetCount(); ++i)
      AddText(pParent, lines[i], x, y + i * s_fLine, fWidth, s_fLine, uiFontSize, color);

    return lines.GetCount() * s_fLine;
  }

  /// A borderless button: rows and tooltips over text.
  plUiButton* AddGhostButton(plUiElement* pParent, plStringView sName, float x, float y, float fWidth, float fHeight, plColorGammaUB hover = plColorGammaUB(0, 0, 0, 0))
  {
    plUiButton* pButton = plEditorUi::Button(pParent, {}, sName);
    pButton->ClearChildren();
    Place(pButton, x, y, fWidth, fHeight);
    pButton->m_Color = plColorGammaUB(0, 0, 0, 0);
    pButton->m_HoverColor = hover;
    pButton->m_PressedColor = hover;
    pButton->m_hStyle = plEditorUi::GetGhostStyle();
    return pButton;
  }

  float TextButtonWidth(plStringView sLabel)
  {
    return plMath::Ceil(plEditorUi::MeasureText(sLabel, s_uiLabelSize)) + 20.0f;
  }

  /// A text button; a disabled one sinks into the field colour with dim text and ignores the pointer.
  plUiButton* AddTextButton(plUiElement* pParent, plStringView sLabel, plStringView sName, float x, float y, float fHeight, bool bEnabled)
  {
    plUiButton* pButton = plEditorUi::TextButton(pParent, sLabel, sName);
    Place(pButton, x, y, TextButtonWidth(sLabel), fHeight);

    if (!bEnabled)
    {
      pButton->m_sDisabledBinding = s_szDisabled;
      static_cast<plUiText*>(pButton->m_Children[0])->m_Color = plEditorUi::Dim();
    }

    return pButton;
  }

  /// A dropdown: a field well with the choice and a chevron.
  plUiButton* AddDropdown(plUiElement* pParent, plStringView sName, plStringView sValue, float x, float y, float fWidth, float fHeight, bool bEnabled)
  {
    using namespace plEditorUi;

    plUiButton* pButton = Button(pParent, {}, sName);
    pButton->ClearChildren();
    Place(pButton, x, y, fWidth, fHeight);
    pButton->m_Color = Field();
    pButton->m_HoverColor = Field();
    pButton->m_PressedColor = Field();
    pButton->m_DisabledColor = Field();
    pButton->m_fBorderWidth = 1.0f;
    pButton->m_BorderColor = Line();
    pButton->m_vCornerRadius = plVec4(4);

    if (!bEnabled)
      pButton->m_sDisabledBinding = s_szDisabled;

    AddText(pButton, sValue, 8.0f, 0.0f, fWidth - 8.0f - 22.0f, fHeight, s_uiLabelSize, bEnabled ? Foreground() : Dim());

    plUiImage* pChevron = SvgImage(pButton, IconPath(Icon::Expand));
    Place(pChevron, fWidth - 20.0f, (fHeight - 14.0f) * 0.5f, 14.0f, 14.0f);
    pChevron->m_Color = Dim();
    return pButton;
  }

  float DropdownWidth(plStringView sValue)
  {
    return plMath::Ceil(plEditorUi::MeasureText(sValue, s_uiLabelSize)) + 36.0f;
  }

  void AddDot(plUiElement* pParent, float x, float fCenterY, const plColorGammaUB& color)
  {
    plUiPanel* pDot = plEditorUi::Panel(pParent);
    Place(pDot, x, fCenterY - 3.0f, 6.0f, 6.0f);
    pDot->m_Color = color;
    pDot->m_vCornerRadius = plVec4(3);
  }

  void AddLine(plUiElement* pParent, float x, float y, float fWidth, const plColorGammaUB& color)
  {
    plUiPanel* pLine = plEditorUi::Panel(pParent);
    Place(pLine, x, y, fWidth, 1.0f);
    pLine->m_Color = color;
  }

  /// A check box (style pass D9) at a position: a field well, or the selection fill with an accent edge and tick.
  void AddCheckBox(plUiElement* pParent, float x, float y, bool bOn)
  {
    using namespace plEditorUi;

    plUiPanel* pBox = Panel(pParent);
    Place(pBox, x, y, 14.0f, 14.0f);
    pBox->m_Color = bOn ? Selection() : Field();
    pBox->m_fBorderWidth = 1.0f;
    pBox->m_BorderColor = bOn ? Mix(Selection(), Accent(), 0.55f) : Border();
    pBox->m_vCornerRadius = plVec4(3);

    if (bOn)
    {
      plUiImage* pTick = SvgImage(pBox, IconPath(Icon::Check));
      Fill(pTick, plVec4(1));
      pTick->m_Color = Accent();
    }
  }

  /// A table header cell: uppercase, dim, right-aligned for number columns.
  void AddColumnTitle(plUiElement* pParent, plStringView sTitle, float x, float fWidth, bool bRight)
  {
    plStringBuilder sUpper = sTitle;
    sUpper.ToUpper();
    plUiText* pText = AddText(pParent, sUpper, x + 8.0f, 0.0f, fWidth - 16.0f, s_fTableHeader, s_uiTagSize, plEditorUi::Dim());
    if (bRight)
      pText->m_HAlign = plUiTextHAlign::Right;
  }
} // namespace

plEngineUsdLiveView::plEngineUsdLiveView(plUsdLivePanel* pPanel)
  : plEditorUiContent("Usd.LivePanel")
  , m_pPanel(pPanel)
  , m_Tabs(*GetSurface(), "Usd.Tabs")
{
  m_Tabs.m_CurrentChanged.AddEventHandler([this](plUInt32 uiTab)
    {
      // installations and connectors are looked for when the tab is opened; Blender is asked in the background
      if (uiTab == Apps)
      {
        if (plUsdApps* pApps = plUsdApps::GetSingleton())
          pApps->RefreshIfOlder(plTime::MakeFromSeconds(10));
      }

      m_bRebuild = true;
    });

  UpdateTabs();

  // the panel refreshes the content from now on
  m_pPanel->SetView(this);
}

plEngineUsdLiveView::~plEngineUsdLiveView()
{
  m_pPanel->SetView(nullptr);
}

void plEngineUsdLiveView::SetCurrentTab(Tab tab)
{
  m_Tabs.SetCurrent(tab);
  m_bRebuild = true;
}

plColorGammaUB plEngineUsdLiveView::ToneColor(Tone tone)
{
  // state dots and texts in the theme colours (D7): live in the success green, attention in the warning colour
  switch (tone)
  {
    case Tone::Good:
      return plEditorUi::Axis(1);
    case Tone::Attention:
      return plEditorUi::Warning();
    default:
      return plEditorUi::Dim();
  }
}

// Gathering.

void plEngineUsdLiveView::Refresh(plDocument* pDocument, plUsdLiveLink* pLink, const plDynamicArray<plUsdLiveLink*>& links, const plDynamicArray<plUsdLayerInfo>& layers)
{
  GatherHeader(pDocument, pLink, links);
  GatherToolbar(pDocument, pLink);
  GatherLayers(pLink, layers);
  GatherParticipants(pLink, layers);
  GatherApps(pLink);
  GatherStatus(pLink);

  UpdateTabs();

  // the document is rebuilt only when what it shows changed
  const plString sState = DescribeState();
  if (sState != m_sBuiltState)
  {
    m_sBuiltState = sState;
    m_bRebuild = true;
  }
}

void plEngineUsdLiveView::GatherHeader(plDocument* pDocument, plUsdLiveLink* pLink, const plDynamicArray<plUsdLiveLink*>& links)
{
  m_bHasDocument = pDocument != nullptr;
  m_bHasLink = pDocument != nullptr && pLink != nullptr;

  // the picker only appears when there is something to pick
  m_LinkNames.Clear();
  for (plUsdLiveLink* pOther : links)
    m_LinkNames.PushBack(pOther->GetSettings().m_sStageFile.GetFileName());
  m_iShownLink = links.IndexOf(pLink) == plInvalidIndex ? -1 : static_cast<plInt32>(links.IndexOf(pLink));

  m_bShowState = false;

  if (!m_bHasLink)
  {
    m_sStage = pDocument ? "No USD stage in this scene" : "No scene";
    m_sStagePath.Clear();
    m_sDetail = pDocument ? "Link one with the first button in the toolbar." : "Open a scene to link USD stages into it.";
    return;
  }

  const plUsdLiveLink::Settings& settings = pLink->GetSettings();
  m_sStage = settings.m_sStageFile.GetFileName();
  m_sStagePath = settings.m_sStageFile;

  plStringBuilder sDetail;
  if (pLink->IsWritable())
    sDetail.SetFormat("session '{}'{}writing {}", settings.m_sSessionName, Sep, pLink->GetParticipantLayer().GetFileNameAndExtension());
  else
    sDetail.SetFormat("read-only{}{}", Sep, settings.m_sStageFile);
  m_sDetail = sDetail;

  m_bShowState = true;
  if (pLink->IsWritable())
  {
    plHybridArray<plUsdLayerInfo, 16> layers;
    pLink->GetLayers(layers);

    plStringBuilder sFolder = pLink->GetParticipantLayer();
    sFolder.PathParentDirectory();

    plUInt32 uiParticipants = 0;
    for (const plUsdLayerInfo& layer : layers)
    {
      if (plUsdLiveText::IsInFolder(layer.m_sRealPath, sFolder) && layer.m_sRealPath.GetFileNameAndExtension() != "session.usda")
        ++uiParticipants;
    }

    plStringBuilder sState;
    sState.SetFormat("Files{}{} participant{}", Sep, uiParticipants, uiParticipants == 1 ? "" : "s");
    m_sState = sState;
    m_StateTone = Tone::Good;
  }
  else
  {
    m_sState = "Read-only";
    m_StateTone = Tone::Attention;
  }
}

void plEngineUsdLiveView::GatherToolbar(plDocument* pDocument, plUsdLiveLink* pLink)
{
  m_Actions[Link] = {pDocument != nullptr, "Link a USD stage to this scene"};

  m_Actions[Session].m_bEnabled = pLink != nullptr && !pLink->IsWritable();
  if (pLink && pLink->IsWritable())
  {
    plStringBuilder sTip;
    sTip.SetFormat("Session '{}' is running", pLink->GetSettings().m_sSessionName);
    m_Actions[Session].m_sTooltip = sTip;
  }
  else
  {
    m_Actions[Session].m_sTooltip = "Start or join a session, so your edits are written into a layer of your own";
  }

  m_Actions[Reload] = {pLink != nullptr, "Reload every layer and resend meshes and materials"};
  m_Actions[Unlink] = {pLink != nullptr, "Unlink this stage and remove its generated objects"};
  m_Actions[OpenIn] = {pLink != nullptr, "Open this stage in another tool, in a live session"};
  m_Actions[Bake] = {pLink != nullptr, "Bake now: rebuild every mesh and material and write the bake prefab"};
  m_Actions[Partition] = {true, "Import a USD stage as the cells of a world partition"};

  const bool bOwner = pLink != nullptr && pLink->IsSessionOwner();
  m_Actions[Commit].m_bEnabled = bOwner;
  if (pLink == nullptr || !pLink->IsWritable())
    m_Actions[Commit].m_sTooltip = "Commit a session into the asset. Start a session first.";
  else if (bOwner)
    m_Actions[Commit].m_sTooltip = "Commit the session: fold every participant's changes into the asset";
  else
    m_Actions[Commit].m_sTooltip = "Only the session's owner, the tool whose layer is strongest, can commit it";

  m_Actions[Save].m_bEnabled = pLink != nullptr && pLink->HasUnsavedChanges();
  if (pLink && pLink->IsWritable())
  {
    plStringBuilder sTip;
    sTip.SetFormat("Save {} now", pLink->GetParticipantLayer().GetFileNameAndExtension());
    m_Actions[Save].m_sTooltip = sTip;
  }
  else
  {
    m_Actions[Save].m_sTooltip = "Save your layer now";
  }

  bool bCanRevert = false;
  if (pLink && pLink->IsWritable())
  {
    for (const plDocumentObject* pObject : pLink->GetDocument()->GetSelectionManager()->GetSelection())
    {
      if (pLink->HasOpinions(pObject->GetGuid()))
      {
        bCanRevert = true;
        break;
      }
    }
  }
  m_Actions[Revert] = {bCanRevert, "Revert your opinions on the selected objects"};
}

void plEngineUsdLiveView::GatherLayers(plUsdLiveLink* pLink, const plDynamicArray<plUsdLayerInfo>& layers)
{
  m_Layers.Clear();

  if (pLink == nullptr)
  {
    m_sLayersNote = "Link a stage to see its layers.";
    return;
  }

  plStringBuilder sSessionFolder = pLink->GetParticipantLayer();
  sSessionFolder.PathParentDirectory();

  for (const plUsdLayerInfo& layer : layers)
  {
    const bool bInSession = !sSessionFolder.IsEmpty() && plUsdLiveText::IsInFolder(layer.m_sRealPath, sSessionFolder);

    // the session root only composes the layers; it holds no opinions of its own
    if (bInSession && layer.m_sRealPath.GetFileNameAndExtension() == "session.usda")
      continue;

    LayerRow& row = m_Layers.ExpandAndGetRef();
    row.m_sFile = layer.m_sRealPath.IsEmpty() ? plStringView(layer.m_sIdentifier) : layer.m_sRealPath.GetFileNameAndExtension();
    row.m_sPath = layer.m_sRealPath.IsEmpty() ? plStringView(layer.m_sIdentifier) : plStringView(layer.m_sRealPath);
    row.m_sIdentifier = layer.m_sIdentifier;
    row.m_bEditTarget = layer.m_bIsEditTarget;
    row.m_bMuted = layer.m_bMuted;

    plStringBuilder sOpinions;
    if (bInSession)
      sOpinions.SetFormat("{}", pLink->CountLayerOpinions(layer.m_sIdentifier));
    else
      sOpinions = "-";
    row.m_sOpinions = sOpinions;

    plStringBuilder sState;
    if (layer.m_bIsEditTarget)
      sState.SetFormat("your layer{}{}", Sep, layer.m_bDirty ? plString("unsaved") : plString(plStringBuilder("saved ", plUsdLiveText::FormatAge(layer.m_ModificationTime))));
    else if (layer.m_bMuted)
      sState = "muted in this editor";
    else if (bInSession)
      sState.SetFormat("saved {}", plUsdLiveText::FormatAge(layer.m_ModificationTime));
    else
      sState = "asset, read-only";
    row.m_sState = sState;

    // other participants' layers can be muted here; the asset, the session root and this editor's own layer cannot
    row.m_bMutable = bInSession && !layer.m_bIsEditTarget;
  }

  const plUsdBasis& basis = pLink->GetBasis();
  plStringBuilder sNote;
  if (!pLink->IsWritable())
    sNote.Append("Read-only. Start a session to get a layer of your own; your edits then go into it and every other tool sees them.\n");

  sNote.AppendFormat("Stage: upAxis {}, {} m per unit, mapped to Plasma as {} (a mirror, so winding is flipped).", basis.GetUpAxis() == plUsdBasis::UpAxis::Y ? "Y" : "Z",
    basis.GetMetersPerUnit(), basis.GetUpAxis() == plUsdBasis::UpAxis::Y ? "(x, z, y)" : "(x, -y, z)");
  m_sLayersNote = sNote;
}

void plEngineUsdLiveView::GatherParticipants(plUsdLiveLink* pLink, const plDynamicArray<plUsdLayerInfo>& layers)
{
  m_Participants.Clear();
  m_bSession = pLink != nullptr && pLink->IsWritable();

  if (!m_bSession)
  {
    m_sParticipantsNote = pLink ? "No session. Start one to share this stage with other tools; each tool then writes a layer of its own and appears here." : "Link a stage first.";
    return;
  }

  plStringBuilder sSessionFolder = pLink->GetParticipantLayer();
  sSessionFolder.PathParentDirectory();

  plHybridArray<plUsdLiveHub::Peer, 4> peers;
  plUsdLiveManager::GetSingleton()->GetHub()->GetPeers(pLink->GetSessionRoot(), peers);

  for (const plUsdLayerInfo& layer : layers)
  {
    if (!plUsdLiveText::IsInFolder(layer.m_sRealPath, sSessionFolder) || layer.m_sRealPath.GetFileNameAndExtension() == "session.usda")
      continue;

    ParticipantRow& row = m_Participants.ExpandAndGetRef();

    plString sTool, sUser;
    plUsdLiveText::SplitParticipant(layer.m_sRealPath.GetFileNameAndExtension(), sTool, sUser);
    row.m_sTool = sTool;
    row.m_sUser = layer.m_bIsEditTarget ? plString(plStringBuilder(sUser, " (you)")) : sUser;
    row.m_sPath = layer.m_sRealPath;

    const plUsdLiveHub::Peer* pPeer = nullptr;
    for (const plUsdLiveHub::Peer& peer : peers)
    {
      if (plUsdLiveText::IsSameFile(peer.m_sLayer, layer.m_sRealPath))
        pPeer = &peer;
    }

    plStringBuilder sState;
    if (layer.m_bMuted)
    {
      row.m_Tone = Tone::Idle;
      sState = "muted in this editor";
    }
    else if (pPeer != nullptr)
    {
      // connected to the live hub: its changes arrive as it makes them
      row.m_Tone = Tone::Good;
      if (pPeer->m_fLatencyMs >= 0.0f)
        sState.SetFormat("live, {} ms", plArgF(pPeer->m_fLatencyMs, 1));
      else
        sState = "live";
    }
    else if (layer.m_bIsEditTarget && layer.m_bDirty)
    {
      row.m_Tone = Tone::Attention;
      sState = "editing, unsaved";
    }
    else
    {
      const bool bRecent = layer.m_ModificationTime.IsValid() && (plTimestamp::CurrentTimestamp() - layer.m_ModificationTime).GetSeconds() < 60.0;
      row.m_Tone = bRecent || layer.m_bIsEditTarget ? Tone::Good : Tone::Attention;
      sState.SetFormat("saved {}", plUsdLiveText::FormatAge(layer.m_ModificationTime));
    }
    row.m_sState = sState;

    plStringBuilder sOpinions;
    sOpinions.SetFormat("{}", pLink->CountLayerOpinions(layer.m_sIdentifier));
    row.m_sOpinions = sOpinions;

    row.m_bHighlighted = !pLink->GetHighlightedLayer().IsEmpty() && pLink->GetHighlightedLayer() == layer.m_sRealPath;
  }

  plStringBuilder sNote;
  sNote.SetFormat("Session folder {}. Other tools join by opening session.usda there. Click a participant to highlight the objects their layer decides.", sSessionFolder);
  m_sParticipantsNote = sNote;
}

void plEngineUsdLiveView::GatherApps(plUsdLiveLink* pLink)
{
  using State = plUsdApps::ConnectorState;
  using InstanceState = plUsdApps::InstanceState;

  plUsdApps* pApps = plUsdApps::GetSingleton();
  if (pApps == nullptr)
    return;

  // follows the launched instances, whichever tab is shown
  pApps->Update();

  m_Apps.Clear();

  plHybridArray<const plUsdApps::Instance*, 8> instances;
  pApps->GetInstances(pLink != nullptr && pLink->IsWritable() ? pLink->GetSessionRoot() : plStringView(), instances);

  for (plUInt32 i = 0; i < static_cast<plUInt32>(plUsdApps::App::Count); ++i)
  {
    const plUsdApps::App app = static_cast<plUsdApps::App>(i);
    AppRow& row = m_Apps.ExpandAndGetRef();
    row.m_App = app;
    row.m_sName = plUsdApps::GetName(app);

    const plDynamicArray<plUsdApps::Install>& installs = pApps->GetInstalls(app);
    const plUsdApps::Install* pInstall = pApps->GetSelectedInstall(app);
    const plStringView sBusy = pApps->GetBusyStep(app);
    const plStringView sPackaged = pApps->GetPackagedVersion(app);

    // installations, newest first, then an entry to locate another
    for (const plUsdApps::Install& install : installs)
    {
      row.m_VersionTexts.PushBack(install.m_sVersion.IsEmpty() ? plString("unknown version") : install.m_sVersion);
      row.m_VersionPaths.PushBack(install.m_sExecutable);

      if (pInstall != nullptr && install.m_sExecutable == pInstall->m_sExecutable)
        row.m_iSelectedVersion = static_cast<plInt32>(row.m_VersionTexts.GetCount()) - 1;
    }

    if (!installs.IsEmpty())
    {
      row.m_VersionTexts.PushBack("Locate another...");
      row.m_VersionPaths.PushBack(plString());
    }

    row.m_bVersionsEnabled = sBusy.IsEmpty();
    row.m_sExecutable = pInstall != nullptr ? pInstall->m_sExecutable : plString();

    // the connector
    const State state = pInstall != nullptr ? pInstall->m_Connector.m_State : State::NotInstalled;
    const bool bSupported = pInstall != nullptr && pInstall->m_sUnsupported.IsEmpty();
    {
      plStringBuilder sText;
      plStringBuilder sToolTip = pInstall != nullptr ? pInstall->m_Connector.m_sPath.GetView() : plStringView();
      Tone tone = Tone::Idle;

      const plStringView sVersion = pInstall != nullptr ? pInstall->m_Connector.m_sVersion.GetView() : plStringView();
      const plStringView sLocation = pInstall != nullptr ? pInstall->m_Connector.m_sLocation.GetView() : plStringView();
      const plStringView sNote = pInstall != nullptr ? pInstall->m_Connector.m_sNote.GetView() : plStringView();

      if (!sBusy.IsEmpty())
        sText.SetFormat("Connector: {}...", sBusy);
      else if (pInstall == nullptr)
        sText = "Not found in the usual places. Locate it if it is installed elsewhere.";
      else if (!bSupported)
      {
        sText = pInstall->m_sUnsupported;
        tone = Tone::Attention;
      }
      else
      {
        switch (state)
        {
          case State::Checking:
            sText = "Checking the connector...";
            break;
          case State::Unknown:
            sText.SetFormat("The connector could not be checked. {}", sNote);
            tone = Tone::Attention;
            break;
          case State::NotInstalled:
            sText = sPackaged.IsEmpty() ? "Connector not installed, and this package has none for it" : "Connector not installed";
            if (!sToolTip.IsEmpty())
              sToolTip.Prepend("Install puts it in ");
            break;
          case State::Disabled:
            sText.SetFormat("Connector {}{}switched off in Blender's preferences", sVersion, Sep);
            tone = Tone::Attention;
            break;
          case State::Installed:
            sText.SetFormat("Connector {}{}{}", sVersion, Sep, sLocation);
            break;
          case State::Outdated:
            sText.SetFormat("Connector {}{}this package has {}", sVersion.IsEmpty() ? plStringView("without a version") : sVersion, Sep, sPackaged);
            tone = Tone::Attention;
            break;
          case State::Linked:
            sText.SetFormat("Connector {}{}{}", sVersion.IsEmpty() ? plStringView("of unknown version") : sVersion, Sep, sLocation);
            if (!sNote.IsEmpty())
              sText.AppendFormat("{}{}", Sep, sNote);
            sToolTip.Prepend("Installed some other way, e.g. by the connector's install.py; the Apps tab leaves it alone.\n");
            break;
        }
      }

      row.m_sConnector = sText;
      row.m_sConnectorTooltip = sToolTip;
      row.m_ConnectorTone = tone;
    }

    // Install, Remove and Locate
    {
      const bool bIdle = sBusy.IsEmpty();
      plStringBuilder sInstall = row.m_sName;
      if (pInstall != nullptr)
        sInstall.AppendFormat(" {}", pInstall->m_sVersion);

      row.m_bShowInstall = bSupported && !sPackaged.IsEmpty() && (state == State::NotInstalled || state == State::Outdated || state == State::Disabled);
      row.m_bInstallEnabled = bIdle;
      row.m_sInstallLabel = state == State::Outdated ? "Update" : state == State::Disabled ? "Switch On" : "Install";

      plStringBuilder sTip;
      sTip.SetFormat("Copy the connector {} into {}'s own preferences", sPackaged, sInstall);
      row.m_sInstallTooltip = sTip;

      row.m_bShowRemove = bSupported && (state == State::Installed || state == State::Outdated || state == State::Disabled);
      row.m_bRemoveEnabled = bIdle;
      sTip.SetFormat("Delete the connector the Apps tab installed into {}", sInstall);
      row.m_sRemoveTooltip = sTip;

      row.m_bShowLocate = installs.IsEmpty();
      sTip.SetFormat("Pick {}'s executable, for an installation that was not found", row.m_sName);
      row.m_sLocateTooltip = sTip;
    }

    // Open
    {
      const plString sBlocker = pApps->GetOpenBlocker(app, pLink);
      row.m_bOpenEnabled = sBlocker.IsEmpty();

      plStringBuilder sToolTip = sBlocker;
      if (sToolTip.IsEmpty() && pInstall != nullptr && pLink != nullptr)
      {
        plStringBuilder sTarget;
        sTarget.SetFormat("{} {}", row.m_sName, pInstall->m_sVersion);

        if (pLink->IsWritable())
          sToolTip.SetFormat("Launch {} into session '{}'", sTarget, pLink->GetSettings().m_sSessionName);
        else
          sToolTip.SetFormat("Start session '{}' and launch {} into it", plUsdApps::DefaultSession, sTarget);
      }
      row.m_sOpenTooltip = sToolTip;
    }

    // what the last operation reported
    {
      const plStringView sProblem = pApps->GetProblem(app);
      row.m_Message.m_sText = sProblem.IsEmpty() ? pApps->GetNotice(app) : sProblem;
      row.m_Message.m_Tone = sProblem.IsEmpty() ? Tone::Idle : Tone::Attention;
    }

    // the instances launched into this link's session
    for (const plUsdApps::Instance* pInstance : instances)
    {
      if (pInstance->m_App != app)
        continue;

      plStringBuilder sTool;
      sTool.SetFormat("{} {}", row.m_sName, pInstance->m_sVersion);

      AppLine& line = row.m_Instances.ExpandAndGetRef();
      plStringBuilder sLine;
      switch (pInstance->m_State)
      {
        case InstanceState::Live:
          line.m_Tone = Tone::Good;
          sLine.SetFormat("{}{}live", pInstance->m_sParticipant, Sep);
          if (pInstance->m_fLatencyMs >= 0.0f)
            sLine.AppendFormat(", {} ms", plArgF(pInstance->m_fLatencyMs, 0));
          break;
        case InstanceState::Starting:
          line.m_Tone = Tone::Idle;
          sLine.SetFormat("{}{}starting", sTool, Sep);
          break;
        case InstanceState::NotConnected:
          line.m_Tone = Tone::Attention;
          sLine.SetFormat("{}{}running, but not in the session: is its connector installed and switched on?", sTool, Sep);
          break;
        case InstanceState::Left:
          line.m_Tone = Tone::Idle;
          sLine.SetFormat("{}{}left the session, still running", pInstance->m_sParticipant, Sep);
          break;
      }
      line.m_sText = sLine;
    }
  }

  if (pApps->GetConnectorFolder().IsEmpty())
  {
    m_sAppsNote = "This installation of plasma.usd has no connectors. Build the package, or link it to its source, to get them.";
  }
  else
  {
    plStringBuilder sNote;
    sNote.SetFormat("Install copies a connector into the tool's own preferences; a tool that is running loads it when it starts next. Open launches the tool into "
                    "this link's session, starting one when there is none. Connectors from {}.",
      pApps->GetConnectorFolder());
    m_sAppsNote = sNote;
  }
}

void plEngineUsdLiveView::GatherStatus(plUsdLiveLink* pLink)
{
  const plStringView sBackground = plUsdLiveManager::GetSingleton()->GetBackgroundBakeStatus();

  if (pLink == nullptr)
  {
    m_sStatusLeft = sBackground;
    m_sStatusRight.Clear();
    return;
  }

  plStringBuilder s;
  s.SetFormat("Objects {}", pLink->GetPrimCount());

  if (pLink->IsWritable())
    s.AppendFormat("{}your opinions {}", Sep, pLink->CountLayerOpinions(pLink->GetParticipantLayer()));

  // Include streaming progress as well as errors; the object count alone does not show visibility.
  const plStringView sStatus = pLink->GetStatus();
  if (!sStatus.IsEmpty() && !sStatus.StartsWith("Live") && !sStatus.StartsWith("Read-only"))
  {
    s.AppendFormat("{}{}", Sep, sStatus);
  }

  if (!sBackground.IsEmpty())
    s.AppendFormat("{}{}", Sep, sBackground);

  m_sStatusLeft = s;

  plStringBuilder sBake;
  if (!pLink->GetBakeStatus().IsEmpty())
    sBake.SetFormat("bake: {}", pLink->GetBakeStatus());
  m_sStatusRight = sBake;
}

plString plEngineUsdLiveView::DescribeState() const
{
  plStringBuilder s;
  auto add = [&](plStringView sPart)
  {
    s.Append(sPart);
    s.Append("\x1F");
  };
  auto flag = [&](bool b)
  { add(b ? "1" : "0"); };

  flag(m_bHasDocument);
  flag(m_bHasLink);
  add(m_sStage);
  add(m_sStagePath);
  add(m_sDetail);
  for (const plString& sName : m_LinkNames)
    add(sName);
  s.AppendFormat("{}\x1F", m_iShownLink);
  flag(m_bShowState);
  add(m_sState);
  s.AppendFormat("{}\x1F", static_cast<int>(m_StateTone));

  for (const ActionState& action : m_Actions)
  {
    flag(action.m_bEnabled);
    add(action.m_sTooltip);
  }

  for (const LayerRow& row : m_Layers)
  {
    add(row.m_sFile);
    add(row.m_sPath);
    add(row.m_sOpinions);
    add(row.m_sState);
    flag(row.m_bEditTarget);
    flag(row.m_bMutable);
    flag(row.m_bMuted);
  }
  add(m_sLayersNote);

  flag(m_bSession);
  for (const ParticipantRow& row : m_Participants)
  {
    add(row.m_sTool);
    add(row.m_sUser);
    add(row.m_sState);
    add(row.m_sOpinions);
    s.AppendFormat("{}\x1F", static_cast<int>(row.m_Tone));
    flag(row.m_bHighlighted);
  }
  add(m_sParticipantsNote);

  // the Apps tab only counts while it is shown
  if (GetCurrentTab() == Apps)
  {
    for (const AppRow& row : m_Apps)
    {
      for (const plString& sVersion : row.m_VersionTexts)
        add(sVersion);
      s.AppendFormat("{}\x1F", row.m_iSelectedVersion);
      flag(row.m_bVersionsEnabled);
      add(row.m_sExecutable);
      add(row.m_sConnector);
      add(row.m_sConnectorTooltip);
      s.AppendFormat("{}\x1F", static_cast<int>(row.m_ConnectorTone));
      flag(row.m_bShowInstall);
      flag(row.m_bInstallEnabled);
      add(row.m_sInstallLabel);
      add(row.m_sInstallTooltip);
      flag(row.m_bShowRemove);
      flag(row.m_bRemoveEnabled);
      flag(row.m_bShowLocate);
      flag(row.m_bOpenEnabled);
      add(row.m_sOpenTooltip);
      add(row.m_Message.m_sText);
      for (const AppLine& line : row.m_Instances)
        add(line.m_sText);
    }
    add(m_sAppsNote);
  }

  add(m_sStatusLeft);
  add(m_sStatusRight);
  return s;
}

void plEngineUsdLiveView::UpdateTabs()
{
  plStringBuilder sLayers, sParticipants, sApps;
  sLayers.SetFormat("{}", m_Layers.GetCount());
  sParticipants.SetFormat("{}", m_Participants.GetCount());
  sApps.SetFormat("{}", static_cast<plUInt32>(plUsdApps::App::Count));

  plEditorUiTab tabs[3];
  tabs[0].m_sLabel = "Layers";
  tabs[0].m_sCount = m_Layers.IsEmpty() ? plString() : plString(sLayers);
  tabs[1].m_sLabel = "Participants";
  tabs[1].m_sCount = m_Participants.IsEmpty() ? plString() : plString(sParticipants);
  tabs[2].m_sLabel = "Apps";
  tabs[2].m_sCount = sApps;

  const auto current = m_Tabs.GetTabs();
  bool bSame = current.GetCount() == 3;
  for (plUInt32 i = 0; bSame && i < 3; ++i)
    bSame = current[i].m_sCount == tabs[i].m_sCount;

  if (bSame)
    return;

  m_Tabs.SetTabs(tabs);
  m_bRebuild = true;
}

// Document.

void plEngineUsdLiveView::OnUiUpdate()
{
  if (m_bRebuild || GetSurface()->GetLogicalSize().x != m_fBuiltWidth)
    BuildDocument();
}

void plEngineUsdLiveView::BuildDocument()
{
  using namespace plEditorUi;

  m_bRebuild = false;
  m_fBuiltWidth = GetSurface()->GetLogicalSize().x;
  const float fWidth = plMath::Max(m_fBuiltWidth, 260.0f);

  auto& board = GetSurface()->GetBlackboard();
  board.SetEntryValue(s_szDisabled, true);

  plUiPanel* pRoot = Panel();
  pRoot->m_Color = plEditorUi::Surface();

  float y = BuildHeader(pRoot, fWidth);
  y = BuildToolbar(pRoot, y, fWidth);

  plUiPanel* pTabs = m_Tabs.Build(pRoot, fWidth - 12.0f);
  Place(pTabs, 6.0f, y, fWidth - 12.0f, plEditorUiTabs::Height);
  y += plEditorUiTabs::Height;
  AddLine(pRoot, 0.0f, y, fWidth, Line());
  y += 1.0f;

  // the current tab's content scrolls above the status row
  plUiScrollView* pScroll = PL_DEFAULT_NEW(plUiScrollView);
  pRoot->m_Children.PushBack(pScroll);
  Fill(pScroll, plVec4(0, y, 0, StatusRowHeight + 1.0f));
  pScroll->m_sName = "Usd.Body";
  pScroll->m_Color = plColorGammaUB(0, 0, 0, 0);
  pScroll->m_fSpacing = 0.0f;

  plUiPanel* pBody = Panel(pScroll);
  pBody->m_Color = plColorGammaUB(0, 0, 0, 0);

  float fBody = 0.0f;
  switch (GetCurrentTab())
  {
    case Layers:
      fBody = BuildLayers(pBody, fWidth - 6.0f);
      break;
    case Participants:
      fBody = BuildParticipants(pBody, fWidth - 6.0f);
      break;
    default:
      fBody = BuildApps(pBody, fWidth - 6.0f);
      break;
  }
  pBody->m_vOffsets.w = fBody;

  StatusRow(pRoot, "Usd.StatusLeft", "Usd.StatusRight");
  board.SetEntryValue("Usd.StatusLeft", m_sStatusLeft);
  board.SetEntryValue("Usd.StatusRight", m_sStatusRight);

  SetDocument(Finish(pRoot));
}

float plEngineUsdLiveView::BuildHeader(plUiElement* pRoot, float fWidth)
{
  using namespace plEditorUi;

  auto& board = GetSurface()->GetBlackboard();

  // the stage icon in the prefabs colour
  plUiImage* pIcon = SvgImage(pRoot, LineIcon("UsdLive"));
  Place(pIcon, s_fSide, (s_fHeaderHeight - 26.0f) * 0.5f, 26.0f, 26.0f);
  pIcon->m_Color = plColorScheme::GetCategoryColor("Prefabs", plColorScheme::CategoryColorUsage::BorderColor);

  // right to left: the state, then the link picker when the scene has several links
  float fRight = fWidth - s_fSide;

  if (m_bShowState)
  {
    const float fText = plMath::Ceil(MeasureText(m_sState, s_uiLabelSize)) + 2.0f;
    const plColorGammaUB color = Mix(ToneColor(m_StateTone), Foreground(), 0.18f);
    AddText(pRoot, m_sState, fRight - fText, 0.0f, fText, s_fHeaderHeight, s_uiLabelSize, color);
    AddDot(pRoot, fRight - fText - 11.0f, s_fHeaderHeight * 0.5f, ToneColor(m_StateTone));
    fRight -= fText + 11.0f + 12.0f;
  }

  if (m_LinkNames.GetCount() > 1)
  {
    const plStringView sShown = m_iShownLink >= 0 ? m_LinkNames[m_iShownLink].GetView() : plStringView();
    const float fPicker = plMath::Min(DropdownWidth(sShown), 180.0f);
    AddDropdown(pRoot, "Usd.picker", sShown, fRight - fPicker, (s_fHeaderHeight - ControlHeight) * 0.5f, fPicker, ControlHeight, true);
    SetToolTip(board, "Usd.picker", "The scene has several USD links; pick the one to show.");
    fRight -= fPicker + 12.0f;
  }

  // the stage name and how this editor takes part
  const float fTitleX = s_fSide + 26.0f + 12.0f;
  const float fTitleWidth = plMath::Max(fRight - fTitleX, 40.0f);

  AddText(pRoot, m_sStage, fTitleX, 7.0f, fTitleWidth, 18.0f, FontSize, Foreground());
  AddText(pRoot, m_sDetail, fTitleX, 25.0f, fTitleWidth, 16.0f, s_uiNumberSize, Dim(), m_bHasLink);

  plUiButton* pStageTip = AddGhostButton(pRoot, "Usd.stage", fTitleX, 7.0f, fTitleWidth, 18.0f);
  PL_IGNORE_UNUSED(pStageTip);
  SetToolTip(board, "Usd.stage", m_sStagePath);

  AddGhostButton(pRoot, "Usd.detail", fTitleX, 25.0f, fTitleWidth, 16.0f);
  SetToolTip(board, "Usd.detail", m_sDetail);

  AddLine(pRoot, 0.0f, s_fHeaderHeight, fWidth, Line());
  return s_fHeaderHeight + 1.0f;
}

float plEngineUsdLiveView::BuildToolbar(plUiElement* pRoot, float y, float fWidth)
{
  using namespace plEditorUi;

  auto& board = GetSurface()->GetBlackboard();

  struct ToolButton
  {
    Action m_Action;
    const char* m_szName;
    const char* m_szLineIcon; ///< a package line icon, or nullptr for m_ThemeIcon
    Icon m_ThemeIcon;
    bool m_bSeparatorBefore;
  };

  const ToolButton buttons[] = {
    {Link, "Usd.link", "Link", Icon::Open, false},
    {Session, "Usd.session", "Session", Icon::Play, false},
    {Reload, "Usd.reload", "Reload", Icon::Reset, false},
    {Save, "Usd.save", nullptr, Icon::Save, false},
    {Revert, "Usd.revert", nullptr, Icon::Reset, false},
    {Unlink, "Usd.unlink", nullptr, Icon::Clear, false},
    {OpenIn, "Usd.openin", nullptr, Icon::External, true},
    {Bake, "Usd.bake", "Bake", Icon::Object, false},
    {Commit, "Usd.commit", nullptr, Icon::Check, false},
    {Partition, "Usd.partition", "Partition", Icon::Grid, true},
  };

  float x = 6.0f;
  const float fTop = y + 3.0f;

  for (const ToolButton& button : buttons)
  {
    if (button.m_bSeparatorBefore)
    {
      plUiPanel* pSeparator = Panel(pRoot);
      Place(pSeparator, x + 4.0f, fTop + 4.0f, 1.0f, s_fIconButton - 8.0f);
      pSeparator->m_Color = Line();
      x += 9.0f;
    }

    const ActionState& state = m_Actions[button.m_Action];

    plUiButton* pButton = Button(pRoot, {}, button.m_szName);
    pButton->ClearChildren();
    Place(pButton, x, fTop, s_fIconButton, s_fIconButton);
    pButton->m_Color = plColorGammaUB(0, 0, 0, 0);
    pButton->m_vCornerRadius = plVec4(4);
    pButton->m_hStyle = GetGhostStyle();

    plUiImage* pIcon = button.m_szLineIcon != nullptr ? ButtonIcon(pButton, LineIcon(button.m_szLineIcon)) : ButtonIcon(pButton, button.m_ThemeIcon);
    plColorGammaUB color = Muted();

    // disabled: no plate, a faded icon (ghost style)
    if (!state.m_bEnabled)
    {
      pButton->m_sDisabledBinding = s_szDisabled;
      color.a = 90;
    }
    pIcon->m_Color = color;

    // Open in opens a menu: a small caret in the corner
    if (button.m_Action == OpenIn)
    {
      plUiText* pCaret = AddText(pButton, plEditorUi::TreeExpanded, s_fIconButton - 9.0f, s_fIconButton - 12.0f, 9.0f, 12.0f, s_uiTagSize, color);
      pCaret->m_Overflow = plUiTextOverflow::Overflow;
    }

    SetToolTip(board, button.m_szName, state.m_sTooltip);
    x += s_fIconButton + 2.0f;
  }

  AddLine(pRoot, 0.0f, y + s_fToolbarHeight, fWidth, Line());
  return y + s_fToolbarHeight + 1.0f;
}

float plEngineUsdLiveView::BuildEmpty(plUiElement* pBody, float fWidth, plStringView sLine, bool bOfferLink)
{
  using namespace plEditorUi;

  // an empty state: an icon, the line, and the action that fills the tab
  float y = 26.0f;

  plUiImage* pIcon = SvgImage(pBody, LineIcon("UsdLive"));
  Place(pIcon, plMath::Round((fWidth - 22.0f) * 0.5f), y, 22.0f, 22.0f);
  pIcon->m_Color = Dim();
  y += 22.0f + 8.0f;

  plHybridArray<plString, 4> lines;
  WrapLines(sLine, FontSize, fWidth - 40.0f, lines);
  for (const plString& sText : lines)
  {
    plUiText* pText = AddText(pBody, sText, 20.0f, y, fWidth - 40.0f, 18.0f, FontSize, Muted());
    pText->m_HAlign = plUiTextHAlign::Center;
    y += 18.0f;
  }

  if (bOfferLink)
  {
    const plStringView sLabel = m_bHasLink ? plStringView("Start or Join a Session...") : plStringView("Link a USD Stage...");
    y += 8.0f;
    AddTextButton(pBody, sLabel, "Usd.emptyaction", plMath::Round((fWidth - TextButtonWidth(sLabel)) * 0.5f), y, ControlHeight, true);
    y += ControlHeight;
  }

  return y + 26.0f;
}

float plEngineUsdLiveView::BuildNote(plUiElement* pBody, plStringView sText, float y, float fWidth)
{
  return 8.0f + AddLines(pBody, sText, s_fSide, y + 8.0f, fWidth - 2 * s_fSide, s_uiNumberSize, plEditorUi::Dim()) + 8.0f;
}

float plEngineUsdLiveView::BuildLayers(plUiElement* pBody, float fWidth)
{
  using namespace plEditorUi;

  if (!m_bHasLink)
    return BuildEmpty(pBody, fWidth, m_sLayersNote, m_bHasDocument);

  auto& board = GetSurface()->GetBlackboard();

  // #, Layer, Opinions, State
  const float fNumber = 36.0f;
  const float fOpinions = 76.0f;
  const float fState = plMath::Max(140.0f, (fWidth - fNumber - fOpinions) * 0.4f);
  const float fLayer = plMath::Max(fWidth - fNumber - fOpinions - fState, 60.0f);
  const float xLayer = fNumber;
  const float xOpinions = xLayer + fLayer;
  const float xState = xOpinions + fOpinions;

  AddColumnTitle(pBody, "#", 0.0f, fNumber, true);
  AddColumnTitle(pBody, "Layer", xLayer, fLayer, false);
  AddColumnTitle(pBody, "Opinions", xOpinions, fOpinions, true);
  AddColumnTitle(pBody, "State", xState, fState, false);
  AddLine(pBody, 0.0f, s_fTableHeader - 1.0f, fWidth, Line());

  float y = s_fTableHeader;

  for (plUInt32 i = 0; i < m_Layers.GetCount(); ++i)
  {
    const LayerRow& row = m_Layers[i];

    const plString sRow = Name("layer", i);
    AddGhostButton(pBody, sRow, 0.0f, y, fWidth, RowHeight, Hover());
    plStringBuilder sTip(row.m_sPath, "\nStrongest first. Untick a participant's layer to mute it in this editor only.");
    SetToolTip(board, sRow, sTip);

    plStringBuilder sNumber;
    sNumber.SetFormat("{}", i + 1);
    plUiText* pNumber = AddText(pBody, sNumber, 8.0f, y, fNumber - 16.0f, RowHeight, s_uiNumberSize, Muted(), true);
    pNumber->m_HAlign = plUiTextHAlign::Right;

    float xFile = xLayer + 8.0f;
    if (row.m_bMutable)
    {
      // ticked: the layer counts; unticked: muted in this editor
      const plString sMute = Name("mute", i);
      plUiButton* pMute = AddGhostButton(pBody, sMute, xFile - 2.0f, y + 2.0f, 18.0f, RowHeight - 4.0f);
      AddCheckBox(pMute, 2.0f, (RowHeight - 4.0f - 14.0f) * 0.5f, !row.m_bMuted);
      SetToolTip(board, sMute, "Mute this layer in this editor only");
      xFile += 14.0f + 8.0f;
    }

    AddText(pBody, row.m_sFile, xFile, y, xOpinions - xFile - 4.0f, RowHeight, FontSize, Foreground());

    plUiText* pOpinions = AddText(pBody, row.m_sOpinions, xOpinions + 8.0f, y, fOpinions - 16.0f, RowHeight, s_uiNumberSize, Muted(), true);
    pOpinions->m_HAlign = plUiTextHAlign::Right;

    const bool bUnsaved = row.m_sState.FindSubString("unsaved") != nullptr;
    AddText(pBody, row.m_sState, xState + 8.0f, y, fState - 16.0f, RowHeight, FontSize, bUnsaved ? Warning() : Muted());

    AddLine(pBody, 0.0f, y + RowHeight - 1.0f, fWidth, RowLine());
    y += RowHeight;
  }

  return y + BuildNote(pBody, m_sLayersNote, y, fWidth);
}

float plEngineUsdLiveView::BuildParticipants(plUiElement* pBody, float fWidth)
{
  using namespace plEditorUi;

  if (!m_bSession)
    return BuildEmpty(pBody, fWidth, m_sParticipantsNote, m_bHasLink);

  auto& board = GetSurface()->GetBlackboard();

  // Participant, State, Opinions
  const float fOpinions = 76.0f;
  const float fState = plMath::Max(150.0f, (fWidth - fOpinions) * 0.4f);
  const float fName = plMath::Max(fWidth - fOpinions - fState, 80.0f);
  const float xState = fName;
  const float xOpinions = xState + fState;

  AddColumnTitle(pBody, "Participant", 0.0f, fName, false);
  AddColumnTitle(pBody, "State", xState, fState, false);
  AddColumnTitle(pBody, "Opinions", xOpinions, fOpinions, true);
  AddLine(pBody, 0.0f, s_fTableHeader - 1.0f, fWidth, Line());

  float y = s_fTableHeader;

  for (plUInt32 i = 0; i < m_Participants.GetCount(); ++i)
  {
    const ParticipantRow& row = m_Participants[i];

    // a click toggles highlighting the objects this layer decides
    const plString sRow = Name("part", i);
    plUiButton* pRow = AddGhostButton(pBody, sRow, 0.0f, y, fWidth, RowHeight, row.m_bHighlighted ? Selection() : Hover());
    if (row.m_bHighlighted)
      pRow->m_Color = Selection();

    plStringBuilder sTip("Click to highlight the objects this layer decides.\n", row.m_sPath);
    SetToolTip(board, sRow, sTip);

    const float fTool = plMath::Ceil(MeasureText(row.m_sTool, FontSize)) + 2.0f;
    AddText(pBody, row.m_sTool, 8.0f, y, plMath::Min(fTool, fName - 16.0f), RowHeight, FontSize, Foreground());
    if (fTool + 16.0f < fName - 16.0f)
      AddText(pBody, row.m_sUser, 8.0f + fTool + 8.0f, y, fName - 16.0f - fTool - 8.0f, RowHeight, FontSize, row.m_bHighlighted ? Foreground() : Muted());

    AddDot(pBody, xState + 8.0f, y + RowHeight * 0.5f, ToneColor(row.m_Tone));
    AddText(pBody, row.m_sState, xState + 8.0f + 11.0f, y, fState - 27.0f, RowHeight, s_uiLabelSize, Mix(ToneColor(row.m_Tone), Foreground(), 0.18f));

    plUiText* pOpinions = AddText(pBody, row.m_sOpinions, xOpinions + 8.0f, y, fOpinions - 16.0f, RowHeight, s_uiNumberSize, Muted(), true);
    pOpinions->m_HAlign = plUiTextHAlign::Right;

    AddLine(pBody, 0.0f, y + RowHeight - 1.0f, fWidth, RowLine());
    y += RowHeight;
  }

  return y + BuildNote(pBody, m_sParticipantsNote, y, fWidth);
}

plTexture2DResourceHandle plEngineUsdLiveView::GetExecutableIcon(plStringView sExecutable)
{
  if (sExecutable.IsEmpty())
    return {};

  const plString sKey = sExecutable;
  if (const plTexture2DResourceHandle* pKnown = m_ExecutableIcons.GetValue(sKey))
    return *pKnown;

  // the tool's own icon, as the platform shows it for the executable
  plTexture2DResourceHandle hIcon;

  plUInt32 uiWidth = 0, uiHeight = 0;
  plDynamicArray<plColorGammaUB> pixels;
  if (GetExecutableIconPixels(sExecutable, 24, uiWidth, uiHeight, pixels))
  {
    plStringBuilder sName("UsdLive/AppIcon/", sExecutable);
    hIcon = plEditorUi::ImageTexture(sName, uiWidth, uiHeight, pixels);
  }

  m_ExecutableIcons.Insert(sKey, hIcon);
  return hIcon;
}

float plEngineUsdLiveView::BuildApps(plUiElement* pBody, float fWidth)
{
  using namespace plEditorUi;

  auto& board = GetSurface()->GetBlackboard();
  float y = 0.0f;

  for (plUInt32 i = 0; i < m_Apps.GetCount(); ++i)
  {
    const AppRow& row = m_Apps[i];
    const float fTop = y + 8.0f;
    const float xText = s_fSide + 28.0f + 10.0f;

    // the tool's own icon, or its initial in a neutral square
    const plTexture2DResourceHandle hIcon = GetExecutableIcon(row.m_sExecutable);
    if (hIcon.IsValid())
    {
      plUiImage* pImage = PL_DEFAULT_NEW(plUiImage);
      pBody->m_Children.PushBack(pImage);
      Place(pImage, s_fSide, fTop, 24.0f, 24.0f);
      pImage->m_BrushMode = plUiBrushMode::Image;
      pImage->m_hTexture = hIcon;
    }
    else
    {
      plUiPanel* pSquare = Panel(pBody);
      Place(pSquare, s_fSide, fTop, 24.0f, 24.0f);
      pSquare->m_Color = plColorGammaUB(0, 0, 0, 0);
      pSquare->m_fBorderWidth = 1.0f;
      pSquare->m_BorderColor = Border();
      pSquare->m_vCornerRadius = plVec4(4);

      plUiText* pLetter = AddText(pSquare, plStringView(row.m_sName.GetData(), row.m_sName.GetData() + 1), 0.0f, 0.0f, 24.0f, 24.0f, FontSize, Muted());
      pLetter->m_HAlign = plUiTextHAlign::Center;
    }

    // the buttons, right-aligned: Open in, Install / Update / Switch On, Remove, Locate...
    struct RowButton
    {
      plString m_sLabel;
      plString m_sName;
      plString m_sTooltip;
      bool m_bEnabled;
    };

    plHybridArray<RowButton, 4> buttons;
    buttons.PushBack({plStringBuilder("Open in ", row.m_sName), Name("open", i), row.m_sOpenTooltip, row.m_bOpenEnabled});
    if (row.m_bShowInstall)
      buttons.PushBack({row.m_sInstallLabel, Name("install", i), row.m_sInstallTooltip, row.m_bInstallEnabled});
    if (row.m_bShowRemove)
      buttons.PushBack({"Remove", Name("remove", i), row.m_sRemoveTooltip, row.m_bRemoveEnabled});
    if (row.m_bShowLocate)
      buttons.PushBack({"Locate...", Name("locate", i), row.m_sLocateTooltip, true});

    float fButtons = 0.0f;
    for (const RowButton& button : buttons)
      fButtons += TextButtonWidth(button.m_sLabel) + 4.0f;

    // name and version box on the first line; the buttons beside them while they fit, else on a line of their own
    const float fName = plMath::Ceil(MeasureText(row.m_sName, FontSize)) + 2.0f;
    AddText(pBody, row.m_sName, xText, fTop, fName, ControlHeight, FontSize, Foreground());

    float xAfterName = xText + fName + 8.0f;
    if (!row.m_VersionTexts.IsEmpty())
    {
      const plStringView sVersion = row.m_iSelectedVersion >= 0 ? row.m_VersionTexts[row.m_iSelectedVersion].GetView() : plStringView();
      const float fVersion = plMath::Min(DropdownWidth(sVersion), 150.0f);
      const plString sName = Name("version", i);
      AddDropdown(pBody, sName, sVersion, xAfterName, fTop + 1.0f, fVersion, ControlHeight - 2.0f, row.m_bVersionsEnabled);
      SetToolTip(board, sName, "The installation Install and Open use");
      xAfterName += fVersion + 8.0f;
    }

    const bool bButtonsBeside = xAfterName + fButtons <= fWidth - s_fSide;
    float yLine = fTop + ControlHeight + 4.0f;
    const float fButtonsY = bButtonsBeside ? fTop : yLine;
    if (!bButtonsBeside)
      yLine += ControlHeight + 4.0f;

    float xButton = fWidth - s_fSide - fButtons + 4.0f;
    for (const RowButton& button : buttons)
    {
      AddTextButton(pBody, button.m_sLabel, button.m_sName, xButton, fButtonsY, ControlHeight, button.m_bEnabled);
      SetToolTip(board, button.m_sName, button.m_sTooltip);
      xButton += TextButtonWidth(button.m_sLabel) + 4.0f;
    }

    // the connector, what the last operation reported, the instances in the session
    const float fTextWidth = fWidth - s_fSide - xText;
    const float fConnector = AddLines(pBody, row.m_sConnector, xText, yLine, fTextWidth, s_uiNumberSize, ToneColor(row.m_ConnectorTone));
    if (!row.m_sConnectorTooltip.IsEmpty())
    {
      const plString sName = Name("connector", i);
      AddGhostButton(pBody, sName, xText, yLine, fTextWidth, fConnector);
      SetToolTip(board, sName, row.m_sConnectorTooltip);
    }
    yLine += fConnector;

    if (!row.m_Message.m_sText.IsEmpty())
      yLine += 2.0f + AddLines(pBody, row.m_Message.m_sText, xText, yLine + 2.0f, fTextWidth, s_uiNumberSize, row.m_Message.m_Tone == Tone::Attention ? Warning() : Dim());

    for (const AppLine& line : row.m_Instances)
    {
      AddDot(pBody, xText, yLine + 2.0f + s_fLine * 0.5f, ToneColor(line.m_Tone));
      AddText(pBody, line.m_sText, xText + 11.0f, yLine + 2.0f, fTextWidth - 11.0f, s_fLine, s_uiLabelSize, Mix(ToneColor(line.m_Tone), Foreground(), 0.18f));
      yLine += 2.0f + s_fLine;
    }

    y = plMath::Max(yLine, fTop + ControlHeight) + 8.0f;
    AddLine(pBody, 0.0f, y, fWidth, Line());
    y += 1.0f;
  }

  // the footer: what Install and Open do, and Check Again
  const float fCheck = TextButtonWidth("Check Again");
  const float fNote = AddLines(pBody, m_sAppsNote, s_fSide, y + 6.0f, fWidth - 3 * s_fSide - fCheck, s_uiLabelSize, Dim());
  AddTextButton(pBody, "Check Again", "Usd.checkagain", fWidth - s_fSide - fCheck, y + 6.0f + plMath::Max(fNote - ControlHeight, 0.0f) * 0.5f, ControlHeight, true);
  SetToolTip(board, "Usd.checkagain", "Look for installations and connectors again");

  return y + 6.0f + plMath::Max(fNote, ControlHeight) + 6.0f;
}

// Input.

void plEngineUsdLiveView::OnUiEvent(const plMsgUiEvent& event)
{
  const plStringView sId = event.m_sIdentifier.GetView();
  plUInt32 uiIndex = 0;

  struct NamedAction
  {
    const char* m_szName;
    Action m_Action;
  };

  const NamedAction actions[] = {
    {"Usd.link", Link},
    {"Usd.session", Session},
    {"Usd.reload", Reload},
    {"Usd.save", Save},
    {"Usd.revert", Revert},
    {"Usd.unlink", Unlink},
    {"Usd.openin", OpenIn},
    {"Usd.bake", Bake},
    {"Usd.commit", Commit},
    {"Usd.partition", Partition},
  };

  for (const NamedAction& action : actions)
  {
    if (sId == action.m_szName)
    {
      RunAction(action.m_Action);
      return;
    }
  }

  if (sId == "Usd.picker")
  {
    OpenLinkPicker();
    return;
  }

  if (sId == "Usd.emptyaction")
  {
    RunAction(m_bHasLink ? Session : Link);
    return;
  }

  if (sId == "Usd.checkagain")
  {
    plUsdApps::GetSingleton()->Refresh();
    m_pPanel->OnRefresh();
    return;
  }

  // a participant's layer: ticked counts, unticked is muted in this editor
  if (ParseIndex(sId, "Usd.mute.", uiIndex) && uiIndex < m_Layers.GetCount())
  {
    plUsdLiveLink* pLink = m_pPanel->GetShownLink();
    if (pLink == nullptr || !m_Layers[uiIndex].m_bMutable)
      return;

    const plStatus res = pLink->SetLayerMuted(m_Layers[uiIndex].m_sIdentifier, !m_Layers[uiIndex].m_bMuted);
    if (res.Failed())
      plEditorUiServices::MessageBoxStatus(res, "Muting the layer failed.");

    m_pPanel->OnRefresh();
    return;
  }

  if (ParseIndex(sId, "Usd.part.", uiIndex) && uiIndex < m_Participants.GetCount())
  {
    plUsdLiveLink* pLink = m_pPanel->GetShownLink();
    if (pLink == nullptr)
      return;

    const plString& sLayer = m_Participants[uiIndex].m_sPath;
    plUsdLiveManager::GetSingleton()->SetHighlightedLayer(pLink, pLink->GetHighlightedLayer() == sLayer ? plStringView() : plStringView(sLayer));
    m_pPanel->OnRefresh();
    return;
  }

  if (ParseIndex(sId, "Usd.version.", uiIndex) && uiIndex < m_Apps.GetCount())
  {
    OpenVersionMenu(uiIndex);
    return;
  }

  if (ParseIndex(sId, "Usd.open.", uiIndex) && uiIndex < m_Apps.GetCount())
  {
    if (m_Apps[uiIndex].m_bOpenEnabled)
      m_pPanel->OpenIn(static_cast<plUInt8>(m_Apps[uiIndex].m_App), false);
    return;
  }

  // failures show in the row, which the next refresh fills
  if (ParseIndex(sId, "Usd.install.", uiIndex) && uiIndex < m_Apps.GetCount())
  {
    plUsdApps::GetSingleton()->InstallConnector(m_Apps[uiIndex].m_App).IgnoreResult();
    m_pPanel->OnRefresh();
    return;
  }

  if (ParseIndex(sId, "Usd.remove.", uiIndex) && uiIndex < m_Apps.GetCount())
  {
    plUsdApps::GetSingleton()->RemoveConnector(m_Apps[uiIndex].m_App).IgnoreResult();
    m_pPanel->OnRefresh();
    return;
  }

  if (ParseIndex(sId, "Usd.locate.", uiIndex) && uiIndex < m_Apps.GetCount())
  {
    plUsdApps::GetSingleton()->Locate(m_Apps[uiIndex].m_App).IgnoreResult();
    m_pPanel->OnRefresh();
    return;
  }
}

void plEngineUsdLiveView::RunAction(Action action)
{
  if (action >= ActionCount || !m_Actions[action].m_bEnabled)
    return;

  plUsdLiveLink* pLink = m_pPanel->GetShownLink();

  switch (action)
  {
    case Link:
      plUsdLivePanel::LinkStageInteractive(m_pPanel->GetActiveSceneDocument());
      break;

    case Session:
      if (pLink != nullptr)
        plUsdLivePanel::StartSessionInteractive(pLink);
      break;

    case Reload:
      if (pLink != nullptr)
        pLink->RequestFullResync();
      break;

    case Save:
      m_pPanel->SaveShownLayer();
      break;

    case Revert:
      m_pPanel->RevertSelection();
      break;

    case Unlink:
      if (pLink != nullptr)
        plUsdLivePanel::UnlinkInteractive(pLink);
      break;

    case OpenIn:
      OpenOpenInMenu();
      break;

    case Bake:
      if (pLink != nullptr)
        pLink->RequestBake(true);
      break;

    case Commit:
      if (pLink != nullptr)
        plUsdLivePanel::CommitInteractive(pLink);
      break;

    case Partition:
      m_pPanel->ImportPartition();
      break;

    default:
      break;
  }

  m_pPanel->OnRefresh();
}

plVec2 plEngineUsdLiveView::GetBelow(plStringView sElement) const
{
  plDynamicArray<plUiFocusable> focusables;
  GetSurface()->GetCanvas().CollectFocusables(GetSurface()->GetLogicalSize(), focusables);

  for (const plUiFocusable& focusable : focusables)
  {
    if (focusable.m_pElement->m_sName == sElement)
      return plVec2(focusable.m_Rect.x, focusable.m_Rect.y + focusable.m_Rect.height);
  }

  return plVec2(s_fSide, s_fHeaderHeight);
}

void plEngineUsdLiveView::OpenLinkPicker()
{
  plEditorUiMenu menu;
  for (plUInt32 i = 0; i < m_LinkNames.GetCount(); ++i)
  {
    plEditorUiMenuItem& item = menu.AddAction(m_LinkNames[i], i);
    item.m_bCheckable = true;
    item.m_bChecked = static_cast<plInt32>(i) == m_iShownLink;
  }

  plEditorUiMenuView::Show(*GetSurface(), GetBelow("Usd.picker"), menu, [this](plUInt32 uiLink)
    { m_pPanel->OnLinkPicked(static_cast<int>(uiLink)); });
}

void plEngineUsdLiveView::OpenOpenInMenu()
{
  plUsdApps* pApps = plUsdApps::GetSingleton();
  pApps->RefreshIfOlder(plTime::MakeFromSeconds(30));

  plUsdLiveLink* pLink = m_pPanel->GetShownLink();

  // one entry per tool, then the Apps tab, where they are installed
  plEditorUiMenu menu;
  for (plUInt32 i = 0; i < static_cast<plUInt32>(plUsdApps::App::Count); ++i)
  {
    const plUsdApps::App app = static_cast<plUsdApps::App>(i);
    const char* szName = plUsdApps::GetName(app);
    const plUsdApps::Install* pInstall = pApps->GetSelectedInstall(app);
    const plString sBlocker = pApps->GetOpenBlocker(app, pLink);

    plStringBuilder sText = szName;
    if (pInstall != nullptr)
      sText.AppendFormat(" {}", pInstall->m_sVersion);

    if (pInstall == nullptr)
      sText.Append(" (not found)");
    else if (!pInstall->m_sUnsupported.IsEmpty())
      sText.Append(" (not supported)");
    else if (pInstall->m_Connector.m_State == plUsdApps::ConnectorState::NotInstalled)
      sText.Append(" (no connector)");

    plEditorUiMenuItem& item = menu.AddAction(sText, i);
    item.m_bEnabled = sBlocker.IsEmpty();

    plStringBuilder sTooltip;
    if (sBlocker.IsEmpty())
      sTooltip.SetFormat("Open this stage in {}, in a live session", szName);
    else
      sTooltip = sBlocker;
    item.m_sTooltip = sTooltip;
  }

  constexpr plUInt32 uiManageApps = 100;
  menu.AddSeparator();
  menu.AddAction("Manage Apps...", uiManageApps);

  plEditorUiMenuView::Show(*GetSurface(), GetBelow("Usd.openin"), menu, [this](plUInt32 uiPick)
    {
      if (uiPick == uiManageApps)
        m_pPanel->ShowAppsTab();
      else
        m_pPanel->OpenIn(static_cast<plUInt8>(uiPick), true);
    });
}

void plEngineUsdLiveView::OpenVersionMenu(plUInt32 uiApp)
{
  const AppRow& row = m_Apps[uiApp];
  if (!row.m_bVersionsEnabled)
    return;

  // installations, newest first; the last entry locates another
  plEditorUiMenu menu;
  for (plUInt32 i = 0; i < row.m_VersionTexts.GetCount(); ++i)
  {
    plEditorUiMenuItem& item = menu.AddAction(row.m_VersionTexts[i], i);
    item.m_bCheckable = !row.m_VersionPaths[i].IsEmpty();
    item.m_bChecked = static_cast<plInt32>(i) == row.m_iSelectedVersion;
    item.m_sTooltip = row.m_VersionPaths[i].IsEmpty() ? plString("Pick the executable of an installation that was not found") : row.m_VersionPaths[i];
  }

  plEditorUiMenuView::Show(*GetSurface(), GetBelow(Name("version", uiApp)), menu, [this, uiApp](plUInt32 uiPick)
    {
      // the row as it is now: the refresh may have found other installations meanwhile
      if (uiApp >= m_Apps.GetCount() || uiPick >= m_Apps[uiApp].m_VersionPaths.GetCount())
        return;

      const plUsdApps::App app = m_Apps[uiApp].m_App;
      const plString sExecutable = m_Apps[uiApp].m_VersionPaths[uiPick];

      plUsdApps* pApps = plUsdApps::GetSingleton();
      if (sExecutable.IsEmpty())
        pApps->Locate(app).IgnoreResult();
      else
        pApps->SelectInstall(app, sExecutable);

      m_pPanel->OnRefresh();
    });
}
