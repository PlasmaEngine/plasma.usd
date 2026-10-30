#include <EditorPluginUsd/EditorPluginUsdPCH.h>

#include <EditorPluginUsd/Hub/UsdLiveHub.h>
#include <EditorPluginUsd/LiveLink/UsdLiveManager.h>
#include <EditorPluginUsd/Utils/UsdJson.h>
#include <Foundation/Algorithm/HashingUtils.h>

#include <Foundation/Basics/Platform/Win/IncludeWindows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

namespace
{
  constexpr int ProtocolVersion = 1;
  constexpr plTime PingInterval = plTime::MakeFromSeconds(2);
  constexpr plTime SessionInterval = plTime::MakeFromSeconds(5);

  /// A tool that sends no line break for this long is not speaking the protocol.
  constexpr plUInt32 MaxLineBytes = 256 * 1024 * 1024;

  constexpr plUInt32 ReadChunkBytes = 64 * 1024;

  bool SamePath(plStringView a, plStringView b)
  {
    plStringBuilder sA = a, sB = b;
    sA.MakeCleanPath();
    sB.MakeCleanPath();
    return !sA.IsEmpty() && sA.IsEqual_NoCase(sB);
  }

  plString ToLine(const plVariantDictionary& message)
  {
    plStringBuilder sLine = plUsdJson::Write(message, false);
    sLine.Append("\n");
    return sLine;
  }

  void GetSessionLinks(plStringView sSessionRoot, plHybridArray<plUsdLiveLink*, 2>& out_links)
  {
    out_links.Clear();

    plHybridArray<plUsdLiveLink*, 8> links;
    plUsdLiveManager::GetSingleton()->GetAllLinks(links);
    for (plUsdLiveLink* pLink : links)
    {
      if (SamePath(pLink->GetSessionRoot(), sSessionRoot))
        out_links.PushBack(pLink);
    }
  }

  void SetNonBlocking(SOCKET hSocket)
  {
    u_long uiNonBlocking = 1;
    ioctlsocket(hSocket, FIONBIO, &uiNonBlocking);
  }
} // namespace

struct plUsdLiveHub::Connection
{
  SOCKET m_hSocket = INVALID_SOCKET;
  plDynamicArray<char> m_Buffer;   ///< received bytes not handled yet
  plDynamicArray<char> m_Outgoing; ///< queued bytes the socket did not take yet
  plUInt32 m_uiOutgoingSent = 0;   ///< the part of m_Outgoing already sent
  bool m_bBye = false;             ///< the tool said bye: close once the queue is sent
  bool m_bBroken = false;          ///< a send failed: close on the next poll
  bool m_bHello = false;
  plString m_sParticipant;
  plString m_sTool;
  plString m_sLayer;
  plString m_sSessionRoot;
  plString m_sLastLayerLine; ///< The tool's last layer message, for tools that join later.
  plInt32 m_iPingId = 0;
  plTime m_PingSent;
  float m_fLatencyMs = -1.0f;
  plUInt32 m_uiProcessId = 0;
};

plUsdLiveHub::plUsdLiveHub()
{
  StartServer();
}

plUsdLiveHub::~plUsdLiveHub()
{
  for (const auto& pConnection : m_Connections)
    closesocket(pConnection->m_hSocket);
  m_Connections.Clear();

  if (m_uiListenSocket != ~0ull)
  {
    closesocket(static_cast<SOCKET>(m_uiListenSocket));
    m_uiListenSocket = ~0ull;
    WSACleanup();
  }
}

