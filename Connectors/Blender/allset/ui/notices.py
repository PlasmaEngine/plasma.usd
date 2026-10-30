# SPDX-License-Identifier: GPL-3.0-or-later
"""「Not in Blender」 notices: the near substitute + one line, for Maya menu items (docs/PLAN.md 「Maya 워크플로우
계획」 W3, docs/steps/step_54.md 「계획」 (b)).

One table for every place a Maya item has no Blender equivalent, or a different one: the item's tooltip
(greyed items: ``allset.menu_unavailable``; items with a substitute: ``allset.menu_item``) and the Help
Line, which shows the notice while the tooltip is up — Maya's Help Line shows a menu item's annotation the same
way. Keys are Maya menu label paths (``ui/menu_sets.iter_items``).
"""

import time

import bpy

from .. import live_refs

# path: (substitute in Blender, one line). Paths with an action in menu_map.MAP run that substitute.
NOTICES = {
    "Mesh Display/Assign Existing Set/No bakesets defined": ("Render Properties > Bake", "Maya lists the scene's bake sets here; Blender bakes from Render Properties > Bake (Cycles)."),
    # step 159
    **{f"Skin/{item}": ("Bind Skin / Paint Skin Weights", "Blender has no volume capsules to drag: Bind Skin gives Maya's "
                        "closest-distance weights, then paint them (Skin > Paint Skin Weights).")
       for item in ("Interactive Bind Skin", "Interactive Bind Skin Tool")},
    # Windows > Settings/Preferences (step 120)
    "Windows/Settings/Preferences/Marking Menu Editor": (
        "Marking menus", "The marking menus are the kit's Maya layouts; Blender has no editor for them "
        "(Preferences > Keymap edits which key opens which menu)."),
    "Windows/Settings/Preferences/Shelf Editor": (
        "Shelves", "The shelves are Maya's factory shelves; there is no shelf editor (right-click a Blender "
        "button > Add to Quick Favorites keeps your own commands)."),
    "Windows/Settings/Preferences/Panel Editor": (
        "Workspaces", "Blender saves panel layouts as workspaces: drag area edges and save the file or "
        "use Windows > Workspaces."),
    # Pivot / snapping (step 59)
    "Modify/Make Live": (
        "Face snapping", "Blender snaps onto every surface, not only the live one: Make Live turns face "
        "snapping on and names the surface in the Status Line."),
    "Modify/Bake Pivot": (
        "Pivot orientation", "Blender has one pivot (the origin), so only the pivot orientation is baked into "
        "Rotate; the position already is the object's pivot."),
    # Animation (W3: Time Editor / Trax -> NLA; Animation Layers real since step 188, Character Sets since step 187)
    "Windows/Animation Editors/Trax Editor": (
        "NLA Editor", "Blender has no Trax: clips are actions laid out as NLA strips."),
    "Windows/Animation Editors/Time Editor": (
        "NLA Editor", "Blender has no Time Editor: clips are actions laid out as NLA strips."),
    # Playback (step 56): Blender playback always loops
    **{f"Playback/Playback Looping/{kind}": (
        "Continuous", "Blender playback always loops (no Once / Oscillate).") for kind in ("Once", "Oscillate")},
    # Sets (step 96): kit sets are collections outside the scene; Blender has no exclusive sets
    "Create/Sets/Partition": (
        "Set", "Blender has no partitions (sets whose members can't be in another set of the partition): use Create > Sets > Set."),
    # Dependency graph editors (W3: Node Editor -> none, Hypergraph -> Outliner)
    "Windows/Node Editor": (
        "Shader Editor", "Blender has no node graph of attribute connections: attribute links are drivers "
        "(Expression Editor), material nodes are in the Shader Editor (Hypershade)."),
    "Windows/General Editors/Hypergraph: Hierarchy": (
        "Outliner", "Blender has no Hypergraph: the Outliner shows the hierarchy."),
    "Windows/General Editors/Hypergraph: Connections": (
        "Drivers", "Blender has no connection graph: attribute links are drivers (Expression Editor)."),
    "Windows/General Editors/Namespace Editor": (
        "ns: name prefix", "Blender has no namespaces: Maya Workflow Kit keeps them as a \"ns:\" prefix in names."),
    # Rendering (W3: Light Editor -> kit light list, step 67)
    "Windows/Rendering Editors/Light Editor": (
        "kit Light Editor", "Blender has no Light Editor: Maya Workflow Kit lists the scene lights in a 3D View sidebar tab "
        "(Exposure is part of Power)."),
    "Windows/Rendering Editors/Hypershade": (
        "Shader Editor + kit list", "Blender has no Hypershade: the Shader Editor is the graph, its Hypershade sidebar "
        "tab the browser (add nodes with Shift+A)."),
    # step 152: mapped (operators/render_batch.py); the tooltip says how Blender does it
    "Render/Batch Render": (
        "Background Blender", "Blender has no batch render queue: Maya Workflow Kit renders a copy of the scene in "
        "a separate background Blender (blender -b ... -a)."),
    # step 109: mapped items whose Blender result differs (docs/steps/step_109.md)
    "Create/NURBS Primitives/Square": (
        "Four NURBS curves", "Blender has no NURBS square: four straight NURBS curves under an empty, like Maya's group."),
    "Create/NURBS Primitives/Cube": (
        "Six NURBS patches", "Blender has no NURBS cube: six NURBS surface patches under an empty, like Maya's group."),
    "Create/NURBS Primitives/Cone": (
        "NURBS cylinder", "Blender has no NURBS cone: a NURBS cylinder with its top row of points closed."),
    "Create/Curve Tools/Pencil Curve Tool": (
        "Curve Draw tool", "Blender's Draw tool on a new curve in Edit Mode: drag to draw (on the 3D cursor's plane)."),
    "Create/Sweep Mesh": (
        "Curve bevel, converted", "Blender makes the mesh from a copy of the curve with a round bevel (8 sides); no live "
        "profile options."),
    "Curves/Add Points Tool": (
        "Extrude (E)", "Blender extrudes the curve's end points: move the mouse, click to place the new point."),
    "Curves/Offset/Offset Curve": (
        "Poly curve", "Blender's offset copy is a poly curve through the moved points, not a fitted NURBS curve."),
    "Surfaces/Planar": (
        "Filled 2D curve", "Blender fills closed 2D curves: a copy of the curve, flattened onto its own XY plane."),
    "Surfaces/Bevel Plus": (
        "Curve extrude + bevel, converted", "Blender builds a mesh from the filled curve, extruded and bevelled both ways around "
        "the curve's plane."),
    "Mesh Tools/Sculpting Tools/Spray Tool": (
        "Draw Sharp brush", "Blender has no stamp spray brush: Draw Sharp is the closest (also sculpt Ctrl+8)."),
    "Mesh Tools/Sculpting Tools/Repeat Tool": (
        "Layer brush", "Blender has no repeating stamp brush: Layer is the closest (also sculpt Ctrl+9)."),
    "Fluids/Add/Edit Contents/Emit from Object": (
        "Quick Smoke", "Blender makes the selected meshes smoke flows inside a new Mantaflow domain."),
    "Fluids/Make Collide": (
        "Fluid effector", "Blender gives the selected meshes a Fluid modifier of type Effector."),
    "nCloth/Display Input Mesh": (
        "Cloth modifier off", "Blender shows the input mesh by turning the Cloth modifier off in the viewport."),
    "nCloth/Display Current Mesh": (
        "Cloth modifier on", "Blender shows the simulated mesh with the Cloth modifier on in the viewport."),
    "nHair/Paint Hair Follicles": (
        "Hair Sculpt mode", "Blender hair is a Curves object groomed in Sculpt mode: on the selected mesh the kit adds "
        "an empty hair object (or picks its hair) and switches to it; paint follicles with the Add brush."),
    # step 110: mapped items whose Blender result differs (docs/steps/step_110.md)
    "Mesh Tools/Make Hole": (
        "Bridged loops", "Blender faces can't have holes: the stamp outline is bridged to the face's border with "
        "quads (select the face to cut, then the stamp face)."),
    "Skeleton/Set Preferred Angle": (
        "Stored rotation", "Blender's IK bends from the rest pose: the angle is stored for Assume Preferred Angle, "
        "the solver doesn't read it."),
    "Skeleton/Create IK Spline Handle": (
        "Spline IK constraint", "Blender makes a Spline IK constraint on the end bone and the curve curve1; there is no "
        "ikHandle / effector node."),
    # step 223 L14 (operators/ik_fk.py)
    "Skeleton/Enable IK/FK Control": (
        "Always on", "Blender always lets you rotate joints in an IK chain: the rotation shows where the handle's "
        "Ik Blend is 0 (Key > IK/FK Keys)."),
    "Key/IK/FK Keys/Set IK/FK Key": (
        "Ik Blend + joint keys", "Keys the handle (Ik Blend included) and the chain joints' solved rotation; Blender "
        "has no handle Offset channel to key."),
    "Deform/Cluster": (
        "Hook modifier", "Blender's cluster is a Hook modifier driven by the clusterNHandle empty."),
    "Deform/Soft Modification Tool": (
        "Hook with falloff", "Blender's soft modification is a Hook modifier on the whole mesh with Maya's falloff "
        "(radius 5, smooth), driven by softModNHandle."),
    "Render/IPR Render Current Frame": (
        "Viewport Rendered", "Blender's IPR is the viewport in Rendered shading (Render Region: Ctrl+B)."),
    # FX (step 68, decision 10)
    **{path: ("one-point mesh emitter", "Blender particles come from a mesh: Maya Workflow Kit makes a one-point mesh emitter1 "
              "with the particles, sized in Maya units.")
       for path in ("nParticles/Create Emitter", "nParticles/Create Emitter#2")},
    "nCloth/Create nCloth": (
        "Cloth modifier", "Blender cloth simulates real sizes: Maya Workflow Kit scales collision distances, stiffness and gravity "
        "so one unit moves like Maya's (nucleus Space Scale in the Attribute Editor)."),
    **{f"nCache/{item}": (
        "Baked caches", "Blender always plays a baked cache: Delete Cache to simulate again.")
       for item in ("Enable All Caches on Selected", "Disable All Caches on Selected")},
    # Lights (PLAN W5; step 137 operators/lights.py)
    **{f"Create/Lights/{kind} Light": (
        f"{kind} light", "Maya Decay Rate No Decay renders in Cycles; Blender EEVEE and the viewport get darker with the "
        "square of the distance, the power matching Maya intensity 1 at 10 units.") for kind in ("Point", "Spot")},
    "Create/Lights/Area Light": (
        "Area light", "Blender's area light falls off like Maya's; its power matches Maya intensity 1 (2 x 2 units)."),
    # step 214 (operators/maya_lights.py)
    "Create/Lights/Ambient Light": (
        "World + point light", "Blender has no ambient light: the World colour gives the flat part and a point light "
        "the Ambient Shade part; surfaces turned away from the light get more light than in Maya."),
    "Create/Lights/Volume Light": (
        "Point light cut at the sphere", "Blender has no volume light: a point light at the centre cut at the "
        "sphere (the object's scale); Cycles renders Maya's falloff, EEVEE and the viewport approximate it."),
    # Rigging (W3: HumanIK / Quick Rig -> Rigify)
    **{path: ("Rigify", "Blender has no HumanIK / Quick Rig: the bundled Rigify add-on builds a rig from a "
                        "meta-rig fitted to the mesh (Generate Rig).")
       for path in ("Skeleton/HumanIK", "Skeleton/Quick Rig", "Windows/Animation Editors/HumanIK",
                    "Windows/Animation Editors/Quick Rig")},
    # Scene / files (W3: Reference -> link + library override, Set Project -> relative paths)
    # Scene management (step 69, operators/project.py)
    "File/Create Reference...": (
        "Link + Library Override", "Blender references are linked .blend files: Maya Workflow Kit makes them editable "
        "(library override) and prefixes the names with the namespace (ns:name)."),
    "File/Set Project...": (
        "Maya Workflow Kit project", "Blender has no projects: Maya Workflow Kit keeps this folder, opens file dialogs in its scenes "
        "folder and renders to its images folder."),
    "File/Project Window": (
        "Maya Workflow Kit project", "Blender has no projects: Maya Workflow Kit makes Maya's project folders and keeps the folder "
        "as your project."),
    # UV (step 66): Blender cuts / sews UVs in the UV Editor only
    # Modeling (step 64): the Crease Tool works, with Blender's 0-1 crease underneath
    "Mesh Tools/Crease Tool": (
        "Edge crease", "Blender stores creases as 0-1 (1 = fully sharp). The kit shows Maya's value (sharp for that "
        "many smooth levels) and stores sqrt(value / 10); see it with 3 (smooth mesh preview)."),
    # FX (W3: nucleus -> scene gravity + per-object settings; Gravity field)
    "Modify/Evaluate Nodes/nucleuses": (
        "Scene gravity", "Blender has no nucleus node: gravity is in Scene properties, solver settings are per object."),
    "Fields/Solvers/Gravity": (
        "Wind field", "Blender has no gravity field: a wind field blowing down; the scene's gravity is global "
        "(Scene properties)."),
}


