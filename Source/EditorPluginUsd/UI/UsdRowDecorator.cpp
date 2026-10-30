#include <EditorPluginUsd/EditorPluginUsdPCH.h>

#include <EditorPluginUsd/Bake/UsdBakePrefab.h>
#include <EditorPluginUsd/LiveLink/UsdLiveManager.h>
#include <EditorPluginUsd/UI/UsdRowDecorator.h>
#include <Foundation/Math/ColorScheme.h>
#include <ToolsFoundation/Document/Document.h>
#include <ToolsFoundation/Object/DocumentObjectManager.h>

namespace
{
  /// The accent marks this editor's own opinions; everything else stays neutral.
  const plColorGammaUB AccentColor(0x6E, 0xA8, 0xFE);
  const plColorGammaUB NeutralColor(0xB4, 0xB4, 0xBC);

  bool IsSameFile(plStringView sA, plStringView sB)
  {
    plStringBuilder a = sA, b = sB;
    a.MakeCleanPath();
    b.MakeCleanPath();
    return !a.IsEmpty() && a.IsEqual_NoCase(b);
  }

  bool IsInFolderOf(plStringView sFile, plStringView sSibling)
  {
    plStringBuilder sFolder = sFile, sSiblingFolder = sSibling;
    sFolder.PathParentDirectory();
    sSiblingFolder.PathParentDirectory();
    return !sSibling.IsEmpty() && sFolder.IsEqual_NoCase(sSiblingFolder);
  }

  constexpr const char* UsdIcon = ":/UsdPlugin/Icons/UsdLive.svg";
  constexpr const char* XformIcon = ":/UsdPlugin/Icons/UsdPrimXform.svg";
  constexpr const char* MeshIcon = ":/UsdPlugin/Icons/UsdPrimMesh.svg";
  constexpr const char* LightIcon = ":/UsdPlugin/Icons/UsdPrimLight.svg";
  constexpr const char* CameraIcon = ":/UsdPlugin/Icons/UsdPrimCamera.svg";
  constexpr const char* BakeIcon = ":/UsdPlugin/Icons/UsdBake.svg";
} // namespace

plUsdRowDecorator::plUsdRowDecorator()
{
  if (plUsdLiveManager* pManager = plUsdLiveManager::GetSingleton())
  {
    m_ManagerSubscription = pManager->m_Events.AddEventHandler(plMakeDelegate(&plUsdRowDecorator::ManagerEventHandler, this));
  }
}

plUsdRowDecorator::~plUsdRowDecorator()
{
  if (plUsdLiveManager* pManager = plUsdLiveManager::GetSingleton())
  {
    pManager->m_Events.RemoveEventHandler(m_ManagerSubscription);
  }
}

void plUsdRowDecorator::ManagerEventHandler(const plUsdLiveManagerEvent& e)
{
  // Prim state changes request their own rows; a new highlight touches every row of the link.
  if (e.m_Type == plUsdLiveManagerEvent::Type::HighlightChanged)
  {
    plObjectRowDecoratorRegistry::RequestRefresh(e.m_pDocument);
  }
}

