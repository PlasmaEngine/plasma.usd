"""The opinions of Maya's layer that a stronger layer outvotes.

maya-usd authors plain USD into the edit target, so unlike Houdini's connector nothing has to be flattened or stripped:
Maya's participant layer is sent as it is. What is left to work out is which of its opinions the session composes
differently because a stronger participant's layer decides them. Maya's proxy stage is the session itself, so Maya
shows the stronger value for those, and the panel lists them.

outvoted() is the Houdini connector's (opinions.py there) with one more case. Moving a prim whose transform op already
exists, maya-usd sets that op's value and leaves xformOpOrder alone; when a stronger layer then decides an xformOpOrder
without that op (Plasma writes a single xformOp:transform), Maya's value has no effect and is reported as an outvoted
transform, although no stronger layer sets that attribute itself.
"""

import os

from pxr import Sdf, Usd


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


def _unused_op_order(prim, op_name, own):
    """The spec of a stronger layer deciding an xformOpOrder that leaves the op out, or None."""
    order = prim.GetAttribute("xformOpOrder")
    if not order:
        return None
    ops = order.Get()
    if ops is None or op_name in ops or "!invert!" + op_name in ops:
        return None
    stronger = _deciding(order.GetPropertyStack(Usd.TimeCode.Default()), _attribute_opinion)
    return stronger if stronger is not None and stronger.layer != own else None


def outvoted(stage, own):
    """The opinions of own (this Maya's participant layer) that the session composes differently because a stronger
    layer decides them: sorted (prim path, what, deciding layer name), what being "transform", "active" or a property
    name. stage is the session as every participant composes it."""
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
            if name.startswith("xformOp:"):
                order = _unused_op_order(prim, name, own)
                if order is not None:
                    report(path.GetPrimPath(), what, order)
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
