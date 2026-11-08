#include <EditorPluginUsd/EditorPluginUsdPCH.h>

#include <Core/Utils/Blackboard.h>
#include <EditorPluginScene/Panels/WorldStreamingPanel/WorldStreamingContent.h>
#include <EditorPluginScene/Scene/Scene2Document.h>
#include <EditorPluginUsd/PartitionImport/UsdPartitionImportEngineDlg.h>
#include <EditorPluginUsd/PartitionImport/UsdPartitionImporter.h>
#include <EditorPluginUsd/PartitionImport/UsdPartitionPanelExtension.h>
#include <EditorPluginUsd/PartitionImport/UsdPartitionStageRead.h>
#include <EditorPluginUsd/PartitionImport/UsdPrimTreeView.h>
#include <EditorUi/Menu.h>
#include <EditorUi/ProcessView.h>
#include <EditorUi/Surface.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Math/ColorScheme.h>
#include <Foundation/Utilities/ConversionUtils.h>
#include <GuiFoundation/Dialogs/FileDialog.h>
#include <GuiFoundation/UIServices/UIServices.h>

namespace
{
  const char* s_szScan = "Scan";
  const char* s_szImport = "Import";
  const char* s_szClose = "Close";

  const char* s_szStage = "Upi.Stage";
  const char* s_szStageText = "Upi.Stage.Text";
  const char* s_szStageSubmit = "Upi.Stage.Submit";
  const char* s_szBrowse = "Upi.Browse";
  const char* s_szStageInfo = "Upi.StageInfo";
  const char* s_szPartition = "Upi.Partition";
  const char* s_szPartitionText = "Upi.Partition.Text";
  const char* s_szCellSize = "Upi.CellSize";
  const char* s_szBatch = "Upi.Batch";
  const char* s_szSplit = "Upi.Split";
  const char* s_szLights = "Upi.Lights";
  const char* s_szSkipGuides = "Upi.SkipGuides";
  const char* s_szRegion = "Upi.Region";
  const char* s_szSelectAll = "Upi.Prims.SelectAll";
  const char* s_szSelectAllText = "Upi.Prims.SelectAll.Text";
  const char* s_szVariantPrefix = "Upi.Variant.";
  const char* s_szNumberSubmit = "Upi.Number.Submit";

  const char* s_szIdle = "Scan counts the objects, cells and unique meshes of the checked prims.";
  const char* s_szRunning = "A USD import is running; wait for it or cancel it in the World Streaming panel.";
  const char* s_szScanToolTip = "Loads each checked prim fully, one at a time, and counts what the import would place. Large stages take a while and need memory for their largest prim.";
  const char* s_szPrimsToolTip = "Each checked prim is loaded, scanned and placed on its own, so memory stays bounded by the largest. Payload prims show the size of their proxy variant when they have one, else -.";
  const char* s_szStagePlaceholder = "A .usd, .usda, .usdc or .usdz file";
  const char* s_szPartitionToolTip = "The world partition the stage is imported into. Its cells are written to '<scene>_data/<Partition>/'.";
  const char* s_szCellSizeToolTip = "Edge length of the partition's cells. Each object goes into the cell under the centre of its bounds.";
  const char* s_szCellSizeFixedToolTip = "The cell size of an imported partition stays; import into a new partition for another one.";
  const char* s_szBatchLabel = "Batch small objects, below";
  const char* s_szBatchToolTip = "Small instances of one prototype in a cell become one instanced object, and small loose meshes under one prim in a cell one merged mesh. Off: one object per prim.";
  const char* s_szSplitLabel = "Split large meshes along cell borders, wider than";
  const char* s_szSplitToolTip = "Loose meshes wider than this are cut at the cell borders, one watertight piece per cell. Instances are never cut. Off: large meshes go whole into the cell under their centre.";
  const char* s_szLightsLabel = "Import lights and cameras, into the always loaded layer '<Partition> Globals'";
  const char* s_szSkipGuidesLabel = "Skip guide prims (helpers, volumes, bounds)";
  const char* s_szSkipGuidesToolTip = "Prims with the guide or proxy purpose are never imported.";
  const char* s_szRegionLabel = "Region";
  const char* s_szRegionToolTip = "Only objects whose bounds centre lies in the rectangle are imported, in metres after the stage's axes are converted (Z up).";
  const char* s_szNoVariants = "The checked prims have no variant sets.";
  const char* s_szVariantToolTip = "Selected in the stage's session layer for every checked prim with this set; no file changes. Default: {}.";
  const char* s_szSizeToolTip = "Measured with the proxy variant.";

  const char* const s_szRegionModes[] = {"Whole stage", "Rectangle"};
  const char* const s_szRegionLabels[] = {"X from", "Y from", "X to", "Y to"};

  constexpr float s_fWidth = 780.0f;
  constexpr float s_fLabelWidth = 110.0f;
  constexpr float s_fTop = 12.0f;
  constexpr float s_fBottom = 12.0f;
  constexpr float s_fGap = 10.0f;
  constexpr float s_fInfoLine = 16.0f;
  constexpr float s_fCheckPitch = 28.0f;
  constexpr float s_fRowPitch = plEditorUi::ControlHeight + plEditorUiForm::RowGap;
  constexpr float s_fSectionPitch = plEditorUi::SectionHeaderHeight + 6.0f;
  constexpr float s_fHintHeight = 18.0f;
  constexpr float s_fNumberWidth = 110.0f;
  constexpr float s_fCellSizeWidth = 120.0f;
  constexpr float s_fRegionWidth = 150.0f;
  constexpr float s_fVariantWidth = 260.0f;
  constexpr float s_fRegionNumberWidth = 100.0f;
  constexpr float s_fProcessHeight = 96.0f;
  constexpr float s_fMinTreeHeight = 120.0f;
  constexpr float s_fPreferredTreeHeight = 210.0f;

  /// Qt shows the tree fully expanded up to this many rows, else two levels.
  constexpr plUInt32 s_uiExpandAllRows = 300;

  const plColorGammaUB s_Transparent(0, 0, 0, 0);

  /// Sections use the Prefabs colour, as the stage in the USD Live panel.
  plColorGammaUB SectionColor()
  {
    return plColorGammaUB(plColorScheme::GetCategoryColor("Prefabs", plColorScheme::CategoryColorUsage::BorderColor));
  }

  plString Trimmed(plStringView sText)
  {
    plStringBuilder sTrimmed = sText;
    sTrimmed.Trim(" \t\r\n\v\f");
    return sTrimmed;
  }

  plString FormatNumber(double fValue, plUInt32 uiDecimals)
  {
    plStringBuilder sText;
    sText.SetFormat("{}", plArgF(fValue, static_cast<plInt32>(uiDecimals)));
    return sText;
  }

  plString Indexed(const char* szPrefix, plUInt32 uiIndex)
  {
    plStringBuilder sName;
    sName.SetFormat("{}{}", szPrefix, uiIndex);
    return sName;
  }

