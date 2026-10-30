#include <EditorPluginUsd/EditorPluginUsdPCH.h>

#include <EditorPluginUsd/Hub/UsdLiveHub.h>
#include <EditorPluginUsd/LiveLink/UsdLiveManager.h>
#include <EditorPluginUsd/UI/UsdApps.h>
#include <EditorPluginUsd/Utils/UsdJson.h>
#include <EditorUi/SettingsFile.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Strings/StringConversion.h>
#include <Foundation/System/EnvironmentVariableUtils.h>
#include <Foundation/System/Process.h>
#include <Foundation/Threading/Mutex.h>
#include <Foundation/Utilities/ConversionUtils.h>
#include <GuiFoundation/Dialogs/FileDialog.h>
#include <UsdBridge/UsdRuntime.h>

#include <algorithm>

#include <Foundation/Basics/Platform/Win/IncludeWindows.h>
#include <tlhelp32.h>

PL_IMPLEMENT_SINGLETON(plUsdApps);

namespace
{
  using App = plUsdApps::App;
  using ConnectorState = plUsdApps::ConnectorState;

  constexpr const char* ProbeMarker = "PLASMA_USD_PROBE ";

  /// Blender's repository for extensions installed from a file; the Apps tab only ever installs into and removes from it.
  constexpr const char* BlenderRepository = "user_default";

  /// A launched tool that has not joined the hub after this long has no connector, or one that is switched off.
  constexpr plTime JoinTimeout = plTime::MakeFromSeconds(120);
  constexpr plTime RunningCheckInterval = plTime::MakeFromSeconds(3);
  constexpr int ProcessTimeoutMs = 120 * 1000;

  /// Install, Remove and Open look again when the last look is older than this, so an action run long after the Apps tab
  /// was open sees what was installed since. Blender keeps its last answer while it is asked again.
  constexpr plTime OperationRescanAge = plTime::MakeFromSeconds(10);

  struct AppTraits
  {
    const char* m_szName;
    const char* m_szExecutableFilter; ///< What Locate asks for.
    const char* m_szMinimumVersion;
    const char* m_szConnectorFolder; ///< Its folder among the package's connectors.
  };

  constexpr AppTraits s_Traits[] = {
    {"Blender", "blender.exe", "4.2", "Blender"},
    {"Houdini", "houdini*.exe", "20.5", "Houdini"},
    {"Maya", "maya.exe", "2024", "Maya"},
  };

  const AppTraits& Traits(App app)
  {
    return s_Traits[(int)app];
  }

  /// The editor's settings keep what the Apps tab remembers, under "plasma.usd/Apps/<tool>/".
  plString SettingsKey(App app, const char* szKey)
  {
    plStringBuilder s;
    s.SetFormat("plasma.usd/Apps/{}/{}", Traits(app).m_szName, szKey);
    return s;
  }

  /// The executables picked with Locate, kept as one setting separated by '|'.
  void ReadLocated(App app, plDynamicArray<plString>& out_located)
  {
    out_located.Clear();

    plHybridArray<plStringView, 4> entries;
    plStringView(plEditorUiSettingsFile::GetCurrent().GetValue(SettingsKey(app, "Located"))).Split(false, entries, "|");

    for (plStringView sEntry : entries)
      out_located.PushBack(sEntry);
  }

  void WriteLocated(App app, plArrayPtr<const plString> located)
  {
    plStringBuilder sValue;
    for (const plString& sEntry : located)
    {
      if (!sValue.IsEmpty())
        sValue.Append("|");
      sValue.Append(sEntry);
    }

    plEditorUiSettingsFile::GetCurrent().SetValue(SettingsKey(app, "Located"), sValue);
  }

  plString CleanPath(plStringView sPath)
  {
    if (sPath.IsEmpty())
      return {};

    plStringBuilder s = sPath;
    s.MakeCleanPath();
    s.Trim(nullptr, "/");
    return s;
  }

  bool SamePath(plStringView a, plStringView b)
  {
    return !a.IsEmpty() && CleanPath(a).IsEqual_NoCase(CleanPath(b));
  }

  /// The folder a file or folder is in, without a trailing slash.
  plString ParentFolder(plStringView sPath)
  {
    plStringBuilder s = CleanPath(sPath);
    s.PathParentDirectory();
    return CleanPath(s);
  }

  using Version = plHybridArray<plUInt32, 4>;

  /// The numbers at the start, separated by dots: "5.2.1 LTS" reads as 5.2.1, text without a number as no version.
  Version ToVersion(plStringView sVersion)
  {
    Version version;

    const char* p = sVersion.GetStartPointer();
    const char* pEnd = sVersion.GetEndPointer();
    while (p < pEnd && *p >= '0' && *p <= '9')
    {
      plUInt32 uiNumber = 0;
      while (p < pEnd && *p >= '0' && *p <= '9')
        uiNumber = uiNumber * 10 + static_cast<plUInt32>(*p++ - '0');

      version.PushBack(uiNumber);

      if (p + 1 >= pEnd || p[0] != '.' || p[1] < '0' || p[1] > '9')
        break;
      ++p;
    }

    return version;
  }

  /// Negative, zero or positive as a is older than, the same as or newer than b. Of two versions that agree as far as
  /// both go, the longer one is newer; no version is older than any.
  int CompareVersions(const Version& a, const Version& b)
  {
    const plUInt32 uiCommon = plMath::Min(a.GetCount(), b.GetCount());
    for (plUInt32 i = 0; i < uiCommon; ++i)
    {
      if (a[i] != b[i])
        return a[i] < b[i] ? -1 : 1;
    }

    return static_cast<int>(a.GetCount()) - static_cast<int>(b.GetCount());
  }

  bool IsVersionBelow(plStringView sVersion, plStringView sThan)
  {
    return CompareVersions(ToVersion(sVersion), ToVersion(sThan)) < 0;
  }

  /// A connector without a version is from before versions were written, so older than any.
  bool IsOlder(const plString& sVersion, const plString& sThan)
  {
    return sVersion.IsEmpty() || IsVersionBelow(sVersion, sThan);
  }

  bool ReadTextFile(plStringView sFile, plStringBuilder& out_sText)
  {
    out_sText.Clear();

    plOSFile file;
    if (file.Open(sFile, plFileOpenMode::Read).Failed())
      return false;

    plDynamicArray<plUInt8> content;
    file.ReadAll(content);
    out_sText = plStringView(reinterpret_cast<const char*>(content.GetData()), content.GetCount());
    return true;
  }

  /// The value of the first `<name> = "<value>"` line in a file: a Blender manifest's version, or a module's __version__.
  plString ReadAssignedString(plStringView sFile, plStringView sName)
  {
    plStringBuilder sText;
    if (!ReadTextFile(sFile, sText))
      return {};

    plHybridArray<plStringView, 64> lines;
    sText.Split(false, lines, "\n");

    for (plStringView sLine : lines)
    {
      sLine.Trim(" \t\r", nullptr);
      if (!sLine.StartsWith(sName))
        continue;

      sLine.Shrink(plStringUtils::GetCharacterCount(sName.GetStartPointer(), sName.GetEndPointer()), 0);
      sLine.Trim(" \t", nullptr);
      if (!sLine.StartsWith("="))
        continue;

      sLine.Shrink(1, 0);
      sLine.Trim(" \t", nullptr);
      if (!sLine.StartsWith("\"") && !sLine.StartsWith("'"))
        continue;

      sLine.Shrink(1, 0);
      const char* pEnd = sLine.GetStartPointer();
      while (pEnd < sLine.GetEndPointer() && *pEnd != '"' && *pEnd != '\'')
        ++pEnd;

      if (pEnd < sLine.GetEndPointer() && pEnd > sLine.GetStartPointer())
        return plStringView(sLine.GetStartPointer(), pEnd);
    }

    return {};
  }

