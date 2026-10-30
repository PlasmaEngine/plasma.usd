#include <EditorPluginUsd/EditorPluginUsdPCH.h>

#include <EditorPluginUsd/LiveLink/UsdLiveManager.h>
#include <EditorPluginUsd/UI/UsdPropertyGrid.h>
#include <EditorUi/Controls.h>
#include <EditorUi/MenuBuilder.h>
#include <EditorUi/Widgets.h>
#include <Core/Utils/Blackboard.h>
#include <Foundation/Utilities/ConversionUtils.h>
#include <GuiFoundation/EngineUi/EngineColorDialog.h>
#include <GuiFoundation/UIServices/UIServices.h>
#include <ToolsFoundation/Document/Document.h>
#include <ToolsFoundation/Object/DocumentObjectManager.h>


namespace
{
  /// Properties of the light and camera components a link maps from USD; see plUsdPrimComponents.
  constexpr const char* s_ValueProperties[] = {"Intensity", "IntensityUnit", "LightColor", "UseColorTemperature", "Temperature", "SpecularMultiplier",
    "CastShadows", "OuterSpotAngle", "InnerSpotAngle", "Width", "Height", "Radius", "Length", "Mode", "Dimensions", "FocalLength", "SensorWidth",
    "SensorHeight", "NearPlane", "FarPlane", "Aperture", "FocusMode", "FocusDistance", "ExposureCompensation"};

  constexpr float s_fRow = 22.0f;
  constexpr float s_fLabel = 96.0f;
  constexpr const char* s_szIcon = ":/UsdPlugin/Icons/UsdLive.svg";
  constexpr const char* s_szRevertIcon = ":/EditorPluginScene/Icons/PrefabRevert.svg";

  /// "xform" for the transform properties, "visibility" for Active, "values" for a light's or camera's settings: the
  /// opinion each of them is written as.
  plStringView GetOpinionKind(const plPropertyGridModel::Row& row)
  {
    if (row.m_pProperty == nullptr)
      return {};

    const plStringView sName = row.m_pProperty->GetPropertyName();
    if (sName == "LocalPosition" || sName == "LocalRotation" || sName == "LocalScaling" || sName == "LocalUniformScaling")
      return "xform";
    if (sName == "Active")
      return "visibility";

    for (const char* szValue : s_ValueProperties)
    {
      if (sName == szValue)
        return "values";
    }

    return {};
  }

  /// The generated object a selected object stands for: itself, or for a light or camera component, its owner.
  const plDocumentObject* GetPrimObject(plUsdLiveLink* pLink, const plDocumentObject* pObject)
  {
    if (pLink->IsGeneratedObject(pObject->GetGuid()))
      return pObject;

    const plDocumentObject* pOwner = pObject->GetParent();
    if (pOwner != nullptr && pLink->IsGeneratedObject(pOwner->GetGuid()))
      return pOwner;

    return nullptr;
  }

  /// The link that generated every object of the row's selection (or the owners of the selected components), else
  /// nullptr.
  plUsdLiveLink* GetSelectionLink(const plPropertyGridModel::Row& row)
  {
    plUsdLiveManager* pManager = plUsdLiveManager::GetSingleton();
    if (pManager == nullptr || row.m_Items.IsEmpty())
      return nullptr;

    plUsdLiveLink* pResult = nullptr;
    for (const plPropertySelection& selection : row.m_Items)
    {
      const plDocumentObject* pObject = selection.m_pObject;
      if (pObject == nullptr)
        return nullptr;

      const plDocument* pDocument = pObject->GetDocumentObjectManager()->GetDocument();
      plUsdLiveLink* pLink = pManager->FindLinkForObject(pDocument, pObject->GetGuid());
      if (pLink == nullptr && pObject->GetParent() != nullptr)
        pLink = pManager->FindLinkForObject(pDocument, pObject->GetParent()->GetGuid());

      if (pLink == nullptr || GetPrimObject(pLink, pObject) == nullptr || (pResult != nullptr && pResult != pLink))
        return nullptr;

      pResult = pLink;
    }

    return pResult;
  }

