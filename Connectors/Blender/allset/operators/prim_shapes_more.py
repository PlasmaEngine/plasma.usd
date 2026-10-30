# SPDX-License-Identifier: GPL-3.0-or-later
"""Maya 2022 Pipe / Helix / Gear / Soccer Ball (Create > Polygon Primitives) as data, like ``prim_shapes``.

Pure Python (no bpy), Maya Y-up space and Maya units, ``prim_shapes.Mesh`` results. Every rule follows
Maya 2022 (factory Create menu commands).
No Maya code or data was copied; vertex order, face lists (start corner
and winding), UVs per face corner and hard edges were compared case by case with Maya's output.

polyPipe: rings of ``subdivisionsAxis`` vertices, vertex i at angle -360 i / n in (x, z) (ring starts on +X, unlike
polyCylinder). Ring order = the cross-section loop: inner wall bottom -> top, top cap inner -> outer, outer wall top
-> bottom, bottom cap outer -> inner (caps add ``subdivisionsCaps`` - 1 rings; round cap = a half circle of radius
thickness / 2 around the mid radius). One quad strip per loop segment; UV u = i / n, v = 1 - k / K over the K loop
segments. Hard: the four wall rims (not with Round Cap) and the wall verticals when a side spans >= 30 degrees.

polyHelix: ``round(coils) * subdivisionsCoil`` steps (at least one coil's worth; half rounds up) over
``coils`` turns; each step a section of ``subdivisionsAxis`` vertices in the vertical plane through the axis, coil
radius width / 2 - radius, rising from -height / 2 to +height / 2, turning from +X towards -Z (Direction 1,
Maya's "Counterclockwise" seen from +Y) or towards +Z (0, "Clockwise"; the section is mirrored to stay outward).
Caps: flat or round rings along the horizontal tangent, the bottom cap's rings first (tip -> rim), the top cap's
after the body, then the two cap centres; ``subdivisionsCaps`` 0 = one n-gon each. UVs = Maya's cylinder layout
(body u 0.375..0.625, v 0.3125..0.68844; cap circles r 0.15625). Hard: the lengthwise body edges when a section side
spans >= 30 degrees, the cap rims unless Round Cap.

polyGear (modelingToolkit): an n-gon of radius ``radius`` whose corners are cut, one six-point tooth on every side
(side k faces 90 - 360 k / n degrees in (x, z): +Z first, then towards +X; base = gearSpacing of the side, tip =
gearTip of the base at gearOffset, middle width ((1 + tip) / 2) * gearMiddle of the base at half the gearOffset), an
inner ring of 2n points (``internalRadius``; 0 = one centre vertex), heightDivisions + 1
levels from top to bottom, twist = rotation of level l by twist * l / H, taper = outline scale 1 + (taper - 1) l / H
(the inner ring is twisted, not tapered). Hard: edges whose faces meet at >= 30 degrees.

polyPrimitiveMisc (Soccer Ball): a truncated icosahedron of circumradius ``radius`` with a vertex on +Y; Side Length
= radius * 0.4036 (Maya's constant, linked both ways). Vertex / face order and the UV net are Maya's, rebuilt from
the icosahedron (see ``_soccer``).
"""

import math

from . import prim_shapes as ps

Mesh, _a, AXIS = ps.Mesh, ps._a, ps.AXIS

