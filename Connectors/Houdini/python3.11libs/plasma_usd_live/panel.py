"""The "Plasma USD Live" Python Panel.

Not in a session, it lists the open sessions (from the presence files) with a Join button each, marks the ones this
scene already has nodes for, and starts or joins a session on any USD file. In a session it shows the stage and
session, this Houdini's layer, how edits travel ("Live via Plasma hub" with the round trip time, or "File sync"), the
participants (from the hub with their live state and latency while connected, otherwise from the session files, strongest
first), the opinions of this Houdini's that a stronger layer outvotes, what stops the network's opinions from being
read, the recent sync events, and Leave.

The panel only shows the link's state and starts or ends it; the link itself runs from Houdini's event loop, so closing
the panel does not leave the session. It refreshes every half second, and the session list every two seconds.
"""

import os
import re
import traceback
import weakref

import hou
from PySide6 import QtCore, QtGui, QtWidgets

import plasma_usd_live
from . import hub, live_link, network, sessions

INTERFACE = "plasma_usd_live"
REFRESH_MS = 500
SESSIONS_EVERY = 4  # refreshes between two reads of the presence folder

_panels = weakref.WeakSet()


def instances():
    return list(_panels)


def open_panel():
    """Shows the panel: the pane tab that has it already, or a new floating one."""
    for tab in hou.ui.paneTabs():
        if tab.type() == hou.paneTabType.PythonPanel:
            interface = tab.activeInterface()
            if interface is not None and interface.name() == INTERFACE:
                tab.setIsCurrentTab()
                return tab
    tab = hou.ui.curDesktop().createFloatingPaneTab(hou.paneTabType.PythonPanel, size=(460, 620))
    tab.setActiveInterface(hou.pypanel.interfaceByName(INTERFACE))
    tab.showToolbar(False)
    return tab


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


def join(stage, session, parent=None):
    """Joins or starts a session, as the Join buttons do; reports failures in a message box. Returns the link."""
    QtWidgets.QApplication.setOverrideCursor(QtCore.Qt.WaitCursor)
    try:
        link = plasma_usd_live.join_session(stage, session)
    except Exception as e:
        traceback.print_exc()
        QtWidgets.QApplication.restoreOverrideCursor()
        hou.ui.displayMessage("Could not join '{}': {}".format(session, e), severity=hou.severityType.Error,
                              title="Plasma USD Live")
        return None
    QtWidgets.QApplication.restoreOverrideCursor()
    show_setup(link)
    return link


def join_from_environment():
    """Joins the session the Plasma editor launched this Houdini into, named by PLASMA_USD_LIVE_STAGE and
    PLASMA_USD_LIVE_SESSION, and shows the panel. The variables are taken out of the environment first, so the processes
    Houdini starts do not join too. Returns the link, or None when Houdini was not launched into a session."""
    stage = os.environ.pop("PLASMA_USD_LIVE_STAGE", "")
    session = os.environ.pop("PLASMA_USD_LIVE_SESSION", "")
    if not stage or not session:
        return None
    link = join(stage, session)
    if link is not None:
        open_panel()
    return link


def show_setup(link):
    """Makes plasma_live current, and shows its network in a network editor that looks at another LOP network or
    none."""
    live = link.live_node()
    if live is None:
        return
    live.setCurrent(True, clear_all_selected=True)
    for tab in hou.ui.paneTabs():
        if tab.type() == hou.paneTabType.NetworkEditor and tab.isCurrentTab():
            if tab.pwd() != live.parent():
                tab.setPwd(live.parent())
            break


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


