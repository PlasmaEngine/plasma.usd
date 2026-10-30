# SPDX-License-Identifier: GPL-3.0-or-later
"""Maya keymap, as add-on keymap items (step 130, docs/steps/step_130_keymap.md).

The Maya keymap data = Industry Compatible data (static, ``ic_data``) patched by ``maya_patches``. Until step 130 it became a keyconfig "Maya-style" that was made
the *active* keyconfig — its name was saved in the user preferences and outlived the add-on (GitHub #46, #40). Now
the active keyconfig and every keymap preference stay as the user has them:

- every non-modal Maya item goes into the **add-on keyconfig** (``wm.keyconfigs.addon``). Blender merges add-on
  items in front of the active keyconfig's items of the same keymap, one at a time at the head (measured: they
  come out reversed), so they are added in reverse to keep the Maya order (``first=True`` / PASS_THROUGH items);
- the active keyconfig's own items of those keymaps are switched off **in memory** (``KeyMapItem.active``) while
  Maya mode is on, so an event the Maya data doesn't bind falls through to the later keymaps exactly as in the old
  Maya keyconfig. A consuming "blocker" item can't do that: whatever it returns, it either stops the whole event or
  lets the user's item below it run (measured). Preset keyconfigs are rebuilt from their preset file at every start
  and the flags are not part of the user's keymap edits (measured: saved preferences + restart = items active,
  no user-modified keymap), so nothing outlives the add-on;
- modal keymaps can't be add-on keymaps ("Modal key-maps not supported for add-on key-config", measured): the few
  Maya modal differences (Transform / Knife modal maps) are switched / added in the active keyconfig's modal map in
  memory the same way and removed again.

A persistent timer follows the active keyconfig (the user picks another preset, a keyconfig preference rebuilds
it, the preset loads after the add-on at startup) and re-applies; ``unregister`` restores everything. Small
feature-owned overlays still go through ``add_item``.
"""

import bpy

from .. import compat

# Before step 130 the kit made a keyconfig of this name active and Blender stored the name in the preferences;
# switch.py still recognises it to clean up after older versions. No keyconfig of this name is made any more.
KEYCONFIG_NAME = "Maya-style"
TARGET_VERSION = compat.TARGET   # docs/user_decisions.md decision 4
WATCH_INTERVAL = 1.0

# (KeyMap, KeyMapItem) pairs add_item created in wm.keyconfigs.addon.
_addon_keymaps = []

# What apply() did, as plain data (no RNA references kept across ticks: a regenerated keyconfig frees them).
_state = {
    "applied": False,
    "addon_items": set(),        # pointers of the Maya items in the add-on keyconfig
    "addon_made": [],            # (name, space, region) of add-on keymaps apply() created
    "kc_ptr": 0,                 # the active keyconfig whose items were switched off
    "kc_name": "",
    "off": [],                   # (km name, space, region, index, idname, type, value) switched off there
    "modal_added": set(),        # pointers of Maya modal items added to that keyconfig
    "modal_made": [],            # (name, space, region) of modal keymaps created there (none in a GUI session)
    "addon_count": -1,           # add-on keyconfig item count _keep_first last saw (step 265)
}


def add_item(km_name, idname, type, value, space_type="EMPTY", region_type="WINDOW", **kwargs):
    """Add one item to the add-on keyconfig and remember it for cleanup.

    Returns the KeyMapItem, or None when there is no add-on keyconfig
    (it can be missing in some background sessions).
    """
    kc = bpy.context.window_manager.keyconfigs.addon
    if kc is None:
        return None
    km = kc.keymaps.new(name=km_name, space_type=space_type, region_type=region_type)
    kmi = km.keymap_items.new(idname, type, value, **kwargs)
    _addon_keymaps.append((km, kmi))
    return kmi


def generate_data():
    """IC data with the Maya patches applied (fresh list each call). Strict on Blender 5.2 and 4.5 (step 272:
    the kit's own static IC data of those minors); elsewhere IC differences are collected in ``patch.DRIFT``
    (docs/steps/step_53.md)."""
    from . import ic_data, maya_patches, patch
    patch.STRICT = compat.IS_STRICT
    patch.DRIFT.clear()
    data = maya_patches.apply(ic_data.generate())
    if patch.DRIFT:
        print(f"Maya Workflow Kit: {len(patch.DRIFT)} keymap patches don't apply to Blender {bpy.app.version_string} "
              "(made for 4.5 / 5.2):", *patch.DRIFT[:20], sep="\n  ")
    return data


def _where(km_args):
    return km_args.get("space_type", "EMPTY"), km_args.get("region_type", "WINDOW")


