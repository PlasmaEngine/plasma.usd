# SPDX-License-Identifier: GPL-3.0-or-later
"""Maya 2022 polySuperShape (Create > Polygon Primitives > Super Ellipse / Spherical Harmonics / Ultra Shape).

Pure Python (no bpy), Maya Y-up space and Maya units, returning ``prim_shapes.Mesh``. Every rule follows
Maya 2022 (factory menu items, then one attribute changed at a time).
No Maya code or data was copied.

Measured rules in brief:

- One node type for all three menu items; they differ only by ``shape`` (0 Super Ellipse, 1 Spherical Harmonics,
  2 Ultra). The menu passes radius 1, 16 / 16 divisions, createUV 2 (sawtooth poles), merge on, the factory values.
- Grid: n = 4 x horizontalDivisions columns around Y, R = 4 x verticalDivisions rows from the bottom pole to the
  top one. Angles are float32: latitude b_i = f32(f32(i * f32(pi / R)) - f32(pi / 2)), longitude
  t_j = f32(j * f32(2 pi / n)); Maya's around angle is phi = -t (x = cos phi, z = sin phi). The float32 angles
  matter: cos(f32(pi / 2)) = -4.4e-8, raised to a small exponent it moves points by up to 0.04.
- Super Ellipse: x = C(b, e0) C(phi, e1), y = S(b, e0), z = C(b, e0) S(phi, e1) with C / S = signed powers
  sign(c) |c|^e of cos / sin (0 stays 0); Ellipse Mirror on = e1 is ignored (e0 used for both).
- Spherical Harmonics: r = (P(sin(h0 b), h1) + P(cos(h2 b), h3) + P(sin(h4 phi), h5) + P(cos(h6 phi), h7)) / 2
  (P = signed power), point = r (cos b cos phi, sin b, cos b sin phi). Defaults = (0 + 1 + 0 + 1) / 2 = sphere.
- Ultra Shape: two blended superformulas per direction, u0..u7 around (angle phi / 4), u8..u15 up (angle b):
  F(a) = ((1 - mix) (|cos(m1 a)|^e2 + |sin(m1 a)|^e1) + mix (|cos(m2 a)|^e4 + |sin(m2 a)|^e3)) ^ -e5 with
  (m1, e1, e2, mix, m2, e3, e4, e5) = the eight attributes in order; point = (r1 r2 cos b cos phi, r2 sin b,
  r1 r2 cos b sin phi). Ultra Mirror on = the vertical set copies the horizontal one.
- Radius scales the point. Horizontal / Vertical Revolutions, Vertical Offset, Internal Radius, X / Z Offset change
  nothing in Maya 2022 (on all three shapes, alone and together).
- Vertices column-major: column 0 rows 0..R (both poles), then each further column rows 1..R-1; column n = column 0.
  Faces row-major (rows from the bottom, columns inner): quad (i, j) (i, j+1) (i+1, j+1) (i+1, j) with merged
  corners dropped, so the poles are triangle fans (n x R faces, n x (R - 1) + 2 vertices). Merge Vertices off: the
  full (n + 1) x (R + 1) grid, quads only, open seam and degenerate pole quads.
- UVs: u = j / n, v = i / R; createUV 2 (menu) gives each pole triangle its own pole UV at u = (j + 0.5) / n,
  createUV 1 one pole UV at u = 0.5, createUV 0 none; without merging the pole corners keep the grid UVs.
- Hard edges: normals more than 30 degrees apart, and every border edge (Merge Vertices off).
"""

import math
import struct

from .prim_shapes import Mesh, _a

# menu command -shape value -> shape enum index; MENU_LABELS: Create > Polygon Primitives label -> -shape value
SHAPES = {"SuperEllipse": 0, "SphericalHarmonics": 1, "UltraShape": 2}
MENU_LABELS = {"Super Ellipse": "SuperEllipse", "Spherical Harmonics": "SphericalHarmonics",
               "Ultra Shape": "UltraShape"}
SHAPE_ENUM = ("enum", ("Super Ellipse", "Spherical Harmonics", "Ultra"))

