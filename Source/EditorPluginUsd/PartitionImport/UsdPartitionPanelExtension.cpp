#include <EditorPluginUsd/EditorPluginUsdPCH.h>

#include <EditorFramework/Assets/AssetBrowserContextMenu.h>
#include <EditorFramework/EngineShell/EngineShell.h>
#include <EditorPluginScene/Panels/WorldStreamingPanel/WorldStreamingContent.h>
#include <EditorPluginScene/Scene/Scene2Document.h>
#include <EditorPluginScene/WorldStreaming/WorldPartitionRegistry.h>
#include <EditorPluginScene/WorldStreaming/WorldStreamingEditor.h>
#include <EditorPluginUsd/PartitionImport/UsdPartitionImportDialog.h>
#include <EditorPluginUsd/PartitionImport/UsdPartitionPanelExtension.h>
#include <EditorUi/Platform/EditorPlatform.h>
#include <Foundation/IO/OSFile.h>
#include <GuiFoundation/DocumentWindow/DocumentWindow.h>
#include <GuiFoundation/UIServices/UIServices.h>
#include <UsdBridge/UsdStage.h>

namespace
{
  // Fixes of the import's problems: "<op>|<partition>|<problem type>".
  constexpr const char* FixUpdate = "update";
  constexpr const char* FixPaths = "paths";

  plString MakeFix(const char* szOp, const plUuid& partition, int iType)
  {
    plStringBuilder sGuid;
    plConversionUtils::ToString(partition, sGuid);
    plStringBuilder s;
    s.SetFormat("{}|{}|{}", szOp, sGuid, iType);
    return s;
  }

  const char* GetProblemText(plUsdImportProblem::Type type)
  {
    switch (type)
    {
      case plUsdImportProblem::Type::MeshFailed:
        return "mesh(es) could not be built, or left gprims out";
      case plUsdImportProblem::Type::NotSplit:
        return "mesh(es) could not be cut along the cells and are in the Distant layer whole";
      case plUsdImportProblem::Type::Oversized:
        return "instance(s) wider than the split size, placed by their centre";
      case plUsdImportProblem::Type::Distant:
        return "object(s) larger than the map, in the Distant layer";
      case plUsdImportProblem::Type::RootFailed:
        return "prim(s) could not be scanned";
      case plUsdImportProblem::Type::CellConflict:
        return "loaded cell(s) with unsaved changes were rewritten on disk";
    }
    return "problem(s)";
  }

  bool IsInformation(plUsdImportProblem::Type type)
  {
    return type == plUsdImportProblem::Type::NotSplit || type == plUsdImportProblem::Type::Oversized || type == plUsdImportProblem::Type::Distant;
  }

  constexpr int ProblemTypeCount = static_cast<int>(plUsdImportProblem::Type::CellConflict) + 1;

  constexpr const char* UpdateTooltip = "Scans the prims whose layer files changed and rewrites the cells whose content changed. Edits of imported objects are overwritten; objects you added are kept.";
  constexpr const char* SettingsTooltip = "Changes the prims, variants and options of the import and imports again.";

  const char* GetUpdateLabel(const plUsdPartitionImportManifest& manifest)
  {
    return manifest.m_bComplete ? "Update from USD" : "Resume import";
  }

  /// Whether the running import writes this partition; it then saves the partition's manifest often.
  bool IsImporting(const plScene2Document* pScene, const plUuid& partition)
  {
    const plUsdPartitionImporter* pImporter = plUsdPartitionImporter::GetSingleton();
    return pImporter != nullptr && pImporter->IsRunning() && pImporter->GetScene() == pScene && pImporter->GetPartition() == partition;
  }

  plString FormatDuration(plTime duration)
  {
    const int iSeconds = static_cast<int>(duration.GetSeconds());

    plStringBuilder s;
    if (iSeconds < 60)
      s.SetFormat("{} s", iSeconds);
    else if (iSeconds < 3600)
      s.SetFormat("{} min {} s", iSeconds / 60, iSeconds % 60);
    else
      s.SetFormat("{} h {} min", iSeconds / 3600, (iSeconds / 60) % 60);
    return s;
  }
} // namespace

