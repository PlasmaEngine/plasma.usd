#pragma once

#include <EditorPluginUsd/EditorPluginUsdDLL.h>
#include <Foundation/Containers/Deque.h>
#include <Foundation/Containers/HashTable.h>
#include <Foundation/Strings/String.h>
#include <Foundation/System/Process.h>
#include <Foundation/Threading/TaskSystem.h>
#include <Foundation/Types/Status.h>
#include <Foundation/Types/UniquePtr.h>
#include <UsdBridge/UsdTexture.h>

struct plUsdSurface;

/// \brief Prepares the texture files a USD stage references, for the live link and the bake.
///
/// The engine only opens files under its data directories, and USD textures normally live beside the USD, anywhere on
/// disk. So each texture is converted into DDS files in the project's AssetCache. Every texture gets two versions:
///
/// - The preview: decoded in the editor, reduced to at most MaxSize, given mipmaps and written uncompressed into
///   AssetCache/UsdLivePreviews, which project export leaves out. It is quick to make, so the live link shows textures
///   soon after a stage opens.
/// - The final version: made afterwards by the engine's texture converter (TexConv) at full resolution, at most
///   FinalMaxSize, and block-compressed (see plUsdTexture::GetConversionArguments), into
///   AssetCache/Generated/UsdLive/Textures (see plUsdBakeStore), which project export ships. The live link switches to it
///   once it is there, and the bake waits for it, so games get these. Conversions start once no preview is waiting, a
///   few at a time, in processes of below-normal priority. A texture whose conversion fails gets a copy of its preview
///   as its final version.
///
/// An opacity mask is merged into the alpha channel of the colour texture it belongs to, which is what the engine's
/// masked materials read: the preview merges it in the editor, TexConv takes it as a second input mapped to alpha.
///
/// File names are a hash of the sources' paths, sizes and time stamps, so re-exporting a texture produces new files and
/// the materials that use it are sent again, and files that exist from an earlier session are used as they are.
class PL_EDITORPLUGINUSD_DLL plUsdLiveTextures
{
  PL_DISALLOW_COPY_AND_ASSIGN(plUsdLiveTextures);

public:
  plUsdLiveTextures();
  ~plUsdLiveTextures();

  /// \brief The longest edge a preview texture keeps. Larger sources are halved until they fit.
  static constexpr plUInt32 MaxSize = 1024;

  /// \brief The longest edge a final texture keeps. Larger sources are scaled down to it.
  static constexpr plUInt32 FinalMaxSize = 4096;

  enum class State
  {
    Pending, ///< Nothing to show yet.
    Preview, ///< The preview is ready; the final version is still being made.
    Ready,   ///< The final version is ready, or the preview stands in for one that could not be made.
    Failed,  ///< The source could not be read.
  };

  /// \brief Returns the state of a texture and the resource id the engine loads it by: the preview's in State::Preview,
  /// the final version's in State::Ready, none otherwise. The first request schedules the preparation. sOpacityFile may
  /// be empty and is only used with plUsdTexture::Usage::Color; when it is not, out_pSoftAlpha (if given) receives
  /// whether the mask is soft coverage to blend rather than a cut-out (see plUsdTexture::HasSoftAlpha), once the preview
  /// is ready.
  State Request(plStringView sColorFile, plStringView sOpacityFile, plUsdTexture::Usage usage, plString& out_sResourceId, bool* out_pSoftAlpha = nullptr);

  /// \brief Replaces a surface's texture files with the prepared textures' resource ids: final versions where they are
  /// ready, previews where only those are. Removes the ones with nothing to show yet or that failed. Returns false while
  /// any texture is short of its final version, so a caller that shows the previews resolves again when
  /// GetFinishedCount changes. Requests every texture it sees. The opacity mask goes into the base colour texture's
  /// alpha. out_pSoftOpacity receives whether that mask is soft coverage to blend.
  bool Resolve(plUsdSurface& inout_surface, bool* out_pSoftOpacity = nullptr);

  /// \brief Collects finished previews and conversions and starts queued ones. Main thread.
  void Update();

  /// \brief Whether anything is still being prepared or converted.
  bool IsBusy() const { return !m_Queue.IsEmpty() || !m_Running.IsEmpty() || !m_ConversionQueue.IsEmpty() || !m_Converting.IsEmpty(); }

  /// \brief Counts finished previews and final versions, successful or not. Anything waiting on textures only needs
  /// another look when this changed.
  plUInt32 GetFinishedCount() const { return m_uiFinished; }

private:
  struct Entry
  {
    plString m_sColorFile;
    plString m_sOpacityFile;
    plUsdTexture::Usage m_Usage = plUsdTexture::Usage::Color;
    bool m_bSoftAlpha = false;
    State m_State = State::Pending;

    // The preview, made on a task. The task also checks whether the final version exists from an earlier session.
    plString m_sPreviewFile;
    plStatus m_Result;
    bool m_bFinalExists = false;
    plTaskGroupID m_Task;

    // The final version, made by TexConv. Holds the preview's copy once the conversion has failed.
    plString m_sFinalFile;
    plString m_sConversionErrors; ///< TexConv's error messages, collected by the process's output thread.
    plTime m_ConversionStart;
    plUniquePtr<plProcess> m_pProcess;
  };

  void StartConversion(Entry& ref_entry);
  void FinishConversion(Entry& ref_entry, const plStatus& result);

  /// \brief Makes the preview the final version: copies it next to the final versions, so project export ships it.
  void UsePreviewAsFinal(Entry& ref_entry);

  /// \brief Deletes every preview among the final versions, e.g. the copies conversions that failed in an earlier session
  /// left there. A conversion that fails again copies its preview again.
  void RemoveStalePreviews();

  plString m_sPreviewFolder;
  plString m_sOutputFolder;
  plString m_sConverter;             ///< TexConv's path.
  bool m_bConverterMissing = false;  ///< TexConv could not be started; every texture keeps its preview.
  plDynamicArray<plUniquePtr<Entry>> m_Storage;
  plHashTable<plString, Entry*> m_Entries;
  plDeque<Entry*> m_Queue;           ///< Waiting for their preview.
  plDynamicArray<Entry*> m_Running;  ///< Preview being made.
  plDeque<Entry*> m_ConversionQueue; ///< Preview ready, waiting for TexConv.
  plDynamicArray<Entry*> m_Converting;
  plUInt32 m_uiFinished = 0;
};
