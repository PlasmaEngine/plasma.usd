# SPDX-License-Identifier: GPL-3.0-or-later
"""Add-on preferences (Preferences > Add-ons > Maya Workflow Kit).

Options backed by docs/user_decisions.md: Maya units (#1) and parent mode (#2)
from step 10. Only this add-on's own settings live here: since step 130 (#13) no option writes Blender's
preferences (the navigation / undo / panel / auto key / theme / splash / density / font / wire presets are gone).
"""

import bpy


class ALLSET_AddonPreferences(bpy.types.AddonPreferences):
    # Must equal the top-level package name: "bl_ext.<repo>.allset" as an
    # extension, "allset" as a legacy add-on.
    bl_idname = __package__

    # Set once "Maya" was activated automatically on first enable, so later
    # starts respect a different keymap the user picked (docs/steps/step_2.md).
    keymap_auto_activated: bpy.props.BoolProperty(options={'HIDDEN'})

    # Maya mode on/off (switch.py, docs/steps/step_60.md), kept across restarts. Off after install: enabling the
    # extension changes nothing until the user opts in (welcome dialog / button / switch; docs/steps/step_115.md).
    maya_mode: bpy.props.BoolProperty(name="Maya Mode", default=False, options={'HIDDEN'})
    # Edit-mode marquee/lasso also selects occluded components (Maya selectPrefUseDepth false).
    select_through: bpy.props.BoolProperty(
        name="Marquee Selects Hidden Components",
        description="In mesh edit mode, box and lasso selection also pick vertices, edges and faces "
                    "behind the surface, like Maya (Camera based selection off). X-ray is switched "
                    "on only while dragging",
        default=True)
    # Maya preSelectHilite true.
    preselect_highlight: bpy.props.BoolProperty(
        name="Pre-selection Highlight",
        description="In mesh edit mode (select and Move / Rotate / Scale tools), highlight the vertex, edge or face "
                    "under the mouse before clicking (Maya)",
        default=True)
    # Maya selectPrefPriorityMode "animation", ignoreSelectionPriority false.
    select_priority: bpy.props.BoolProperty(
        name="Selection Priority",
        description="In object mode, a click picks IK handles, then joints (armatures), then curves before meshes "
                    "under the mouse, like Maya's selection priority",
        default=True)
    # Step 217 / docs/strategy/DECISIONS7.md decision 6: bone collections = the rigger's "animator controls" set.
    pick_walk_skip_hidden_collections: bpy.props.BoolProperty(
        name="Pick Walk Skips Hidden Bone Collections",
        description="In Pose mode the arrow keys walk past bones whose bone collections are all hidden (Rigify MCH / "
                    "DEF / ORG bones), to the nearest shown parent, child or sibling; a generated Rigify rig follows "
                    "its controls' ORG hierarchy (upper_arm_fk.L Up = shoulder.L). Maya walks into hidden nodes: "
                    "bones hidden one by one (H) are still walked into, and an armature with controller tags "
                    "(Control > Tag As Controller) follows its controller hierarchy instead. Off: plain hierarchy",
        default=True)
    # PR #125 extras with no Maya counterpart (no Maya 2022-2027 viewport header has a mode picker or a pulse,
    # PR125_MAYA_LATER.md 4a): off by default.
    armature_mode_header: bpy.props.BoolProperty(
        name="Armature Mode Picker",
        description="Show an Object / Edit Joints / Pose Joints picker at the start of the main viewport header "
                    "while a skeleton is active (not part of Maya)",
        default=False)
    armature_mode_pulse: bpy.props.BoolProperty(
        name="Pulse Armature Mode",
        description="Softly pulse the viewport header's mode button while a skeleton is selected (not part of Maya)",
        default=False)
    # First start (docs/steps/step_42.md): welcome dialog shown once.
    welcome_done: bpy.props.BoolProperty(options={'HIDDEN'})
    # step 260 (#177): how often the welcome dialog opened without "Make Blender feel like Maya" being clicked
    welcome_shown: bpy.props.IntProperty(options={'HIDDEN'})

    start_in_maya_layout: bpy.props.BoolProperty(
        name="Start in Maya Layout",
        description="When Blender starts without a file in Maya mode, open a new scene in the Maya layout",
        default=False)

    # Step 138 (#63): Maya's UI doesn't come from the scene file: opening a file
    # shows the Maya layout (ui/scene_carry.py).
    maya_layout_on_open: bpy.props.BoolProperty(
        name="Keep Maya Layout When Opening Files",
        description="In Maya mode, a file opened in another layout is shown in the Maya layout (its own Maya layout "
                    "workspace when it has one). The file's other workspaces stay",
        default=True)

    lock_ortho_tumble: bpy.props.BoolProperty(
        name="Lock Tumble in Ortho Views",
        description="Alt+LMB does nothing in top/front/side orthographic views (Maya)",
        default=True)

    # Channel Box virtual slider.
    cb_speed: bpy.props.EnumProperty(
        name="Slider Speed", default='MEDIUM',
        items=(('SLOWEST', "Slowest", "x0.01"), ('SLOW', "Slow", "x0.1"),
               ('MEDIUM', "Medium", "x1"), ('FAST', "Fast", "x10")))
    cb_hyperbolic: bpy.props.BoolProperty(
        name="Hyperbolic", description="Value change accelerates with drag distance")

    # Graph Editor View > Snap Time To Selection (Maya optionVar snapTimeToSelection, default off; Alt+/ toggles it;
    # docs/steps/step_92.md).
    snap_time_to_selection: bpy.props.BoolProperty(
        name="Snap Time To Selection",
        description="Selecting keys in the Graph Editor moves the current time to the earliest selected key")
    # Graph Editor View > Auto Frame / Auto Frame Time / Curve Name (Maya factory: on / off / Never;
    # ui/graph_view.py, step 150).
    graph_auto_frame: bpy.props.BoolProperty(
        name="Auto Frame", default=True,
        description="When the selection changes, the Graph Editor frames the curves it shows (Maya View > Auto Frame)")
    # Dope Sheet View > Auto Frame (Maya displayAutoFitItem, factory off in the Dope Sheet; step 166)
    dope_auto_frame: bpy.props.BoolProperty(
        name="Dope Sheet Auto Frame",
        description="When the selection changes, the Dope Sheet frames all the keys it shows (Maya Dope Sheet "
                    "View > Auto Frame)")
    graph_auto_frame_time: bpy.props.BoolProperty(
        name="Auto Frame Time",
        description="When the selection changes, the Graph Editor frames the keys along the time axis only "
                    "(Maya View > Auto Frame Time; Auto Frame wins when both are on)")
    graph_curve_names: bpy.props.EnumProperty(
        name="Curve Name", default='NEVER',
        description="Write each curve's Maya name (pCube1.translateX) beside its last key in the Graph Editor",
        items=(('ALWAYS', "Always", "Name every curve"), ('NEVER', "Never", "No curve names"),
               ('ACTIVE', "Active Only", "Name the selected curves")))

    # Graph Editor depth 2 (step 198): View > Show Buffer Curves (factory off),
    # Curves > Default Tangent Weight (factory Non-weighted), Retime Tool Snap On Frame (factory off), Edit > Set Curve
    # Colors... rules (Maya curveRGBColor: attribute name -> colour; JSON list of [attribute, r, g, b]).
    graph_show_buffers: bpy.props.BoolProperty(
        name="Show Buffer Curves",
        description="Draw each buffered curve's snapshot behind it in the Graph Editor (Maya View > Show Buffer Curves)")
    default_tangent_weight: bpy.props.EnumProperty(
        name="Default Tangent Weight", default='NON_WEIGHTED',
        items=(('NON_WEIGHTED', "Non-weighted Tangents", "New curves the kit keys keep every tangent a third of "
                "the way to the next key"),
               ('WEIGHTED', "Weighted Tangents", "New curves the kit keys have tangents whose length can change")),
        description="Tangent weight of the curves the kit creates (Maya Curves > Default Tangent Weight)")
    retime_snap_on_frame: bpy.props.BoolProperty(
        name="Snap On Frame",
        description="Retime markers and their drags land on whole frames (Maya Retime Tool setting)")
    curve_color_rules: bpy.props.StringProperty(
        name="Curve Colors", default="",
        description="Colour rules by attribute name (Maya Edit > Set Curve Colors...)")

    # Script Editor History menu (Maya scriptEditorInfo optionVars; Maya 2022 factory states:
    # Batch Render Messages / Line numbers in errors on,
    # the rest off, History Output Show Both). Read by command_line/runner.py (step 216).
    se_batch_render_messages: bpy.props.BoolProperty(
        name="Batch Render Messages", default=True,
        description="Show batch rendering messages in the Script Editor history (Maya; the kit's batch render "
                    "writes its messages to the system console)")
    se_echo_all: bpy.props.BoolProperty(
        name="Echo All Commands",
        description="Write every command run from a menu, shelf or marking menu into the Script Editor history "
                    "(as the Python call the kit runs)")
    se_line_numbers: bpy.props.BoolProperty(
        name="Line numbers in errors", default=True,
        description="Error messages name the script line the error happened on")
    se_stack_trace: bpy.props.BoolProperty(
        name="Show Stack Trace",
        description="An error writes the whole Python traceback into the Script Editor history")
    se_suppress_results: bpy.props.BoolProperty(
        name="Suppress Command Results", description="No \"# Result:\" lines in the Script Editor history")
    se_suppress_info: bpy.props.BoolProperty(
        name="Suppress Info Messages", description="No info messages in the Script Editor history")
    se_suppress_warnings: bpy.props.BoolProperty(
        name="Suppress Warning Messages", description="No warning messages in the Script Editor history")
    se_suppress_errors: bpy.props.BoolProperty(
        name="Suppress Error Messages", description="No error messages in the Script Editor history")
    se_suppress_stack_window: bpy.props.BoolProperty(
        name="Suppress Stack Window",
        description="Maya: no stack trace window on error (Blender has no such window: kept for Maya's menu)")
    se_suppress_dup_vars: bpy.props.BoolProperty(
        name="Suppress Duplicate Variable Messages",
        description="Maya: no warnings about redeclared MEL variables (the kit runs no MEL: kept for Maya's menu)")
    se_history_output: bpy.props.EnumProperty(
        name="History Output", default='BOTH',
        items=(('BOTH', "Show Both", "Show both MEL and Python output in the history"),
               ('MEL', "Show MEL Only", "Show only MEL output in the history"),
               ('PYTHON', "Show Python Only", "Show only Python output in the history")))

    # Channel Box Channels > Sync Selection in Graph Editor / Sync Timeline Display (Maya optionVars, factory off;
    # Maya Channel Box Channels menu, ui/cb_channels.py, step 158).
    cb_sync_graph: bpy.props.BoolProperty(
        name="Sync Selection in Graph Editor",
        description="Selecting channels in the Channel Box selects their curves, and S keys only the selected channels")
    cb_sync_timeline: bpy.props.BoolProperty(
        name="Sync Timeline Display",
        description="The Time Slider shows only the keys of the channels selected in the Channel Box")
    # Graph Editor Select > Curve / Key / In Tangent / Out Tangent (Maya selectType pick masks, factory all on;
    # Maya Graph Editor Select menu, operators/graph_key_menu.py, step 157).
    graph_select_curve: bpy.props.BoolProperty(
        name="Curve", default=True, description="Clicking or dragging in the Graph Editor can pick curves")
    graph_select_key: bpy.props.BoolProperty(
        name="Key", default=True, description="Clicking or dragging in the Graph Editor can pick keys")
    graph_select_in: bpy.props.BoolProperty(
        name="In Tangent", default=True, description="Clicking or dragging in the Graph Editor can pick in tangents")
    graph_select_out: bpy.props.BoolProperty(
        name="Out Tangent", default=True, description="Clicking or dragging in the Graph Editor can pick out tangents")
    # Tangents > Default In / Out Tangent (Maya keyTangent -global, factory Auto / Auto):
    # the tangents of the keys the kit sets (S, Shift+W/E/R, Channel Box keys, Auto Key). Blender's own preference
    # is never written (#40).
    default_in_tangent: bpy.props.EnumProperty(
        name="Default In Tangent", default='AUTO',
        items=(('AUTO', "Auto", "Maya Auto: Blender Auto Clamped"), ('SPLINE', "Spline", "Blender Auto"),
               ('LINEAR', "Linear", ""), ('CLAMPED', "Clamped", "Blender Auto Clamped"),
               ('FLAT', "Flat", "Level handles"), ('PLATEAU', "Plateau", "Blender Auto Clamped")),
        description="In tangent of the keys the kit sets (Maya Tangents > Default In Tangent)")
    default_out_tangent: bpy.props.EnumProperty(
        name="Default Out Tangent", default='AUTO',
        items=(('AUTO', "Auto", "Maya Auto: Blender Auto Clamped"), ('SPLINE', "Spline", "Blender Auto"),
               ('LINEAR', "Linear", ""), ('CLAMPED', "Clamped", "Blender Auto Clamped"),
               ('FLAT', "Flat", "Level handles"), ('PLATEAU', "Plateau", "Blender Auto Clamped"),
               ('STEPPED', "Stepped", "Blender Constant")),
        description="Out tangent of the keys the kit sets (Maya Tangents > Default Out Tangent)")

    # Render > Test Resolution (Maya optionVar renderViewTestResolution, default Render Settings; step 152,
    # operators/render_batch.py): the size of Render Current Frame / Render Sequence.
    test_resolution: bpy.props.EnumProperty(
        name="Test Resolution",
        items=(("PANEL", "Camera Panel", "The size of the 3D View"),
               ("SETTINGS", "Render Settings", "The Render Settings size"),
               ("10", "10%", ""), ("25", "25%", ""), ("50", "50%", ""), ("75", "75%", ""),
               ("110", "110%", ""), ("125", "125%", ""), ("150", "150%", "")),
        default="SETTINGS",
        description="Maya Render > Test Resolution: the size of Render Current Frame / Render Sequence "
                    "(batch renders use the Render Settings size)")

    # Top UI (docs/steps/step_7.md; Maya Windows > UI Elements).
    maya_top_bar: bpy.props.BoolProperty(
        name="Maya Menu Bar",
        description="Top bar shows the Maya menus, menu sets and Status Line instead of "
                    "Blender's menus and workspace tabs (Blender menus stay as submenus)",
        default=True)
    # Maya view furniture (docs/steps/step_46.md), in views whose Blender navigation gizmo is off.
    maya_viewcube: bpy.props.BoolProperty(
        name="ViewCube", description="Maya ViewCube in the top right of the view (click a face to look "
                                     "along it)", default=True)
    maya_camera_names: bpy.props.BoolProperty(
        name="Camera Names", description="Maya camera name (persp, top -Y, ...) at the bottom of the view",
        default=True)
    def _update_selection_wire(self, context):
        for window in context.window_manager.windows:
            for area in window.screen.areas:
                if area.type == 'VIEW_3D':
                    area.tag_redraw()

    def _update_dock_colors(self, context):
        for window in context.window_manager.windows:
            for area in window.screen.areas:
                if area.type == 'PROPERTIES':
                    area.tag_redraw()

    # Maya wireframe on selected objects over shading (docs/steps/step_47.md).
    maya_selection_wire: bpy.props.BoolProperty(
        name="Selection Wireframe", description="Selected objects show their wireframe over shading, like "
                                                "Maya (lead light green, others white); in wireframe shading "
                                                "unselected objects are Maya's dark blue",
        default=True, update=_update_selection_wire)
    # Maya component colours in mesh edit mode (docs/steps/step_134_sel.md).
    maya_component_colors: bpy.props.BoolProperty(
        name="Component Colors", description="Mesh edit mode shows Maya's component colors: light blue edges, "
                                              "purple / yellow vertices, orange edges and stippled faces",
        default=True, update=_update_selection_wire)
    # Maya grey docks (step 195): the dock areas are lifted from the theme's Properties grey to Maya's #444444
    maya_dock_colors: bpy.props.BoolProperty(
        name="Dock Colors", description="The Channel Box / Attribute Editor / Modeling Toolkit docks are drawn "
                                        "in Maya's lighter grey",
        default=True, update=_update_dock_colors)
    maya_view_axis: bpy.props.BoolProperty(
        name="View Axis", description="Maya axis triad in the bottom left of the view", default=True)
    # Maya in-view messages (inViewMessageEnable 1; docs/steps/step_72.md).
    in_view_messages: bpy.props.BoolProperty(
        name="In-View Messages", description="Short fading messages in the view when you group, hide, toggle "
                                             "object/component selection (F8), soft select or Make Live, like Maya",
        default=True)
    maya_panel_menus: bpy.props.BoolProperty(
        name="Panel Menus",
        description="3D View headers show Maya's panel menus (View, Shading, Lighting, Show, Renderer, "
                    "Panels) instead of Blender's (Blender menus stay as a submenu)",
        default=True)
    menu_set: bpy.props.EnumProperty(
        name="Menu Set", description="Maya menu set: which menus follow the common ones",
        items=lambda self, context: _menu_set_items(), default=0)
    show_status_line: bpy.props.BoolProperty(
        name="Status Line", description="Maya Status Line icons in the top bar", default=True)
    # Maya layout parity (docs/steps/step_49.md).
    maya_toolbox: bpy.props.BoolProperty(
        name="Maya Tool Box", description="3D View toolbars show Maya's Tool Box in every mode: Select, Lasso, "
                                          "Paint Select, Move, Rotate, Scale, the last other tool and quick layout "
                                          "buttons (other tools stay on keys, menus and the shelf)",
        default=True, update=_update_selection_wire)
    show_command_line: bpy.props.BoolProperty(
        name="Command Line", description="Python command line in the status bar", default=True)
    show_help_line: bpy.props.BoolProperty(
        name="Help Line", description="Maya-style help for the active tool in the status bar",
        default=True)

    # Marking menus and hotbox (docs/steps/step_9.md).
    marking_menu_style: bpy.props.EnumProperty(
        name="Marking Menus",
        description="How Maya marking menus are shown",
        items=(('ENGINE', "Maya Engine", "Maya-style marking menus: drag, flick, in-place submenus, "
                                          "option boxes"),
               ('PIE', "Blender Pies", "Blender pie menus (8 slots, submenus open new pies)")),
        default='ENGINE')
    hotbox_style: bpy.props.EnumProperty(
        name="Hotbox Style",
        items=(('ALL', "Zones and Menu Rows", ""), ('ZONES', "Zones Only", ""),
               ('CENTER', "Center Zone Only", "")),
        default='ALL')
    hotbox_all_sets: bpy.props.BoolProperty(
        name="Show All Menu Sets", description="Hotbox shows the menus of every menu set, not only "
                                               "the current one (Maya's default, step 52)",
        default=True)

    # Shelf (docs/steps/step_12.md): current tab (factory file name or "USER:<name>").
    shelf_current: bpy.props.StringProperty(options={'HIDDEN'})

    # Maya project (operators/project.py, docs/steps/step_69.md): Blender has no projects, so the current
    # project folder and the Recent Projects list live here, like Maya keeps the workspace in its prefs.
    project_root: bpy.props.StringProperty(name="Current Project", subtype='DIR_PATH',
                                           description="Maya Workflow Kit project folder: file dialogs open in its scenes "
                                                       "folder, renders go to its images folder")
    recent_projects: bpy.props.StringProperty(options={'HIDDEN'})   # newline-separated, newest first
    # Maya option window values (operators/option_windows.py, step 103): Maya keeps them in optionVars.
    option_settings: bpy.props.StringProperty(options={'HIDDEN'})   # JSON {window key: {field: value}}

    # Scene concepts (docs/steps/step_10.md; docs/user_decisions.md #1, #2).
    parent_mode: bpy.props.EnumProperty(
        name="Parent Mode", description="What P (Parent) does to the child's values",
        items=(('MAYA', "Maya", "Keep the world position and recompute the local values "
                                "relative to the parent (parent inverse = identity)"),
               ('BLENDER', "Blender", "Keep the local values; Blender stores a hidden parent "
                                      "inverse matrix")),
        default='MAYA')
    maya_naming: bpy.props.BoolProperty(
        name="Maya Naming", description="Create menu names objects pCube1 / pCubeShape1 "
                                        "(otherwise Blender names)",
        default=True)
    # Maya optionVar manipSmartDuplicate 1.
    smart_duplicate: bpy.props.BoolProperty(
        name="Smart Duplicate", description="Shift+drag a Move/Rotate/Scale manipulator "
                                            "duplicates the selection and transforms the copy",
        default=True)
    # Maya optionVar manipSmartExtrude 1.
    smart_extrude: bpy.props.BoolProperty(
        name="Smart Extrude", description="Shift+drag a Move/Rotate/Scale manipulator on faces "
                                          "or edges extrudes them and transforms the new ones",
        default=True)

    def _update_units(self, context):
        from .operators import units
        if self.maya_units:
            units.apply_file()
        else:
            units.restore_file()

    maya_units: bpy.props.BoolProperty(
        name="Maya Units (1 BU = 1 cm)",
        description="Scene unit scale 0.01 with centimetres and Maya camera clipping "
                    "(0.1 - 10000) for this file and new files. Physics, light falloff and "
                    "importers assume 1 BU = 1 m. Turning it off restores the previous values",
        default=False, update=_update_units)

    def draw(self, context):
        layout = self.layout
        draw_about(layout)
        box = layout.box()
        row = box.row()
        row.scale_y = 1.6
        if self.maya_mode:
            row.operator("allset.switch_mode", text="Switch to Blender", icon='BLENDER').mode = 'BLENDER'
        else:
            # Nothing is changed until the user asks for it (step 115): the same dialog as the first start.
            row.operator("allset.welcome", text="Make Blender feel like Maya", icon='EVENT_M')
            row.operator("allset.switch_mode", text="Maya Mode Only").mode = 'MAYA'
        box.label(text="Ctrl+Alt+Shift+M or the Maya / Blender switch at the right of the top bar switches at any time. "
                       "Blender's own preferences are never changed.", icon='INFO')
        hint = undo_hint(context)
        emulate = emulate_hint(context)
        if hint or emulate:
            box.label(text="Recommended setup", icon='LOOP_BACK')
        for text in (hint, emulate):
            if text:
                _wrapped(box, text)
        if not self.maya_mode:
            return
        box = layout.box()
        row = box.row()
        row.scale_y = 1.4
        row.operator("allset.new_maya_scene", icon='FILE_NEW')
        row.operator("allset.maya_layout_here", icon='WORKSPACE')
        box.prop(self, "start_in_maya_layout")
        box.prop(self, "maya_layout_on_open")
        col = layout.column()
        col.prop(self, "lock_ortho_tumble")
        col.prop(self, "select_through")
        col.prop(self, "preselect_highlight")
        col.prop(self, "select_priority")
        col.prop(self, "pick_walk_skip_hidden_collections")
        col.prop(self, "armature_mode_header")
        col.prop(self, "armature_mode_pulse")

        box = layout.box()
        box.label(text="Scene Concepts")
        row = box.row()
        row.prop(self, "parent_mode")
        row.prop(self, "maya_naming")
        row = box.row()
        row.prop(self, "maya_units")
        row.prop(self, "smart_duplicate")
        row.prop(self, "smart_extrude")
        box.prop(self, "project_root")

        box = layout.box()
        box.label(text="UI Elements")
        row = box.row()
        row.prop(self, "maya_top_bar")
        row.prop(self, "maya_panel_menus")
        row.prop(self, "menu_set", text="")
        row = box.row()
        row.prop(self, "marking_menu_style")
        row.prop(self, "hotbox_style", text="")
        row.prop(self, "hotbox_all_sets")
        row = box.row()
        row.prop(self, "show_status_line")
        row.prop(self, "show_command_line")
        row.prop(self, "show_help_line")
        row = box.row()
        row.prop(self, "maya_toolbox")
        row = box.row()
        row.prop(self, "maya_viewcube")
        row.prop(self, "maya_camera_names")
        row.prop(self, "maya_view_axis")
        row.prop(self, "maya_selection_wire")
        row.prop(self, "maya_component_colors")
        box.prop(self, "maya_dock_colors")
        box.prop(self, "in_view_messages")


