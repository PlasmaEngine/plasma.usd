"""Installs the Plasma USD Live connector into a user's Houdini as a package that points at this folder, for working on the
connector. Users install a copy from the Plasma editor's USD Live panel (the Apps tab), which the editor then reports as
its own; one installed with this script is reported as linked and left alone.

    python install.py [houdini preference folder]      (default: Documents/houdini21.0 of the current user)
    python install.py --uninstall [folder]

It writes <preference folder>/packages/plasma_usd_live.json, which adds this folder to HOUDINI_PATH: Houdini then
finds python3.11libs/plasma_usd_live, the Python panel, the Plasma shelf and the Windows menu entry here. Nothing is
copied, so edits to this folder apply the next time Houdini starts.
"""

import json
import os
import sys

HOUDINI_VERSION = "21.0"
PACKAGE_FILE = "plasma_usd_live.json"


def default_preference_folder():
    # Houdini's Windows default; USERPROFILE rather than HOME, which shells like Git Bash point elsewhere.
    home = os.environ.get("USERPROFILE") or os.path.expanduser("~")
    return os.path.join(home, "Documents", "houdini" + HOUDINI_VERSION)


def main(argv):
    uninstall = "--uninstall" in argv
    args = [a for a in argv if a != "--uninstall"]
    folder = args[0] if args else default_preference_folder()
    target = os.path.join(folder, "packages", PACKAGE_FILE)

    if uninstall:
        if os.path.exists(target):
            os.remove(target)
            print("Removed", target)
        else:
            print("Not installed:", target)
        return 0

    here = os.path.dirname(os.path.abspath(__file__)).replace("\\", "/")
    package = {
        "env": [{"PLASMA_USD_LIVE_HOUDINI": here}],
        "path": "$PLASMA_USD_LIVE_HOUDINI",
    }
    os.makedirs(os.path.dirname(target), exist_ok=True)
    with open(target, "w", encoding="utf-8", newline="\n") as f:
        json.dump(package, f, indent=4)
        f.write("\n")
    print("Installed", target, "->", here)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