def _find(kc, name, space, region):
    return kc.keymaps.find(name, space_type=space, region_type=region)


def _live_keyconfig(ptr):
    """The keyconfig with this pointer if it still exists (looked up, never a kept reference)."""
    for kc in bpy.context.window_manager.keyconfigs:
        if kc.as_pointer() == ptr:
            return kc
    return None


def _modal_sig(kmi):
    return (kmi.propvalue, kmi.type, kmi.value, kmi.any, kmi.shift, kmi.ctrl, kmi.alt, kmi.oskey, kmi.key_modifier)


def _modal_data_sig(item):
    ev = item[1]
    return (item[0], ev["type"], ev.get("value", "PRESS"), bool(ev.get("any")),
            *(int(ev.get(m) or 0) for m in ("shift", "ctrl", "alt", "oskey")), ev.get("key_modifier", "NONE"))


def _new_items(km, before):
    return [k for k in km.keymap_items if k.as_pointer() not in before]


# --- add-on keyconfig ------------------------------------------------------------------------------------------

def _apply_addon(data):
    from bl_keymap_utils.io import keymap_init_from_data
    kc = bpy.context.window_manager.keyconfigs.addon
    if kc is None:
        return
    for name, km_args, content in data:
        if km_args.get("modal"):
            continue
        space, region = _where(km_args)
        if _find(kc, name, space, region) is None:
            _state["addon_made"].append((name, space, region))
        km = kc.keymaps.new(name, **km_args)
        before = {k.as_pointer() for k in km.keymap_items}
        # reversed: Blender puts add-on items at the head of the merged keymap one by one (step_130_keymap.md)
        keymap_init_from_data(km, list(reversed(content["items"])))
        _state["addon_items"].update(k.as_pointer() for k in _new_items(km, before))


def _restore_addon():
    kc = bpy.context.window_manager.keyconfigs.addon
    ptrs = _state["addon_items"]
    if kc is not None and ptrs:
        for km in kc.keymaps:
            for kmi in [k for k in km.keymap_items if k.as_pointer() in ptrs]:
                km.keymap_items.remove(kmi)
        for name, space, region in _state["addon_made"]:
            km = _find(kc, name, space, region)
            if km is not None and not km.keymap_items:
                kc.keymaps.remove(km)
    ptrs.clear()
    _state["addon_made"].clear()
    _state["addon_count"] = -1


# --- the active keyconfig (in memory only) --------------------------------------------------------------------

def _apply_active(data):
    from bl_keymap_utils.io import keymap_init_from_data
    kc = bpy.context.window_manager.keyconfigs.active
    if kc is None:
        return
    _state["kc_ptr"], _state["kc_name"] = kc.as_pointer(), kc.name
    off = _state["off"]
    for name, km_args, content in data:
        space, region = _where(km_args)
        km = _find(kc, name, space, region)
        if not km_args.get("modal"):
            for i, kmi in enumerate(km.keymap_items if km is not None else ()):
                if kmi.active:
                    kmi.active = False
                    off.append((name, space, region, i, kmi.idname, kmi.type, kmi.value))
            continue
        if km is None:
            km = kc.keymaps.new(name, **km_args)
            _state["modal_made"].append((name, space, region))
        wanted = {_modal_data_sig(it) for it in content["items"]}
        have = set()
        for i, kmi in enumerate(km.keymap_items):
            if not kmi.active:
                continue
            if _modal_sig(kmi) in wanted:
                have.add(_modal_sig(kmi))
            else:
                kmi.active = False
                off.append((name, space, region, i, kmi.propvalue, kmi.type, kmi.value))
        missing = [it for it in content["items"] if _modal_data_sig(it) not in have]
        before = {k.as_pointer() for k in km.keymap_items}
        keymap_init_from_data(km, missing, is_modal=True)
        _state["modal_added"].update(k.as_pointer() for k in _new_items(km, before))


def _restore_active():
    kc = _live_keyconfig(_state["kc_ptr"]) if _state["kc_ptr"] else None
    if kc is not None:
        added = _state["modal_added"]
        for km in kc.keymaps:
            if not km.is_modal or not added:
                continue
            for kmi in [k for k in km.keymap_items if k.as_pointer() in added]:
                km.keymap_items.remove(kmi)
        for name, space, region, i, ident, type, value in _state["off"]:
            km = _find(kc, name, space, region)
            if km is None or i >= len(km.keymap_items):
                continue
            kmi = km.keymap_items[i]
            if (kmi.propvalue if km.is_modal else kmi.idname) == ident and (kmi.type, kmi.value) == (type, value):
                kmi.active = True
        for name, space, region in _state["modal_made"]:
            km = _find(kc, name, space, region)
            if km is not None:
                kc.keymaps.remove(km)
    _state.update(kc_ptr=0, kc_name="", off=[], modal_made=[])
    _state["modal_added"].clear()


