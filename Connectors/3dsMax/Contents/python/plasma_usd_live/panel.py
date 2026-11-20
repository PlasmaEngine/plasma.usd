"""The "Plasma USD Live" panel, a dock widget in 3ds Max's main window.

Not in a session, it lists the open sessions (from the presence files) with a Join button each, marks the one this scene
was linked to, and starts or joins a session on any USD file. In a session it shows the stage and session, this 3ds
Max's layer, how edits travel ("Live via Plasma hub" with the round trip time, or "File sync"), how many nodes are
linked, the participants (from the hub with their live state and latency while connected, otherwise from the session
files, strongest first), the recent sync events and Leave. When USD for 3ds Max cannot be used, the panel says why and
joining is disabled.

The panel only shows the link's state and starts or ends it; the link runs from its own timer, so closing the panel does
not leave the session. It refreshes every half second, and the session list every two seconds.
"""

import os
import re
import traceback

from pymxs import runtime as rt

import plasma_usd_live
from . import hub, sessions
from .qt import QtCore, QtWidgets, exec_dialog, is_valid, main_window

TITLE = "Plasma USD Live"
OBJECT_NAME = "plasmaUsdLivePanel"
REFRESH_MS = 500
SESSIONS_EVERY = 4  # refreshes between two reads of the presence folder

_panel = None


def open_panel():
    """Shows the panel, docked on the right of 3ds Max's main window the first time."""
    global _panel
    if not is_valid(_panel):
        _panel = None
    if _panel is None:
        parent = main_window()
        _panel = Panel(parent)
        if parent is not None and hasattr(parent, "addDockWidget"):
            parent.addDockWidget(QtCore.Qt.RightDockWidgetArea, _panel)
        else:
            _panel.setFloating(True)
    _panel.show()
    _panel.raise_()
    _panel.refresh(force=True)
    return _panel


def format_latency(ms):
    return "{:.0f} ms".format(ms) if isinstance(ms, (int, float)) and not isinstance(ms, bool) else None


def hub_status(link):
    """The line saying how edits travel."""
    connection = link.hub
    if connection.live:
        latency = format_latency(connection.latency_ms)
        return "Live via Plasma hub" + (", " + latency if latency else "")
    if connection.state != hub.OFF:
        return "File sync, connecting to the Plasma hub"
    return "File sync"


def participant_lines(link):
    """(group title, lines) for the participants box."""
    lines = []
    if link.hub.live and link.hub.participants:
        for p in link.hub.participants:
            name = str(p.get("participant", "?"))
            text = name + ("  (you)" if name == link.participant else "")
            if p.get("live") is True:
                latency = format_latency(p.get("latencyMs"))
                text += " - live" + (", " + latency if latency else "")
            else:
                text += " - not live"
            lines.append(text)
        return "Participants (Plasma hub)", lines
    for name, online in link.participants():
        lines.append(name + ("  (you)" if name == link.participant else "") + ("" if online else " - away"))
    return "Participants, strongest first", lines


def usd_problem():
    """Why USD for 3ds Max cannot be used, or None."""
    try:
        from . import max_host
        max_host.ensure_usd()
        return None
    except Exception as e:
        return str(e)


def error(text):
    print("[Plasma USD Live]", text, flush=True)
    QtWidgets.QMessageBox.critical(main_window(), TITLE, text)


def join(stage, session):
    """Joins or starts a session, as the Join buttons do; reports failures in a dialog. Returns the link."""
    QtWidgets.QApplication.setOverrideCursor(QtCore.Qt.WaitCursor)
    try:
        link = plasma_usd_live.join_session(stage, session)
    except Exception as e:
        traceback.print_exc()
        QtWidgets.QApplication.restoreOverrideCursor()
        error("Could not join '{}': {}".format(session, e))
        return None
    QtWidgets.QApplication.restoreOverrideCursor()
    return link


def join_from_environment():
    """Joins the session the Plasma editor launched this 3ds Max into, named by PLASMA_USD_LIVE_STAGE and
    PLASMA_USD_LIVE_SESSION, and shows the panel. The variables are taken out of the environment first, so the processes
    3ds Max starts do not join too. Returns the link, or None when 3ds Max was not launched into a session."""
    stage = os.environ.pop("PLASMA_USD_LIVE_STAGE", "")
    session = os.environ.pop("PLASMA_USD_LIVE_SESSION", "")
    if not stage or not session:
        return None
    link = join(stage, session)
    if link is not None:
        open_panel()
    return link