_H_DEFAULTS = (0.0, 1.0, 0.0, 1.0, 0.0, 1.0, 0.0, 1.0)
_U_DEFAULTS = (0.0, 1.0, 1.0, 0.5, 0.0, 1.0, 1.0, 1.0) * 2

# Channel Box order (Maya listAttr -k); ``shape`` is Maya's non-keyable enum, placed where Maya's attribute order
# has it (right after radius). Limits = Maya hard min / max (soft ranges not enforced).
NODES = {
    "polySuperShape": [
        _a("radius", "Radius", 1.0, "length", 0.001),
        _a("shape", "Shape", 0, SHAPE_ENUM),
        _a("ellipse0", "Ellipse 0", 1.0, "float"), _a("ellipse1", "Ellipse 1", 1.0, "float"),
        *[_a(f"harmonics{i}", f"Harmonics {i}", d, "float", None if i % 2 == 0 else 0.0)
          for i, d in enumerate(_H_DEFAULTS)],
        *[_a(f"ultra{i}", f"Ultra {i}", d, "float", None if i % 4 == 0 else 0.0, 1.0 if i % 8 == 3 else None)
          for i, d in enumerate(_U_DEFAULTS)],
        _a("ellipseMirror", "Ellipse Mirror", True, "bool"), _a("ultraMirror", "Ultra Mirror", False, "bool"),
        _a("horizontalDivisions", "Horizontal Divisions", 16, "int", 1),
        _a("verticalDivisions", "Vertical Divisions", 16, "int", 1),
        _a("mergeVertices", "Merge Vertices", True, "bool"),
        _a("horizontalRevolutions", "Horizontal Revolutions", 1.0, "float", 0.0),
        _a("verticalRevolutions", "Vertical Revolutions", 1.0, "float", 0.0),
        _a("verticalOffset", "Vertical Offset", 0.0, "float", 0.0),
        _a("internalRadius", "Internal Radius", 0.0, "float", 0.0),
        _a("xOffset", "X Offset", 0.0, "float", 0.0), _a("zOffset", "Z Offset", 0.0, "float", 0.0),
    ],
}

HARD_ANGLE = 30.0
_PACK = struct.Struct("f")


def _f32(x):
    return _PACK.unpack(_PACK.pack(x))[0]


def _pw(c, e):
    """Signed power sign(c) |c|^e; a zero base stays 0 (also with exponent 0)."""
    if c == 0.0:
        return 0.0
    return math.copysign(abs(c) ** e, c)


def _superformula(a, u):
    m1, e1, e2, mix, m2, e3, e4, e5 = u
    s1 = abs(math.cos(m1 * a)) ** e2 + abs(math.sin(m1 * a)) ** e1
    s2 = abs(math.cos(m2 * a)) ** e4 + abs(math.sin(m2 * a)) ** e3
    s = (1.0 - mix) * s1 + mix * s2
    return s ** -e5 if s > 0.0 else 0.0


def _point_fn(a):
    shape, rad = int(a.get("shape", 0)), a["radius"]
    if shape == 0:
        e0 = a["ellipse0"]
        e1 = e0 if a["ellipseMirror"] else a["ellipse1"]

        def point(b, phi):
            cb = _pw(math.cos(b), e0)
            return (rad * cb * _pw(math.cos(phi), e1), rad * _pw(math.sin(b), e0), rad * cb * _pw(math.sin(phi), e1))
        return point
    if shape == 1:
        h = [a[f"harmonics{i}"] for i in range(8)]

        def point(b, phi):
            r = rad * (_pw(math.sin(h[0] * b), h[1]) + _pw(math.cos(h[2] * b), h[3])
                       + _pw(math.sin(h[4] * phi), h[5]) + _pw(math.cos(h[6] * phi), h[7])) / 2.0
            cb = math.cos(b)
            return (r * cb * math.cos(phi), r * math.sin(b), r * cb * math.sin(phi))
        return point
    u = [a[f"ultra{i}"] for i in range(16)]
    hu, vu = u[:8], (u[:8] if a["ultraMirror"] else u[8:])

    def point(b, phi):
        r1, r2 = _superformula(phi / 4.0, hu), rad * _superformula(b, vu)
        cb = math.cos(b)
        return (r1 * r2 * cb * math.cos(phi), r2 * math.sin(b), r1 * r2 * cb * math.sin(phi))
    return point