def _active_changed():
    """The active keyconfig is not the one apply() switched off (another preset, a rebuilt preset)."""
    kcs = bpy.context.window_manager.keyconfigs
    kc = kcs.active
    ptr = kc.as_pointer() if kc is not None else 0
    if ptr != _state["kc_ptr"]:
        return True
    if kc is None or not _state["off"]:
        return False
    # same address, rebuilt keyconfig: the first item switched off is on again
    name, space, region, i, ident, type, value = _state["off"][0]
    km = _find(kc, name, space, region)
    return km is None or i >= len(km.keymap_items) or km.keymap_items[i].active


def _event_sig(kmi):
    return (kmi.type, kmi.value, kmi.any, kmi.shift, kmi.ctrl, kmi.alt, kmi.oskey, kmi.key_modifier)


def _keep_first():
    """Maya keys first in Maya mode (step 265, Issue #178): Blender merges the add-on keyconfig into the keymaps
    putting each add-on item at the head in turn, so an item another add-on adds later (a "Draw" add-on's D in Object
    Mode) runs before the kit's same key and the Maya key never fires. When an active item of another add-on with the
    same event sits after a kit item in an add-on keymap, the kit's items of that keymap move to its end (= first
    again). Only runs when the add-on keyconfig's item count changed; Maya mode off removes the kit's items, so the
    other add-on's key works again. Returns the number of keymaps reordered."""
    kc = bpy.context.window_manager.keyconfigs.addon
    ptrs = _state["addon_items"]
    if kc is None or not ptrs:
        return 0
    count = sum(len(km.keymap_items) for km in kc.keymaps)
    if count == _state["addon_count"]:
        return 0
    moved = 0
    for km in kc.keymaps:
        if km.is_modal:
            continue
        kit_at = {}
        clash = False
        for i, kmi in enumerate(km.keymap_items):
            if kmi.as_pointer() in ptrs:
                kit_at.setdefault(_event_sig(kmi), i)
            elif kmi.active and _event_sig(kmi) in kit_at:
                clash = True
                break
        if not clash:
            continue
        kit = [k for k in km.keymap_items if k.as_pointer() in ptrs]
        for old in kit:
            new = km.keymap_items.new_from_item(old)
            ptrs.discard(old.as_pointer())
            ptrs.add(new.as_pointer())
            km.keymap_items.remove(old)
        moved += 1
    _state["addon_count"] = sum(len(km.keymap_items) for km in kc.keymaps)
    return moved


def _watch():
    if not _state["applied"]:
        return None
    if _active_changed():
        _restore_active()
        _apply_active(generate_data())
    _keep_first()
    return WATCH_INTERVAL


# --- public ----------------------------------------------------------------------------------------------------

def is_applied():
    return _state["applied"]


def activate():
    """Put the Maya keymap in place (idempotent). Never changes which keyconfig is active."""
    if _state["applied"]:
        return
    data = generate_data()
    _apply_addon(data)
    _apply_active(data)
    _state["applied"] = True
    if not bpy.app.timers.is_registered(_watch):
        bpy.app.timers.register(_watch, first_interval=0.0, persistent=True)


def deactivate():
    """Take the Maya keymap out again: add-on items removed, the active keyconfig's items as they were."""
    if bpy.app.timers.is_registered(_watch):
        bpy.app.timers.unregister(_watch)
    _restore_active()
    _restore_addon()
    _state["applied"] = False


class ALLSET_OT_keymap_activate(bpy.types.Operator):
    """Use the Maya keymap (add-on keymap items; your active keymap stays selected)"""
    bl_idname = "allset.keymap_activate"
    bl_label = "Activate Maya Keymap"
    bl_options = {'REGISTER', 'INTERNAL'}

    def execute(self, context):
        activate()
        return {'FINISHED'}


classes = (ALLSET_OT_keymap_activate,)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)
    activate()


def unregister():
    try:
        deactivate()
    except (ReferenceError, RuntimeError):
        pass  # Keyconfigs already freed (Blender quitting): nothing outlives the session anyway.
    for km, kmi in reversed(_addon_keymaps):
        try:
            km.keymap_items.remove(kmi)
        except (ReferenceError, RuntimeError):
            pass  # Keyconfig already freed (Blender quitting).
    _addon_keymaps.clear()
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