  /// The version of the connector in a folder laid out like the package's connectors/<app>.
  plString ReadConnectorVersion(App app, plStringView sFolder)
  {
    switch (app)
    {
      case App::Blender:
        return ReadAssignedString(plStringBuilder(sFolder, "/plasma_usd_live/blender_manifest.toml"), "version");
      case App::Houdini:
        return ReadAssignedString(plStringBuilder(sFolder, "/python3.11libs/plasma_usd_live/__init__.py"), "__version__");
      default:
        return ReadAssignedString(plStringBuilder(sFolder, "/scripts/plasma_usd_live/__init__.py"), "__version__");
    }
  }

  plString DocumentsFolder()
  {
    return CleanPath(plOSFile::GetUserDocumentsFolder());
  }

  /// Where a Houdini version keeps its preferences, as Houdini decides it: HOUDINI_USER_PREF_DIR (__HVER__ standing for
  /// the version), else houdini<major.minor> in HOME when that is set, else in Documents.
  plString HoudiniPreferenceFolder(const plString& sVersion)
  {
    const Version version = ToVersion(sVersion);

    plStringBuilder sMajorMinor;
    sMajorMinor.SetFormat("{}.{}", version.GetCount() > 0 ? version[0] : 0, version.GetCount() > 1 ? version[1] : 0);

    plStringBuilder sFolder = plEnvironmentVariableUtils::GetValueString("HOUDINI_USER_PREF_DIR");
    if (!sFolder.IsEmpty())
    {
      sFolder.ReplaceAll("__HVER__", sMajorMinor);
      return CleanPath(sFolder);
    }

    plStringBuilder sHome = plEnvironmentVariableUtils::GetValueString("HOME");
    if (sHome.IsEmpty())
      sHome = DocumentsFolder();

    return CleanPath(plStringBuilder(sHome, "/houdini", sMajorMinor));
  }

  /// The modules folder every Maya version reads: MAYA_APP_DIR/modules, else maya/modules in Documents.
  plString MayaModulesFolder()
  {
    const plString sAppFolder = plEnvironmentVariableUtils::GetValueString("MAYA_APP_DIR");
    return CleanPath(plStringBuilder(sAppFolder.IsEmpty() ? plStringBuilder(DocumentsFolder(), "/maya") : plStringBuilder(sAppFolder), "/modules"));
  }

  void GetProgramFilesFolders(plDynamicArray<plString>& out_folders)
  {
    out_folders.Clear();

    for (const char* szVariable : {"ProgramW6432", "ProgramFiles"})
    {
      const plString sFolder = CleanPath(plEnvironmentVariableUtils::GetValueString(szVariable));
      if (sFolder.IsEmpty())
        continue;

      bool bKnown = false;
      for (const plString& sKnown : out_folders)
        bKnown |= sKnown.IsEqual_NoCase(sFolder);

      if (!bKnown)
        out_folders.PushBack(sFolder);
    }
  }

  /// The folders in a folder whose names match a pattern with * and ?, case-insensitively.
  void ListFolders(plStringView sFolder, plStringView sPattern, plDynamicArray<plString>& out_names)
  {
    out_names.Clear();

    plFileSystemIterator it;
    for (it.StartSearch(plStringBuilder(sFolder, "/", sPattern), plFileSystemIteratorFlags::ReportFolders); it.IsValid(); it.Next())
      out_names.PushBack(it.GetStats().m_sName);
  }

  /// The files in a folder whose names match a pattern with * and ?.
  void ListFiles(plStringView sFolder, plStringView sPattern, plDynamicArray<plString>& out_names)
  {
    out_names.Clear();

    plFileSystemIterator it;
    for (it.StartSearch(plStringBuilder(sFolder, "/", sPattern), plFileSystemIteratorFlags::ReportFiles); it.IsValid(); it.Next())
      out_names.PushBack(it.GetStats().m_sName);
  }

  /// A path with backslashes, for command lines of other tools.
  plString NativePath(plStringView sPath)
  {
    plStringBuilder s = sPath;
    s.ReplaceAll("/", "\\");
    return s;
  }