  /// The index after a prefix of an element name ("Upi.Variant.3"); false for other names.
  bool ParseIndex(plStringView sId, plStringView sPrefix, plUInt32& out_uiIndex)
  {
    if (!sId.StartsWith(sPrefix))
      return false;

    const plStringBuilder sIndex(plStringView(sId.GetStartPointer() + sPrefix.GetElementCount(), sId.GetEndPointer()));
    return plConversionUtils::StringToUInt(sIndex, out_uiIndex).Succeeded();
  }

  /// The width a check box with its label needs.
  float CheckWidth(plStringView sLabel)
  {
    return 24.0f + plMath::Ceil(plEditorUi::MeasureText(sLabel, plEditorUi::FontSize - 1)) + 4.0f;
  }

  /// A number that cannot be edited: dimmed and right-aligned, with its unit.
  void DisabledNumber(plUiElement* pParent, const plRectFloat& rect, plStringView sText)
  {
    plUiPanel* pWell = plEditorUi::ReadOnlyField(pParent, rect, plStringBuilder(sText, " m"), true);
    plUiText* pText = static_cast<plUiText*>(pWell->m_Children[0]);
    plEditorUi::SetRect(pText, plRectFloat(4.0f, 0, rect.width - 8.0f, rect.height));
    pText->m_HAlign = plUiTextHAlign::Right;
  }

  /// The scene's partition of that name; the last when several differ only in case.
  const plSceneWorldPartition* FindPartition(plScene2Document* pScene, plStringView sName)
  {
    const plSceneWorldPartition* pFound = nullptr;

    plDynamicArray<const plSceneWorldPartition*> partitions;
    pScene->GetPartitions(partitions);
    for (const plSceneWorldPartition* pPartition : partitions)
    {
      if (pPartition->m_sName.IsEqual_NoCase(sName))
        pFound = pPartition;
    }

    return pFound;
  }
} // namespace

//////////////////////////////////////////////////////////////////////////

plUsdPartitionImportEngineDlg::plUsdPartitionImportEngineDlg(plScene2Document* pScene, const plUsdPartitionImportOptions& options, bool bExistingPartition)
  : plEngineDialog("UsdPartitionImport", "World Partition from USD")
  , m_pScene(pScene)
  , m_bExistingPartition(bExistingPartition)
{
  m_Numbers[BatchNumber] = {"Upi.BatchSize", 0.1, 10000.0, 1, 8.0};
  m_Numbers[SplitNumber] = {"Upi.SplitSize", 1.0, 100000.0, 0, 128.0};
  for (plUInt32 i = 0; i < 4; ++i)
  {
    static const char* const s_szNames[] = {"Upi.XFrom", "Upi.YFrom", "Upi.XTo", "Upi.YTo"};
    m_Numbers[RegionXFrom + i] = {s_szNames[i], -1000000.0, 1000000.0, 0, i < 2 ? -512.0 : 512.0};
  }

  m_DocumentSubscription = plDocumentManager::s_Events.AddEventHandler(plMakeDelegate(&plUsdPartitionImportEngineDlg::DocumentManagerEventHandler, this));

  if (!IsReady())
    return;

  m_pTree = PL_DEFAULT_NEW(plUsdPrimTreeView, *GetSurface(), "Upi.Prims");
  m_ChecksSubscription = m_pTree->m_ChecksChanged.AddEventHandler(plMakeDelegate(&plUsdPartitionImportEngineDlg::OnChecksChanged, this));

  m_pProcess = PL_DEFAULT_NEW(plEditorUiProcessView, *GetSurface(), "Upi.Process");
  m_pProcess->SetPlaceholder(s_szIdle);

  SetOptions(options);

  m_Timers.Start(plTime::MakeFromMilliseconds(100), [this]()
    { OnTick(); }, true);
}

plUsdPartitionImportEngineDlg::~plUsdPartitionImportEngineDlg()
{
  m_Timers.StopAll();

  // The tree and the process view draw into the window; it goes while they still live.
  CloseWindow();

  plDocumentManager::s_Events.RemoveEventHandler(m_DocumentSubscription);

  if (m_pTree != nullptr)
    m_pTree->m_ChecksChanged.RemoveEventHandler(m_ChecksSubscription);

  // Workers finish on their own and free what they read.
  if (m_pRead != nullptr)
    m_pRead->m_bCancel.Set(true);
  if (m_pScan != nullptr)
    m_pScan->m_bCancel.Set(true);
}

plVec2 plUsdPartitionImportEngineDlg::GetPreferredSize() const
{
  const float fForm = 3.0f * s_fRowPitch + 2.0f * s_fInfoLine + plEditorUiForm::RowGap;
  const float fHeight = s_fTop + fForm + s_fGap + s_fSectionPitch + s_fPreferredTreeHeight + s_fGap + GetVariantsHeight() + s_fGap + GetOptionsHeight() + s_fGap + s_fProcessHeight + s_fBottom;
  return plVec2(s_fWidth, fHeight + plEditorUiDialogHost::FooterHeight);
}

void plUsdPartitionImportEngineDlg::DocumentManagerEventHandler(const plDocumentManager::Event& e)
{
  if (e.m_Type == plDocumentManager::Event::Type::DocumentClosing && e.m_pDocument == m_pScene)
  {
    m_pScene = nullptr;
    Close(plEngineDialog::Rejected);
  }
}

//////////////////////////////////////////////////////////////////////////
// The stage

void plUsdPartitionImportEngineDlg::SetOptions(const plUsdPartitionImportOptions& options)
{
  m_InitialOptions = options;

  m_sStageText = options.m_sStageFile;
  m_sStageCommitted = options.m_sStageFile;
  m_sPartitionText = options.m_sPartitionName;
  WriteText(s_szStageText, m_sStageText);
  WriteText(s_szPartitionText, m_sPartitionText);

  m_CellSizes.Clear();
  for (float fSize : plUsdPartitionImportUi::CellSizes)
    m_CellSizes.PushBack(fSize);
  if (!m_CellSizes.Contains(options.m_fCellSize))
    m_CellSizes.PushBack(options.m_fCellSize);
  m_fCellSize = options.m_fCellSize;

  m_bBatch = options.m_fBatchSize > 0.0f;
  SetNumber(BatchNumber, options.m_fBatchSize > 0.0f ? options.m_fBatchSize : 8.0f);

  m_bSplit = options.m_fSplitSize >= 0.0f;
  SetNumber(SplitNumber, options.m_fSplitSize > 0.0f ? options.m_fSplitSize : options.m_fCellSize);
  m_bSplitFollowsCell = options.m_fSplitSize <= 0.0f;

  m_bLights = options.m_bLightsAndCameras;

  m_bRegion = options.m_Region.IsValid();
  SetNumber(RegionXFrom, m_bRegion ? options.m_Region.m_vMin.x : -512.0);
  SetNumber(RegionYFrom, m_bRegion ? options.m_Region.m_vMin.y : -512.0);
  SetNumber(RegionXTo, m_bRegion ? options.m_Region.m_vMax.x : 512.0);
  SetNumber(RegionYTo, m_bRegion ? options.m_Region.m_vMax.y : 512.0);

  if (m_bExistingPartition)
    SetTitle(plStringBuilder("USD Import Settings - ", options.m_sPartitionName));

  UpdateButtons();
  ReadStage();
  MarkChanged();
}

