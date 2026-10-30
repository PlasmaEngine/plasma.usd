#pragma once

#include <EditorPluginUsd/PartitionImport/UsdPartitionImportData.h>
#include <Foundation/Containers/HashSet.h>
#include <Foundation/Threading/AtomicInteger.h>
#include <Foundation/Threading/Mutex.h>
#include <Foundation/Time/Time.h>
#include <Foundation/Types/RefCounted.h>
#include <Foundation/Types/SharedPtr.h>

/// \brief A prim of the stage listing of the World Partition from USD dialogs.
struct plUsdPartitionPrimEntry
{
  plString m_sPath;
  plString m_sName;
  plString m_sType;
  plUInt32 m_uiParent = plInvalidIndex;
  plUInt32 m_uiPlainDepth = plInvalidIndex; ///< Levels below the top of its part without payloads; invalid in other parts.
  bool m_bPayload = false;
  bool m_bPayloadBelow = false;
  bool m_bExcluded = false;
  bool m_bCandidate = false; ///< Can be a root: a payload prim, or a prim near the top of a part without payloads.
  bool m_bDefault = false;   ///< A root of the import when none are chosen.
  plDynamicArray<plUsdVariantSet> m_VariantSets;
  plBoundingBox m_Bounds = plBoundingBox::MakeInvalid();
};

/// \brief The stage listing or a Scan of the World Partition from USD dialogs, run on a worker.
///
/// The listing reads the stage's prims level by level with no payload loaded, then measures the payload prims with their
/// proxy variants. A Scan loads each root fully, one at a time, and counts what the import would place. The dialogs poll
/// it from the main thread; cancelling lets the worker finish on its own and free what it read.
struct plUsdPartitionStageRead : public plRefCounted
{
  // Input
  plString m_sStage;
  bool m_bScan = false;
  plUsdPartitionImportOptions m_Options; ///< The excluded paths of a listing; everything of a Scan.
  plDynamicArray<plString> m_Roots;      ///< Scan only.

  plAtomicBool m_bCancel;
  plAtomicBool m_bListed; ///< The listing or scan is done; m_Prims no longer changes.
  plAtomicBool m_bDone;   ///< Also the sizes of payload prims are measured.

  mutable plMutex m_Mutex;
  plString m_sProgress;

  // Listing
  plStatus m_Result;
  plTime m_Duration;
  plString m_sBasis;
  plDynamicArray<plUsdPartitionPrimEntry> m_Prims;
  bool m_bTruncated = false;
  bool m_bAnyPayload = false;
  plDynamicArray<plString> m_UnlistedDefaults; ///< Roots the import finds on its own that the listing does not show.

  // Sizes of payload prims, measured with their proxy variants after the listing; taken by the main thread.
  struct Size
  {
    plUInt32 m_uiPrim = 0;
    plBoundingBox m_Bounds;
  };
  plDynamicArray<Size> m_Sizes;
  plUInt32 m_uiSizesToMeasure = 0;
  plUInt32 m_uiSizesMeasured = 0;

  // Scan
  plAtomicInteger32 m_iRootsDone; ///< Roots scanned or failed so far, for the progress.
  plUInt32 m_uiRootsScanned = 0;
  plUInt64 m_uiPlaceables = 0;
  plUInt64 m_uiInstances = 0;
  plUInt64 m_uiGprims = 0;
  plUInt64 m_uiLightsAndCameras = 0;
  plUInt64 m_uiOutsideRegion = 0;
  plUInt64 m_uiSplit = 0;
  plUInt64 m_uiSplitPieces = 0;
  plUInt64 m_uiDistant = 0;
  plUInt32 m_uiCells = 0;
  plUInt32 m_uiPrototypes = 0;
  plUInt64 m_uiLooseMeshes = 0;
  plString m_sFailedRoot;

  /// \brief Starts listing the stage's prims on a worker; the options give the excluded paths and the default roots.
  static plSharedPtr<plUsdPartitionStageRead> StartListing(plStringView sStage, const plUsdPartitionImportOptions& options);

  /// \brief Starts a Scan of the roots with the options on a worker.
  static plSharedPtr<plUsdPartitionStageRead> StartScan(const plUsdPartitionImportOptions& options, plArrayPtr<const plString> roots);