  /// The string values of a key of HKEY_LOCAL_MACHINE, or its subkeys' names.
  void ReadRegistry(plStringView sKey, bool bSubkeys, plDynamicArray<plString>& out_entries)
  {
    out_entries.Clear();

    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, plStringWChar(sKey).GetData(), 0, KEY_READ, &hKey) != ERROR_SUCCESS)
      return;

    for (DWORD uiIndex = 0;; ++uiIndex)
    {
      wchar_t szName[512] = {};
      DWORD uiNameLength = PL_ARRAY_SIZE(szName);

      if (bSubkeys)
      {
        if (RegEnumKeyExW(hKey, uiIndex, szName, &uiNameLength, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
          break;

        out_entries.PushBack(plStringUtf8(szName).GetData());
        continue;
      }

      wchar_t szData[1024] = {};
      DWORD uiDataBytes = sizeof(szData) - sizeof(wchar_t);
      DWORD uiType = 0;
      const LSTATUS result = RegEnumValueW(hKey, uiIndex, szName, &uiNameLength, nullptr, &uiType, reinterpret_cast<BYTE*>(szData), &uiDataBytes);
      if (result == ERROR_NO_MORE_ITEMS)
        break;

      if (result == ERROR_SUCCESS && uiType == REG_SZ)
        out_entries.PushBack(plStringUtf8(szData).GetData());
    }

    RegCloseKey(hKey);
  }

  /// A string value of a key of HKEY_LOCAL_MACHINE; empty when there is none.
  plString ReadRegistryString(plStringView sKey, plStringView sValue)
  {
    wchar_t szData[1024] = {};
    DWORD uiDataBytes = sizeof(szData) - sizeof(wchar_t);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, plStringWChar(sKey).GetData(), plStringWChar(sValue).GetData(), RRF_RT_REG_SZ, nullptr, szData, &uiDataBytes) != ERROR_SUCCESS)
      return {};

    return plStringUtf8(szData).GetData();
  }

  /// The version in an installation folder's name: "Blender 5.2", "Houdini 21.0.729", "Maya2025".
  plString VersionFromFolderName(plStringView sName, const char* szPrefix)
  {
    if (!sName.StartsWith_NoCase(szPrefix))
      return {};

    sName.Shrink(plStringUtils::GetCharacterCount(szPrefix), 0);
    sName.Trim();
    return sName;
  }

  plString VersionFromExecutable(App app, plStringView sExecutable)
  {
    // Blender's executable sits in its installation folder; Houdini's and Maya's in bin/ below it.
    plString sFolder = ParentFolder(sExecutable);
    if (app != App::Blender)
      sFolder = ParentFolder(sFolder);

    return VersionFromFolderName(plStringView(sFolder).GetFileNameAndExtension(), Traits(app).m_szName);
  }

  void AddInstall(plDynamicArray<plUsdApps::Install>& ref_installs, plStringView sExecutable, plStringView sVersion, bool bLocated)
  {
    if (!plOSFile::ExistsFile(sExecutable))
      return;

    for (const plUsdApps::Install& install : ref_installs)
    {
      if (SamePath(install.m_sExecutable, sExecutable))
        return;
    }

    plUsdApps::Install& install = ref_installs.ExpandAndGetRef();
    install.m_sExecutable = CleanPath(sExecutable);
    install.m_sVersion = sVersion;
    install.m_bLocated = bLocated;
  }

  /// The line of a tool's output that says what went wrong: its last error line, else its last line.
  plString DescribeFailure(const char* szWhat, int iExitCode, plStringView sOutput)
  {
    plHybridArray<plStringView, 32> lines;
    sOutput.Split(false, lines, "\n");

    plStringView sDetail;
    for (plStringView sLine : lines)
    {
      sLine.Trim();
      if (sLine.StartsWith_NoCase("ERROR"))
        sDetail = sLine;
    }

    if (sDetail.IsEmpty() && !lines.IsEmpty())
    {
      sDetail = lines.PeekBack();
      sDetail.Trim();
    }

    plStringBuilder s;
    if (iExitCode == -1)
      s.SetFormat("{}: the tool did not start, or took longer than {} s.", szWhat, ProcessTimeoutMs / 1000);
    else if (sDetail.IsEmpty())
      s.SetFormat("{} (exit code {}).", szWhat, iExitCode);
    else
      s.SetFormat("{}: {}", szWhat, sDetail);
    return s;
  }

  /// Copies a folder, leaving out Python caches and the named files at its top level.
  plStatus CopyConnectorFolder(plStringView sFrom, plStringView sTo, plArrayPtr<const char* const> skip)
  {
    const plString sRoot = CleanPath(sFrom);

    plFileSystemIterator it;
    for (it.StartSearch(sRoot, plFileSystemIteratorFlags::ReportFilesRecursive); it.IsValid(); it.Next())
    {
      plStringBuilder sFile;
      it.GetStats().GetFullPath(sFile);
      sFile.MakeCleanPath();

      plStringBuilder sRelative = sFile;
      sRelative.MakeRelativeTo(sRoot).IgnoreResult();

      bool bSkip = sRelative.FindSubString("__pycache__") != nullptr || sRelative.EndsWith(".pyc");
      for (const char* szSkip : skip)
        bSkip |= sRelative.IsEqual_NoCase(szSkip);

      if (bSkip)
        continue;

      const plStringBuilder sTarget(sTo, "/", sRelative);
      if (plOSFile::CreateDirectoryStructure(ParentFolder(sTarget)).Failed() || plOSFile::CopyFile(sFile, sTarget).Failed())
        return plStatus(plFmt("Could not copy '{}' to '{}'.", sFile, sTarget));
    }

    return plStatus(PL_SUCCESS);
  }

  bool Exists(plStringView sPath)
  {
    return plOSFile::ExistsDirectory(sPath) || plOSFile::ExistsFile(sPath);
  }

  /// Copies a folder beside the target and then swaps it in, so a failure leaves the previous copy as it was.
  plStatus ReplaceFolder(plStringView sFrom, plStringView sTo, plArrayPtr<const char* const> skip)
  {
    const plStringBuilder sNew(sTo, ".new");
    const plStringBuilder sOld(sTo, ".old");
    plOSFile::DeleteFolder(sNew).IgnoreResult();
    plOSFile::DeleteFolder(sOld).IgnoreResult();

    const plStatus res = CopyConnectorFolder(sFrom, sNew, skip);
    if (res.Failed())
    {
      plOSFile::DeleteFolder(sNew).IgnoreResult();
      return res;
    }

    if (Exists(sTo) && plOSFile::MoveFileOrDirectory(sTo, sOld).Failed())
    {
      plOSFile::DeleteFolder(sNew).IgnoreResult();
      return plStatus(plFmt("Could not replace '{}'. Close the tool if it runs, then try again.", sTo));
    }

    if (plOSFile::MoveFileOrDirectory(sNew, sTo).Failed())
    {
      plOSFile::MoveFileOrDirectory(sOld, sTo).IgnoreResult();
      plOSFile::DeleteFolder(sNew).IgnoreResult();
      return plStatus(plFmt("Could not move the connector into '{}'.", sTo));
    }

    plOSFile::DeleteFolder(sOld).IgnoreResult();
    return plStatus(PL_SUCCESS);
  }

  plStatus WriteTextFile(plStringView sFile, plStringView sContent)
  {
    if (plUsdJson::WriteTextFile(sFile, sContent).Failed())
      return plStatus(plFmt("Could not write '{}'.", sFile));

    return plStatus(PL_SUCCESS);
  }

  plStatus RemoveInstalledCopy(plStringView sPointerFile, plStringView sFolder)
  {
    if (plOSFile::ExistsFile(sPointerFile) && plOSFile::DeleteFile(sPointerFile).Failed())
      return plStatus(plFmt("Could not delete '{}'.", sPointerFile));

    if (plOSFile::ExistsDirectory(sFolder) && plOSFile::DeleteFolder(sFolder).Failed())
      return plStatus(plFmt("Could not delete all of '{}'. Close the tool if it runs, then try again.", sFolder));

    return plStatus(PL_SUCCESS);
  }
} // namespace

struct plUsdApps::BackgroundProcess
{
  plMutex m_OutputMutex;
  plStringBuilder m_sOutput; ///< stdout and stderr, as the reader threads get them
  ProcessDone m_OnDone;
  plTime m_Started;
  bool m_bStarted = false;
  plProcess m_Process; ///< last, so the tool and its reader threads end before the output goes

  void OnOutput(plStringView sText)
  {
    PL_LOCK(m_OutputMutex);
    m_sOutput.Append(sText);
  }
};

plUsdApps::plUsdApps()
  : m_SingletonRegistrar(this)
{
  for (int i = 0; i < (int)App::Count; ++i)
    m_Apps[i].m_sSelected = plEditorUiSettingsFile::GetCurrent().GetValue(SettingsKey((App)i, "Selected"));
}

plUsdApps::~plUsdApps()
{
  // Background tools end with their processes; their callbacks never run.
  m_Timers.StopAll();
  m_Processes.Clear();

  for (Instance& instance : m_Instances)
  {
    if (instance.m_hProcess != nullptr)
      CloseHandle((HANDLE)instance.m_hProcess);
  }
}

const char* plUsdApps::GetName(App app)
{
  return Traits(app).m_szName;
}

bool plUsdApps::FindApp(plStringView sName, App& out_app)
{
  for (int i = 0; i < (int)App::Count; ++i)
  {
    if (sName.IsEqual_NoCase(s_Traits[i].m_szName))
    {
      out_app = (App)i;
      return true;
    }
  }

  return false;
}

const plDynamicArray<plUsdApps::Install>& plUsdApps::GetInstalls(App app) const
{
  return GetState(app).m_Installs;
}

plStringView plUsdApps::GetPackagedVersion(App app) const
{
  return GetState(app).m_sPackagedVersion;
}

plStringView plUsdApps::GetBusyStep(App app) const
{
  return GetState(app).m_sBusyStep;
}

plStringView plUsdApps::GetProblem(App app) const
{
  return GetState(app).m_sProblem;
}

plStringView plUsdApps::GetNotice(App app) const
{
  return GetState(app).m_sNotice;
}

void plUsdApps::RefreshIfOlder(plTime maxAge)
{
  if (!m_bRefreshed || plTime::Now() - m_LastRefresh > maxAge)
    Refresh();
}

void plUsdApps::Refresh()
{
  m_bRefreshed = true;
  m_LastRefresh = plTime::Now();

  FindConnectorFolder();

  for (int i = 0; i < (int)App::Count; ++i)
  {
    const App app = (App)i;
    AppState& state = GetState(app);

    state.m_sPackagedVersion.Clear();
    if (!m_sConnectorFolder.IsEmpty())
      state.m_sPackagedVersion = ReadConnectorVersion(app, plStringBuilder(m_sConnectorFolder, "/", Traits(app).m_szConnectorFolder));

    ScanInstalls(app, state.m_Installs);

    for (Install& install : state.m_Installs)
    {
      if (app == App::Blender && install.m_sUnsupported.IsEmpty())
        ProbeBlender(install.m_sExecutable, false);

      EvaluateConnector(app, install);
    }
  }

  m_NextRunningCheck = plTime::MakeZero();
  UpdateRunning();
}

void plUsdApps::FindConnectorFolder()
{
  m_sConnectorFolder.Clear();

  const plString sBinaries = CleanPath(plUsdRuntime::GetOpenUsdDirectory());
  if (sBinaries.IsEmpty())
    return;

  // Staged beside the plugin by the build, so an installed package has them.
  plHybridArray<plString, 3> candidates;
  candidates.PushBack(plStringBuilder(sBinaries, "/connectors"));

  // A package linked to its source names that folder in PackageSource.txt; the package is two levels above Bin/<configuration>.
  const plString sPackage = ParentFolder(ParentFolder(sBinaries));

  plStringBuilder sSource;
  if (ReadTextFile(plStringBuilder(sPackage, "/PackageSource.txt"), sSource))
  {
    sSource.Trim();
    candidates.PushBack(plStringBuilder(CleanPath(sSource), "/Connectors"));
  }

  candidates.PushBack(plStringBuilder(sPackage, "/Connectors"));

  for (const plString& sCandidate : candidates)
  {
    for (const AppTraits& traits : s_Traits)
    {
      if (plOSFile::ExistsDirectory(plStringBuilder(sCandidate, "/", traits.m_szConnectorFolder)))
      {
        m_sConnectorFolder = CleanPath(sCandidate);
        return;
      }
    }
  }
}