# Step 111 (decision 12, docs/distribution/naming.md, docs/distribution/extensions_audit.md 「필수 고지문」)
# Undo depth (step 141, B6): Maya's undo queue is infinite, Blender keeps 32 steps
# by default and the kit may not write Blender's preferences (step 130), so the kit only tells the user (delegated).
UNDO_RECOMMENDED = 256      # Blender's maximum
UNDO_HINT = ("Maya's undo is unlimited; Blender keeps {steps} steps. For long Maya-style sessions raise Edit > "
             "Preferences > System > Memory & Limits > Undo Steps to 256 (Blender's maximum).")


EMULATE_HINT = ("Emulate 3 Button Mouse is on (Preferences > Input): Blender turns Alt+left drag into a middle drag "
                "before any key map sees it, so Maya's Alt+LMB tumble can't work. Turn it off for Maya navigation "
                "(Alt+MMB track and Alt+RMB dolly work either way).")


def emulate_hint(context=None):
    """Step 260 (#177): the Emulate 3 Button Mouse hint while that Blender preference is on, else None (read only:
    the kit never writes Blender's preferences, step 130). Blender converts Alt+LMB into a plain MMB without Alt
    (wm_event_system.cc wm_eventemulation, docs/steps/step_260_prep.md)."""
    inputs = (context or bpy.context).preferences.inputs
    return EMULATE_HINT if getattr(inputs, "use_mouse_emulate_3_button", False) else None


