"""Plasma USD Live: join the Plasma editor's USD live sessions from Maya, through maya-usd.

Plasma > USD Live... opens the panel: it lists the sessions other tools are in, and starts or joins a session on any USD
file. Joining makes (or finds again) a maya-usd proxy shape on the session's session.usda, with this Maya's layer,
maya-<user>.usda, as its edit target. What is done to the stage's USD prims in Maya goes to the session as that layer,
and the other participants' edits show up in the same stage. Edits travel through the Plasma editor's live hub when it
runs one for the session, otherwise through the session files. Maya data made from USD prims (Edit as Maya) is not
synced.

Modules: sessions (discovery and presence) and hub (the connection to the Plasma hub), standard library only; opinions
(what a stronger layer outvotes) and live_link (the sync), pxr only; maya_host (the proxy shape, edit target, layer
protection, timer and scene callbacks), qt (PySide6 or PySide2), panel (the dockable panel) and menu (the Plasma menu).

Installed as a Maya module: a plasma_usd_live.mod in the user's maya/modules folder puts scripts/ on Maya's Python path
at startup. The Plasma editor's Apps tab writes it for a copy of the connector beside it; install.py writes it for this
folder. scripts/userSetup.py adds the Plasma menu, and joins the session when the Plasma editor launched Maya into one.

Nothing here imports Maya or pxr when the package is imported: pxr is importable once maya-usd is loaded, which joining
does first.
"""

import getpass
import sys

__version__ = "0.3.0"


def user_name():
    return getpass.getuser().lower().replace(" ", "_")


def join_session(stage, session):
    """Joins or starts a session on a USD stage as maya-<user>; returns the live link."""
    from . import maya_host
    return maya_host.join_session(stage, session, user_name())


def active_link():
    """The link of the joined session, or None."""
    module = sys.modules.get(__name__ + ".live_link")
    return module.active_link() if module is not None else None


def leave():
    link = active_link()
    if link is not None:
        link.leave()