# Channel Box (keyable) attributes in Maya's order with Maya's nice names, then the non-keyable ones the kit edits
# like prim_shapes' Round Cap (polyHelix Direction: enum Clockwise / Counterclockwise, menu 1). Defaults = the Create
# menu commands; limits = the attributes' minimum / maximum (no maxima unless listed).
# polyPipe Subdivisions Caps: the menu creates 0 although setAttr's minimum is 1; 0 and 1 build the same pipe.
NODES = {
    "polyPipe": [*AXIS, _a("radius", "Radius", 1.0, "length", 0.01), _a("height", "Height", 2.0, "length", 0.01),
                 _a("thickness", "Thickness", 0.5, "length", 0.01),
                 _a("subdivisionsAxis", "Subdivisions Axis", 20, "int", 3),
                 _a("subdivisionsHeight", "Subdivisions Height", 1, "int", 1),
                 _a("subdivisionsCaps", "Subdivisions Caps", 0, "int", 0),
                 _a("roundCap", "Round Cap", False, "bool")],
    "polyHelix": [*AXIS, _a("coils", "Coils", 3.0, "float", 0.5), _a("height", "Height", 2.0, "length", 0.01),
                  _a("width", "Width", 2.0, "length", 0.01), _a("radius", "Radius", 0.4, "length", 0.01),
                  _a("subdivisionsAxis", "Subdivisions Axis", 8, "int", 3, 1001),
                  _a("subdivisionsCoil", "Subdivisions Coil", 50, "int", 2, 10001),
                  _a("subdivisionsCaps", "Subdivisions Caps", 0, "int", 0, 1001),
                  _a("roundCap", "Round Cap", False, "bool"),
                  _a("direction", "Direction", 1, ("enum", ("Clockwise", "Counterclockwise")))],
    "polyGear": [_a("sides", "Sides", 16, "int", 3), _a("radius", "Radius", 1.0, "length", 0.001),
                 _a("internalRadius", "Internal Radius", 0.3, "length", 0.0),
                 _a("height", "Height", 1.0, "length", 0.001),
                 _a("heightDivisions", "Height Divisions", 10, "int", 1),
                 _a("gearSpacing", "Gear Spacing", 0.6, "float", 0.0),
                 _a("gearOffset", "Gear Offset", 0.2, "length", 0.0),
                 _a("gearTip", "Gear Tip", 0.5, "float", 0.0), _a("gearMiddle", "Gear Middle", 1.2, "float", 0.0),
                 _a("twist", "Twist", 0.0, "angle"), _a("taper", "Taper", 1.0, "float", 0.001)],
    "polyPrimitiveMisc": [*AXIS, _a("radius", "Radius", 1.0, "length", 0.01),
                          _a("sideLength", "Side Length", 0.4036, "length", 0.01)],
}
# Soccer Ball: Maya keeps Side Length = Radius * 0.4036 (both ways: radius 2 -> 0.8072, side 1 -> radius 2.4777;
# Maya 2022); the shape follows the radius.
SOCCER_SIDE = 0.4036


def orient_z(verts, a):
    """polyPipe / polyPrimitiveMisc axis: the +Y build is first turned +Y -> +Z (90 degrees about X), then by the
    shortest rotation +Z -> axis (pipe_sa6_ax100: (0.5, -0.5, 0) -> (-0.5, 0, -0.5); ax111, ax001, axm; soccer_*).
    Not the shortest +Y -> axis of polyCube / polyCylinder (``prim_shapes._orient``), which polyHelix uses."""
    x, y, z = a.get("axisX", 0.0), a.get("axisY", 1.0), a.get("axisZ", 0.0)
    n = math.sqrt(x * x + y * y + z * z)
    if n < 1e-12:
        return verts
    x, y, z = x / n, y / n, z / n
    rx = ((1.0, 0.0, 0.0), (0.0, 0.0, -1.0), (0.0, 1.0, 0.0))
    if z > 1 - 1e-12:
        m = rx
    elif z < -1 + 1e-12:
        m = _mul(Z_FLIP, rx)
    else:
        kx, ky = -y, x   # rotation axis Z x a = (-y, x, 0), angle acos(z)
        s = math.sqrt(kx * kx + ky * ky)
        kx, ky = kx / s, ky / s
        c, sn, t = z, s, 1 - z
        m = _mul(((t * kx * kx + c, t * kx * ky, sn * ky),
                  (t * kx * ky, t * ky * ky + c, -sn * kx),
                  (-sn * ky, sn * kx, c)), rx)
    return [ps._apply(m, p) for p in verts]


# Axis 0 0 -1 (+Z -> -Z has no shortest rotation): 180 degrees about X [I] (soccer_ax00m1 checks it)
Z_FLIP = ((1.0, 0.0, 0.0), (0.0, -1.0, 0.0), (0.0, 0.0, -1.0))


def _mul(p, q):
    return tuple(tuple(sum(p[i][k] * q[k][j] for k in range(3)) for j in range(3)) for i in range(3))


# --- polyPipe -------------------------------------------------------------------------------------------------

def _pipe_ring(n, r, y):
    out = []
    for i in range(n):
        t = -2 * math.pi * i / n
        out.append((r * math.cos(t), y, r * math.sin(t)))
    return out


