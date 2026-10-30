#pragma once

#include <EditorPluginUsd/PartitionImport/UsdPartitionImportData.h>
#include <Foundation/Containers/HybridArray.h>
#include <Foundation/Types/SharedPtr.h>
#include <Foundation/Types/UniquePtr.h>
#include <GuiFoundation/EngineUi/EngineDialog.h>
#include <ToolsFoundation/Document/DocumentManager.h>

class plEditorUiProcessView;
class plScene2Document;
class plUsdPrimTreeView;
struct plUsdPartitionStageRead;

/// \brief World Partition from USD drawn by the engine: the stage with Browse and its info line, the partition name and
/// the cell size; the prims as a tree with check boxes, the type, the payload and variant notes and the size; the variant
/// sets of the checked prims; the options (batching, splitting, lights and cameras, region); and the Scan in a process
/// block. The footer has Scan (Stop while scanning) on the left, Import (Import again for an imported partition) and
/// Close. Problems show in the validation line as you type; Import stays disabled while there is one.
///
/// Not modal, one at a time: plUsdPartitionImportDialog::ShowForNewImport and ShowForPartition open it. It reads the
/// stage and scans on workers and closes with its scene.
class PL_EDITORPLUGINUSD_DLL plUsdPartitionImportEngineDlg : public plEngineDialog
{
public:
  plUsdPartitionImportEngineDlg(plScene2Document* pScene, const plUsdPartitionImportOptions& options, bool bExistingPartition);
  ~plUsdPartitionImportEngineDlg();

protected:
  void BuildBody(plUiElement* pBody, const plVec2& vSize) override;
  void OnButton(plStringView sId) override;
  void OnUiEvent(const plMsgUiEvent& event) override;
  void OnUiUpdate() override;
  plString GetToolTipAt(const plVec2& vLogical) const override;
  plVec2 GetPreferredSize() const override;

private:
  struct VariantRow
  {
    plString m_sSet;
    plString m_sDefault; ///< What the import picks when the set has no override.
    plString m_sChosen;
    plString m_sStageSelection;
    plDynamicArray<plString> m_Variants;
    bool m_bShown = false; ///< A checked root has the set.
    plRectFloat m_FieldRect = plRectFloat(0, 0, 0, 0);
  };

  /// A metres field that commits when it loses focus or on Enter, clamped and rounded as a spin box does.
  struct Number
  {
    const char* m_szName = nullptr;
    double m_fMin = 0.0;
    double m_fMax = 0.0;
    plUInt32 m_uiDecimals = 0;
    double m_fValue = 0.0;
    plString m_sCommitted;
  };

  enum NumberIndex : plUInt32
  {
    BatchNumber,
    SplitNumber,
    RegionXFrom,
    RegionYFrom,
    RegionXTo,
    RegionYTo,
    NumberCount,
  };

  // the stage and the scan
  void SetOptions(const plUsdPartitionImportOptions& options);
  void ReadStage();
  void OnTick();
  void ShowSizes();
  void FillTree();
  void FillVariants();
  void UpdateVariantRows();
  void StartScan();
  void ShowScanResult();
  void Import();
  void Browse();

  // the options as the fields hold them
  void CollectCheckedRoots(plDynamicArray<plString>& out_roots, bool& out_bDefault) const;
  bool GetOptions(plUsdPartitionImportOptions& out_options, plString& out_sError) const;
  bool IsRead() const;
  bool IsScanning() const;
  static bool IsImportRunning();

  // validation and buttons
  void MarkChanged();
  void UpdateState();
  void UpdateButtons();
  void SetStageInfo(plStringView sText);

  // fields
  plString ReadText(plStringView sEntry) const;
  void WriteText(plStringView sEntry, plStringView sText);
  void CommitFields(plStringView sSubmitted);
  void CommitNumber(plUInt32 uiNumber, plStringView sText);
  void SetNumber(plUInt32 uiNumber, double fValue);
  void SetCellSize(float fSize);

  // menus
  void ShowMenu(const plRectFloat& field, plArrayPtr<const plString> items, plInt32 iChecked, plDelegate<void(plUInt32)> onPick);
  void ShowCellSizeMenu();
  void ShowRegionMenu();
  void ShowVariantMenu(plUInt32 uiRow);

  // building
  float BuildStageForm(plUiElement* pBody, float fWidth);
  float BuildVariants(plUiElement* pBody, float fY, float fWidth);
  float BuildOptions(plUiElement* pBody, float fY, float fWidth);
  float GetVariantsHeight() const;
  float GetOptionsHeight() const;
  void Section(plUiElement* pBody, float fY, float fWidth, plStringView sTitle);
  void AddToolTip(const plRectFloat& rect, plStringView sText);

  void OnChecksChanged(plUInt32 uiItem);
  void DocumentManagerEventHandler(const plDocumentManager::Event& e);

  plScene2Document* m_pScene = nullptr;
  bool m_bExistingPartition = false;
  plUsdPartitionImportOptions m_InitialOptions;
  plEventSubscriptionID m_DocumentSubscription = 0;
  plEventSubscriptionID m_ChecksSubscription = 0;

  plSharedPtr<plUsdPartitionStageRead> m_pRead; ///< The stage listing of the current stage file, on its worker or done.
  plSharedPtr<plUsdPartitionStageRead> m_pScan; ///< A running or finished Scan.
  plString m_sReadStage;                        ///< The stage file m_pRead is about.
  bool m_bTreeFilled = false;
  bool m_bSizesDone = false;   ///< Every measured size of m_pRead is in the tree.
  bool m_bScanShown = false;   ///< The finished scan's result is in the process block.
  plString m_sStageInfo;       ///< The stage info line without the size measuring progress.
  plDynamicArray<plUInt32> m_PrimItems; ///< Tree item by index of the listed prim; plInvalidIndex where it is not shown.
  plDynamicArray<plUInt32> m_ItemPrims; ///< Listed prim by tree item.

  // the fields
  plString m_sStageText;     ///< As typed.
  plString m_sStageCommitted;
  plString m_sPartitionText; ///< As typed.
  float m_fCellSize = 128.0f;
  plHybridArray<float, 5> m_CellSizes;
  bool m_bBatch = true;
  bool m_bSplit = true;
  bool m_bSplitFollowsCell = true; ///< The split size changes with the cell size.
  bool m_bLights = true;
  bool m_bRegion = false;
  Number m_Numbers[NumberCount];
  plDynamicArray<VariantRow> m_VariantRows;

  // validation, recomputed after a change
  bool m_bOptionsDirty = true;
  bool m_bOptionsValid = false;
  plString m_sOptionsError;
  plString m_sNameTaken;    ///< The message for a new import into an existing partition's name.
  plString m_sStartError;   ///< Why the import did not start, until something changes.
  bool m_bButtonsScanning = false;
  bool m_bButtonsImportRunning = false;

  // where the dropdowns and the tree were built, in body units
  plRectFloat m_CellSizeRect = plRectFloat(0, 0, 0, 0);
  plRectFloat m_RegionRect = plRectFloat(0, 0, 0, 0);
  plRectFloat m_TreeRect = plRectFloat(0, 0, 0, 0);
  plUInt32 m_uiBuiltVariantRows = 0;

  plUniquePtr<plUsdPrimTreeView> m_pTree;
  plUniquePtr<plEditorUiProcessView> m_pProcess;
  plEditorUiTimerGroup m_Timers; ///< OnTick every 100 ms
};