  bool HasEditOpinion(const plUsdPrimOpinions& opinions, plStringView sKind)
  {
    if (sKind == "xform")
      return opinions.m_bEditXform;
    if (sKind == "values")
      return opinions.m_bEditValues;
    return opinions.m_bEditVisibility;
  }

  /// Each bound material of the prim once, in the order of the mesh's material groups.
  void GetMaterialGroups(const plUsdLivePrimInfo& info, plHybridArray<const plUsdMaterialGroup*, 4>& out_groups)
  {
    out_groups.Clear();
    for (const plUsdMaterialGroup& group : info.m_MaterialGroups)
    {
      if (!group.m_bHasSurface || group.m_Surface.m_sMaterialPath.IsEmpty())
        continue;

      bool bListed = false;
      for (const plUsdMaterialGroup* pListed : out_groups)
        bListed |= pListed->m_Surface.m_sMaterialPath == group.m_Surface.m_sMaterialPath;

      if (!bListed)
        out_groups.PushBack(&group);
    }
  }

  /// The single selected object and its link, when the selection is one object a link knows.
  plUsdLiveLink* GetHeaderLink(const plPropertyGridModel& model, const plDocumentObject*& out_pObject)
  {
    plUsdLiveManager* pManager = plUsdLiveManager::GetSingleton();
    const auto selection = model.GetSelection();
    if (pManager == nullptr || selection.GetCount() != 1 || selection[0].m_pObject == nullptr)
      return nullptr;

    out_pObject = selection[0].m_pObject;
    const plDocument* pDocument = out_pObject->GetDocumentObjectManager()->GetDocument();
    return pManager->FindLinkForObject(pDocument, out_pObject->GetGuid());
  }

  plString FormatValue(float fValue)
  {
    plStringBuilder s;
    s.SetFormat("{}", plArgF(fValue, 3));
    return s;
  }

  /// The section's elements, placed top to bottom.
  class HeaderBuilder
  {
  public:
    explicit HeaderBuilder(plPropertyGridHeaderExtension::Context& ref_context)
      : m_Context(ref_context)
    {
    }

    void Title(plStringView sTitle)
    {
      plUiImage* pIcon = plEditorUi::SvgImage(m_Context.m_pParent, s_szIcon);
      plEditorUi::SetRect(pIcon, plRectFloat(10.0f, m_fY + 4.5f, 15.0f, 15.0f));
      plEditorUi::TextAt(m_Context.m_pParent, sTitle, plRectFloat(31.0f, m_fY, GetWidth() - 31.0f, 24.0f), plEditorUi::Foreground(), plEditorUi::FontSize);
      m_fY += 26.0f;
    }

    /// A label and a value; the tooltip names the element so the grid shows it on hover.
    void Row(plStringView sLabel, plStringView sValue, plStringView sTooltip = {})
    {
      plEditorUi::TextAt(m_Context.m_pParent, sLabel, plRectFloat(10.0f, m_fY, s_fLabel, s_fRow), plEditorUi::Dim(), plEditorUi::FontSize - 2);
      plUiText* pValue = plEditorUi::TextAt(m_Context.m_pParent, sValue, plRectFloat(10.0f + s_fLabel, m_fY, GetWidth() - s_fLabel - 20.0f, s_fRow), plEditorUi::Foreground(), plEditorUi::FontSize - 1);

      plStringBuilder sName;
      sName.SetFormat("Usd.Header.Row{}", m_uiRows++);
      pValue->m_sName = sName;
      plEditorUi::SetToolTip(m_Context.m_Blackboard, sName, sTooltip.IsEmpty() ? sValue : sTooltip);
      m_fY += s_fRow;
    }

    /// A label and a button that runs a command of the header.
    plUiButton* ButtonRow(plStringView sLabel, plStringView sText, plStringView sCommand, bool bEnabled, plStringView sTooltip, float fRightSpace = 0.0f)
    {
      plEditorUi::TextAt(m_Context.m_pParent, sLabel, plRectFloat(10.0f, m_fY, s_fLabel, s_fRow), plEditorUi::Dim(), plEditorUi::FontSize - 2);
      const plString sAction = m_Context.m_RegisterAction(sCommand);
      plUiButton* pButton = plEditorUi::Dropdown(m_Context.m_pParent, m_Context.m_Blackboard, plRectFloat(10.0f + s_fLabel, m_fY + 1.0f, GetWidth() - s_fLabel - 20.0f - fRightSpace, s_fRow - 2.0f), sText, sAction, bEnabled);
      plEditorUi::SetToolTip(m_Context.m_Blackboard, sAction, sTooltip);
      m_fY += s_fRow;
      return pButton;
    }