def poly_pipe(a):
    r, h, t = a["radius"], a["height"], a["thickness"]
    n, sh, sc = a["subdivisionsAxis"], a["subdivisionsHeight"], max(1, a["subdivisionsCaps"])
    rnd = bool(a.get("roundCap"))
    t = min(t, r - 1e-5)   # Maya keeps the inner wall at radius 1e-5 when Thickness >= Radius (pipe_sa6_t15)
    ri = r - t
    mid, half = r - t / 2, t / 2
    prof = []   # (radius, y) of each ring along the loop
    h = h / 2   # Maya's pipe is half its Height attribute tall (height 3 -> y +-0.75)
    for j in range(sh + 1):
        prof.append((ri, -h / 2 + h * j / sh))
    for c in range(1, sc):
        if rnd:
            ang = math.pi * c / sc
            prof.append((mid - half * math.cos(ang), h / 2 + half * math.sin(ang)))
        else:
            prof.append((ri + t * c / sc, h / 2))
    for j in range(sh + 1):
        prof.append((r, h / 2 - h * j / sh))
    for c in range(1, sc):
        if rnd:
            ang = math.pi * c / sc
            prof.append((mid + half * math.cos(ang), -h / 2 - half * math.sin(ang)))
        else:
            prof.append((r - t * c / sc, -h / 2))
    K = len(prof)
    verts = []
    for rr, yy in prof:
        verts += _pipe_ring(n, rr, yy)
    faces, uvs = [], []
    for k in range(K):
        k2 = (k + 1) % K
        for i in range(n):
            i2 = (i + 1) % n
            faces.append([k * n + i2, k * n + i, k2 * n + i, k2 * n + i2])
            v0, v1 = 1 - k / K, 1 - (k + 1) / K
            uvs.append([((i + 1) / n, v0), (i / n, v0), (i / n, v1), ((i + 1) / n, v1)])
    hard = set()
    walls = [list(range(0, sh + 1)), list(range(sh + sc, 2 * sh + sc + 1))]   # inner, outer wall ring indices
    if not rnd:
        for k in (walls[0][0], walls[0][-1], walls[1][0], walls[1][-1]):
            hard |= {frozenset((k * n + i, k * n + (i + 1) % n)) for i in range(n)}
    if 360.0 / n > PIPE_FACET:
        for w in walls:
            for k in w[:-1]:
                hard |= {frozenset((k * n + i, (k + 1) * n + i)) for i in range(n)}
    return Mesh(orient_z(verts, a), faces, uvs, hard)


# Pipe wall verticals / helix lengthwise edges are hard from 30 degrees per side on (pipe 12 sides hard, 13 soft;
# helix 12 hard, 13 / 30 soft: helix_s12_sco4 / s13_sco4 / s30_sco4)
PIPE_FACET = 29.9


# --- polyHelix ------------------------------------------------------------------------------------------------

HELIX_SIDE_U, HELIX_SIDE_V = (0.375, 0.625), (0.3125, 0.688440)
HELIX_CAP_R, HELIX_CAP_V, HELIX_CAP_DV = 0.15625, (0.15625, 0.84375), 0.00625


def helix_steps(coils, per_coil):
    """Body steps: whole coils (half rounds up, at least one) x Subdivisions Coil (coils c05 / c11 /
    c12 / c15 / c17 / c25: 8 / 8 / 8 / 16 / 16 / 24 steps at 8 per coil)."""
    return max(1, int(math.floor(coils + 0.5))) * per_coil