void plUsdApps::ScanInstalls(App app, plDynamicArray<Install>& out_installs) const
{
  out_installs.Clear();

  plHybridArray<plString, 2> programFiles;
  GetProgramFilesFolders(programFiles);

  plHybridArray<plString, 8> names;

  switch (app)
  {
    case App::Blender:
    {
      for (const plString& sProgramFiles : programFiles)
      {
        const plStringBuilder sFoundation(sProgramFiles, "/Blender Foundation");
        ListFolders(sFoundation, "Blender*", names);
        for (const plString& sInstall : names)
          AddInstall(out_installs, plStringBuilder(sFoundation, "/", sInstall, "/blender.exe"), VersionFromFolderName(sInstall, "Blender"), false);
      }

      const plString sProgramFilesX86 = CleanPath(plEnvironmentVariableUtils::GetValueString("ProgramFiles(x86)"));
      AddInstall(out_installs, plStringBuilder(sProgramFilesX86, "/Steam/steamapps/common/Blender/blender.exe"), {}, false);
      break;
    }

    case App::Houdini:
    {
      // The installer and the Houdini Launcher record each version as a value: "21.0.0.729" = its folder.
      ReadRegistry("SOFTWARE\\Side Effects Software\\Houdini", false, names);
      for (const plString& sValue : names)
      {
        const plString sFolder = CleanPath(sValue);
        if (!sFolder.IsEmpty())
          AddInstall(out_installs, plStringBuilder(sFolder, "/bin/houdini.exe"), VersionFromFolderName(plStringView(sFolder).GetFileNameAndExtension(), "Houdini"), false);
      }

      for (const plString& sProgramFiles : programFiles)
      {
        const plStringBuilder sVendor(sProgramFiles, "/Side Effects Software");
        ListFolders(sVendor, "Houdini *", names);
        for (const plString& sInstall : names)
          AddInstall(out_installs, plStringBuilder(sVendor, "/", sInstall, "/bin/houdini.exe"), VersionFromFolderName(sInstall, "Houdini"), false);
      }
      break;
    }

    case App::Maya:
    {
      ReadRegistry("SOFTWARE\\Autodesk\\Maya", true, names);
      for (const plString& sVersion : names)
      {
        const plString sFolder = CleanPath(ReadRegistryString(plStringBuilder("SOFTWARE\\Autodesk\\Maya\\", sVersion, "\\Setup\\InstallPath"), "MAYA_INSTALL_LOCATION"));
        if (!sFolder.IsEmpty())
          AddInstall(out_installs, plStringBuilder(sFolder, "/bin/maya.exe"), sVersion, false);
      }

      for (const plString& sProgramFiles : programFiles)
      {
        const plStringBuilder sVendor(sProgramFiles, "/Autodesk");
        ListFolders(sVendor, "Maya*", names);
        for (const plString& sInstall : names)
          AddInstall(out_installs, plStringBuilder(sVendor, "/", sInstall, "/bin/maya.exe"), VersionFromFolderName(sInstall, "Maya"), false);
      }
      break;
    }

    default:
      break;
  }

  plHybridArray<plString, 4> located;
  ReadLocated(app, located);

  for (const plString& sExecutable : located)
    AddInstall(out_installs, sExecutable, VersionFromExecutable(app, sExecutable), true);

  for (Install& install : out_installs)
  {
    if (!install.m_sVersion.IsEmpty() && IsVersionBelow(install.m_sVersion, Traits(app).m_szMinimumVersion))
    {
      plStringBuilder s;
      s.SetFormat("The connector needs {} {} or later.", Traits(app).m_szName, Traits(app).m_szMinimumVersion);
      install.m_sUnsupported = s;
    }
  }

  // Newest first; the first is the default choice.
  std::stable_sort(out_installs.GetData(), out_installs.GetData() + out_installs.GetCount(), [](const Install& a, const Install& b)
    { return CompareVersions(ToVersion(a.m_sVersion), ToVersion(b.m_sVersion)) > 0; });
}

const plUsdApps::Install* plUsdApps::GetSelectedInstall(App app) const
{
  const AppState& state = GetState(app);
  if (state.m_Installs.IsEmpty())
    return nullptr;

  for (const Install& install : state.m_Installs)
  {
    if (install.m_sExecutable.IsEqual_NoCase(state.m_sSelected))
      return &install;
  }

  for (const Install& install : state.m_Installs)
  {
    if (install.m_sUnsupported.IsEmpty())
      return &install;
  }

  return &state.m_Installs[0];
}

plUsdApps::Install* plUsdApps::GetSelectedInstallMutable(App app)
{
  return const_cast<Install*>(GetSelectedInstall(app));
}

void plUsdApps::SelectInstall(App app, plStringView sExecutable)
{
  AppState& state = GetState(app);
  state.m_sSelected = sExecutable;
  state.m_sProblem.Clear();
  state.m_sNotice.Clear();
  plEditorUiSettingsFile::GetCurrent().SetValue(SettingsKey(app, "Selected"), sExecutable);
}

void plUsdApps::EvaluateConnector(App app, Install& ref_install) const
{
  switch (app)
  {
    case App::Blender:
      EvaluateBlender(ref_install);
      break;
    case App::Houdini:
      EvaluateHoudini(ref_install);
      break;
    case App::Maya:
      EvaluateMaya(ref_install);
      break;
    default:
      break;
  }
}

void plUsdApps::EvaluateBlender(Install& ref_install) const
{
  Connector& connector = ref_install.m_Connector;

  const BlenderProbe* pProbe = m_BlenderProbes.GetValue(ref_install.m_sExecutable);
  if (pProbe == nullptr || (!pProbe->m_bValid && pProbe->m_bRunning))
  {
    connector = {};
    connector.m_State = ConnectorState::Checking;
    return;
  }

  if (!pProbe->m_bValid)
  {
    connector = {};
    connector.m_State = ConnectorState::Unknown;
    connector.m_sNote = pProbe->m_sError;
    return;
  }

  if (!pProbe->m_sVersion.IsEmpty())
  {
    ref_install.m_sVersion = pProbe->m_sVersion;

    ref_install.m_sUnsupported.Clear();
    if (IsVersionBelow(ref_install.m_sVersion, Traits(App::Blender).m_szMinimumVersion))
      ref_install.m_sUnsupported = "The connector needs Blender 4.2 or later.";
  }

  // The connector can sit in several repositories; the one in user_default is the copy the Apps tab manages.
  const BlenderProbe::Repository* pOwn = nullptr;
  const BlenderProbe::Repository* pOther = nullptr;
  bool bOwnEnabled = false;
  bool bOtherEnabled = false;

  for (const BlenderProbe::Repository& repository : pProbe->m_Repositories)
  {
    if (!plOSFile::ExistsFile(plStringBuilder(repository.m_sDirectory, "/plasma_usd_live/blender_manifest.toml")))
      continue;

    const plString sModule = plStringBuilder("bl_ext.", repository.m_sId, ".plasma_usd_live");
    const bool bEnabled = repository.m_bEnabled && pProbe->m_Addons.Contains(sModule);

    if (repository.m_sId == BlenderRepository)
    {
      pOwn = &repository;
      bOwnEnabled = bEnabled;
    }
    else if (pOther == nullptr || bEnabled)
    {
      pOther = &repository;
      bOtherEnabled = bEnabled;
    }
  }

  connector = {};

  if (pOther != nullptr && (bOtherEnabled || pOwn == nullptr))
  {
    plStringBuilder sLocation;
    sLocation.SetFormat("in Blender's repository '{}'", pOther->m_sName);

    connector.m_State = ConnectorState::Linked;
    connector.m_sVersion = ReadConnectorVersion(App::Blender, pOther->m_sDirectory);
    connector.m_sLocation = sLocation;
    connector.m_sPath = pOther->m_sDirectory;
    if (!bOtherEnabled)
      connector.m_sNote = "Switched off in Blender's preferences.";
    return;
  }

  if (pOwn != nullptr)
  {
    connector.m_sVersion = ReadConnectorVersion(App::Blender, pOwn->m_sDirectory);
    connector.m_sLocation = "extension in the user_default repository";
    connector.m_sPath = plStringBuilder(pOwn->m_sDirectory, "/plasma_usd_live");

    if (!bOwnEnabled)
      connector.m_State = ConnectorState::Disabled;
    else if (IsOlder(connector.m_sVersion, GetState(App::Blender).m_sPackagedVersion))
      connector.m_State = ConnectorState::Outdated;
    else
      connector.m_State = ConnectorState::Installed;
    return;
  }

  connector.m_State = ConnectorState::NotInstalled;
  for (const BlenderProbe::Repository& repository : pProbe->m_Repositories)
  {
    if (repository.m_sId == BlenderRepository)
      connector.m_sPath = repository.m_sDirectory;
  }
}