    /// A small revert button at the right of the row just placed.
    void RevertButton(plStringView sCommand, plStringView sTooltip)
    {
      const plString sAction = m_Context.m_RegisterAction(sCommand);
      plEditorUi::IconPushButton(m_Context.m_pParent, m_Context.m_Blackboard, plRectFloat(GetWidth() - 32.0f, m_fY - s_fRow + 1.0f, 22.0f, s_fRow - 2.0f), s_szRevertIcon, sAction);
      plEditorUi::SetToolTip(m_Context.m_Blackboard, sAction, sTooltip);
    }

    /// A label and a number field that commits through a command of the header.
    void NumberRow(plStringView sLabel, plStringView sCommand, plStringView sName, float fValue)
    {
      plEditorUi::TextAt(m_Context.m_pParent, sLabel, plRectFloat(10.0f, m_fY, s_fLabel, s_fRow), plEditorUi::Dim(), plEditorUi::FontSize - 2);
      plUiTextInput* pInput = m_Context.m_AddInput(sCommand, sName, FormatValue(fValue), 0.05);
      plEditorUi::SetRect(pInput, plRectFloat(10.0f + s_fLabel, m_fY + 1.0f, 90.0f, s_fRow - 2.0f));
      m_fY += s_fRow;
    }

    void Finish()
    {
      // a hairline under the section, as under every object header
      m_fY += 4.0f;
      plUiPanel* pLine = plEditorUi::Panel(m_Context.m_pParent);
      plEditorUi::SetRect(pLine, plRectFloat(0, m_fY, GetWidth(), 1.0f));
      pLine->m_Color = plEditorUi::Line();
      m_Context.m_pParent->m_vOffsets.w = m_fY + 1.0f;
    }

    float GetWidth() const { return plMath::Max(160.0f, m_Context.m_fWidth); }

    plPropertyGridHeaderExtension::Context& m_Context;
    float m_fY = 4.0f;
    plUInt32 m_uiRows = 0;
  };

  class UsdHeader : public plPropertyGridHeaderExtension
  {
  public:
    plStringView GetName() const override { return "Usd.Prim"; }

    bool Build(Context& context) const override
    {
      const plDocumentObject* pObject = nullptr;
      plUsdLiveLink* pLink = GetHeaderLink(context.m_Model, pObject);
      if (pLink == nullptr)
        return false;

      HeaderBuilder header(context);
      const plUsdLiveLink::Settings& settings = pLink->GetSettings();

      if (pLink->GetRootObject() == pObject->GetGuid())
      {
        header.Title("USD Link");
        header.Row("Stage", settings.m_sStageFile.GetFileNameAndExtension(), settings.m_sStageFile);
        header.Row("Session", pLink->IsWritable() ? plStringView(settings.m_sSessionName) : plStringView("none (read-only)"));
        header.Row("Your layer", pLink->IsWritable() ? pLink->GetParticipantLayer().GetFileNameAndExtension() : plStringView("-"), pLink->GetParticipantLayer());

        plStringBuilder sObjects;
        sObjects.SetFormat("{}", pLink->GetPrimCount());
        header.Row("Objects", sObjects);
        header.Finish();
        return true;
      }

      plUsdLivePrimInfo info;
      if (!pLink->GetPrimInfo(pObject->GetGuid(), info))
        return false;

      header.Title("USD Prim");
      header.Row("Path", info.m_sPath);
      header.Row("Type", info.m_sTypeName);

      const plStringView sWinner = info.m_Opinions.m_sWinningLayer;
      header.Row("Decided by", sWinner.IsEmpty() ? settings.m_sStageFile.GetFileNameAndExtension() : sWinner.GetFileNameAndExtension(), sWinner.IsEmpty() ? plStringView(settings.m_sStageFile) : sWinner);

      plStringBuilder sState = info.m_bActive ? "active" : "deactivated";
      if (info.m_bTransformAnimated)
        sState.Append(", animated");
      if (info.m_Opinions.HasEditOpinion())
        sState.Append(", overridden in your layer");
      header.Row("State", sState);

      BuildVariants(header, pLink, pObject, info);
      BuildMaterials(header, pLink, info);
      header.Finish();
      return true;
    }

