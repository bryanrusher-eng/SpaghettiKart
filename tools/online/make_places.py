import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from i4lib import *

OUT = os.path.dirname(__file__)
MASK = 8  # umbral para considerar "pintado" (0..15)


def components(px, thr=MASK):
    seen = [[False] * W for _ in range(H)]
    comps = []
    for y in range(H):
        for x in range(W):
            if px[y][x] >= thr and not seen[y][x]:
                stack = [(x, y)]
                seen[y][x] = True
                pts = []
                while stack:
                    cx, cy = stack.pop()
                    pts.append((cx, cy))
                    for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1), (1, 1), (-1, -1), (1, -1), (-1, 1)):
                        nx, ny = cx + dx, cy + dy
                        if 0 <= nx < W and 0 <= ny < H and not seen[ny][nx] and px[ny][nx] >= thr:
                            seen[ny][nx] = True
                            stack.append((nx, ny))
                comps.append(pts)
    return comps


def bbox(pts):
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    return min(xs), min(ys), max(xs), max(ys)


def crop_glyph(px, comp_pts):
    """Devuelve (matriz recortada con el antialias vecino, x0, y0)."""
    x0, y0, x1, y1 = bbox(comp_pts)
    pad = 2
    x0 = max(0, x0 - pad)
    y0 = max(0, y0 - pad)
    x1 = min(W - 1, x1 + pad)
    y1 = min(H - 1, y1 + pad)
    own = set(comp_pts)
    g = [[0] * (x1 - x0 + 1) for _ in range(y1 - y0 + 1)]
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            # incluye el borde suave (valores bajos) pegado a este componente
            if (x, y) in own or px[y][x] > 0:
                near = any((x + dx, y + dy) in own for dx in (-2, -1, 0, 1, 2) for dy in (-2, -1, 0, 1, 2))
                if near:
                    g[y - y0][x - x0] = px[y][x]
    return g, x0, y0


def digit_and_suffix(name):
    px = load_i4(name)
    comps = components(px)
    comps.sort(key=len, reverse=True)
    dig = comps[0]
    dg, dx0, dy0 = crop_glyph(px, dig)
    dbox = bbox(dig)
    # sufijo: todo lo que esta a la derecha del digito
    suf = [[0] * W for _ in range(H)]
    for c in comps[1:]:
        b = bbox(c)
        if b[0] > dbox[2] - 6:
            for (x, y) in c:
                suf[y][x] = px[y][x]
    return px, dg, dbox, suf


def resize(g, nw, nh):
    h = len(g)
    w = len(g[0])
    out = [[0.0] * nw for _ in range(nh)]
    for y in range(nh):
        for x in range(nw):
            sx0, sx1 = x * w / nw, (x + 1) * w / nw
            sy0, sy1 = y * h / nh, (y + 1) * h / nh
            tot = 0.0
            area = 0.0
            iy = int(sy0)
            while iy < sy1 and iy < h:
                ix = int(sx0)
                while ix < sx1 and ix < w:
                    ox = min(ix + 1, sx1) - max(ix, sx0)
                    oy = min(iy + 1, sy1) - max(iy, sy0)
                    if ox > 0 and oy > 0:
                        tot += g[iy][ix] * ox * oy
                        area += ox * oy
                    ix += 1
                iy += 1
            out[y][x] = tot / area if area else 0
    return [[min(15, int(round(v))) for v in row] for row in out]


def rot180(g):
    return [row[::-1] for row in g[::-1]]


def binarize(g, thr=MASK):
    return [[1 if v >= thr else 0 for v in row] for row in g]


def fill_holes(m):
    h = len(m)
    w = len(m[0])
    outside = [[False] * w for _ in range(h)]
    stack = [(0, 0)]
    outside[0][0] = True
    while stack:
        x, y = stack.pop()
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nx, ny = x + dx, y + dy
            if 0 <= nx < w and 0 <= ny < h and not outside[ny][nx] and m[ny][nx] == 0:
                outside[ny][nx] = True
                stack.append((nx, ny))
    return [[0 if outside[y][x] else 1 for x in range(w)] for y in range(h)]