def poly_helix(a):
    c, h, w, r = a["coils"], a["height"], a["width"], a["radius"]
    n, per, sc = a["subdivisionsAxis"], a["subdivisionsCoil"], a["subdivisionsCaps"]
    rnd = bool(a.get("roundCap")) and sc > 0
    d = 1 if a.get("direction", 1) else -1
    N = helix_steps(c, per)
    R = w / 2 - r

    def frame(j):
        t = -d * 2 * math.pi * c * j / N
        radial = (math.cos(t), 0.0, math.sin(t))
        fwd = (d * math.sin(t), 0.0, -d * math.cos(t))   # horizontal direction of travel
        centre = (R * radial[0], -h / 2 + h * j / N, R * radial[2])
        return centre, radial, fwd

    def section(j, scale=1.0, along=0.0):
        centre, radial, fwd = frame(j)
        out = []
        for k in range(n):
            ang = -2 * math.pi * (k + 1) / n   # Maya's ring rule in the (radial, up) plane
            pr, pv = math.cos(ang) * r * scale, d * math.sin(ang) * r * scale
            out.append((centre[0] + pr * radial[0] + along * fwd[0], centre[1] + pv,
                        centre[2] + pr * radial[2] + along * fwd[2]))
        return out

    def cap_ring(j, cnum, sign):
        # ring cnum (1 .. sc-1) counted from the rim; sign -1 = bottom (behind the start), +1 = top
        if rnd:
            ang = math.pi / 2 * cnum / sc
            return section(j, math.cos(ang), sign * r * math.sin(ang))
        return section(j, (sc - cnum) / sc)

    rings = []   # (vertex list, uv kind, uv radius fraction)
    for cn in range(sc - 1, 0, -1):
        rings.append((cap_ring(0, cn, -1), "bottom", (sc - cn) / sc))
    body0 = len(rings)
    for j in range(N + 1):
        rings.append((section(j), "body", j))
    for cn in range(1, sc):
        rings.append((cap_ring(N, cn, 1), "top", (sc - cn) / sc))
    verts = [p for ring in rings for p in ring[0]]
    centres = []
    if sc > 0:
        for j, sign in ((0, -1), (N, 1)):
            centre, _radial, fwd = frame(j)
            along = sign * r if rnd else 0.0
            centres.append(len(verts))
            verts.append((centre[0] + along * fwd[0], centre[1], centre[2] + along * fwd[2]))

    def ring_uv(ring, k):
        _v, kind, x = rings[ring]
        if kind == "body":
            return (HELIX_SIDE_U[0] + (HELIX_SIDE_U[1] - HELIX_SIDE_U[0]) * k / n,
                    HELIX_SIDE_V[0] + (HELIX_SIDE_V[1] - HELIX_SIDE_V[0]) * x / N)
        return cap_uv(k, kind == "top", x)

    def cap_uv(k, top, frac):
        ang = -2 * math.pi * (k + 1) / n
        rho = HELIX_CAP_R * frac
        if top:
            return (0.5 + rho * math.cos(ang), HELIX_CAP_V[1] - rho * math.sin(ang))
        return (0.5 + rho * math.cos(ang), HELIX_CAP_V[0] + rho * math.sin(ang))

    faces, uvs = [], []
    for q in range(len(rings) - 1):
        kind = "body" if body0 <= q < body0 + N else "cap"
        for k in range(n):
            k2 = (k + 1) % n
            faces.append([q * n + k, q * n + k2, (q + 1) * n + k2, (q + 1) * n + k])
            if kind == "body":
                uvs.append([ring_uv(q, k), ring_uv(q, k + 1), ring_uv(q + 1, k + 1), ring_uv(q + 1, k)])
            else:   # cap quads: both rings on the cap circle (the body section = the rim, fraction 1)
                top = q >= body0
                fa = 1.0 if rings[q][1] == "body" else rings[q][2]
                fb = 1.0 if rings[q + 1][1] == "body" else rings[q + 1][2]
                uvs.append([cap_uv(k, top, fa), cap_uv(k2, top, fa), cap_uv(k2, top, fb), cap_uv(k, top, fb)])
    last = len(rings) - 1
    top_frac = rings[last][2] if rings[last][1] == "top" else 1.0
    bot_frac = rings[0][2] if rings[0][1] == "bottom" else 1.0
    if sc == 0:
        f = [0] + list(range(n - 1, 0, -1))
        faces.append(f)
        uvs.append([cap_uv(k, False, 1.0) for k in f])
        f = [last * n + k for k in range(n)]
        faces.append(f)
        uvs.append([cap_uv(k, True, 1.0) for k in range(n)])
    else:
        cb, ct = centres
        for k in range(n):
            k2 = (k + 1) % n
            faces.append([k2, k, cb])
            uvs.append([cap_uv(k2, False, bot_frac), cap_uv(k, False, bot_frac),
                        (0.5, HELIX_CAP_V[0] - HELIX_CAP_DV)])
        for k in range(n):
            k2 = (k + 1) % n
            faces.append([last * n + k, last * n + k2, ct])
            uvs.append([cap_uv(k, True, top_frac), cap_uv(k2, True, top_frac),
                        (0.5, HELIX_CAP_V[1] - HELIX_CAP_DV)])
    hard = set()
    if 360.0 / n > PIPE_FACET:
        for q in range(body0, body0 + N):
            hard |= {frozenset((q * n + k, (q + 1) * n + k)) for k in range(n)}
    if not rnd:
        for q in (body0, body0 + N):
            hard |= {frozenset((q * n + k, q * n + (k + 1) % n)) for k in range(n)}
    return Mesh(ps._orient(verts, a), faces, uvs, hard)