    plStatus Execute(plStringView sCommand, Context& context) const override
    {
      const plDocumentObject* pObject = nullptr;
      plUsdLiveLink* pLink = GetHeaderLink(context.m_Model, pObject);
      if (pLink == nullptr)
        return plStatus("The USD prim is no longer selected.");

      const plUuid object = pObject->GetGuid();

      if (sCommand.StartsWith("variant:"))
        return PickVariant(context, pLink, object, plStringView(sCommand.GetStartPointer() + 8, sCommand.GetEndPointer()));

      if (sCommand == "revertvariants")
        return pLink->RevertProperty(plArrayPtr<const plUuid>(&object, 1), "variants");

      plUsdLivePrimInfo info;
      if (!pLink->GetPrimInfo(object, info))
        return plStatus("The USD prim is no longer known.");

      plHybridArray<const plUsdMaterialGroup*, 4> groups;
      GetMaterialGroups(info, groups);

      // "revertmaterial:<i>", "color:<i>", "value:<i>:<Metallic|Roughness|Opacity>"
      plHybridArray<plStringView, 3> parts;
      plStringBuilder sCopy = sCommand;
      sCopy.Split(false, parts, ":");

      plUInt32 uiMaterial = 0;
      if (parts.GetCount() < 2 || plConversionUtils::StringToUInt(parts[1], uiMaterial).Failed() || uiMaterial >= groups.GetCount())
        return plStatus("The material is no longer bound.");

      const plString sPath = groups[uiMaterial]->m_Surface.m_sMaterialPath;

      if (parts[0] == "revertmaterial")
        return pLink->RevertSurface(sPath);

      if (parts[0] == "color")
      {
        plColor picked = groups[uiMaterial]->m_Surface.m_BaseColor;
        if (!plEngineColorDialog::PickColor(picked, false, false, "Base Color"))
          return plStatus(PL_SUCCESS);

        return pLink->SetSurfaceValue(sPath, plUsdStage::SurfaceValue::BaseColor, picked);
      }

      if (parts[0] == "value" && parts.GetCount() == 3)
      {
        plStringBuilder sName;
        sName.SetFormat("Usd.Header.M{}.{}", uiMaterial, parts[2]);
        const plVariant text = context.m_Blackboard.GetEntryValue(plTempHashedString(sName));

        double fValue = 0.0;
        if (!text.IsA<plString>() || plConversionUtils::StringToFloat(text.Get<plString>(), fValue).Failed())
          return plStatus("Enter a number from 0 to 1.");

        const plUsdStage::SurfaceValue value = parts[2] == "Metallic" ? plUsdStage::SurfaceValue::Metallic : parts[2] == "Roughness" ? plUsdStage::SurfaceValue::Roughness : plUsdStage::SurfaceValue::Opacity;
        return pLink->SetSurfaceValue(sPath, value, static_cast<float>(plMath::Clamp(fValue, 0.0, 1.0)));
      }

      return plStatus("Unknown USD header action.");
    }

  private:
    /// One row per variant set; one button reverts every selection this editor made on the prim.
    void BuildVariants(HeaderBuilder& ref_header, plUsdLiveLink* pLink, const plDocumentObject* pObject, const plUsdLivePrimInfo& info) const
    {
      plDynamicArray<plUsdVariantSet> sets;
      pLink->GetVariantSets(pObject->GetGuid(), sets);

      const bool bWritable = pLink->IsWritable();
      const plStringView sReadOnly = "The link is read-only; start a session to switch variants in your own layer.";

      for (plUInt32 i = 0; i < sets.GetCount(); ++i)
      {
        const plUsdVariantSet& set = sets[i];
        const bool bRevert = i == 0 && info.m_Opinions.m_bEditVariants;
        ref_header.ButtonRow(set.m_sName, set.m_sSelection, plStringBuilder("variant:", set.m_sName), bWritable, bWritable ? plStringView() : sReadOnly, bRevert ? 26.0f : 0.0f);

        if (bRevert)
          ref_header.RevertButton("revertvariants", "Remove your layer's variant selections on this prim, so the other layers' selections apply again");
      }
    }