# Step 101 (docs/steps/step_101.md 「계획」 2): mapped items whose Blender result differs from Maya's.
NOTICES.update({
    "File/Archive Scene": (
        "Pack Resources", "Blender packs textures and sounds into the .blend instead of zipping the scene folder."),
    **{f"File/{item}": ("FBX export", "Blender has no game-engine live links: export FBX (Unity / Unreal read it).")
       for item in ("Send To Unity", "Send To Unreal")},
    "File/Game Exporter": ("Kit Game Exporter", "Blender writes each clip as an FBX take (an NLA strip while it writes); "
                           "Maya's extra Take 001 and repeated clips in later clip files are left out."),
    "File/View Sequence...": (
        "Play Rendered Animation", "Blender plays the render output frames (Render > View Animation)."),
    "Edit/Delete All by Type/Shading Groups and Materials": (
        "Purge Unused Data", "Blender removes unused materials (and other unused data) with File > Clean Up > Purge."),
    "Modify/Replace Objects": (
        "Link Object Data", "Blender gives the selected objects the active object's mesh (Object > Link/Transfer Data)."),
    # step 168: Evaluate Nodes items stand for Blender data
    "Modify/Evaluate Nodes/IK Solvers": (
        "IK constraints", "Blender mutes the IK constraints (Maya switches the IK solve off); on again unmutes them."),
    "Modify/Evaluate Nodes/Expressions": (
        "Drivers", "Blender mutes its drivers (Maya expressions); Set Driven Key curves keep working."),
    "Modify/Evaluate Nodes/nRigids": ("Collision", "Blender switches the colliders' Collision off."),
    "Modify/Evaluate Nodes/Rigid Bodies": ("Rigid Body World", "Blender switches the scene's Rigid Body World off."),
    "Modify/Evaluate Nodes/Dynamic Constraints": (
        "No nConstraints", "Blender has no nucleus constraints: rigid body constraints switch off with Rigid Bodies."),
    "Modify/Evaluate Nodes/Global Stitch": ("No stitching", "Blender has no NURBS surface stitching to switch off."),
    "Modify/Transformation Tools/Show Manipulator Tool": (
        "Default Object Manipulator", "Blender has no per-node manipulators: the object's Default Object "
        "Manipulator tool, else the Transform tool."),
    "Modify/Align Tool": (
        "Align popup", "Blender draws no align icons on the bounding box: pick the axis and Min / Mid / Max in a "
        "popup; the last selected object stays."),
    "Modify/Convert/Smooth Mesh Preview to Polygons": (
        "Apply the preview", "Blender applies the smooth preview Subsurf to the base mesh: deformers before it (skin, "
        "lattice) keep working on the denser mesh."),
    "Playback/Playback Looping/Continuous": (
        "Continuous", "Blender playback always loops: Continuous is already on."),
    **{f"Display/Heads Up Display/{item} Details": (
        "Statistics overlay", "Blender's Statistics overlay shows the object and selection counts.")
       for item in ("Object", "Select")},        # Maya 2026 label "Select Details" (step 219)
    "Display/Toggle Show/Hide": (
        "Maya visibility toggle", "Blender: same as h (selected objects shown / hidden)."),
    **{f"Mesh Tools/Sculpting Tools/{maya} Tool": (brush, f"Blender has no {maya} brush: the {brush} sculpt brush "
                                                          "is the closest.")
       for maya, brush in (("Foamy", "Clay"), ("Wax", "Clay Strips"), ("Knife", "Crease Sharp"),
                           ("Amplify", "Sharpen"), ("Freeze", "Mask"))},
    "Deform/Cluster": ("Hook", "Blender's cluster1 is a Hook modifier driven by cluster1Handle; its weights are the "
                       "vertex group cluster1."),
    # step 144 (#34, docs/steps/step_144_deform.md): the Deform menu's Blender constructs and the Maya deformers
    # Blender has none of
    "Deform/Nonlinear/Sine": ("Wave modifier", "Blender has no sine deformer: sine1 is a Wave modifier along X "
                              "around sine1Handle (Blender's wave shape, no bounds)."),
    "Deform/Lattice": ("Lattice modifier", "Blender: ffd1Lattice drives the Lattice modifier ffd1; the Hook ffd1Base "
                       "in front of it carries the points when the lattice object moves (Maya's base lattice)."),
    "Deform/Wrap": ("Surface Deform", "Blender: wrap1 is a Surface Deform modifier bound to the influence (shape), "
                    "the Hook wrap1Base after it follows the influence's transform."),
    "Deform/Proximity Wrap": ("Surface Deform", "Blender: proximityWrap1 is a Surface Deform modifier bound to the "
                              "driver (Maya's Surface wrap mode)."),
    "Deform/Wire": ("Geometry Nodes wire", "Blender has no wire deformer: wire1 is a Geometry Nodes modifier with "
                    "Maya's wire rule (dropoff, rotation, dropoff locators); select the objects, then the curve."),
    "Deform/Wire#2/Add Holder": ("No holders", "Blender's kit wire has no holder curves: limit the wire with its "
                                 "Dropoff distance or paint its weights (Paint Weights > Wire)."),
    **{f"Deform/ShrinkWrap#2/{item}": ("No inner object", "Blender's Shrinkwrap has no inner object: Projection "
                                       "Toward Inner Object projects along the inverted normals.")
       for item in ("Set Inner Object", "Remove Inner Object")},
    "Deform/Point On Curve": ("Hook", "Blender hooks the selected control point to locator1 (Maya fits the curve "
                              "through the point by least squares)."),
    "Deform/Wrinkle": ("Sculpt mode", "Blender has no wrinkle deformer: sculpt the wrinkles (Crease brush) or use a "
                       "Displace modifier (Deform > Texture)."),
    **{f"Deform/Paint Weights/{kind}": (
        "Weight Paint", "Blender paints the deformer's vertex group in Weight Paint mode (the group of the "
        "deformer's name).")
       for kind in ("Blend Shape", "Cluster", "Delta Mush", "Proximity Wrap", "Lattice", "ShrinkWrap",
                    "Nonlinear", "Texture Deformer", "Wire")},
    **{f"Constrain/{item}": ("Shrinkwrap constraint", "Blender sticks an object to a surface with a Shrinkwrap "
                                                     "constraint (Nearest Surface Point).")
       for item in ("Closest Point", "Point On Poly")},
    "Constrain/Rivet": ("Vertex parent", "Blender: select the object, then vertices of the mesh, Vertex Parent (Ctrl+P)."),
    "Toon/Assign Outline": ("Line Art", "Blender draws toon outlines with a Line Art Grease Pencil object."),
    "Create/Cameras/Stereo Camera": (
        "Stereoscopy", "Blender stereo = a camera with Render > Stereoscopy (Views) on, no stereo rig."),
    "Audio/Import Audio Clip to Time Editor...": (
        "Sound strip", "Blender has no Time Editor: the sound becomes a Video Sequencer strip."),
    **{f"Cache/Geometry Cache/{item}": ("Alembic", "Blender writes / reads geometry caches as Alembic (.abc) "
                                                   "files, played by a Mesh Sequence Cache modifier.")
       for item in ("Create New Cache", "Import Cache...", "Export Cache")},
    **{f"Cache/GPU Cache/{item}": ("Alembic", "Blender has no GPU cache node: GPU caches are Alembic files.")
       for item in ("Import...", "Export All...", "Export Selection...")},
    "Help/Maya Help": ("Blender Manual", "Blender's manual opens in the web browser."),
    "Help/About Maya": ("About Blender", "Blender's About shows its version (Maya Workflow Kit is an extension)."),
})

