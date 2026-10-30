"""The "Plasma USD Live" panel, a dockable Maya workspace control.

Not in a session, it lists the open sessions (from the presence files) with a Join button each, marks the ones this
scene already has a proxy shape for, and starts or joins a session on any USD file. In a session it shows the stage and
session, this Maya's layer, how edits travel ("Live via Plasma hub" with the round trip time, or "File sync"), the
participants (from the hub with their live state and latency while connected, otherwise from the session files,
strongest first), the opinions of Maya's layer that a stronger layer outvotes (double-click selects the prim), what keeps
Maya's edits from reaching the session (with Target My Layer when the edit target moved), the recent sync events, Select
Proxy and Leave. When maya-usd cannot be loaded, the panel says why and joining is disabled.

The panel only shows the link's state and starts or ends it; the link runs from its own timer, so closing the panel does
not leave the session. It refreshes every half second, and the session list every two seconds.

Docked, Maya rebuilds the panel at startup: the workspace control's uiScript calls open_panel(restore=True), the pattern
of Maya's MayaQWidgetDockableMixin documentation.
"""

import os
import re
import traceback

from maya import OpenMayaUI as omui
from maya import cmds
from maya.app.general.mayaMixin import MayaQWidgetDockableMixin

import plasma_usd_live
from . import hub, maya_host, sessions
from .qt import QtCore, QtWidgets, exec_dialog, is_valid

TITLE = "Plasma USD Live"
OBJECT_NAME = "plasmaUsdLivePanel"
WORKSPACE_CONTROL = OBJECT_NAME + "WorkspaceControl"  # the name MayaQWidgetDockableMixin gives it
UI_SCRIPT = "import plasma_usd_live.panel; plasma_usd_live.panel.open_panel(restore=True)"
REFRESH_MS = 500
SESSIONS_EVERY = 4  # refreshes between two reads of the presence folder

_panel = None


def open_panel(restore=False):
    """Shows the panel; with restore, fills the workspace control Maya rebuilt at startup."""
    global _panel
    if not is_valid(_panel):
        _panel = None
    if restore:
        parent = omui.MQtUtil.getCurrentParent()
        if _panel is None:
            _panel = Panel()
        pointer = omui.MQtUtil.findControl(_panel.objectName())
        omui.MQtUtil.addWidgetToMayaLayout(int(pointer), int(parent))
        return _panel

    if _panel is None and cmds.workspaceControl(WORKSPACE_CONTROL, query=True, exists=True):
        cmds.deleteUI(WORKSPACE_CONTROL)  # left by a panel that is gone
    if _panel is None:
        _panel = Panel()
    _panel.show(dockable=True, width=460, height=620, uiScript=UI_SCRIPT)
    if cmds.workspaceControl(WORKSPACE_CONTROL, query=True, exists=True):
        cmds.workspaceControl(WORKSPACE_CONTROL, edit=True, restore=True, visible=True)
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


def error(text):
    print("[Plasma USD Live]", text, flush=True)
    cmds.confirmDialog(title=TITLE, message=text, button=["OK"], icon="critical")


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
    """Joins the session the Plasma editor launched this Maya into, named by PLASMA_USD_LIVE_STAGE and
    PLASMA_USD_LIVE_SESSION, and shows the panel. The variables are taken out of the environment first, so the processes
    Maya starts do not join too. Returns the link, or None when Maya was not launched into a session."""
    stage = os.environ.pop("PLASMA_USD_LIVE_STAGE", "")
    session = os.environ.pop("PLASMA_USD_LIVE_SESSION", "")
    if not stage or not session or cmds.about(batch=True):
        return None
    link = join(stage, session)
    if link is not None:
        open_panel()
    return link


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


