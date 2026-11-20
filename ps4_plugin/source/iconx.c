// On-console icon extraction. Formats (see tools/icon_extract/README.md):
//   IGA archive  v11 little-endian (Imaginators, SuperChargers), v10 big-endian (Swap Force);
//                files in 32 KiB blocks, raw deflate
//   IGZ object   v9 (strings 2-byte aligned) / v7 (packed); igImage2 + its pixel pool
//   textures     BC1/BC3 in 8x8-block tiles, Morton order inside (Imaginators);
//                32-bit RGBA, GCN 2D macro tiling (Swap Force, 192x192 icons)
// Everything is stored bottom-up.
#include "iconx.h"
#include "inflate.h"
#include "icon_map.h"
#include "tt_icons.h"
#include "menu.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <orbis/libkernel.h>

#define ICON_SIZE 96
#define N_NAMES ((int)(sizeof(k_icon_names) / sizeof(k_icon_names[0])))
#define N_MAP   ((int)(sizeof(k_icon_map) / sizeof(k_icon_map[0])))
// Version of the built-in tables: a pack written with other tables is redone by the same game.
#define MAP_VERSION (N_MAP * 1000 + (int)(sizeof(k_tt_icons) / sizeof(k_tt_icons[0])))

static volatile int g_progress = -1;
static volatile int g_changed;
static volatile int g_stop;

// ---------------------------------------------------------------------------
// Little helpers
// ---------------------------------------------------------------------------
static int rd_at(int fd, uint64_t off, void *buf, size_t n)
{
    if (lseek(fd, (off_t)off, SEEK_SET) < 0) return -1;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (uint8_t *)buf + got, n - got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}
