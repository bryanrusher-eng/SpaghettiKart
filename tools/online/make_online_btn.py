import sys, os, math, struct
sys.path.insert(0, os.path.dirname(__file__))
from rgb16lib import *

HERE = os.path.dirname(__file__)
base = load_rgba16('textures/texture_tkmk00/texture_mode_battle')  # 64x18
W, H = 64, 18
BLACK = (0, 0, 0, 255)

img = [[base[y][x] for x in range(W)] for y in range(H)]

# --- caja de texto: todo lo transparente pasa a negro opaco (no se dibuja caja detras) ---
for y in range(H):
    for x in range(W):
        if img[y][x][3] == 0:
            img[y][x] = BLACK

# colores por fila del degradado del texto original (filas 3..14)
rowcol = {}
for y in range(3, 15):
    cols = [img[y][x] for x in range(19, 61) if max(img[y][x][:3]) >= 40 and not (abs(img[y][x][0] - img[y][x][1]) < 24 and abs(img[y][x][1] - img[y][x][2]) < 24)]
    r = sum(c[0] for c in cols) // len(cols)
    g = sum(c[1] for c in cols) // len(cols)
    b = sum(c[2] for c in cols) // len(cols)
    rowcol[y] = (r, g, b, 255)

# borra el texto original (deja el interior negro)
for y in range(2, 16):
    for x in range(17, 63):
        img[y][x] = BLACK

# ---------- letras (12 filas) ----------
GH = 12


def solid(w):
    return [[1] * w for _ in range(GH)]


def glyph_O(w=7):
    g = [[0] * w for _ in range(GH)]
    for y in range(GH):
        for x in range(w):
            inside = 2 <= x < w - 2 and 2 <= y < GH - 2
            corner = (x in (0, w - 1)) and (y in (0, GH - 1))
            if not inside and not corner:
                g[y][x] = 1
    return g


def glyph_N(w=7):
    g = [[0] * w for _ in range(GH)]
    for y in range(GH):
        for x in range(w):
            if x < 2 or x >= w - 2:
                g[y][x] = 1
        c = 1.0 + (w - 4.0) * y / (GH - 1)
        for x in range(w):
            if abs(x - (c + 1.0)) < 1.15:
                g[y][x] = 1
    return g


def glyph_L(w=6):
    g = [[0] * w for _ in range(GH)]
    for y in range(GH):
        for x in range(w):
            if x < 3 or y >= GH - 3:
                g[y][x] = 1
    return g


def glyph_I(w=3):
    return solid(w)


def glyph_E(w=6):
    g = [[0] * w for _ in range(GH)]
    for y in range(GH):
        for x in range(w):
            if x < 3 or y < 3 or y >= GH - 3 or (4 <= y <= 6 and x < w - 1):
                g[y][x] = 1
    return g


letters = [glyph_O(7), glyph_N(7), glyph_L(6), glyph_I(3), glyph_N(7), glyph_E(6)]
total = sum(len(l[0]) for l in letters) + (len(letters) - 1)  # 1 columna de contorno entre letras
x0 = 18 + (42 - (total + 2)) // 2 + 1  # centrado en la caja (columnas 18..59)

# mascara de relleno en la caja
fill = [[0] * W for _ in range(H)]
x = x0 + 1
for l in letters:
    for gy in range(GH):
        for gx in range(len(l[0])):
            if l[gy][gx]:
                fill[3 + gy][x + gx] = 1
    x += len(l[0]) + 1

# contorno negro (ya es negro: solo hace falta el relleno de color); recolorea
for y in range(H):
    for x in range(W):
        if fill[y][x]:
            img[y][x] = rowcol.get(y, rowcol[3])

# ---------- icono: casilla gris con bisel + globo ----------
gray_samples = [img[y][x] for y in range(2, 16) for x in range(2, 14) if base[y][x][3] and max(base[y][x][:3]) < 200 and abs(base[y][x][0] - base[y][x][1]) < 24]
gr = sum(c[0] for c in gray_samples) // len(gray_samples)
gg = sum(c[1] for c in gray_samples) // len(gray_samples)
gb = sum(c[2] for c in gray_samples) // len(gray_samples)
GRAY = (gr, gg, gb, 255)
for y in range(1, 17):
    for x in range(1, 16):
        img[y][x] = GRAY