# Greyed items (no Blender action in Maya Workflow Kit) by Maya menu path prefix: (closest Blender feature, one line).
# notices.category picks the longest matching prefix; tests/test_step101_menu_honesty.py checks that every
# greyed menu item has a notice and that every rule here matches at least one greyed item.
# (Create Description... / Create Interactive Groom Splines are mapped to a new hair Curves object.)
_XGEN_ITEMS = ("XGen Editor", "XGen Library", "Import Collections or Descriptions...",
               "Export Collections or Descriptions...", "Export Selection as Archive(s)...",
               "Batch Convert Scenes to Archives...", "Convert XGen Primitives to Polygons...",
               "Convert to Interactive Groom...", "Import Preset...", "Export as Preset...",
               "Interactive Groom Editor", "Interactive Grooming Tools", "Cache",
               "Convert Interactive Groom to Polygons...", "Display HUD")
CATEGORIES = {
    # whole Maya-only systems
    "Surfaces": ("NURBS surface edit mode", "Blender's NURBS surfaces can't be trimmed and have no Fillet, Stitch "
                 "or surface Booleans (Maya Workflow Kit builds Loft, Revolve, Extrude, Birail 1, Boundary, Square, "
                 "Attach, Detach, Rebuild and Reverse as NURBS surfaces): edit CVs in Edit Mode or model with "
                 "polygons."),
    "Generate": ("Grease Pencil / Geometry Nodes", "Blender has no Paint Effects: draw strokes with Grease Pencil, "
                 "scatter geometry with Geometry Nodes."),
    **{f"Generate/{item}": ("Hair curves", "Blender has no XGen: hair is a Curves object groomed in Sculpt mode "
                            "with Geometry Nodes hair modifiers.") for item in _XGEN_ITEMS},
    "Generate/Character Generator": ("none", "Blender has no character web service: model or import the character."),
    "Generate/ReCap": ("Import", "Blender has no ReCap link: import point clouds / meshes from files (PLY, OBJ)."),
    "nHair": ("Hair curves", "Blender hair = Curves objects groomed in Sculpt mode; there are no follicles or "
              "nHair dynamics (use the Hair Dynamics node group)."),
    "nConstraint": ("Cloth pin group / Rigid Body Constraint", "Blender has no nConstraints: pin cloth with a "
                    "vertex group (Cloth > Shape > Pin Group), connect rigid bodies with Rigid Body Constraints."),
    "nCloth": ("Cloth modifier", "Blender cloth is a Cloth modifier (Physics properties): rest shape, output space "
               "and texture maps are not separate nodes."),
    "nCache": ("Physics cache (Bake)", "Blender caches each simulation in its Physics properties (Bake / Free "
               "Bake): merging, replacing or editing cache frames isn't available."),
    "nParticles": ("Particle system", "Blender particles are a particle system on a mesh: instancing = Render As "
                   "Object / Collection; there are no goals, sprite wizard or collision events."),
    "Fluids": ("Mantaflow fluid", "Blender fluids are Mantaflow domains (Physics > Fluid): no 2D containers, "
               "contents painting, ponds or boats; Ocean is a modifier."),
    "Fields/Solvers": ("Force fields / Rigid Body World", "Blender fields are Force Field objects; rigid body "
                       "solvers, keys and initial states are in Scene > Rigid Body World and each Physics tab."),
    "Effects": ("Quick Effects", "Blender's Object > Quick Effects has Smoke, Fire, Liquid and Explode; Fireworks, "
                "Lightning, Shatter and Flow aren't built in (Cell Fracture extension for Shatter)."),
    "Toon": ("Line Art / Freestyle", "Blender toon lines are Line Art (Grease Pencil) or Freestyle; line attributes "
             "are Line Art modifiers, not painted maps."),
    "Toon/Assign Fill Shader": ("Shader to RGB material", "Blender toon fills are materials with Shader to RGB + a "
                                "Color Ramp (EEVEE)."),
    "Stereo": ("Stereoscopy (Views)", "Blender stereo = Render > Stereoscopy with the camera's Stereoscopy "
               "settings: there are no stereo rigs or camera links."),
    "Audio": ("Sound strips", "Blender sounds are Video Sequencer strips (waveforms show in the Sequencer); "
              "No sounds available is greyed in Maya too."),
    "Cache/Geometry Cache": ("Alembic / Mesh Sequence Cache", "Blender plays geometry caches as Alembic files in a "
                             "Mesh Sequence Cache modifier: replacing, merging or editing cache frames isn't available."),
    "Cache/Alembic Cache": ("Alembic import", "Blender's Alembic import already keeps the file linked (Mesh Sequence "
                            "Cache); replace = import again, About = the Blender manual."),
    "Cache/GPU Cache": ("Cache File properties", "Blender has no GPU cache node: reload an imported Alembic from its "
                        "Cache File properties."),
    "Help": ("Blender Help menu", "Blender has no Autodesk account, product research or analytics services; "
             "Blender's Help menu has the manual and bug reports."),
    # step 197: character sets are real (operators/character_sets.py); step 217: controller tags too
    # (operators/controllers.py); what is left is HumanIK
    "Control": ("Bone collections", "Blender has no HumanIK control rigs, character mapping or redirection: bone "
                "collections group the controls. Character sets: Control > Create Character Set."),
    # rigging / animation
    "Key": ("Copy Transforms constraints", "Blender has no character mapping or redirection: retarget with Copy "
            "Transforms constraints. Character sets: Key > Create Character Set."),
    # step 223: Set IK/FK Key / Enable IK Solver / Move IK to FK work (operators/ik_fk.py); Connect to IK/FK is left
    "Key/IK": ("Ik Blend property", "Blender has no Connect to IK/FK: drive the handle's Ik Blend from a control "
               "with Key > Set Driven Key or a driver."),
    "Key/Scene Time Warp": ("NLA strip time", "Blender has no scene time warp: remap time per action with an NLA "
                            "strip's Scale / Repeat or a Time Offset modifier."),
    "Skeleton": ("Armature edit / pose mode", "Blender: IK handles are IK constraints (switch with Influence), "
                 "joints are bone heads edited in armature edit mode."),
    "Deform": ("Modifiers", "Blender deformers are modifiers (Properties > Modifiers) with vertex-group weights: no "
               "deformer sets or tweak nodes."),
    "Visualize": ("Motion Paths", "Blender shows motion with Motion Paths (Object properties); there is no turntable "
                  "camera rig or animated sweep surface (the kit has Animation Snapshot and ghosting)."),
    "Playback": ("Timeline Playback popover", "Blender's playback options are in the Timeline's Playback popover; "
                 "there is no cached-playback toggle or stepped preview."),
    # rendering
    "Render": ("Render properties", "Blender's render settings are in Properties > Render / Output: render "
               "diagnostics, Backburner and NURBS tessellation have no counterpart."),
    "Texturing": ("Shader Editor / Texture Paint", "Blender has no PSD networks or texture reference objects: "
                  "textures are image nodes in the Shader Editor, painted in Texture Paint mode."),
    "Lighting/Shading": ("Cycles Bake", "Blender transfers maps between meshes with Cycles Bake (Render "
                         "properties > Bake, Selected to Active)."),
    # modeling
    **{f"Mesh Display/{sub}": ("Color Attribute node", "Blender mixes vertex colors into materials with a Color "
                               "Attribute node in the Shader Editor, not a per-object setting.")
       for sub in ("Color Material Channel", "Material Blend Setting")},
    "Mesh Display/Per Instance Sharing": ("Linked data", "Blender instances share mesh data (Alt+D, Object > "
                                          "Link/Transfer Data); per-instance attributes aren't available."),
    "Mesh Tools": ("Edit mode tools", "Blender: paint reduce / transfer weights = vertex groups on Decimate / "
                   "Data Transfer modifiers."),
    "Mesh Tools/Sculpting Tools": ("Sculpt mode", "Blender has no Spray / Repeat / Imprint stamps or Maya sculpt "
                                   "targets: use a textured Draw brush, or sculpt on a shape key."),
    "Mesh": ("Object > Link/Transfer Data", "Blender has no attribute clipboard or vertex-order transfer: Object > "
             "Link/Transfer Data copies data between objects."),
    "Mesh/Smooth Proxy": ("Subdivision Surface modifier", "Blender's smooth proxy is the Subdivision Surface "
                          "modifier: there is no mirror proxy."),
    "Edit Mesh": ("Edit mode", "Blender: invisible faces = delete or hide faces, reorder vertices = Mesh > Sort "
                  "Elements, project a curve = Knife Project."),
    "Curves": ("Curve edit mode", "Blender curves don't have this command: use curve edit tools or Geometry Nodes "
               "(Resample = Rebuild, Fillet Curve, Offset, Trim)."),
    # common menus
    "Create": ("Add menu (Shift+A)", "Blender doesn't have this built in; Add (Shift+A) and the Extra Objects "
               "extension have the closest shapes."),
    "Create/Polygon Primitives": ("Create at the origin", "The kit creates primitives at the origin at once; "
                                  "Blender has no interactive (drag to size) creation."),
    "Create/NURBS Primitives": ("Add > Surface", "Blender's NURBS primitives are in Add > Surface (no cube, cone or "
                                "square); objects are created at once, never interactively."),
    "Create/Volume Primitives": ("Volume / Mesh to Volume", "Blender has no volume primitives: a Volume object or a "
                                 "Mesh to Volume modifier."),
    "Create/Measure Tools": ("Measure tool", "Blender's Measure tool measures distances and angles; parameter and "
                             "arc length tools aren't built in."),
    "Create/Asset": ("Asset Browser", "Blender assets are data-blocks marked as assets (Asset Browser), not "
                     "containers."),
    "Create/Scene Assembly": ("Collection instance", "Blender's scene assemblies are collection instances (Add > "
                              "Collection Instance)."),
    "Select": ("Select menu", "Blender has no selection by this type or constraint: Select All by Type, Select "
               "Linked and Select Similar are the closest."),
    **{f"Select/{sub}": ("Curve edit mode", "Blender selects curve and surface control points in edit mode (Tab); "
                         "hulls, isoparms and edit points aren't separate components.")
       for sub in ("Components#2", "Components#3", "CV Selection Boundary", "Surface Border", "Cluster Curve")},
    # Step 141: the conversions work (operators/select_convert.py); Blender has no face-corner (vertex face) selection.
    "Select/Convert Selection": ("Faces / UV Editor", "Blender has no vertex-face component: per-corner normals and "
                                 "colours follow the selected faces (Mesh > Normals > Set from Faces); single "
                                 "corners are picked in the UV Editor with Sticky Selection off."),
    "Select/Components/Vertex Face": ("Faces / UV Editor", "Blender has no vertex-face component: per-corner normals "
                                      "and colours follow the selected faces (Mesh > Normals > Set from Faces); "
                                      "single corners are picked in the UV Editor with Sticky Selection off."),
    "Modify": ("Object menu", "Blender has no command by this name: see Object > Transform / Apply / Snap."),
    "Modify/Asset": ("Asset Browser / library overrides", "Blender has no assets with published attributes: mark "
                     "data-blocks as assets, expose values with library overrides or custom properties."),
    "Modify/Convert": ("Object > Convert", "Blender converts curves, text, meshes and Grease Pencil; subdiv surfaces, "
                       "Paint Effects, displacement and fluids aren't separate types."),
    "Modify/Transformation Tools": ("Toolbar transform tools", "Blender's manipulator is the active tool's gizmo: "
                                    "there is no Type Manipulator."),
    "Edit": ("Edit menu", "Blender has no LOD groups, fur or attribute-value transfer: Object > Link/Transfer Data "
             "copies data between objects."),
    **{f"Edit/{sub}": ("Outliner / Purge", "Blender has no node of this type: delete it in the Outliner, or File > "
                       "Clean Up > Purge for unused data.") for sub in ("Delete All by Type", "Delete by Type")},
    "File": ("File menu", "Blender has no ATOM files or After Effects Live Link: move animation with "
             "Append / Link or FBX / Alembic export."),
    "Display": ("Viewport Overlays", "Blender draws this through the Viewport Overlays popover or Object properties > "
                "Viewport Display; there is no per-camera visibility."),
    "Windows": ("Blender editors", "Blender has no editor by this name: any area's editor-type menu gives the "
                "closest editor."),
    **{f"Windows/{item}": ("Window menu", "Blender windows are managed by the OS (Window > Toggle Window Fullscreen); "
                           "there is no Home screen.")
       for item in ("Home", "Minimize Application", "Raise Main Window", "Raise Application Windows")},
}