plUsdPartitionPanelExtension::plUsdPartitionPanelExtension()
{
  if (plUsdPartitionImporter* pImporter = plUsdPartitionImporter::GetSingleton())
    m_ImporterSubscription = pImporter->m_Events.AddEventHandler(plMakeDelegate(&plUsdPartitionPanelExtension::ImporterEventHandler, this));

  m_AssetBrowserSubscription = plAssetBrowserContextMenu::s_Events.AddEventHandler(plMakeDelegate(&plUsdPartitionPanelExtension::AssetBrowserEventHandler, this));

  plWorldStreamingPanelExtension::Register(this);
}

plUsdPartitionPanelExtension::~plUsdPartitionPanelExtension()
{
  // The panel drops the menu entries and details of the extension when it unregisters.
  plWorldStreamingPanelExtension::Unregister(this);

  plAssetBrowserContextMenu::s_Events.RemoveEventHandler(m_AssetBrowserSubscription);

  if (plUsdPartitionImporter* pImporter = plUsdPartitionImporter::GetSingleton())
    pImporter->m_Events.RemoveEventHandler(m_ImporterSubscription);
}

plScene2Document* plUsdPartitionPanelExtension::GetTargetScene()
{
  if (plEditorUiWorldStreaming* pPanel = plEditorUiWorldStreaming::GetShown())
  {
    if (plScene2Document* pScene = pPanel->GetTrackedScene())
      return pScene;
  }

  // the active document tab first, then any open scene
  if (plEditorUiEngineShell* pShell = plEditorUiEngineShell::GetSingleton())
  {
    plEditorUiDocumentFrame* pFrame = pShell->GetActiveFrame();
    plDocument* pActive = pFrame != nullptr ? pFrame->GetDocument() : nullptr;
    if (plScene2Document* pScene = pActive != nullptr ? plDynamicCast<plScene2Document*>(pActive->GetMainDocument()) : nullptr)
      return pScene;
  }

  for (plDocumentWindow* pWindow : plDocumentWindow::GetAllDocumentWindows())
  {
    plDocument* pDocument = pWindow->GetDocument();
    if (pDocument != nullptr)
    {
      if (plScene2Document* pScene = plDynamicCast<plScene2Document*>(pDocument->GetMainDocument()))
        return pScene;
    }
  }

  return nullptr;
}

const plUsdPartitionImportManifest* plUsdPartitionPanelExtension::GetManifest(const plScene2Document* pScene, const plUuid& partition, bool bReadChanges)
{
  const plSceneWorldPartition* pPartition = pScene != nullptr ? pScene->GetPartition(partition) : nullptr;
  if (pPartition == nullptr)
    return nullptr;

  const plString sFile = plUsdPartitionImportManifest::GetPath(pScene->GetPartitionFolder(*pPartition));
  CachedManifest& cached = m_Manifests[sFile];
  if (!bReadChanges && cached.m_bValid)
    return &cached.m_Manifest;

  plFileStats stats;
  if (plOSFile::GetFileStats(sFile, stats).Failed())
  {
    cached.m_bValid = false;
    cached.m_iFileTime = 0;
    return nullptr;
  }

  const plInt64 iTime = stats.m_LastModificationTime.GetInt64(plSIUnitOfTime::Microsecond);
  if (iTime != cached.m_iFileTime || stats.m_uiFileSize != cached.m_uiFileSize)
  {
    cached.m_iFileTime = iTime;
    cached.m_uiFileSize = stats.m_uiFileSize;
    cached.m_Manifest = plUsdPartitionImportManifest();
    cached.m_bValid = cached.m_Manifest.Load(sFile).Succeeded() && cached.m_Manifest.m_Partition == partition;
  }

  return cached.m_bValid ? &cached.m_Manifest : nullptr;
}

