# Genera los iconos del cuadro de items para el Swap legendario y el Espejo, con el estilo de los del propio juego
# (40x32, marco plateado con bisel, interior negro, relleno con degradados y contorno oscuro).
# Salida: texturas CI8 + paleta RGBA16 listas para el juego (carpeta online-assets/spaghetti-places16) y una vista previa PNG.
import sys, os, math
sys.path.insert(0, os.path.dirname(__file__))
from itemlib import *

HERE = os.path.dirname(__file__)
OUT = os.path.join(HERE, '..', '..', '..', 'online-assets', 'spaghetti-places16', 'textures', 'common_data')

SS = 8  # subpixeles por lado (antialiasing)

# ---------- geometria ----------
def sd_circle(x, y, cx, cy, r):
    return math.hypot(x - cx, y - cy) - r

def sd_box(x, y, cx, cy, hw, hh, rad=0.0):
    dx = abs(x - cx) - (hw - rad)
    dy = abs(y - cy) - (hh - rad)
    return math.hypot(max(dx, 0), max(dy, 0)) + min(max(dx, dy), 0) - rad

def sd_poly(x, y, pts):
    # distancia con signo a un poligono (negativa dentro)
    n = len(pts)
    d = float('inf')
    inside = False
    j = n - 1
    for i in range(n):
        xi, yi = pts[i]; xj, yj = pts[j]
        ex, ey = xj - xi, yj - yi
        wx, wy = x - xi, y - yi
        t = max(0.0, min(1.0, (wx * ex + wy * ey) / (ex * ex + ey * ey)))
        d = min(d, math.hypot(wx - ex * t, wy - ey * t))
        if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
            inside = not inside
        j = i
    return -d if inside else d

def lerp(a, b, t):
    return a + (b - a) * t

def mix(c1, c2, t):
    t = max(0.0, min(1.0, t))
    return tuple(lerp(c1[i], c2[i], t) for i in range(3))

def over(dst, src, a):
    a = max(0.0, min(1.0, a))
    return tuple(dst[i] * (1 - a) + src[i] * a for i in range(3))

def smooth_edge(sd, w=0.5):
    # cobertura (0..1) a partir de la distancia con signo (antialiasing del borde)
    return max(0.0, min(1.0, 0.5 - sd / (2 * w)))

# ---------- flecha del Swap ----------
def arrow_pts(cx, cy, length, shaft, head_h, head_l, direction):
    hl = length / 2.0
    pts = [(-hl, -shaft), (hl - head_l, -shaft), (hl - head_l, -head_h), (hl, 0), (hl - head_l, head_h),
           (hl - head_l, shaft), (-hl, shaft)]
    return [(cx + direction * px, cy + py) for px, py in pts]

GOLD_TOP = (255, 246, 160)
GOLD_MID = (255, 205, 40)
GOLD_BOT = (205, 120, 5)
OUTLINE = (92, 48, 0)

def shade_arrow(x, y, pts, cy, shaft, top_col, mid_col, bot_col):
    sd = sd_poly(x, y, pts)
    cov = smooth_edge(sd)
    if cov <= 0:
        return None
    t = (y - (cy - shaft * 1.6)) / (shaft * 3.2)  # 0 arriba .. 1 abajo (de la cabeza)
    if t < 0.45:
        col = mix(top_col, mid_col, t / 0.45)
    else:
        col = mix(mid_col, bot_col, (t - 0.45) / 0.55)
    # contorno oscuro por dentro del borde
    ol = max(0.0, min(1.0, (sd + 1.25) / 1.0))
    col = mix(col, OUTLINE, ol)
    # brillo blanco fino cerca del borde superior
    if -2.6 < sd < -1.2 and y < cy - shaft * 0.1:
        col = mix(col, (255, 255, 255), 0.55)
    return col, cov

def sparkle(x, y, cx, cy, r):
    # estrella de 4 puntas (destello)
    dx, dy = abs(x - cx), abs(y - cy)
    k = (dx / r) ** 0.6 + (dy / r) ** 0.6
    return max(0.0, 1.0 - k) if k < 1.0 else 0.0

