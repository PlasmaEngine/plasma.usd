"""The Plasma menu in Maya's main window: USD Live... opens the panel, Leave USD Live Session leaves.

install() runs from userSetup.py once Maya's UI is up. When a Plasma menu exists already (another Plasma tool made it),
the items are added to it; installing again replaces them.
"""

from maya import cmds, mel

MENU = "plasmaMainMenu"
OPEN_ITEM = "plasmaUsdLiveOpenItem"
LEAVE_ITEM = "plasmaUsdLiveLeaveItem"


def open_panel(*_):
    from . import panel
    panel.open_panel()


def leave(*_):
    import plasma_usd_live
    plasma_usd_live.leave()


def install():
    """Adds the menu items; nothing in batch mode."""
    if cmds.about(batch=True):
        return
    if not cmds.menu(MENU, exists=True):
        window = mel.eval("$plasmaUsdLiveMainWindow = $gMainWindow")
        cmds.menu(MENU, label="Plasma", parent=window, tearOff=True)
    for item in (OPEN_ITEM, LEAVE_ITEM):
        if cmds.menuItem(item, exists=True):
            cmds.deleteUI(item, menuItem=True)
    cmds.menuItem(OPEN_ITEM, parent=MENU, label="USD Live...", command=open_panel,
                  annotation="Join the Plasma editor's USD live sessions")
    cmds.menuItem(LEAVE_ITEM, parent=MENU, label="Leave USD Live Session", command=leave)
