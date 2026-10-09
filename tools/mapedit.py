#!/usr/bin/env python3
"""Deterministic edits of a Quake .map, for the compile benchmark (tools/bench.py).

Each benchmark scenario is a list of edits (move a brush, retexture a brush, add a brush,
change a point entity). The brushes and entities are picked with a fixed seed, so a scenario
always makes exactly the same changes to the same base map: runs on different versions of
qbspserver compile the same files and their times can be compared.

    mapedit.py base.map out.map scenario     writes the edited map and prints the edits
    mapedit.py base.map --list               lists the scenarios

Bump SCENARIO_VERSION whenever the scenarios or the way edits are picked change: bench.py
only compares runs with the same version.
"""
import argparse
import random
import re
import sys
import zlib

SCENARIO_VERSION = 2
# 2: + bigmove (a big brush: walls, floors span many cells), + align (texture alignment)

# name -> (description, edits). An edit is (kind, count); a float count is a fraction of the candidates.
SCENARIOS = {
    'nochange': ('recompile without changes', []),
    'entity':   ('1 point entity changed (moved 16u, light value)', [('entity', 1)]),
    'texture':  ('1 brush retextured', [('texture', 1)]),
    'move':     ('1 small brush moved 8u', [('move', 1)]),
    'bigmove':  ('1 big brush (>= 512u) moved 8u', [('bigmove', 1)]),
    'align':    ('1 brush\'s texture alignment shifted 16 texels', [('align', 1)]),
    'add':      ('1 brush added', [('add', 1)]),
    'geotex':   ('3 moved + 3 retextured + 1 added', [('move', 3), ('texture', 3), ('add', 1)]),
    'many':     ('2% moved + 2% retextured + 10 added + 10 entities',
                 [('move', 0.02), ('texture', 0.02), ('add', 10), ('entity', 10)]),
}

FACE_RE = re.compile(
    r'^\s*\(\s*(\S+)\s+(\S+)\s+(\S+)\s*\)\s*\(\s*(\S+)\s+(\S+)\s+(\S+)\s*\)\s*\(\s*(\S+)\s+(\S+)\s+(\S+)\s*\)'
    r'\s*(\S+)(.*)$')
KEY_RE = re.compile(r'^\s*"([^"]*)"\s+"([^"]*)"\s*$')

# textures that are not plain visible solid (moving or retexturing them changes the map's structure).
# 'skip' (hidden faces of a solid brush) is fine: those brushes are moved, and their skip faces kept.
SPECIAL_PREFIXES = ('*', '!', 'sky', 'clip', 'hint', 'trigger', 'origin', 'nodraw', 'null', '__tb',
                    'common/', 'lavaclip', 'waterskip', 'noclip', 'hull')
HIDDEN_TEXTURES = ('skip',)
# entities that qbsp merges into the world: their brushes are candidates for edits
WORLD_CLASSES = ('worldspawn', 'func_group', 'func_detail', 'func_detail_wall')
SMALL_BRUSH = 128  # a brush this big or smaller (on every axis) is a prop / trim, unlikely to seal the map
BIG_BRUSH = 512  # a brush this big on some axis is a wall / floor / ceiling: it spans several cells
MOVE_DIST = 8


class Face:
    __slots__ = ('points', 'texture', 'rest')

    def __init__(self, points, texture, rest):
        self.points = points  # 9 floats
        self.texture = texture
        self.rest = rest  # alignment etc., kept as text

    def text(self):
        p = [fmt_num(v) for v in self.points]
        return f'( {p[0]} {p[1]} {p[2]} ) ( {p[3]} {p[4]} {p[5]} ) ( {p[6]} {p[7]} {p[8]} ) {self.texture}{self.rest}'


class Brush:
    __slots__ = ('faces', 'raw')

    def __init__(self):
        self.faces = []
        self.raw = None  # lines of a brush we don't understand (patches...), written back as is

    def textures(self):
        return {f.texture for f in self.faces}

    def bounds(self):
        xs = [f.points[i] for f in self.faces for i in (0, 3, 6)]
        ys = [f.points[i] for f in self.faces for i in (1, 4, 7)]
        zs = [f.points[i] for f in self.faces for i in (2, 5, 8)]
        return (min(xs), min(ys), min(zs)), (max(xs), max(ys), max(zs))

    def text(self):
        if self.raw is not None:
            return '\n'.join(self.raw)
        return '{\n' + ''.join(f.text() + '\n' for f in self.faces) + '}'