bool plUsdPartitionPanelExtension::ConfirmRewritingCells(const plScene2Document* pScene, const plUuid& partition)
{
  const plWorldPartitionRegistry* pRegistry = pScene != nullptr ? pScene->GetWorldStreaming()->GetRegistry(partition) : nullptr;
  if (pRegistry == nullptr)
    return true;

  plUInt32 uiModified = 0;
  plStringBuilder sList;
  for (auto it : pRegistry->GetCells())
  {
    const plSceneDocument* pCell = pScene->GetLayerDocument(it.Value().m_Layer);
    if (pCell == nullptr || !pCell->IsModified())
      continue;

    if (++uiModified <= 10)
      sList.AppendFormat("\n  {}", plStringView(it.Value().m_sPath).GetFileName());
  }

  if (uiModified == 0)
    return true;

  if (uiModified > 10)
    sList.AppendFormat("\n  and {} more", uiModified - 10);

  return plEditorUiServices::MessageBoxQuestion(plFmt("{} loaded cell(s) of this partition have unsaved changes:{}\n\nThe import rewrites the cells whose USD "
                                                      "content changed on disk; such cells are then listed in Problems. Continue?",
                                                  uiModified, sList),
           plEditorUiMessageButton::Yes | plEditorUiMessageButton::No, plEditorUiMessageButton::No) == plEditorUiMessageButton::Yes;
}

void plUsdPartitionPanelExtension::StartUpdate(plScene2Document* pScene, const plUuid& partition)
{
  plUsdPartitionImporter* pImporter = plUsdPartitionImporter::GetSingleton();
  if (pImporter == nullptr || pScene == nullptr)
    return;

  if (!ConfirmRewritingCells(pScene, partition))
    return;

  // A button press, so the reason it cannot start is shown.
  const plStatus res = pImporter->Update(pScene, partition);
  if (res.Failed())
    plEditorUiServices::MessageBoxStatus(res, "The USD import could not start.");
}

void plUsdPartitionPanelExtension::ImporterEventHandler(const plUsdPartitionImportEvent& e)
{
  if (e.m_Type == plUsdPartitionImportEvent::Type::Started)
    m_Failures.Remove(e.m_Partition);
  else if (e.m_Type == plUsdPartitionImportEvent::Type::Finished && e.m_Result.Failed() && !e.m_bCancelled)
    m_Failures[e.m_Partition] = e.m_Result.m_sMessage;
}

void plUsdPartitionPanelExtension::AssetBrowserEventHandler(const plAssetBrowserContextMenuEvent& e)
{
  if (e.m_SelectedFiles.GetCount() != 1 || !plUsdStage::IsUsdFile(e.m_SelectedFiles[0]))
    return;

  // the stage becomes a world partition of the open scene; without one there is nothing to import into
  if (GetTargetScene() == nullptr)
    return;

  const plString sStage = e.m_SelectedFiles[0];
  e.m_AddAction("Import as World Partition...", ":/EditorPluginScene/Icons/WorldPartition.svg", [sStage]()
    {
      if (plScene2Document* pTarget = GetTargetScene())
        plUsdPartitionImportDialog::ShowForNewImport(pTarget, sStage); });
}

void plUsdPartitionPanelExtension::GetNewMenuEntries(plScene2Document* pScene, plDynamicArray<MenuEntry>& out_entries)
{
  MenuEntry& entry = out_entries.ExpandAndGetRef();
  entry.m_sLabel = "World partition from USD...";
  entry.m_sToolTip = "Imports a USD stage as the cells of a new world partition: each object goes into the cell under it, large meshes are cut along the cells.";
  entry.m_bEnabled = pScene != nullptr;
  entry.m_OnPick = [pScene]()
  { plUsdPartitionImportDialog::ShowForNewImport(pScene, {}); };
}

void plUsdPartitionPanelExtension::AddImportEntries(plScene2Document* pScene, const plUuid& partition, const plUsdPartitionImportManifest& manifest, plDynamicArray<MenuEntry>& out_entries)
{
  const plUsdPartitionImporter* pImporter = plUsdPartitionImporter::GetSingleton();
  const bool bRunning = pImporter != nullptr && pImporter->IsRunning();

  MenuEntry& update = out_entries.ExpandAndGetRef();
  update.m_sLabel = GetUpdateLabel(manifest);
  update.m_sToolTip = UpdateTooltip;
  update.m_bEnabled = !bRunning;
  update.m_OnPick = [this, pScene, partition]()
  { StartUpdate(pScene, partition); };

  MenuEntry& settings = out_entries.ExpandAndGetRef();
  settings.m_sLabel = "Open import settings...";
  settings.m_sToolTip = SettingsTooltip;
  settings.m_bEnabled = !bRunning;
  settings.m_OnPick = [pScene, partition]()
  { plUsdPartitionImportDialog::ShowForPartition(pScene, partition); };
}

