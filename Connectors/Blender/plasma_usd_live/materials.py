"""Material values synced between Blender and the session's USD materials.

Blender's USD importer names each material after its USD Material prim, so at import every new Blender material is
matched to a Material prim of the same name (ignoring Blender's ".001" suffixes; a bound material decides between prims
that share a name) and tagged with ``usd_material_path``.

Synced both ways: the Principled BSDF's Base Color, Metallic, Roughness and Alpha, and the matching input of the USD
material's surface shader (``ComputeSurfaceSource`` in the universal render context, then "mtlx"), chosen by the
shader's info:id (see SHADER_INPUTS). Colours are linear on both sides and copied as they are.
  * Blender -> USD: a changed value is written into Blender's layer. A shader input connected to a material interface
    input writes that interface input instead; an input driven by another node's output (a texture, say) is left alone.
  * USD -> Blender: a composed value another layer changes is set on the BSDF input, unless a node drives that input
    in Blender.
"""

import re

import bpy

bpy.utils.expose_bundled_modules()
from pxr import Gf, Sdf, UsdShade  # noqa: E402

MATERIAL_PATH_PROP = "usd_material_path"

NAME_SUFFIX = re.compile(r"\.\d{3,}$")

KEYS = ("base_color", "metallic", "roughness", "alpha")

BSDF_INPUTS = {
    "base_color": "Base Color",
    "metallic": "Metallic",
    "roughness": "Roughness",
    "alpha": "Alpha",
}

_PREVIEW_SURFACE = {
    "base_color": "diffuseColor",
    "metallic": "metallic",
    "roughness": "roughness",
    "alpha": "opacity",
}

# Shader input for each value, by the surface shader's info:id.
SHADER_INPUTS = {
    "UsdPreviewSurface": _PREVIEW_SURFACE,
    "ND_UsdPreviewSurface_surfaceshader": _PREVIEW_SURFACE,
    "ND_open_pbr_surface_surfaceshader": {
        "base_color": "base_color",
        "metallic": "base_metalness",
        "roughness": "specular_roughness",
        "alpha": "geometry_opacity",
    },
    "ND_standard_surface_surfaceshader": {
        "base_color": "base_color",
        "metallic": "metalness",
        "roughness": "specular_roughness",
        "alpha": "opacity",
    },
    "ND_gltf_pbr_surfaceshader": {
        "base_color": "base_color",
        "metallic": "metallic",
        "roughness": "roughness",
        "alpha": "alpha",
    },
}

# Inputs that are colours; the others are floats. Used when an input is authored for the first time.
COLOR_INPUTS = {
    ("ND_standard_surface_surfaceshader", "opacity"),
}

VECTOR_TYPES = (Gf.Vec3f, Gf.Vec3d, Gf.Vec3h)

TOLERANCE = 1e-5


def is_close(a, b):
    if isinstance(a, tuple) or isinstance(b, tuple):
        if not (isinstance(a, tuple) and isinstance(b, tuple)) or len(a) != len(b):
            return False
        return all(abs(x - y) <= TOLERANCE for x, y in zip(a, b))
    return abs(a - b) <= TOLERANCE


# -- USD side -------------------------------------------------------------------------------------------------------


def surface_shader(stage, path):
    """(shader, info:id) of a material's surface shader, or (None, None)."""
    prim = stage.GetPrimAtPath(path)
    if not prim or not prim.IsA(UsdShade.Material):
        return None, None
    material = UsdShade.Material(prim)
    for context in ("", "mtlx"):
        shader = material.ComputeSurfaceSource(context)[0]
        if shader:
            return shader, shader.GetShaderId()
    return None, None


def is_output(attr):
    return UsdShade.Utils.GetType(attr.GetName()) == UsdShade.AttributeType.Output