void plUsdApps::EvaluateHoudini(Install& ref_install) const
{
  Connector& connector = ref_install.m_Connector;
  connector = {};

  const plString sPreferences = HoudiniPreferenceFolder(ref_install.m_sVersion);
  const plStringBuilder sOwn(sPreferences, "/plasma_usd_live");
  const plStringBuilder sPackageFile(sPreferences, "/packages/plasma_usd_live.json");

  if (!plOSFile::ExistsFile(sPackageFile))
  {
    connector.m_State = ConnectorState::NotInstalled;
    connector.m_sPath = sPreferences;
    return;
  }

  // install.py and the Apps tab both name the folder in an environment variable that "path" then uses.
  plVariantDictionary package;
  plUsdJson::ReadFile(sPackageFile, package);

  plString sFolder;
  for (const plVariant& entry : plUsdJson::GetArray(package, "env"))
  {
    const plVariant* pValue = plUsdJson::AsObject(entry).GetValue("PLASMA_USD_LIVE_HOUDINI");
    if (pValue != nullptr && pValue->IsA<plString>())
      sFolder = pValue->Get<plString>();
  }

  if (sFolder.IsEmpty())
    sFolder = plUsdJson::GetString(package, "path");

  sFolder = CleanPath(sFolder);
  connector.m_sVersion = ReadConnectorVersion(App::Houdini, sFolder);

  if (SamePath(sFolder, sOwn))
  {
    connector.m_State = IsOlder(connector.m_sVersion, GetState(App::Houdini).m_sPackagedVersion) ? ConnectorState::Outdated : ConnectorState::Installed;
    connector.m_sLocation = plStringBuilder("package in ", sPreferences);
    connector.m_sPath = sOwn;
  }
  else
  {
    connector.m_State = ConnectorState::Linked;
    connector.m_sLocation = plStringBuilder("linked to ", sFolder);
    connector.m_sPath = sPackageFile;
  }
}

void plUsdApps::EvaluateMaya(Install& ref_install) const
{
  Connector& connector = ref_install.m_Connector;
  connector = {};

  const plString sModules = MayaModulesFolder();
  const plStringBuilder sOwn(sModules, "/plasma_usd_live");
  const plStringBuilder sModuleFile(sModules, "/plasma_usd_live.mod");

  plStringBuilder sText;
  if (!ReadTextFile(sModuleFile, sText))
  {
    connector.m_State = ConnectorState::NotInstalled;
    connector.m_sPath = sModules;
    return;
  }

  // "+ [conditions] plasma_usd_live <version> <path>"; conditions such as MAYAVERSION:2025 contain a colon.
  plString sVersion;
  plStringBuilder sFolder;

  plHybridArray<plStringView, 16> lines;
  sText.Split(false, lines, "\n");

  for (const plStringView sLine : lines)
  {
    plHybridArray<plStringView, 8> parts;
    sLine.Split(false, parts, " ", "\t", "\r");
    if (parts.IsEmpty() || parts[0] != "+")
      continue;

    plUInt32 uiFirst = 1;
    while (uiFirst < parts.GetCount() && parts[uiFirst].FindSubString(":") != nullptr)
      ++uiFirst;

    if (parts.GetCount() >= uiFirst + 3 && parts[uiFirst] == "plasma_usd_live")
    {
      sVersion = parts[uiFirst + 1];
      for (plUInt32 i = uiFirst + 2; i < parts.GetCount(); ++i)
      {
        if (i > uiFirst + 2)
          sFolder.Append(" ");
        sFolder.Append(parts[i]);
      }
      break;
    }
  }

  if (sFolder.IsRelativePath())
    sFolder.Prepend(sModules, "/");
  sFolder = CleanPath(sFolder);

  const plString sConnectorVersion = ReadConnectorVersion(App::Maya, sFolder);
  connector.m_sVersion = sConnectorVersion.IsEmpty() ? sVersion : sConnectorVersion;

  if (SamePath(sFolder, sOwn))
  {
    connector.m_State = IsOlder(connector.m_sVersion, GetState(App::Maya).m_sPackagedVersion) ? ConnectorState::Outdated : ConnectorState::Installed;
    connector.m_sLocation = plStringBuilder("module in ", sModules);
    connector.m_sPath = sOwn;
  }
  else
  {
    connector.m_State = ConnectorState::Linked;
    connector.m_sLocation = plStringBuilder("linked to ", sFolder);
    connector.m_sPath = sModuleFile;
  }
}

void plUsdApps::UpdateRunning()
{
  const plTime now = plTime::Now();
  if (now < m_NextRunningCheck)
    return;

  m_NextRunningCheck = now + RunningCheckInterval;

  // Only processes named like the tools are asked for their image path; asking every process is slow and mostly denied.
  plHybridArray<plString, 8> runningFolders;

  HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (hSnapshot != INVALID_HANDLE_VALUE)
  {
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);

    for (BOOL bNext = Process32FirstW(hSnapshot, &entry); bNext; bNext = Process32NextW(hSnapshot, &entry))
    {
      plStringBuilder sName = plStringUtf8(entry.szExeFile).GetData();
      sName.ToLower();
      if (!sName.StartsWith("blender") && !sName.StartsWith("houdini") && !sName.StartsWith("happrentice") && !sName.StartsWith("hindie") && !sName.StartsWith("maya"))
        continue;

      HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
      if (hProcess == nullptr)
        continue;

      wchar_t szPath[MAX_PATH * 2] = {};
      DWORD uiLength = PL_ARRAY_SIZE(szPath);
      if (QueryFullProcessImageNameW(hProcess, 0, szPath, &uiLength))
        runningFolders.PushBack(ParentFolder(plStringUtf8(szPath).GetData()));

      CloseHandle(hProcess);
    }

    CloseHandle(hSnapshot);
  }

  for (AppState& state : m_Apps)
  {
    bool bAnyRunning = false;
    for (Install& install : state.m_Installs)
    {
      const plString sFolder = ParentFolder(install.m_sExecutable);
      install.m_bRunning = std::any_of(runningFolders.GetData(), runningFolders.GetData() + runningFolders.GetCount(), [&](const plString& sRunning)
        { return SamePath(sRunning, sFolder); });
      bAnyRunning |= install.m_bRunning;
    }

    // The notice says a running tool loads the connector on its next start; once it no longer runs, that is done.
    if (!bAnyRunning)
      state.m_sNotice.Clear();
  }
}

