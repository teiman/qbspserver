"""Experiment: compile thin / small world brushes as func_detail (see qbspserver.md, "Automatic detail").

A tall world column's sides are attractive splitters for qbsp (axial, few splits), so they can be
chosen high in the tree and cut portals and brushes across a whole room. As func_detail they are
only used at the bottom of the tree (inside a vis cluster), and vis ignores them, which a thin
column barely changes. Collision, lighting and the drawn surfaces stay the same.

Detail doesn't seal the map, so a brush that touches the void (outside the map) is kept: it may be
part of the seal. That is tested on a BSP of the original map: just outside each face of the
brush, a point that the BSP says is solid must be inside a world brush that stays world (not a
candidate itself: two candidates side by side could be sealing together).

    autodetail.py in.map ref.bsp out.map [--column 64] [--small 128]
"""
import argparse
import math
import sys
from collections import defaultdict

import bspcheck
import mapedit

CONTENTS_SOLID = -2
GRID = 256  # cell size of the brush lookup


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def brush_planes(brush):
    """[(normal, dist)] like qbsp's mapface_t::set_planepts: the brush is behind every plane."""
    planes = []
    for f in brush.faces:
        p = f.points
        p0, p1, p2 = (p[0], p[1], p[2]), (p[3], p[4], p[5]), (p[6], p[7], p[8])
        n = cross(sub(p0, p1), sub(p2, p1))
        length = math.sqrt(dot(n, n))
        if length < 1e-9:
            planes.append(None)
            continue
        n = (n[0] / length, n[1] / length, n[2] / length)
        planes.append((n, dot(p1, n)))
    return planes


def base_winding(normal, dist, size=1e5):
    # a big square on the plane
    ax = max(range(3), key=lambda i: abs(normal[i]))
    up = (0.0, 0.0, 1.0) if ax != 2 else (1.0, 0.0, 0.0)
    d = dot(up, normal)
    up = sub(up, (normal[0] * d, normal[1] * d, normal[2] * d))
    ul = math.sqrt(dot(up, up))
    up = (up[0] / ul, up[1] / ul, up[2] / ul)
    right = cross(up, normal)
    org = (normal[0] * dist, normal[1] * dist, normal[2] * dist)

    def at(a, b):
        return tuple(org[i] + right[i] * a * size + up[i] * b * size for i in range(3))

    return [at(-1, 1), at(1, 1), at(1, -1), at(-1, -1)]


def clip_back(w, normal, dist, eps=0.001):
    """The part of winding `w` behind the plane."""
    out = []
    n = len(w)
    for i in range(n):
        a, b = w[i], w[(i + 1) % n]
        da, db = dot(a, normal) - dist, dot(b, normal) - dist
        if da <= eps:
            out.append(a)
        if (da < -eps and db > eps) or (da > eps and db < -eps):
            t = da / (da - db)
            out.append(tuple(a[k] + (b[k] - a[k]) * t for k in range(3)))
    return out


def face_windings(planes):
    """[(normal, winding)] for each face of the brush (windings may be empty)."""
    result = []
    for i, pl in enumerate(planes):
        if pl is None:
            continue
        w = base_winding(*pl)
        for j, other in enumerate(planes):
            if j != i and other is not None and w:
                w = clip_back(w, *other)
        result.append((pl[0], w))
    return result


def sample_points(normal, w, offsets=(0.25, 1.0), max_side=24):
    """Points just outside a face: a grid over it (up to max_side x max_side), slightly inside its
    edges, at each distance in `offsets` (a slit thinner than a unit next to a brush can be void)."""
    if len(w) < 3:
        return []
    c = tuple(sum(p[k] for p in w) / len(w) for k in range(3))
    u = sub(w[1], w[0])
    ul = math.sqrt(dot(u, u))
    if ul < 1e-6:
        return []
    u = (u[0] / ul, u[1] / ul, u[2] / ul)
    v = cross(normal, u)
    pu = [dot(sub(p, c), u) for p in w]
    pv = [dot(sub(p, c), v) for p in w]
    u0, u1, v0, v1 = min(pu), max(pu), min(pv), max(pv)
    step = max(4.0, (u1 - u0) / max_side, (v1 - v0) / max_side)
    # the polygon's edges as 2D half-planes (inside: left of each edge, the winding being clockwise
    # or counter-clockwise: take the sign from the centre)
    edges = []
    for i in range(len(w)):
        a, b = (pu[i], pv[i]), (pu[(i + 1) % len(w)], pv[(i + 1) % len(w)])
        ex, ey = b[0] - a[0], b[1] - a[1]
        el = math.hypot(ex, ey)
        if el < 1e-6:
            continue
        edges.append((a, ex / el, ey / el))
    sign = 1 if all(ex * (0 - a[1]) - ey * (0 - a[0]) >= 0 for a, ex, ey in edges) else -1
    pts = [c]
    x = u0 + step / 2
    while x < u1:
        y = v0 + step / 2
        while y < v1:
            if all(sign * (ex * (y - a[1]) - ey * (x - a[0])) > 0.5 for a, ex, ey in edges):
                pts.append(tuple(c[k] + u[k] * x + v[k] * y for k in range(3)))
            y += step
        x += step
    return [tuple(p[k] + normal[k] * o for k in range(3)) for o in offsets for p in pts]