    /// One bound material: its name, its base colour and values, editable where no texture drives them.
    void BuildMaterials(HeaderBuilder& ref_header, plUsdLiveLink* pLink, const plUsdLivePrimInfo& info) const
    {
      plHybridArray<const plUsdMaterialGroup*, 4> groups;
      GetMaterialGroups(info, groups);

      const bool bWritable = pLink->IsWritable();
      plStringBuilder sCommand, sName, sTooltip;

      for (plUInt32 i = 0; i < groups.GetCount(); ++i)
      {
        const plUsdSurface& surface = groups[i]->m_Surface;
        ref_header.Row("Material", surface.m_sMaterialPath.GetFileNameAndExtension(), surface.m_sMaterialPath);

        if (pLink->HasSurfaceOpinion(surface.m_sMaterialPath))
        {
          sCommand.SetFormat("revertmaterial:{}", i);
          ref_header.RevertButton(sCommand, "Remove your layer's changes to this material, so the other layers' values show again");
        }

        // a texture drives the value, or the link is read-only: the value shows, it cannot change here
        auto lockedReason = [&](plStringView sTexture, const char* szWhat) -> plString
        {
          if (!sTexture.IsEmpty())
            return plStringBuilder("A texture drives the ", szWhat, ": ", sTexture.GetFileNameAndExtension());
          if (!bWritable)
            return plString("The link is read-only; start a session to change materials in your own layer.");
          return {};
        };

        sCommand.SetFormat("color:{}", i);
        const plString sColorLock = lockedReason(surface.m_sBaseColorTexture, "base colour");
        const plColorGammaUB color(surface.m_BaseColor);
        sName.SetFormat("#{}{}{}", plArgU(color.r, 2, true, 16, true), plArgU(color.g, 2, true, 16, true), plArgU(color.b, 2, true, 16, true));
        plUiButton* pColor = ref_header.ButtonRow("Base colour", sName, sCommand, sColorLock.IsEmpty(), sColorLock);
        pColor->m_Color = color;

        const struct
        {
          const char* m_szLabel;
          const char* m_szKind;
          float m_fValue;
          plStringView m_sTexture;
          const char* m_szWhat;
        } values[] = {
          {"Metallic", "Metallic", surface.m_fMetallic, surface.m_sMetallicTexture, "metallic value"},
          {"Roughness", "Roughness", surface.m_fRoughness, surface.m_sRoughnessTexture, "roughness"},
          {"Opacity", "Opacity", surface.m_fOpacity, surface.m_sOpacityTexture, "opacity"},
        };

        for (const auto& value : values)
        {
          const plString sLock = lockedReason(value.m_sTexture, value.m_szWhat);
          if (!sLock.IsEmpty())
          {
            ref_header.Row(value.m_szLabel, FormatValue(value.m_fValue), sLock);
            continue;
          }

          sCommand.SetFormat("value:{}:{}", i, value.m_szKind);
          sName.SetFormat("Usd.Header.M{}.{}", i, value.m_szKind);
          ref_header.NumberRow(value.m_szLabel, sCommand, sName, value.m_fValue);
        }
      }
    }

    plStatus PickVariant(Context& context, plUsdLiveLink* pLink, const plUuid& object, plStringView sSet) const
    {
      plDynamicArray<plUsdVariantSet> sets;
      pLink->GetVariantSets(object, sets);

      for (const plUsdVariantSet& set : sets)
      {
        if (set.m_sName != sSet)
          continue;

        // an engine menu (decision S4-D47): the pick arrives after the menu closes, so it finds the prim's link again
        if (!context.m_ShowMenu.IsValid())
          return plStatus("The variants cannot be shown here.");

        plEditorUiMenuBuilder menu;
        for (const plString& sVariant : set.m_Variants)
        {
          const bool bSelected = sVariant == set.m_sSelection;
          menu.AddCheck(sVariant, bSelected, [pModel = &context.m_Model, object, sSetName = plString(sSet), sVariant, bSelected]()
            {
              const plDocumentObject* pObject = nullptr;
              plUsdLiveLink* pPickLink = GetHeaderLink(*pModel, pObject);
              if (bSelected || pPickLink == nullptr || pObject->GetGuid() != object)
                return;

              pPickLink->SetVariantSelection(object, sSetName, sVariant).LogFailure();
            });
        }

        context.m_ShowMenu(menu);
        return plStatus(PL_SUCCESS);
      }

      return plStatus("The variant set is no longer there.");
    }
  };