def _anim_editor_categories(ed):
    """Greyed items of the Graph Editor / Dope Sheet menu bars (step 104; Maya 2026's menus since step
    269)."""
    m = {
        f"{ed}/Edit/Transformation Tools": (
            "G / S on keys", "Blender moves and scales keys with G and S (or by dragging): it has no lattice or "
            "region key tool (the Retime Tool is the kit's)."),
        f"{ed}/Edit/Scene Time Warp": (
            "Time Stretching", "Blender has no time warp curve: Output Properties > Frame Range > Time Stretching "
            "remaps the whole scene."),
        f"{ed}/Curves": (
            "Graph Editor Key / Channel menus", "Blender has no such curve command: its curve tools are in the Graph "
            "Editor's Key and Channel menus (Smooth, Decimate, Clean, Bake, Euler Filter)."),
        f"{ed}/Curves/Spreadsheet...": (
            "Sidebar F-Curve tab", "Blender edits key values in the sidebar (F-Curve > Active Keyframe), not in a "
            "spreadsheet."),
        f"{ed}/List": (
            "Only Show Selected", "Blender lists the selected objects' channels (List > Auto Load Selected Objects); "
            "it has no bookmarks or manual loading."),
        f"{ed}/Show": (
            "Channel filters", "Blender filters channels by data type and by name (header filter popover); Maya's "
            "other node and attribute filters aren't there."),
        f"{ed}/Help/What's New/Highlight What's New": (
            "Release notes", "Blender doesn't highlight new menu items: Help > What's New lists them."),
    }
    if ed != "Graph Editor":
        # Maya 2026's Dope Sheet has no Curves menu, Scene Time Warp or bookmarks; its Transformation Tools and
        # List items left greyed have their own notices (NOTICES below), Spreadsheet moved to the Channels menu
        for key in ("Curves", "Edit/Transformation Tools", "Edit/Scene Time Warp", "List"):
            m.pop(f"{ed}/{key}", None)
        m[f"{ed}/Channels/Spreadsheet..."] = m.pop(f"{ed}/Curves/Spreadsheet...")
    else:
        m[f"{ed}/Edit/Transformation Tools/Sculpting Tools"] = (
            "Key menu", "Blender has no key brushes: its Graph Editor smooths and decimates the selected keys from "
            "the Key menu instead.")
    return m


