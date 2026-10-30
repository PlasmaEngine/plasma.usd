#pragma once

#include <EditorPluginUsd/EditorPluginUsdDLL.h>
#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Containers/HashTable.h>
#include <Foundation/Strings/String.h>
#include <Foundation/Time/Time.h>
#include <Foundation/Types/UniquePtr.h>
#include <Foundation/Types/Variant.h>

class plUsdLiveLink;

/// \brief The live hub: other tools in a session send their layer here as they change it, and get everyone else's back,
/// without waiting for anyone to save.
///
/// A TCP server on 127.0.0.1, on a port the system chooses; the editor's presence file (see
/// plUsdLiveManager::GetPresenceFolder) advertises it as "hubPort". Its sockets do not block; Update accepts, reads and
/// writes them on the main thread. One JSON object per line. A tool says hello with its
/// session root and its layer; from then on it sends its whole layer as usda text whenever it changes it ("layer"), and
/// receives the whole layer of every other participant, this editor's included, once on joining and on each change. A
/// participant layer only holds its tool's opinions, so it stays small. Pings every two seconds measure latency.
/// When the session's owner commits it, every tool is told to reload its layers from disk.
///
/// The protocol, version 1:
///   tool -> hub: hello {protocol, tool, participant, layer, sessionRoot, pid}, layer {layer, seq, usda}, saved {layer, seq},
///                ping {id}, pong {id}, bye
/// The hello's pid, the tool's process id, lets the Apps tab tell which participant is a tool it launched; connectors
/// before 0.3.0 leave it out.
///   hub -> tool: welcome {protocol, participant}, layer {layer, seq, usda}, session {participants: [{participant, tool,
///                layer, live, latencyMs}]}, committed, ping {id}, pong {id}
class PL_EDITORPLUGINUSD_DLL plUsdLiveHub
{
  PL_DISALLOW_COPY_AND_ASSIGN(plUsdLiveHub);

public:
  plUsdLiveHub();
  ~plUsdLiveHub();

  /// \brief The port tools connect to; 0 when the server could not start.
  plUInt16 GetPort() const;

  /// \brief Takes new connections and their messages, then sends this editor's changed layers, pings, and the session
  /// lists. Main thread, from the manager's timer.
  void Update();

  /// \brief A participant in a session, as the panel shows it.
  struct Peer
  {
    plString m_sParticipant;
    plString m_sTool;
    plString m_sLayer;
    float m_fLatencyMs = -1.0f; ///< Negative until the first ping came back.
    plUInt32 m_uiProcessId = 0; ///< 0 when the tool did not say.
  };

  /// \brief The tools connected to a session.
  void GetPeers(plStringView sSessionRoot, plDynamicArray<Peer>& out_peers) const;

  /// \brief Tells every tool in a session that it was committed, so they reload their layers.
  void NotifyCommitted(plStringView sSessionRoot);

private:
  struct Connection;

  void StartServer();
  void AcceptConnections();

  /// \brief Reads what arrived and handles the complete lines; false when the connection ended.
  bool ReadConnection(Connection& ref_connection);

  /// \brief Writes what the socket takes of the connection's queue; false when the connection broke.
  bool WriteConnection(Connection& ref_connection);
  void PollConnections();
  void OnDisconnected(Connection* pConnection);

  /// \brief Whether a connection is still open; compares addresses only, so it is safe for one that was removed.
  bool IsConnection(const Connection* pConnection) const;
  void HandleMessage(Connection& ref_connection, const plVariantDictionary& message);
  void Send(Connection& ref_connection, plStringView sLine);
  void SendSession(plStringView sSessionRoot);
  void SendOwnLayer(plUsdLiveLink* pLink, Connection* pOnlyTo);

  plUInt64 m_uiListenSocket = ~0ull; ///< the listening socket; ~0 while there is none
  plUInt16 m_uiPort = 0;
  plDynamicArray<plUniquePtr<Connection>> m_Connections;
  plHashTable<plUsdLiveLink*, plUInt64> m_SentVersions; ///< Link -> the version of its layer the tools last got.
  plHashTable<plUsdLiveLink*, plUInt64> m_SentHashes;   ///< Link -> hash of that layer's text, so saves send nothing new.
  plTime m_NextPing;
  plTime m_NextSession;
  plInt32 m_iNextPingId = 1;
};
