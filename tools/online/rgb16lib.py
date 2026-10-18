import zipfile, struct, zlib
O2R = r"C:\Users\Lenovo\Desktop\games\SpaghettiKart-src\build\x64\Release\mk64.o2r"

def load_rgba16(path):
    z = zipfile.ZipFile(O2R)
    d = z.read(path)
    t, w, h, size = struct.unpack_from("<IIII", d, 0x40)
    assert t == 2 and size == w * h * 2, (t, w, h, size)
    raw = d[0x50:]
    px = []
    for y in range(h):
        row = []
        for x in range(w):
            v = (raw[(y * w + x) * 2] << 8) | raw[(y * w + x) * 2 + 1]
            r = (v >> 11) & 31; g = (v >> 6) & 31; b = (v >> 1) & 31; a = v & 1
            row.append((r * 255 // 31, g * 255 // 31, b * 255 // 31, 255 * a))
        px.append(row)
    return px

def write_png_rgba(path, px, scale=1, bg=(40, 0, 60)):
    h = len(px); w = len(px[0])
    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            r, g, b, a = px[y][x]
            if a == 0:
                r, g, b = bg
            row += bytes([r, g, b]) * scale
        for _ in range(scale):
            rows.append(b"\x00" + bytes(row))
    raw = b"".join(rows)
    def chunk(t, data):
        c = struct.pack(">I", len(data)) + t + data
        return c + struct.pack(">I", zlib.crc32(t + data) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w * scale, h * scale, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)