CATEGORIES.update({
    **_anim_editor_categories("Graph Editor"),
    **_anim_editor_categories("Dope Sheet"),
    "Graph Editor/View/Theme": (
        "Themes", "Blender's colours come from the theme: Preferences > Themes (install a Maya-like theme there if you want "
        "Maya's colours)."),
    "Graph Editor/Select/Pre-Select Highlight": (
        "Hover", "Blender's Graph Editor doesn't highlight the curve under the mouse: the picked curve is drawn "
        "brighter once selected."),
    "Dope Sheet/View": (
        "View menu", "Blender's Dope Sheet has no scene summary, channel sets or key value display: a key's value "
        "shows in the Graph Editor."),
    "UV Editor/Edit/Reset UV Editor UI": (
        "Load Factory Settings", "Blender resets editors with File > Defaults > Load Factory Settings."),
    "UV Editor/Select": (
        "Select menu", "Blender's UV Editor shows one image, so there is no per-image face filter: Select Linked, "
        "Overlapping and the face filters (Connected / Contained Faces) are there."),
    "UV Editor/Tools": (
        "UV tools", "Blender's UV tools are Select, Move / Rotate / Scale, Rip and the Grab / Relax / Pinch sculpt "
        "brushes."),
    "UV Editor/View": (
        "Overlays", "Blender's UV Editor has no such display option: see its Overlays popover and View menu."),
    "UV Editor/View/Isolate Select": (
        "Add Selected / Remove Selected", "Blender has no saved quick-select sets to load into the isolate view: "
        "build it with Add Selected / Remove Selected (Ctrl+1 isolates the selection)."),
    # step 178: Maya's symmetrizeUV only works after the Symmetrize UV brush's own seam pick in Maya
    # (without it no UV changes): no rule to copy.
    "UV Editor/Modify/Symmetrize": (
        "UV > Mirror / Snap", "Blender has no UV symmetrize: mirror the UVs (Modify > Flip) and weld them (Match UVs), "
        "or unwrap with the mesh's symmetry (Mirror modifier with Mirror U)."),
    "UV Editor/Tools/Symmetrize": (
        "UV > Mirror / Snap", "Blender has no UV symmetrize brush: mirror the UVs (Modify > Flip) and weld them "
        "(Match UVs), or unwrap with the mesh's symmetry (Mirror modifier with Mirror U)."),
    "UV Editor/Image": (
        "Image field", "Blender shows one image (header image field): no dimming, PSD networks, bake preview or pixel "
        "ruler."),
    "UV Editor/Textures": (
        "Image field", "Blender lists the images in the header image field."),
    "UV Editor/UV Sets": (
        "UV Maps", "Blender's UV sets are UV Maps (Object Data Properties > UV Maps: rename there; no per-instance "
        "sharing)."),
    "UV Editor/Help/What's New/Highlight What's New": (
        "Release notes", "Blender doesn't highlight new menu items: Help > What's New lists them."),
    "UV Toolkit/Options": (
        "Panel sections", "Blender's panels have no settings reset: fold the sections by hand."),
})


SHOW_SECONDS = 5.0
_current = {"text": "", "until": 0.0}
# [(window pointer, status bar area)] seen while drawing it. Global areas aren't in screen.areas, so the area can't be
# found again by pointer; it lives as long as its window, so it is only touched while that window is open (#36).
_status_area = []


def notice(path):
    """(substitute, line) written for this Maya menu path, or None."""
    return NOTICES.get(path)


def category(path):
    """(substitute, line) of the most specific CATEGORIES rule for ``path`` (longest prefix), or None."""
    best = None
    for prefix in CATEGORIES:
        if (path == prefix or path.startswith(prefix + "/")) and (best is None or len(prefix) > len(best)):
            best = prefix
    return CATEGORIES[best] if best is not None else None


def greyed_notice(path):
    """The notice a greyed (unmapped) item shows: its own, else its category's (step 101: none without one)."""
    return NOTICES.get(path) or PANEL_MENU.get(path) or category(path)


# Notices of kit panel rows that are not Maya menu items (e.g. Modeling Toolkit options, step 63),
# filled by the panels' register(); same (substitute, line) form.
PANEL_NOTICES = {}

# Greyed rows of the viewport panel menus, the panel toolbar and the Status Line (ui/panel_menu_data.leaves(), step 177):
# same (substitute, line) form; keys = their ``allset.menu_unavailable`` items. Not Maya main-menu paths, so kept out
# of NOTICES (test_step54 checks those against menus.json); tests/test_step177_panel_menus.py: every greyed row has one.
_PM, _PT, _SL = "Panel menu/", "Panel toolbar/", "Status Line/"
_PER_OBJECT = ("Object Properties > Viewport Display", "Blender sets this per object, not per view: Object Properties > "
               "Viewport Display > Display As (Bounds / Wire / Solid).")
_NO_FILTER = ("Show > All / None", "Blender's views filter by object type only (mesh, curve, light, camera, empty ...): "
              "this Maya node type has no type of its own, so it shows with its object type.")
PANEL_MENU = {
    _PM + "View/View Sequence Time": (
        "Timeline markers", "Blender has no camera sequencer view time: bind scene cameras to timeline markers "
        "(Ctrl+B in the Timeline) or cut camera strips in the Video Sequencer."),
    _PM + "View/Camera Tools/2D Pan∕Zoom Tool": (
        "Camera Shift X / Y", "Blender has no 2D pan / zoom of the film back: move the camera's Shift X / Y "
        "(Attribute Editor > Film Offset) or zoom in the camera view (wheel)."),
    _PM + "View/Camera Tools/Azimuth Elevation Tool": (
        "Tumble Tool / Alt+LMB", "Blender has no azimuth / elevation camera tool: tumble with Alt+LMB or the Tumble Tool."),
    _PM + "View/Camera Tools/Yaw-Pitch Tool": (
        "Walk Tool / Fly Tool", "Blender has no yaw-pitch tool: turn the camera in place with the Walk or Fly Tool."),
    _PM + "Shading/Smooth Shade Selected Items": (
        "Smooth Shade All", "Blender shades a whole view one way (Solid / Wireframe), not per selected object."),
    _PM + "Shading/Flat Shade All": (
        "Use Flat Lighting", "Blender shades faces from the mesh normals in every view; there is no faceted display mode "
        "(Lighting > Use Flat Lighting is the nearest view setting)."),
    _PM + "Shading/Flat Shade Selected Items": (
        "Use Flat Lighting", "Blender shades faces from the mesh normals in every view; there is no per-object faceted "
        "display mode."),
    _PM + "Shading/Bounding Box": _PER_OBJECT,
    _PM + "Shading/Points": (
        "Wireframe", "Blender's views have no points-only display mode: Wireframe is the nearest (Object Properties > "
        "Viewport Display > Display As shows one object as Bounds / Wire)."),
    _PM + "Shading/X-Ray Active Components": (
        "X-Ray", "Blender X-rays the whole view (X-Ray), not only the components being edited."),
    _PM + "Shading/Interactive Shading": (
        "none", "Blender keeps the view's shading while you tumble (no wireframe fallback): nothing to switch."),
    _PM + "Shading/Smooth Wireframe": (
        "none", "Blender always draws anti-aliased wires (a preference: Viewport > Quality > Anti-Aliasing)."),
    _PM + "Shading/Hardware Fog": (
        "Mist pass", "Blender's viewport has no fog: the render's Mist pass (View Layer properties) gives depth fog."),
    _PM + "Lighting/Use Selected Lights": (
        "Use All Lights + hide lights (h)", "Blender lights a view with every visible scene light and has no per-view "
        "light list: hide the lights you don't want (h) or use light linking (Object Properties > Shading)."),
    _PM + "Lighting/Use No Lights": (
        "Use Flat Lighting", "Blender's views always light the scene (studio, flat or scene lights): Use Flat Lighting "
        "shows the plain colours."),
    _PM + "Lighting/Two Sided Lighting": (
        "none", "Blender always lights both sides of a face in the viewport (Backface Culling hides back faces)."),
    **{_PM + f"Show/{t}": _NO_FILTER for t in (
        "Controllers", "Subdiv Surfaces", "Planes", "Dynamics", "Particle Instancers",
        "Follicles", "nCloths", "nParticles", "nRigids", "Dynamic Constraints", "Dimensions", "Handles",
        "Texture Placements", "Strokes", "Plugin Shapes", "Clip Ghosts")},
    # step 197 (#104) re-measured in a GUI 4.5: overlay display_handle NONE and vertex_opacity 0 change no pixel of a
    # NURBS surface / curve hull or CVs in Edit Mode (tmp/p197 probe), so no overlay toggle can stand in for these
    **{_PM + f"Show/{t}": (
        "Edit Mode", "Blender draws a curve's or surface's control points and control polygon (hull) only in Edit "
        "Mode, and always there: they can't be shown in Object Mode or hidden per view.") for t in (
        "NURBS CVs", "NURBS Hulls")},
    _PM + "Show/Hold-Outs": (
        "Object Properties > Visibility > Holdout", "Blender holdouts are a render setting per object / collection; "
        "views don't filter them."),
    _PM + "Renderer/Legacy Default Viewport": (
        "Viewport 2.0", "Blender has one viewport renderer (Workbench / EEVEE), listed as Viewport 2.0 and the render "
        "engine."),
    _PM + "Renderer/Legacy High Quality Viewport": (
        "Viewport 2.0", "Blender has one viewport renderer (Workbench / EEVEE), listed as Viewport 2.0 and the render "
        "engine."),
    _PM + "Panels/Stereo": (
        "Render Properties > Stereoscopy", "Blender's stereo is a scene setting (Stereoscopy, Views) shown in a stereo "
        "window mode, not a panel type."),
    _PM + "Panels/Camera Set": (
        "Timeline markers", "Blender has no camera sets: bind cameras to timeline markers to switch between them."),
    _PM + "Panels/Hypergraph Panel": (
        "Outliner / Node Editor", "Blender has no Hypergraph: hierarchy is in the Outliner, node networks in the Node "
        "Editor (Panels > Panel)."),
    _PM + "Panels/Saved Layouts": (
        "Windows > Workspaces", "Blender saves layouts as workspaces (Windows > Workspaces, the Status Line drop-down)."),
    _PM + "Panels/Panel Editor...": (
        "Workspaces", "Blender has no panel editor: split / join areas by their corners and keep layouts as workspaces."),
    _PT + "Bookmarks": (
        "Look Through Selected / Undo View Change", "Blender has no camera bookmarks: keep a camera per view (Panels > "
        "Look Through Selected) or step back with View > Undo View Change."),
    _PT + "2D Pan∕Zoom": (
        "Camera Shift X / Y", "Blender has no 2D pan / zoom of the film back: move the camera's Shift X / Y (Attribute "
        "Editor > Film Offset)."),
    _PT + "Resolution Gate": (
        "Camera view frame (always on)", "Blender's camera view always draws the render resolution frame (Maya's "
        "resolution gate; Maya's factory camera has it off, overscan 1.0): it can't be "
        "hidden. Gate Mask dims the outside (Blender passepartout)."),
    _PT + "Field Chart": (
        "Safe Action / Safe Title", "Blender has no field chart: the safe areas (Safe Action / Safe Title) are the "
        "camera guides it draws."),
    _PT + "Motion Blur": (
        "Render Properties > Motion Blur", "Blender's viewport doesn't preview motion blur: it is a render setting "
        "(EEVEE / Cycles Motion Blur)."),
    _PT + "Multisample Anti-aliasing": (
        "Preferences > Viewport > Anti-Aliasing", "Blender's viewport anti-aliasing is a preference, not a per-view "
        "button (the kit doesn't change preferences)."),
    _PT + "X-Ray Active Components": (
        "X-Ray", "Blender X-rays the whole view (X-Ray), not only the components being edited."),
    _SL + "Select by hierarchy": (
        "Outliner / Select > Hierarchy", "Blender has no hierarchy pick mode: pick roots in the Outliner, Select > "
        "Hierarchy takes the children."),
    _SL + "Select by handle object type": (
        "none", "Blender has no selection handles: pick the object itself."),
    _SL + "Select by dynamic object type": (
        "Select by object type", "Blender's simulations are modifiers on meshes (cloth, fluid, particles): pick them "
        "with the mesh / point types."),
    _SL + "Snap to projected center": (
        "Snap to Face Project", "Blender snaps to the face under the mouse (Face Project), not to the centre of the "
        "object behind it."),
    _SL + "Snap to view planes": (
        "Snap to Grid", "Blender has no view planes: snap to the grid or to a vertex."),
    _SL + "Render Setup": (
        "View Layers", "Blender's render layers are View Layers (top bar, View Layer properties), with collections "
        "for overrides."),
    _SL + "Pause Viewport 2.0": (
        "none", "Blender's viewport can't be paused: switch to Wireframe or hide heavy objects."),
    _SL + "Show or hide the Character Controls": (
        "none", "Blender has no HumanIK character controls: rig with armatures (Skeleton menu)."),
}