bool plUsdRowDecorator::Decorate(const plDocumentObject* pObject, plObjectRowDecoration& inout_decoration) const
{
  plUsdLiveManager* pManager = plUsdLiveManager::GetSingleton();
  if (pManager == nullptr || pObject == nullptr)
    return false;

  const plDocument* pDocument = pObject->GetDocumentObjectManager()->GetDocument();

  if (plUsdBakePrefab::IsBakeObject(pDocument, pObject->GetGuid()))
  {
    inout_decoration.m_sIcon = BakeIcon;

    plObjectRowBadge& badge = inout_decoration.m_Badges.ExpandAndGetRef();
    badge.m_sText = "bake";
    badge.m_Color = NeutralColor;

    const plDocumentObject* pParent = pObject->GetParent();
    plUsdLiveLink* pOwner = pParent ? pManager->FindLinkForObject(pDocument, pParent->GetGuid()) : nullptr;

    plStringBuilder sTooltip = "The stage's baked copy, used by exports, the Player and prefab instances.";
    if (pOwner != nullptr && pOwner->GetRootObject() == pParent->GetGuid())
    {
      const bool bLive = pOwner->GetPrimCount() > 0;
      inout_decoration.m_bDimmed = bLive;
      sTooltip.Append(bLive ? "\nHidden while the live stage is shown." : "\nShown until the live stage has streamed in.");
      if (!pOwner->GetBakeStatus().IsEmpty())
        sTooltip.AppendFormat("\nBake: {}", pOwner->GetBakeStatus());
    }
    else
    {
      sTooltip.Append("\nNo live link here, so the bake is what shows: a placed USD prefab, or a stage that was unlinked.");
    }

    badge.m_sTooltip = sTooltip;
    inout_decoration.m_sTooltip = sTooltip;
    return true;
  }

  plUsdLiveLink* pLink = pManager->FindLinkForObject(pDocument, pObject->GetGuid());
  if (pLink == nullptr)
    return false;

  const plUsdLiveLink::Settings& settings = pLink->GetSettings();

  if (pLink->GetRootObject() == pObject->GetGuid())
  {
    inout_decoration.m_sIcon = UsdIcon;

    plObjectRowBadge& badge = inout_decoration.m_Badges.ExpandAndGetRef();
    badge.m_bDot = true;

    plStringBuilder sTooltip = settings.m_sStageFile;
    if (pLink->IsWritable())
    {
      plStringBuilder sText;
      sText.SetFormat("session \xC2\xB7 {}", pLink->GetParticipantCount());
      badge.m_sText = sText;
      badge.m_Color = plColorScheme::LightUI(plColorScheme::Green);
      sTooltip.AppendFormat("\nSession '{}', writing {}", settings.m_sSessionName, pLink->GetParticipantLayer().GetFileNameAndExtension());
    }
    else
    {
      badge.m_sText = "read-only";
      badge.m_Color = NeutralColor;
      sTooltip.Append("\nRead-only. Start a session in the USD Live panel to write a layer of your own.");
    }

    badge.m_sTooltip = sTooltip;
    inout_decoration.m_sTooltip = sTooltip;
    return true;
  }

  plUsdLivePrimInfo info;
  if (!pLink->GetPrimInfo(pObject->GetGuid(), info))
    return false;

  switch (info.m_Kind)
  {
    case plUsdPrimKind::Mesh:
    case plUsdPrimKind::Gprim:
    case plUsdPrimKind::Instancer:
      inout_decoration.m_sIcon = MeshIcon;
      break;
    case plUsdPrimKind::Light:
      inout_decoration.m_sIcon = LightIcon;
      break;
    case plUsdPrimKind::Camera:
      inout_decoration.m_sIcon = CameraIcon;
      break;
    default:
      inout_decoration.m_sIcon = XformIcon;
      break;
  }

  const plUsdPrimOpinions& opinions = info.m_Opinions;
  const plStringView sParticipantFile = pLink->GetParticipantLayer().GetFileNameAndExtension();

  if (opinions.HasEditOpinion())
  {
    plStringBuilder sWhat;
    if (opinions.m_bEditXform)
      sWhat.Append("moves it");
    if (opinions.m_bEditVisibility)
      sWhat.Append(sWhat.IsEmpty() ? "" : ", ", "sets its visibility");
    if (opinions.m_bEditActive)
      sWhat.Append(sWhat.IsEmpty() ? "" : ", ", "deactivates it");
    if (opinions.m_bEditValues)
      sWhat.Append(sWhat.IsEmpty() ? "" : ", ", "changes its settings");
    if (opinions.m_bEditVariants)
      sWhat.Append(sWhat.IsEmpty() ? "" : ", ", "selects its variants");
    if (opinions.m_bEditComponents)
      sWhat.Append(sWhat.IsEmpty() ? "" : ", ", "changes its Plasma components");

    plObjectRowBadge& dot = inout_decoration.m_Badges.ExpandAndGetRef();
    dot.m_bDot = true;
    dot.m_Color = AccentColor;
    dot.m_sTooltip = plStringBuilder("Your layer (", sParticipantFile, ") ", sWhat);
  }

  // Which layer decides: this editor's, another participant's, or the asset (its root layer, sublayers or references).
  {
    const plStringView sWinner = opinions.m_sWinningLayer;

    plObjectRowBadge& letter = inout_decoration.m_Badges.ExpandAndGetRef();
    letter.m_Color = NeutralColor;

    if (!sWinner.IsEmpty() && IsSameFile(sWinner, pLink->GetParticipantLayer()))
    {
      letter.m_sText = "P";
      letter.m_Color = AccentColor;
      letter.m_sTooltip = plStringBuilder("Your layer decides this prim: ", sParticipantFile);
    }
    else if (!sWinner.IsEmpty() && IsInFolderOf(sWinner, pLink->GetParticipantLayer()))
    {
      const plStringView sFile = sWinner.GetFileNameAndExtension();
      plStringBuilder sInitial = sFile.GetSubString(0, 1);
      sInitial.ToUpper();
      letter.m_sText = sInitial;
      letter.m_sTooltip = plStringBuilder("Another participant's layer decides this prim: ", sFile);
    }
    else
    {
      letter.m_sText = "A";
      letter.m_sTooltip = plStringBuilder("The asset decides this prim: ", sWinner.IsEmpty() ? settings.m_sStageFile.GetFileNameAndExtension() : sWinner.GetFileNameAndExtension());
    }

    if (!pLink->GetHighlightedLayer().IsEmpty() && IsSameFile(sWinner, pLink->GetHighlightedLayer()))
      letter.m_Color = plColorScheme::LightUI(plColorScheme::Yellow);
  }

  inout_decoration.m_bDimmed = !info.m_bActive || info.m_bPrototype;
  inout_decoration.m_bStrikeThrough = !info.m_bActive;

  plStringBuilder sTooltip;
  sTooltip.SetFormat("{} ({})", info.m_sPath, info.m_sTypeName);
  if (!info.m_bActive)
    sTooltip.AppendFormat("\nDeactivated{}. Reactivate it from the context menu.", opinions.m_bEditActive ? plStringBuilder(" in ", sParticipantFile) : plStringBuilder());
  if (info.m_bPrototype)
    sTooltip.Append("\nBelow a point instancer: hidden here, drawn only by the instancer.");
  if (info.m_Kind == plUsdPrimKind::Instancer)
    sTooltip.AppendFormat("\nPoint instancer drawing {} instances.", info.m_uiInstances);
  inout_decoration.m_sTooltip = sTooltip;

  return true;
}
