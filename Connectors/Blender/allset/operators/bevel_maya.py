# SPDX-License-Identifier: GPL-3.0-or-later
"""Maya polyBevel3 (Edit Mesh > Bevel, Ctrl+B) at once, as an editable history node like step 65's extrusion
(#16, docs/steps/step_139.md 「계획」).

- Factory values: Fraction 0.5, Offset As Fraction on, Offset 0.2, Segments 1, Depth 1 (Chamfer on, Mitering auto).
- Fraction is relative to one length for the whole selection: the shortest "slide" edge (an edge touching an end of a
  beveled edge), halved when both of its ends are used (two shells of different size get the same width). The width
  Maya applies is 0.9999 x that (both modes).
- The profile is Blender's superellipse with ``profile = t^2``, t = 0.5 + Depth x (sqrt(1/2) - 1/2): Depth 1 = the
  round arc (0.5), 0 = straight, -1 = the arc mirrored over the chord (exact for 2 segments).
- Zero-length slide edges don't count, and what the bevel makes is merged within 0.0001 (Merge Vertices): the
  cap of a zero-length extrusion bevels into an inset (12/20/10, cap at 0.25), Fraction 1 merges (6/9/5).
- Faces bevel their edges; Maya leaves nothing selected afterwards. Vertex mode = Chamfer Vertex (Maya's Bevel
  on vertices chamfers them: 0.25 of each edge, no editable node here).
- Numbering (step 257, #149): kept vertices first in their old
  order, then the new vertices of each bevelled vertex, the bevelled vertices taken in the order the selected edges
  reach them (ascending Maya edge id, ends by ascending vertex id) and each one's new vertices in Maya's vertex ->
  edge order (``vertex_edges``). Faces, when every bevelled vertex ends two or more bevelled edges: old faces off the
  bevelled edges, the bevel faces, old faces along bevelled edges, corner faces. Maya's edge ids = the mesh's edge
  index (``maya_edge_ids``) unless the caller passes them.
  Not renumbered (Maya's order there is unknown): Segments > 1, border vertices, merged results.
"""

import math

import bmesh

from . import extrude

MERGE = 1e-4          # polyBevel3 Merge Vertices on, tolerance 0.0001 (factory)
ZERO = 1e-6           # zero-length slide edges (a zero-length extrusion) don't limit the fraction
FIT = 0.9999          # Maya's bevel width = 0.9999 x the asked width (offset 0.1 -> 0.09999, fraction 0.5 -> 0.49995)
CHAMFER = 25.0        # Chamfer Vertex: 0.25 of each edge (unit cube and 4 x 1 x 2 box)


def slide_limit(edges):
    """Maya's fraction base (a perpendicular width): min over slide edges of length x sin(angle to the beveled edge)
    / (number of their ends a bevel slides along).

    Another beveled edge is a slide only where it shares a face with this one (the cube's top loop: the corner slides
    inside the top face along it). Step 239 (#124): a cylinder cap's border ring
    has no face holding two ring edges, so Maya slides along the side / cap fan edges (length 1) — and the width is
    perpendicular, so the fan edges at 81 degrees to the ring give 0.49995 x sin 81 = 0.49379 (the side ring lands at
    y 1.5062, the cap ring at radius 0.5). The ring neighbours had given 0.078. Cube cases are all at 90 degrees."""
    sel = set(edges)
    used = {}
    for e in edges:
        faces = set(e.link_faces)
        d = (e.verts[1].co - e.verts[0].co).normalized()
        for v in e.verts:
            for s in v.link_edges:
                if s is e or (s in sel and not faces.intersection(s.link_faces)):
                    continue
                t = (s.other_vert(v).co - v.co)
                sin = t.normalized().cross(d).length if t.length > ZERO else 0.0
                key = s
                ent = used.setdefault(key, [set(), 1.0])
                ent[0].add(v)
                ent[1] = min(ent[1], sin)
    caps = [s.calc_length() * sin / len(ends) for s, (ends, sin) in used.items() if s.calc_length() > ZERO]
    if caps:
        return min(caps)
    return min((e.calc_length() for e in sel), default=0.0)


