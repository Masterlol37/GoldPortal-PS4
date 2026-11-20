"""Decode Alchemy igImage2 textures from PS4 builds to RGBA, and write PNG without dependencies."""
import struct
import zlib


def _rgb565(c):
    r = (c >> 11) & 31
    g = (c >> 5) & 63
    b = c & 31
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


def _color_block(blk, out, stride, x0, y0, alpha=None, force4=True):
    c0, c1, bits = struct.unpack_from("<HHI", blk, 0)
    p0, p1 = _rgb565(c0), _rgb565(c1)
    if c0 > c1 or force4:
        pal = [p0, p1,
               tuple((2 * a + b) // 3 for a, b in zip(p0, p1)),
               tuple((a + 2 * b) // 3 for a, b in zip(p0, p1))]
    else:
        pal = [p0, p1, tuple((a + b) // 2 for a, b in zip(p0, p1)), (0, 0, 0)]
    for i in range(16):
        r, g, b = pal[(bits >> (2 * i)) & 3]
        a = alpha[i] if alpha else (0 if (not force4 and c0 <= c1 and (bits >> (2 * i)) & 3 == 3) else 255)
        o = ((y0 + i // 4) * stride + x0 + i % 4) * 4
        out[o:o + 4] = bytes((r, g, b, a))


def _alpha_block(blk):
    a0, a1 = blk[0], blk[1]
    bits = int.from_bytes(blk[2:8], "little")
    if a0 > a1:
        pal = [a0, a1] + [((6 - k) * a0 + (k + 1) * a1) // 7 for k in range(6)]
    else:
        pal = [a0, a1] + [((4 - k) * a0 + (k + 1) * a1) // 5 for k in range(4)] + [0, 255]
    return [pal[(bits >> (3 * i)) & 7] for i in range(16)]


def _morton(x, y):
    m = 0
    for b in range(3):
        m |= ((x >> b) & 1) << (2 * b) | ((y >> b) & 1) << (2 * b + 1)
    return m


def block_order(bw, bh, tiled):
    """Yield (block_x, block_y) in the order blocks are stored.

    PS4 (GNM 'thin' tiling) stores block-compressed textures as 8x8-block tiles in row-major
    order, with the blocks inside a tile in Morton (Z) order."""
    if not tiled:
        for by in range(bh):
            for bx in range(bw):
                yield bx, by
        return
    tw, th = (bw + 7) // 8, (bh + 7) // 8
    inv = [None] * 64
    for y in range(8):
        for x in range(8):
            inv[_morton(x, y)] = (x, y)
    for ty in range(th):
        for tx in range(tw):
            for m in range(64):
                x, y = inv[m]
                yield tx * 8 + x, ty * 8 + y


def decode(fmt, w, h, data, tiled=True):
    out = bytearray(w * h * 4)
    bw, bh = (w + 3) // 4, (h + 3) // 4
    bpb = {"BC1": 8, "BC2": 16, "BC3": 16}[fmt]
    # Pad to whole tiles in tiled mode; blocks outside the image are skipped.
    pos = 0
    pad_w = ((bw + 7) // 8 * 8) if tiled else bw
    pad_h = ((bh + 7) // 8 * 8) if tiled else bh
    for bx, by in block_order(pad_w, pad_h, tiled):
        blk = data[pos:pos + bpb]
        pos += bpb
        if bx >= bw or by >= bh or len(blk) < bpb:
            continue
        x0, y0 = bx * 4, by * 4
        if x0 + 4 > w or y0 + 4 > h:
            tmp = bytearray(64)
            _decode_block(fmt, blk, tmp, 4, 0, 0)
            for i in range(16):
                x, y = x0 + i % 4, y0 + i // 4
                if x < w and y < h:
                    out[(y * w + x) * 4:(y * w + x) * 4 + 4] = tmp[i * 4:i * 4 + 4]
        else:
            _decode_block(fmt, blk, out, w, x0, y0)
    return bytes(out)


def _decode_block(fmt, blk, out, stride, x0, y0):
    if fmt == "BC1":
        _color_block(blk, out, stride, x0, y0, force4=False)
    elif fmt == "BC3":
        _color_block(blk[8:], out, stride, x0, y0, alpha=_alpha_block(blk))
    elif fmt == "BC2":
        a = int.from_bytes(blk[:8], "little")
        _color_block(blk[8:], out, stride, x0, y0, alpha=[((a >> (4 * i)) & 15) * 17 for i in range(16)])


def resize(rgba, w, h, nw, nh):
    """Box-filter downscale (premultiplied alpha, so edges stay clean)."""
    out = bytearray(nw * nh * 4)
    for y in range(nh):
        sy0, sy1 = y * h // nh, max((y + 1) * h // nh, y * h // nh + 1)
        for x in range(nw):
            sx0, sx1 = x * w // nw, max((x + 1) * w // nw, x * w // nw + 1)
            r = g = b = a = n = 0
            for sy in range(sy0, sy1):
                o = (sy * w + sx0) * 4
                for _ in range(sx0, sx1):
                    pa = rgba[o + 3]
                    r += rgba[o] * pa; g += rgba[o + 1] * pa; b += rgba[o + 2] * pa; a += pa
                    n += 1; o += 4
            o = (y * nw + x) * 4
            if a:
                out[o:o + 4] = bytes((r // a, g // a, b // a, a // n))
    return bytes(out)


def write_png(path, w, h, rgba):
    raw = b"".join(b"\0" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)