class Panel(QtWidgets.QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        _panels.add(self)
        self.shown_version = None
        self.shown_link = None
        self.open_sessions = []
        self.session_signature = None
        self.refreshes = 0

        outer = QtWidgets.QVBoxLayout(self)
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
        self.problem = QtWidgets.QLabel()
        self.problem.setWordWrap(True)
        self.problem.setStyleSheet("color: #e0a040;")
        layout.addWidget(self.problem)

        group = QtWidgets.QGroupBox("Participants")
        self.participants = QtWidgets.QLabel()
        self.participants.setTextFormat(QtCore.Qt.RichText)
        QtWidgets.QVBoxLayout(group).addWidget(self.participants)
        self.participants_group = group
        layout.addWidget(group)

        group = QtWidgets.QGroupBox("Outvoted: a stronger layer decides these")
        group_layout = QtWidgets.QVBoxLayout(group)
        note = QtWidgets.QLabel("Houdini shows its own value for these; Plasma and the other tools show the stronger "
                                "layer's.")
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
        self.show_nodes = QtWidgets.QPushButton("Show plasma_live")
        self.show_nodes.clicked.connect(lambda: self.link() and show_setup(self.link()))
        row.addWidget(self.show_nodes)
        self.leave_button = QtWidgets.QPushButton("Leave")
        self.leave_button.clicked.connect(self.leave)
        row.addWidget(self.leave_button)
        layout.addLayout(row)
        self.pages.addWidget(page)

        self.timer = QtCore.QTimer(self)
        self.timer.timeout.connect(self.refresh)
        self.timer.start(REFRESH_MS)
        self.refresh(force=True)

    # -- state ---------------------------------------------------------------------------------------------------

    @staticmethod
    def link():
        link = live_link.active_link()
        return link if link is not None and link.stage is not None else None

    def refresh(self, force=False):
        try:
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
        in_scene = {sessions.path_key(root) for root in network.sessions_in_scene()}
        signature = [(s.session_root, s.name, tuple(sorted(p.name for p in s.participants)),
                      sessions.path_key(s.session_root) in in_scene) for s in found]
        if not force and signature == self.session_signature:
            return
        self.session_signature = signature
        self.open_sessions = found
        self.session_list.clear()
        for session in found:
            name = session.name + ("  (nodes in this scene)" if sessions.path_key(session.session_root) in in_scene
                                   else "")
            item = QtWidgets.QTreeWidgetItem([name, os.path.basename(session.stage),
                                              ", ".join(p.name for p in session.participants), ""])
            item.setToolTip(1, session.stage)
            item.session = session
            self.session_list.addTopLevelItem(item)
            button = QtWidgets.QPushButton("Join")
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
        self.problem.setText(link.network_problem or "")
        self.problem.setVisible(bool(link.network_problem))

        lines = []
        if link.hub.live and link.hub.participants:
            self.participants_group.setTitle("Participants (Plasma hub)")
            for p in link.hub.participants:
                name = str(p.get("participant", "?"))
                text = name + ("  (you)" if name == link.participant else "")
                if p.get("live") is True:
                    latency = format_latency(p.get("latencyMs"))
                    text += " - live" + (", " + latency if latency else "")
                else:
                    text += " - not live"
                lines.append(text)
        else:
            self.participants_group.setTitle("Participants, strongest first")
            for name, online in link.participants():
                lines.append(name + ("  (you)" if name == link.participant else "") + ("" if online else " - away"))
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
        if join(session.stage, session.name, self) is not None:
            self.refresh(force=True)

    def join_item(self, item, column=0):
        session = getattr(item, "session", None)
        if session is not None:
            self.join_session(session)

    def join_file(self):
        path = hou.ui.selectFile(title="Choose a USD stage", pattern="*.usd *.usda *.usdc *.usdz",
                                 chooser_mode=hou.fileChooserMode.Read)
        if not path:
            return
        stage = hou.text.expandString(path)
        if not os.path.isfile(stage):
            hou.ui.displayMessage("Not a file: " + stage, severity=hou.severityType.Error, title="Plasma USD Live")
            return
        dialog = ChooseSession(stage, self)
        if dialog.exec() != QtWidgets.QDialog.Accepted:
            return
        session = dialog.session()
        if not session:
            hou.ui.displayMessage("The session needs a name", severity=hou.severityType.Error, title="Plasma USD Live")
            return
        if join(stage, session, self) is not None:
            self.refresh(force=True)

    def leave(self):
        plasma_usd_live.leave()
        self.refresh(force=True)

    def select_prim(self, item):
        """Selects an outvoted prim in the viewport and scene graph tree."""
        for tab in hou.ui.paneTabs():
            if tab.type() == hou.paneTabType.SceneViewer:
                tab.setCurrentSceneGraphSelection([item.prim_path])
                break

    def close_panel(self):
        self.timer.stop()
        _panels.discard(self)