def profile(depth):
    t = 0.5 + max(-1.0, min(1.0, depth)) * (math.sqrt(0.5) - 0.5)
    return max(0.0, min(1.0, t * t))


def width(edges, fraction=0.5, offset_as_fraction=True, offset=0.2, unit=1.0):
    if offset_as_fraction:
        return max(0.0, min(1.0, fraction)) * slide_limit(edges) * FIT
    return max(0.0, offset) * unit * FIT


# --- Maya numbering (step 257) -----------------------------------------------------------------------------------

def maya_edge_ids(bm):
    """{BMEdge: Maya edge id}: the mesh's edge index (delegated kit design, step 257). It is Maya's id wherever
    the kit made the edges: kit polyCube / polyCylinder are built with Maya's edge order (prim_shapes ``edges``) and
    kit extrusions sort their result into polyExtrudeFacet's order (extrude.renumber),
    Maya numbers a mesh made from a face list in face-corner order; bmesh's
    ``faces.new`` doesn't (it makes a face's closing edge first), so such a mesh needs its edges made in that order
    first. Elsewhere (other primitives, Blender's own tools, imports) Maya's ids are not known."""
    bm.edges.index_update()
    return {e: e.index for e in bm.edges}


def vertex_edges(v, ids):
    """Maya's vertex -> edge order (MItMeshVertex.getConnectedEdges): start at the edge with the highest Maya id (on
    a border: the border edge the face winding leaves v along), then each next edge = the edge coming into v in the
    face the current edge leaves v in. 477 / 477 vertices of 24 meshes match Maya.
    None for a vertex that isn't a single fan of faces."""
    nxt = {}
    for loop in v.link_loops:
        if loop.edge in nxt:
            return None
        nxt[loop.edge] = loop.link_loop_prev.edge
    edges = list(v.link_edges)
    if not edges:
        return None
    heads = [e for e in nxt if e not in set(nxt.values())]
    if len(heads) > 1:
        return None
    cur = heads[0] if heads else max(edges, key=lambda e: ids[e])
    out = [cur]
    while len(out) < len(edges):
        cur = nxt.get(cur)
        if cur is None or cur in out:
            return None
        out.append(cur)
    return out


def _face_between(v, e_out, e_in):
    for loop in v.link_loops:
        if loop.edge is e_out and loop.link_loop_prev.edge is e_in:
            return loop.face
    return None


def _slots(v, sel, ids):
    """The new vertices of bevelled vertex ``v`` in Maya's order, each as the set of old face indices it lies in;
    None where the order is unknown (border vertices, one bevelled edge at a two-edge vertex)."""
    E = vertex_edges(v, ids)
    if E is None or any(e.is_boundary or e.is_wire for e in E):
        return None
    n = len(E)
    between = [_face_between(v, E[k], E[(k + 1) % n]) for k in range(n)]
    if None in between:
        return None
    marks = [k for k in range(n) if E[k] in sel]
    if len(marks) == 1:
        if n < 3:
            return None
        k = marks[0]
        # one bevelled edge: a new vertex on each edge next to it, in E(v)'s list order
        return [frozenset((between[j - 1].index, between[j].index)) for j in sorted({(k - 1) % n, (k + 1) % n})]
    out = []
    for i, k in enumerate(marks):      # one new vertex per wedge, the wedge after each bevelled edge in E(v)'s order
        span = (marks[(i + 1) % len(marks)] - k) % n or n
        out.append(frozenset(between[(k + j) % n].index for j in range(span)))
    return out