def tooltip(path, mapped):
    """Tooltip text for a Maya menu item with a notice (also shows it in the Help Line)."""
    n = NOTICES.get(path) or PANEL_NOTICES.get(path) or PANEL_MENU.get(path) or (None if mapped else category(path))
    if n is None:
        return None
    substitute, line = n
    head = f"Maya: {path}  →  Blender: {substitute}" if mapped else f"Maya: {path} (greyed)  →  {substitute}"
    show(f"{path.rsplit('/', 1)[-1]}: {line}")
    return f"{head}\n{line}"


def show(text):
    """Put ``text`` in the Help Line for a few seconds."""
    _current["text"] = text
    _current["until"] = time.monotonic() + SHOW_SECONDS
    _redraw_status_later()
    if not bpy.app.timers.is_registered(_expire):
        bpy.app.timers.register(_expire, first_interval=SHOW_SECONDS + 0.1)


def current():
    return _current["text"] if time.monotonic() < _current["until"] else ""


def remember_status_area(area, window=None):
    window = window or bpy.context.window
    if area is not None and window is not None and (not _status_area or _status_area[0][1] != area):
        _status_area[:] = [(window.as_pointer(), area)]


def _redraw_status():
    for win_ptr, area in list(_status_area):
        if live_refs.window(win_ptr) is None:
            _status_area.clear()
            continue
        try:
            area.tag_redraw()
        except ReferenceError:
            _status_area.clear()
    return None


def _redraw_status_later():
    # Not from inside a tooltip / draw callback: one tick later.
    if not bpy.app.timers.is_registered(_redraw_status):
        bpy.app.timers.register(_redraw_status, first_interval=0.0)


def _expire():
    if not current():
        _current["text"] = ""
        _redraw_status()
        return None
    return 0.5


class ALLSET_OT_tip(bpy.types.Operator):
    """A note about the Blender side of a Maya window, shown as a tooltip instead of a sentence in the window
    (docs/steps/step_106.md 「계획」 4); clicking puts it in the Help Line"""
    bl_idname = "allset.tip"
    bl_label = "Note"
    bl_options = {'INTERNAL'}

    text: bpy.props.StringProperty(options={'SKIP_SAVE'})

    @classmethod
    def description(cls, context, props):
        return props.text

    def execute(self, context):
        if self.text:
            show(self.text)
        return {'FINISHED'}


def tip(layout, text, label="", icon='INFO'):
    """A flat button whose tooltip is ``text`` (Maya windows carry no Blender explanation sentences)."""
    op = layout.operator(ALLSET_OT_tip.bl_idname, text=label, icon=icon, emboss=False)
    op.text = text
    return op


def register():
    bpy.utils.register_class(ALLSET_OT_tip)


def unregister():
    bpy.utils.unregister_class(ALLSET_OT_tip)
    for fn in (_expire, _redraw_status):
        if bpy.app.timers.is_registered(fn):
            bpy.app.timers.unregister(fn)
    _current.update(text="", until=0.0)
    _status_area.clear()

# Mapped editor items that run a near substitute (step 104): Maya 2022's newer auto tangents are one Blender handle.
NOTICES.update({
    f"{ed}/Tangents/{sub}Auto/{kind}": (
        "Auto Clamped handles", "Blender has one auto tangent (Auto Clamped): Maya's Ease / Mix / Custom weights "
        "aren't there.")
    for ed in ("Graph Editor",) for sub in ("", "In Tangent/", "Out Tangent/")
    for kind in ("Auto Ease", "Auto Mix", "Auto Custom")})


# Step 269: Maya 2026 editor menu items with no Blender command (Maya annotations)
_REMOVE_KEY = {
    "Unsnapped": ("Select Unsnapped", "Blender has no command that deletes unsnapped keys: Edit > Select Unsnapped, "
                  "then Delete."),
    "Before Current Frame": ("Select before the frame", "Blender deletes only selected keys: Select > Before "
                             "Current Frame ([), then Delete."),
    "After Current Frame": ("Select after the frame", "Blender deletes only selected keys: Select > After Current "
                            "Frame (]), then Delete."),
    "Outside Selected Range": ("Select, then Delete", "Blender has no delete outside a range: select the keys to "
                               "remove, then Delete."),
    "Ripple Delete": ("Delete, then G", "Blender has no ripple delete for keys: delete them, then move the later "
                      "keys back with G."),
}
_SHAPES = ("Shape Keys filter", "Blender shows shape key channels or not (header filters > Shape Keys); it has no "
           "auto expand depth for them.")
NOTICES.update({
    **{f"{ed}/Keys/Remove Key/{item}": notice for ed in ("Graph Editor", "Dope Sheet")
       for item, notice in _REMOVE_KEY.items()},
    **{f"{ed}/List/Show Animated Shapes/{item}": _SHAPES for ed in ("Graph Editor", "Dope Sheet")
       for item in ("One", "None")},
    "Graph Editor/Select/Prefer Selected Curves": (
        "Hide unselected (Shift+H)", "Blender picks keys on every curve it shows: hide the unselected curves "
        "(Shift+H) to pick only on the selected ones."),
    "Graph Editor/View/Channel Sets": (
        "Not in Blender", "Blender's channel list has no channel sets."),
    "Graph Editor/Curves/Offset Curves": (
        "G with a number", "Blender moves the selected keys by a typed amount: G, then the amount, then Enter."),
    "Graph Editor/Curves/Clamp Curves": (
        "Limits modifier", "Blender clamps a curve's values with a Limits F-Curve modifier (Sidebar > Modifiers), "
        "not by changing the keys."),
    "Dope Sheet/Channels/Offset Keys": (
        "G with a number", "Blender moves the selected keys by a typed number of frames: G, then the amount, then "
        "Enter."),
    "Dope Sheet/Edit/Transformation Tools/Select Tool": (
        "Click / box select", "Blender selects keys with a click or a box drag: no tool to pick."),
    "Dope Sheet/Edit/Transformation Tools/Select Keys": (
        "Click / box select", "Blender selects keys with a click or a box drag: no tool to pick."),
    "Dope Sheet/Edit/Transformation Tools/Move Nearest Picked Key": (
        "Drag a key", "Blender's Dope Sheet always moves the key under the mouse when you drag it: no tool to pick."),
})