void plUsdLiveHub::StartServer()
{
  WSADATA wsaData;
  if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
  {
    plLog::Warning("USD Live: the live hub could not start (no sockets); tools sync through saved files only.");
    return;
  }

  const SOCKET hSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;

  int iAddressLength = sizeof(address);
  if (hSocket == INVALID_SOCKET || bind(hSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(hSocket, SOMAXCONN) != 0 ||
      getsockname(hSocket, reinterpret_cast<sockaddr*>(&address), &iAddressLength) != 0)
  {
    plLog::Warning("USD Live: the live hub could not start ({}); tools sync through saved files only.", plArgErrorCode(WSAGetLastError()));

    if (hSocket != INVALID_SOCKET)
      closesocket(hSocket);

    WSACleanup();
    return;
  }

  SetNonBlocking(hSocket);
  m_uiListenSocket = static_cast<plUInt64>(hSocket);
  m_uiPort = ntohs(address.sin_port);

  plLog::Dev("USD Live: live hub on port {}", m_uiPort);
}

plUInt16 plUsdLiveHub::GetPort() const
{
  return m_uiPort;
}

void plUsdLiveHub::AcceptConnections()
{
  if (m_uiListenSocket == ~0ull)
    return;

  while (true)
  {
    const SOCKET hSocket = accept(static_cast<SOCKET>(m_uiListenSocket), nullptr, nullptr);
    if (hSocket == INVALID_SOCKET)
      return;

    SetNonBlocking(hSocket);

    BOOL bNoDelay = TRUE;
    setsockopt(hSocket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&bNoDelay), sizeof(bNoDelay));

    plUniquePtr<Connection> pConnection = PL_DEFAULT_NEW(Connection);
    pConnection->m_hSocket = hSocket;
    m_Connections.PushBack(std::move(pConnection));
  }
}

bool plUsdLiveHub::ReadConnection(Connection& ref_connection)
{
  plDynamicArray<char>& buffer = ref_connection.m_Buffer;

  while (true)
  {
    const plUInt32 uiOld = buffer.GetCount();
    buffer.SetCountUninitialized(uiOld + ReadChunkBytes);

    const int iRead = recv(ref_connection.m_hSocket, buffer.GetData() + uiOld, static_cast<int>(ReadChunkBytes), 0);
    buffer.SetCountUninitialized(uiOld + plMath::Max(iRead, 0));

    if (iRead == 0)
      return false;

    if (iRead < 0)
    {
      if (WSAGetLastError() != WSAEWOULDBLOCK)
        return false;
      break;
    }
  }

  // Complete lines are messages; the rest waits for more.
  plUInt32 uiStart = 0;
  for (plUInt32 i = 0; i < buffer.GetCount(); ++i)
  {
    if (buffer[i] != '\n')
      continue;

    const plStringView sLine(buffer.GetData() + uiStart, i - uiStart);
    uiStart = i + 1;

    plVariantDictionary message;
    if (plUsdJson::Parse(sLine, message))
    {
      if (plUsdJson::GetString(message, "type") == "layer")
      {
        // Kept as it came, for tools that join later.
        plStringBuilder sRaw = sLine;
        sRaw.Append("\n");
        ref_connection.m_sLastLayerLine = sRaw;
      }

      HandleMessage(ref_connection, message);
    }
  }

  buffer.RemoveAtAndCopy(0, uiStart);
  return buffer.GetCount() <= MaxLineBytes;
}

bool plUsdLiveHub::WriteConnection(Connection& ref_connection)
{
  plDynamicArray<char>& outgoing = ref_connection.m_Outgoing;

  while (ref_connection.m_uiOutgoingSent < outgoing.GetCount())
  {
    const plUInt32 uiLeft = outgoing.GetCount() - ref_connection.m_uiOutgoingSent;
    const int iSent = send(ref_connection.m_hSocket, outgoing.GetData() + ref_connection.m_uiOutgoingSent, static_cast<int>(plMath::Min(uiLeft, 1024u * 1024u)), 0);

    if (iSent < 0)
      return WSAGetLastError() == WSAEWOULDBLOCK;

    ref_connection.m_uiOutgoingSent += static_cast<plUInt32>(iSent);
  }

  outgoing.Clear();
  ref_connection.m_uiOutgoingSent = 0;
  return true;
}

