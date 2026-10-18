import zipfile, struct, zlib

O2R = r"C:\Users\Lenovo\Desktop\games\SpaghettiKart-src\build\x64\Release\mk64.o2r"
W, H = 128, 64


def load_i4(name):
    z = zipfile.ZipFile(O2R)
    d = z.read("textures/common_data/common_texture_hud_" + name)
    assert struct.unpack_from("<IIII", d, 0x40) == (5, W, H, 4096)
    raw = d[0x50:]
    px = [[0] * W for _ in range(H)]
    for y in range(H):
        for x in range(W):
            b = raw[(y * W + x) // 2]
            px[y][x] = (b >> 4) if x % 2 == 0 else (b & 15)
    return px


def write_png(path, px, scale=1):
    h = len(px)
    w = len(px[0])
    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            row += bytes([px[y][x] * 17]) * scale
        for _ in range(scale):
            rows.append(b"\x00" + bytes(row))
    raw = b"".join(rows)

    def chunk(t, data):
        c = struct.pack(">I", len(data)) + t + data
        return c + struct.pack(">I", zlib.crc32(t + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w * scale, h * scale, 8, 0, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


def to_i4_resource(px):
    out = bytearray()
    for y in range(H):
        for x in range(0, W, 2):
            out.append((px[y][x] << 4) | px[y][x + 1])
    header = bytes.fromhex("000000005845544f00000000efbeaddeefbeadde") + bytes(0x40 - 20)
    return header + struct.pack("<IIII", 5, W, H, 4096) + bytes(out)
