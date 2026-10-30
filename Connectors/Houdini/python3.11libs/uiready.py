"""Plasma USD Live: joins the session the Plasma editor launched this Houdini into, once Houdini's UI is up.

Houdini runs every uiready.py on HOUDINI_PATH when its UI is ready. This one does nothing unless the Plasma editor set
PLASMA_USD_LIVE_STAGE and PLASMA_USD_LIVE_SESSION for the process (its Open in Houdini).
"""

import os

if os.environ.get("PLASMA_USD_LIVE_STAGE") and os.environ.get("PLASMA_USD_LIVE_SESSION"):
    import hdefereval

    def _plasma_usd_live_join():
        try:
            from plasma_usd_live import panel
            panel.join_from_environment()
        except Exception:
            import traceback
            traceback.print_exc()

    hdefereval.execute_deferred(_plasma_usd_live_join)