void plUsdLiveHub::PollConnections()
{
  AcceptConnections();

  // Handling a message can make a connection go, so each one is looked up again by address.
  plHybridArray<Connection*, 8> connections;
  for (const auto& pConnection : m_Connections)
    connections.PushBack(pConnection.Borrow());

  for (Connection* pConnection : connections)
  {
    if (!IsConnection(pConnection))
      continue;

    const bool bOpen = !pConnection->m_bBroken && ReadConnection(*pConnection) && WriteConnection(*pConnection);
    if (!bOpen || (pConnection->m_bBye && pConnection->m_Outgoing.IsEmpty()))
      OnDisconnected(pConnection);
  }
}

bool plUsdLiveHub::IsConnection(const Connection* pConnection) const
{
  for (const auto& pOther : m_Connections)
  {
    if (pOther.Borrow() == pConnection)
      return true;
  }

  return false;
}

void plUsdLiveHub::OnDisconnected(Connection* pConnection)
{
  for (plUInt32 i = 0; i < m_Connections.GetCount(); ++i)
  {
    if (m_Connections[i].Borrow() != pConnection)
      continue;

    const plString sSessionRoot = pConnection->m_sSessionRoot;
    const plString sLayer = pConnection->m_sLayer;

    if (pConnection->m_bHello)
      plLog::Info("USD Live: {} left the live hub", pConnection->m_sParticipant);

    closesocket(pConnection->m_hSocket);
    m_Connections.RemoveAtAndCopy(i);

    // Its layer's file is watched again: whatever it saves last is what counts now.
    plHybridArray<plUsdLiveLink*, 2> links;
    GetSessionLinks(sSessionRoot, links);
    for (plUsdLiveLink* pLink : links)
      pLink->ReleaseRemoteLayer(sLayer);

    if (!sSessionRoot.IsEmpty())
      SendSession(sSessionRoot);
    return;
  }
}

void plUsdLiveHub::Send(Connection& ref_connection, plStringView sLine)
{
  if (ref_connection.m_bBroken || ref_connection.m_bBye)
    return;

  ref_connection.m_Outgoing.PushBackRange(plArrayPtr<const char>(sLine.GetStartPointer(), sLine.GetElementCount()));

  if (!WriteConnection(ref_connection))
    ref_connection.m_bBroken = true;
}

