"""Houdini's opinions as one plain USD layer, and the ones the session outvotes.

What the LOP network authors after the session's Layer Break is Houdini's contribution to the session. Those layers
(the stage's root sublayers that ``layersAboveLayerBreak`` does not name) are flattened into one layer with
UsdUtils.FlattenLayerStack, which applies their strength order and list edits the way composition does. Then what only
Houdini understands is removed:
  * the HoudiniLayerInfo prim Houdini keeps in each layer it authors
  * customData keys starting with "Houdini" on prims, attributes and relationships (editor node ids, viewport preview
    tags, SOP data ids)
  * applied API schemas starting with "Houdini" (HoudiniEditableAPI and the like) and properties in the "houdini:"
    namespace
  * attributes left without any value once their Houdini customData is gone (the MaterialX inputs Houdini declares
    only to carry viewport preview tags)
  * prims left holding nothing
  * the layer's own metadata (upAxis, metersPerUnit, defaultPrim, frame rates): the session root decides those
What remains is the layer the other participants receive.
"""

import os

from pxr import Sdf, Usd, UsdUtils

HOUDINI_PREFIX = "Houdini"
HOUDINI_NAMESPACE = "houdini:"
LAYER_INFO_TYPE = "HoudiniLayerInfo"
LIST_OP_FIELDS = ("explicitItems", "prependedItems", "appendedItems", "addedItems", "deletedItems", "orderedItems")


def merge(identifiers):
    """A new anonymous layer holding the opinions of the given layers; identifiers are strongest first, as in
    subLayerPaths."""
    root = Sdf.Layer.CreateAnonymous("plasma_merge.usda")
    root.subLayerPaths = list(identifiers)
    # An empty population mask: flattening reads the layer stack, no prim needs composing.
    stage = Usd.Stage.OpenMasked(root, Usd.StagePopulationMask(), Usd.Stage.LoadNone)
    return UsdUtils.FlattenLayerStack(stage)


def _strip_custom_data(spec):
    """Removes the Houdini keys from a spec's customData; True when there were any."""
    if not spec.HasInfo("customData"):
        return False
    data = dict(spec.GetInfo("customData"))
    kept = {k: v for k, v in data.items() if not k.startswith(HOUDINI_PREFIX)}
    if len(kept) == len(data):
        return False
    if kept:
        spec.SetInfo("customData", kept)
    else:
        spec.ClearInfo("customData")
    return True


def _strip_api_schemas(prim):
    if not prim.HasInfo("apiSchemas"):
        return
    old = prim.GetInfo("apiSchemas")
    if old.isExplicit:
        items = [t for t in old.explicitItems if not t.startswith(HOUDINI_PREFIX)]
        if len(items) != len(old.explicitItems):
            prim.SetInfo("apiSchemas", Sdf.TokenListOp.CreateExplicit(items))
        return
    new = Sdf.TokenListOp()
    changed = False
    empty = True
    for field in LIST_OP_FIELDS[1:]:
        items = list(getattr(old, field))
        kept = [t for t in items if not t.startswith(HOUDINI_PREFIX)]
        changed |= len(kept) != len(items)
        empty &= not kept
        if kept:
            setattr(new, field, kept)
    if not changed:
        return
    if empty:
        prim.ClearInfo("apiSchemas")
    else:
        prim.SetInfo("apiSchemas", new)


def _has_value(attr):
    return (attr.HasDefaultValue() or attr.layer.GetNumTimeSamplesForPath(attr.path) > 0
            or len(attr.connectionPathList.GetAddedOrExplicitItems()) > 0)


def _strip_prim(prim):
    _strip_custom_data(prim)
    _strip_api_schemas(prim)
    for prop in list(prim.properties):
        if prop.name.startswith(HOUDINI_NAMESPACE):
            prim.RemoveProperty(prop)
            continue
        had_houdini_data = _strip_custom_data(prop)
        if had_houdini_data and isinstance(prop, Sdf.AttributeSpec) and not _has_value(prop):
            prim.RemoveProperty(prop)
    for child in list(prim.nameChildren):
        _strip_prim(child)