def value_attribute(shader, shader_id, key, create):
    """The attribute holding the value of a shader input: the input itself, or the material interface input it is
    connected to. None when another node's output drives it, or when it is not authored and create is False."""
    name = SHADER_INPUTS[shader_id][key]
    shader_input = shader.GetInput(name)
    if shader_input:
        producing = shader_input.GetValueProducingAttributes(False)
        if producing:
            return None if is_output(producing[0]) else producing[0]
        sources = shader_input.GetConnectedSources()[0]
        if sources:
            source = sources[0]
            if source.sourceType == UsdShade.AttributeType.Output:
                return None
            interface = source.source.GetInput(source.sourceName)
            if interface:
                return interface.GetAttr()
            if not create:
                return None
            return source.source.CreateInput(source.sourceName, value_type(shader_id, key, name)).GetAttr()
        return shader_input.GetAttr()
    if not create:
        return None
    return shader.CreateInput(name, value_type(shader_id, key, name)).GetAttr()


def value_type(shader_id, key, name):
    if key == "base_color" or (shader_id, name) in COLOR_INPUTS:
        return Sdf.ValueTypeNames.Color3f
    return Sdf.ValueTypeNames.Float


def read_usd(stage, path):
    """{key: value} of every value the material's surface shader holds; colours as (r, g, b), the rest as floats."""
    shader, shader_id = surface_shader(stage, path)
    if shader is None or shader_id not in SHADER_INPUTS:
        return {}
    values = {}
    for key in KEYS:
        attr = value_attribute(shader, shader_id, key, create=False)
        if attr is None:
            continue
        value = attr.Get()
        if value is None:
            continue
        if isinstance(value, VECTOR_TYPES):
            value = (float(value[0]), float(value[1]), float(value[2]))
            if key != "base_color":
                value = sum(value) / 3.0
        elif key == "base_color":
            continue
        else:
            value = float(value)
        values[key] = value
    return values


def write_usd(stage, path, key, value):
    """Authors one value in the stage's edit target. Returns the composed value afterwards, or None when the material
    has no input for it that a value can drive."""
    shader, shader_id = surface_shader(stage, path)
    if shader is None or shader_id not in SHADER_INPUTS:
        return None
    attr = value_attribute(shader, shader_id, key, create=True)
    if attr is None:
        return None
    if attr.GetTypeName().type.pythonClass in VECTOR_TYPES:
        cls = attr.GetTypeName().type.pythonClass
        attr.Set(cls(*value) if isinstance(value, tuple) else cls(value, value, value))
    else:
        attr.Set(float(sum(value) / 3.0 if isinstance(value, tuple) else value))
    return read_usd(stage, path).get(key)


# -- Blender side ---------------------------------------------------------------------------------------------------


def find_bsdf(material):
    tree = material.node_tree
    if tree is None:
        return None
    output = tree.get_output_node('ALL')
    if output is not None:
        surface = output.inputs.get("Surface")
        if surface is not None and surface.is_linked:
            node = surface.links[0].from_node
            if node.type == 'BSDF_PRINCIPLED':
                return node
    for node in tree.nodes:
        if node.type == 'BSDF_PRINCIPLED':
            return node
    return None


def read_socket(socket, key):
    if key == "base_color":
        return tuple(float(v) for v in socket.default_value[:3])
    return float(socket.default_value)


def write_socket(socket, key, value):
    if key == "base_color":
        socket.default_value = (value[0], value[1], value[2], socket.default_value[3])
    else:
        socket.default_value = value


def base_color(material):
    """The (r, g, b) a material shows: its Principled BSDF's Base Color, or its viewport colour."""
    bsdf = find_bsdf(material)
    if bsdf is not None:
        socket = bsdf.inputs.get("Base Color")
        if socket is not None and not socket.is_linked:
            return read_socket(socket, "base_color")
    return tuple(material.diffuse_color[:3])


class MaterialState:
    """What this Blender last knew about one mapped material."""

    __slots__ = ("name", "path", "usd", "blender")

    def __init__(self, name, path):
        self.name = name  # the Blender material; copies of it are unlinked
        self.path = path
        self.usd = {}  # composed values last applied to Blender
        self.blender = {}  # Blender values when last synced


