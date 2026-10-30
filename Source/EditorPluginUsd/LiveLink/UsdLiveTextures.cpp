#include <EditorPluginUsd/EditorPluginUsdPCH.h>

#include <EditorFramework/EditorApp/EditorApp.h>
#include <EditorPluginUsd/LiveLink/UsdBakeStore.h>
#include <EditorPluginUsd/LiveLink/UsdLiveTextures.h>
#include <Foundation/Algorithm/HashingUtils.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/System/SystemInformation.h>
#include <Foundation/Threading/DelegateTask.h>
#include <ToolsFoundation/Project/ToolsProject.h>
#include <UsdBridge/UsdStage.h>
#include <UsdBridge/UsdTexture.h>

#if PL_ENABLED(PL_PLATFORM_WINDOWS_DESKTOP)
#  include <Foundation/Basics/Platform/Win/IncludeWindows.h>
#endif

namespace
{
  /// Each preparation decodes one source on one thread, so a stage with many textures is only fast with several at
  /// once. Every one holds its decoded source (64 MB for a 4K RGBA image) until it is reduced, and meshes are built on
  /// the same long-running threads, so half the cores, within limits.
  plUInt32 GetMaxConcurrentPreparations()
  {
    return plMath::Clamp(plSystemInformation::Get().GetCPUCoreCount() / 2, 2u, 6u);
  }

  /// A conversion keeps about one core busy - BC4 and BC5 are compressed on the CPU in one thread, BC7 on the GPU - and
  /// holds up to 1.3 GB for a 4K texture with a mask. So one per four cores and per 8 GB of memory, one to three.
  plUInt32 GetMaxConcurrentConversions()
  {
    const plUInt32 uiByCores = plSystemInformation::Get().GetCPUCoreCount() / 4;
    const plUInt32 uiByMemory = static_cast<plUInt32>(plSystemInformation::Get().GetInstalledMainMemory() / (8ull << 30));
    return plMath::Clamp(plMath::Min(uiByCores, uiByMemory), 1u, 3u);
  }

  /// A 4K normal map takes about half a minute; a conversion still running after this is stuck.
  constexpr plTime ConversionTimeout = plTime::MakeFromMinutes(10);

  /// The file name for the sources as they are on disk now: a changed source gets a new name. uiFinalMaxSize is 0 for
  /// the preview; a final version's name includes it, so a different limit makes new files.
  ///
  /// The engine's texture loader treats a name ending in "_D" as sRGB colour data. "_N" keeps a normal map's files
  /// apart from those of the same source used as a linear texture, and "_F" marks a final version.
  plString MakeOutputFile(plStringView sFolder, plStringView sColorFile, plStringView sOpacityFile, plUsdTexture::Usage usage, plUInt32 uiFinalMaxSize)
  {
    plStringBuilder sStamp;
    for (plStringView sFile : {sColorFile, sOpacityFile})
    {
      plFileStats stats;
      if (!sFile.IsEmpty() && plOSFile::GetFileStats(sFile, stats).Succeeded())
        sStamp.AppendFormat("{}:{}:{};", sFile, stats.m_uiFileSize, stats.m_LastModificationTime.GetInt64(plSIUnitOfTime::Microsecond));
    }

    if (uiFinalMaxSize > 0)
      sStamp.AppendFormat("final:{};", uiFinalMaxSize);

    const char* szUsage = "";
    if (usage == plUsdTexture::Usage::Color)
      szUsage = "_D";
    else if (usage == plUsdTexture::Usage::Normal)
      szUsage = "_N";

    plStringBuilder sOutput;
    sOutput.SetFormat("{}/{}{}{}.dds", sFolder, plArgU(plHashingUtils::xxHash64String(sStamp), 16, true, 16), uiFinalMaxSize > 0 ? "_F" : "", szUsage);
    return sOutput;
  }