def swap_pixel(x, y):
    col = (0.0, 0.0, 0.0)
    # resplandor dorado de fondo
    d = math.hypot((x - 20) / 15.0, (y - 16) / 11.0)
    if d < 1.0:
        col = over(col, (255, 190, 40), 0.30 * (1 - d) ** 1.5)
    # flecha superior (derecha) y flecha inferior (izquierda)
    up = arrow_pts(19.5, 10.3, 26, 3.3, 6.7, 8.6, +1)
    dn = arrow_pts(20.5, 21.9, 26, 3.3, 6.7, 8.6, -1)
    r2 = shade_arrow(x, y, dn, 21.9, 3.3, (255, 235, 140), (255, 150, 30), (190, 70, 0))
    if r2:
        col = over(col, r2[0], r2[1])
    r1 = shade_arrow(x, y, up, 10.3, 3.3, GOLD_TOP, GOLD_MID, GOLD_BOT)
    if r1:
        col = over(col, r1[0], r1[1])
    # destellos blancos
    for (sx, sy, sr, sa) in ((8.5, 6.0, 3.3, 1.0), (32.0, 26.2, 2.9, 0.95), (33.5, 6.2, 1.8, 0.8)):
        s = sparkle(x, y, sx, sy, sr)
        if s > 0:
            col = over(col, (255, 255, 235), min(1.0, s * 1.6) * sa)
    return col

# ---------- espejo ----------
def mirror_pixel(x, y):
    col = (0.0, 0.0, 0.0)
    cx, cy, R = 20.0, 13.2, 9.6
    # resplandor violeta de fondo
    d = math.hypot((x - cx) / 17.0, (y - 15.0) / 13.0)
    if d < 1.0:
        col = over(col, (150, 80, 255), 0.30 * (1 - d) ** 1.4)
    # mango (dorado)
    sdh = sd_box(x, y, cx, 24.6, 2.2, 4.2, 1.0)
    cov = smooth_edge(sdh)
    if cov > 0:
        t = (x - (cx - 2.2)) / 4.4
        hc = mix((255, 236, 150), (200, 120, 10), t)
        hc = mix(hc, OUTLINE, max(0.0, min(1.0, (sdh + 1.1) / 0.9)))
        col = over(col, hc, cov)
    # marco plateado circular
    sdc = sd_circle(x, y, cx, cy, R)
    cov = smooth_edge(sdc)
    if cov > 0:
        ang = math.atan2(y - cy, x - cx)
        shade = 0.5 + 0.5 * math.cos(ang + 0.9)       # luz arriba-izquierda
        ring = mix((120, 120, 135), (245, 245, 255), shade)
        ring = mix(ring, (35, 35, 55), max(0.0, min(1.0, (sdc + 1.1) / 1.0)))
        col = over(col, ring, cov)
        # cristal (interior)
        sdi = sd_circle(x, y, cx, cy, R - 2.3)
        ci = smooth_edge(sdi)
        if ci > 0:
            u = (x - (cx - R)) / (2 * R)
            v = (y - (cy - R)) / (2 * R)
            glass = mix((150, 225, 255), (30, 75, 175), (u * 0.45 + v * 0.75))
            # destello diagonal blanco
            diag = (x - cx) * 0.8 + (y - cy) * -0.6
            if -4.6 < diag < -2.0 and sdi < -0.6:
                glass = mix(glass, (255, 255, 255), 0.7)
            if 0.2 < diag < 1.2 and sdi < -0.6:
                glass = mix(glass, (255, 255, 255), 0.35)
            col = over(col, glass, ci)
            # flecha doble horizontal (izquierda-derecha) sobre el cristal: los controles se invierten
            ay, hw = cy + 0.6, 7.3
            parts = [
                [(cx - hw, ay), (cx - hw + 3.9, ay - 3.9), (cx - hw + 3.9, ay - 1.6), (cx + hw - 3.9, ay - 1.6),
                 (cx + hw - 3.9, ay - 3.9), (cx + hw, ay), (cx + hw - 3.9, ay + 3.9), (cx + hw - 3.9, ay + 1.6),
                 (cx - hw + 3.9, ay + 1.6), (cx - hw + 3.9, ay + 3.9)]]
            for pts in parts:
                sda = sd_poly(x, y, pts)
                ca = smooth_edge(sda)
                if ca > 0:
                    t = (y - (ay - 3.9)) / 7.8
                    ac = mix((255, 255, 255), (215, 170, 255), t)
                    ac = mix(ac, (60, 15, 125), max(0.0, min(1.0, (sda + 1.05) / 0.9)))
                    col = over(col, ac, ca * ci)
    # destello
    s = sparkle(x, y, 31.5, 5.2, 2.7)
    if s > 0:
        col = over(col, (255, 255, 255), min(1.0, s * 1.7))
    return col