  void SetProgress(plStringView sText);
  plString GetProgress() const;
  void TakeSizes(plDynamicArray<Size>& out_sizes, plUInt32& out_uiMeasured, plUInt32& out_uiTotal);

  /// \brief The roots of the import when the listed candidates in checked are checked. out_bDefault: they are the roots
  /// the import finds on its own. Default roots the listing does not show stay roots, as do roots chosen before that it
  /// does not show; nested roots are removed.
  void CollectRoots(const plHashSet<plString>& checked, const plUsdPartitionImportOptions& initialOptions, plDynamicArray<plString>& out_roots, bool& out_bDefault) const;

  /// \brief Every variant set on a prim that can be a root, with all the variants any of them has.
  void GatherVariantSets(plDynamicArray<plUsdVariantSet>& out_sets) const;

  /// \brief The variant sets a root of the list has.
  void GetUsedVariantSets(plArrayPtr<const plString> roots, plHashSet<plString>& out_sets) const;

  /// \brief The stage info line of a finished listing: the basis, the time, and what the listing leaves out.
  plString FormatStageInfo() const;

  /// \brief The text of a finished Scan: "<n> prim(s) scanned in <s> s: ... Objects before batching."
  plString FormatScanResult() const;

  /// \brief "Not scanned: <root>: <message>" for the first root a Scan could not read; empty when it read all.
  plString FormatNotScanned() const;

  void Run();

private:
  void List(plUsdPartitionScan& scan);
  void MeasureSizes(plUsdPartitionScan& scan);
  void Scan(plUsdPartitionScan& scan);
};

/// \brief What the fields of a World Partition from USD dialog hold, for plUsdPartitionImportUi::BuildOptions.
struct plUsdPartitionImportChoices
{
  struct Variant
  {
    plString m_sSet;
    plString m_sDefault; ///< What the import picks when the set has no override.
    plString m_sChosen;
  };

  plString m_sStageFile;
  plString m_sPartitionName;
  float m_fCellSize = 128.0f;
  plDynamicArray<plString> m_Roots;
  bool m_bDefaultRoots = true;
  plDynamicArray<Variant> m_Variants;
  bool m_bBatch = true;
  float m_fBatchSize = 8.0f;
  bool m_bSplit = true;
  float m_fSplitSize = 128.0f;
  bool m_bLightsAndCameras = true;
  bool m_bRegion = false;
  float m_fRegion[4] = {-512.0f, -512.0f, 512.0f, 512.0f}; ///< X from, Y from, X to, Y to
};

/// \brief What the World Partition from USD dialog uses besides its fields: the cell sizes, formats, the options it
/// builds and its background reads.
namespace plUsdPartitionImportUi
{
  /// The stage listing stops after this many prims; the prims below them can still be chosen by JSON.
  constexpr plUInt32 MaxListedPrims = 20000;

  /// In a part of the stage without payloads, prims down to this many levels below its top can be roots.
  constexpr plUInt32 MaxPlainRootDepth = 2;

  constexpr float CellSizes[] = {64.0f, 128.0f, 256.0f, 512.0f};

  /// The variant an import picks for a set no override names: its most detailed one, else what the stage selects.
  plString GetDefaultVariant(const plUsdVariantSet& set);

  /// The variant chosen before for every root, else the default.
  plString GetChosenVariant(const plUsdVariantSet& set, const plUsdPartitionImportOptions& initialOptions);

  /// "<x> × <y> m", or "-" for invalid bounds.
  plString FormatSize(const plBoundingBox& bounds);

  /// A count with its thousands separated by commas.
  plString FormatCount(plUInt64 uiCount);

  /// "<n> m" for a cell size.
  plString FormatCellSize(float fSize);

  /// \brief The import options from the initial options and the dialog's fields. False with the message for the first
  /// problem: no existing stage, no partition name, no root, an empty region.
  bool BuildOptions(const plUsdPartitionImportOptions& initialOptions, const plUsdPartitionImportChoices& choices, plUsdPartitionImportOptions& out_options, plString& out_sError);

  /// The background reads that finished need no waiting for anymore.
  void ForgetFinishedTasks();

  /// Waits for every background read; before the plugin unloads.
  void WaitForTasks();
} // namespace plUsdPartitionImportUi