# Step 150 (#96): Graph Editor View items Blender can't do, one line each (Maya
# annotations); the mapped ones are in ui/editor_menu_map.py + ui/graph_view.py.
_GE_VIEW = "Graph Editor/View/"
NOTICES.update({
    **{_GE_VIEW + item: (
        "View > Frame", "Blender keeps no history of Graph Editor views: frame again with View > Frame All or "
        "Frame Selection.") for item in ("Undo View Change", "Redo View Change")},
    _GE_VIEW + "Clip Time": (
        "NLA Editor", "Blender has no Trax clip time to switch: a strip's keys show at the strip's time while you "
        "tweak the strip in the NLA Editor (Tab)."),
    _GE_VIEW + "Show Results": (
        "Drivers mode", "Blender draws no curve for a value that isn't keyed: driver curves are in the Graph "
        "Editor's Drivers mode (header mode menu)."),
    _GE_VIEW + "Show Play Range Shades": (
        "Always on", "Blender always darkens the frames outside the playback range in the Graph Editor: the shades "
        "can't be turned off."),
    _GE_VIEW + "Lock Play Range Shades": (
        "Range Slider", "Blender's range shades can't be dragged, so there is nothing to lock: set the range on the "
        "Range Slider."),
    _GE_VIEW + "Highlight Affected Curves": (
        "Selected keys", "Blender highlights the selected keys and their curve, not the curve segment a key or "
        "tangent affects."),
    _GE_VIEW + "Keys/Never": (
        "Keys > Active Only", "Blender always draws the keys of the curves it shows; Keys > Active Only (Preferences "
        "> Animation > Only Selected Curve Keyframes) is the closest."),
    **{_GE_VIEW + "Key View/" + item: (
        "Dope Sheet key types", "Blender draws Graph Editor keys without state marks; key types (breakdown, ...) "
        "show in the Dope Sheet.") for item in ("Detailed", "Simple")},
    **{_GE_VIEW + "Time Ruler Placement/" + item: (
        "Top", "Blender's Graph Editor time ruler is always at the top (the scrubbing area), Maya's factory "
        "placement.") for item in ("Top", "Bottom")},
    _GE_VIEW + "Simple Toolbar": (
        "Graph Editor toolbar", "Blender has one row for the Maya Graph Editor toolbar: there is no simple "
        "variant."),
})
# Show menu items with a specific substitute (Graph Editor and Dope Sheet share the filters)
NOTICES.update({
    **{f"{ed}/Show/Objects/Clips": (
        "NLA Editor", "Blender's clips are NLA strips: they are listed in the NLA Editor, not in this list.")
       for ed in ("Graph Editor", "Dope Sheet")},
})


# Greyed items of the Hypershade / Render Settings / Script Editor menu bars (step 105, Maya 2022)
CATEGORIES.update({
    "Hypershade/File": (
        "File > Append", "Blender shares shading networks by appending or linking materials from another .blend "
        "(File > Append), not by exporting a network file."),
    "Hypershade/Edit": (
        "Shader Editor", "Blender has no such Hypershade command: PSD networks, texture conversion and swatch "
        "rendering aren't there; edit the network in the Shader Editor."),
    "Hypershade/Edit/Delete All by Type": (
        "Outliner / Purge", "Blender has no node of this type to delete: use the Outliner (Blender File view) or "
        "File > Clean Up > Purge."),
    "Hypershade/Edit/Select All by Type": (
        "Outliner", "Blender selects materials and textures in the Outliner's Blender File view, not in the scene."),
    "Hypershade/Edit/Duplicate": (
        "Duplicate", "Blender copies a material with its whole node tree (Duplicate / Shading Network)."),
    "Hypershade/Create": (
        "Shift+A in the Shader Editor", "Blender has no node of this Maya type: add the closest node with Shift+A "
        "in the Shader Editor."),
    "Hypershade/Create/Materials": (
        "Principled BSDF", "Blender has no game, hair-tube, ocean or USD Preview shader of this kind: start from a "
        "Standard Surface (Principled BSDF)."),
    "Hypershade/Create/Volumetric Materials": (
        "Volume shaders", "Blender's volumes are shaders on the material's Volume output (Principled Volume) or the "
        "World's: add them with Shift+A > Volume."),
    "Hypershade/Create/Image Plane": (
        "Image Plane", "Blender's image planes are made in the 3D View (Create > Free Image Plane)."),
    "Hypershade/Create/Rendering": (
        "View Layers / AOVs", "Blender's render passes are View Layer passes (View Layer Properties > Passes)."),
    "Hypershade/Create/Glow": (
        "Bloom / Glare", "Blender makes glow in the compositor (Glare node)."),
    "Hypershade/Create/Particle Utilities": (
        "Attribute node", "Blender's particles feed shaders through the Particle Info / Attribute nodes."),
    "Hypershade/Create/Switch Utilities": (
        "Mix nodes", "Blender switches between inputs with Mix nodes (or Geometry Nodes Switch)."),
    "Hypershade/Create/Create Options": (
        "Shader Editor", "Blender materials always have their output node; textures get their own Mapping node."),
    "Hypershade/Tabs": (
        "Node groups / workspaces", "Blender's Shader Editor has one graph per material: no Hypershade tabs (use "
        "node groups or another workspace)."),
    "Hypershade/Graph": (
        "Shader Editor", "Blender shows the whole node tree of the material: there is no partial graph to build."),
    "Hypershade/Window": (
        "Blender editors", "Blender has no such Hypershade panel: the material preview is in the Properties "
        "Material tab."),
    "Hypershade/Options": (
        "Preferences", "Blender's Shader Editor has no such option (see Preferences > Editing / Interface)."),
    "Hypershade/Help/What's New/Highlight What's New": (
        "Release notes", "Blender doesn't highlight new menu items: Help > What's New lists them."),
    "Render Settings/Edit": (
        "Path field", "Blender has no project image directory: type the folder in the Common tab's Path field."),
    "Render Settings/Presets": (
        "Format presets", "Blender's render presets are the Format panel preset menu (Output Properties); Maya's "
        "AOV / user presets aren't there."),
    "Render Settings/Help/What's New/Highlight What's New": (
        "Release notes", "Blender doesn't highlight new menu items: Help > What's New lists them."),
    "Script Editor/File": (
        "Text Editor", "Blender sources a script by opening it and running it (Text > Run Script) and saves one text "
        "at a time; shelf buttons from scripts aren't there."),
    "Script Editor/Command": (
        "Text Editor View menu", "Blender's Text Editor has no such option (see its View menu and Preferences > "
        "Editing > Text Editor)."),
    "Script Editor/Tabs": (
        "Text data-blocks", "Blender's script tabs are texts: rename one in the Text Editor header's name field."),
    "Script Editor/Help": (
        "Python API", "Blender has no help on a selected command: see the Python API reference (Help > Python "
        "Command Reference)."),
    "Script Editor/Help/What's New/Highlight What's New": (
        "Release notes", "Blender doesn't highlight new menu items: Help > What's New lists them."),
    # step 209: the Reference Editor's menu bar (ui/reference_editor.py)
    "Reference Editor/File/Save Reference Edits": (
        "Open the source .blend", "Blender can't write into a linked file from here: open the referenced .blend, make "
        "the change there and save it (Reload shows it)."),
    "Reference Editor/File/List Unknown Edits...": (
        "Library overrides", "Blender drops an override whose source data is gone: there are no unknown edits to "
        "list."),
    "Reference Editor/Proxy": (
        "Replace Reference", "Blender has no reference proxies: use Replace Reference to switch the file (its edits "
        "are applied again by name)."),
    "Reference Editor/View": (
        "Outliner: Blender File", "Blender keeps no reference view sets: the Outliner's Blender File view lists every "
        "linked file."),
    "Reference Editor/Help/What's New/Highlight What's New": (
        "Release notes", "Blender doesn't highlight new menu items: Help > What's New lists them."),
    # step 213: the Component Editor's menu bar (ui/component_editor.py)
    "Component Editor/Options/Right Justify Headers": (
        "Left-aligned headers", "Blender's panel layout can't right-justify a label row."),
    "Component Editor/Options/Show Namespaces": (
        "Mesh names", "The table shows the mesh data names: Blender has no DAG path or namespace to add."),
    "Component Editor/Options/Sort Alphabetically": (
        "Influence order", "Blender lists vertex groups in their own order: the columns follow it (Maya's influence order)."),
    "Component Editor/Options/Show Path Name": (
        "Mesh names", "The table shows the mesh data names: Blender has no DAG path or namespace to add."),
    "Component Editor/Options/Change Precision...": (
        "3 decimals", "Blender fields show 3 decimals here (Maya's factory precision); type more digits in a cell to set them."),
    "Component Editor/Options/Remember This Layout...": (
        "Hide Zero Columns", "Blender keeps no saved column layouts here: Hide Zero Columns / Show All Columns pick the "
        "columns."),
    "Component Editor/Options/Delete Current Layout": (
        "Hide Zero Columns", "Blender keeps no saved column layouts here: Hide Zero Columns / Show All Columns pick the "
        "columns."),
    "Component Editor/Layout/Show Selected Columns": (
        "Show All Columns", "Blender's panel table has no column selection: Show All Columns / Hide Zero Columns pick them."),
    "Component Editor/Layout/Show Selected Objects": (
        "Load Selected Components", "Blender shows the selected objects' selected vertices with Load Selected Components."),
    "Component Editor/Help/What's New/Highlight What's New": (
        "Release notes", "Blender doesn't highlight new menu items: Help > What's New lists them."),
    # step 213: the Shape Editor's menu bar (ui/shape_editor.py)
    "Shape Editor/File": (
        "Blender files", "Blender has no blend shape file format: keep shapes in a .blend and append the mesh."),
    "Shape Editor/Edit/Group": (
        "Flat target list", "Blender's shape keys are one flat list per mesh: no target groups."),
    "Shape Editor/Shapes/Merge Targets": (
        "New Shape from Mix", "Blender's Shape Keys menu > New Shape from Mix makes one key of the current mix."),
    "Shape Editor/Shapes/Rebuild Target": (
        "Duplicate + Apply", "Blender keeps no target mesh to rebuild: duplicate the mesh and apply the key."),
    "Shape Editor/Options/Show Edit HUD": (
        "Edit button", "Blender draws no edit HUD here: the target being edited has the red Edit button."),
    "Shape Editor/Help/What's New/Highlight What's New": (
        "Release notes", "Blender doesn't highlight new menu items: Help > What's New lists them."),
    # step 213: the Connection Editor's menu bar (ui/connection_editor.py)
    "Connection Editor/Options/Attribute Order": (
        "Maya list order", "Blender attributes are listed visibility, translate, rotate, scale, then custom ones (no "
        "other order)."),
    "Connection Editor/Left Display": (
        "Transform and custom attributes", "The kit lists only attributes Blender has: transform channels, "
        "visibility and custom attributes (no hidden / published / output-only filters)."),
    "Connection Editor/Right Display": (
        "Transform and custom attributes", "The kit lists only attributes Blender has: transform channels, "
        "visibility and custom attributes (no hidden / published / output-only filters)."),
    "Connection Editor/Help/What's New/Highlight What's New": (
        "Release notes", "Blender doesn't highlight new menu items: Help > What's New lists them."),
})