void plUsdLiveHub::HandleMessage(Connection& ref_connection, const plVariantDictionary& message)
{
  const plString sType = plUsdJson::GetString(message, "type");

  if (sType == "ping")
  {
    plVariantDictionary pong;
    pong.Insert("type", plString("pong"));

    const plVariant* pId = message.GetValue("id");
    pong.Insert("id", pId != nullptr ? *pId : plVariant());
    Send(ref_connection, ToLine(pong));
    return;
  }

  if (sType == "pong")
  {
    if (static_cast<plInt32>(plUsdJson::GetNumber(message, "id")) == ref_connection.m_iPingId)
      ref_connection.m_fLatencyMs = (float)((plTime::Now() - ref_connection.m_PingSent).GetMilliseconds() * 0.5);
    return;
  }

  if (sType == "bye")
  {
    // Closed by the next poll, once what is queued for the tool is sent.
    ref_connection.m_bBye = true;
    return;
  }

  if (sType == "hello")
  {
    ref_connection.m_bHello = true;
    ref_connection.m_sParticipant = plUsdJson::GetString(message, "participant");
    ref_connection.m_sTool = plUsdJson::GetString(message, "tool");
    ref_connection.m_uiProcessId = static_cast<plUInt32>(plUsdJson::GetNumber(message, "pid"));

    plStringBuilder sLayer = plUsdJson::GetString(message, "layer");
    sLayer.MakeCleanPath();
    ref_connection.m_sLayer = sLayer;

    plStringBuilder sSessionRoot = plUsdJson::GetString(message, "sessionRoot");
    sSessionRoot.MakeCleanPath();
    ref_connection.m_sSessionRoot = sSessionRoot;

    plHybridArray<plUsdLiveLink*, 2> links;
    GetSessionLinks(sSessionRoot, links);

    plVariantDictionary welcome;
    welcome.Insert("type", plString("welcome"));
    welcome.Insert("protocol", ProtocolVersion);
    welcome.Insert("participant", links.IsEmpty() ? plString() : links[0]->GetSettings().m_sParticipant);
    Send(ref_connection, ToLine(welcome));

    // Everything the newcomer should see: this editor's layer, and the layers of the tools already here.
    for (plUsdLiveLink* pLink : links)
      SendOwnLayer(pLink, &ref_connection);

    for (const auto& pOther : m_Connections)
    {
      if (pOther.Borrow() != &ref_connection && pOther->m_bHello && SamePath(pOther->m_sSessionRoot, sSessionRoot) && !pOther->m_sLastLayerLine.IsEmpty())
        Send(ref_connection, pOther->m_sLastLayerLine);
    }

    plLog::Info("USD Live: {} ({}) joined the live hub", ref_connection.m_sParticipant, ref_connection.m_sTool);
    SendSession(sSessionRoot);
    return;
  }

  if (!ref_connection.m_bHello)
    return;

  if (sType == "layer")
  {
    const plString sLayer = plUsdJson::GetString(message, "layer");
    const plString sUsda = plUsdJson::GetString(message, "usda");

    plHybridArray<plUsdLiveLink*, 2> links;
    GetSessionLinks(ref_connection.m_sSessionRoot, links);
    for (plUsdLiveLink* pLink : links)
    {
      const plStatus res = pLink->ApplyRemoteLayer(sLayer, sUsda);
      if (res.Failed())
        plLog::Warning("USD Live: {}", res.m_sMessage);
    }

    for (const auto& pOther : m_Connections)
    {
      if (pOther.Borrow() != &ref_connection && pOther->m_bHello && SamePath(pOther->m_sSessionRoot, ref_connection.m_sSessionRoot))
        Send(*pOther, ref_connection.m_sLastLayerLine);
    }
    return;
  }
}

void plUsdLiveHub::SendOwnLayer(plUsdLiveLink* pLink, Connection* pOnlyTo)
{
  const plString sText = pLink->ExportOwnLayer();

  plVariantDictionary message;
  message.Insert("type", plString("layer"));
  message.Insert("layer", plString(pLink->GetParticipantLayer()));
  message.Insert("seq", static_cast<plInt64>(pLink->GetOwnLayerVersion()));
  message.Insert("usda", sText);
  const plString sLine = ToLine(message);

  if (pOnlyTo != nullptr)
  {
    Send(*pOnlyTo, sLine);
    return;
  }

  for (const auto& pConnection : m_Connections)
  {
    if (pConnection->m_bHello && SamePath(pConnection->m_sSessionRoot, pLink->GetSessionRoot()))
      Send(*pConnection, sLine);
  }

  m_SentHashes[pLink] = plHashingUtils::xxHash64String(sText);
}

void plUsdLiveHub::SendSession(plStringView sSessionRoot)
{
  plVariantArray participants;

  plHybridArray<plUsdLiveLink*, 2> links;
  GetSessionLinks(sSessionRoot, links);
  for (plUsdLiveLink* pLink : links)
  {
    plVariantDictionary participant;
    participant.Insert("participant", pLink->GetSettings().m_sParticipant);
    participant.Insert("tool", plString("Plasma Editor"));
    participant.Insert("layer", plString(pLink->GetParticipantLayer()));
    participant.Insert("live", true);
    participant.Insert("latencyMs", 0);
    participants.PushBack(participant);
  }

  for (const auto& pConnection : m_Connections)
  {
    if (!pConnection->m_bHello || !SamePath(pConnection->m_sSessionRoot, sSessionRoot))
      continue;

    plVariantDictionary participant;
    participant.Insert("participant", pConnection->m_sParticipant);
    participant.Insert("tool", pConnection->m_sTool);
    participant.Insert("layer", pConnection->m_sLayer);
    participant.Insert("live", true);
    participant.Insert("latencyMs", pConnection->m_fLatencyMs >= 0.0f ? plVariant(pConnection->m_fLatencyMs) : plVariant());
    participants.PushBack(participant);
  }

  plVariantDictionary message;
  message.Insert("type", plString("session"));
  message.Insert("participants", participants);
  const plString sLine = ToLine(message);

  for (const auto& pConnection : m_Connections)
  {
    if (pConnection->m_bHello && SamePath(pConnection->m_sSessionRoot, sSessionRoot))
      Send(*pConnection, sLine);
  }

  plUsdLiveManager::GetSingleton()->BroadcastPeersChanged();
}