void plUsdApps::Update()
{
  if (m_bRefreshed)
    UpdateRunning();

  plUsdLiveManager* pManager = plUsdLiveManager::GetSingleton();
  plUsdLiveHub* pHub = pManager != nullptr ? pManager->GetHub() : nullptr;
  const plTime now = plTime::Now();

  for (plUInt32 i = m_Instances.GetCount(); i-- > 0;)
  {
    Instance& instance = m_Instances[i];

    if (instance.m_hProcess == nullptr || WaitForSingleObject((HANDLE)instance.m_hProcess, 0) != WAIT_TIMEOUT)
    {
      plLog::Dev("USD Live: {} (process {}) has exited", GetName(instance.m_App), instance.m_uiProcessId);

      if (instance.m_hProcess != nullptr)
        CloseHandle((HANDLE)instance.m_hProcess);

      m_Instances.RemoveAtAndCopy(i);
      continue;
    }

    plHybridArray<plUsdLiveHub::Peer, 8> peers;
    if (pHub != nullptr)
      pHub->GetPeers(instance.m_sSessionRoot, peers);

    const plUsdLiveHub::Peer* pPeer = nullptr;
    for (const plUsdLiveHub::Peer& peer : peers)
    {
      if (peer.m_uiProcessId == instance.m_uiProcessId)
        pPeer = &peer;
    }

    if (pPeer != nullptr)
    {
      instance.m_State = InstanceState::Live;
      instance.m_sParticipant = pPeer->m_sParticipant;
      instance.m_fLatencyMs = pPeer->m_fLatencyMs;
    }
    else if (instance.m_State == InstanceState::Live || instance.m_State == InstanceState::Left)
    {
      instance.m_State = InstanceState::Left;
    }
    else
    {
      instance.m_State = now - instance.m_Launched > JoinTimeout ? InstanceState::NotConnected : InstanceState::Starting;
    }
  }
}

void plUsdApps::GetInstances(plStringView sSessionRoot, plDynamicArray<const Instance*>& out_instances) const
{
  out_instances.Clear();

  if (sSessionRoot.IsEmpty())
    return;

  for (const Instance& instance : m_Instances)
  {
    if (SamePath(instance.m_sSessionRoot, sSessionRoot))
      out_instances.PushBack(&instance);
  }
}

void plUsdApps::RunProcess(plStringView sProgram, plArrayPtr<const plString> args, ProcessDone onDone)
{
  plUniquePtr<BackgroundProcess> pProcess = PL_DEFAULT_NEW(BackgroundProcess);
  BackgroundProcess* pRaw = pProcess.Borrow();
  pRaw->m_OnDone = std::move(onDone);
  pRaw->m_Started = plTime::Now();

  plProcessOptions options;
  options.m_sProcess = sProgram;
  for (const plString& sArg : args)
    options.m_Arguments.PushBack(sArg);

  options.m_onStdOut = plMakeDelegate(&BackgroundProcess::OnOutput, pRaw);
  options.m_onStdError = plMakeDelegate(&BackgroundProcess::OnOutput, pRaw);

  pRaw->m_bStarted = pRaw->m_Process.Launch(options).Succeeded();
  m_Processes.PushBack(std::move(pProcess));

  if (!m_Timers.IsActive(m_uiPollTimer))
  {
    m_uiPollTimer = m_Timers.Start(plTime::MakeFromMilliseconds(100), [this]()
      { PollProcesses(); }, true);
  }
}

void plUsdApps::PollProcesses()
{
  // A callback can start further tools, so the ended ones are taken out first.
  plHybridArray<plUniquePtr<BackgroundProcess>, 4> ended;
  for (plUInt32 i = 0; i < m_Processes.GetCount();)
  {
    BackgroundProcess& process = *m_Processes[i];
    const bool bTimedOut = plTime::Now() - process.m_Started > plTime::MakeFromMilliseconds(ProcessTimeoutMs);
    if (process.m_bStarted && !bTimedOut && process.m_Process.GetState() == plProcessState::Running)
    {
      ++i;
      continue;
    }

    ended.PushBack(std::move(m_Processes[i]));
    m_Processes.RemoveAtAndCopy(i);
  }

  for (plUniquePtr<BackgroundProcess>& pProcess : ended)
  {
    int iExitCode = -1;
    if (pProcess->m_bStarted)
    {
      if (pProcess->m_Process.GetState() == plProcessState::Finished)
      {
        pProcess->m_Process.WaitToFinish(plTime::MakeFromSeconds(1)).IgnoreResult();
        iExitCode = pProcess->m_Process.GetExitCode();
      }
      else
      {
        pProcess->m_Process.Terminate().IgnoreResult();
      }
    }

    plString sOutput;
    {
      PL_LOCK(pProcess->m_OutputMutex);
      sOutput = pProcess->m_sOutput;
    }

    pProcess->m_OnDone(iExitCode, sOutput);
  }

  if (m_Processes.IsEmpty())
    m_Timers.Stop(m_uiPollTimer);
}

void plUsdApps::ProbeBlender(const plString& sExecutable, bool bForget)
{
  BlenderProbe& probe = m_BlenderProbes[sExecutable];
  if (bForget)
    probe.m_bValid = false;

  if (probe.m_bRunning)
    return;

  probe.m_bRunning = true;

  // A background Blender with the user's own preferences lists its extension repositories and enabled add-ons.
  plStringBuilder sScript("import bpy, json; p = bpy.context.preferences; print('", ProbeMarker);
  sScript.Append("' + json.dumps({'version': bpy.app.version_string, "
                 "'repos': [{'id': r.module, 'name': r.name, 'dir': r.directory, 'enabled': r.enabled} for r in p.extensions.repos], "
                 "'addons': [a.module for a in p.addons]}), flush=True)");

  const plString args[] = {"--background", "--python-expr", sScript};
  RunProcess(sExecutable, args, [this, sExecutable](int iExitCode, plStringView sOutput)
    { ParseBlenderProbe(sExecutable, iExitCode, sOutput); });
}

void plUsdApps::ParseBlenderProbe(const plString& sExecutable, int iExitCode, plStringView sOutput)
{
  BlenderProbe& probe = m_BlenderProbes[sExecutable];
  probe.m_bRunning = false;
  probe.m_bValid = false;
  probe.m_Repositories.Clear();
  probe.m_Addons.Clear();
  probe.m_sError.Clear();

  const char* pMarker = sOutput.FindSubString(ProbeMarker);
  if (pMarker == nullptr)
  {
    probe.m_sError = DescribeFailure("Blender did not list its extensions", iExitCode, sOutput);
  }
  else
  {
    const char* pStart = pMarker + plStringUtils::GetStringElementCount(ProbeMarker);
    const char* pEnd = plStringView(pStart, sOutput.GetEndPointer()).FindSubString("\n");
    if (pEnd == nullptr)
      pEnd = sOutput.GetEndPointer();

    plStringView sJson(pStart, pEnd);
    sJson.Trim();

    plVariantDictionary root;
    if (!plUsdJson::Parse(sJson, root))
    {
      probe.m_sError = "Blender's list of its extensions could not be read.";
    }
    else
    {
      probe.m_bValid = true;
      probe.m_sVersion = plUsdJson::GetString(root, "version");

      for (const plVariant& value : plUsdJson::GetArray(root, "repos"))
      {
        const plVariantDictionary& repository = plUsdJson::AsObject(value);
        BlenderProbe::Repository& entry = probe.m_Repositories.ExpandAndGetRef();
        entry.m_sId = plUsdJson::GetString(repository, "id");
        entry.m_sName = plUsdJson::GetString(repository, "name");
        entry.m_sDirectory = CleanPath(plUsdJson::GetString(repository, "dir"));
        entry.m_bEnabled = plUsdJson::GetBool(repository, "enabled");
      }

      for (const plVariant& value : plUsdJson::GetArray(root, "addons"))
        probe.m_Addons.PushBack(plUsdJson::AsString(value));
    }
  }

  for (Install& install : GetState(App::Blender).m_Installs)
  {
    if (install.m_sExecutable == sExecutable)
      EvaluateBlender(install);
  }
}