def strip(layer, documentation=""):
    """Removes Houdini's own data from a layer, in place."""
    edit = Sdf.BatchNamespaceEdit()
    for prim in list(layer.rootPrims):
        if prim.typeName == LAYER_INFO_TYPE:
            edit.Add(prim.path, Sdf.Path.emptyPath)
    if edit.edits:
        layer.Apply(edit)

    for prim in list(layer.rootPrims):
        _strip_prim(prim)
    layer.RemoveInertSceneDescription()

    root = layer.pseudoRoot
    for key in list(root.ListInfoKeys()):
        root.ClearInfo(key)
    layer.subLayerPaths = []
    if documentation:
        layer.documentation = documentation


# -- outvoted opinions -----------------------------------------------------------------------------------------------


def layer_name(layer):
    path = layer.realPath or layer.identifier
    return os.path.splitext(os.path.basename(path))[0]


def _is_transform(name):
    return name == "xformOpOrder" or name.startswith("xformOp:")


def _attribute_opinion(spec):
    return spec.HasDefaultValue() or spec.layer.GetNumTimeSamplesForPath(spec.path) > 0


def _deciding(stack, has_opinion):
    """The strongest spec in a stack (strongest first) that holds an opinion, or None."""
    for spec in stack:
        if has_opinion(spec):
            return spec
    return None


def outvoted(stage, own):
    """The opinions of own (this Houdini's participant layer) that the session composes differently because a stronger
    layer decides them: sorted (prim path, what, deciding layer name), what being "transform", "active" or a property
    name. stage is the session as every participant composes it, with own unmuted."""
    found = {}

    def report(prim_path, what, spec):
        key = (str(prim_path), what)
        if key not in found:
            found[key] = layer_name(spec.layer) if spec is not None else "a stronger layer"

    def visit(path):
        if path.IsPrimPath():
            spec = own.GetPrimAtPath(path)
            prim = stage.GetPrimAtPath(path)
            if spec is None or not prim:
                return
            if spec.HasInfo("active"):
                if prim.IsActive() != spec.active:
                    report(path, "active", _deciding(prim.GetPrimStack(), lambda s: s.HasInfo("active")))
            elif spec.specifier == Sdf.SpecifierDef and not prim.IsActive():
                stronger = _deciding(prim.GetPrimStack(), lambda s: s.HasInfo("active"))
                report(path, "active", stronger)
            return

        if not path.IsPropertyPath():
            return
        spec = own.GetPropertyAtPath(path)
        prim = stage.GetPrimAtPath(path.GetPrimPath())
        if spec is None or not prim or not prim.IsActive():
            return
        name = path.name
        what = "transform" if _is_transform(name) else name

        if isinstance(spec, Sdf.AttributeSpec):
            if not spec.HasDefaultValue():
                return
            attr = prim.GetAttribute(name)
            if not attr:
                return
            stronger = _deciding(attr.GetPropertyStack(Usd.TimeCode.Default()), _attribute_opinion)
            if stronger is None or stronger.layer == own:
                return
            if attr.Get() != spec.default:
                report(path.GetPrimPath(), what, stronger)

        elif isinstance(spec, Sdf.RelationshipSpec):
            ours = spec.targetPathList
            wanted = list(ours.explicitItems) if ours.isExplicit else list(ours.prependedItems) + list(
                ours.appendedItems) + list(ours.addedItems)
            if not wanted:
                return
            rel = prim.GetRelationship(name)
            if not rel:
                return
            targets = set(rel.GetTargets())
            if all(t in targets for t in wanted):
                return
            stronger = _deciding(rel.GetPropertyStack(),
                                 lambda s: s.layer != own and len(s.targetPathList.GetAddedOrExplicitItems()) > 0)
            report(path.GetPrimPath(), what, stronger)

    own.Traverse(Sdf.Path.absoluteRootPath, visit)
    return sorted((path, what, by) for (path, what), by in found.items())
