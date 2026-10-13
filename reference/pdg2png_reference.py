"""PDG HH v2 decoder reconstructed from Pdg2Pic.exe and paired samples.
Supported: metadata offset 12, payload types 2 and 17, monochrome fax pages.
Uses Pillow for PNG writing. CCITT code tables follow ITU-T T.4/T.6.
Code trees were serialized from pdfminer.six ccitt.py, copyright Yusuke Shinyama
and contributors, MIT License; see ../licenses/pdfminer_CCITT_LICENSE.txt.
PNG writing uses the observed sample resolution of 300 DPI.
No Pdg2Pic process or native code is executed.
"""
from pathlib import Path
from PIL import Image
import argparse, hashlib, struct
MODE = [[[[[[[[[[[[None, [[[[[[[[[[[[None, 'e'], None], None], None], None], None], None], None], None], None], None], None]], None], None], None], None], [[['x1', 'x2'], ['x3', 'x4']], [['x5', 'x6'], ['x7', 'u']]]], [-3, 3]], [-2, 2]], 'p'], 'h'], [-1, 1]], 0]
WHITE = [[[[[[[[None, [[[1792, [1984, 2048]], [[2112, 2176], [2240, 2304]]], [[1856, 1920], [[2368, 2432], [2496, 2560]]]]], [29, 30]], [[45, 46], 22]], [[23, [47, 48]], 13]], [[[20, [33, 34]], [[35, 36], [37, 38]]], [[19, [31, 32]], 1]]], [[[12, [[53, 54], 26]], [[[39, 40], [41, 42]], [[43, 44], 21]]], [[[28, [61, 62]], [[63, 0], [320, 384]]], 10]]], [[[11, [[27, [59, 60]], [[[1472, 1536], [1600, 1728]], 18]]], [[[24, [49, 50]], [[51, 52], 25]], [[[55, 56], [57, 58]], 192]]], [[[1664, [[448, 512], [[704, 768], 640]]], [[[576, [832, 896]], [[960, 1024], [1088, 1152]]], [[[1216, 1280], [1344, 1408]], 256]]], 2]]], [[[3, [128, 8]], [[9, [16, 17]], 4]], [[5, [[14, 15], 64]], [6, 7]]]]
BLACK = [[[[[[[[None, [[[1792, [1984, 2048]], [[2112, 2176], [2240, 2304]]], [[1856, 1920], [[2368, 2432], [2496, 2560]]]]], [[[18, [[52, [640, 704]], [[768, 832], 55]]], [[[56, [1280, 1344]], [[1408, 1472], 59]], [[60, [1536, 1600]], 24]]], [[[25, [[1664, 1728], 320]], [[384, 448], [[512, 576], 53]]], [[[54, [896, 960]], [[1024, 1088], [1152, 1216]]], 64]]]], [[13, [[[23, [50, 51]], [[44, 45], [46, 47]]], [[[57, 58], [61, 256]], 16]]], [[[17, [[48, 49], [62, 63]]], [[[30, 31], [32, 33]], [[40, 41], 22]]], 14]]], [[10, 11], [[[15, [[[128, 192], [26, 27]], [[28, 29], 19]]], [[[20, [34, 35]], [[36, 37], [38, 39]]], [[21, [42, 43]], 0]]], 12]]], [[9, 8], 7]], [6, 5]], [1, 4]], [3, 2]]
M = 4294967295

def tea(data, key):
    k0, k1, k2, k3 = struct.unpack('<4I', key)
    a = bytearray(data)
    for p in range(0, len(a) - 15, 16):
        v0, v1, v2, v3 = struct.unpack_from('<4I', a, p)
        s = 3816266640
        for r in range(16):
            v3 = v3 - ((v0 >> 5) + k1 ^ (v0 << 4) + k2 ^ v0 + s) & M
            v2 = v2 - ((v3 << 4) + k0 ^ (v3 >> 5) + k3 ^ v3 + s) & M
            v1 = v1 - ((v2 >> 5) + k3 ^ (v2 << 4) + k2 ^ v2 + s) & M
            v0 = v0 - ((v1 << 4) + k0 ^ (v1 >> 5) + k1 ^ v1 + s) & M
            s = s - 2654435769 & M
        struct.pack_into('<4I', a, p, v0, v1, v2, v3)
    return bytes(a)

def sparse(data, w, h, L, z):
    a = w & 255
    d = w - h & 255
    s = w * h & 255
    b = L >> 8 & 255
    q = b * w & 255
    t = (w | h) & 255
    n = w + h + L & 255
    e = s * a + z & 255
    A = e + q * b + t * s - n * d + 250 & 511
    B = t * t + n * d * s - e * q + 250 & 511
    i, j = sorted((A, B))
    k = 19 if j - i == 17 else 17
    if i == j:
        return (bytes(data), dict(A=A, B=B, i=i, j=j, k=k))
    if max(i + k, j + k) >= len(data):
        raise ValueError('Payload is too short for the type 17 transform')
    a = bytearray(data)
    old = a[i]
    a[i] = a[j] ^ a[j + k]
    a[j] = old ^ a[i + k]
    return (bytes(a), dict(A=A, B=B, i=i, j=j, k=k))

