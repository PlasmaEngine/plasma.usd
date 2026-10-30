"""Installs the Plasma USD Live connector into Maya as a module that points at this folder, for working on the connector.
Users install a copy from the Plasma editor's USD Live panel (the Apps tab), which the editor then reports as its own; one
installed with this script is reported as linked and left alone.

    python install.py [modules folder]      (default: the user's maya/modules folder)
    python install.py --uninstall [folder]

It writes <modules folder>/plasma_usd_live.mod naming this folder. Maya 2024 to 2026 read the module files there at
startup and put this folder's scripts/ on their Python path; scripts/userSetup.py then adds the Plasma menu. Nothing is
copied, so edits to this folder apply the next time Maya starts. plasma_usd_live.mod beside this script names this
folder relatively, for adding this folder to MAYA_MODULE_PATH instead.

The default folder is $MAYA_APP_DIR/modules when MAYA_APP_DIR is set, otherwise maya/modules in the user's Documents
folder (on Windows the shell's Documents folder, which may be redirected, as Maya uses it), on macOS
~/Library/Preferences/Autodesk/maya/modules, on Linux ~/maya/modules.
"""

import os
import re
import sys

MODULE_FILE = "plasma_usd_live.mod"
MODULE_NAME = "plasma_usd_live"


def module_version():
    """The connector's __version__, read as text so this script runs without Maya."""
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, "scripts", MODULE_NAME, "__init__.py"), encoding="utf-8") as f:
        match = re.search(r'^__version__\s*=\s*"([^"]+)"', f.read(), re.MULTILINE)
    return match.group(1) if match else "0.0.0"


def documents_folder():
    if sys.platform == "win32":
        try:
            import ctypes
            buffer = ctypes.create_unicode_buffer(260)
            if ctypes.windll.shell32.SHGetFolderPathW(None, 5, None, 0, buffer) == 0 and buffer.value:  # CSIDL_PERSONAL
                return buffer.value
        except (AttributeError, OSError):
            pass
        # USERPROFILE rather than HOME, which shells like Git Bash point elsewhere.
        return os.path.join(os.environ.get("USERPROFILE") or os.path.expanduser("~"), "Documents")
    return os.path.expanduser("~")


def default_modules_folder():
    app_dir = os.environ.get("MAYA_APP_DIR")
    if app_dir:
        return os.path.join(app_dir, "modules")
    if sys.platform == "win32":
        return os.path.join(documents_folder(), "maya", "modules")
    if sys.platform == "darwin":
        return os.path.expanduser("~/Library/Preferences/Autodesk/maya/modules")
    return os.path.expanduser("~/maya/modules")


def main(argv):
    uninstall = "--uninstall" in argv
    args = [a for a in argv if a != "--uninstall"]
    folder = args[0] if args else default_modules_folder()
    target = os.path.join(folder, MODULE_FILE)

    if uninstall:
        if os.path.exists(target):
            os.remove(target)
            print("Removed", target)
        else:
            print("Not installed:", target)
        return 0

    here = os.path.dirname(os.path.abspath(__file__)).replace("\\", "/")
    if " " in here:
        print("Warning: Maya may not read a module path with spaces:", here)
    os.makedirs(folder, exist_ok=True)
    with open(target, "w", encoding="utf-8", newline="\n") as f:
        f.write("+ {} {} {}\n".format(MODULE_NAME, module_version(), here))
    print("Installed", target, "->", here)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