  /// TexConv writes here, and the file is moved into place once it succeeded, so a final version that exists is whole.
  plString GetConversionTempFile(plStringView sFinalFile)
  {
    plStringBuilder sTemp = sFinalFile;
    sTemp.RemoveFileExtension();
    sTemp.Append(".tmp.dds");
    return sTemp;
  }
} // namespace

plUsdLiveTextures::plUsdLiveTextures() = default;

plUsdLiveTextures::~plUsdLiveTextures()
{
  for (Entry* pEntry : m_Running)
  {
    plTaskSystem::WaitForGroup(pEntry->m_Task);
  }

  // Destroying a plProcess terminates it. A conversion cut short leaves only its temporary file.
  for (Entry* pEntry : m_Converting)
  {
    pEntry->m_pProcess.Clear();
    plOSFile::DeleteFile(GetConversionTempFile(pEntry->m_sFinalFile)).IgnoreResult();
  }
}

plUsdLiveTextures::State plUsdLiveTextures::Request(plStringView sColorFile, plStringView sOpacityFile, plUsdTexture::Usage usage, plString& out_sResourceId, bool* out_pSoftAlpha)
{
  out_sResourceId.Clear();
  if (out_pSoftAlpha)
    *out_pSoftAlpha = false;

  // Resolved on first use rather than at construction: the plugin loads while the project is still being opened.
  if (m_sOutputFolder.IsEmpty())
  {
    const plString sRoot = plUsdBakeStore::GetRootFolder();
    if (sRoot.IsEmpty())
      return State::Failed;

    m_sOutputFolder = plStringBuilder(sRoot, "/Textures");

    plStringBuilder sPreviews(plToolsProject::GetSingleton()->GetProjectDirectory(), "/AssetCache/UsdLivePreviews");
    sPreviews.MakeCleanPath();
    m_sPreviewFolder = sPreviews;

    RemoveStalePreviews();
  }

  plStringBuilder sKey;
  sKey.SetFormat("{}|{}|{}", sColorFile, sOpacityFile, static_cast<int>(usage));

  Entry* pEntry = nullptr;
  if (!m_Entries.TryGetValue(sKey, pEntry))
  {
    plUniquePtr<Entry> pNewEntry = PL_DEFAULT_NEW(Entry);
    pNewEntry->m_sColorFile = sColorFile;
    pNewEntry->m_sOpacityFile = sOpacityFile;
    pNewEntry->m_Usage = usage;
    m_Queue.PushBack(pNewEntry.Borrow());
    m_Entries.Insert(sKey, pNewEntry.Borrow());
    m_Storage.PushBack(std::move(pNewEntry));
    return State::Pending;
  }

  Entry& entry = *pEntry;

  // A source that changed on disk is prepared again under new names. Only settled entries are looked at: a preview or
  // conversion under way finishes first, and the next request after it notices the change.
  if ((entry.m_State == State::Ready || entry.m_State == State::Failed) &&
      MakeOutputFile(m_sPreviewFolder, entry.m_sColorFile, entry.m_sOpacityFile, entry.m_Usage, 0) != entry.m_sPreviewFile)
  {
    entry.m_State = State::Pending;
    m_Queue.PushBack(&entry);
  }

  if (entry.m_State == State::Preview)
    out_sResourceId = plUsdBakeStore::ToResourceId(entry.m_sPreviewFile);
  else if (entry.m_State == State::Ready)
    out_sResourceId = plUsdBakeStore::ToResourceId(entry.m_sFinalFile);

  if (out_pSoftAlpha && (entry.m_State == State::Preview || entry.m_State == State::Ready))
    *out_pSoftAlpha = entry.m_bSoftAlpha;

  return entry.m_State;
}

