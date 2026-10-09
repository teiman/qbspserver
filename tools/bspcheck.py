#!/usr/bin/env python3
"""Checks that two Quake BSPs play the same (see "Output requirements" in qbspserver.md).

The incremental compile may build a different BSP tree than a full qbsp run; that is fine as
long as the game can't tell. So this compares what the game sees, not the bytes:

  entities   the entity lump, exactly (after normalizing whitespace)
  models     the same number of models (world + brush entities)
  surfaces   per model and texture: drawn area, its area-weighted centre (catches moved geometry)
             and area-weighted texture coordinates (catches changed texture alignment)
  contents   point contents (empty, solid, water, sky...) in hulls 0, 1 and 2 (drawing, player and
             shambler collision), at random points in every model and on both sides of every face
             that has no twin in the other BSP (the changed areas)

    bspcheck.py a.bsp b.bsp [--points N]       exit code 0 = same, 1 = different

BSP29 and BSP2 (Quake 1) are supported.
"""
import argparse
import math
import random
import struct
import sys

LUMP_ENTITIES, LUMP_PLANES, LUMP_TEXTURES, LUMP_VERTEXES, LUMP_VISIBILITY, LUMP_NODES, LUMP_TEXINFO, \
    LUMP_FACES, LUMP_LIGHTING, LUMP_CLIPNODES, LUMP_LEAFS, LUMP_MARKSURFACES, LUMP_EDGES, LUMP_SURFEDGES, \
    LUMP_MODELS = range(15)

CONTENTS_NAMES = {-1: 'empty', -2: 'solid', -3: 'water', -4: 'slime', -5: 'lava', -6: 'sky'}

# Quake 1 collision hulls (mins, maxs): 1 = player, 2 = shambler
HULL_BOXES = {1: ((-16, -16, -24), (16, 16, 32)), 2: ((-32, -32, -24), (32, 32, 64))}

# Tolerances. Vertices are float32, so a tree split differently gives slightly different numbers.
MAX_CONTENTS_MISMATCH = 0.0005  # fraction of the sampled points
AREA_TOLERANCE = 1e-4  # relative to the texture's area, + 1 unit^2
CENTRE_TOLERANCE = 1e-3  # units the area-weighted centre may move (x area), + 1
ALIGN_TOLERANCE = 1e-3  # texels the area-weighted texture coordinates may move (x area), + 1
MAX_FACE_SAMPLES = 20000  # faces without a twin that get points on both sides