plStatus plUsdApps::InstallConnector(App app)
{
  return Report(app, "Installing the connector", DoInstall(app));
}

plStatus plUsdApps::RemoveConnector(App app)
{
  return Report(app, "Removing the connector", DoRemove(app));
}

plStatus plUsdApps::Open(App app, plUsdLiveLink* pLink)
{
  return Report(app, "Opening the stage", DoOpen(app, pLink));
}

plStatus plUsdApps::Report(App app, const char* szWhat, plStatus res)
{
  if (res.Failed())
  {
    GetState(app).m_sProblem = res.m_sMessage;
    plLog::Error("USD Live: {} in {} failed: {}", szWhat, GetName(app), res.m_sMessage);
  }

  return res;
}

plStatus plUsdApps::DoInstall(App app)
{
  RefreshIfOlder(OperationRescanAge);

  AppState& state = GetState(app);
  Install* pInstall = GetSelectedInstallMutable(app);

  state.m_sProblem.Clear();
  state.m_sNotice.Clear();

  if (!state.m_sBusyStep.IsEmpty())
    return plStatus(plFmt("{} is busy: {}.", GetName(app), state.m_sBusyStep));

  if (pInstall == nullptr)
    return plStatus(plFmt("No {} was found. Locate it first.", GetName(app)));

  if (!pInstall->m_sUnsupported.IsEmpty())
    return plStatus(pInstall->m_sUnsupported.GetView());

  if (state.m_sPackagedVersion.IsEmpty())
    return plStatus(plFmt("This installation of plasma.usd has no {} connector.", GetName(app)));

  switch (pInstall->m_Connector.m_State)
  {
    case ConnectorState::Checking:
      return plStatus("Blender is still being asked about its extensions. Try again in a moment.");

    case ConnectorState::Linked:
      return plStatus(plFmt("{} {} has the connector {}, which is left alone. Remove that first to install a copy.", GetName(app), pInstall->m_sVersion,
        pInstall->m_Connector.m_sLocation));

    default:
      break;
  }

  if (app == App::Blender)
  {
    InstallBlender(*pInstall);
    return plStatus(PL_SUCCESS);
  }

  const plStatus res = app == App::Houdini ? InstallHoudini(*pInstall) : InstallMaya(*pInstall);
  if (res.Succeeded())
  {
    plLog::Info("USD Live: installed the {} connector {} for {} {}", GetName(app), state.m_sPackagedVersion, GetName(app), pInstall->m_sVersion);
    if (pInstall->m_bRunning)
    {
      plStringBuilder sNotice;
      sNotice.SetFormat("{} is running; it loads the connector when it starts next.", GetName(app));
      state.m_sNotice = sNotice;
    }
  }

  EvaluateConnector(app, *pInstall);
  return res;
}

plStatus plUsdApps::DoRemove(App app)
{
  RefreshIfOlder(OperationRescanAge);

  AppState& state = GetState(app);
  Install* pInstall = GetSelectedInstallMutable(app);

  state.m_sProblem.Clear();
  state.m_sNotice.Clear();

  if (!state.m_sBusyStep.IsEmpty())
    return plStatus(plFmt("{} is busy: {}.", GetName(app), state.m_sBusyStep));

  if (pInstall == nullptr)
    return plStatus(plFmt("No {} was found.", GetName(app)));

  switch (pInstall->m_Connector.m_State)
  {
    case ConnectorState::Installed:
    case ConnectorState::Outdated:
    case ConnectorState::Disabled:
      break;

    case ConnectorState::Linked:
      return plStatus(plFmt("The connector is {}, which the Apps tab leaves alone.", pInstall->m_Connector.m_sLocation));

    default:
      return plStatus(plFmt("{} {} has no connector installed by the Apps tab.", GetName(app), pInstall->m_sVersion));
  }

  if (app == App::Blender)
  {
    RemoveBlender(*pInstall);
    return plStatus(PL_SUCCESS);
  }

  const plStatus res = app == App::Houdini ? RemoveHoudini(*pInstall) : RemoveMaya(*pInstall);
  if (res.Succeeded())
    plLog::Info("USD Live: removed the {} connector from {} {}", GetName(app), GetName(app), pInstall->m_sVersion);

  EvaluateConnector(app, *pInstall);
  return res;
}

void plUsdApps::InstallBlender(const Install& install)
{
  const plString sExecutable = install.m_sExecutable;
  const bool bRunning = install.m_bRunning;
  const plStringBuilder sSource(m_sConnectorFolder, "/Blender/plasma_usd_live");
  const plString sBuildFolder = CleanPath(plOSFile::GetTempDataFolder("plasma_usd_live_build"));

  plOSFile::DeleteFolder(sBuildFolder).IgnoreResult();
  plOSFile::CreateDirectoryStructure(sBuildFolder).IgnoreResult();

  GetState(App::Blender).m_sBusyStep = "building the extension";

  // Blender builds the extension's zip itself, from its manifest, and installs it enabled into user_default.
  const plString buildArgs[] = {"--command", "extension", "build", "--source-dir", NativePath(sSource), "--output-dir", NativePath(sBuildFolder)};
  RunProcess(sExecutable, buildArgs, [this, sExecutable, sBuildFolder, bRunning](int iExitCode, plStringView sOutput)
    {
      AppState& state = GetState(App::Blender);

      plHybridArray<plString, 2> zips;
      ListFiles(sBuildFolder, "*.zip", zips);
      if (iExitCode != 0 || zips.IsEmpty())
      {
        plOSFile::DeleteFolder(sBuildFolder).IgnoreResult();
        state.m_sBusyStep.Clear();
        state.m_sProblem = DescribeFailure("Building the extension failed", iExitCode, sOutput);
        plLog::Error("USD Live: {}", state.m_sProblem);
        return;
      }

      state.m_sBusyStep = "installing into Blender";

      const plString installArgs[] = {"--command", "extension", "install-file", "--repo", BlenderRepository, "--enable", NativePath(plStringBuilder(sBuildFolder, "/", zips[0]))};
      RunProcess(sExecutable, installArgs, [this, sExecutable, sBuildFolder, bRunning](int iExitCode, plStringView sOutput)
        {
          plOSFile::DeleteFolder(sBuildFolder).IgnoreResult();

          AppState& state = GetState(App::Blender);
          state.m_sBusyStep.Clear();

          if (iExitCode != 0 || (sOutput.FindSubString("STATUS Installed") == nullptr && sOutput.FindSubString("STATUS Reinstalled") == nullptr))
          {
            state.m_sProblem = DescribeFailure("Installing the extension failed", iExitCode, sOutput);
            plLog::Error("USD Live: {}", state.m_sProblem);
          }
          else
          {
            plLog::Info("USD Live: installed the Blender connector {} with '{}'", state.m_sPackagedVersion, sExecutable);
            if (bRunning)
              state.m_sNotice = "Blender is running; it loads the connector when it starts next.";
          }

          ProbeBlender(sExecutable, true);
          for (Install& other : state.m_Installs)
          {
            if (other.m_sExecutable == sExecutable)
              EvaluateBlender(other);
          }
        });
    });
}

void plUsdApps::RemoveBlender(const Install& install)
{
  const plString sExecutable = install.m_sExecutable;
  GetState(App::Blender).m_sBusyStep = "removing the extension";

  // Always named with its repository: another repository may hold a linked copy, whose files are the package source.
  const plString args[] = {"--command", "extension", "remove", plStringBuilder(BlenderRepository, ".plasma_usd_live")};
  RunProcess(sExecutable, args, [this, sExecutable](int iExitCode, plStringView sOutput)
    {
      AppState& state = GetState(App::Blender);
      state.m_sBusyStep.Clear();

      if (iExitCode != 0 || sOutput.FindSubString("STATUS Removed") == nullptr)
      {
        state.m_sProblem = DescribeFailure("Removing the extension failed", iExitCode, sOutput);
        plLog::Error("USD Live: {}", state.m_sProblem);
      }
      else
      {
        plLog::Info("USD Live: removed the Blender connector with '{}'", sExecutable);
      }

      ProbeBlender(sExecutable, true);
      for (Install& other : state.m_Installs)
      {
        if (other.m_sExecutable == sExecutable)
          EvaluateBlender(other);
      }
    });
}