def _hard_edges(verts, faces):
    """Edges whose two face normals (Newell) are more than ``HARD_ANGLE`` apart, plus border edges."""
    normals = []
    for f in faces:
        nx = ny = nz = 0.0
        for i in range(len(f)):
            p, q = verts[f[i - 1]], verts[f[i]]
            nx += (p[1] - q[1]) * (p[2] + q[2])
            ny += (p[2] - q[2]) * (p[0] + q[0])
            nz += (p[0] - q[0]) * (p[1] + q[1])
        n = math.sqrt(nx * nx + ny * ny + nz * nz) or 1.0
        normals.append((nx / n, ny / n, nz / n))
    owner = {}
    for fi, f in enumerate(faces):
        for i in range(len(f)):
            owner.setdefault(frozenset((f[i], f[i - 1])), []).append(fi)
    c = math.cos(math.radians(HARD_ANGLE))
    hard = set()
    for e, fs in owner.items():
        if len(fs) != 2:
            hard.add(e)
            continue
        p, q = normals[fs[0]], normals[fs[1]]
        if p[0] * q[0] + p[1] * q[1] + p[2] * q[2] < c:
            hard.add(e)
    return frozenset(hard)


def poly_super_shape(a):
    n, R = 4 * int(a["horizontalDivisions"]), 4 * int(a["verticalDivisions"])
    merge, uv_mode = bool(a["mergeVertices"]), int(a.get("uvMode", 2))
    point = _point_fn(a)
    step_b, half_pi, step_t = _f32(math.pi / R), _f32(math.pi / 2), _f32(2 * math.pi / n)
    lat = [_f32(_f32(i * step_b) - half_pi) for i in range(R + 1)]
    lon = [-_f32(j * step_t) for j in range(n + 1)]

    verts, index = [], {}
    for j in range(n + 1):
        for i in range(R + 1):
            if merge and (j == n or (j > 0 and i in (0, R))):
                continue
            index[(i, j)] = len(verts)
            verts.append(point(lat[i], lon[j]))
    if merge:
        def vid(i, j):
            return index[(i, 0)] if j == n or i in (0, R) else index[(i, j)]
    else:
        def vid(i, j):
            return index[(i, j)]

    faces, uvs = [], ([] if uv_mode else None)
    for i in range(R):
        for j in range(n):
            corners = ((i, j), (i, j + 1), (i + 1, j + 1), (i + 1, j))
            face, fuv = [], []
            for ci, cj in corners:
                v = vid(ci, cj)
                if v in face:
                    continue
                face.append(v)
                if uvs is None:
                    continue
                if merge and ci in (0, R):
                    u = (j + 0.5) / n if uv_mode == 2 else 0.5
                else:
                    u = cj / n
                fuv.append((u, ci / R))
            faces.append(face)
            if uvs is not None:
                uvs.append(fuv)
    return Mesh(verts, faces, uvs, _hard_edges(verts, faces))


POLY = {"polySuperShape": poly_super_shape}


def build(attrs):
    """``poly_super_shape`` on the node defaults + ``attrs`` (clamped like ``prim_shapes.clamp``)."""
    a = {s[0]: s[2] for s in NODES["polySuperShape"]}
    a.update(attrs)
    out = {}
    for s in NODES["polySuperShape"]:
        name, _l, _d, kind, lo, hi = s
        v = a[name]
        if kind == "bool":
            v = bool(v)
        elif isinstance(kind, tuple):
            v = max(0, min(len(kind[1]) - 1, int(round(v))))
        else:
            v = int(round(v)) if kind == "int" else float(v)
            v = v if lo is None else max(lo, v)
            v = v if hi is None else min(hi, v)
        out[name] = v
    if "uvMode" in a:
        out["uvMode"] = a["uvMode"]
    return poly_super_shape(out)

