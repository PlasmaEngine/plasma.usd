"""Plasma USD Live: adds the Plasma menu to Maya's main window once the UI is up, and joins the session when the Plasma
editor launched this Maya into one (PLASMA_USD_LIVE_STAGE and PLASMA_USD_LIVE_SESSION). Maya runs every userSetup.py on
its Python path at startup; the plasma_usd_live module puts this folder there."""

import os

import maya.utils


def _plasma_usd_live_menu():
    try:
        from plasma_usd_live import menu
        menu.install()
        if os.environ.get("PLASMA_USD_LIVE_STAGE") and os.environ.get("PLASMA_USD_LIVE_SESSION"):
            from plasma_usd_live import panel
            panel.join_from_environment()
    except Exception:
        import traceback
        traceback.print_exc()


maya.utils.executeDeferred(_plasma_usd_live_menu)