# --- polyGear -------------------------------------------------------------------------------------------------

def _gear_outline(a):
    """Top outline in (x, z): six points per side; side k faces 90 - 360 k / n degrees (+Z first, then towards +X)."""
    n, R = a["sides"], a["radius"]
    sp, off, tip, mid = a["gearSpacing"], a["gearOffset"], a["gearTip"], a["gearMiddle"]
    apo, side = R * math.cos(math.pi / n), 2 * R * math.sin(math.pi / n)
    b = sp * side / 2
    rows = ((apo, -b), (apo + off / 2, -b * (1 + tip) / 2 * mid), (apo + off, -b * tip),
            (apo + off, b * tip), (apo + off / 2, b * (1 + tip) / 2 * mid), (apo, b))
    out = []
    for k in range(n):
        al = math.pi / 2 - 2 * math.pi * k / n
        dx, dz = math.cos(al), math.sin(al)
        tx, tz = dz, -dx
        for dist, along in rows:
            out.append((dist * dx + along * tx, dist * dz + along * tz))
    return out


def poly_gear(a):
    n, H, h = a["sides"], a["heightDivisions"], a["height"]
    ir, tw, tp = a["internalRadius"], math.radians(a["twist"]), a["taper"]
    outline = _gear_outline(a)
    m = len(outline)
    inner = []
    for q in range(2 * n):
        al = math.pi / 2 + math.pi / (2 * n) - math.pi * q / n
        inner.append((ir * math.cos(al), ir * math.sin(al)))

    def level(pts, ell, scale):
        f = ell / H
        s = 1 + (scale - 1) * f
        rot = tw * f
        cs, sn = math.cos(rot), math.sin(rot)
        y = h / 2 - h * f
        return [((x * cs - z * sn) * s, y, (x * sn + z * cs) * s) for x, z in pts]

    verts = []
    for ell in range(H + 1):
        verts += level(outline, ell, tp)
    solid = ir <= 0.0
    if solid:
        inner_ids = [[len(verts)] * (2 * n), [len(verts) + 1] * (2 * n)]
        verts += [(0.0, h / 2, 0.0), (0.0, -h / 2, 0.0)]
        levels_in = None
    else:
        base = len(verts)
        for ell in range(H + 1):
            verts += level(inner, ell, 1.0)
        levels_in = [[base + ell * 2 * n + q for q in range(2 * n)] for ell in range(H + 1)]
        inner_ids = [levels_in[0], levels_in[H]]

    def O(ell, p):
        return ell * m + p % m

    # UVs: caps planar (top centre (0.25, 0.75), bottom (0.25, 0.25), scale 0.25 / (radius + offset)); outer wall
    # u 0.5..1 by arc length of the outline, v 1..0.5; inner wall u 0.5..1, v 0.5..0
    ks = 0.25 / (a["radius"] + a["gearOffset"])

    kb = ks / tp   # the bottom cap is normalised by its own (tapered) size, inner ring included (gear_s6_tp)

    def cap_uv(vid, top):
        x, _y, z = verts[vid]
        return (0.25 + ks * x, 0.75 - ks * z) if top else (0.25 + kb * x, 0.25 + kb * z)

    acc = [0.0]
    for p in range(m):
        q = outline[(p + 1) % m]
        acc.append(acc[-1] + math.hypot(q[0] - outline[p][0], q[1] - outline[p][1]))
    per = acc[-1] or 1.0
    faces, uvs = [], []
    for k in range(n):   # top tooth faces
        b = 6 * k
        for f in ([b, b + 1, b + 4, b + 5], [b + 1, b + 2, b + 3, b + 4]):
            faces.append(f)
            uvs.append([cap_uv(v, True) for v in f])
    for ell in range(H):
        for p in range(m):
            faces.append([O(ell, p), O(ell + 1, p), O(ell + 1, p + 1), O(ell, p + 1)])
            u0, u1 = 0.5 + 0.5 * acc[p] / per, 0.5 + 0.5 * acc[p + 1] / per
            v0, v1 = 1 - 0.5 * ell / H, 1 - 0.5 * (ell + 1) / H
            uvs.append([(u0, v0), (u0, v1), (u1, v1), (u1, v0)])
    for k in range(n):   # bottom tooth faces
        b = H * m + 6 * k
        for f in ([b + 5, b + 4, b + 1, b], [b + 4, b + 3, b + 2, b + 1]):
            faces.append(f)
            uvs.append([cap_uv(v, False) for v in f])
    if not solid:
        L = 2 * n
        for ell in range(H):
            for q in range(L):
                q2 = (q + 1) % L
                faces.append([levels_in[ell][q], levels_in[ell][q2], levels_in[ell + 1][q2], levels_in[ell + 1][q]])
                u0, u1 = 0.5 + 0.5 * q / L, 0.5 + 0.5 * (q + 1) / L
                v0, v1 = 0.5 - 0.5 * ell / H, 0.5 - 0.5 * (ell + 1) / H
                uvs.append([(u0, v0), (u1, v0), (u1, v1), (u0, v1)])
    it, ib = inner_ids
    for k in range(n):
        i0, i1, i2 = 2 * k, 2 * k + 1, (2 * k + 2) % (2 * n)
        quads = [([it[i0], O(0, 6 * k), O(0, 6 * k + 5), it[i1]], True),
                 ([it[i1], O(0, 6 * k + 5), O(0, 6 * k + 6), it[i2]], True),
                 ([ib[i1], O(H, 6 * k + 5), O(H, 6 * k), ib[i0]], False),
                 ([ib[i2], O(H, 6 * k + 6), O(H, 6 * k + 5), ib[i1]], False)]
        for f, top in quads:
            g = []
            for v in f:
                if v not in g:
                    g.append(v)
            faces.append(g)
            fu = [cap_uv(v, top) for v in g]
            if solid and not top:   # Maya swaps the two outline corners' UVs of the bottom triangles (gear_s6_ir0)
                fu = [fu[0], fu[2], fu[1]]
            uvs.append(fu)
    return Mesh(verts, faces, uvs, hard_by_angle(verts, faces, GEAR_FACET))