class Bsp:
    def __init__(self, path):
        with open(path, 'rb') as f:
            data = f.read()
        self.path = path
        magic = data[:4]
        if magic == b'BSP2':
            bsp2 = True
        elif struct.unpack_from('<i', data, 0)[0] == 29:
            bsp2 = False
        else:
            raise ValueError(f'{path}: not a BSP29 / BSP2 file (magic {magic!r})')

        lumps = [struct.unpack_from('<ii', data, 4 + 8 * i) for i in range(15)]

        def lump(i):
            ofs, size = lumps[i]
            return data[ofs:ofs + size]

        self.entities = lump(LUMP_ENTITIES).split(b'\0', 1)[0].decode('latin-1')
        self.planes = [(nx, ny, nz, d) for nx, ny, nz, d, _ in struct.iter_unpack('<4fi', lump(LUMP_PLANES))]
        self.vertexes = list(struct.iter_unpack('<3f', lump(LUMP_VERTEXES)))
        self.surfedges = [v[0] for v in struct.iter_unpack('<i', lump(LUMP_SURFEDGES))]
        # mins, maxs, headnode[4], firstface, numfaces
        self.models = [(m[0:3], m[3:6], m[9:13], m[14], m[15])
                       for m in struct.iter_unpack('<9f4i3i', lump(LUMP_MODELS))]
        texinfo = list(struct.iter_unpack('<8f2i', lump(LUMP_TEXINFO)))
        self.texinfo_miptex = [t[8] for t in texinfo]
        self.texinfo_vecs = [(t[0:4], t[4:8]) for t in texinfo]
        self.texture_names = self._texture_names(lump(LUMP_TEXTURES))

        if bsp2:
            self.nodes = [n[0:3] for n in struct.iter_unpack('<3i6f2I', lump(LUMP_NODES))]
            self.leaf_contents = [l[0] for l in struct.iter_unpack('<2i6f2I4B', lump(LUMP_LEAFS))]
            self.clipnodes = list(struct.iter_unpack('<3i', lump(LUMP_CLIPNODES)))
            self.edges = list(struct.iter_unpack('<2I', lump(LUMP_EDGES)))
            faces = struct.iter_unpack('<5i4Bi', lump(LUMP_FACES))
        else:
            self.nodes = [n[0:3] for n in struct.iter_unpack('<i2h6h2H', lump(LUMP_NODES))]
            self.leaf_contents = [l[0] for l in struct.iter_unpack('<2i6h2H4B', lump(LUMP_LEAFS))]
            # 16-bit clipnode children: values past 32767 wrap negative but are node numbers;
            # only -1..-15 are contents
            self.clipnodes = [(p, fix_child(a), fix_child(b))
                              for p, a, b in struct.iter_unpack('<i2h', lump(LUMP_CLIPNODES))]
            self.edges = list(struct.iter_unpack('<2H', lump(LUMP_EDGES)))
            faces = struct.iter_unpack('<2hi2h4Bi', lump(LUMP_FACES))
        # planenum, side, firstedge, numedges, texinfo
        self.faces = [f[0:5] for f in faces]
        self._face_info = None

    @staticmethod
    def _texture_names(data):
        if len(data) < 4:
            return []
        count = struct.unpack_from('<i', data, 0)[0]
        names = []
        for i in range(count):
            ofs = struct.unpack_from('<i', data, 4 + 4 * i)[0]
            if ofs < 0:
                names.append('(missing)')
            else:
                names.append(data[ofs:ofs + 16].split(b'\0', 1)[0].decode('latin-1').lower())
        return names

    def contents(self, model, hull, p):
        x, y, z = p
        planes = self.planes
        num = self.models[model][2][hull]
        if hull == 0:
            nodes = self.nodes
            while num >= 0:
                plane, front, back = nodes[num]
                nx, ny, nz, d = planes[plane]
                num = front if nx * x + ny * y + nz * z - d >= 0 else back
            return self.leaf_contents[-1 - num]
        clipnodes = self.clipnodes
        if not clipnodes:
            return -1  # a map without clipping hulls (e.g. nothing solid): all empty
        while num >= 0:
            plane, front, back = clipnodes[num]
            nx, ny, nz, d = planes[plane]
            num = front if nx * x + ny * y + nz * z - d >= 0 else back
        return num

    def face_info(self):
        """[(model, texture, area, centre, normal, (s, t) at the centre)] for every drawn face."""
        if self._face_info is not None:
            return self._face_info
        info = []
        for m, (_, _, _, first, count) in enumerate(self.models):
            for planenum, side, firstedge, numedges, texinfo in self.faces[first:first + count]:
                miptex = self.texinfo_miptex[texinfo]
                name = self.texture_names[miptex] if 0 <= miptex < len(self.texture_names) else '(none)'
                pts = []
                for i in range(firstedge, firstedge + numedges):
                    e = self.surfedges[i]
                    pts.append(self.vertexes[self.edges[e][0] if e >= 0 else self.edges[-e][1]])
                area, centre = polygon_area_centre(pts)
                nx, ny, nz, _ = self.planes[planenum]
                normal = (-nx, -ny, -nz) if side else (nx, ny, nz)
                (sx, sy, sz, so), (tx, ty, tz, to) = self.texinfo_vecs[texinfo]
                cx, cy, cz = centre
                st = (cx * sx + cy * sy + cz * sz + so, cx * tx + cy * ty + cz * tz + to)
                info.append((m, name, area, centre, normal, st))
        self._face_info = info
        return info


def fix_child(c):
    return c + 65536 if c < -15 else c


def polygon_area_centre(pts):
    """Area and centroid of a convex polygon (fan triangulation from the first vertex)."""
    x0, y0, z0 = pts[0]
    area = cx = cy = cz = 0.0
    for i in range(1, len(pts) - 1):
        x1, y1, z1 = pts[i]
        x2, y2, z2 = pts[i + 1]
        ax, ay, az = x1 - x0, y1 - y0, z1 - z0
        bx, by, bz = x2 - x0, y2 - y0, z2 - z0
        crx, cry, crz = ay * bz - az * by, az * bx - ax * bz, ax * by - ay * bx
        a = 0.5 * math.sqrt(crx * crx + cry * cry + crz * crz)
        area += a
        cx += a * (x0 + x1 + x2) / 3
        cy += a * (y0 + y1 + y2) / 3
        cz += a * (z0 + z1 + z2) / 3
    if area <= 0:
        n = len(pts)
        return 0.0, (sum(p[0] for p in pts) / n, sum(p[1] for p in pts) / n, sum(p[2] for p in pts) / n)
    return area, (cx / area, cy / area, cz / area)


def hull_offset(hull, normal):
    """How far a hull's collision plane sits in front of the drawn surface, along its normal."""
    if hull == 0:
        return 0.0
    mins, maxs = HULL_BOXES[hull]
    return -sum(n * (lo if n > 0 else hi) for n, lo, hi in zip(normal, mins, maxs))


def normalize_entities(text):
    return [' '.join(line.split()) for line in text.splitlines() if line.strip()]


def surface_totals(bsp):
    """{(model, texture): [area, moment x, y, z, moment s, t]}"""
    totals = {}
    for m, name, area, (cx, cy, cz), _, (fs, ft) in bsp.face_info():
        t = totals.setdefault((m, name), [0.0] * 6)
        t[0] += area
        t[1] += area * cx
        t[2] += area * cy
        t[3] += area * cz
        t[4] += area * fs
        t[5] += area * ft
    return totals


def face_key(face):
    m, name, area, (cx, cy, cz), *_ = face
    return (m, name, round(area), round(cx * 2), round(cy * 2), round(cz * 2))