class Entity:
    __slots__ = ('keys', 'brushes')

    def __init__(self):
        self.keys = []  # [(key, value)] in file order
        self.brushes = []

    def get(self, key):
        for k, v in self.keys:
            if k == key:
                return v
        return None

    def set(self, key, value):
        for i, (k, _) in enumerate(self.keys):
            if k == key:
                self.keys[i] = (k, value)
                return
        self.keys.append((key, value))

    def text(self):
        out = ['{']
        out += [f'"{k}" "{v}"' for k, v in self.keys]
        out += [b.text() for b in self.brushes]
        out.append('}')
        return '\n'.join(out)


def fmt_num(v):
    return str(int(v)) if v == int(v) else repr(v)


def parse_map(text):
    entities = []
    ent = brush = None
    raw_depth = 0
    for lineno, line in enumerate(text.splitlines(), 1):
        s = line.strip()
        if brush is not None and brush.raw is not None:
            # inside a brush we don't parse: copy lines until it closes
            brush.raw.append(line)
            raw_depth += s.count('{') - s.count('}')
            if raw_depth == 0:
                ent.brushes.append(brush)
                brush = None
            continue
        if not s or s.startswith('//'):
            continue
        if s == '{':
            if ent is None:
                ent = Entity()
            elif brush is None:
                brush = Brush()
            else:
                raise ValueError(f'line {lineno}: unexpected {{')
        elif s == '}':
            if brush is not None:
                ent.brushes.append(brush)
                brush = None
            elif ent is not None:
                entities.append(ent)
                ent = None
            else:
                raise ValueError(f'line {lineno}: unexpected }}')
        elif brush is not None:
            m = FACE_RE.match(s)
            if m:
                brush.faces.append(Face([float(m.group(i)) for i in range(1, 10)], m.group(10), m.group(11)))
            else:
                # patchDef2, brushDef...: keep the whole brush as text
                brush.raw = ['{'] + [f.text() for f in brush.faces] + [line]
                brush.faces = []
                raw_depth = 1 + s.count('{') - s.count('}')
        elif ent is not None:
            m = KEY_RE.match(s)
            if not m:
                raise ValueError(f'line {lineno}: not a key/value: {s}')
            ent.keys.append((m.group(1), m.group(2)))
        else:
            raise ValueError(f'line {lineno}: text outside an entity: {s}')
    if ent is not None or brush is not None:
        raise ValueError('unexpected end of file')
    return entities


def write_map(entities):
    return '\n'.join(e.text() for e in entities) + '\n'


def shift_alignment(rest, texels):
    """Adds `texels` to the horizontal texture offset of a face's alignment text.

    Standard format: ' xoff yoff rot xscale yscale'; Valve 220: ' [ ux uy uz uoff ] [ ... ] rot sx sy'."""
    tokens = rest.split()
    if not tokens:
        return rest
    i = 4 if tokens[0] == '[' else 0  # the u offset in Valve 220, the x offset otherwise
    if i >= len(tokens):
        return rest
    try:
        tokens[i] = fmt_num(float(tokens[i]) + texels)
    except ValueError:
        return rest
    return ' ' + ' '.join(tokens)


def is_special(texture):
    t = texture.lower()
    return t.startswith(SPECIAL_PREFIXES)


def is_hidden(texture):
    return texture.lower() in HIDDEN_TEXTURES


def plain_brush(brush):
    textures = brush.textures()
    return (brush.raw is None and brush.faces and not any(is_special(t) for t in textures)
            and not all(is_hidden(t) for t in textures))


def pick(rnd, candidates, count):
    n = max(1, round(len(candidates) * count)) if isinstance(count, float) else count
    n = min(n, len(candidates))
    return rnd.sample(candidates, n)


def origin_of(ent):
    o = ent.get('origin')
    if o is None:
        return None
    try:
        x, y, z = (float(v) for v in o.split())
    except ValueError:
        return None
    return x, y, z


def point_entities(entities, lights_first=True):
    """Point entities with an origin, lights if there are any (they float in the playable space)."""
    points = [e for e in entities[1:] if not e.brushes and origin_of(e) is not None]
    points = [e for e in points if not (e.get('classname') or '').startswith('info_player')]
    lights = [e for e in points if (e.get('classname') or '').startswith('light')]
    return lights if (lights_first and lights) else points


def box_brush(center, half, texture):
    (cx, cy, cz), h = center, half
    x0, y0, z0, x1, y1, z1 = cx - h, cy - h, cz - h, cx + h, cy + h, cz + h
    planes = [  # three points per plane, standard Quake winding (normals out)
        ((x0, 0, 0), (x0, 1, 0), (x0, 0, 1)),
        ((x1, 0, 0), (x1, 0, 1), (x1, 1, 0)),
        ((0, y0, 0), (0, y0, 1), (1, y0, 0)),
        ((0, y1, 0), (1, y1, 0), (0, y1, 1)),
        ((0, 0, z0), (1, 0, z0), (0, 1, z0)),
        ((0, 0, z1), (0, 1, z1), (1, 0, z1)),
    ]
    b = Brush()
    for p in planes:
        b.faces.append(Face([c for pt in p for c in pt], texture, ' 0 0 0 1 1'))
    return b