def undo_hint(context=None):
    """The undo-depth hint while Blender's Undo Steps is below 256, else None (read only, nothing is written)."""
    steps = (context or bpy.context).preferences.edit.undo_steps
    return UNDO_HINT.format(steps=steps) if steps < UNDO_RECOMMENDED else None


ABOUT = ("Other tools give you one piece at a time: a pivot, a keymap, a shelf, a theme. Maya Workflow Kit brings the whole Maya "
         "workflow into Blender in one install: hotbox and marking menus, Channel Box, Attribute Editor, Outliner, "
         "menu sets, shelves, option windows, Freeze / History concepts, centimetre units and pCube1-style naming.")
TRADEMARK_NOTICE = (
    "Autodesk and Maya are registered trademarks or trademarks of Autodesk, Inc., and/or its subsidiaries and/or "
    "affiliates in the USA and/or other countries. This project is an independent, free and open-source (GPL) "
    "extension for Blender. It is not affiliated with, endorsed by, sponsored by, or otherwise approved by Autodesk, "
    "Inc. or the Blender Foundation. It contains no Autodesk code, icons, MEL scripts or other Autodesk assets; "
    "references to Maya are solely to describe compatibility and the intended audience.")


def _wrapped(layout, text, width=110):
    line = ""
    col = layout.column(align=True)
    for word in text.split():
        if line and len(line) + 1 + len(word) > width:
            col.label(text=line)
            line = word
        else:
            line = f"{line} {word}" if line else word
    if line:
        col.label(text=line)


def draw_about(layout):
    """The About box of the preferences: what Maya Workflow Kit is, and the trademark notice."""
    box = layout.box()
    box.label(text="Maya Workflow Kit: the whole Maya workflow in Blender, in one install", icon='INFO')
    _wrapped(box, ABOUT)
    sub = box.column()
    sub.scale_y = 0.8
    sub.enabled = False
    _wrapped(sub, TRADEMARK_NOTICE)


_SET_ITEMS = []


def _menu_set_items():
    # Kept in a module list: Blender needs the enum item strings to stay referenced.
    if not _SET_ITEMS:
        from .ui import menu_sets
        _SET_ITEMS.extend(menu_sets.SET_ITEMS)
    return _SET_ITEMS


classes = (ALLSET_AddonPreferences,)

register, unregister = bpy.utils.register_classes_factory(classes)


def get(context=None):
    """Return this add-on's preferences instance."""
    context = context or bpy.context
    return context.preferences.addons[__package__].preferences
