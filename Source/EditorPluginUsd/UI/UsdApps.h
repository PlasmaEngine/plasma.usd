#pragma once

#include <EditorPluginUsd/EditorPluginUsdDLL.h>
#include <EditorUi/Runtime.h>
#include <Foundation/Configuration/Singleton.h>
#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Containers/HashTable.h>
#include <Foundation/Strings/String.h>
#include <Foundation/Time/Time.h>
#include <Foundation/Types/Status.h>
#include <Foundation/Types/UniquePtr.h>

#include <functional>

class plUsdLiveLink;

/// \brief The tools the package has connectors for: where they are installed, whether their connector is, and the
/// instances of them this editor launched into a session. The USD Live panel's Apps tab shows it.
///
/// The connectors ship with the package: the build stages them beside the plugin in connectors/, and a package linked to
/// its source also finds them through PackageSource.txt. Install copies a connector into the tool's own preferences, the
/// way that tool installs add-ons, so it keeps working when the package moves:
///   Blender   an extension in the user_default repository, built and installed with Blender's own command line
///   Houdini   <preferences>/plasma_usd_live, and packages/plasma_usd_live.json pointing at it
///   Maya      <maya app folder>/modules/plasma_usd_live, and modules/plasma_usd_live.mod naming it
///   3ds Max   %APPDATA%/Autodesk/ApplicationPlugins/PlasmaUsdLive, an application package every version reads
/// A connector installed some other way, such as a developer's link to the package source, is reported as linked and left
/// alone.
///
/// Open launches a tool into a link's session: Blender with the stage and session on its command line, Houdini, Maya and
/// 3ds Max with PLASMA_USD_LIVE_STAGE and PLASMA_USD_LIVE_SESSION in their environment. The connectors join on startup. Launched
/// processes are matched to the live hub's participants by the process id they send with their hello.
class PL_EDITORPLUGINUSD_DLL plUsdApps
{
  PL_DECLARE_SINGLETON(plUsdApps);

public:
  plUsdApps();
  ~plUsdApps();

  enum class App : plUInt8
  {
    Blender,
    Houdini,
    Maya,
    Max, ///< 3ds Max
    Count
  };

  /// \brief The session Open starts when a link has none.
  static constexpr const char* DefaultSession = "live";

  static const char* GetName(App app);

  /// \brief Accepts the name case-insensitively and without spaces ("blender", "Houdini", "3dsmax"); "Max" names 3ds Max.
  static bool FindApp(plStringView sName, App& out_app);

  enum class ConnectorState : plUInt8
  {
    Checking,     ///< Blender is being asked in the background.
    Unknown,      ///< Asking failed; m_sNote says why.
    NotInstalled, ///< Not installed in this installation's preferences.
    Disabled,     ///< Installed, but switched off in the tool's preferences.
    Installed,
    Outdated,     ///< Older than the one this package has.
    Linked,       ///< Installed some other way, e.g. linked to the package source. Left alone.
  };

  struct Connector
  {
    ConnectorState m_State = ConnectorState::Checking;
    plString m_sVersion;  ///< The installed connector's version, empty when unknown.
    plString m_sLocation; ///< Where it is installed, as the row says it: "package in C:/.../houdini21.0".
    plString m_sPath;     ///< The folder or file behind that, or where Install puts it.
    plString m_sNote;
  };

  /// \brief One installation of a tool on this computer.
  struct Install
  {
    plString m_sVersion;
    plString m_sExecutable;
    plString m_sUnsupported; ///< Why the connector cannot run in it; empty when it can.
    bool m_bLocated = false; ///< Chosen with Locate rather than found.
    bool m_bRunning = false; ///< A process runs from this installation's folder.
    Connector m_Connector;
  };

  enum class InstanceState : plUInt8
  {
    Starting,     ///< Launched; not on the live hub yet.
    Live,         ///< On the live hub, in the session it was launched into.
    NotConnected, ///< Running for a while without ever joining the hub.
    Left,         ///< Was live, left the session, still running.
  };

  /// \brief A process this editor launched into a session.
  struct Instance
  {
    App m_App = App::Blender;
    plString m_sVersion;
    plUInt32 m_uiProcessId = 0;
    plString m_sSessionRoot;
    plTime m_Launched;
    InstanceState m_State = InstanceState::Starting;
    plString m_sParticipant; ///< Once it joined the hub.
    float m_fLatencyMs = -1.0f;
    void* m_hProcess = nullptr; ///< Kept open, so the process id is not reused while it is tracked.
  };

  /// \brief Looks for installations and their connectors again. Blender's connectors are read by a background Blender,
  /// so they show Checking, or their previous state, until it answers.
  void Refresh();

  /// \brief Refresh(), unless the last one is younger than maxAge.
  void RefreshIfOlder(plTime maxAge);

  /// \brief Follows the launched instances and which installations run. Main thread, from the panel's timer.
  void Update();

  const plDynamicArray<Install>& GetInstalls(App app) const;

  /// \brief The installation Open and Install use: the one picked last, else the newest. nullptr when none is known.
  const Install* GetSelectedInstall(App app) const;
  void SelectInstall(App app, plStringView sExecutable);