void plUsdPartitionPanelExtension::GetPartitionMenuEntries(plScene2Document* pScene, const plUuid& partition, plDynamicArray<MenuEntry>& out_entries)
{
  if (const plUsdPartitionImportManifest* pManifest = GetManifest(pScene, partition, !IsImporting(pScene, partition)))
    AddImportEntries(pScene, partition, *pManifest, out_entries);
}

const plUsdPartitionImportManifest* plUsdPartitionPanelExtension::SelectDetails(plScene2Document* pScene, const plUuid& partition)
{
  // While this partition imports, its counts come from the importer, not from the manifest it keeps saving.
  const plUsdPartitionImportManifest* pManifest = GetManifest(pScene, partition, !IsImporting(pScene, partition));
  if (pManifest == nullptr)
  {
    m_pDetailsScene = nullptr;
    return nullptr;
  }

  m_pDetailsScene = pScene;
  m_DetailsPartition = partition;
  return pManifest;
}

plString plUsdPartitionPanelExtension::GetSummary(const plScene2Document* pScene, const plUuid& partition, const plUsdPartitionImportManifest& manifest) const
{
  // the parts, joined with ", "
  plStringBuilder sSummary;
  auto next = [&]() -> plStringBuilder&
  {
    if (!sSummary.IsEmpty())
      sSummary.Append(", ");
    return sSummary;
  };

  if (IsImporting(pScene, partition))
  {
    plUsdPartitionImportProgress progress;
    plUsdPartitionImporter::GetSingleton()->GetProgress(progress);
    next().Append("Importing");
    if (progress.m_uiRoots > 0)
      next().AppendFormat("{} of {} prim(s) done", progress.m_uiRootsDone, progress.m_uiRoots);
    if (progress.m_uiCells > 0)
      next().AppendFormat("{} of {} cell(s) written", progress.m_uiCellsWritten, progress.m_uiCells);
    return sSummary;
  }

  plUInt32 uiRootsDone = 0;
  for (const plUsdPartitionImportManifest::Root& root : manifest.m_Roots)
    uiRootsDone += root.m_bDone ? 1 : 0;

  next().Append(manifest.m_bComplete ? "Imported" : "Unfinished");
  if (manifest.m_Options.m_Roots.IsEmpty())
    next().AppendFormat("{} prim(s), found on their own", manifest.m_Roots.GetCount());
  else
    next().AppendFormat("{} chosen prim(s)", manifest.m_Options.m_Roots.GetCount());
  if (!manifest.m_bComplete && !manifest.m_Roots.IsEmpty())
    next().AppendFormat("{} done", uiRootsDone);
  next().AppendFormat("{} cell(s) written", manifest.m_Cells.GetCount());
  if (!manifest.m_Problems.IsEmpty())
    next().AppendFormat("{} problem(s)", manifest.m_Problems.GetCount());
  return sSummary;
}

