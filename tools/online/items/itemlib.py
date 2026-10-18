# Utilidades para las texturas del cuadro de items (CI8 de 40x32 + paleta RGBA16 de 256 colores), como las del propio juego.
import sys, os, struct, zipfile
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from rgb16lib import write_png_rgba, O2R

PATH = 'textures/common_data/common_%s_item_window_%s'
W, H = 40, 32
HEADER = bytes.fromhex("000000005845544f00000000efbeaddeefbeadde") + bytes(0x40 - 20)

def load_item(name):
    """Devuelve (indices[H][W], paleta[256] de (r,g,b,a)) de un icono del juego (p. ej. 'star')."""
    z = zipfile.ZipFile(O2R)
    t = z.read(PATH % ('texture', name))
    p = z.read(PATH % ('tlut', name))
    ty, w, h, size = struct.unpack_from('<IIII', t, 0x40)
    assert (ty, w, h) == (4, W, H), (ty, w, h)
    idx = [[t[0x50 + y * W + x] for x in range(W)] for y in range(H)]
    ty2, n, one, size2 = struct.unpack_from('<IIII', p, 0x40)
    assert ty2 == 2 and n == 256, (ty2, n)
    pal = []
    for i in range(256):
        v = (p[0x50 + i * 2] << 8) | p[0x50 + i * 2 + 1]
        r = (v >> 11) & 31; g = (v >> 6) & 31; b = (v >> 1) & 31; a = v & 1
        pal.append((r * 255 // 31, g * 255 // 31, b * 255 // 31, 255 * a))
    return idx, pal

def to_rgba(idx, pal):
    return [[pal[idx[y][x]] for x in range(W)] for y in range(H)]

def encode_ci8(img):
    """img[H][W] de (r,g,b,a) -> (textura OTEX CI8, paleta OTEX RGBA16). Cuantiza a 255 colores (+ el 0 transparente)."""
    colors = []
    lut = {}
    idx = [[0] * W for _ in range(H)]
    # colores unicos (reducidos a 5 bits por canal, como el hardware)
    def q(c):
        r, g, b, a = c
        return ((r >> 3), (g >> 3), (b >> 3), 1 if a >= 128 else 0)
    for y in range(H):
        for x in range(W):
            c = q(img[y][x])
            if c[3] == 0:
                idx[y][x] = 0
                continue
            if c not in lut:
                lut[c] = len(colors) + 1
                colors.append(c)
            idx[y][x] = lut[c]
    assert len(colors) <= 255, 'demasiados colores: %d' % len(colors)
    tex = HEADER + struct.pack('<IIII', 4, W, H, W * H) + bytes(v for row in idx for v in row)
    pal = bytearray(512)
    for i, c in enumerate(colors):
        v = (c[0] << 11) | (c[1] << 6) | (c[2] << 1) | 1
        pal[(i + 1) * 2:(i + 1) * 2 + 2] = struct.pack('>H', v)
    tlut = HEADER + struct.pack('<IIII', 2, 256, 1, 512) + bytes(pal)
    return tex, tlut