def unmatched_faces(a, b):
    """Faces of a with no twin (same model, texture, ~area, ~centre) in b."""
    keys = {}
    for f in b.face_info():
        k = face_key(f)
        keys[k] = keys.get(k, 0) + 1
    out = []
    for f in a.face_info():
        k = face_key(f)
        if keys.get(k, 0):
            keys[k] -= 1
        else:
            out.append(f)
    return out


def compare(a, b, points=20000, seed=1):
    """Returns (ok, report lines)."""
    lines = []
    ok = True

    def fail(msg):
        nonlocal ok
        ok = False
        lines.append('FAIL ' + msg)

    # entities
    ea, eb = normalize_entities(a.entities), normalize_entities(b.entities)
    if ea != eb:
        diff = sum(1 for x, y in zip(ea, eb) if x != y) + abs(len(ea) - len(eb))
        fail(f'entities: {diff} lines differ ({len(ea)} vs {len(eb)} lines)')
    else:
        lines.append(f'ok   entities: {len(ea)} lines identical')

    # models
    if len(a.models) != len(b.models):
        fail(f'models: {len(a.models)} vs {len(b.models)}')
        return ok, lines
    lines.append(f'ok   models: {len(a.models)}')

    # surfaces: area and area-weighted centre per model and texture
    sa, sb = surface_totals(a), surface_totals(b)
    bad = []
    for key in sorted(set(sa) | set(sb)):
        ta, tb = sa.get(key, [0.0] * 6), sb.get(key, [0.0] * 6)
        area = max(ta[0], tb[0])
        darea = abs(ta[0] - tb[0])
        dmoment = math.sqrt(sum((x - y) ** 2 for x, y in zip(ta[1:4], tb[1:4])))
        dalign = math.sqrt(sum((x - y) ** 2 for x, y in zip(ta[4:6], tb[4:6])))
        if (darea > AREA_TOLERANCE * area + 1 or dmoment > CENTRE_TOLERANCE * area + 1
                or dalign > ALIGN_TOLERANCE * area + 1):
            bad.append((key, ta[0], tb[0], dmoment / max(area, 1), dalign / max(area, 1)))
    if bad:
        (m, tex), x, y, shift, align = bad[0]
        fail(f'surfaces: {len(bad)} model/texture groups differ, e.g. model {m} {tex}: '
             f'area {x:.0f} vs {y:.0f}, centre moved {shift:.3f}u, texture coordinates moved {align:.3f} texels')
    else:
        lines.append(f'ok   surfaces: {len(sb)} model/texture groups, {sum(t[0] for t in sb.values()):.0f} units^2')

    # contents at the same points in both BSPs
    rnd = random.Random(seed)
    random_points = []  # (model, point)
    for m, (mins, maxs, *_) in enumerate(b.models):
        n = points if m == 0 else max(20, points // 200)
        lo = [v - 8 for v in mins]
        hi = [v + 8 for v in maxs]
        for _ in range(n):
            random_points.append((m, tuple(rnd.uniform(lo[i], hi[i]) for i in range(3))))

    changed = unmatched_faces(a, b) + unmatched_faces(b, a)
    if len(changed) > MAX_FACE_SAMPLES:
        changed = rnd.sample(changed, MAX_FACE_SAMPLES)

    for hull in (0, 1, 2):
        face_points = []
        for m, _, _, (cx, cy, cz), n, _ in changed:
            # 1 unit on each side of the surface (of the expanded plane for hulls 1 and 2)
            off = hull_offset(hull, n)
            for d in (off + 1, off - 1):
                face_points.append((m, (cx + n[0] * d, cy + n[1] * d, cz + n[2] * d)))

        # the two groups are judged apart: a few bad points next to a changed face must not
        # hide among thousands of random ones
        results = []
        for group, samples in (('random', random_points), ('at changed faces', face_points)):
            mismatches = {}
            for m, p in samples:
                ca, cb = a.contents(m, hull, p), b.contents(m, hull, p)
                if ca != cb:
                    key = (CONTENTS_NAMES.get(ca, ca), CONTENTS_NAMES.get(cb, cb))
                    mismatches[key] = mismatches.get(key, 0) + 1
            nbad = sum(mismatches.values())
            detail = ', '.join(f'{x}->{y} x{n}' for (x, y), n in sorted(mismatches.items(), key=lambda t: -t[1])[:4])
            results.append((group, len(samples), nbad, detail))

        failed = [r for r in results if r[1] and r[2] / r[1] > MAX_CONTENTS_MISMATCH]
        summary = '; '.join(f'{nbad} of {n} {group} differ' + (f' ({detail})' if detail else '')
                            for group, n, nbad, detail in results if n)
        if failed:
            fail(f'hull {hull} contents: {summary}')
        else:
            lines.append(f'ok   hull {hull} contents: {summary}')
    return ok, lines


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('--points', type=int, default=20000, help='random points in the world model, per hull')
    args = ap.parse_args()

    ok, lines = compare(Bsp(args.a), Bsp(args.b), args.points)
    for line in lines:
        print(line)
    print('SAME' if ok else 'DIFFERENT')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
