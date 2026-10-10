#!/usr/bin/env python3
"""Writes a WAD2 with the textures embedded in a Quake BSP (read from a .bsp file or from a .pak).

For test maps from the original game, whose texture wads aren't shipped: the textures come out of
the game's own compiled map (e.g. E1M1's from id1/pak0.pak). The wads are the game's data: they
stay out of git (*.wad in .gitignore).

    wadfrombsp.py D:/quake/id1/pak0.pak:maps/e1m1.bsp testmap/gfx/base.wad
    wadfrombsp.py some.bsp out.wad
"""
import argparse
import struct
import sys

LUMP_TEXTURES = 2


def read_from_pak(pak_path, name):
    with open(pak_path, 'rb') as f:
        data = f.read()
    magic, dirofs, dirlen = struct.unpack_from('<4sii', data, 0)
    if magic != b'PACK':
        raise ValueError(f'{pak_path}: not a pak file')
    for i in range(dirlen // 64):
        entry_name, filepos, filelen = struct.unpack_from('<56sii', data, dirofs + 64 * i)
        if entry_name.split(b'\0', 1)[0].decode('latin-1').lower() == name.lower():
            return data[filepos:filepos + filelen]
    raise ValueError(f'{name} not found in {pak_path}')


def textures_of(bsp):
    version = struct.unpack_from('<i', bsp, 0)[0]
    if version != 29 and bsp[:4] != b'BSP2':
        raise ValueError('not a BSP29 / BSP2 file')
    ofs, size = struct.unpack_from('<ii', bsp, 4 + 8 * LUMP_TEXTURES)
    lump = bsp[ofs:ofs + size]
    count = struct.unpack_from('<i', lump, 0)[0]
    offsets = struct.unpack_from(f'<{count}i', lump, 4)
    textures = []
    for i, o in enumerate(offsets):
        if o < 0:
            continue  # a missing texture
        name, width, height = struct.unpack_from('<16sII', lump, o)
        end = offsets[i + 1] if i + 1 < count and offsets[i + 1] > o else len(lump)
        # header (40 bytes) + 4 mip levels
        mip_size = 40 + sum((width >> l) * (height >> l) for l in range(4))
        data = lump[o:o + min(mip_size, end - o)]
        textures.append((name.split(b'\0', 1)[0].decode('latin-1'), data))
    return textures


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('bsp', help='a .bsp, or pak.pak:maps/name.bsp')
    ap.add_argument('out')
    args = ap.parse_args()

    if '.pak:' in args.bsp.lower():
        i = args.bsp.lower().index('.pak:') + 4
        bsp = read_from_pak(args.bsp[:i], args.bsp[i + 1:])
    else:
        with open(args.bsp, 'rb') as f:
            bsp = f.read()

    textures = textures_of(bsp)
    body = b''
    lumps = []
    pos = 12
    for name, data in textures:
        lumps.append((pos, len(data), name))
        body += data
        pos += len(data)
    directory = b''.join(struct.pack('<iiibbh16s', p, n, n, 0x44, 0, 0, name.encode('latin-1')[:15])
                         for p, n, name in lumps)
    with open(args.out, 'wb') as f:
        f.write(struct.pack('<4sii', b'WAD2', len(lumps), pos))
        f.write(body)
        f.write(directory)
    print(f'{len(textures)} textures -> {args.out}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