void plUsdPartitionImportEngineDlg::ReadStage()
{
  const plString sStage = Trimmed(m_sStageCommitted);
  if (sStage == m_sReadStage && m_pRead != nullptr)
    return;

  if (m_pRead != nullptr)
    m_pRead->m_bCancel.Set(true);
  if (m_pScan != nullptr)
    m_pScan->m_bCancel.Set(true);
  m_pRead.Clear();
  m_pScan.Clear();

  m_sReadStage = sStage;
  m_bTreeFilled = false;
  m_bSizesDone = false;
  m_PrimItems.Clear();
  m_ItemPrims.Clear();
  m_pTree->Clear();
  m_VariantRows.Clear();

  m_pProcess->Clear();
  m_pProcess->SetPlaceholder(s_szIdle);

  if (sStage.IsEmpty())
    SetStageInfo("Choose a stage.");
  else if (!plOSFile::ExistsFile(sStage))
    SetStageInfo("The file does not exist.");
  else
  {
    m_pRead = plUsdPartitionStageRead::StartListing(sStage, m_InitialOptions);
    SetStageInfo("Reading the stage...");
  }

  MarkChanged();
  Rebuild();
}

void plUsdPartitionImportEngineDlg::OnTick()
{
  plUsdPartitionImportUi::ForgetFinishedTasks();

  if (m_pRead != nullptr && !m_bTreeFilled)
  {
    if (!m_pRead->m_bListed)
    {
      SetStageInfo(plStringBuilder("Reading the stage: ", m_pRead->GetProgress(), "..."));
    }
    else if (m_pRead->m_Result.Failed())
    {
      SetStageInfo(plStringBuilder("The stage could not be read: ", m_pRead->m_Result.m_sMessage));
      m_bTreeFilled = true;
      m_bSizesDone = true;
      MarkChanged();
    }
    else
    {
      FillTree();
      FillVariants();
      m_bTreeFilled = true;

      m_sStageInfo = m_pRead->FormatStageInfo();
      SetStageInfo(m_sStageInfo);
      MarkChanged();
      Rebuild();
    }
  }

  if (m_pRead != nullptr && m_bTreeFilled && !m_bSizesDone)
    ShowSizes();

  // Another import may start or finish while the dialog is open; a running import and a scan would both hold a root.
  if (IsScanning() && IsImportRunning())
    m_pScan->m_bCancel.Set(true);

  if (m_pScan != nullptr)
  {
    if (!m_pScan->m_bDone)
    {
      m_pProcess->SetTitle(plStringBuilder("Scanning ", m_pScan->GetProgress(), "..."));
      m_pProcess->SetProgress(m_pScan->m_Roots.IsEmpty() ? 0.0f : static_cast<float>(m_pScan->m_iRootsDone) / m_pScan->m_Roots.GetCount());
      RequestRedraw();
    }
    else if (!m_bScanShown)
    {
      m_bScanShown = true;
      ShowScanResult();
    }
  }

  UpdateState();
}

void plUsdPartitionImportEngineDlg::ShowSizes()
{
  // Read before taking the sizes, so none measured in between is missed.
  const bool bDone = m_pRead->m_bDone;

  plDynamicArray<plUsdPartitionStageRead::Size> sizes;
  plUInt32 uiMeasured = 0, uiTotal = 0;
  m_pRead->TakeSizes(sizes, uiMeasured, uiTotal);

  for (const plUsdPartitionStageRead::Size& size : sizes)
  {
    const plUInt32 uiItem = size.m_uiPrim < m_PrimItems.GetCount() ? m_PrimItems[size.m_uiPrim] : plInvalidIndex;
    if (uiItem != plInvalidIndex)
      m_pTree->SetSize(uiItem, plUsdPartitionImportUi::FormatSize(size.m_Bounds), s_szSizeToolTip);
  }

  if (bDone)
  {
    m_bSizesDone = true;
    SetStageInfo(m_sStageInfo);

    // the measured sizes may need a wider column
    Rebuild();
  }
  else if (uiTotal > 0)
  {
    plStringBuilder sInfo;
    sInfo.SetFormat("{}; measuring sizes {}/{}", m_sStageInfo, uiMeasured, uiTotal);
    SetStageInfo(sInfo);
  }
}

void plUsdPartitionImportEngineDlg::FillTree()
{
  const plDynamicArray<plUsdPartitionPrimEntry>& prims = m_pRead->m_Prims;

  m_PrimItems.Clear();
  m_PrimItems.SetCount(prims.GetCount(), plInvalidIndex);
  m_ItemPrims.Clear();

  // Roots chosen before: checked when listed, kept as they are otherwise.
  const bool bChosenRoots = !m_InitialOptions.m_Roots.IsEmpty();

  plDynamicArray<plUsdPrimTreeView::Item> items;
  for (plUInt32 i = 0; i < prims.GetCount(); ++i)
  {
    const plUsdPartitionPrimEntry& prim = prims[i];

    // Prims leading to payload prims, and the prims that can be roots; the rest is part of a root.
    if (!prim.m_bCandidate && !prim.m_bPayloadBelow)
      continue;

    const plUInt32 uiParent = prim.m_uiParent != plInvalidIndex ? m_PrimItems[prim.m_uiParent] : plInvalidIndex;
    if (prim.m_uiParent != plInvalidIndex && uiParent == plInvalidIndex)
      continue;

    m_PrimItems[i] = items.GetCount();
    m_ItemPrims.PushBack(i);

    plUsdPrimTreeView::Item& item = items.ExpandAndGetRef();
    item.m_uiParent = uiParent;
    item.m_sName = prim.m_sName;
    item.m_sToolTip = prim.m_sPath;
    item.m_sType = prim.m_sType;
    item.m_sSize = plUsdPartitionImportUi::FormatSize(prim.m_Bounds);

    plStringBuilder sNotes;
    auto addNote = [&](plStringView sNote)
    {
      if (!sNotes.IsEmpty())
        sNotes.Append(", ");
      sNotes.Append(sNote);
    };

    if (prim.m_bPayload)
      addNote("payload");
    for (const plUsdVariantSet& set : prim.m_VariantSets)
      addNote(plStringBuilder(set.m_sName, ": ", set.m_sSelection.IsEmpty() ? plStringView("-") : plStringView(set.m_sSelection)));
    if (prim.m_bExcluded)
      addNote("excluded");
    item.m_sNotes = sNotes;

    if (prim.m_bExcluded)
    {
      item.m_bEnabled = false;
      item.m_sToolTip = plStringBuilder(prim.m_sPath, " is never imported.");
      continue;
    }

    if (prim.m_bCandidate)
    {
      item.m_bCheckable = true;
      item.m_bChecked = bChosenRoots ? m_InitialOptions.m_Roots.Contains(prim.m_sPath) : prim.m_bDefault;
    }
    else
    {
      // Checks or unchecks the payload prims below it.
      item.m_bAutoTristate = true;
    }
  }

  const plUInt32 uiShown = items.GetCount();
  m_pTree->SetItems(std::move(items));

  // Expanded down to the payload prims, unless that lists hundreds.
  if (uiShown <= s_uiExpandAllRows)
    m_pTree->ExpandAll();
  else
    m_pTree->ExpandToDepth(1);
}