bool plUsdLiveTextures::Resolve(plUsdSurface& inout_surface, bool* out_pSoftOpacity)
{
  bool bFinal = true;

  auto resolve = [&](plString& ref_sFile, plStringView sOpacityFile, plUsdTexture::Usage usage, bool* out_pSoftAlpha)
  {
    if (ref_sFile.IsEmpty() && sOpacityFile.IsEmpty())
      return;

    plString sPrepared;
    switch (Request(ref_sFile, sOpacityFile, usage, sPrepared, out_pSoftAlpha))
    {
      case State::Pending:
        // The source itself is outside the engine's data directories, so it cannot stand in meanwhile.
        ref_sFile.Clear();
        bFinal = false;
        break;
      case State::Preview:
        ref_sFile = sPrepared;
        bFinal = false;
        break;
      case State::Ready:
        ref_sFile = sPrepared;
        break;
      case State::Failed:
        ref_sFile.Clear();
        break;
    }
  };

  resolve(inout_surface.m_sBaseColorTexture, inout_surface.m_sOpacityTexture, plUsdTexture::Usage::Color, out_pSoftOpacity);
  resolve(inout_surface.m_sNormalTexture, {}, plUsdTexture::Usage::Normal, nullptr);
  resolve(inout_surface.m_sRoughnessTexture, {}, plUsdTexture::Usage::Linear, nullptr);
  resolve(inout_surface.m_sMetallicTexture, {}, plUsdTexture::Usage::Linear, nullptr);
  return bFinal;
}

void plUsdLiveTextures::Update()
{
  for (plUInt32 i = 0; i < m_Running.GetCount();)
  {
    Entry* pEntry = m_Running[i];
    if (!plTaskSystem::IsTaskGroupFinished(pEntry->m_Task))
    {
      ++i;
      continue;
    }

    m_Running.RemoveAtAndSwap(i);
    ++m_uiFinished;

    if (pEntry->m_Result.Failed())
    {
      pEntry->m_State = State::Failed;
      plLog::Warning("USD Live: texture '{}' is unavailable: {}", pEntry->m_sColorFile.IsEmpty() ? pEntry->m_sOpacityFile : pEntry->m_sColorFile,
        pEntry->m_Result.m_sMessage);
    }
    else if (pEntry->m_bFinalExists)
    {
      pEntry->m_State = State::Ready;
    }
    else
    {
      pEntry->m_State = State::Preview;
      m_ConversionQueue.PushBack(pEntry);
    }
  }

  for (plUInt32 i = 0; i < m_Converting.GetCount();)
  {
    Entry* pEntry = m_Converting[i];
    plProcess& process = *pEntry->m_pProcess;

    plStatus result(PL_SUCCESS);
    if (process.GetState() != plProcessState::Finished)
    {
      if (plTime::Now() - pEntry->m_ConversionStart < ConversionTimeout)
      {
        ++i;
        continue;
      }

      process.Terminate().IgnoreResult();
      result = plStatus(plFmt("TexConv was still running after {} minutes", ConversionTimeout.GetMinutes()));
    }
    else if (process.GetExitCode() != 0)
    {
      result = plStatus(plFmt("TexConv returned {}: {}", process.GetExitCode(), pEntry->m_sConversionErrors.IsEmpty() ? plStringView("no message") : pEntry->m_sConversionErrors.GetView()));
    }

    m_Converting.RemoveAtAndSwap(i);
    FinishConversion(*pEntry, result);
  }

  const plUInt32 uiMaxRunning = GetMaxConcurrentPreparations();
  while (m_Running.GetCount() < uiMaxRunning && !m_Queue.IsEmpty())
  {
    Entry* pEntry = m_Queue.PeekFront();
    m_Queue.PopFront();

    pEntry->m_sPreviewFile = MakeOutputFile(m_sPreviewFolder, pEntry->m_sColorFile, pEntry->m_sOpacityFile, pEntry->m_Usage, 0);
    pEntry->m_sFinalFile = MakeOutputFile(m_sOutputFolder, pEntry->m_sColorFile, pEntry->m_sOpacityFile, pEntry->m_Usage, FinalMaxSize);
    pEntry->m_bFinalExists = false;
    pEntry->m_bSoftAlpha = false;

    plSharedPtr<plDelegateTask<void>> pTask = PL_DEFAULT_NEW(plDelegateTask<void>, "USD Live texture", plTaskNesting::Never, [pEntry]()
      {
        pEntry->m_Result = plStatus(PL_SUCCESS);

        // Converted in an earlier session. Without a mask there is nothing the preview is still needed for.
        pEntry->m_bFinalExists = plOSFile::ExistsFile(pEntry->m_sFinalFile);
        if (pEntry->m_bFinalExists && pEntry->m_sOpacityFile.IsEmpty())
          return;

        if (plOSFile::ExistsFile(pEntry->m_sPreviewFile))
        {
          // Prepared in an earlier session; only a mask needs another look, to tell cutting from blending.
          if (!pEntry->m_sOpacityFile.IsEmpty())
            pEntry->m_Result = plUsdTexture::ReadSoftAlpha(pEntry->m_sPreviewFile, pEntry->m_bSoftAlpha);
          return;
        }

        pEntry->m_Result = plUsdTexture::PrepareForPreview(pEntry->m_sColorFile, pEntry->m_sOpacityFile, pEntry->m_Usage == plUsdTexture::Usage::Color,
          MaxSize, pEntry->m_sPreviewFile, &pEntry->m_bSoftAlpha);
      });

    pEntry->m_Task = plTaskSystem::StartSingleTask(pTask, plTaskPriority::LongRunning);
    m_Running.PushBack(pEntry);
  }

  // Final versions wait until no preview does: previews are what a stage that was just opened needs first.
  if (m_Queue.IsEmpty() && m_Running.IsEmpty())
  {
    const plUInt32 uiMaxConverting = GetMaxConcurrentConversions();
    while (m_Converting.GetCount() < uiMaxConverting && !m_ConversionQueue.IsEmpty())
    {
      Entry* pEntry = m_ConversionQueue.PeekFront();
      m_ConversionQueue.PopFront();
      StartConversion(*pEntry);
    }
  }
}