# ---------- composicion sobre el marco del juego ----------
def render(pixel_fn):
    idx, pal = load_item('none')
    base = to_rgba(idx, pal)
    out = [[base[y][x] for x in range(W)] for y in range(H)]
    for y in range(3, H - 3):
        for x in range(3, W - 3):
            r = g = b = 0.0
            for sy in range(SS):
                for sx in range(SS):
                    c = pixel_fn(x + (sx + 0.5) / SS, y + (sy + 0.5) / SS)
                    r += c[0]; g += c[1]; b += c[2]
            n = SS * SS
            out[y][x] = (int(r / n + 0.5), int(g / n + 0.5), int(b / n + 0.5), 255)
    return out

def reduce_palette(img):
    # el interior tiene demasiados colores: se baja de bits por canal hasta que quepa en la paleta (255 colores + transparente)
    frame = set()
    for y in range(H):
        for x in range(W):
            if not (3 <= x < W - 3 and 3 <= y < H - 3):
                frame.add(img[y][x])
    for bits in (5, 4, 3):
        step = 8 - bits
        q = [[img[y][x] if not (3 <= x < W - 3 and 3 <= y < H - 3) else
              ((img[y][x][0] >> step) * 255 // ((1 << bits) - 1), (img[y][x][1] >> step) * 255 // ((1 << bits) - 1),
               (img[y][x][2] >> step) * 255 // ((1 << bits) - 1), 255) for x in range(W)] for y in range(H)]
        cols = set()
        for row in q:
            for c in row:
                if c[3]:
                    cols.add((c[0] >> 3, c[1] >> 3, c[2] >> 3))
        if len(cols) <= 255:
            return q, bits, len(cols)
    raise SystemExit('no cabe en la paleta')

if __name__ == '__main__':
    os.makedirs(OUT, exist_ok=True)
    sheet = [[(0, 0, 0, 0)] * (W * 2 + 3) for _ in range(H + 2)]
    for k, (name, fn) in enumerate((('swap', swap_pixel), ('mirror', mirror_pixel))):
        img = render(fn)
        img, bits, ncol = reduce_palette(img)
        tex, tlut = encode_ci8(img)
        open(os.path.join(OUT, 'common_texture_item_window_%s' % name), 'wb').write(tex)
        open(os.path.join(OUT, 'common_tlut_item_window_%s' % name), 'wb').write(tlut)
        print(name, 'colores', ncol, 'bits', bits)
        for y in range(H):
            for x in range(W):
                sheet[1 + y][1 + k * (W + 1) + x] = img[y][x]
    # vista previa junto a iconos del propio juego (estrella, hongo, cascara roja) para comparar el estilo
    cmp_names = ['star', 'mushroom', 'red_shell']
    sheet2 = [[(0, 0, 0, 0)] * (W * 5 + 6) for _ in range(H + 2)]
    for k, nm in enumerate(cmp_names):
        idx, pal = load_item(nm)
        im = to_rgba(idx, pal)
        for y in range(H):
            for x in range(W):
                sheet2[1 + y][1 + k * (W + 1) + x] = im[y][x]
    for k in range(2):
        for y in range(H):
            for x in range(W):
                sheet2[1 + y][1 + (3 + k) * (W + 1) + x] = sheet[1 + y][1 + k * (W + 1) + x]
    write_png_rgba(os.path.join(HERE, 'special_icons_preview.png'), sheet2, 5, bg=(70, 70, 85))
    print('ok')
