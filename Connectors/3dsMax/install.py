"""Installs the Plasma USD Live connector into 3ds Max as an application package that points at this folder, for working
on the connector. Users install a copy from the Plasma editor's USD Live panel (the Apps tab), which the editor then
reports as its own; one installed with this script is reported as linked and left alone.

    python install.py [ApplicationPlugins folder]      (default: %APPDATA%/Autodesk/ApplicationPlugins)
    python install.py --uninstall [folder]

It writes <folder>/PlasmaUsdLive: this folder's PackageContents.xml, and startup scripts that only fileIn the ones in
this folder, so the connector's Python is read from here and edits apply the next time 3ds Max starts. Contents/linked.txt
names this folder; the Apps tab reads it to say where the link goes. Every 3ds Max from 2024 reads the packages in that
folder at startup.
"""

import os
import shutil
import sys

PACKAGE = "PlasmaUsdLive"
STARTUP_SCRIPTS = ("plasma_usd_live_menu.ms", "plasma_usd_live.ms")


def default_folder():
    base = os.environ.get("APPDATA") or os.path.join(os.path.expanduser("~"), "AppData", "Roaming")
    return os.path.join(base, "Autodesk", "ApplicationPlugins")


def main(argv):
    uninstall = "--uninstall" in argv
    args = [a for a in argv if a != "--uninstall"]
    target = os.path.join(args[0] if args else default_folder(), PACKAGE)

    if uninstall:
        if os.path.isdir(target):
            shutil.rmtree(target)
            print("Removed", target)
        else:
            print("Not installed:", target)
        return 0

    here = os.path.dirname(os.path.abspath(__file__))
    if os.path.isdir(target):
        shutil.rmtree(target)
    startup = os.path.join(target, "Contents", "startup")
    os.makedirs(startup)
    shutil.copyfile(os.path.join(here, "PackageContents.xml"), os.path.join(target, "PackageContents.xml"))
    for name in STARTUP_SCRIPTS:
        source = os.path.join(here, "Contents", "startup", name).replace("\\", "/")
        with open(os.path.join(startup, name), "w", encoding="utf-8", newline="\n") as f:
            f.write("-- Linked to the Plasma USD Live connector source by install.py.\n")
            f.write('fileIn @"{}"\n'.format(source))
    with open(os.path.join(target, "Contents", "linked.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write(here.replace("\\", "/") + "\n")
    print("Installed", target, "->", here)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
