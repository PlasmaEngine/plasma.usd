# SPDX-License-Identifier: GPL-3.0-or-later
"""What ``import maya`` / ``import maya.cmds`` / ``from maya import cmds`` import in a Command Line, Script Editor or
shelf entry: ``runner.prepare`` rewrites those lines to import this module by its package path (no import hook)."""

from . import cmds_shim

cmds = cmds_shim