# polyGear hard edges: faces meeting at >= 30 degrees over the whole mesh (30.0 = 6 sides' inner ring hard, 29.04
# soft: gear_s6 / s3 / s16_h1 / r2 / tw / tp); a face without area (Gear Offset 0, Gear Spacing 1) has no normal and
# hardens its edges [I: gear_s6_off0 keeps 4 of those soft in Maya]
GEAR_FACET = 29.9


def hard_by_angle(verts, faces, limit):
    normals = []
    for f in faces:
        nx = ny = nz = 0.0
        for i in range(len(f)):
            p, q = verts[f[i - 1]], verts[f[i]]
            nx += (p[1] - q[1]) * (p[2] + q[2])
            ny += (p[2] - q[2]) * (p[0] + q[0])
            nz += (p[0] - q[0]) * (p[1] + q[1])
        n = math.sqrt(nx * nx + ny * ny + nz * nz)
        normals.append((nx / n, ny / n, nz / n) if n > 1e-10 else (0.0, 0.0, 0.0))
    owner = {}
    for fi, f in enumerate(faces):
        for i in range(len(f)):
            owner.setdefault(frozenset((f[i], f[i - 1])), []).append(fi)
    c = math.cos(math.radians(limit))
    hard = set()
    for e, fs in owner.items():
        if len(fs) == 2:
            a, b = normals[fs[0]], normals[fs[1]]
            if a[0] * b[0] + a[1] * b[1] + a[2] * b[2] < c:
                hard.add(e)
        else:
            hard.add(e)
    return hard