void plUsdLiveHub::Update()
{
  PollConnections();

  if (m_Connections.IsEmpty())
    return;

  plHybridArray<plUsdLiveLink*, 8> links;
  plUsdLiveManager::GetSingleton()->GetAllLinks(links);

  // This editor's layers, as they change; at most once per tick of the manager's timer, so a drag is a stream of updates.
  plHashTable<plUsdLiveLink*, plUInt64> versions;
  for (plUsdLiveLink* pLink : links)
  {
    if (pLink->GetSessionRoot().IsEmpty())
      continue;

    const plUInt64 uiVersion = pLink->GetOwnLayerVersion();
    versions[pLink] = uiVersion;

    plUInt64 uiSent = 0;
    if (m_SentVersions.TryGetValue(pLink, uiSent) && uiSent == uiVersion)
      continue;

    // A save bumps the version without changing the text.
    plUInt64 uiSentHash = 0;
    if (m_SentHashes.TryGetValue(pLink, uiSentHash) && uiSentHash == plHashingUtils::xxHash64String(pLink->ExportOwnLayer()))
      continue;

    SendOwnLayer(pLink, nullptr);
  }

  m_SentVersions = versions;

  const plTime now = plTime::Now();

  if (now >= m_NextPing)
  {
    m_NextPing = now + PingInterval;

    for (const auto& pConnection : m_Connections)
    {
      if (!pConnection->m_bHello)
        continue;

      pConnection->m_iPingId = m_iNextPingId++;
      pConnection->m_PingSent = now;

      plVariantDictionary ping;
      ping.Insert("type", plString("ping"));
      ping.Insert("id", pConnection->m_iPingId);
      Send(*pConnection, ToLine(ping));
    }
  }

  if (now >= m_NextSession)
  {
    m_NextSession = now + SessionInterval;

    plHybridArray<plString, 4> roots;
    for (const auto& pConnection : m_Connections)
    {
      if (pConnection->m_bHello && !roots.Contains(pConnection->m_sSessionRoot))
        roots.PushBack(pConnection->m_sSessionRoot);
    }

    for (const plString& sRoot : roots)
      SendSession(sRoot);
  }
}

void plUsdLiveHub::GetPeers(plStringView sSessionRoot, plDynamicArray<Peer>& out_peers) const
{
  out_peers.Clear();

  for (const auto& pConnection : m_Connections)
  {
    if (!pConnection->m_bHello || !SamePath(pConnection->m_sSessionRoot, sSessionRoot))
      continue;

    Peer& peer = out_peers.ExpandAndGetRef();
    peer.m_sParticipant = pConnection->m_sParticipant;
    peer.m_sTool = pConnection->m_sTool;
    peer.m_sLayer = pConnection->m_sLayer;
    peer.m_fLatencyMs = pConnection->m_fLatencyMs;
    peer.m_uiProcessId = pConnection->m_uiProcessId;
  }
}

void plUsdLiveHub::NotifyCommitted(plStringView sSessionRoot)
{
  plVariantDictionary message;
  message.Insert("type", plString("committed"));
  const plString sLine = ToLine(message);

  for (const auto& pConnection : m_Connections)
  {
    if (pConnection->m_bHello && SamePath(pConnection->m_sSessionRoot, sSessionRoot))
    {
      // Their layers were emptied on disk; what they sent before no longer holds.
      pConnection->m_sLastLayerLine.Clear();
      Send(*pConnection, sLine);
    }
  }
}
