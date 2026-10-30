#pragma once

#include <EditorPluginUsd/EditorPluginUsdDLL.h>
#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Containers/HashTable.h>
#include <Foundation/Time/Time.h>
#include <Foundation/Types/UniquePtr.h>
#include <Foundation/Types/Uuid.h>

/// \brief Keeps the bakes of links whose documents are closed up to date.
///
/// It knows every link in the project from the .usdlinks sidecars. A link whose document is closed, and whose bake is
/// missing or older than one of the layer files it was composed from, is baked in the background: a worker thread opens
/// the stage, reads the prims and builds the meshes whose build hash changed; the main thread then prepares the textures
/// through the shared cache and writes the materials and the bake prefab. One link at a time. Links of open documents
/// are baked by their live links.
///
/// A closed document gets its USD Bake object when it is next opened; until then its bake prefab is up to date but not
/// yet referenced.
class PL_EDITORPLUGINUSD_DLL plUsdBackgroundBaker
{
  PL_DISALLOW_COPY_AND_ASSIGN(plUsdBackgroundBaker);

public:
  plUsdBackgroundBaker();
  ~plUsdBackgroundBaker();

  /// \brief Finds every sidecar in the project. Called when a project opens.
  void ScanProject();

  /// \brief Forgets every link and waits for a running bake. Called when the project closes.
  void Clear();

  /// \brief Reads one document's sidecar again, after its links changed.
  void UpdateDocument(plStringView sDocumentPath);

  /// \brief Checks for out-of-date bakes and drives the running one. Main thread.
  void Update();

  /// \brief What the background bake is doing, for the UI; empty when idle.
  plStringView GetStatus() const { return m_sStatus; }

private:
  struct Link
  {
    plString m_sDocument;
    plUuid m_Root;
    plString m_sStageFile;
    plString m_sSession;
    plString m_sLinkId;
  };

  struct Job;

  void ReadSidecar(plStringView sDocumentPath);
  bool IsDocumentOpen(const Link& link) const;
  bool NeedsBake(const Link& link) const;
  void StartJob(const Link& link);
  void UpdateJob();

  /// \brief Runs on a worker thread: opens the stage, reads the prims and writes the mesh files whose build hash
  /// changed. Nothing else touches the job until the task finished.
  static void RunWorker(Job& job);

  plDynamicArray<Link> m_Links;
  plUniquePtr<Job> m_pJob;
  plTime m_NextCheck;
  plUInt32 m_uiNextLink = 0;
  plHashTable<plString, plTime> m_RetryAfter; ///< Link id -> when a failed link is tried again.
  plString m_sStatus;
};