class Reader:

    def __init__(self, data):
        self.data = data
        self.pos = 0

    def read(self, tree):
        t = tree
        while isinstance(t, list):
            p = self.pos
            if p >= len(self.data) * 8:
                raise ValueError('Truncated fax bitstream')
            self.pos = p + 1
            t = t[self.data[p // 8] >> 7 - p % 8 & 1]
        if t is None:
            raise ValueError('invalid at ' + str(self.pos))
        return t

    def run(self, color):
        n = 0
        while True:
            x = self.read(WHITE if color else BLACK)
            if not isinstance(x, int):
                raise ValueError(x)
            n += x
            if x < 64:
                return n

def decode(data, w, h):
    r = Reader(data)
    prev = [0, w, w, w, w]
    lines = []
    pad = -w % 8
    for y in range(h):
        cur = []
        p = 0
        pos = 0
        color = 0
        while pos < w:
            m = r.read(MODE)
            if m == 'p':
                p += 1
                pos = prev[p]
                p += 1
            elif m == 'h':
                pos += r.run(color)
                cur.append(pos)
                pos += r.run(1 - color)
                cur.append(pos)
                while p < len(prev) and prev[p] <= pos:
                    p += 2
            elif isinstance(m, int):
                pos = max(0, prev[p] + m)
                p += 1
                cur.append(pos)
                color = 1 - color
            else:
                raise ValueError((y, pos, m))
        bits = 0
        for j in range(0, len(cur) - 1, 2):
            a = min(w, max(0, cur[j]))
            b = min(w, max(0, cur[j + 1]))
            if b > a:
                bits |= (1 << b - a) - 1 << w - b
        lines.append((bits << pad).to_bytes((w + 7) // 8, 'big'))
        prev = cur + [pos] * 4
    return (b''.join(lines), r.pos)

def decode_pdg(path):
    data = Path(path).read_bytes()
    if len(data) < 140 or data[:4] != b'HH\x02\x00':
        raise ValueError('Expected HH v2 PDG header')
    if struct.unpack_from('<I', data, 8)[0] != 12:
        raise ValueError('Only metadata layout offset 12 is supported')
    if data[56]:
        raise ValueError('Outer transformation at header 0x38 is not supported')
    width, height = struct.unpack_from('<HH', data, 16)
    offset, length = struct.unpack_from('<II', data, 24)
    if not (0 < width <= 65535 and 0 < height <= 65535):
        raise ValueError('Invalid image dimensions')
    if offset < 140 or offset + length > len(data):
        raise ValueError('Payload range extends outside the file')
    payload = data[offset:offset + length]
    kind = data[15]
    if kind == 2:
        if length % 16:
            raise ValueError('Type 2 payload must contain whole 16-byte blocks')
        payload = tea(payload, hashlib.md5(data[64:112]).digest())
    elif kind == 17:
        z = struct.unpack_from('<H', data, 94)[0]
        payload, _ = sparse(payload, width, height, length, z)
    else:
        raise ValueError(f'Unsupported payload encoding: {kind}')
    raster, bits = decode(payload, width, height)
    return (Image.frombytes('1', (width, height), raster), bits)

def write_png(image, target, overwrite=False):
    paletted = image.convert('L').point(lambda value: 1 if value else 0, mode='P')
    paletted.putpalette([0, 0, 0, 255, 255, 255])
    Path(target).parent.mkdir(parents=True, exist_ok=True)
    with Path(target).open('wb' if overwrite else 'xb') as handle:
        paletted.save(handle, format='PNG', bits=1, dpi=(300, 300))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path, help='One PDG file or a directory of PDG files')
    parser.add_argument('output', type=Path, help='Output PNG file, or output directory for batch conversion')
    parser.add_argument('--overwrite', action='store_true', help='Allow replacing existing PNG files')
    args = parser.parse_args()
    if args.input.is_dir():
        paths = sorted(args.input.glob('*.pdg'))
        if not paths:
            parser.error('Input directory contains no .pdg files')
        if not args.overwrite:
            existing = [args.output / (p.stem + '.png') for p in paths if (args.output / (p.stem + '.png')).exists()]
            if existing:
                parser.error(f'Output already exists: {existing[0]}')
        for source in paths:
            image, bits = decode_pdg(source)
            target = args.output / (source.stem + '.png')
            write_png(image, target, args.overwrite)
            print(f'{source.name} -> {target.name} ({image.width} x {image.height}, {bits} fax bits)')
    else:
        if args.output.exists() and (not args.overwrite):
            parser.error(f'Output already exists: {args.output}')
        image, bits = decode_pdg(args.input)
        write_png(image, args.output, args.overwrite)
        print(f'{args.input.name} -> {args.output} ({image.width} x {image.height}, {bits} fax bits)')
if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, IndexError) as error:
        raise SystemExit(f'Conversion failed: {error}')