plStatus plUsdApps::InstallHoudini(const Install& install)
{
  if (install.m_sVersion.IsEmpty())
    return plStatus(plFmt("The version of '{}' is not known, so neither is its preference folder.", install.m_sExecutable));

  const plString sPreferences = HoudiniPreferenceFolder(install.m_sVersion);
  const plStringBuilder sOwn(sPreferences, "/plasma_usd_live");

  const char* const skip[] = {"install.py"};
  const plStatus res = ReplaceFolder(plStringBuilder(m_sConnectorFolder, "/Houdini"), sOwn, skip);
  if (res.Failed())
    return res;

  // The same package file install.py writes, pointing at the copy instead of the package source.
  plVariantDictionary variable;
  variable.Insert("PLASMA_USD_LIVE_HOUDINI", plString(sOwn));

  plVariantArray env;
  env.PushBack(variable);

  plVariantDictionary package;
  package.Insert("env", env);
  package.Insert("path", plString("$PLASMA_USD_LIVE_HOUDINI"));

  return WriteTextFile(plStringBuilder(sPreferences, "/packages/plasma_usd_live.json"), plUsdJson::Write(package, true));
}

plStatus plUsdApps::RemoveHoudini(const Install& install)
{
  const plString sPreferences = HoudiniPreferenceFolder(install.m_sVersion);
  return RemoveInstalledCopy(plStringBuilder(sPreferences, "/packages/plasma_usd_live.json"), plStringBuilder(sPreferences, "/plasma_usd_live"));
}

plStatus plUsdApps::InstallMaya(const Install& install)
{
  const plString sModules = MayaModulesFolder();

  const char* const skip[] = {"install.py", "plasma_usd_live.mod"};
  const plStatus res = ReplaceFolder(plStringBuilder(m_sConnectorFolder, "/Maya"), plStringBuilder(sModules, "/plasma_usd_live"), skip);
  if (res.Failed())
    return res;

  // Relative to the module file, so a Documents path with spaces in it cannot break the line.
  plStringBuilder sLine;
  sLine.SetFormat("+ plasma_usd_live {} ./plasma_usd_live\n", GetState(App::Maya).m_sPackagedVersion);
  return WriteTextFile(plStringBuilder(sModules, "/plasma_usd_live.mod"), sLine);
}

plStatus plUsdApps::RemoveMaya(const Install& install)
{
  const plString sModules = MayaModulesFolder();
  return RemoveInstalledCopy(plStringBuilder(sModules, "/plasma_usd_live.mod"), plStringBuilder(sModules, "/plasma_usd_live"));
}

plStatus plUsdApps::Locate(App app)
{
  plHybridArray<plString, 2> programFiles;
  GetProgramFilesFolders(programFiles);

  plStringBuilder sCaption, sFilter;
  sCaption.SetFormat("Where is {}?", Traits(app).m_szName);
  sFilter.SetFormat("{} ({})", Traits(app).m_szName, Traits(app).m_szExecutableFilter);

  const plString sChosen = plFileDialog::GetOpenFileName(sCaption, programFiles.IsEmpty() ? plStringView() : plStringView(programFiles[0]), sFilter);
  if (sChosen.IsEmpty())
    return plStatus(PL_SUCCESS);

  return AddExecutable(app, sChosen);
}

plStatus plUsdApps::AddExecutable(App app, plStringView sExecutable)
{
  const plString sPath = CleanPath(sExecutable);
  if (!plOSFile::ExistsFile(sPath))
    return plStatus(plFmt("'{}' does not exist.", sExecutable));

  plHybridArray<plString, 4> located;
  ReadLocated(app, located);

  bool bKnown = false;
  for (const plString& sLocated : located)
    bKnown |= SamePath(sLocated, sPath);

  if (!bKnown)
  {
    located.PushBack(sPath);
    WriteLocated(app, located);
  }

  SelectInstall(app, sPath);
  Refresh();

  plLog::Info("USD Live: using {} at '{}'", GetName(app), sPath);
  return plStatus(PL_SUCCESS);
}

plString plUsdApps::GetOpenBlocker(App app, const plUsdLiveLink* pLink) const
{
  plStringBuilder s;

  const Install* pInstall = GetSelectedInstall(app);
  if (pLink == nullptr)
    s = "Link a USD stage first.";
  else if (pInstall == nullptr)
    s.SetFormat("{} was not found. Locate it in the Apps tab.", GetName(app));
  else if (!pInstall->m_sUnsupported.IsEmpty())
    s = pInstall->m_sUnsupported;
  else if (!GetState(app).m_sBusyStep.IsEmpty())
    s.SetFormat("Wait until {} is done {}.", GetName(app), GetState(app).m_sBusyStep);
  else if (pInstall->m_Connector.m_State == ConnectorState::NotInstalled)
    s.SetFormat("Install the connector into {} {} first.", GetName(app), pInstall->m_sVersion);
  else if (pInstall->m_Connector.m_State == ConnectorState::Disabled)
    s = "The connector is switched off in Blender's preferences. Install it again to switch it on.";

  return s;
}

plStatus plUsdApps::DoOpen(App app, plUsdLiveLink* pLink)
{
  RefreshIfOlder(OperationRescanAge);

  GetState(app).m_sProblem.Clear();

  const plString sBlocker = GetOpenBlocker(app, pLink);
  if (!sBlocker.IsEmpty())
    return plStatus(sBlocker.GetView());

  const Install& install = *GetSelectedInstall(app);

  if (!pLink->IsWritable())
  {
    // The tool needs a session to write into; this editor starts it and keeps the strongest layer.
    const plStatus res = plUsdLiveManager::GetSingleton()->StartSession(pLink, DefaultSession, plUsdLiveManager::GetDefaultParticipant());
    if (res.Failed())
      return res;
  }

  const plString& sStage = pLink->GetSettings().m_sStageFile;
  const plString& sSession = pLink->GetSettings().m_sSessionName;

  plProcessOptions options;
  options.m_sProcess = install.m_sExecutable;
  options.m_sWorkingDirectory = NativePath(ParentFolder(sStage));
  options.m_bHideConsoleWindow = false;

  if (app == App::Blender)
  {
    options.m_Arguments.PushBack("--");
    options.m_Arguments.PushBack("--usd-live-stage");
    options.m_Arguments.PushBack(sStage);
    options.m_Arguments.PushBack("--usd-live-session");
    options.m_Arguments.PushBack(sSession);
  }
  else
  {
    options.AddEnvironmentVariable("PLASMA_USD_LIVE_STAGE", sStage);
    options.AddEnvironmentVariable("PLASMA_USD_LIVE_SESSION", sSession);
  }

  // The tool keeps running on its own; the handle opened below follows it.
  plProcess process;
  if (process.Launch(options).Failed())
    return plStatus(plFmt("Could not start '{}'.", install.m_sExecutable));

  const plOsProcessID iProcessId = process.GetProcessID();
  process.Detach();

  Instance& instance = m_Instances.ExpandAndGetRef();
  instance.m_App = app;
  instance.m_sVersion = install.m_sVersion;
  instance.m_uiProcessId = (plUInt32)iProcessId;
  instance.m_sSessionRoot = pLink->GetSessionRoot();
  instance.m_Launched = plTime::Now();
  instance.m_hProcess = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)iProcessId);

  plLog::Info("USD Live: opened '{}' in {} {}, session '{}' (process {})", pLink->GetSettings().m_sStageFile, GetName(app), install.m_sVersion,
    pLink->GetSettings().m_sSessionName, instance.m_uiProcessId);
  return plStatus(PL_SUCCESS);
}