def linked_stage():
    """The stage this scene was linked to in an earlier session, or None."""
    try:
        custom = rt.Name("custom")
        index = rt.fileProperties.findProperty(custom, "plasmaUsdLive")
        if index:
            import json
            return json.loads(str(rt.fileProperties.getPropertyValue(custom, index))).get("stage")
    except Exception:
        pass
    return None


class ChooseSession(QtWidgets.QDialog):
    """Picks one of the sessions beside a USD file, or names a new one."""

    def __init__(self, stage, parent=None):
        super().__init__(parent)
        self.setWindowTitle("Plasma USD Live Session")
        layout = QtWidgets.QVBoxLayout(self)
        layout.addWidget(QtWidgets.QLabel(os.path.basename(stage)))
        self.buttons = []
        for name, participants in sessions.list_sessions_beside(stage):
            button = QtWidgets.QRadioButton("{}    ({})".format(name, ", ".join(participants) or "no participants"))
            button.session_name = name
            layout.addWidget(button)
            self.buttons.append(button)
        self.new = QtWidgets.QRadioButton("New session, with your layer strongest:")
        self.new.session_name = None
        layout.addWidget(self.new)
        self.buttons.append(self.new)
        self.name = QtWidgets.QLineEdit("session")
        layout.addWidget(self.name)
        self.buttons[0].setChecked(True)
        box = QtWidgets.QDialogButtonBox(QtWidgets.QDialogButtonBox.Ok | QtWidgets.QDialogButtonBox.Cancel)
        box.button(QtWidgets.QDialogButtonBox.Ok).setText("Join")
        box.accepted.connect(self.accept)
        box.rejected.connect(self.reject)
        layout.addWidget(box)

    def session(self):
        for button in self.buttons:
            if button.isChecked():
                if button.session_name is not None:
                    return button.session_name
                return re.sub(r"[^A-Za-z0-9_\-]+", "-", self.name.text()).strip("-") or None
        return None


