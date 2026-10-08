#!/usr/bin/env python3
"""Adds a grid of hint brushes to the worldspawn of a Quake .map.

qbsp only splits the BSP along brush planes, so a big open area becomes a few
huge leafs and gridvis can't cull inside it. Hint planes every N units force
the split, so leafs end up at most N x N (in X and Y).

    hintgrid.py in.map out.map [--grid 512] [--bounds xmin ymin zmin xmax ymax zmax]

Without --bounds, the grid covers the bounds of the worldspawn brushes.
"""
import argparse
import re
import sys

# Face of an axis-aligned box: outward normal -> (a, b) with a x b = normal.
# qbsp computes normal = (p0 - p1) x (p2 - p1), so p0 = p1 + a, p2 = p1 + b.
_AXES = {
    '+x': ((0, 1, 0), (0, 0, 1)),
    '-x': ((0, 0, 1), (0, 1, 0)),
    '+y': ((0, 0, 1), (1, 0, 0)),
    '-y': ((1, 0, 0), (0, 0, 1)),
    '+z': ((1, 0, 0), (0, 1, 0)),
    '-z': ((0, 1, 0), (1, 0, 0)),
}


def _fmt(v):
    return '( %s )' % ' '.join('%g' % c for c in v)


def box_brush(mins, maxs, textures):
    """Text of an axis-aligned brush. textures: dict face -> texture name
    ('+x', '-x', ...); missing faces use textures['default']."""
    lines = ['{']
    for face, (a, b) in _AXES.items():
        axis = 'xyz'.index(face[1])
        p1 = [0.0, 0.0, 0.0]
        p1[axis] = maxs[axis] if face[0] == '+' else mins[axis]
        p0 = [p1[i] + 64 * a[i] for i in range(3)]
        p2 = [p1[i] + 64 * b[i] for i in range(3)]
        tex = textures.get(face, textures.get('default'))
        lines.append('%s %s %s %s 0 0 0 1 1' % (_fmt(p0), _fmt(p1), _fmt(p2), tex))
    lines.append('}')
    return '\n'.join(lines)


def hint_brushes(bounds, grid):
    """Hint brushes with one hint face on every grid line inside bounds."""
    (x0, y0, z0), (x1, y1, z1) = bounds
    out = []
    thick = 8
    x = (int(x0) // grid + 1) * grid
    while x < x1:
        out.append(box_brush((x, y0, z0), (x + thick, y1, z1), {'-x': 'hint', 'default': 'hintskip'}))
        x += grid
    y = (int(y0) // grid + 1) * grid
    while y < y1:
        out.append(box_brush((x0, y, z0), (x1, y + thick, z1), {'-y': 'hint', 'default': 'hintskip'}))
        y += grid
    return out


_POINT = re.compile(r'\(\s*([-\d.eE+]+)\s+([-\d.eE+]+)\s+([-\d.eE+]+)\s*\)')


def worldspawn_span(text):
    """(start, end) of the worldspawn entity: the first { ... } block."""
    start = text.index('{')
    depth = 0
    for i in range(start, len(text)):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                return start, i
    raise ValueError('unbalanced braces')


def brush_bounds(text):
    """Approximate bounds of the brushes (from their plane points)."""
    pts = [tuple(float(c) for c in m) for m in _POINT.findall(text)]
    if not pts:
        raise ValueError('no brushes found')
    return (tuple(min(p[i] for p in pts) for i in range(3)), tuple(max(p[i] for p in pts) for i in range(3)))


def add_hint_grid(text, grid, bounds=None):
    start, end = worldspawn_span(text)
    if bounds is None:
        bounds = brush_bounds(text[start:end])
    brushes = hint_brushes(bounds, grid)
    return text[:end] + '\n'.join(brushes) + '\n' + text[end:], len(brushes)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('input')
    ap.add_argument('output')
    ap.add_argument('--grid', type=int, default=512, help='grid spacing in units (default 512)')
    ap.add_argument('--bounds', type=float, nargs=6, metavar='N', help='xmin ymin zmin xmax ymax zmax')
    args = ap.parse_args()

    with open(args.input, encoding='latin-1') as f:
        text = f.read()
    bounds = (tuple(args.bounds[:3]), tuple(args.bounds[3:])) if args.bounds else None
    text, count = add_hint_grid(text, args.grid, bounds)
    with open(args.output, 'w', encoding='latin-1', newline='\n') as f:
        f.write(text)
    print('[hintgrid] %d hint brushes every %d units -> %s' % (count, args.grid, args.output))


if __name__ == '__main__':
    sys.exit(main())