# --- Soccer Ball (polyPrimitiveMisc) ----------------------------------------------------------------------------
# Maya's vertex / face numbering follows no rule we could find (breadth-first from the +Y vertex, but the order inside
# each layer flips orientation), so the face lists (Maya face order, start corner, outward counter-clockwise) and the
# UV net (parent face of every face) are the topology of Maya's CreatePolygonSoccerBall; positions are
# computed: a truncated icosahedron vertex lies on its pentagon's icosahedron vertex A, a third of the way to the
# icosahedron vertex B of the pentagon across its hexagon-hexagon edge, (2 A + B) / 3, turned so that vertex 0 is
# on +Y and vertex 1 on the +X side of the x-y plane (both Maya).

SOCCER_FACES = (
    (0, 1, 4, 11, 7, 2), (0, 2, 6, 14, 9, 3), (3, 8, 5, 1, 0), (5, 12, 19, 10, 4, 1), (7, 15, 13, 6, 2),
    (9, 17, 26, 16, 8, 3), (4, 10, 18, 20, 11), (5, 8, 16, 25, 21, 12), (6, 13, 22, 33, 23, 14),
    (7, 11, 20, 30, 24, 15), (9, 14, 23, 27, 17), (19, 29, 39, 28, 18, 10), (21, 31, 29, 19, 12),
    (13, 15, 24, 34, 32, 22), (26, 36, 35, 25, 16), (27, 37, 46, 36, 26, 17), (28, 38, 40, 30, 20, 18),
    (21, 25, 35, 45, 41, 31), (22, 32, 42, 43, 33), (33, 43, 47, 37, 27, 23), (24, 30, 40, 44, 34),
    (39, 49, 48, 38, 28), (29, 31, 41, 50, 49, 39), (32, 34, 44, 52, 51, 42), (35, 36, 46, 54, 53, 45),
    (47, 55, 54, 46, 37), (48, 56, 52, 44, 40, 38), (41, 45, 53, 57, 50), (51, 58, 55, 47, 43, 42),
    (48, 49, 50, 57, 59, 56), (51, 52, 56, 59, 58), (53, 54, 55, 58, 59, 57))
# UV net: face -> the face it is unfolded from (shared UV edge in Maya's UVs; face 6 has its own UV ids but sits on
# face 3's edge). Face 0 is the root.
SOCCER_NET = {1: 0, 5: 1, 8: 1, 10: 1, 2: 5, 7: 5, 15: 5, 4: 8, 13: 8, 19: 8, 3: 7, 14: 7, 17: 7, 9: 13, 18: 13,
              23: 13, 12: 17, 22: 17, 24: 17, 11: 22, 27: 22, 25: 24, 20: 23, 26: 23, 28: 23, 16: 26, 29: 26, 30: 26,
              21: 29, 31: 29, 6: 3}
# Face 6's UVs are its net corners shifted by one (corner k gets corner k + 1's place), which is why Maya's UVs keep it
# a separate shell.
SOCCER_UV_SHIFT = {6: 1}
# Net placement (Create UVs = Normalize Collectively and Preserve Aspect Ratio): width 1 (an edge = 2 / 31), u from
# 0, v from 0.2 [I: Maya 0.2; centring the net would give 0.2052]; face 0's first edge points 240 degrees.
SOCCER_UV_V0, SOCCER_ROOT_DIR = 0.2, 240.0

_SOCCER_CACHE = []


def _soccer():
    if not _SOCCER_CACHE:
        _SOCCER_CACHE.append((_soccer_points(), [list(f) for f in SOCCER_FACES], _soccer_uvs()))
    verts, faces, uvs = _SOCCER_CACHE[0]
    return list(verts), [list(f) for f in faces], [list(u) for u in uvs]


def _unit(p):
    n = math.sqrt(sum(c * c for c in p))
    return tuple(c / n for c in p)