void plUsdPartitionImportEngineDlg::FillVariants()
{
  m_VariantRows.Clear();

  // Every set on a prim that can be a root, with all variants any of them has.
  plDynamicArray<plUsdVariantSet> sets;
  m_pRead->GatherVariantSets(sets);

  for (const plUsdVariantSet& set : sets)
  {
    VariantRow& row = m_VariantRows.ExpandAndGetRef();
    row.m_sSet = set.m_sName;
    row.m_sDefault = plUsdPartitionImportUi::GetDefaultVariant(set);
    row.m_sStageSelection = set.m_sSelection;
    row.m_Variants = set.m_Variants;

    // the earlier choice for every root, else the default; the first variant when the set no longer has it
    const plString sChosen = plUsdPartitionImportUi::GetChosenVariant(set, m_InitialOptions);
    row.m_sChosen = set.m_Variants.Contains(sChosen) || set.m_Variants.IsEmpty() ? sChosen : set.m_Variants[0];
  }

  UpdateVariantRows();
}

void plUsdPartitionImportEngineDlg::UpdateVariantRows()
{
  if (m_pRead == nullptr || !m_pRead->m_bListed || m_pRead->m_Result.Failed())
    return;

  plDynamicArray<plString> roots;
  bool bDefault = false;
  CollectCheckedRoots(roots, bDefault);

  // A set is offered while a checked root has it.
  plHashSet<plString> usedSets;
  m_pRead->GetUsedVariantSets(roots, usedSets);

  bool bChanged = false;
  for (VariantRow& row : m_VariantRows)
  {
    const bool bUsed = usedSets.Contains(row.m_sSet);
    bChanged |= bUsed != row.m_bShown;
    row.m_bShown = bUsed;
  }

  if (bChanged)
    Rebuild();
}

void plUsdPartitionImportEngineDlg::OnChecksChanged(plUInt32 uiItem)
{
  PL_IGNORE_UNUSED(uiItem);

  UpdateVariantRows();
  MarkChanged();
}

void plUsdPartitionImportEngineDlg::Browse()
{
  if (m_bExistingPartition)
    return;

  const plString sFile = plFileDialog::GetOpenFileName("USD Stage", Trimmed(m_sStageText), "USD (*.usd *.usda *.usdc *.usdz)");
  if (sFile.IsEmpty())
    return;

  m_sStageText = sFile;
  WriteText(s_szStageText, m_sStageText);

  if (m_sPartitionText.IsEmpty())
  {
    m_sPartitionText = plStringView(sFile).GetFileName();
    WriteText(s_szPartitionText, m_sPartitionText);
  }

  m_sStageCommitted = m_sStageText;
  ReadStage();
}

//////////////////////////////////////////////////////////////////////////
// Scan and import

void plUsdPartitionImportEngineDlg::StartScan()
{
  plUsdPartitionImportOptions options;
  plString sError;
  if (!GetOptions(options, sError))
  {
    MarkChanged();
    return;
  }

  plDynamicArray<plString> roots;
  bool bDefault = false;
  CollectCheckedRoots(roots, bDefault);
  m_pScan = plUsdPartitionStageRead::StartScan(options, roots);
  m_bScanShown = false;

  m_pProcess->Clear();
  m_pProcess->SetPlaceholder(s_szIdle);
  m_pProcess->Start();
  m_pProcess->SetTitle("Scanning...");
  m_pProcess->SetProgress(0.0f);
  RequestRedraw();

  UpdateState();
}

void plUsdPartitionImportEngineDlg::ShowScanResult()
{
  const plUsdPartitionStageRead& scan = *m_pScan;

  if (scan.m_Result.Failed())
  {
    const plString sText = plStringBuilder("The scan failed: ", scan.m_Result.m_sMessage);
    m_pProcess->AppendLine(sText, plEditorUiProcessLineStyle::Error);
    m_pProcess->Finish(plEditorUiProcessState::Failed);
  }
  else if (scan.m_bCancel)
  {
    m_pProcess->AppendLine("The scan was stopped.", plEditorUiProcessLineStyle::Warning);
    m_pProcess->Finish(plEditorUiProcessState::Cancelled);
  }
  else
  {
    m_pProcess->AppendLine(scan.FormatScanResult(), plEditorUiProcessLineStyle::Output);

    const plString sNotScanned = scan.FormatNotScanned();
    if (!sNotScanned.IsEmpty())
      m_pProcess->AppendLine(sNotScanned, plEditorUiProcessLineStyle::Warning);

    m_pProcess->Finish(plEditorUiProcessState::Finished);
  }

  RequestRedraw();
  UpdateState();
}

void plUsdPartitionImportEngineDlg::Import()
{
  plUsdPartitionImporter* pImporter = plUsdPartitionImporter::GetSingleton();
  if (pImporter == nullptr || m_pScene == nullptr)
    return;

  // a stage typed but not read yet starts its read instead
  CommitFields("*");
  UpdateState();
  if (!GetHost().IsButtonEnabled(s_szImport))
    return;

  plUsdPartitionImportOptions options;
  plString sError;
  if (!GetOptions(options, sError))
    return;

  // A new import never takes over an existing partition; its settings dialog changes and imports it again.
  const plSceneWorldPartition* pExisting = FindPartition(m_pScene, options.m_sPartitionName);
  if (pExisting != nullptr && !m_bExistingPartition)
    return;

  if (pExisting != nullptr && !plUsdPartitionPanelExtension::ConfirmRewritingCells(m_pScene, pExisting->m_Partition))
    return;

  // The dialog's reads hold the stage too; the import opens it again on its own.
  if (m_pScan != nullptr)
    m_pScan->m_bCancel.Set(true);

  const plStatus res = pImporter->Start(m_pScene, options);
  if (res.Failed())
  {
    m_sStartError = res.m_sMessage;
    UpdateState();
    return;
  }

  if (plEditorUiWorldStreaming* pPanel = plEditorUiWorldStreaming::GetShown())
    pPanel->ShowPartition(pImporter->GetPartition());

  Close(plEngineDialog::Accepted);
}

//////////////////////////////////////////////////////////////////////////
// Options

bool plUsdPartitionImportEngineDlg::IsRead() const
{
  return m_pRead != nullptr && m_bTreeFilled && m_pRead->m_Result.Succeeded();
}

bool plUsdPartitionImportEngineDlg::IsScanning() const
{
  return m_pScan != nullptr && !m_pScan->m_bDone;
}

bool plUsdPartitionImportEngineDlg::IsImportRunning()
{
  const plUsdPartitionImporter* pImporter = plUsdPartitionImporter::GetSingleton();
  return pImporter != nullptr && pImporter->IsRunning();
}