def dilate(m, r):
    h = len(m)
    w = len(m[0])
    out = [[0] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            if m[y][x]:
                for dy in range(-r, r + 1):
                    for dx in range(-r, r + 1):
                        if dx * dx + dy * dy <= r * r:
                            ny, nx = y + dy, x + dx
                            if 0 <= ny < h and 0 <= nx < w:
                                out[ny][nx] = 1
    return out


def erode(m, r):
    inv = [[1 - v for v in row] for row in m]
    # el borde exterior cuenta como vacio
    return [[1 - v for v in row] for row in dilate_pad(inv, r)]


def dilate_pad(m, r):
    h = len(m)
    w = len(m[0])
    big = [[1] * (w + 2 * r) for _ in range(h + 2 * r)]
    for y in range(h):
        for x in range(w):
            big[y + r][x + r] = m[y][x]
    d = dilate(big, r)
    return [row[r:r + w] for row in d[r:r + h]]


def blur_to_levels(m):
    """Suaviza una mascara binaria a 16 niveles (antialias basico 3x3)."""
    h = len(m)
    w = len(m[0])
    out = [[0] * w for _ in range(h)]
    k = [[1, 2, 1], [2, 4, 2], [1, 2, 1]]
    for y in range(h):
        for x in range(w):
            s = 0
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    ny, nx = y + dy, x + dx
                    if 0 <= ny < h and 0 <= nx < w:
                        s += m[ny][nx] * k[dy + 1][dx + 1]
            out[y][x] = int(round(s / 16 * 15))
    return out


# ---------- digitos ----------
D = {}
for d, nm in [(1, '1st'), (2, '2nd'), (3, '3rd'), (4, '4th'), (5, '5th'), (6, '6th'), (7, '7th'), (8, '8th')]:
    px, g, box, suf = digit_and_suffix(nm)
    D[d] = (g, box)
    if d == 4:
        TH = (suf, box)  # sufijo "th" tal como aparece al lado de un digito

# 9 = 6 girado 180 grados (la cursiva se conserva)
g6, b6 = D[6]
D[9] = (rot180(g6), b6)

# 0 = ovalo en cursiva (superelipse con inclinacion) del mismo alto que el 8
def ring_zero(gw, gh, shear=0.21, n=2.7, side=13.0, topbot=10.5, ss=4):
    out = [[0] * gw for _ in range(gh)]
    cx = (gw - 1) / 2.0
    cy = (gh - 1) / 2.0
    a = (gw - shear * gh) / 2.0 - 1.5
    b = gh / 2.0 - 1.5
    ai = a - side
    bi = b - topbot
    for y in range(gh):
        for x in range(gw):
            acc = 0
            for sy in range(ss):
                for sx in range(ss):
                    py = y + (sy + 0.5) / ss - 0.5 - cy
                    px_ = x + (sx + 0.5) / ss - 0.5 - cx + shear * py
                    outer = (abs(px_) / a) ** n + (abs(py) / b) ** n
                    inner = (abs(px_) / ai) ** n + (abs(py) / bi) ** n
                    if outer <= 1.0 and inner > 1.0:
                        acc += 1
            out[y][x] = int(round(15.0 * acc / (ss * ss)))
    return out


g8, b8 = D[8]
D[0] = (ring_zero(int(len(g8[0]) * 0.95), len(g8)), b8)


def paste(canvas, g, x0, y0):
    for y, row in enumerate(g):
        for x, v in enumerate(row):
            yy, xx = y0 + y, x0 + x
            if 0 <= yy < H and 0 <= xx < W and v > canvas[yy][xx]:
                canvas[yy][xx] = v


def compose(n):
    canvas = [[0] * W for _ in range(H)]
    # sufijo "th": mismo sitio y tamano que en 4th..8th, pero recolocado segun el ancho
    suf, sbox = TH
    sx_min = min(x for y in range(H) for x in range(W) if suf[y][x] > 0)
    sx_max = max(x for y in range(H) for x in range(W) if suf[y][x] > 0)
    sy_min = min(y for y in range(H) for x in range(W) if suf[y][x] > 0)
    sy_max = max(y for y in range(H) for x in range(W) if suf[y][x] > 0)
    suf_crop = [row[sx_min:sx_max + 1] for row in suf[sy_min:sy_max + 1]]
    digs = [int(c) for c in str(n)]
    base = 61  # linea de base (fila inferior) de los digitos originales
    if len(digs) == 1:
        g, box = D[digs[0]]
        gh = len(g)
        gw = len(g[0])
        # mismo sitio que el 8 de "8th"
        x0 = D[8][1][0] - 2
        y0 = D[8][1][3] + 2 - gh + 1
        paste(canvas, g, x0, y0)
        paste(canvas, suf_crop, sx_min, sy_min)
    else:
        scale = 0.72
        gl = []
        for dg in digs:
            g, box = D[dg]
            nh = int(round(len(g) * scale))
            nw = int(round(len(g[0]) * scale))
            gl.append(resize(g, nw, nh))
        total = sum(len(g[0]) for g in gl) - 6 * (len(gl) - 1)
        # sufijo reducido
        ss = 0.8
        sc = resize(suf_crop, int(round(len(suf_crop[0]) * ss)), int(round(len(suf_crop) * ss)))
        total_all = total + 3 + len(sc[0])
        x = max(1, (W - total_all) // 2 - 2)
        for g in gl:
            y0 = base - len(g) + 3
            paste(canvas, g, x, y0)
            x += len(g[0]) - 6
        x += 6 + 3
        paste(canvas, sc, x, base - len(sc) + 2 - (len(gl[0]) - len(sc)) // 8)
    return canvas


if __name__ == '__main__':
    nums = list(range(9, 17))
    imgs = {n: compose(n) for n in nums}
    gap = 6
    sheet = [[0] * (W * 2 + gap) for _ in range((H + gap) * 4)]
    for i, n in enumerate(nums):
        ox = (i % 2) * (W + gap)
        oy = (i // 2) * (H + gap)
        for y in range(H):
            for x in range(W):
                sheet[oy + y][ox + x] = imgs[n][y][x]
    write_png(os.path.join(OUT, 'sheet_new.png'), sheet, 3)
    if len(sys.argv) > 1 and sys.argv[1] == 'write':
        outdir = sys.argv[2]
        os.makedirs(outdir, exist_ok=True)
        names = {9: '9th', 10: '10th', 11: '11th', 12: '12th', 13: '13th', 14: '14th', 15: '15th', 16: '16th'}
        for n, nm in names.items():
            open(os.path.join(outdir, 'common_texture_hud_' + nm), 'wb').write(to_i4_resource(imgs[n]))
        print('escritos', len(names))
    print('ok')