def true_bounds(brush):
    """The brush's real bounds, from its face polygons (a face's 3 points needn't be its vertices)."""
    pts = [p for _, w in face_windings(brush_planes(brush)) for p in w]
    if not pts:
        return brush.bounds()
    return tuple(min(p[k] for p in pts) for k in range(3)), tuple(max(p[k] for p in pts) for k in range(3))


class BrushIndex:
    """Which map brushes contain a point (solid world brushes only)."""

    def __init__(self, brushes):
        self.cells = defaultdict(list)
        self.planes = []
        for b in brushes:
            idx = len(self.planes)
            if b is None:  # (keeps the indices of the list)
                self.planes.append([])
                continue
            planes = [p for p in brush_planes(b) if p is not None]
            self.planes.append(planes)
            (x0, y0, z0), (x1, y1, z1) = true_bounds(b)
            for gx in range(int(math.floor(x0 / GRID)), int(math.floor(x1 / GRID)) + 1):
                for gy in range(int(math.floor(y0 / GRID)), int(math.floor(y1 / GRID)) + 1):
                    for gz in range(int(math.floor(z0 / GRID)), int(math.floor(z1 / GRID)) + 1):
                        self.cells[(gx, gy, gz)].append(idx)

    def covering(self, p):
        """The brushes (indices) that contain p."""
        key = tuple(int(math.floor(p[k] / GRID)) for k in range(3))
        return [idx for idx in self.cells.get(key, ())
                if self.planes[idx] and all(dot(n, p) - d < -0.01 for n, d in self.planes[idx])]


def is_candidate_shape(brush, column, small):
    (x0, y0, z0), (x1, y1, z1) = true_bounds(brush)
    ext = sorted((x1 - x0, y1 - y0, z1 - z0))
    if ext[2] <= small:
        return 'small'
    if ext[1] <= column:
        return 'column'
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('map')
    ap.add_argument('ref_bsp', help='a BSP of the same map (to find the brushes that touch the void)')
    ap.add_argument('out')
    ap.add_argument('--column', type=float, default=64, help='two smallest sizes at most this: a column / beam')
    ap.add_argument('--small', type=float, default=128, help='every size at most this: a small piece')
    args = ap.parse_args()

    with open(args.map, encoding='latin-1') as f:
        entities = mapedit.parse_map(f.read())
    bsp = bspcheck.Bsp(args.ref_bsp)

    # Detail doesn't seal: a candidate goes to func_detail only if every solid point just outside
    # it is inside a world brush that stays world. First the candidates with a solid point outside
    # that no world brush covers (the void, a filled pocket, or only detail there) stay. Then, in
    # map order, a candidate moves if all its solid outside points are covered by brushes that stay
    # (the non-candidates, and the candidates kept so far); otherwise it stays too.
    world_ents = [e for e in entities if (e.get('classname') or '') in ('worldspawn', 'func_group')]
    world = [b for e in world_ents for b in e.brushes if b.raw is None and b.faces]

    # only solid brushes seal (not liquids, clip, hint, triggers...): sky does
    def seals(b):
        return mapedit.plain_brush(b) or any(t.lower().startswith('sky') for t in b.textures())

    index = BrushIndex([b if seals(b) else None for b in world])
    shape = {}
    for i, b in enumerate(world):
        if mapedit.plain_brush(b):
            sh = is_candidate_shape(b, args.column, args.small)
            if sh:
                shape[i] = sh
    stats = defaultdict(int)
    for sh in shape.values():
        stats[sh] += 1

    # per candidate: for each solid point just outside it, the other world brushes covering it
    covers = {}
    for i in shape:
        lists = []
        for normal, w in face_windings(brush_planes(world[i])):
            for p in sample_points(normal, w):
                if bsp.contents(0, 0, p) == CONTENTS_SOLID:
                    lists.append([j for j in index.covering(p) if j != i])
        covers[i] = lists

    stay = set(range(len(world))) - set(shape)
    for i, lists in covers.items():
        if any(not l for l in lists):
            stay.add(i)
            stats['touch the void'] += 1
    moved_ids = set()
    for i in sorted(shape):
        if i in stay:
            continue
        if all(any(j in stay for j in l) for l in covers[i]):
            moved_ids.add(i)
        else:
            stay.add(i)
            stats['sealing with other candidates'] += 1

    moved = [world[i] for i in sorted(moved_ids)]
    moved_set = {id(b) for b in moved}
    for e in world_ents:
        e.brushes = [b for b in e.brushes if id(b) not in moved_set]

    if moved:
        detail = mapedit.Entity()
        detail.keys = [('classname', 'func_detail')]
        detail.brushes = moved
        entities.append(detail)
    with open(args.out, 'w', encoding='latin-1', newline='\n') as f:
        f.write(mapedit.write_map(entities))

    world_left = sum(len(e.brushes) for e in world_ents)
    print(f'candidates: {stats["column"]} columns / beams, {stats["small"]} small; kept: '
          f'{stats["touch the void"]} touch the void, {stats["sealing with other candidates"]} may seal with '
          f'other candidates; {len(moved)} moved to func_detail; {world_left} world brushes left')


if __name__ == '__main__':
    sys.exit(main())
