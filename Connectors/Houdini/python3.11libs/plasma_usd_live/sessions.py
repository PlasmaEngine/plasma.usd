"""Finding USD live sessions and the Plasma editor's live hub.

A session is a folder beside the asset, ``<asset dir>/.live/<asset stem>/<session>/``, holding ``session.usda`` and one
layer per participant. Tools in a session also keep a presence file in ``%APPDATA%/PlasmaUsdLive/Presence`` and rewrite
it every few seconds; the Plasma editor does the same. The open sessions are the ones with a fresh presence file.
The Plasma editor's presence file also carries ``hubPort``, the local TCP port of its live hub for that session.

This is the Blender connector's sessions.py; the only difference is the participant name, ``houdini-<user>``.
"""

import json
import os
import time
import zlib

PRESENCE_INTERVAL_SECONDS = 5.0
PRESENCE_STALE_SECONDS = 20.0

# Presence files this old are deleted by whoever lists the folder.
PRESENCE_CLEANUP_SECONDS = 3600.0

PARTICIPANT_PREFIX = "houdini-"


class Participant:
    __slots__ = ("name", "tool", "pid")

    def __init__(self, name, tool, pid):
        self.name = name
        self.tool = tool
        self.pid = pid


class OpenSession:
    __slots__ = ("session_root", "stage", "name", "participants")

    def __init__(self, session_root, stage, name):
        self.session_root = session_root
        self.stage = stage
        self.name = name
        self.participants = []


def presence_folder():
    base = os.environ.get("APPDATA") or os.path.expanduser("~")
    return os.path.join(base, "PlasmaUsdLive", "Presence")


def path_key(path):
    """A path normalised for comparing: separators, '..' and case do not matter."""
    return os.path.normcase(os.path.normpath(path)).replace("\\", "/").lower()


def same_path(a, b):
    return path_key(a) == path_key(b)


def session_folder(stage, session):
    stage = os.path.abspath(stage)
    stem = os.path.splitext(os.path.basename(stage))[0]
    return os.path.join(os.path.dirname(stage), ".live", stem, session).replace("\\", "/")


def list_open_sessions():
    """Sessions some tool is connected to right now, sorted by stage and name."""
    folder = presence_folder()
    try:
        names = os.listdir(folder)
    except OSError:
        return []

    now = time.time()
    sessions = {}

    for name in names:
        if not name.endswith(".json"):
            continue
        path = os.path.join(folder, name)
        try:
            age = now - os.path.getmtime(path)
            if age > PRESENCE_CLEANUP_SECONDS:
                os.remove(path)
                continue
            if age > PRESENCE_STALE_SECONDS:
                continue
            with open(path, encoding="utf-8") as f:
                data = json.load(f)
        except (OSError, ValueError):
            continue

        root = data.get("sessionRoot")
        stage = data.get("stage")
        if not root or not stage or not os.path.exists(root):
            continue

        key = os.path.normcase(os.path.normpath(root))
        session = sessions.get(key)
        if session is None:
            session_name = data.get("session") or os.path.basename(os.path.dirname(root))
            session = sessions[key] = OpenSession(root, stage, session_name)
        session.participants.append(Participant(data.get("participant", "?"), data.get("tool", ""), data.get("pid", 0)))

    return sorted(sessions.values(), key=lambda s: (os.path.basename(s.stage).lower(), s.name.lower()))


def find_hub_port(session_root):
    """The port of the live hub for a session, from the freshest presence file of that session with a ``hubPort``;
    None when no running Plasma editor offers one."""
    folder = presence_folder()
    try:
        names = os.listdir(folder)
    except OSError:
        return None

    now = time.time()
    best = None

    for name in names:
        if not name.endswith(".json"):
            continue
        path = os.path.join(folder, name)
        try:
            mtime = os.path.getmtime(path)
            if now - mtime > PRESENCE_STALE_SECONDS:
                continue
            with open(path, encoding="utf-8") as f:
                data = json.load(f)
        except (OSError, ValueError):
            continue
        if not isinstance(data, dict):
            continue

        port = data.get("hubPort")
        root = data.get("sessionRoot")
        if isinstance(port, bool) or not isinstance(port, int) or not 0 < port < 65536:
            continue
        if not isinstance(root, str) or not same_path(root, session_root):
            continue
        if best is None or mtime > best[0]:
            best = (mtime, port)

    return best[1] if best is not None else None


def list_sessions_beside(stage):
    """(name, participant layer names) of every session folder beside a stage, open or not."""
    live = os.path.dirname(session_folder(stage, "x"))
    try:
        names = sorted(os.listdir(live), key=str.lower)
    except OSError:
        return []

    result = []
    for name in names:
        root = os.path.join(live, name, "session.usda")
        if not os.path.isfile(root):
            continue
        participants = [os.path.splitext(f)[0] for f in os.listdir(os.path.join(live, name))
                        if f.endswith(".usda") and f != "session.usda"]
        result.append((name, sorted(participants)))
    return result


def choose_participant(session_root, user):
    """'houdini-<user>', numbered when another running Houdini already uses that name in this session."""
    taken = set()
    for session in list_open_sessions():
        if same_path(session.session_root, session_root):
            taken = {p.name for p in session.participants if p.pid != os.getpid()}

    base = PARTICIPANT_PREFIX + user
    name = base
    index = 2
    while name in taken:
        name = "{}-{}".format(base, index)
        index += 1
    return name


class PresenceWriter:
    """This Houdini's presence file for one session."""

    def __init__(self, info):
        self.info = info
        crc = zlib.crc32(info["sessionRoot"].encode("utf-8"))
        self.path = os.path.join(presence_folder(), "{}-{}-{}.json".format(info["participant"], os.getpid(), crc))
        self.next_refresh = 0.0

    def refresh(self, force=False):
        now = time.monotonic()
        if not force and now < self.next_refresh:
            return
        self.next_refresh = now + PRESENCE_INTERVAL_SECONDS
        try:
            os.makedirs(os.path.dirname(self.path), exist_ok=True)
            temp = self.path + ".tmp"
            with open(temp, "w", encoding="utf-8") as f:
                json.dump(self.info, f, indent=2)
            os.replace(temp, self.path)
        except OSError:
            pass

    def remove(self):
        try:
            os.remove(self.path)
        except OSError:
            pass