void plUsdLiveTextures::StartConversion(Entry& ref_entry)
{
  if (m_bConverterMissing)
  {
    // Reported when it first failed to start.
    UsePreviewAsFinal(ref_entry);
    ref_entry.m_State = State::Ready;
    ++m_uiFinished;
    return;
  }

  if (m_sConverter.IsEmpty())
    m_sConverter = plEditorApp::GetSingleton()->FindToolApplication("TexConv");

  if (plOSFile::CreateDirectoryStructure(m_sOutputFolder).Failed())
  {
    FinishConversion(ref_entry, plStatus(plFmt("could not create '{}'", m_sOutputFolder)));
    return;
  }

  // A cut-out keeps its coverage in the smaller mipmaps, as in the preview; soft alpha is averaged like colour.
  const bool bCutout = !ref_entry.m_sOpacityFile.IsEmpty() && !ref_entry.m_bSoftAlpha;

  plProcessOptions options;
  options.m_sProcess = m_sConverter;
  plUsdTexture::GetConversionArguments(ref_entry.m_sColorFile, ref_entry.m_sOpacityFile, ref_entry.m_Usage, bCutout, FinalMaxSize,
    GetConversionTempFile(ref_entry.m_sFinalFile), options.m_Arguments);

  // Runs on the process's output thread. The entry's errors are only read once the process has finished, which
  // includes its output thread.
  Entry* pEntry = &ref_entry;
  pEntry->m_sConversionErrors.Clear();
  options.m_onStdOut = [pEntry](plStringView sLine)
  {
    // TexConv's console log writes "Error: " after any indentation and time stamp.
    const char* szError = sLine.FindSubString("Error: ");
    if (szError == nullptr)
      return;

    plStringView sMessage(szError + 7, sLine.GetEndPointer());
    sMessage.Trim(" \r\n");

    plStringBuilder sErrors = pEntry->m_sConversionErrors;
    if (!sErrors.IsEmpty())
      sErrors.Append(" ");
    sErrors.Append(sMessage);
    pEntry->m_sConversionErrors = sErrors;
  };

  // Launched suspended and resumed once lowered, so every thread it starts runs below normal priority.
  ref_entry.m_pProcess = PL_DEFAULT_NEW(plProcess);
  if (ref_entry.m_pProcess->Launch(options, plProcessLaunchFlags::Suspended).Failed())
  {
    ref_entry.m_pProcess.Clear();
    m_bConverterMissing = true;
    FinishConversion(ref_entry, plStatus(plFmt("'{}' could not be started; textures keep their previews until the editor restarts", m_sConverter)));
    return;
  }

#if PL_ENABLED(PL_PLATFORM_WINDOWS_DESKTOP)
  SetPriorityClass(static_cast<HANDLE>(ref_entry.m_pProcess->GetProcessHandle()), BELOW_NORMAL_PRIORITY_CLASS);
#endif

  ref_entry.m_pProcess->ResumeSuspended().IgnoreResult();
  ref_entry.m_ConversionStart = plTime::Now();
  m_Converting.PushBack(&ref_entry);
}