void plUsdPartitionImportEngineDlg::CollectCheckedRoots(plDynamicArray<plString>& out_roots, bool& out_bDefault) const
{
  out_roots.Clear();
  out_bDefault = true;

  if (m_pRead == nullptr || !m_bTreeFilled || m_pRead->m_Result.Failed())
  {
    out_roots = m_InitialOptions.m_Roots;
    out_bDefault = m_InitialOptions.m_Roots.IsEmpty();
    return;
  }

  plHashSet<plString> checked;
  for (plUInt32 i = 0; i < m_pTree->GetItemCount(); ++i)
  {
    const plUsdPrimTreeView::Item& item = m_pTree->GetItem(i);
    if (item.m_bCheckable && item.m_bEnabled && m_pTree->GetCheck(i) == plUsdPrimTreeView::Check::Checked)
      checked.Insert(m_pRead->m_Prims[m_ItemPrims[i]].m_sPath);
  }

  m_pRead->CollectRoots(checked, m_InitialOptions, out_roots, out_bDefault);
}

bool plUsdPartitionImportEngineDlg::GetOptions(plUsdPartitionImportOptions& out_options, plString& out_sError) const
{
  plUsdPartitionImportChoices choices;
  choices.m_sStageFile = Trimmed(m_bExistingPartition ? m_sStageCommitted : m_sStageText);
  choices.m_sPartitionName = Trimmed(m_sPartitionText);
  choices.m_fCellSize = m_fCellSize;
  CollectCheckedRoots(choices.m_Roots, choices.m_bDefaultRoots);

  for (const VariantRow& row : m_VariantRows)
    choices.m_Variants.PushBack({row.m_sSet, row.m_sDefault, row.m_sChosen});

  choices.m_bBatch = m_bBatch;
  choices.m_fBatchSize = static_cast<float>(m_Numbers[BatchNumber].m_fValue);
  choices.m_bSplit = m_bSplit;
  choices.m_fSplitSize = static_cast<float>(m_Numbers[SplitNumber].m_fValue);
  choices.m_bLightsAndCameras = m_bLights;
  choices.m_bRegion = m_bRegion;
  for (plUInt32 i = 0; i < 4; ++i)
    choices.m_fRegion[i] = static_cast<float>(m_Numbers[RegionXFrom + i].m_fValue);

  return plUsdPartitionImportUi::BuildOptions(m_InitialOptions, choices, out_options, out_sError);
}

//////////////////////////////////////////////////////////////////////////
// Validation and buttons

void plUsdPartitionImportEngineDlg::MarkChanged()
{
  m_bOptionsDirty = true;
  m_sStartError.Clear();
  UpdateState();
}

void plUsdPartitionImportEngineDlg::UpdateState()
{
  if (!IsReady())
    return;

  const bool bRead = IsRead();
  const bool bScanning = IsScanning();
  const bool bImportRunning = IsImportRunning();

  plEditorUi::WriteEnabled(GetSurface()->GetBlackboard(), s_szSelectAll, bRead && m_bTreeFilled && !bScanning && !bImportRunning && m_pTree->GetItemCount() > 0);

  if (m_bOptionsDirty)
  {
    m_bOptionsDirty = false;
    WriteText(s_szSelectAllText, m_pTree->AreAllChecked() ? "Select none" : "Select all");

    plUsdPartitionImportOptions options;
    plString sError;
    m_bOptionsValid = GetOptions(options, sError);

    // no root is a problem once the stage is read; until then the info line says how the read goes
    m_sOptionsError = sError;
    if (!bRead && sError == "Check at least one prim to import.")
      m_sOptionsError.Clear();

    m_sNameTaken.Clear();
    if (m_bOptionsValid && !m_bExistingPartition && m_pScene != nullptr)
    {
      if (const plSceneWorldPartition* pExisting = FindPartition(m_pScene, options.m_sPartitionName))
      {
        plStringBuilder sTaken;
        sTaken.SetFormat("A partition named '{}' exists; choose another name, or use 'Open import settings' of that partition to import it again.", pExisting->m_sName);
        m_sNameTaken = sTaken;
      }
    }
  }

  plEditorUiSeverity severity = plEditorUiSeverity::None;
  plStringView sMessage;
  if (!m_sOptionsError.IsEmpty())
    sMessage = m_sOptionsError;
  else if (!m_sNameTaken.IsEmpty())
    sMessage = m_sNameTaken;
  else if (!m_sStartError.IsEmpty())
    sMessage = m_sStartError;

  if (!sMessage.IsEmpty())
  {
    severity = plEditorUiSeverity::Error;
  }
  else if (bImportRunning)
  {
    severity = plEditorUiSeverity::Info;
    sMessage = s_szRunning;
  }

  if (GetHost().GetValidationText() != sMessage || GetHost().GetValidationSeverity() != severity)
    GetHost().SetValidation(severity, sMessage);

  if (bScanning != m_bButtonsScanning || bImportRunning != m_bButtonsImportRunning)
  {
    m_bButtonsScanning = bScanning;
    m_bButtonsImportRunning = bImportRunning;
    UpdateButtons();
  }

  // Stop stays available; a new scan waits for a running import, which holds a root of its own in memory.
  GetHost().SetButtonEnabled(s_szScan, bRead && (bScanning || (!bImportRunning && m_bOptionsValid)));
  GetHost().SetButtonEnabled(s_szImport, bRead && m_bOptionsValid && m_sNameTaken.IsEmpty() && !bScanning && !bImportRunning && m_pScene != nullptr);
}

void plUsdPartitionImportEngineDlg::UpdateButtons()
{
  using Role = plEditorUiDialogButton::Role;

  plEditorUiDialogButton buttons[3];
  buttons[0].m_sId = s_szScan;
  buttons[0].m_sLabel = m_bButtonsScanning ? "Stop" : "Scan";
  buttons[0].m_Role = Role::Left;
  buttons[0].m_sToolTip = m_bButtonsImportRunning && !m_bButtonsScanning ? s_szRunning : s_szScanToolTip;

  buttons[1].m_sId = s_szImport;
  buttons[1].m_sLabel = m_bExistingPartition ? "Import again" : "Import";
  buttons[1].m_Role = Role::Accept;
  buttons[1].m_sToolTip = m_bButtonsImportRunning ? s_szRunning : "";

  buttons[2].m_sId = s_szClose;
  buttons[2].m_sLabel = "Close";
  buttons[2].m_Role = Role::Reject;

  // the enabled states stay until UpdateState sets them
  for (plEditorUiDialogButton& button : buttons)
    button.m_bEnabled = GetHost().IsButtonEnabled(button.m_sId) || button.m_sId == s_szClose;

  GetHost().SetButtons(buttons);
}

void plUsdPartitionImportEngineDlg::SetStageInfo(plStringView sText)
{
  WriteText(s_szStageInfo, sText);
}

//////////////////////////////////////////////////////////////////////////
// Fields

plString plUsdPartitionImportEngineDlg::ReadText(plStringView sEntry) const
{
  const plVariant value = GetSurface()->GetBlackboard().GetEntryValue(plTempHashedString(plString(sEntry).GetData()));
  return value.IsValid() ? value.ConvertTo<plString>() : plString();
}

