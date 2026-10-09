#!/usr/bin/env python3
"""Writes a placeholder WAD2 with one plain texture per texture name a .map uses.

For test maps whose real wads aren't available (e3m4: LibreQuake's texture wads are not in this
tree). With the textures present, qbsp embeds every texture with its name, so bspcheck can tell
the textures apart, and the benchmark includes the cost of loading and embedding textures.

    makewad.py map.map out.wad [--size 64]

Each texture is a flat colour picked from its name (same name, same colour).
"""
import argparse
import re
import struct
import sys
import zlib

FACE_RE = re.compile(r'^\s*\(.*?\)\s*\(.*?\)\s*\(.*?\)\s*(\S+)')


def texture_names(path):
    names = set()
    with open(path, encoding='utf-8', errors='surrogateescape') as f:
        for line in f:
            m = FACE_RE.match(line)
            if m:
                names.add(m.group(1))
    return sorted(names, key=str.lower)


def miptex(name, size):
    colour = zlib.crc32(name.lower().encode()) % 224 + 16  # avoid the fullbright end of the palette
    header = struct.pack('<16sII4I', name.encode()[:15], size, size, 0, 0, 0, 0)
    offsets = []
    data = b''
    pos = len(header)
    for level in range(4):
        side = size >> level
        offsets.append(pos)
        level_data = bytes([colour]) * (side * side)
        data += level_data
        pos += len(level_data)
    header = struct.pack('<16sII4I', name.encode()[:15], size, size, *offsets)
    return header + data


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('map')
    ap.add_argument('out')
    ap.add_argument('--size', type=int, default=64)
    args = ap.parse_args()

    names = texture_names(args.map)
    lumps = []
    body = b''
    pos = 12
    for name in names:
        data = miptex(name, args.size)
        lumps.append((pos, len(data), name))
        body += data
        pos += len(data)

    directory = b''.join(struct.pack('<iiibbh16s', p, n, n, 0x44, 0, 0, name.encode()[:15]) for p, n, name in lumps)
    with open(args.out, 'wb') as f:
        f.write(struct.pack('<4sii', b'WAD2', len(lumps), pos))
        f.write(body)
        f.write(directory)
    print(f'{len(names)} textures -> {args.out}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