cx, cy, r = 7.6, 8.6, 6.2
light = (-0.5, -0.6, 0.62)
ln = math.sqrt(sum(v * v for v in light))
light = tuple(v / ln for v in light)


def clamp(v):
    return max(0, min(255, int(v)))


def land(lon, lat):
    blobs = [(-0.65, 0.5, 0.6, 0.42), (1.0, -0.25, 0.4, 0.7), (2.6, 0.3, 0.5, 0.35)]
    for (l0, b0, a, b) in blobs:
        dl = (lon - l0 + math.pi) % (2 * math.pi) - math.pi
        if (dl / a) ** 2 + ((lat - b0) / b) ** 2 < 1.0:
            return True
    return False


rot = 0.35
for y in range(1, 17):
    for x in range(1, 16):
        nx = (x - cx) / r
        ny = (y - cy) / r
        d2 = nx * nx + ny * ny
        if d2 > 1.0:
            continue
        nz = math.sqrt(max(0.0, 1.0 - d2))
        lat = math.asin(max(-1, min(1, -ny)))
        lon = math.atan2(nx, nz) + rot
        shade = max(0.0, nx * light[0] + ny * light[1] + nz * light[2])
        base_col = (40, 120, 235)
        if land(lon, lat):
            base_col = (70, 190, 80)
        # rejilla de red: paralelos y meridianos
        on_grid = False
        for k in range(-2, 3):
            if abs(lat - k * math.pi / 6) < 0.075:
                on_grid = True
        for k in range(8):
            if abs(((lon - k * math.pi / 4) + math.pi) % (2 * math.pi) - math.pi) * max(0.35, math.cos(lat)) < 0.07:
                on_grid = True
        if on_grid:
            base_col = (200, 235, 255)
        f = 0.45 + 0.75 * shade
        col = (clamp(base_col[0] * f), clamp(base_col[1] * f), clamp(base_col[2] * f), 255)
        if d2 > 0.78:  # borde oscuro del globo
            t = (d2 - 0.78) / 0.22
            col = (clamp(col[0] * (1 - 0.6 * t)), clamp(col[1] * (1 - 0.6 * t)), clamp(col[2] * (1 - 0.5 * t)), 255)
        if shade > 0.93 and d2 < 0.5:  # brillo
            col = (clamp(col[0] + 80), clamp(col[1] + 80), clamp(col[2] + 60), 255)
        img[y][x] = col

if __name__ == '__main__':
    write_png_rgba(os.path.join(HERE, 'online_btn.png'), img, 10)
    # vista junto a los botones originales
    others = [base, load_rgba16('textures/texture_tkmk00/texture_mode_mario_gp')]
    sheet = [[(0, 0, 0, 255)] * W for _ in range(H * 3 + 8)]
    for k, im in enumerate([img] + others):
        for y in range(H):
            for x in range(W):
                sheet[k * (H + 2) + y][x] = im[y][x]
    write_png_rgba(os.path.join(HERE, 'online_cmp.png'), sheet, 8)

    # recurso binario OTEX RGBA16 (misma cabecera que las texturas originales)
    out = bytearray()
    for y in range(H):
        for x in range(W):
            r_, g_, b_, a_ = img[y][x]
            v = ((r_ >> 3) << 11) | ((g_ >> 3) << 6) | ((b_ >> 3) << 1) | (1 if a_ else 0)
            out += struct.pack('>H', v)
    header = bytes.fromhex("000000005845544f00000000efbeaddeefbeadde") + bytes(0x40 - 20)
    data = header + struct.pack('<IIII', 2, W, H, W * H * 2) + bytes(out)
    open(os.path.join(HERE, 'texture_online'), 'wb').write(data)
    print('ok', len(data))