void plUsdPartitionImportEngineDlg::WriteText(plStringView sEntry, plStringView sText)
{
  if (!IsReady())
    return;

  GetSurface()->GetCanvas().SetTextBinding(sEntry, sText);
  GetSurface()->GetBlackboard().SetEntryValue(sEntry, plString(sText));
}

void plUsdPartitionImportEngineDlg::CommitFields(plStringView sSubmitted)
{
  // a field commits once it lost focus, or right away for sSubmitted ("*" commits every field)
  const plStringView sFocused = GetSurface()->GetCanvas().GetFocusedElementName();
  auto commits = [&](plStringView sName)
  { return sFocused != sName || sSubmitted == "*" || sSubmitted == sName; };

  if (!m_bExistingPartition && m_sStageText != m_sStageCommitted && commits(s_szStage))
  {
    m_sStageCommitted = m_sStageText;
    ReadStage();
  }

  for (plUInt32 i = 0; i < NumberCount; ++i)
  {
    const plString sText = ReadText(plStringBuilder(m_Numbers[i].m_szName, ".Text"));
    if (sText != m_Numbers[i].m_sCommitted && commits(m_Numbers[i].m_szName))
      CommitNumber(i, sText);
  }
}

void plUsdPartitionImportEngineDlg::CommitNumber(plUInt32 uiNumber, plStringView sText)
{
  Number& number = m_Numbers[uiNumber];

  // as a spin box: text that is no number gives the last value back, numbers clamp to the range
  double fValue = number.m_fValue;
  if (plConversionUtils::StringToFloat(Trimmed(sText), fValue).Failed() || !plMath::IsFinite(fValue))
    fValue = number.m_fValue;

  SetNumber(uiNumber, fValue);

  if (uiNumber == SplitNumber)
    m_bSplitFollowsCell = static_cast<float>(number.m_fValue) == m_fCellSize;

  MarkChanged();
}

void plUsdPartitionImportEngineDlg::SetNumber(plUInt32 uiNumber, double fValue)
{
  Number& number = m_Numbers[uiNumber];

  const double fScale = number.m_uiDecimals == 0 ? 1.0 : 10.0;
  number.m_fValue = plMath::Round(plMath::Clamp(fValue, number.m_fMin, number.m_fMax) * fScale) / fScale;
  number.m_sCommitted = FormatNumber(number.m_fValue, number.m_uiDecimals);
  WriteText(plStringBuilder(number.m_szName, ".Text"), number.m_sCommitted);
}

void plUsdPartitionImportEngineDlg::SetCellSize(float fSize)
{
  m_fCellSize = fSize;

  // A split size that followed the old cell size follows the new one.
  if (!m_bSplit || m_bSplitFollowsCell)
  {
    SetNumber(SplitNumber, fSize);
    m_bSplitFollowsCell = true;
  }

  MarkChanged();
  Rebuild();
}

//////////////////////////////////////////////////////////////////////////
// Menus

void plUsdPartitionImportEngineDlg::ShowMenu(const plRectFloat& field, plArrayPtr<const plString> items, plInt32 iChecked, plDelegate<void(plUInt32)> onPick)
{
  plEditorUiMenu menu;
  menu.m_fMinWidth = plMath::Max(field.width, 120.0f);

  for (plUInt32 i = 0; i < items.GetCount(); ++i)
  {
    plEditorUiMenuItem& item = menu.AddAction(items[i], i);
    item.m_bCheckable = true;
    item.m_bChecked = static_cast<plInt32>(i) == iChecked;
  }

  const plVec2 vPos(GetBodyRect().x + field.x, GetBodyRect().y + field.y + field.height);
  plEditorUiMenuView::Show(*GetSurface(), vPos, menu, onPick);
}

void plUsdPartitionImportEngineDlg::ShowCellSizeMenu()
{
  plHybridArray<plString, 5> items;
  plInt32 iChecked = -1;
  for (plUInt32 i = 0; i < m_CellSizes.GetCount(); ++i)
  {
    if (m_CellSizes[i] == m_fCellSize)
      iChecked = static_cast<plInt32>(i);
    items.PushBack(plUsdPartitionImportUi::FormatCellSize(m_CellSizes[i]));
  }

  ShowMenu(m_CellSizeRect, items, iChecked, [this](plUInt32 uiPicked)
    {
      if (uiPicked < m_CellSizes.GetCount())
        SetCellSize(m_CellSizes[uiPicked]); });
}

void plUsdPartitionImportEngineDlg::ShowRegionMenu()
{
  const plString items[] = {s_szRegionModes[0], s_szRegionModes[1]};
  ShowMenu(m_RegionRect, items, m_bRegion ? 1 : 0, [this](plUInt32 uiPicked)
    {
      if (uiPicked > 1 || (uiPicked == 1) == m_bRegion)
        return;

      m_bRegion = uiPicked == 1;
      MarkChanged();
      Rebuild(); });
}

void plUsdPartitionImportEngineDlg::ShowVariantMenu(plUInt32 uiRow)
{
  if (uiRow >= m_VariantRows.GetCount())
    return;

  const VariantRow& row = m_VariantRows[uiRow];

  plDynamicArray<plString> items;
  plInt32 iChecked = -1;
  for (plUInt32 i = 0; i < row.m_Variants.GetCount(); ++i)
  {
    plStringBuilder sText = row.m_Variants[i];
    if (row.m_Variants[i] == row.m_sStageSelection)
      sText.Append(" (stage)");
    items.PushBack(sText);

    if (row.m_Variants[i] == row.m_sChosen)
      iChecked = static_cast<plInt32>(i);
  }

  ShowMenu(row.m_FieldRect, items, iChecked, [this, uiRow](plUInt32 uiPicked)
    {
      if (uiRow >= m_VariantRows.GetCount() || uiPicked >= m_VariantRows[uiRow].m_Variants.GetCount())
        return;

      m_VariantRows[uiRow].m_sChosen = m_VariantRows[uiRow].m_Variants[uiPicked];
      MarkChanged();
      Rebuild(); });
}

//////////////////////////////////////////////////////////////////////////
// Building

void plUsdPartitionImportEngineDlg::Section(plUiElement* pBody, float fY, float fWidth, plStringView sTitle)
{
  plUiPanel* pHolder = plEditorUi::Panel(pBody);
  plEditorUi::SetRect(pHolder, plRectFloat(plEditorUiDialogHost::Padding, fY, fWidth, plEditorUi::SectionHeaderHeight));
  pHolder->m_Color = s_Transparent;
  plEditorUi::SectionHeader(pHolder, sTitle, SectionColor());
}

void plUsdPartitionImportEngineDlg::AddToolTip(const plRectFloat& rect, plStringView sText)
{
  plEditorUiForm::ToolTip toolTip;
  toolTip.m_Rect = rect;
  toolTip.m_sText = sText;
  AddBodyToolTips(plArrayPtr<const plEditorUiForm::ToolTip>(&toolTip, 1));
}