bool plUsdPartitionPanelExtension::GetPartitionDetails(plScene2Document* pScene, const plUuid& partition, plDynamicArray<DetailsItem>& out_items)
{
  // the source, the summary, and the buttons; nothing for partitions that are not imports
  const plUsdPartitionImportManifest* pManifest = SelectDetails(pScene, partition);
  if (pManifest == nullptr)
    return true;

  const plUsdPartitionImporter* pImporter = plUsdPartitionImporter::GetSingleton();
  const bool bRunning = pImporter != nullptr && pImporter->IsRunning();

  DetailsItem& source = out_items.ExpandAndGetRef();
  source.m_sText = plStringBuilder("Source: ", plStringView(pManifest->m_Options.m_sStageFile).GetFileNameAndExtension());
  source.m_sToolTip = pManifest->m_Options.m_sStageFile;

  out_items.ExpandAndGetRef().m_sText = GetSummary(pScene, partition, *pManifest);

  DetailsItem& update = out_items.ExpandAndGetRef();
  update.m_sText = GetUpdateLabel(*pManifest);
  update.m_sToolTip = UpdateTooltip;
  update.m_bButton = true;
  update.m_bEnabled = !bRunning;
  update.m_OnClick = [this]()
  { StartUpdate(m_pDetailsScene, m_DetailsPartition); };

  DetailsItem& settings = out_items.ExpandAndGetRef();
  settings.m_sText = "Open import settings";
  settings.m_sToolTip = SettingsTooltip;
  settings.m_bButton = true;
  settings.m_bEnabled = !bRunning;
  settings.m_OnClick = [this]()
  {
    if (m_pDetailsScene != nullptr)
      plUsdPartitionImportDialog::ShowForPartition(m_pDetailsScene, m_DetailsPartition);
  };
  return true;
}

void plUsdPartitionPanelExtension::GatherProblems(plScene2Document* pScene, plDynamicArray<Problem>& out_problems)
{
  if (pScene == nullptr)
    return;

  plHybridArray<const plSceneWorldPartition*, 4> partitions;
  pScene->GetPartitions(partitions);
  for (const plSceneWorldPartition* pPartition : partitions)
  {
    // The running import saves its manifest often; it is read again once it finished.
    if (IsImporting(pScene, pPartition->m_Partition))
      continue;

    const plUsdPartitionImportManifest* pManifest = GetManifest(pScene, pPartition->m_Partition);
    if (pManifest == nullptr)
      continue;

    const plString& sName = pPartition->m_sName;
    plStringBuilder sText;

    if (const plString* pFailure = m_Failures.GetValue(pPartition->m_Partition))
    {
      sText.SetFormat("'{}': the USD import failed: {}", sName, *pFailure);
      Problem& problem = out_problems.ExpandAndGetRef();
      problem.m_sText = sText;
      problem.m_sFix = MakeFix(FixUpdate, pPartition->m_Partition, -1);
    }
    else if (!pManifest->m_bComplete)
    {
      sText.SetFormat("'{}': the USD import is unfinished. Right-click to resume it.", sName);
      Problem& problem = out_problems.ExpandAndGetRef();
      problem.m_sText = sText;
      problem.m_sFix = MakeFix(FixUpdate, pPartition->m_Partition, -1);
    }

    // One row per kind of problem, with the first prim as an example.
    plUInt32 uiCounts[ProblemTypeCount] = {};
    const plUsdImportProblem* pFirst[ProblemTypeCount] = {};
    for (const plUsdImportProblem& p : pManifest->m_Problems)
    {
      const int iType = static_cast<int>(p.m_Type);
      if (iType >= ProblemTypeCount)
        continue;
      if (uiCounts[iType]++ == 0)
        pFirst[iType] = &p;
    }

    for (int iType = 0; iType < ProblemTypeCount; ++iType)
    {
      if (uiCounts[iType] == 0)
        continue;

      const plUsdImportProblem::Type type = static_cast<plUsdImportProblem::Type>(iType);
      Problem& problem = out_problems.ExpandAndGetRef();
      problem.m_bInfo = IsInformation(type);
      problem.m_sFix = MakeFix(FixPaths, pPartition->m_Partition, iType);
      if (pFirst[iType]->m_sMessage.IsEmpty() || type == plUsdImportProblem::Type::Oversized || type == plUsdImportProblem::Type::Distant)
        sText.SetFormat("'{}': {} {}, e.g. {}", sName, uiCounts[iType], GetProblemText(type), pFirst[iType]->m_sPath);
      else
        sText.SetFormat("'{}': {} {}, e.g. {}: {}", sName, uiCounts[iType], GetProblemText(type), pFirst[iType]->m_sPath, pFirst[iType]->m_sMessage);
      problem.m_sText = sText;
    }
  }
}

