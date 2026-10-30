#pragma once

#include <EditorPluginScene/Panels/WorldStreamingPanel/WorldStreamingPanelExtension.h>
#include <EditorPluginUsd/PartitionImport/UsdPartitionImporter.h>
#include <Foundation/Containers/Map.h>

struct plAssetBrowserContextMenuEvent;

/// \brief The USD import's part of the World Streaming panel: '+ New > World partition from USD...', the source and
/// 'Update from USD' of an imported partition, the import's problems, and its progress with Cancel. Also adds 'Import as
/// World Partition...' to the asset browser's menu of USD files.
class PL_EDITORPLUGINUSD_DLL plUsdPartitionPanelExtension : public plWorldStreamingPanelExtension
{
public:
  plUsdPartitionPanelExtension();
  ~plUsdPartitionPanelExtension();

  /// \brief The scene USD imports go into from menus: the World Streaming panel's, else the scene of the visible
  /// document window. May be nullptr.
  static plScene2Document* GetTargetScene();

  /// \brief Asks whether to go on when loaded cells of the partition have unsaved changes; true when there are none.
  /// Only for button and menu paths: it shows a dialog.
  static bool ConfirmRewritingCells(const plScene2Document* pScene, const plUuid& partition);

  virtual void GetNewMenuEntries(plScene2Document* pScene, plDynamicArray<MenuEntry>& out_entries) override;
  virtual void GetPartitionMenuEntries(plScene2Document* pScene, const plUuid& partition, plDynamicArray<MenuEntry>& out_entries) override;
  virtual bool GetPartitionDetails(plScene2Document* pScene, const plUuid& partition, plDynamicArray<DetailsItem>& out_items) override;
  virtual void GatherProblems(plScene2Document* pScene, plDynamicArray<Problem>& out_problems) override;
  virtual void GetProblemMenuEntries(plScene2Document* pScene, plStringView sFix, plDynamicArray<MenuEntry>& out_entries) override;
  virtual bool GetProgress(const plScene2Document* pScene, Progress& out_progress) override;
  virtual void CancelProgress(plScene2Document* pScene) override;

private:
  struct CachedManifest
  {
    plInt64 m_iFileTime = 0;
    plUInt64 m_uiFileSize = 0;
    bool m_bValid = false;
    plUsdPartitionImportManifest m_Manifest;
  };

  /// The manifest of an imported partition, read again when its file changed (with bReadChanges, else only when not
  /// read yet); nullptr for other partitions.
  const plUsdPartitionImportManifest* GetManifest(const plScene2Document* pScene, const plUuid& partition, bool bReadChanges = true);

  void StartUpdate(plScene2Document* pScene, const plUuid& partition);

  /// 'Update from USD' (or 'Resume import') and 'Open import settings...' of the partition and problem menus.
  void AddImportEntries(plScene2Document* pScene, const plUuid& partition, const plUsdPartitionImportManifest& manifest, plDynamicArray<MenuEntry>& out_entries);

  /// The details' summary line: imported or importing, prims, cells and problems.
  plString GetSummary(const plScene2Document* pScene, const plUuid& partition, const plUsdPartitionImportManifest& manifest) const;

  /// The partition whose details show and its manifest, for the details' buttons; nullptr when the details show none.
  const plUsdPartitionImportManifest* SelectDetails(plScene2Document* pScene, const plUuid& partition);
  void ImporterEventHandler(const plUsdPartitionImportEvent& e);
  void AssetBrowserEventHandler(const plAssetBrowserContextMenuEvent& e);

  plMap<plString, CachedManifest> m_Manifests; ///< By manifest file.

  /// The last failed import of a partition, until it is started again.
  plHashTable<plUuid, plString> m_Failures;

  plEventSubscriptionID m_ImporterSubscription = 0;
  plEventSubscriptionID m_AssetBrowserSubscription = 0;

  plScene2Document* m_pDetailsScene = nullptr;
  plUuid m_DetailsPartition;
};