float plUsdPartitionImportEngineDlg::GetVariantsHeight() const
{
  plUInt32 uiShown = 0;
  for (const VariantRow& row : m_VariantRows)
    uiShown += row.m_bShown ? 1 : 0;

  return s_fSectionPitch + (uiShown > 0 ? uiShown * s_fRowPitch : s_fHintHeight);
}

float plUsdPartitionImportEngineDlg::GetOptionsHeight() const
{
  return s_fSectionPitch + 4.0f * s_fCheckPitch + s_fRowPitch + (m_bRegion ? s_fRowPitch : 0.0f);
}

void plUsdPartitionImportEngineDlg::BuildBody(plUiElement* pBody, const plVec2& vSize)
{
  using namespace plEditorUi;

  const float x = plEditorUiDialogHost::Padding;
  const float fWidth = plMath::Max(vSize.x - 2.0f * x, 200.0f);

  float y = BuildStageForm(pBody, fWidth) + s_fGap;

  // the tree takes what the sections below leave
  const float fBelow = s_fGap + GetVariantsHeight() + s_fGap + GetOptionsHeight() + s_fGap + s_fProcessHeight + s_fBottom;
  const float fTreeHeight = plMath::Max(vSize.y - y - s_fSectionPitch - fBelow, s_fMinTreeHeight);

  Section(pBody, y, fWidth, "Prims");
  const float fSelectAllWidth = plMath::Max(PushButtonWidth("Select all"), PushButtonWidth("Select none"));
  plBlackboard& board = GetSurface()->GetBlackboard();
  WriteText(s_szSelectAllText, m_pTree->AreAllChecked() ? "Select none" : "Select all");
  plUiButton* pSelectAll = PushButton(pBody, board, plRectFloat(x + fWidth - fSelectAllWidth, y, fSelectAllWidth, SectionHeaderHeight), "Select all", s_szSelectAll,
    IsRead() && m_bTreeFilled && !IsScanning() && !IsImportRunning() && m_pTree->GetItemCount() > 0);
  for (plUiElement* pChild : pSelectAll->m_Children)
  {
    if (plUiText* pText = plDynamicCast<plUiText*>(pChild))
    {
      pText->m_sTextBinding = s_szSelectAllText;
    }
  }
  SetToolTip(board, s_szSelectAll, "Check every importable prim, or clear all checks when all are checked, including collapsed branches.");
  y += s_fSectionPitch;

  m_TreeRect = plRectFloat(x, y, fWidth, fTreeHeight);
  m_pTree->Build(pBody, m_TreeRect);
  y += fTreeHeight + s_fGap;

  y = BuildVariants(pBody, y, fWidth) + s_fGap;
  y = BuildOptions(pBody, y, fWidth) + s_fGap;

  m_pProcess->Build(pBody, plRectFloat(x, y, fWidth, s_fProcessHeight));
}

float plUsdPartitionImportEngineDlg::BuildStageForm(plUiElement* pBody, float fWidth)
{
  using namespace plEditorUi;

  plBlackboard& board = GetSurface()->GetBlackboard();

  plEditorUiForm form(pBody, plRectFloat(plEditorUiDialogHost::Padding, s_fTop, fWidth, 1000.0f), s_fLabelWidth);

  // USD stage, with Browse
  const plRectFloat stage = form.Row("USD stage");
  const float fBrowse = PushButtonWidth("Browse...");
  const plRectFloat stageField(stage.x, stage.y, plMath::Max(stage.width - fBrowse - 6.0f, 40.0f), stage.height);
  if (m_bExistingPartition)
    ReadOnlyField(pBody, stageField, m_sStageCommitted, true);
  else
    TextField(pBody, stageField, s_szStage, s_szStageText, s_szStagePlaceholder, s_szStageSubmit, true);
  PushButton(pBody, board, plRectFloat(stageField.Right() + 6.0f, stage.y, fBrowse, stage.height), "Browse...", s_szBrowse, !m_bExistingPartition);

  // the stage info line wraps over two lines
  const plRectFloat info = form.FieldRow(2.0f * s_fInfoLine);
  plUiText* pInfo = TextAt(pBody, {}, info, Dim(), FontSize - 2);
  pInfo->m_sTextBinding = s_szStageInfo;
  pInfo->m_bWrap = true;
  pInfo->m_VAlign = plUiTextVAlign::Top;
  pInfo->m_Overflow = plUiTextOverflow::Ellipsis;

  const plRectFloat partition = form.Row("Partition", ControlHeight, s_szPartitionToolTip);
  if (m_bExistingPartition)
  {
    ReadOnlyField(pBody, partition, m_sPartitionText);
    AddToolTip(partition, s_szPartitionToolTip);
  }
  else
  {
    TextField(pBody, partition, s_szPartition, s_szPartitionText);
    SetToolTip(board, s_szPartition, s_szPartitionToolTip);
  }

  const plStringView sCellToolTip = m_bExistingPartition ? s_szCellSizeFixedToolTip : s_szCellSizeToolTip;
  const plRectFloat cell = form.Row("Cell size", ControlHeight, sCellToolTip);
  m_CellSizeRect = plRectFloat(cell.x, cell.y, plMath::Min(s_fCellSizeWidth, cell.width), cell.height);
  Dropdown(pBody, board, m_CellSizeRect, plUsdPartitionImportUi::FormatCellSize(m_fCellSize), s_szCellSize, !m_bExistingPartition);
  SetToolTip(board, s_szCellSize, sCellToolTip);
  if (m_bExistingPartition)
    AddToolTip(m_CellSizeRect, sCellToolTip);

  AddBodyToolTips(form.GetToolTips());
  return form.GetY() - plEditorUiForm::RowGap;
}

float plUsdPartitionImportEngineDlg::BuildVariants(plUiElement* pBody, float fY, float fWidth)
{
  using namespace plEditorUi;

  plBlackboard& board = GetSurface()->GetBlackboard();

  Section(pBody, fY, fWidth, "Variants");
  fY += s_fSectionPitch;

  plEditorUiForm form(pBody, plRectFloat(plEditorUiDialogHost::Padding, fY, fWidth, 1000.0f), s_fLabelWidth);

  plUInt32 uiShown = 0;
  for (plUInt32 i = 0; i < m_VariantRows.GetCount(); ++i)
  {
    VariantRow& row = m_VariantRows[i];
    if (!row.m_bShown)
      continue;

    plStringBuilder sToolTip;
    sToolTip.SetFormat(s_szVariantToolTip, row.m_sDefault);

    const plRectFloat field = form.Row(row.m_sSet, ControlHeight, sToolTip);
    row.m_FieldRect = plRectFloat(field.x, field.y, plMath::Min(s_fVariantWidth, field.width), field.height);

    plStringBuilder sText = row.m_sChosen;
    if (!row.m_sChosen.IsEmpty() && row.m_sChosen == row.m_sStageSelection)
      sText.Append(" (stage)");

    const plString sName = Indexed(s_szVariantPrefix, i);
    Dropdown(pBody, board, row.m_FieldRect, sText, sName, !row.m_Variants.IsEmpty());
    SetToolTip(board, sName, sToolTip);
    ++uiShown;
  }

  AddBodyToolTips(form.GetToolTips());

  if (uiShown > 0)
    return form.GetY() - plEditorUiForm::RowGap;

  TextAt(pBody, s_szNoVariants, plRectFloat(plEditorUiDialogHost::Padding, fY, fWidth, s_fHintHeight), Dim(), FontSize - 1);
  return fY + s_fHintHeight;
}

