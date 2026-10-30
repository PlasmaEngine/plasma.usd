"""The connection to the Plasma editor's live hub.

While the Plasma editor is in a session it runs a hub on 127.0.0.1 that relays every participant's layer, so edits
reach the other tools within a frame or two instead of waiting for files to be saved. Its presence file for the session
names the port (``hubPort``). The connection is polled from the live link's tick and never blocks Maya: the socket
is non-blocking and partial lines are buffered. Without a hub, or after the connection drops, the link syncs through
the session files alone and a new connection is tried every few seconds.

This is the Houdini connector's hub.py, unchanged but for this paragraph: its clock is time.perf_counter, because
time.monotonic advances in ~16 ms steps on Windows, too coarse for the round trip times and the 50 ms layer interval.
Standard library only.

Protocol version 1: one JSON object per line, UTF-8, '\\n' terminated; unknown message types and fields are ignored.
  tool -> hub: hello (with this process's id, which tells the Plasma editor a tool it launched has joined), layer (this
               tool's whole layer after it changes, at most ~20 per second, always the final state), saved (after the
               layer file is saved), ping, pong (answers every hub ping at once), bye.
  hub -> tool: welcome, layer (another participant's whole layer: once per participant after welcome, then on every
               change), session (participants with live state and latency), committed (the owner folded all layers
               into the asset and cleared them), ping (every ~2 s).
"""

import errno
import json
import os
import select
import socket
import time

from . import sessions

PROTOCOL = 1
HOST = "127.0.0.1"

# Monotonic and sub-millisecond on every platform; the live link uses it too, so its times compare with these.
clock = time.perf_counter

RETRY_SECONDS = 3.0
CONNECT_TIMEOUT_SECONDS = 3.0
WELCOME_TIMEOUT_SECONDS = 5.0
PING_SECONDS = 2.0
SILENCE_SECONDS = 8.0  # the hub pings every ~2 s, so this long without a byte means it is gone
LAYER_INTERVAL_SECONDS = 0.05  # at most ~20 layer messages per second
QUICK_POLL_SECONDS = 0.005  # while a ping waits for its pong, so the round trip is not rounded up to the tick
QUICK_POLL_WINDOW_SECONDS = 0.1
BACKLOG_BYTES = 1 << 20  # while this much is still unsent, newer layer states wait and replace each other
MAX_BUFFER_BYTES = 256 << 20
RECV_BYTES = 1 << 20

# Waiting for a non-blocking connect; Windows reports WSAEWOULDBLOCK.
CONNECT_PENDING = {0, errno.EINPROGRESS, errno.EWOULDBLOCK, errno.EALREADY, 10035}

OFF = "off"
CONNECTING = "connecting"
HANDSHAKE = "handshake"
LIVE = "live"