void plUsdPartitionPanelExtension::GetProblemMenuEntries(plScene2Document* pScene, plStringView sFix, plDynamicArray<MenuEntry>& out_entries)
{
  plHybridArray<plStringView, 3> parts;
  sFix.Split(false, parts, "|");
  if (parts.GetCount() != 3 || pScene == nullptr)
    return;

  const plStringBuilder sGuid = parts[1];
  if (!plConversionUtils::IsStringUuid(sGuid))
    return;

  const plUuid partition = plConversionUtils::ConvertStringToUuid(sGuid);
  const plUsdPartitionImportManifest* pManifest = GetManifest(pScene, partition, !IsImporting(pScene, partition));
  if (pManifest == nullptr)
    return;

  if (parts[0] == FixPaths)
  {
    plInt32 iType = -1;
    plConversionUtils::StringToInt(parts[2], iType).IgnoreResult();

    // the prims of this problem, one per line, with the reason
    plStringBuilder sPaths;
    plUInt32 uiPaths = 0;
    for (const plUsdImportProblem& p : pManifest->m_Problems)
    {
      if (static_cast<int>(p.m_Type) != iType)
        continue;

      sPaths.Append(uiPaths++ == 0 ? "" : "\n", p.m_sPath);
      if (!p.m_sMessage.IsEmpty())
        sPaths.Append("\t", p.m_sMessage);
    }

    plStringBuilder sLabel;
    sLabel.SetFormat("Copy {} prim path(s)", uiPaths);

    MenuEntry& copy = out_entries.ExpandAndGetRef();
    copy.m_sLabel = sLabel;
    copy.m_sToolTip = "Copies the prims of this problem, one per line, with the reason.";
    const plString sCopied = sPaths;
    copy.m_OnPick = [sCopied]()
    { plEditorPlatform::SetClipboardText(sCopied); };
  }

  AddImportEntries(pScene, partition, *pManifest, out_entries);
}

bool plUsdPartitionPanelExtension::GetProgress(const plScene2Document* pScene, Progress& out_progress)
{
  const plUsdPartitionImporter* pImporter = plUsdPartitionImporter::GetSingleton();
  if (pImporter == nullptr || !pImporter->IsRunning() || pImporter->GetScene() != pScene || pScene == nullptr)
    return false;

  plUsdPartitionImportProgress progress;
  pImporter->GetProgress(progress);

  const plSceneWorldPartition* pPartition = pScene->GetPartition(pImporter->GetPartition());
  const plString sName = pPartition != nullptr ? pPartition->m_sName : plString("USD");

  plStringBuilder sText;
  sText.SetFormat("USD import '{}': {}", sName, progress.m_sPhase);
  if (!progress.m_sRoot.IsEmpty())
    sText.Append(" ", progress.m_sRoot);
  if (progress.m_uiRoots > 0)
    sText.AppendFormat(", prims {}/{}", progress.m_uiRootsDone, progress.m_uiRoots);
  if (progress.m_uiCells > 0)
    sText.AppendFormat(", cells {}/{}", progress.m_uiCellsWritten, progress.m_uiCells);
  sText.AppendFormat(", {} meshes built", progress.m_uiMeshesBuilt);
  sText.AppendFormat(", {}", FormatDuration(progress.m_Elapsed));

  const float fFraction = progress.GetFraction();
  if (fFraction > 0.02f && fFraction < 1.0f)
    sText.AppendFormat(", about {} left", FormatDuration(progress.m_Elapsed * ((1.0 - fFraction) / fFraction)));

  out_progress.m_sText = sText;
  out_progress.m_fFraction = fFraction;
  plStringBuilder sStatus;
  sStatus.SetFormat("USD import {}%", static_cast<int>(fFraction * 100.0f));
  out_progress.m_sStatus = sStatus;
  out_progress.m_bCanCancel = true;
  return true;
}

void plUsdPartitionPanelExtension::CancelProgress(plScene2Document* pScene)
{
  plUsdPartitionImporter* pImporter = plUsdPartitionImporter::GetSingleton();
  if (pImporter != nullptr && pImporter->GetScene() == pScene)
  {
    plLog::Info("USD Import: cancelled; Update from USD resumes it.");
    pImporter->Cancel();
  }
}