float plUsdPartitionImportEngineDlg::BuildOptions(plUiElement* pBody, float fY, float fWidth)
{
  using namespace plEditorUi;

  plBlackboard& board = GetSurface()->GetBlackboard();
  const float x = plEditorUiDialogHost::Padding;

  Section(pBody, fY, fWidth, "Options");
  fY += s_fSectionPitch;

  // the batch and split sizes and the region share a column after the longest check box
  const float fNumberX = x + plMath::Max(CheckWidth(s_szBatchLabel), CheckWidth(s_szSplitLabel)) + 8.0f;

  auto sizeRow = [&](const char* szLabel, const char* szAction, const char* szToolTip, bool bChecked, plUInt32 uiNumber)
  {
    CheckButton(pBody, board, plRectFloat(x, fY, CheckWidth(szLabel), ControlHeight), szLabel, szAction, bChecked);
    SetToolTip(board, szAction, szToolTip);

    const plRectFloat number(fNumberX, fY, s_fNumberWidth, ControlHeight);
    if (bChecked)
      NumberField(pBody, number, m_Numbers[uiNumber].m_szName, plStringBuilder(m_Numbers[uiNumber].m_szName, ".Text"), s_szNumberSubmit, {}, "m");
    else
      DisabledNumber(pBody, number, m_Numbers[uiNumber].m_sCommitted);

    fY += s_fCheckPitch;
  };

  sizeRow(s_szBatchLabel, s_szBatch, s_szBatchToolTip, m_bBatch, BatchNumber);
  sizeRow(s_szSplitLabel, s_szSplit, s_szSplitToolTip, m_bSplit, SplitNumber);

  CheckButton(pBody, board, plRectFloat(x, fY, plMath::Min(CheckWidth(s_szLightsLabel), fWidth), ControlHeight), s_szLightsLabel, s_szLights, m_bLights);
  fY += s_fCheckPitch;

  // always on: guide and proxy prims are never imported
  const plRectFloat skip(x, fY, CheckWidth(s_szSkipGuidesLabel), ControlHeight);
  CheckButton(pBody, board, skip, s_szSkipGuidesLabel, s_szSkipGuides, true, false);
  AddToolTip(skip, s_szSkipGuidesToolTip);
  fY += s_fCheckPitch;

  TextAt(pBody, s_szRegionLabel, plRectFloat(x, fY, fNumberX - x, ControlHeight), Muted());
  m_RegionRect = plRectFloat(fNumberX, fY, s_fRegionWidth, ControlHeight);
  Dropdown(pBody, board, m_RegionRect, s_szRegionModes[m_bRegion ? 1 : 0], s_szRegion);
  SetToolTip(board, s_szRegion, s_szRegionToolTip);
  fY += s_fRowPitch;

  if (m_bRegion)
  {
    float fX = x;
    for (plUInt32 i = 0; i < 4; ++i)
    {
      const float fLabel = plMath::Ceil(MeasureText(s_szRegionLabels[i], FontSize - 1)) + 1.0f;
      TextAt(pBody, s_szRegionLabels[i], plRectFloat(fX, fY, fLabel, ControlHeight), Muted());
      fX += fLabel + 6.0f;

      const Number& number = m_Numbers[RegionXFrom + i];
      NumberField(pBody, plRectFloat(fX, fY, s_fRegionNumberWidth, ControlHeight), number.m_szName, plStringBuilder(number.m_szName, ".Text"), s_szNumberSubmit, {}, "m");
      fX += s_fRegionNumberWidth + 14.0f;
    }

    fY += s_fRowPitch;
  }

  return fY - plEditorUiForm::RowGap;
}

//////////////////////////////////////////////////////////////////////////
// Events

void plUsdPartitionImportEngineDlg::OnUiUpdate()
{
  if (m_pProcess != nullptr && m_pProcess->Update())
    RequestRedraw();

  // the stage and the partition name are checked as they are typed
  const plString sStage = m_bExistingPartition ? m_sStageCommitted : ReadText(s_szStageText);
  const plString sPartition = m_bExistingPartition ? m_sPartitionText : ReadText(s_szPartitionText);
  if (sStage != m_sStageText || sPartition != m_sPartitionText)
  {
    m_sStageText = sStage;
    m_sPartitionText = sPartition;
    MarkChanged();
  }

  CommitFields({});
}

void plUsdPartitionImportEngineDlg::OnUiEvent(const plMsgUiEvent& event)
{
  const plStringView sId = event.m_sIdentifier.GetView();
  plUInt32 uiIndex = 0;

  if (sId == s_szStageSubmit)
  {
    CommitFields(s_szStage);
  }
  else if (sId == s_szNumberSubmit)
  {
    CommitFields("*");
  }
  else if (sId == s_szBrowse)
  {
    Browse();
  }
  else if (sId == s_szSelectAll)
  {
    if (IsRead() && m_bTreeFilled && !IsScanning() && !IsImportRunning())
    {
      m_pTree->SetAllChecked(!m_pTree->AreAllChecked());
      OnChecksChanged(plInvalidIndex);
    }
  }
  else if (sId == s_szCellSize)
  {
    if (!m_bExistingPartition)
      ShowCellSizeMenu();
  }
  else if (sId == s_szRegion)
  {
    ShowRegionMenu();
  }
  else if (ParseIndex(sId, s_szVariantPrefix, uiIndex))
  {
    ShowVariantMenu(uiIndex);
  }
  else if (sId == s_szBatch || sId == s_szSplit || sId == s_szLights)
  {
    // the check boxes flip on click; a size field shows only while its box is checked
    CommitFields("*");
    bool& ref_bValue = sId == s_szBatch ? m_bBatch : (sId == s_szSplit ? m_bSplit : m_bLights);
    ref_bValue = !ref_bValue;
    MarkChanged();
    Rebuild();
  }
}

void plUsdPartitionImportEngineDlg::OnButton(plStringView sId)
{
  if (sId == s_szScan)
  {
    if (IsScanning())
    {
      m_pScan->m_bCancel.Set(true);
      return;
    }

    CommitFields("*");
    UpdateState();
    if (GetHost().IsButtonEnabled(s_szScan))
      StartScan();
    return;
  }

  if (sId == s_szImport)
  {
    Import();
    return;
  }

  plEngineDialog::OnButton(sId);
}

plString plUsdPartitionImportEngineDlg::GetToolTipAt(const plVec2& vLogical) const
{
  const plVec2 vBody = vLogical - plVec2(GetBodyRect().x, GetBodyRect().y);
  if (m_pTree != nullptr && m_TreeRect.Contains(vBody))
    return m_pTree->GetToolTipAt(vBody - plVec2(m_TreeRect.x, m_TreeRect.y), s_szPrimsToolTip);

  return plEngineDialog::GetToolTipAt(vLogical);
}
