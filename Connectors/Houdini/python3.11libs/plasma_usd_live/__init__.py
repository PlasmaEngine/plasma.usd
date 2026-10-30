"""Plasma USD Live: join the Plasma editor's USD live sessions from Houdini (Solaris).

Open the "Plasma USD Live" panel (Windows menu, the Plasma shelf, or a Python Panel pane tab): it lists the sessions
other tools are in, and starts or joins a session on any USD file. Joining builds the session's LOP setup in /stage
(network.py); edits made with LOPs wired in above the plasma_live node go to the session as this Houdini's layer,
houdini-<user>.usda, and the other participants' edits show up in the same stage. Edits travel through the Plasma
editor's live hub when it runs one for the session, otherwise through the session files (live_link.py).

Modules: sessions (discovery and presence), hub (the connection to the Plasma hub), network (the LOP setup),
opinions (Houdini's opinions as a plain layer, and the ones the session outvotes), live_link (the sync), panel (the
Python Panel).

Installed as a Houdini package: a JSON file in the user's houdini<version>/packages folder adds this connector's folder
to HOUDINI_PATH. The Plasma editor's Apps tab writes it for a copy of the connector in the preference folder; install.py
writes it for this folder. A Houdini the Plasma editor launches into a session joins it on startup (uiready.py).
"""

import getpass

from . import live_link

__version__ = "0.3.0"


def user_name():
    return getpass.getuser().lower().replace(" ", "_")


def join_session(stage, session):
    """Joins or starts a session on a USD stage as houdini-<user>; returns the live link."""
    return live_link.join_session(stage, session, user_name())


def active_link():
    return live_link.active_link()


def leave():
    live_link.leave_active()