def _plan(bm, edges, ids):
    """Maya's order of what the bevel will leave, as keys the result is matched to (None: unknown here)."""
    sel = set(edges)
    bm.verts.index_update()
    bm.faces.index_update()
    order, seen = [], set()
    for e in sorted(edges, key=lambda e: ids[e]):
        for v in sorted(e.verts, key=lambda v: v.index):
            if v not in seen:
                seen.add(v)
                order.append(v)
    slots, edge_faces, met, corners = [], [], set(), []
    for v in order:
        s = _slots(v, sel, ids)
        if s is None or len(set(s)) != len(s):
            return None
        slots += [(v.index, key) for key in s]
        for e in vertex_edges(v, ids):      # a bevel face comes when its edge is first met in this walk
            if e in sel and e not in met:
                met.add(e)
                edge_faces.append(frozenset(x.index for x in e.verts))
        if sum(1 for e in v.link_edges if e in sel) >= 3:
            corners.append(v.index)
    along = sorted({f.index for e in edges for f in e.link_faces})
    # the face order below is Maya's when every bevelled vertex ends two or more bevelled edges (closed loops, all
    # edges: 16 / 16 cases); with a chain end or a lone edge Maya orders the faces another way, not yet
    # known, so the faces then keep Blender's order
    closed = all(sum(1 for e in v.link_edges if e in sel) >= 2 for v in order)
    return {"kept": [v.index for v in bm.verts if v not in seen], "slots": slots, "edge_faces": edge_faces,
            "corners": corners, "along": along, "faces": len(bm.faces), "closed": closed,
            "co": {v.index: v.co.copy() for v in order}}


def _renumber(bm, plan, vlay, flay, made, new_verts):
    """Sort the bevelled mesh into Maya's order. Vertices only when every one is matched; faces only when every face
    is (else they keep Blender's order). A new vertex is matched by the old faces it lies in, and among bevelled
    vertices with a wedge of the same faces by the nearest old position (bevel copies a neighbour's custom data onto
    new vertices, so they carry no reliable source index)."""
    kept = {i: n for n, i in enumerate(plan["kept"])}
    by_key = {}
    for n, (v, key) in enumerate(plan["slots"]):
        by_key.setdefault(key, []).append((v, len(kept) + n))
    rank, owner = {}, {}
    for v in bm.verts:
        if v not in new_verts:
            r = kept.get(v[vlay] - 1)
            src = v[vlay] - 1
        else:
            cands = by_key.get(frozenset(f[flay] - 1 for f in v.link_faces if f not in made), ())
            if not cands:
                return
            src, r = min(cands, key=lambda c: (plan["co"][c[0]] - v.co).length_squared)
        if r is None:
            return
        rank[v] = r
        owner[v] = src
    if sorted(rank.values()) != list(range(len(bm.verts))):
        return
    made_owners = {f: frozenset(owner[v] for v in f.verts) for f in made}
    bm.verts.sort(key=lambda v: rank[v])
    bm.verts.index_update()
    if not plan["closed"]:
        return
    along = {i: n for n, i in enumerate(plan["along"])}
    plain = {i: n for n, i in enumerate(i for i in range(plan["faces"]) if i not in along)}
    edge_rank = {key: n for n, key in enumerate(plan["edge_faces"])}
    corner_rank = {v: n for n, v in enumerate(plan["corners"])}
    frank = {}
    for f in bm.faces:
        if f in made:
            srcs = made_owners[f]
            if srcs in edge_rank:
                frank[f] = (1, edge_rank[srcs])
            elif len(srcs) == 1 and next(iter(srcs)) in corner_rank:
                frank[f] = (3, corner_rank[next(iter(srcs))])
            else:
                return
        else:
            i = f[flay] - 1
            frank[f] = (2, along[i]) if i in along else (0, plain[i])
    if len(set(frank.values())) != len(frank):
        return
    bm.faces.sort(key=lambda f: frank[f][0] * 1000000 + frank[f][1])
    bm.faces.index_update()


