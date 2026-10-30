# SPDX-License-Identifier: GPL-3.0-or-later
"""Maya file dialog's left column in every Blender file browser: Current Project + the project's folders + Set
Project (Maya's file dialog: under Folder Bookmarks, "Current Project:"
drop-down, the project folder list, "Set Project..." button). Blender's own bookmarks stay above it.
docs/steps/step_102.md 「계획」 1."""

import os

import bpy

from ..operators import project


class ALLSET_PT_file_project(bpy.types.Panel):
    bl_space_type = 'FILE_BROWSER'
    bl_region_type = 'TOOLS'
    bl_category = "Bookmarks"
    bl_label = "Current Project"

    @classmethod
    def poll(cls, context):
        space = context.space_data
        return space is not None and space.type == 'FILE_BROWSER' and space.browse_mode == 'FILES'

    def draw(self, context):
        layout = self.layout
        root = project.current_root(context)
        layout.menu("ALLSET_MT_recent_projects", text=os.path.basename(root) or root, icon='DOWNARROW_HLT')
        col = layout.column(align=True)
        rows = [("Workspace Root", root)] + [(name, os.path.join(root, name)) for name in project.FOLDERS
                                              if os.path.isdir(os.path.join(root, name))]
        for text, path in rows:
            row = col.row(align=True)
            row.alignment = 'LEFT'
            row.operator("allset.file_goto", text=text, icon='FILE_FOLDER', emboss=False).path = path
        layout.operator("allset.file_set_project", text="Set Project...")


classes = (ALLSET_PT_file_project,)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