class MaterialSync:
    def __init__(self, link):
        self.link = link
        self.materials = {}

    def map_imported(self, new_materials):
        """Tags the materials Blender's importer just created with the path of their USD material."""
        stage = self.link.stage
        by_name = {}
        for prim in stage.Traverse():
            if prim.IsA(UsdShade.Material):
                by_name.setdefault(prim.GetName(), []).append(str(prim.GetPath()))

        ambiguous = []
        for material in new_materials:
            candidates = by_name.get(NAME_SUFFIX.sub("", material.name), [])
            if len(candidates) > 1:
                bound = self.bound_materials(material) & set(candidates)
                candidates = sorted(bound)
            if len(candidates) != 1:
                if candidates:
                    ambiguous.append(material.name)
                continue
            material[MATERIAL_PATH_PROP] = candidates[0]
            self.materials[candidates[0]] = MaterialState(material.name, candidates[0])

        if ambiguous:
            self.link.note("Not synced, several USD materials match: " + ", ".join(ambiguous))

    def bound_materials(self, material):
        """Paths of the USD materials bound to the prims of the linked objects that use a Blender material."""
        from .live_link import PRIM_PATH_PROP

        stage = self.link.stage
        paths = set()
        for obj in bpy.data.objects:
            path = obj.get(PRIM_PATH_PROP)
            if path is None or not any(slot.material == material for slot in obj.material_slots):
                continue
            prim = stage.GetPrimAtPath(path)
            if not prim:
                continue
            binding = UsdShade.MaterialBindingAPI(prim)
            for target in [binding] + [UsdShade.MaterialBindingAPI(s.GetPrim())
                                       for s in binding.GetMaterialBindSubsets()]:
                bound = target.ComputeBoundMaterial()[0]
                if bound:
                    paths.add(str(bound.GetPath()))
        return paths

    def adopt(self):
        """Picks up the materials an earlier import in this file tagged."""
        stage = self.link.stage
        for material in bpy.data.materials:
            path = material.get(MATERIAL_PATH_PROP)
            if not path or path in self.materials:
                continue
            prim = stage.GetPrimAtPath(path)
            if prim and prim.IsA(UsdShade.Material):
                self.materials[path] = MaterialState(material.name, path)

    def resolve(self, material):
        """The state of the USD material a Blender material drives, or None. A copy of a mapped material is unlinked."""
        path = material.get(MATERIAL_PATH_PROP)
        if path is None:
            return None
        st = self.materials.get(path)
        if st is None:
            return None
        if material.name != st.name:
            owner = bpy.data.materials.get(st.name)
            if owner is not None and owner.get(MATERIAL_PATH_PROP) == path:
                del material[MATERIAL_PATH_PROP]
                return None
            st.name = material.name
        return st

    def material_of(self, st):
        material = bpy.data.materials.get(st.name)
        if material is not None and material.get(MATERIAL_PATH_PROP) == st.path:
            return material
        for material in bpy.data.materials:
            if self.resolve(material) is st:
                return material
        return None

    def pull(self):
        """Applies composed values that changed since the last pull. Returns the number of Blender materials changed."""
        stage = self.link.stage
        changed = 0
        for st in self.materials.values():
            material = self.material_of(st)
            if material is None:
                continue
            bsdf = find_bsdf(material)
            if bsdf is None:
                continue
            values = read_usd(stage, st.path)
            touched = False
            for key in KEYS:
                socket = bsdf.inputs.get(BSDF_INPUTS[key])
                if socket is None:
                    continue
                value = values.get(key)
                if value is not None and (st.usd.get(key) is None or not is_close(st.usd[key], value)):
                    st.usd[key] = value
                    if not socket.is_linked and not is_close(read_socket(socket, key), value):
                        write_socket(socket, key, value)
                        touched = True
                st.blender[key] = read_socket(socket, key)
            changed += touched
        return changed

    def push(self):
        """Authors the BSDF values the user changed since the last sync. Returns the number of values authored."""
        stage = self.link.stage
        authored = 0
        for material in list(bpy.data.materials):
            st = self.resolve(material)
            if st is None:
                continue
            bsdf = find_bsdf(material)
            if bsdf is None:
                continue
            for key in KEYS:
                socket = bsdf.inputs.get(BSDF_INPUTS[key])
                if socket is None or socket.is_linked:
                    continue
                value = read_socket(socket, key)
                old = st.blender.get(key)
                st.blender[key] = value
                if old is None or is_close(old, value):
                    continue
                composed = write_usd(stage, st.path, key, value)
                if composed is not None:
                    st.usd[key] = composed
                    authored += 1
        return authored