def can_leak(name):
    """Whether a scenario removes geometry somewhere, which may open a hole to the void."""
    return any(kind in ('move', 'bigmove') for kind, _ in SCENARIOS[name][1])


def apply_scenario(entities, name, attempt=0):
    """Applies a scenario in place. Returns a list of human-readable edits.

    attempt picks another set of brushes / entities (bench.py tries the next one when the edits
    make the map leak, and remembers which one in testmap/bench_seeds.json)."""
    _, edits = SCENARIOS[name]
    rnd = random.Random(zlib.crc32(name.encode() if attempt == 0 else f'{name}#{attempt}'.encode()))
    world = entities[0]
    log = []

    plain = [b for e in entities if e.get('classname') in WORLD_CLASSES for b in e.brushes if plain_brush(b)]
    textures = sorted({f.texture for b in plain for f in b.faces if not is_hidden(f.texture)})
    small = [b for b in plain if all(hi - lo <= SMALL_BRUSH for lo, hi in zip(*b.bounds()))]
    big = [b for b in plain if any(hi - lo >= BIG_BRUSH for lo, hi in zip(*b.bounds()))]
    # most used plain texture, for added brushes
    usage = {}
    for b in plain:
        for f in b.faces:
            if not is_hidden(f.texture):
                usage[f.texture] = usage.get(f.texture, 0) + 1
    common_tex = max(sorted(usage), key=lambda t: usage[t]) if usage else 'base'

    for kind, count in edits:
        if kind in ('move', 'bigmove'):
            for b in pick(rnd, small if kind == 'move' else big, count):
                axis = rnd.randrange(3)
                d = MOVE_DIST if rnd.random() < 0.5 else -MOVE_DIST
                for f in b.faces:
                    for i in range(axis, 9, 3):
                        f.points[i] += d
                lo, _ = b.bounds()
                log.append(f'move brush near ({fmt_num(lo[0])} {fmt_num(lo[1])} {fmt_num(lo[2])}) {"xyz"[axis]}{d:+d}')
        elif kind == 'align':
            for b in pick(rnd, plain, count):
                for f in b.faces:
                    f.rest = shift_alignment(f.rest, 16)
                log.append(f'shift texture alignment of brush {",".join(sorted(b.textures()))} by 16')
        elif kind == 'texture':
            for b in pick(rnd, plain, count):
                old = sorted(t for t in b.textures() if not is_hidden(t))
                others = [t for t in textures if t not in old] or textures
                new = rnd.choice(others)
                for f in b.faces:
                    if not is_hidden(f.texture):
                        f.texture = new
                log.append(f'retexture brush {",".join(old)} -> {new}')
        elif kind == 'add':
            for e in pick(rnd, point_entities(entities), count):
                x, y, z = origin_of(e)
                world.brushes.append(box_brush((x, y, z - 24), 8, common_tex))
                log.append(f'add 16u box under {e.get("classname")} at ({fmt_num(x)} {fmt_num(y)} {fmt_num(z)})')
        elif kind == 'entity':
            for e in pick(rnd, point_entities(entities), count):
                x, y, z = origin_of(e)
                e.set('origin', f'{fmt_num(x + 16)} {fmt_num(y)} {fmt_num(z)}')
                light = e.get('light')
                if light is not None:
                    try:
                        e.set('light', fmt_num(float(light) + 10))
                    except ValueError:
                        pass
                log.append(f'{e.get("classname")} at ({fmt_num(x)} {fmt_num(y)} {fmt_num(z)}) moved x+16')
        else:
            raise ValueError(f'unknown edit kind {kind}')
    return log


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('base')
    ap.add_argument('out', nargs='?')
    ap.add_argument('scenario', nargs='?')
    ap.add_argument('--attempt', type=int, default=0, help='another pick of brushes (see bench_seeds.json)')
    ap.add_argument('--list', action='store_true')
    args = ap.parse_args()

    if args.list:
        for name, (desc, _) in SCENARIOS.items():
            print(f'{name:10} {desc}')
        return 0
    if not args.out or args.scenario not in SCENARIOS:
        ap.error('need out.map and a scenario (see --list)')

    with open(args.base, encoding='utf-8', errors='surrogateescape') as f:
        entities = parse_map(f.read())
    for line in apply_scenario(entities, args.scenario, args.attempt):
        print(line)
    with open(args.out, 'w', encoding='utf-8', errors='surrogateescape', newline='\n') as f:
        f.write(write_map(entities))
    return 0


if __name__ == '__main__':
    sys.exit(main())