  /// \brief The version of the connector this package has, empty when it has none for the app.
  plStringView GetPackagedVersion(App app) const;

  /// \brief The folder the connectors are taken from, empty when the package has none.
  plStringView GetConnectorFolder() const { return m_sConnectorFolder; }

  /// \brief What an install or remove is doing right now; empty when nothing runs.
  plStringView GetBusyStep(App app) const;

  /// \brief The last operation's failure, until the next operation.
  plStringView GetProblem(App app) const;

  /// \brief What the last operation wants the user to know, e.g. that a running tool loads the connector on its next start.
  plStringView GetNotice(App app) const;

  /// \brief Installs or updates the connector in the selected installation. Blender's runs in the background.
  plStatus InstallConnector(App app);
  plStatus RemoveConnector(App app);

  /// \brief Asks for the tool's executable and remembers it.
  plStatus Locate(App app);

  /// \brief Remembers an executable as an installation, as Locate does, and selects it.
  plStatus AddExecutable(App app, plStringView sExecutable);

  /// \brief Why Open is not available for a link, empty when it is.
  plString GetOpenBlocker(App app, const plUsdLiveLink* pLink) const;

  /// \brief Starts a session when the link has none, with this editor's layer strongest, then launches the selected
  /// installation into it.
  plStatus Open(App app, plUsdLiveLink* pLink);

  /// \brief The running instances launched into a session.
  void GetInstances(plStringView sSessionRoot, plDynamicArray<const Instance*>& out_instances) const;

private:
  struct AppState
  {
    plDynamicArray<Install> m_Installs;
    plString m_sSelected; ///< Executable picked last, remembered across runs.
    plString m_sPackagedVersion;
    plString m_sBusyStep;
    plString m_sProblem;
    plString m_sNotice;
  };

  /// \brief What a background Blender reported about itself.
  struct BlenderProbe
  {
    bool m_bRunning = false;
    bool m_bValid = false;
    plString m_sError;
    plString m_sVersion;
    plDynamicArray<plString> m_Addons;

    struct Repository
    {
      plString m_sId;
      plString m_sName;
      plString m_sDirectory;
      bool m_bEnabled = false;
    };

    plDynamicArray<Repository> m_Repositories;
  };

  AppState& GetState(App app) { return m_Apps[(int)app]; }
  const AppState& GetState(App app) const { return m_Apps[(int)app]; }
  Install* GetSelectedInstallMutable(App app);

  /// \brief Records a failure as the app's problem, which its row shows, and logs it.
  plStatus Report(App app, const char* szWhat, plStatus res);
  plStatus DoInstall(App app);
  plStatus DoRemove(App app);
  plStatus DoOpen(App app, plUsdLiveLink* pLink);

  void FindConnectorFolder();
  void ScanInstalls(App app, plDynamicArray<Install>& out_installs) const;
  void EvaluateConnector(App app, Install& ref_install) const;
  void EvaluateBlender(Install& ref_install) const;
  void EvaluateHoudini(Install& ref_install) const;
  void EvaluateMaya(Install& ref_install) const;
  void EvaluateMax(Install& ref_install) const;
  void UpdateRunning();

  /// \brief A tool run in the background; its output is collected until it ends.
  struct BackgroundProcess;
  using ProcessDone = std::function<void(int, plStringView)>;

  /// \brief Runs a command line in the background and calls onDone once, on the main thread, with its exit code (-1 when
  /// it did not start or took too long) and everything it printed.
  void RunProcess(plStringView sProgram, plArrayPtr<const plString> args, ProcessDone onDone);

  /// \brief Calls onDone for the background tools that ended or took too long.
  void PollProcesses();

  /// \brief Asks a background Blender for its extension repositories and enabled add-ons. bForget drops the previous
  /// answer, so the connector reads Checking until the new one arrives.
  void ProbeBlender(const plString& sExecutable, bool bForget);
  void ParseBlenderProbe(const plString& sExecutable, int iExitCode, plStringView sOutput);
  void InstallBlender(const Install& install);
  void RemoveBlender(const Install& install);

  plStatus InstallHoudini(const Install& install);
  plStatus RemoveHoudini(const Install& install);
  plStatus InstallMaya(const Install& install);
  plStatus RemoveMaya(const Install& install);
  plStatus InstallMax(const Install& install);
  plStatus RemoveMax(const Install& install);

  AppState m_Apps[(int)App::Count];
  plHashTable<plString, BlenderProbe> m_BlenderProbes; ///< Executable -> what it reported last.
  plDynamicArray<Instance> m_Instances;
  plString m_sConnectorFolder;
  plTime m_LastRefresh;
  plTime m_NextRunningCheck;
  bool m_bRefreshed = false;
  plDynamicArray<plUniquePtr<BackgroundProcess>> m_Processes; ///< Destroying one ends its tool.
  plEditorUiTimerGroup m_Timers;                              ///< polls m_Processes while any run
  plUInt32 m_uiPollTimer = 0;
};