# Greyed marking-menu entries (step 136, docs/steps/step_136_items.md): key = "<menu>/<label path>" (the kit menu
# name in marking/menus.MENUS, then the labels down to the item), same (substitute, one line) form. While a marking
# menu hovers a greyed entry, marking/modal.py puts its line in the Help Line. tests/test_step136_items.py checks that
# every greyed entry has one and every key is a greyed entry.
_RMB_LIST = {
    # DG Traversal / Inputs / Outputs list the object's nodes since step 165 (marking/rmb_nodes.py)
    "Metadata": ("Custom properties", "Blender has no component metadata: store values as custom properties (Attribute "
                 "Editor Extra Attributes) or mesh attributes."),
    "Time Editor": ("NLA Editor", "Blender's clip-based animation editor is the NLA Editor (Windows > Animation "
                    "Editors)."),
}
MARKING = {
    **{f"{menu}/{label}": n for menu in ("RMB_MESH", "RMB_CURVE") for label, n in _RMB_LIST.items()},
    "RMB_MESH/Vertex Face": (
        "Face corners", "Blender has no vertex-face component: per-corner values (normals, colours, UVs) are edited "
        "on the face corners through their tools."),
    "RMB_CURVE/Curve Point": (
        "Control points", "Blender curves are edited by their control points only: there are no points on the curve "
        "to pick (Subdivide adds one)."),
    "RMB_CURVE/Hull": (
        "Control points", "Blender draws the control polygon but has no hull component: select its control points."),
    "RMB_CURVE/Edit Point": (
        "Control points", "Blender curves have no edit points: Bezier curves are edited by their knots and handles."),
    "RMB_EMPTY/Complete Tool": (
        "Enter", "Blender tools finish with Enter or a right-click: there is no tool to complete from this menu."),
    "PIVOT_EDIT/Show Orientation Handle": (
        "Transform orientation", "Blender's pivot has no orientation handle: turn the pivot with a custom "
        "orientation (Orientation menu > Create)."),
    "SHIFT_VERTEX/Reorder Vertices": (
        "Sort Mesh Elements", "Blender can't set the vertex order from three picked vertices: Mesh > Sort Elements "
        "reorders them by a rule."),
    "SHIFT_EDGE/Merge/Collapse Edges/Merge Edge Tool": (
        "Merge / Bridge", "Blender has no tool that merges two clicked border edges: select both edges and use Merge "
        "(At Center) or Bridge."),
    "SHIFT_FACE/Assign Invisible Faces": (
        "Delete faces / Hide", "Blender's Subdivision Surface has no invisible faces: delete the faces or hide them "
        "(Display > Hide)."),
    "OPT_MOVE/Snap/Face Center": (
        "Snap to Face", "Blender snaps to faces (Face Project / Face Nearest), not to their centres."),
    "OPT_MOVE/Keep Spacing": (
        "Snap With", "Blender moves the selected components together while snapping (Snap With in the snapping "
        "popover); there is no separate keep-spacing switch."),
    "OPT_MOVE/Axis/Live Object Axis": (
        "Custom orientation", "Blender has no live object axis: create an orientation from the object (Orientation "
        "menu > Create) and pick it."),
    "OPT_MOVE/Axis/Custom": (
        "Custom orientation", "Blender's custom axis is a saved orientation: Orientation menu > Create, from the "
        "selection."),
    "OPT_ROTATE/Custom/Custom": (
        "Custom orientation", "Blender's custom axis is a saved orientation: Orientation menu > Create, from the "
        "selection."),
    "OPT_ROTATE/Custom/Reset": (
        "Orientation: Object", "Blender keeps saved orientations until you delete them: pick Object to go back to "
        "the default rotate axis."),
    "OPT_SCALE/Axis/Live Object Axis": (
        "Custom orientation", "Blender has no live object axis: create an orientation from the object (Orientation "
        "menu > Create) and pick it."),
    **{f"SELECT_ALL/{label}": (
        "Modifier stack", "Blender's history is the modifier stack, which has no upstream / downstream nodes to "
        "select or switch off: show / hide modifiers in Properties > Modifiers.")
       for label in ("Select All Inputs", "Select All Outputs", "Enable All Outputs", "Disable All Outputs")},
    "HB_NORTH/Persp/Relationship Editor": (
        "Workspaces", "Blender has no relationship editor layout: the Outliner and Properties sit beside the view "
        "in every workspace."),
    "HB_EAST/Time Slider": (
        "Timeline area", "Blender's Time Slider is the Timeline area: drag its edge to hide it, or maximize the view "
        "(Ctrl+Space) for more room."),
    "HB_EAST/Range Slider": (
        "Timeline header", "The Range Slider is part of the Timeline header in Blender: it hides with the Timeline."),
    "HB_WEST/Hierarchy": (
        "Select > Hierarchy", "Blender has no hierarchy select mode: click the object and use Select > Hierarchy "
        "(or the Outliner) to take its children."),
    "HB_WEST/Dynamics Mask": (
        "Object types filter", "Blender's dynamics are modifiers and physics settings on ordinary objects, so they "
        "have no selection mask of their own."),
    "SELECT_COMPONENT/All Components off": (
        "Mesh select mode", "Blender always keeps vertex, edge or face picking on: switch with F9 / F10 / F11."),
    "SELECT_COMPONENT/Save to shelf": (
        "Quick Favorites", "Blender has no shelf button from a pick mask: right-click a select mode button > Add to "
        "Quick Favorites."),
}
_PICK_MASK = ("Pick mask", "Blender picks this kind of component in the object's own edit mode (Tab), not through a "
              "pick mask: the mask only switches mesh vertex / edge / face picking.")
MARKING.update({f"SELECT_COMPONENT/{p}": _PICK_MASK for p in (
    "Points/All Points", "Points/CVs", "Points/Lattice Points", "Points/Subdiv Mesh Points", "Points/Particles",
    "Parm Points/All Parm Points", "Parm Points/Edit Points", "Parm Points/Curve Points",
    "Parm Points/Subdiv Mesh UVs", "Parm Points/Surface Points", "Lines/Trim Edges", "Lines/All Lines",
    "Lines/Springs", "Lines/Isoparms", "Hulls", "Pivots/Scale Pivots", "Pivots/Joint Pivots", "Pivots/All Pivots",
    "Pivots/Rotate Pivots", "Handles", "Misc./Local Rotation Axes", "Misc./Image Planes", "Misc./All Misc")})


# step 206: polyWarpImage bakes a file texture from one UV set into another as a new image file
# (Maya UV > Warp Image); Blender does that with a Cycles bake.
NOTICES.update({
    "UV Editor/Modify/Warp Image": (
        "Cycles Bake", "Blender has no image warp between UV sets: assign the texture with the old UV Map, make the "
        "new UV Map active for an empty image and bake (Render Properties > Bake, Cycles)."),
})