class Panel(QtWidgets.QDockWidget):
    def __init__(self, parent=None):
        super().__init__(TITLE, parent)
        self.setObjectName(OBJECT_NAME)
        self.setAllowedAreas(QtCore.Qt.LeftDockWidgetArea | QtCore.Qt.RightDockWidgetArea)
        self.shown_version = None
        self.shown_link = None
        self.session_signature = None
        self.refreshes = 0
        self.unavailable = usd_problem()

        body = QtWidgets.QWidget()
        self.setWidget(body)
        body.setMinimumWidth(360)
        outer = QtWidgets.QVBoxLayout(body)
        self.unavailable_label = QtWidgets.QLabel(self.unavailable or "")
        self.unavailable_label.setWordWrap(True)
        self.unavailable_label.setStyleSheet("color: #e0a040;")
        self.unavailable_label.setVisible(bool(self.unavailable))
        outer.addWidget(self.unavailable_label)
        self.pages = QtWidgets.QStackedWidget()
        outer.addWidget(self.pages)

        # -- not in a session -----------------------------------------------------------------------------------
        page = QtWidgets.QWidget()
        layout = QtWidgets.QVBoxLayout(page)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(QtWidgets.QLabel("<b>Open sessions</b>"))
        self.session_list = QtWidgets.QTreeWidget()
        self.session_list.setHeaderLabels(["Session", "Stage", "Participants", ""])
        self.session_list.setRootIsDecorated(False)
        self.session_list.itemDoubleClicked.connect(self.join_item)
        layout.addWidget(self.session_list, 1)
        self.none_label = QtWidgets.QLabel("No tool is in a session right now.")
        layout.addWidget(self.none_label)
        self.open_file = QtWidgets.QPushButton("Start or Join on a USD File...")
        self.open_file.clicked.connect(self.join_file)
        self.open_file.setEnabled(not self.unavailable)
        layout.addWidget(self.open_file)
        self.pages.addWidget(page)

        # -- in a session ---------------------------------------------------------------------------------------
        page = QtWidgets.QWidget()
        layout = QtWidgets.QVBoxLayout(page)
        layout.setContentsMargins(0, 0, 0, 0)
        self.title = QtWidgets.QLabel()
        self.title.setTextInteractionFlags(QtCore.Qt.TextSelectableByMouse)
        layout.addWidget(self.title)
        self.status = QtWidgets.QLabel()
        layout.addWidget(self.status)

        group = QtWidgets.QGroupBox("Participants")
        self.participants = QtWidgets.QLabel()
        QtWidgets.QVBoxLayout(group).addWidget(self.participants)
        self.participants_group = group
        layout.addWidget(group)

        group = QtWidgets.QGroupBox("Recent")
        self.recent = QtWidgets.QLabel()
        self.recent.setAlignment(QtCore.Qt.AlignTop | QtCore.Qt.AlignLeft)
        self.recent.setTextInteractionFlags(QtCore.Qt.TextSelectableByMouse)
        self.recent.setWordWrap(True)
        QtWidgets.QVBoxLayout(group).addWidget(self.recent)
        layout.addWidget(group, 1)

        self.leave_button = QtWidgets.QPushButton("Leave")
        self.leave_button.clicked.connect(self.leave)
        layout.addWidget(self.leave_button)
        self.pages.addWidget(page)

        self.timer = QtCore.QTimer(self)
        self.timer.timeout.connect(self.refresh)
        self.timer.start(REFRESH_MS)

    # -- state ---------------------------------------------------------------------------------------------------

    @staticmethod
    def link():
        link = plasma_usd_live.active_link()
        return link if link is not None and link.stage is not None else None

    def refresh(self, force=False):
        try:
            if not force and not self.isVisible():
                return
            self.refreshes += 1
            link = self.link()
            if link is None:
                if force or self.shown_link is not None or self.refreshes % SESSIONS_EVERY == 0:
                    self.show_sessions(force or self.shown_link is not None)
                self.shown_link = None
                return
            if force or link is not self.shown_link or link.version != self.shown_version:
                self.show_link(link)
        except Exception:
            traceback.print_exc()
            self.timer.stop()

    def show_sessions(self, force):
        self.pages.setCurrentIndex(0)
        found = sessions.list_open_sessions()
        linked = linked_stage()
        signature = [(s.session_root, s.name, tuple(sorted(p.name for p in s.participants))) for s in found]
        signature.append((self.unavailable, linked))
        if not force and signature == self.session_signature:
            return
        self.session_signature = signature
        self.session_list.clear()
        for session in found:
            name = session.name
            if linked and sessions.same_path(linked, session.stage):
                name += "  (linked in this scene)"
            item = QtWidgets.QTreeWidgetItem([name, os.path.basename(session.stage),
                                              ", ".join(p.name for p in session.participants), ""])
            item.setToolTip(1, session.stage)
            item.session = session
            self.session_list.addTopLevelItem(item)
            button = QtWidgets.QPushButton("Join")
            button.setEnabled(not self.unavailable)
            button.clicked.connect(lambda checked=False, s=session: self.join_session(s))
            self.session_list.setItemWidget(item, 3, button)
        for column in range(3):
            self.session_list.resizeColumnToContents(column)
        self.none_label.setVisible(not found)

    def show_link(self, link):
        self.shown_link = link
        self.shown_version = link.version
        self.pages.setCurrentIndex(1)
        present = sum(st.state == "present" for st in link.prims.values())
        self.title.setText("<b>{}</b><br>Session: {}<br>Your layer: {}.usda<br>Linked nodes: {}".format(
            os.path.basename(link.stage_path), link.session_name, link.participant, present))
        self.title.setToolTip(link.session_root)
        self.status.setText(hub_status(link))

        title, lines = participant_lines(link)
        self.participants_group.setTitle(title)
        self.participants.setText("\n".join(lines))

        self.recent.setText("\n".join(reversed(link.log)))

    # -- actions -------------------------------------------------------------------------------------------------

    def join_session(self, session):
        if join(session.stage, session.name) is not None:
            self.refresh(force=True)

    def join_item(self, item, column=0):
        session = getattr(item, "session", None)
        if session is not None and not self.unavailable:
            self.join_session(session)

    def join_file(self):
        stage, _ = QtWidgets.QFileDialog.getOpenFileName(self, "Choose a USD stage", "",
                                                         "USD Files (*.usd *.usda *.usdc *.usdz)")
        if not stage:
            return
        if not os.path.isfile(stage):
            error("Not a file: " + stage)
            return
        dialog = ChooseSession(stage, self)
        if exec_dialog(dialog) != QtWidgets.QDialog.Accepted:
            return
        session = dialog.session()
        if not session:
            error("The session needs a name")
            return
        if join(stage, session) is not None:
            self.refresh(force=True)

    def leave(self):
        plasma_usd_live.leave()
        self.refresh(force=True)