  UsdHeader s_Header;
  plEventSubscriptionID s_ManagerSubscription = 0;
} // namespace

bool plUsdPropertyRowDecorator::GetState(const plPropertyGridModel::Row& row, bool& out_bActive, bool& out_bMixed, plStringBuilder& out_sTooltip) const
{
  const plStringView sKind = GetOpinionKind(row);
  if (sKind.IsEmpty())
    return false;

  plUsdLiveLink* pLink = GetSelectionLink(row);
  if (pLink == nullptr)
    return false;

  plUInt32 uiOverridden = 0;
  bool bAnimated = false;
  plString sWinner;
  for (const plPropertySelection& selection : row.m_Items)
  {
    plUsdLivePrimInfo info;
    if (pLink->GetPrimInfo(GetPrimObject(pLink, selection.m_pObject)->GetGuid(), info))
    {
      if (HasEditOpinion(info.m_Opinions, sKind))
        ++uiOverridden;

      bAnimated |= info.m_bTransformAnimated;

      if (sWinner.IsEmpty())
        sWinner = info.m_Opinions.m_sWinningLayer;
    }
  }

  const plUInt32 uiCount = row.m_Items.GetCount();
  out_bActive = uiOverridden > 0;
  out_bMixed = uiOverridden > 0 && uiOverridden < uiCount;

  const plStringView sParticipantFile = pLink->GetParticipantLayer().GetFileNameAndExtension();
  if (sKind == "xform" && bAnimated)
    out_sTooltip.SetFormat("Animated in USD. The transform follows the animation, so edits here are not kept.");
  else if (!pLink->IsWritable())
    out_sTooltip.SetFormat("From USD. The link is read-only, so edits here are not written back; start a session to write a layer of your own.");
  else if (out_bActive)
    out_sTooltip.SetFormat("Your layer ({}) overrides this. Click to remove your opinion, so the other layers' value shows again.", sParticipantFile);
  else
    out_sTooltip.SetFormat("From {}. Edits go into your layer ({}).", sWinner.IsEmpty() ? pLink->GetSettings().m_sStageFile.GetFileNameAndExtension() : sWinner.GetFileNameAndExtension(), sParticipantFile);

  return true;
}

void plUsdPropertyRowDecorator::Toggle(const plPropertyGridModel::Row& row) const
{
  plUsdLiveLink* pLink = GetSelectionLink(row);
  if (pLink == nullptr || !pLink->IsWritable())
    return;

  plHybridArray<plUuid, 16> objects;
  for (const plPropertySelection& selection : row.m_Items)
    objects.PushBack(GetPrimObject(pLink, selection.m_pObject)->GetGuid());

  const plStatus res = pLink->RevertProperty(objects, GetOpinionKind(row));
  if (res.Failed())
    plEditorUiServices::MessageBoxStatus(res, "Reverting failed.");
}

void plUsdPropertyGridHeaders::Register()
{
  plPropertyGridHeaderExtension::Register(&s_Header);

  // the section follows the link's prim states
  if (plUsdLiveManager* pManager = plUsdLiveManager::GetSingleton())
  {
    s_ManagerSubscription = pManager->m_Events.AddEventHandler([](const plUsdLiveManagerEvent&)
      { plPropertyGridExtension::NotifyChanged(); });
  }
}

void plUsdPropertyGridHeaders::Unregister()
{
  if (plUsdLiveManager* pManager = plUsdLiveManager::GetSingleton())
    pManager->m_Events.RemoveEventHandler(s_ManagerSubscription);

  plPropertyGridHeaderExtension::Unregister(&s_Header);
}