def bevel_edges(bm, edges, fraction=0.5, offset_as_fraction=True, offset=0.2, segments=1, depth=1.0, unit=1.0,
                edge_ids=None, **_unused):
    """polyBevel3 on ``edges`` (bmesh). Returns the vertices the bevel made (empty: nothing done). ``edge_ids``
    ({BMEdge: Maya edge id}) replaces ``maya_edge_ids`` for a mesh whose Maya edge numbering the caller knows."""
    edges = [e for e in edges if e.is_valid]
    w = width(edges, fraction, offset_as_fraction, offset, unit)
    if not edges or w <= 0.0:
        return []
    ids = edge_ids if edge_ids is not None else maya_edge_ids(bm)
    plan = _plan(bm, edges, ids) if ids is not None and int(segments) == 1 else None
    bm.edges.index_update()
    picked = [e.index for e in edges]
    # source indices ride along as custom data (bevel copies them onto what it makes); added before elements are
    # collected again, since adding a layer can invalidate element references (step 64)
    vlay = bm.verts.layers.int.new(".allset_bevel_src")
    tlay = bm.verts.layers.int.new(".allset_bevel_made")
    flay = bm.faces.layers.int.new(".allset_bevel_src")
    for v in bm.verts:
        v[vlay] = v.index + 1
    for f in bm.faces:
        f[flay] = f.index + 1
    bm.edges.ensure_lookup_table()
    edges = [bm.edges[i] for i in picked]
    verts = {v for e in edges for v in e.verts}
    ret = bmesh.ops.bevel(bm, geom=list(edges) + list(verts), offset=w, offset_type='OFFSET',
                          segments=max(1, int(segments)), profile=profile(depth), affect='EDGES',
                          clamp_overlap=False, loop_slide=True)
    made = [v for v in ret["verts"] if v.is_valid]
    near = set(made)
    for v in made:
        near.update(e.other_vert(v) for e in v.link_edges)
    # Maya merges what the bevel made onto coincident vertices (a zero-length extrusion's sides, Fraction 1), at
    # their midpoint (Fraction 1: 0.49995 between 0.5 and 0.4999)
    pairs = bmesh.ops.find_doubles(bm, verts=list(near), dist=MERGE * 1.001)["targetmap"]
    for v, target in pairs.items():
        target.co = (v.co + target.co) / 2
    if pairs:
        bmesh.ops.weld_verts(bm, targetmap=pairs)
    for v in near:
        if v.is_valid:
            v[tlay] = 1
    if plan is not None and not pairs:      # Maya's numbering after Merge Vertices merged something is unknown
        _renumber(bm, plan, vlay, flay, {f for f in ret["faces"] if f.is_valid}, {v for v in made if v.is_valid})
    bm.verts.index_update()
    tracked = [v.index for v in bm.verts if v[tlay]]
    bm.verts.layers.int.remove(vlay)
    bm.verts.layers.int.remove(tlay)
    bm.faces.layers.int.remove(flay)
    bm.verts.ensure_lookup_table()
    extrude.deselect_all(bm)
    bm.select_flush_mode()
    bm.normal_update()
    return [bm.verts[i] for i in tracked]


def chamfer_vertices(bm, verts):
    """Maya Chamfer Vertex (Ctrl+B in vertex mode): a face where each vertex was, 0.25 of each edge in."""
    verts = [v for v in verts if v.is_valid]
    if not verts:
        return []
    ret = bmesh.ops.bevel(bm, geom=verts, offset=CHAMFER, offset_type='PERCENT', segments=1, profile=0.5,
                          affect='VERTICES', clamp_overlap=False)
    extrude.deselect_all(bm)      # Maya selects only the polyChamfer node
    bm.select_flush_mode()
    bm.normal_update()
    return list(ret["verts"])


def _build(bm, params, unit):
    return bevel_edges(bm, [e for e in bm.edges if e.select], unit=unit, **params)


BEVEL = extrude.add_spec(extrude.Spec(
    'BEVEL', "polyBevel", ("fraction", "offset_as_fraction", "offset", "segments", "depth"),
    {"fraction": "Fraction", "offset_as_fraction": "Offset As Fraction", "offset": "Offset", "segments": "Segments",
     "depth": "Depth"},
    {"fraction": 0.5, "offset_as_fraction": True, "offset": 0.2, "segments": 1, "depth": 1.0},
    {"fraction": "NONE", "offset_as_fraction": "BOOL", "offset": "NONE", "segments": "INT", "depth": "NONE"},
    None, "bevel_edit", _build, node_type="polyBevel3"))