class HubClient:
    """One live link's connection to the hub of its session."""

    def __init__(self, link):
        self.link = link
        self.sock = None
        self.state = OFF
        self.port = None
        self.inbox = bytearray()
        self.outbox = bytearray()
        self.deadline = 0.0
        self.next_attempt = 0.0
        self.last_heard = 0.0
        self.next_ping = 0.0
        self.ping_id = 0
        self.pings = {}
        self.quick_until = 0.0
        self.latency_ms = None  # round trip of this tool's last ping
        self.hub_participant = None
        self.participants = []  # the last session message: dicts with participant, tool, layer, live, latencyMs
        self.driven = set()  # path_key of every layer the hub has sent during this connection
        self.not_live = set()  # path_key of the layers of participants the hub reports as not live
        self.seq = 0
        self.layer_pending = False
        self.last_layer_sent = 0.0

    @property
    def live(self):
        return self.state == LIVE

    def poll_interval(self, interval):
        """The tick interval to use next: shorter while a ping waits for its pong."""
        return QUICK_POLL_SECONDS if clock() < self.quick_until else interval

    def is_driven(self, path):
        """True while the hub, not the file on disk, decides the content of this layer."""
        if self.state != LIVE:
            return False
        key = sessions.path_key(path)
        return key in self.driven and key not in self.not_live

    # -- connection ------------------------------------------------------------------------------------------

    def poll(self, now=None):
        """Connects, reads and dispatches what arrived, pings and sends what is due. Never waits."""
        now = clock() if now is None else now

        if self.state == OFF:
            if now >= self.next_attempt:
                self.connect(now)
            if self.state == OFF:
                return

        if self.state == CONNECTING:
            self.finish_connect(now)
            if self.state == CONNECTING or self.sock is None:
                return

        self.receive(now)
        if self.sock is None:
            return

        if self.state == HANDSHAKE and now > self.deadline:
            self.drop("no welcome from the hub")
            return

        if self.state == LIVE:
            if now - self.last_heard > SILENCE_SECONDS:
                self.drop("the hub stopped answering")
                return
            if now >= self.next_ping:
                self.next_ping = now + PING_SECONDS
                self.ping_id += 1
                self.pings[self.ping_id] = now
                for old in [i for i in self.pings if i <= self.ping_id - 8]:
                    del self.pings[old]
                self.send({"type": "ping", "id": self.ping_id})
                self.quick_until = now + QUICK_POLL_WINDOW_SECONDS
            self.send_layer_if_due(now)

        self.flush()

    def connect(self, now):
        self.next_attempt = now + RETRY_SECONDS
        self.port = sessions.find_hub_port(self.link.session_root)
        if self.port is None:
            return

        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.setblocking(False)
        try:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        except OSError:
            pass
        err = sock.connect_ex((HOST, self.port))
        if err not in CONNECT_PENDING:
            sock.close()
            return

        self.sock = sock
        self.state = CONNECTING
        self.deadline = now + CONNECT_TIMEOUT_SECONDS
        if err == 0:
            self.on_connected(now)

    def finish_connect(self, now):
        try:
            _, writable, failed = select.select([], [self.sock], [self.sock], 0)
        except (OSError, ValueError):
            self.drop(None)
            return
        if failed or writable:
            if self.sock.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR):
                self.drop(None)  # nothing listens on the port (yet); try again later
            else:
                self.on_connected(now)
        elif now > self.deadline:
            self.drop(None)

    def on_connected(self, now):
        self.state = HANDSHAKE
        self.deadline = now + WELCOME_TIMEOUT_SECONDS
        self.last_heard = now
        self.inbox.clear()
        self.outbox.clear()
        self.send({
            "type": "hello",
            "protocol": PROTOCOL,
            "tool": self.link.tool,
            "participant": self.link.participant,
            "layer": self.link.layer_path,
            "sessionRoot": self.link.session_root,
            "pid": os.getpid(),
        })
        self.flush()

    def drop(self, reason):
        """Closes the connection; the link falls back to the session files and a new connection is tried later."""
        was_live = self.state == LIVE
        driven = self.driven
        self.close_socket()
        self.state = OFF
        self.next_attempt = clock() + RETRY_SECONDS
        self.driven = set()
        self.not_live = set()
        self.participants = []
        self.pings.clear()
        self.quick_until = 0.0
        self.latency_ms = None
        self.layer_pending = False
        if was_live:
            self.link.on_hub_lost(reason or "connection closed", driven)

    def close(self):
        """Says goodbye and disconnects, when the link leaves the session. Best effort, without waiting and without
        calling back into the link."""
        sock = self.sock
        if sock is not None and self.state in (HANDSHAKE, LIVE):
            self.outbox += b'{"type":"bye"}\n'
            try:
                while self.outbox:
                    del self.outbox[:sock.send(self.outbox)]
                sock.shutdown(socket.SHUT_WR)
                while sock.recv(RECV_BYTES):  # unread input would turn the close into a reset
                    pass
            except OSError:  # includes a full send buffer
                pass
        self.close_socket()
        self.state = OFF
        self.driven = set()
        self.not_live = set()
        self.participants = []
        self.layer_pending = False

    def close_socket(self):
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
        self.sock = None
        self.inbox.clear()
        self.outbox.clear()

    # -- sending ---------------------------------------------------------------------------------------------

    def send(self, message):
        if self.sock is None:
            return
        self.outbox += (json.dumps(message, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")
        if len(self.outbox) > MAX_BUFFER_BYTES:
            self.drop("the hub does not read")

    def flush(self):
        while self.outbox and self.sock is not None:
            try:
                sent = self.sock.send(self.outbox)
            except (BlockingIOError, InterruptedError):
                return
            except OSError as e:
                self.drop("connection lost: {}".format(e.strerror or e))
                return
            del self.outbox[:sent]

    def layer_changed(self):
        """This tool's layer changed; the whole layer goes to the hub when the rate allows."""
        if self.state == LIVE:
            self.layer_pending = True

    def send_layer_if_due(self, now=None, force=False):
        if not self.layer_pending or self.state != LIVE:
            return
        now = clock() if now is None else now
        if not force and (now - self.last_layer_sent < LAYER_INTERVAL_SECONDS or len(self.outbox) > BACKLOG_BYTES):
            return
        self.layer_pending = False
        self.last_layer_sent = now
        self.seq += 1
        self.send({"type": "layer", "layer": self.link.layer_path, "seq": self.seq,
                   "usda": self.link.layer.ExportToString()})
        self.flush()

    def layer_saved(self):
        """The layer file was saved. The link saves only once the layer message for that state went out, so the
        notice names it."""
        if self.state != LIVE:
            return
        self.send({"type": "saved", "layer": self.link.layer_path, "seq": self.seq})
        self.flush()

    # -- receiving -------------------------------------------------------------------------------------------

    def receive(self, now):
        closed = None
        while True:
            try:
                data = self.sock.recv(RECV_BYTES)
            except (BlockingIOError, InterruptedError):
                break
            except OSError as e:
                closed = "connection lost: {}".format(e.strerror or e)
                break
            if not data:
                closed = "the hub closed the connection"
                break
            self.inbox += data
            self.last_heard = now
            if len(self.inbox) > MAX_BUFFER_BYTES:
                closed = "message too large"
                break

        messages = []
        while True:
            end = self.inbox.find(b"\n")
            if end < 0:
                break
            line = bytes(self.inbox[:end])
            del self.inbox[:end + 1]
            try:
                message = json.loads(line.decode("utf-8"))
            except ValueError:  # includes UnicodeDecodeError
                continue
            if isinstance(message, dict):
                messages.append(message)

        # What arrived before the connection closed still counts.
        if messages:
            self.dispatch(messages)
        if closed is not None and self.sock is not None:
            self.drop(closed)

    def dispatch(self, messages):
        # Layer messages are applied together, the latest per layer, before anything that depends on their order.
        layers = {}

        def apply_layers():
            if layers:
                batch = list(layers.values())
                layers.clear()
                self.link.apply_hub_layers(batch)

        for message in messages:
            if self.sock is None:
                break
            kind = message.get("type")

            if kind == "ping":
                self.send({"type": "pong", "id": message.get("id")})
                self.flush()

            elif kind == "layer":
                path, usda = message.get("layer"), message.get("usda")
                if isinstance(path, str) and isinstance(usda, str):
                    key = sessions.path_key(path)
                    self.driven.add(key)
                    layers.pop(key, None)
                    layers[key] = message

            elif kind == "welcome":
                if self.state == HANDSHAKE:
                    self.state = LIVE
                    self.hub_participant = message.get("participant")
                    self.next_ping = clock()
                    self.link.on_hub_connected(message.get("protocol"))
                    self.layer_pending = True
                    self.send_layer_if_due(force=True)

            elif kind == "session":
                participants = message.get("participants")
                if isinstance(participants, list):
                    self.participants = [p for p in participants if isinstance(p, dict)]
                    self.not_live = {sessions.path_key(p["layer"]) for p in self.participants
                                     if isinstance(p.get("layer"), str) and p.get("live") is False}
                    self.link.redraw()

            elif kind == "pong":
                sent = self.pings.pop(message.get("id"), None)
                if sent is not None:
                    self.latency_ms = (clock() - sent) * 1000.0
                    self.quick_until = 0.0
                    self.link.redraw()

            elif kind == "committed":
                apply_layers()
                self.link.on_committed()

        apply_layers()
