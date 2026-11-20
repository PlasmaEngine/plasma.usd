"""Plasma USD Live: join the Plasma editor's USD live sessions from 3ds Max, through USD for 3ds Max.

Plasma > USD Live... opens the panel: it lists the sessions other tools are in, and starts or joins a session on any USD
file. Joining imports the stage as native nodes (or finds them again in a scene saved in that session) and keeps them
and the session in step: what is done to them in 3ds Max goes to the session as this 3ds Max's layer, max-<user>.usda,
and the other participants' edits show up on them. Edits travel through the Plasma editor's live hub when it runs one
for the session, otherwise through the session files. See live_link.py for what is synced.

Modules: sessions (discovery and presence) and hub (the connection to the Plasma hub), standard library only;
live_link (the sync) and meshes (Mesh prims for added nodes), pxr only; max_host (USD for 3ds Max, nodes, timer and
callbacks), qt (PySide6 or PySide2) and panel (the dockable panel).

Installed as an Autodesk application package, PlasmaUsdLive in the user's ApplicationPlugins folder, which every 3ds
Max version from 2024 reads at startup. The Plasma editor's Apps tab copies the connector there; install.py writes a
package pointing at this folder. Its startup script (Contents/startup/plasma_usd_live.ms) puts Contents/python on
Python's path, adds the Plasma menu, and joins the session when the Plasma editor launched 3ds Max into one.

Nothing here imports pymxs or pxr when the package is imported.
"""

import getpass
import sys

__version__ = "0.3.0"


def user_name():
    return getpass.getuser().lower().replace(" ", "_")


def join_session(stage, session):
    """Joins or starts a session on a USD stage as max-<user>; returns the live link."""
    from . import max_host
    return max_host.join_session(stage, session, user_name())


def active_link():
    """The link of the joined session, or None."""
    module = sys.modules.get(__name__ + ".live_link")
    return module.active_link() if module is not None else None


def leave():
    link = active_link()
    if link is not None:
        link.leave()