class Panel(MayaQWidgetDockableMixin, QtWidgets.QWidget):
    def __init__(self, parent=None):
        super().__init__(parent=parent)
        self.setObjectName(OBJECT_NAME)
        self.setWindowTitle(TITLE)
        self.shown_version = None
        self.shown_link = None
        self.open_sessions = []
        self.session_signature = None
        self.refreshes = 0
        self.unavailable = None  # why maya-usd cannot be used, or None

        outer = QtWidgets.QVBoxLayout(self)
        self.unavailable_label = QtWidgets.QLabel()
        self.unavailable_label.setWordWrap(True)
        self.unavailable_label.setStyleSheet("color: #e0a040;")
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
        row = QtWidgets.QHBoxLayout()
        self.problem = QtWidgets.QLabel()
        self.problem.setWordWrap(True)
        self.problem.setStyleSheet("color: #e0a040;")
        row.addWidget(self.problem, 1)
        self.retarget_button = QtWidgets.QPushButton("Target My Layer")
        self.retarget_button.clicked.connect(self.retarget)
        row.addWidget(self.retarget_button)
        layout.addLayout(row)

        group = QtWidgets.QGroupBox("Participants")
        self.participants = QtWidgets.QLabel()
        self.participants.setTextFormat(QtCore.Qt.RichText)
        QtWidgets.QVBoxLayout(group).addWidget(self.participants)
        self.participants_group = group
        layout.addWidget(group)

        group = QtWidgets.QGroupBox("Outvoted: a stronger layer decides these")
        group_layout = QtWidgets.QVBoxLayout(group)
        note = QtWidgets.QLabel("Maya and the other tools show the stronger layer's value; yours has no effect while "
                                "that layer decides.")
        note.setWordWrap(True)
        group_layout.addWidget(note)
        self.outvoted = QtWidgets.QListWidget()
        self.outvoted.itemDoubleClicked.connect(self.select_prim)
        group_layout.addWidget(self.outvoted)
        self.outvoted_group = group
        layout.addWidget(group, 1)

        group = QtWidgets.QGroupBox("Recent")
        self.recent = QtWidgets.QLabel()
        self.recent.setAlignment(QtCore.Qt.AlignTop | QtCore.Qt.AlignLeft)
        self.recent.setTextInteractionFlags(QtCore.Qt.TextSelectableByMouse)
        QtWidgets.QVBoxLayout(group).addWidget(self.recent)
        layout.addWidget(group, 1)

        row = QtWidgets.QHBoxLayout()
        self.select_button = QtWidgets.QPushButton("Select Proxy")
        self.select_button.clicked.connect(self.select_proxy)
        row.addWidget(self.select_button)
        self.leave_button = QtWidgets.QPushButton("Leave")
        self.leave_button.clicked.connect(self.leave)
        row.addWidget(self.leave_button)
        layout.addLayout(row)
        self.pages.addWidget(page)

        self.check_maya_usd()
        self.timer = QtCore.QTimer(self)
        self.timer.timeout.connect(self.refresh)
        self.timer.start(REFRESH_MS)
        self.refresh(force=True)

    # -- state ---------------------------------------------------------------------------------------------------

    @staticmethod
    def link():
        link = plasma_usd_live.active_link()
        return link if link is not None and link.stage is not None else None

    def check_maya_usd(self):
        try:
            maya_host.ensure_plugin()
            self.unavailable = None
        except RuntimeError as e:
            self.unavailable = str(e)
        self.unavailable_label.setText(self.unavailable or "")
        self.unavailable_label.setVisible(bool(self.unavailable))
        self.open_file.setEnabled(not self.unavailable)

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
        try:
            in_scene = {sessions.path_key(root) for root in maya_host.sessions_in_scene()}
        except Exception:
            in_scene = set()
        signature = [(s.session_root, s.name, tuple(sorted(p.name for p in s.participants)),
                      sessions.path_key(s.session_root) in in_scene) for s in found]
        signature.append(self.unavailable)
        if not force and signature == self.session_signature:
            return
        self.session_signature = signature
        self.open_sessions = found
        self.session_list.clear()
        for session in found:
            name = session.name + ("  (proxy in this scene)" if sessions.path_key(session.session_root) in in_scene
                                   else "")
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
        self.title.setText("<b>{}</b><br>Session: {}<br>Your layer: {}.usda".format(
            os.path.basename(link.stage_path), link.session_name, link.participant))
        self.title.setToolTip(link.session_root)
        self.status.setText(hub_status(link))
        self.problem.setText(link.problem or "")
        self.problem.setVisible(bool(link.problem))
        self.retarget_button.setVisible(bool(link.target_elsewhere))

        title, lines = participant_lines(link)
        self.participants_group.setTitle(title)
        self.participants.setText("<br>".join(lines))

        self.outvoted.clear()
        for path, what, by in link.outvoted:
            item = QtWidgets.QListWidgetItem("{}  {}: {} decides".format(path, what, by))
            item.prim_path = path
            self.outvoted.addItem(item)
        self.outvoted_group.setVisible(bool(link.outvoted))

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
        found = cmds.fileDialog2(fileMode=1, caption="Choose a USD stage", okCaption="Choose",
                                 fileFilter="USD Files (*.usd *.usda *.usdc *.usdz)")
        if not found:
            return
        stage = found[0]
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

    def retarget(self):
        link = self.link()
        if link is not None:
            link.retarget()
            self.refresh(force=True)

    def select_proxy(self):
        link = self.link()
        if link is not None:
            maya_host.select_proxy(link)

    def select_prim(self, item):
        link = self.link()
        if link is not None:
            maya_host.select_prim(link, item.prim_path)

    def leave(self):
        plasma_usd_live.leave()
        self.refresh(force=True)