static uint16_t u16le(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32le(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t u64le(const uint8_t *p) { return u32le(p) | (uint64_t)u32le(p + 4) << 32; }
static uint32_t u32be(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t u16be(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

// ---------------------------------------------------------------------------
// IGA archive
// ---------------------------------------------------------------------------
typedef struct {
    int       fd, be;
    uint32_t  ver, n, sector, nl, nm, ns;
    uint64_t *off;
    uint32_t *len, *bi;
    uint8_t  *tables;          // large (u32), medium (u16), small (u8), raw in file byte order
    uint8_t  *large, *medium, *small;
    char    **names;           // lower case, '/' separators
} iga;

static uint32_t A32(const iga *a, const uint8_t *p) { return a->be ? u32be(p) : u32le(p); }
static uint32_t A16(const iga *a, const uint8_t *p) { return a->be ? u16be(p) : u16le(p); }

static void iga_close(iga *a)
{
    if (a->fd >= 0) close(a->fd);
    if (a->names) for (uint32_t i = 0; i < a->n; i++) free(a->names[i]);
    free(a->names); free(a->off); free(a->len); free(a->bi); free(a->tables);
    memset(a, 0, sizeof(*a));
    a->fd = -1;
}

static int iga_open(iga *a, const char *path)
{
    memset(a, 0, sizeof(*a));
    a->fd = open(path, O_RDONLY);
    if (a->fd < 0) return -1;
    uint8_t h[0x38];
    if (rd_at(a->fd, 0, h, sizeof(h))) goto fail;
    if (!memcmp(h, "IGA\x1a", 4)) a->be = 0;
    else if (!memcmp(h, "\x1a" "AGI", 4)) a->be = 1;
    else goto fail;
    a->ver = A32(a, h + 4);
    a->n = A32(a, h + 0x0C);
    a->sector = A32(a, h + 0x10);
    a->nl = A32(a, h + 0x1C); a->nm = A32(a, h + 0x20); a->ns = A32(a, h + 0x24);
    uint64_t nto = a->be ? ((uint64_t)u32be(h + 0x28) << 32 | u32be(h + 0x2C)) : u64le(h + 0x28);
    uint32_t nts = A32(a, h + 0x30);
    if ((a->ver != 10 && a->ver != 11) || a->n == 0 || a->n > 200000) goto fail;

    size_t ftab = (size_t)a->n * 16, ttab = (size_t)a->nl * 4 + (size_t)a->nm * 2 + a->ns;
    uint8_t *toc = malloc(ftab + ttab);
    if (!toc || rd_at(a->fd, 0x38 + 4ull * a->n, toc, ftab + ttab)) { free(toc); goto fail; }
    a->off = malloc(8 * a->n); a->len = malloc(4 * a->n); a->bi = malloc(4 * a->n);
    if (!a->off || !a->len || !a->bi) { free(toc); goto fail; }
    for (uint32_t i = 0; i < a->n; i++) {
        const uint8_t *e = toc + 16 * i;
        if (a->ver == 11) {
            uint64_t q = a->be ? ((uint64_t)u32be(e) << 32 | u32be(e + 4)) : u64le(e);
            a->off[i] = q & 0xFFFFFFFFFFull;
            a->len[i] = A32(a, e + 8);
            a->bi[i] = A32(a, e + 12);
        } else {
            a->off[i] = A32(a, e + 4);
            a->len[i] = A32(a, e + 8);
            a->bi[i] = A32(a, e + 12);
        }
    }
    a->tables = malloc(ttab ? ttab : 1);
    if (!a->tables) { free(toc); goto fail; }
    memcpy(a->tables, toc + ftab, ttab);
    free(toc);
    a->large = a->tables;
    a->medium = a->large + 4 * a->nl;
    a->small = a->medium + 2 * a->nm;

    uint8_t *nt = malloc(nts + 1);
    if (!nt || rd_at(a->fd, nto, nt, nts)) { free(nt); goto fail; }
    nt[nts] = 0;
    a->names = calloc(a->n, sizeof(char *));
    if (!a->names) { free(nt); goto fail; }
    for (uint32_t i = 0; i < a->n; i++) {
        uint32_t s = A32(a, nt + 4 * i);
        if (s >= nts) { a->names[i] = strdup(""); continue; }
        const char *p = (const char *)nt + s;
        if (a->ver == 11) {                 // "build path\0logical path\0"
            size_t l = strlen(p);
            if (s + l + 1 < nts) p += l + 1;
        }
        char *c = strdup(p);
        for (char *q = c; q && *q; q++) *q = *q == '\\' ? '/' : (char)tolower((unsigned char)*q);
        a->names[i] = c;
    }
    free(nt);
    return 0;
fail:
    iga_close(a);
    return -1;
}

static int iga_find(const iga *a, const char *lower_name)
{
    for (uint32_t i = 0; i < a->n; i++)
        if (a->names[i] && !strcmp(a->names[i], lower_name)) return (int)i;
    return -1;
}

static uint32_t tbl_entry(const iga *a, int which, uint32_t k)
{
    if (which == 0) return a->small[k];
    if (which == 1) return A16(a, a->medium + 2 * k);
    return A32(a, a->large + 4 * k);
}

static int read_blocks(const iga *a, uint64_t off, uint32_t ln, int comp, int which, uint32_t idx, uint8_t *out)
{
    static uint8_t cbuf[0x8000 + 64];
    uint32_t flag = which == 0 ? 0x80u : which == 1 ? 0x8000u : 0x80000000u;
    uint32_t nblocks = (ln + 0x7FFF) / 0x8000, done = 0;
    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t v = tbl_entry(a, which, idx + b);
        uint32_t want = ln - done < 0x8000 ? ln - done : 0x8000;
        uint64_t p = off + (uint64_t)(v & (flag - 1)) * a->sector;
        if (v & flag) {
            uint8_t l2[2];
            if (rd_at(a->fd, p, l2, 2)) return -1;
            uint32_t clen = u16le(l2);
            if (clen > sizeof(cbuf) || rd_at(a->fd, p + 2, cbuf, clen)) return -1;
            if (comp != 1) return -1;
            if (inflate_raw(cbuf, clen, out + done, want) != (long)want) return -1;
        } else {
            if (rd_at(a->fd, p, out + done, want)) return -1;
        }
        done += want;
    }
    return 0;
}

static uint8_t *iga_read(const iga *a, int i, uint32_t *outlen)
{
    uint32_t ln = a->len[i], bi = a->bi[i];
    uint8_t *out = malloc(ln ? ln : 1);
    if (!out) return NULL;
    *outlen = ln;
    if (bi == 0xFFFFFFFFu) {
        if (rd_at(a->fd, a->off[i], out, ln)) { free(out); return NULL; }
        return out;
    }
    uint32_t nblocks = (ln + 0x7FFF) / 0x8000, idx = bi & 0x0FFFFFFF;
    int comp = (int)(bi >> 28);
    // Which table a file uses depends on its on-disk span: try the ones it fits in.
    uint32_t sizes[3] = { a->ns, a->nm, a->nl };
    for (int which = 0; which < 3; which++)
        if (idx + nblocks <= sizes[which] && read_blocks(a, a->off[i], ln, comp, which, idx, out) == 0)
            return out;
    free(out);
    return NULL;
}

// ---------------------------------------------------------------------------
// IGZ object files
// ---------------------------------------------------------------------------
typedef struct {
    const uint8_t *d;
    uint32_t n, ver;
    uint32_t soff[16], ssize[16];
    int nsec;
} igz;

static int igz_open(igz *z, const uint8_t *d, uint32_t n)
{
    memset(z, 0, sizeof(*z));
    if (n < 0x120 || memcmp(d, "\x01ZGI", 4)) return -1;
    z->d = d; z->n = n;
    z->ver = u32le(d + 4);
    for (int k = 0; k < 16; k++) {
        uint32_t off = u32le(d + 0x14 + 16 * k + 4), size = u32le(d + 0x14 + 16 * k + 8);
        if (!off) break;
        if ((uint64_t)off + size > n) return -1;
        z->soff[k] = off; z->ssize[k] = size; z->nsec = k + 1;
    }
    return z->nsec >= 2 ? 0 : -1;
}

// Fixup block body by magic; returns count, or -1.
static int igz_fixup(const igz *z, const char *magic, const uint8_t **body, uint32_t *blen)
{
    uint32_t p = z->soff[0], end = z->soff[0] + z->ssize[0];
    while (p + 16 <= end) {
        uint32_t cnt = u32le(z->d + p + 4), size = u32le(z->d + p + 8), start = u32le(z->d + p + 12);
        if (size < 16 || p + size > end) break;
        if (!memcmp(z->d + p, magic, 4)) {
            *body = z->d + p + start;
            *blen = size - start;
            return (int)cnt;
        }
        p += size;
    }
    return -1;
}

// k-th string of a string list (aligned: each entry padded to 2 bytes).
static const char *igz_str(const uint8_t *body, uint32_t blen, int k, int aligned)
{
    uint32_t p = 0;
    for (int i = 0; p < blen; i++) {
        const char *s = (const char *)body + p;
        size_t l = strnlen(s, blen - p);
        if (i == k) return s;
        p += (uint32_t)l + 1;
        if (aligned && (p & 1)) p++;
    }
    return NULL;
}

// The texture a material depends on: "textures:/X" -> "textures/x" (lower case).
static int igz_texture_dep(const igz *z, char *out, size_t outsz)
{
    const uint8_t *b; uint32_t bl;
    int cnt = igz_fixup(z, "TDEP", &b, &bl);
    for (int k = 0; k < cnt * 2; k++) {
        const char *s = igz_str(b, bl, k, 0);
        if (!s) break;
        if (!strncasecmp(s, "textures:/", 10)) {
            snprintf(out, outsz, "textures/%s", s + 10);
            for (char *q = out; *q; q++) *q = *q == '\\' ? '/' : (char)tolower((unsigned char)*q);
            return 0;
        }
    }
    return -1;
}

// igImage2 size and pixel data.
static int igz_image(const igz *z, int *w, int *h, const uint8_t **pix, uint32_t *plen)
{
    const uint8_t *b; uint32_t bl;
    int cnt = igz_fixup(z, "TMET", &b, &bl), t = -1;
    for (int k = 0; k < cnt; k++) {
        const char *s = igz_str(b, bl, k, z->ver >= 9);
        if (s && !strcmp(s, "igImage2")) { t = k; break; }
    }
    if (t < 0) return -1;
    for (int pi = 2; pi < z->nsec; pi++) {        // pools are sections 1..; the image object is not in the first
        const uint8_t *p = z->d + z->soff[pi];
        uint32_t sz = z->ssize[pi], dims;
        if (sz < 0x58) continue;
        if (u64le(p) == (uint64_t)t) dims = 0x48;
        else if (u64le(p + 0x10) == (uint64_t)t) dims = 0x50;
        else continue;
        *w = u16le(p + dims);
        *h = u16le(p + dims + 2);
        int best = -1;
        for (int k = 1; k < z->nsec; k++)
            if (k != pi && (best < 0 || z->ssize[k] > z->ssize[best])) best = k;
        if (best < 0 || *w <= 0 || *h <= 0) return -1;
        *pix = z->d + z->soff[best];
        *plen = z->ssize[best];
        return 0;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Texture decoding (to RGBA, top row first)
// ---------------------------------------------------------------------------
static int morton8(int x, int y)
{
    int m = 0;
    for (int b = 0; b < 3; b++) m |= ((x >> b) & 1) << (2 * b) | ((y >> b) & 1) << (2 * b + 1);
    return m;
}

static void rgb565(uint16_t c, uint8_t *o)
{
    int r = c >> 11 & 31, g = c >> 5 & 63, b = c & 31;
    o[0] = (uint8_t)(r << 3 | r >> 2); o[1] = (uint8_t)(g << 2 | g >> 4); o[2] = (uint8_t)(b << 3 | b >> 2);
}

// One 4x4 block into out (w x h), alpha from a BC3 alpha block if given.
static void bc_block(const uint8_t *cb, const uint8_t *ab, uint8_t *out, int w, int h, int x0, int y0, int force4)
{
    uint16_t c0 = u16le(cb), c1 = u16le(cb + 2);
    uint32_t bits = u32le(cb + 4);
    uint8_t pal[4][4];
    rgb565(c0, pal[0]); rgb565(c1, pal[1]);
    pal[0][3] = pal[1][3] = 255;
    if (c0 > c1 || force4) {
        for (int k = 0; k < 3; k++) {
            pal[2][k] = (uint8_t)((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = (uint8_t)((pal[0][k] + 2 * pal[1][k]) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int k = 0; k < 3; k++) { pal[2][k] = (uint8_t)((pal[0][k] + pal[1][k]) / 2); pal[3][k] = 0; }
        pal[2][3] = 255; pal[3][3] = 0;
    }
    uint8_t ap[8];
    uint64_t abits = 0;
    if (ab) {
        ap[0] = ab[0]; ap[1] = ab[1];
        if (ap[0] > ap[1]) for (int k = 0; k < 6; k++) ap[2 + k] = (uint8_t)(((6 - k) * ap[0] + (k + 1) * ap[1]) / 7);
        else { for (int k = 0; k < 4; k++) ap[2 + k] = (uint8_t)(((4 - k) * ap[0] + (k + 1) * ap[1]) / 5); ap[6] = 0; ap[7] = 255; }
        for (int k = 0; k < 6; k++) abits |= (uint64_t)ab[2 + k] << (8 * k);
    }
    for (int i = 0; i < 16; i++) {
        int x = x0 + (i & 3), y = y0 + (i >> 2);
        if (x >= w || y >= h) continue;
        uint8_t *o = out + ((size_t)y * w + x) * 4;
        const uint8_t *c = pal[bits >> (2 * i) & 3];
        o[0] = c[0]; o[1] = c[1]; o[2] = c[2];
        o[3] = ab ? ap[abits >> (3 * i) & 7] : c[3];
    }
}

static uint8_t *decode_texture(int w, int h, const uint8_t *pix, uint32_t plen, int tail)
{
    uint8_t *out = calloc((size_t)w * h, 4);
    if (!out) return NULL;
    if ((uint64_t)plen >= (uint64_t)w * h * 4) {
        // 32-bit RGBA with GCN 2D macro tiling (Swap Force). Layout solved for the 192x192
        // collection icons (pitch 256): chunk = 8x8 pixels (Morton inside), chunk index bits are
        // XORs of the tile coordinates (pipe = x3^x4^y3, x4^y4, x5^y5, then bank/row bits).
        static const uint16_t mask[10] = { 0x023, 0x042, 0x084, 0x108, 0x190, 0x040, 0x020, 0x010, 0x100, 0x200 };
        if (w != 192 || h != 192 || plen < 256u * 192 * 4) { free(out); return NULL; }
        const uint8_t *base = pix + plen - 256u * 192 * 4;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                unsigned v = (unsigned)(x >> 3) | (unsigned)(y >> 3) << 5, c = 0;
                for (int j = 0; j < 10; j++) c |= (unsigned)__builtin_parity(mask[j] & v) << j;
                const uint8_t *p = base + c * 256 + morton8(x & 7, y & 7) * 4;
                uint8_t *o = out + ((size_t)y * w + x) * 4;
                o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = p[3];
            }
        return out;
    }
    // BC1 / BC3: 4x4 blocks stored as 8x8-block tiles, Morton order inside a tile.
    int bw = (w + 3) / 4, bh = (h + 3) / 4, tw = (bw + 7) / 8, th = (bh + 7) / 8;
    int bc1 = (double)plen / ((double)w * h) < 0.75;
    int bpb = bc1 ? 8 : 16;
    uint64_t need = (uint64_t)tw * th * 64 * bpb;
    if (need > plen) { free(out); return NULL; }
    if (tail) pix += plen - need;        // Trap Team: the blocks are the end of the pool
    int inv[64][2];
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) { inv[morton8(x, y)][0] = x; inv[morton8(x, y)][1] = y; }
    const uint8_t *p = pix;
    for (int ty = 0; ty < th; ty++)
        for (int tx = 0; tx < tw; tx++)
            for (int m = 0; m < 64; m++, p += bpb) {
                int bx = tx * 8 + inv[m][0], by = ty * 8 + inv[m][1];
                if (bx >= bw || by >= bh) continue;
                if (bc1) bc_block(p, NULL, out, w, h, bx * 4, by * 4, 0);
                else bc_block(p + 8, p, out, w, h, bx * 4, by * 4, 1);
            }
    return out;
}

// Box-filter downscale with premultiplied alpha, flipping the image upright on the way.
static void resize_flip(const uint8_t *src, int w, int h, uint8_t *dst, int n)
{
    for (int y = 0; y < n; y++) {
        int sy0 = y * h / n, sy1 = (y + 1) * h / n;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < n; x++) {
            int sx0 = x * w / n, sx1 = (x + 1) * w / n;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            uint32_t r = 0, g = 0, b = 0, a = 0, cnt = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                const uint8_t *s = src + ((size_t)(h - 1 - sy) * w + sx0) * 4;
                for (int sx = sx0; sx < sx1; sx++, s += 4) {
                    r += s[0] * s[3]; g += s[1] * s[3]; b += s[2] * s[3]; a += s[3]; cnt++;
                }
            }
            uint8_t *o = dst + ((size_t)y * n + x) * 4;
            if (a) { o[0] = (uint8_t)(r / a); o[1] = (uint8_t)(g / a); o[2] = (uint8_t)(b / a); o[3] = (uint8_t)(a / cnt); }
            else o[0] = o[1] = o[2] = o[3] = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// Extraction
// ---------------------------------------------------------------------------
static int write_all(const char *path, const void *d, size_t n)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0777);
    if (fd < 0) return -1;
    ssize_t r = write(fd, d, n);
    close(fd);
    return r == (ssize_t)n ? 0 : -1;
}

// One icon: material -> texture -> RGBA file. Returns 0 if written.
static int extract_one(const iga *a, const char *name, const char *out_dir, char *why, size_t whysz)
{
    char path[256], tex[256];
    igz z;
    uint32_t tl = 0;
    int ti = -1;
    // Toys handled in game code have no material: their icon texture is looked up directly
    // (tools/icon_extract/extract_icons.py EXTRA_TOYS).
    static const struct { const char *name, *suffix; } extra[] = {
        { "Extra_Collections_Chest",       "toyinventoryicons!magicitems!collections_chest`tga,101.igz" },
        { "Extra_Collections_Observatory", "toyinventoryicons!magicitems!2016expansiontoys!collections_observatory`tga,101.igz" },
        { "Extra_Collections_TreeHouse",   "toyinventoryicons!magicitems!2016expansiontoys!collections_treehouse`tga,101.igz" },
    };
    for (size_t k = 0; k < sizeof(extra) / sizeof(extra[0]); k++) {
        if (strcmp(name, extra[k].name)) continue;
        size_t sl = strlen(extra[k].suffix);
        for (uint32_t i = 0; i < a->n && ti < 0; i++) {
            size_t nl = strlen(a->names[i]);
            if (nl >= sl && !strcmp(a->names[i] + nl - sl, extra[k].suffix)) ti = (int)i;
        }
        if (ti < 0) { snprintf(why, whysz, "no material"); return -1; }
        snprintf(tex, sizeof(tex), "%s", a->names[ti]);
    }
    if (ti < 0) {
        snprintf(path, sizeof(path), "materialinstances/toycollection/%s.igz", name);
        for (char *q = path; *q; q++) *q = (char)tolower((unsigned char)*q);
        int mi = iga_find(a, path);
        if (mi < 0) { snprintf(why, whysz, "no material"); return -1; }
        uint32_t ml = 0;
        uint8_t *md = iga_read(a, mi, &ml);
        if (!md || igz_open(&z, md, ml) || igz_texture_dep(&z, tex, sizeof(tex))) {
            free(md); snprintf(why, whysz, "bad material"); return -1;
        }
        free(md);
        ti = iga_find(a, tex);
    }
    uint8_t *td = ti >= 0 ? iga_read(a, ti, &tl) : NULL;
    int w = 0, h = 0; const uint8_t *pix; uint32_t plen;
    if (!td || igz_open(&z, td, tl) || igz_image(&z, &w, &h, &pix, &plen)) {
        free(td); snprintf(why, whysz, "bad texture %s", tex); return -1;
    }
    uint8_t *rgba = decode_texture(w, h, pix, plen, 0);
    free(td);
    if (!rgba) { snprintf(why, whysz, "unsupported texture %dx%d (%u bytes)", w, h, plen); return -1; }
    static uint8_t small[ICON_SIZE * ICON_SIZE * 4];
    resize_flip(rgba, w, h, small, ICON_SIZE);
    free(rgba);
    snprintf(path, sizeof(path), "%s/%s.rgba", out_dir, name);
    if (write_all(path, small, sizeof(small))) { snprintf(why, whysz, "cannot write %s", path); return -1; }
    return 0;
}

// Trap Team: igImage2 sits inside the first pool (not at a pool start); width/height at +0x40.
static int igz_image_any(const igz *z, int *w, int *h, const uint8_t **pix, uint32_t *plen)
{
    if (igz_image(z, w, h, pix, plen) == 0) return 0;
    const uint8_t *b; uint32_t bl;
    int cnt = igz_fixup(z, "TMET", &b, &bl), t = -1;
    for (int k = 0; k < cnt; k++) {
        const char *s = igz_str(b, bl, k, z->ver >= 8);
        if (s && !strcmp(s, "igImage2")) { t = k; break; }
    }
    if (t < 0) return -1;
    for (int pi = 1; pi < z->nsec; pi++) {
        const uint8_t *p = z->d + z->soff[pi];
        for (uint32_t o = 0; o + 0x48 <= z->ssize[pi]; o += 8) {
            if (u64le(p + o) != (uint64_t)t) continue;
            int ww = u16le(p + o + 0x40), hh = u16le(p + o + 0x42);
            if (ww < 8 || hh < 8 || ww > 4096 || hh > 4096) continue;
            int best = -1;
            for (int k = 1; k < z->nsec; k++)
                if (k != pi && (best < 0 || z->ssize[k] > z->ssize[best])) best = k;
            if (best < 0) return -1;
            *w = ww; *h = hh;
            *pix = z->d + z->soff[best];
            *plen = z->ssize[best];
            return 0;
        }
    }
    return -1;
}

static int write_icon(const char *out_dir, const char *name, const uint8_t *rgba, int w, int h)
{
    static uint8_t small[ICON_SIZE * ICON_SIZE * 4];
    char path[256];
    resize_flip(rgba, w, h, small, ICON_SIZE);
    snprintf(path, sizeof(path), "%s/%s.rgba", out_dir, name);
    return write_all(path, small, sizeof(small));
}

static int write_index(const char *out_dir, const char *txt, size_t len)
{
    char tmp[256], idx[256];
    snprintf(tmp, sizeof(tmp), "%s/index.tmp", out_dir);
    snprintf(idx, sizeof(idx), "%s/index.txt", out_dir);
    int r = write_all(tmp, txt, len);
    if (r == 0) { unlink(idx); r = rename(tmp, idx); }
    return r;
}

// Trap Team: pictures by hash from misc/ui_collection_*.arc (table from make_tt_map.py).
static int extract_tt(const char *misc_dir, const char *out_dir, const char *title_id, int year)
{
    iga arcs[2];
    char path[256];
    int n = (int)(sizeof(k_tt_icons) / sizeof(k_tt_icons[0])), ok = 0, logged = 0;
    for (int k = 0; k < 2; k++) {
        snprintf(path, sizeof(path), "%s/%s", misc_dir, k_tt_archives[k]);
        if (iga_open(&arcs[k], path)) { LOG("icons: cannot open %s", path); if (k) iga_close(&arcs[0]); return -1; }
    }
    LOG("icons: reading Trap Team pictures from %s", misc_dir);
    mkdir(out_dir, 0777);
    static uint8_t have[1024];
    memset(have, 0, sizeof(have));
    uint64_t t0 = sceKernelGetProcessTime();
    for (int i = 0; i < n && i < 1024 && !g_stop; i++) {
        g_progress = i * 100 / n;
        char hname[16], name[32];
        snprintf(hname, sizeof(hname), "0x%08x", k_tt_icons[i].hash);
        const iga *a = &arcs[k_tt_icons[i].archive];
        int fi = iga_find(a, hname);
        uint32_t len = 0;
        uint8_t *d = fi >= 0 ? iga_read(a, fi, &len) : NULL;
        igz z;
        int w = 0, h = 0; const uint8_t *pix; uint32_t plen;
        uint8_t *rgba = NULL;
        if (d && !igz_open(&z, d, len) && !igz_image_any(&z, &w, &h, &pix, &plen))
            rgba = decode_texture(w, h, pix, plen, 1);
        free(d);
        snprintf(name, sizeof(name), "TT_%u_%u", k_tt_icons[i].id, k_tt_icons[i].variant);
        if (rgba && write_icon(out_dir, name, rgba, w, h) == 0) { have[i] = 1; ok++; }
        else if (logged++ < 8) LOG("icons: %s (%s): %s", name, hname, fi < 0 ? "not in archive" : "cannot decode");
        free(rgba);
        usleep(500);
    }
    iga_close(&arcs[0]); iga_close(&arcs[1]);
    if (g_stop || !ok) return -1;

    // index: every picture by exact variant, plus a default per figure (its plain variant:
    // no deco/flags in the low 10 bits, else the first one listed).
    size_t cap = 64 + (size_t)n * 2 * 48, tl = 0;
    char *txt = malloc(cap);
    if (!txt) return -1;
    tl += (size_t)snprintf(txt + tl, cap - tl, "size %d\nsource %s %d %d\n", ICON_SIZE, title_id, year, MAP_VERSION);
    int rows = 0;
    for (int i = 0; i < n; i++) {
        if (!have[i]) continue;
        unsigned id = k_tt_icons[i].id, var = k_tt_icons[i].variant;
        int def = -1;
        for (int j = 0; j < n; j++)
            if (have[j] && k_tt_icons[j].id == id && (def < 0 || ((k_tt_icons[j].variant & 0x3FF) == 0 &&
                                                                  (k_tt_icons[def].variant & 0x3FF) != 0)))
                def = j;
        // Swap Force bottoms (1000-1015) have no picture of their own: they use the top's.
        for (int half = 0; half < (id >= 2000 && id < 2999 ? 2 : 1); half++) {
            unsigned as = half ? id - 1000 : id;
            tl += (size_t)snprintf(txt + tl, cap - tl, "%u %u TT_%u_%u\n", as, var, id, var);
            if (def == i) tl += (size_t)snprintf(txt + tl, cap - tl, "%u -1 TT_%u_%u\n", as, id, var);
            rows++;
        }
    }
    int r = write_index(out_dir, txt, tl);
    free(txt);
    LOG("icons: %d of %d Trap Team pictures, %d figure entries (%llu ms)", ok, n, rows,
        (unsigned long long)((sceKernelGetProcessTime() - t0) / 1000));
    return r;
}

static const char *const k_archives[] = { "toycollectionmaterials.pak", "toycollectionimages.pak" };

// game_dir: the game's root (/app0) or, for tests, a folder with the archives themselves.
int iconx_extract_now(const char *game_dir, const char *out_dir, const char *title_id, int year)
{
    iga a;
    char path[256];
    int opened = 0;
    static const char *const sub[] = { "archives/", "" };
    for (size_t s = 0; s < 2 && !opened; s++)
        for (size_t k = 0; k < sizeof(k_archives) / sizeof(k_archives[0]) && !opened; k++) {
            snprintf(path, sizeof(path), "%s/%s%s", game_dir, sub[s], k_archives[k]);
            opened = iga_open(&a, path) == 0;
        }
    if (!opened) {
        // Trap Team keeps its pictures in misc/*.arc
        for (size_t s = 0; s < 2; s++) {
            snprintf(path, sizeof(path), "%s/%s%s", game_dir, s ? "" : "misc/", k_tt_archives[0]);
            int fd = open(path, O_RDONLY);
            if (fd >= 0) {
                close(fd);
                snprintf(path, sizeof(path), "%s%s", game_dir, s ? "" : "/misc");
                return extract_tt(path, out_dir, title_id, year);
            }
        }
        LOG("icons: no icon archive in %s", game_dir);
        return -1;
    }
    LOG("icons: reading %s (IGA v%u, %u files)", path, a.ver, a.n);
    mkdir(out_dir, 0777);
    static uint8_t have[N_NAMES];
    memset(have, 0, sizeof(have));
    int ok = 0, logged = 0;
    uint64_t t0 = sceKernelGetProcessTime();
    for (int i = 0; i < N_NAMES && !g_stop; i++) {
        g_progress = i * 100 / N_NAMES;
        char why[160];
        if (extract_one(&a, k_icon_names[i], out_dir, why, sizeof(why)) == 0) { have[i] = 1; ok++; }
        else if (strcmp(why, "no material") && logged++ < 8) LOG("icons: %s: %s", k_icon_names[i], why);
        usleep(500);                    // stay gentle while the game runs
    }
    iga_close(&a);
    if (g_stop) return -1;
    if (!ok) { LOG("icons: no icon found in this game's archive"); return -1; }

    // index.txt: written to a temp file first, then renamed over the old one.
    size_t cap = 64 + (size_t)N_MAP * 64, len = 0;
    char *txt = malloc(cap);
    if (!txt) return -1;
    len += (size_t)snprintf(txt + len, cap - len, "size %d\nsource %s %d %d\n", ICON_SIZE, title_id, year, MAP_VERSION);
    int rows = 0;
    for (int i = 0; i < N_MAP; i++)
        if (have[k_icon_map[i].name]) {
            len += (size_t)snprintf(txt + len, cap - len, "%u %d %s\n", k_icon_map[i].id, k_icon_map[i].variant,
                                    k_icon_names[k_icon_map[i].name]);
            rows++;
        }
    char tmp[256], idx[256];
    snprintf(tmp, sizeof(tmp), "%s/index.tmp", out_dir);
    snprintf(idx, sizeof(idx), "%s/index.txt", out_dir);
    int r = write_all(tmp, txt, len);
    free(txt);
    if (r == 0) { unlink(idx); r = rename(tmp, idx); }
    LOG("icons: %d of %d icons from %s, %d figure entries (%llu ms)", ok, N_NAMES, title_id, rows,
        (unsigned long long)((sceKernelGetProcessTime() - t0) / 1000));
    return r;
}

// Year code of the pack in dir: from its "source" line; a pack without one is the
// Imaginators pack made on the PC (year 5). -1 if there is no pack.
static int pack_year(const char *dir, int *map_version)
{
    *map_version = 0;
    char path[256];
    snprintf(path, sizeof(path), "%s/index.txt", dir);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    const char *s = strstr(buf, "source ");
    if (!s) return 5;
    char title[32]; int year = -1;
    int got = sscanf(s, "source %31s %d %d", title, &year, map_version);
    if (got < 2) return 5;
    return year;
}

static char g_title[16];
static int  g_year;

static void *iconx_thread(void *arg)
{
    (void)arg;
    for (int i = 0; i < 150 && !g_stop; i++) usleep(100000);   // let the game finish loading first
    if (g_stop) return NULL;
    g_progress = 0;
    if (iconx_extract_now("/app0", GP_ICON_DIR, g_title, g_year) == 0) g_changed = 1;
    g_progress = -1;
    return NULL;
}

void iconx_start(const char *title_id, int year)
{
    int mapv = 0, have = pack_year(GP_ICON_DIR, &mapv);
    if (have > year || (have == year && mapv == MAP_VERSION)) {
        LOG("icons: keeping the pack from year code %d (this game: %d)", have, year);
        return;
    }
    LOG("icons: %s - reading the icons from this game (year code %d)",
        have < 0 ? "no icon pack yet" : have == year ? "pack was made with older tables" : "pack is from an older game",
        year);
    snprintf(g_title, sizeof(g_title), "%s", title_id);
    g_year = year;
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    if (pthread_create(&th, &attr, iconx_thread, NULL) == 0) pthread_detach(th);
    pthread_attr_destroy(&attr);
}

int iconx_progress(void) { return g_progress; }

int iconx_take_changed(void)
{
    if (!g_changed) return 0;
    g_changed = 0;
    return 1;
}

void iconx_stop(void) { g_stop = 1; }