def _soccer_points():
    faces = SOCCER_FACES
    pent_of = {}
    for fi, f in enumerate(faces):
        if len(f) == 5:
            for v in f:
                pent_of[v] = fi
    nbr = {}
    for f in faces:
        for i in range(len(f)):
            nbr.setdefault(f[i], set()).add(f[i - 1])
            nbr.setdefault(f[i - 1], set()).add(f[i])
    across = {v: next(w for w in nbr[v] if pent_of[w] != pent_of[v]) for v in nbr}
    pent_cycle = {fi: [pent_of[across[v]] for v in f] for fi, f in enumerate(faces) if len(f) == 5}
    ico, _f = ps._platonic_base(4)
    ico = [_unit(p) for p in ico]
    # icosahedron neighbours in counter-clockwise order seen from outside (Maya's faces wind that way too)
    icyc = {}
    for a, pa in enumerate(ico):
        ns = sorted((b for b, pb in enumerate(ico) if b != a and sum(x * y for x, y in zip(pa, pb)) > 0.4))
        ref = tuple(ico[ns[0]][k] - pa[k] for k in range(3))

        def ang(b, pa=pa, ref=ref):
            d = tuple(ico[b][k] - pa[k] for k in range(3))
            cr = (ref[1] * d[2] - ref[2] * d[1], ref[2] * d[0] - ref[0] * d[2], ref[0] * d[1] - ref[1] * d[0])
            t = math.atan2(sum(cr[k] * pa[k] for k in range(3)), sum(ref[k] * d[k] for k in range(3)))
            return t % (2 * math.pi)
        icyc[a] = sorted(ns, key=ang)
    p0 = pent_of[0]
    phi = {p0: 0, pent_of[across[0]]: icyc[0][0]}
    todo = True
    while todo:
        todo = False
        for P, cyc in pent_cycle.items():
            if P not in phi or all(Q in phi for Q in cyc):
                continue
            k = next(i for i, Q in enumerate(cyc) if Q in phi)
            ic = icyc[phi[P]]
            off = ic.index(phi[cyc[k]]) - k
            for i, Q in enumerate(cyc):
                phi.setdefault(Q, ic[(i + off) % 5])
            todo = True
    pts = []
    for v in range(len(nbr)):
        A, B = ico[phi[pent_of[v]]], ico[phi[pent_of[across[v]]]]
        pts.append(_unit(tuple(2 * A[k] + B[k] for k in range(3))))
    y = pts[0]
    x = _unit(tuple(pts[1][k] - sum(pts[1][j] * y[j] for j in range(3)) * y[k] for k in range(3)))
    z = (x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0])
    return [tuple(sum(p[k] * axis[k] for k in range(3)) for axis in (x, y, z)) for p in pts]


def _soccer_uvs():
    faces = SOCCER_FACES
    place = {}
    root = faces[0]
    t = math.radians(SOCCER_ROOT_DIR)
    pos, cur = [], (0.0, 0.0)
    for i in range(len(root)):
        pos.append(cur)
        cur = (cur[0] + math.cos(t), cur[1] + math.sin(t))
        t += 2 * math.pi / len(root)
    place[0] = pos
    while len(place) < len(faces):
        for fi, parent in SOCCER_NET.items():
            if fi in place or parent not in place:
                continue
            pf, pp = faces[parent], place[parent]
            f = faces[fi]
            where = {v: pp[i] for i, v in enumerate(pf)}
            n = len(f)
            j = next(i for i in range(n) if f[i] in where and f[(i + 1) % n] in where)
            uv = [None] * n
            uv[j], uv[(j + 1) % n] = where[f[j]], where[f[(j + 1) % n]]
            a, b = uv[j], uv[(j + 1) % n]
            t = math.atan2(b[1] - a[1], b[0] - a[0])
            cur = b
            for k in range(2, n):
                t += 2 * math.pi / n
                cur = (cur[0] + math.cos(t), cur[1] + math.sin(t))
                uv[(j + k) % n] = cur
            place[fi] = uv
    allu = [p for fi in place for p in place[fi]]
    u0, u1 = min(p[0] for p in allu), max(p[0] for p in allu)
    v0 = min(p[1] for p in allu)
    s = 1.0 / (u1 - u0)
    out = []
    for fi in range(len(faces)):
        pts = place[fi]
        k = SOCCER_UV_SHIFT.get(fi, 0)
        pts = pts[k:] + pts[:k]
        out.append([((p[0] - u0) * s, SOCCER_UV_V0 + (p[1] - v0) * s) for p in pts])
    return out


def poly_soccer_ball(a):
    verts, faces, uvs = _soccer()
    r = a["radius"]
    verts = [(x * r, y * r, z * r) for x, y, z in verts]
    return Mesh(orient_z(verts, a), faces, uvs, ())


POLY = {"polyPipe": poly_pipe, "polyHelix": poly_helix, "polyGear": poly_gear, "polyPrimitiveMisc": poly_soccer_ball}