void plUsdLiveTextures::FinishConversion(Entry& ref_entry, const plStatus& result)
{
  ref_entry.m_pProcess.Clear();
  ref_entry.m_State = State::Ready;
  ++m_uiFinished;

  const plString sTemp = GetConversionTempFile(ref_entry.m_sFinalFile);
  plStatus res = result;

  if (res.Succeeded())
  {
    if (plOSFile::ExistsFile(ref_entry.m_sFinalFile))
      plOSFile::DeleteFile(ref_entry.m_sFinalFile).IgnoreResult();

    if (plOSFile::MoveFileOrDirectory(sTemp, ref_entry.m_sFinalFile).Succeeded())
      return;

    res = plStatus(plFmt("could not move '{}' into place", sTemp));
  }

  plOSFile::DeleteFile(sTemp).IgnoreResult();
  plLog::Warning("USD Live: texture '{}' keeps its preview, its full-resolution version failed: {}",
    ref_entry.m_sColorFile.IsEmpty() ? ref_entry.m_sOpacityFile : ref_entry.m_sColorFile, res.m_sMessage);

  UsePreviewAsFinal(ref_entry);
}

void plUsdLiveTextures::RemoveStalePreviews()
{
  // Final versions carry "_F" in their names; anything else there is a preview, which project export would ship.
  plHybridArray<plString, 64> stale;

  plFileSystemIterator it;
  for (it.StartSearch(m_sOutputFolder, plFileSystemIteratorFlags::ReportFiles); it.IsValid(); it.Next())
  {
    const plStringView sName = it.GetStats().m_sName;
    if (sName.HasExtension(".dds") && sName.FindSubString("_F") == nullptr)
    {
      plStringBuilder sFile;
      it.GetStats().GetFullPath(sFile);
      stale.PushBack(sFile);
    }
  }

  for (const plString& sFile : stale)
    plOSFile::DeleteFile(sFile).IgnoreResult();

  if (!stale.IsEmpty())
    plLog::Dev("USD Live: removed {} preview textures from '{}'", stale.GetCount(), m_sOutputFolder);
}

void plUsdLiveTextures::UsePreviewAsFinal(Entry& ref_entry)
{
  plStringBuilder sCopy(m_sOutputFolder, "/", plPathUtils::GetFileNameAndExtension(ref_entry.m_sPreviewFile));

  if (plOSFile::CopyFile(ref_entry.m_sPreviewFile, sCopy).Succeeded())
  {
    ref_entry.m_sFinalFile = sCopy;
    return;
  }

  // Still shows in the editor; only an export misses it.
  plLog::Warning("USD Live: could not copy the preview of texture '{}' next to the final versions",
    ref_entry.m_sColorFile.IsEmpty() ? ref_entry.m_sOpacityFile : ref_entry.m_sColorFile);
  ref_entry.m_sFinalFile = ref_entry.m_sPreviewFile;
}
