#include "gfx.h"
#include "font_data.h"
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

int cv_init(canvas *c, int w, int h)
{
    // Prefer pages straight from the system: the game's malloc heap is small and
    // the canvas is several MB.
    size_t size = ((size_t)w * h * 4 + 0x3FFF) & ~(size_t)0x3FFF;
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    c->mapped = p != MAP_FAILED && p != NULL;
    c->px = c->mapped ? p : malloc((size_t)w * h * 4);
    if (!c->px) return -1;
    c->w = w;
    c->h = h;
    cv_clear(c);
    return 0;
}

void cv_clear(canvas *c)
{
    memset(c->px, 0, (size_t)c->w * c->h * 4);
}

// "Source over destination" with straight alpha.
static inline uint32_t over(uint32_t dst, uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    if (a == 0) return dst;
    uint32_t da = dst >> 24;
    if (a == 255 || da == 0) return a << 24 | r << 16 | g << 8 | b;
    uint32_t dw = da * (255 - a) / 255;              // destination weight
    uint32_t oa = a + dw;
    uint32_t dr = dst >> 16 & 0xFF, dg = dst >> 8 & 0xFF, db = dst & 0xFF;
    dr = (r * a + dr * dw) / oa;
    dg = (g * a + dg * dw) / oa;
    db = (b * a + db * dw) / oa;
    return oa << 24 | dr << 16 | dg << 8 | db;
}

static inline uint32_t over_c(uint32_t dst, uint32_t color, uint32_t cov /* 0..255 */)
{
    uint32_t a = (color >> 24) * cov / 255;
    return over(dst, color >> 16 & 0xFF, color >> 8 & 0xFF, color & 0xFF, a);
}

void cv_fill(canvas *c, int x, int y, int w, int h, uint32_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > c->w) w = c->w - x;
    if (y + h > c->h) h = c->h - y;
    for (int j = 0; j < h; j++) {
        uint32_t *row = c->px + (size_t)(y + j) * c->w + x;
        if (color >> 24 == 255) {
            for (int i = 0; i < w; i++) row[i] = color;
        } else {
            for (int i = 0; i < w; i++) row[i] = over_c(row[i], color, 255);
        }
    }
}

void cv_frame(canvas *c, int x, int y, int w, int h, int t, uint32_t color)
{
    cv_fill(c, x, y, w, t, color);
    cv_fill(c, x, y + h - t, w, t, color);
    cv_fill(c, x, y + t, t, h - 2 * t, color);
    cv_fill(c, x + w - t, y + t, t, h - 2 * t, color);
}

// How much of pixel (px, py) lies inside the rounded rectangle, in 16ths (4x4 samples).
static int rr_cover(int px, int py, int x, int y, int w, int h, int r)
{
    if (w <= 0 || h <= 0) return 0;
    if (px < x || py < y || px >= x + w || py >= y + h) return 0;
    if (r <= 0) return 16;
    // Corner square this pixel is in, if any, and that corner's circle centre.
    int cx, cy;
    if (px < x + r) cx = x + r; else if (px >= x + w - r) cx = x + w - r; else return 16;
    if (py < y + r) cy = y + r; else if (py >= y + h - r) cy = y + h - r; else return 16;
    // 4x4 sample centres, everything in 1/8 pixel units.
    int n = 0, rr = (r * 8) * (r * 8);
    for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++) {
            int dx = px * 8 + sx * 2 + 1 - cx * 8, dy = py * 8 + sy * 2 + 1 - cy * 8;
            if (dx * dx + dy * dy <= rr) n++;
        }
    return n;
}

void cv_round(canvas *c, int x, int y, int w, int h, int r, int t, uint32_t color)
{
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < 0 || py >= c->h) continue;
        uint32_t *row = c->px + (size_t)py * c->w;
        for (int i = 0; i < w; i++) {
            int px = x + i;
            if (px < 0 || px >= c->w) continue;
            // Fast path: the straight middle of a filled shape.
            int cov;
            if (!t && i >= r && i < w - r) cov = 16;
            else if (!t && j >= r && j < h - r) cov = 16;
            else {
                cov = rr_cover(px, py, x, y, w, h, r);
                if (t && cov) cov -= rr_cover(px, py, x + t, y + t, w - 2 * t, h - 2 * t, r > t ? r - t : 0);
            }
            if (cov > 0) row[px] = over_c(row[px], color, (uint32_t)cov * 255 / 16);
        }
    }
}

void cv_image(canvas *c, int x, int y, int dw, int dh, const uint8_t *rgba, int sw, int sh)
{
    if (!rgba) return;
    for (int j = 0; j < dh; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= c->h) continue;
        const uint8_t *srow = rgba + (size_t)(j * sh / dh) * sw * 4;
        uint32_t *drow = c->px + (size_t)yy * c->w;
        for (int i = 0; i < dw; i++) {
            int xx = x + i;
            if (xx < 0 || xx >= c->w) continue;
            const uint8_t *p = srow + (i * sw / dw) * 4;
            drow[xx] = over(drow[xx], p[0], p[1], p[2], p[3]);
        }
    }
}

// --- text ---

// Next code point from UTF-8 (invalid bytes are taken as Latin-1). Advances *s.
static unsigned next_cp(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    unsigned c = p[0];
    if (c >= 0xC2 && c <= 0xDF && (p[1] & 0xC0) == 0x80) {
        *s += 2;
        return (c & 0x1F) << 6 | (p[1] & 0x3F);
    }
    if (c >= 0xE0 && c <= 0xEF && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *s += 3;
        return (c & 0x0F) << 12 | (p[1] & 0x3F) << 6 | (p[2] & 0x3F);
    }
    *s += 1;
    return c;
}

static int glyph_index(unsigned cp)
{
    if (cp < FONT_FIRST || cp > FONT_LAST) cp = '?';
    return (int)cp - FONT_FIRST;
}

static int glyph(canvas *c, int x, int y, int gi, uint32_t color)
{
    int w = font_width[gi];
    const uint8_t *a = font_alpha + font_offset[gi];
    for (int j = 0; j < FONT_HEIGHT; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= c->h) continue;
        uint32_t *row = c->px + (size_t)yy * c->w;
        for (int i = 0; i < w; i++) {
            int xx = x + i;
            if (xx >= 0 && xx < c->w && a[j * w + i]) row[xx] = over_c(row[xx], color, a[j * w + i]);
        }
    }
    return w;
}

int text_width(const char *s)
{
    int w = 0;
    while (*s) w += font_width[glyph_index(next_cp(&s))];
    return w;
}

int text_height(void) { return FONT_HEIGHT; }

int cv_text(canvas *c, int x, int y, const char *s, uint32_t color, int maxw)
{
    int x0 = x;
    int dots = 3 * font_width[glyph_index('.')];
    int clip = maxw > 0 && text_width(s) > maxw;
    while (*s) {
        int gi = glyph_index(next_cp(&s));
        if (clip && x - x0 + font_width[gi] > maxw - dots) {
            for (int k = 0; k < 3; k++) x += glyph(c, x, y, glyph_index('.'), color);
            break;
        }
        x += glyph(c, x, y, gi, color);
    }
    return x - x0;
}

// --- frame buffer output ---

const char *fb_format_name(uint32_t f)
{
    switch (f) {
    case 0x80000000: return "A8R8G8B8_SRGB";
    case 0x80002200: return "A8B8G8R8_SRGB";
    case 0x88060000: return "A2R10G10B10";
    case 0x88000000: return "A2R10G10B10_SRGB";
    case 0x88740000: return "A2R10G10B10_BT2020_PQ";
    case 0xC1060000: return "A16R16G16B16_FLOAT";
    default:         return "unknown";
    }
}

// Converts 0x..RRGGBB to the buffer's 32-bit pixel.
static inline uint32_t pack(uint32_t f, uint32_t c)
{
    uint32_t r = c >> 16 & 0xFF, g = c >> 8 & 0xFF, b = c & 0xFF;
    switch (f) {
    case 0x80002200:                                   // memory R,G,B,A
        return 0xFF000000u | b << 16 | g << 8 | r;
    case 0x88060000: case 0x88000000: case 0x88740000: // 2:10:10:10, R in the high bits
        return 3u << 30 | (r * 1023 / 255) << 20 | (g * 1023 / 255) << 10 | (b * 1023 / 255);
    default:                                           // A8R8G8B8: memory B,G,R,A
        return 0xFF000000u | (c & 0xFFFFFF);
    }
}

// Byte offset of pixel (x, y). TILE mode is not decoded yet: this is the "8x8 micro tiles
// in Morton order, tiles row by row" layout, which is only a first guess (see /api/fb).
static inline size_t px_offset(const framebuffer *fb, int x, int y)
{
    if (!fb->tiled)
        return ((size_t)y * fb->pitch + x) * 4;
    unsigned lx = x & 7, ly = y & 7, m = 0;
    for (int b = 0; b < 3; b++)
        m |= ((lx >> b) & 1) << (2 * b) | ((ly >> b) & 1) << (2 * b + 1);
    size_t tile = (size_t)(y >> 3) * (fb->pitch >> 3) + (x >> 3);
    return (tile * 64 + m) * 4;
}

void fb_present(const framebuffer *fb, const canvas *c, int x, int y, int scale)
{
    if (!fb->base || scale < 1) return;
    uint32_t f = fb->format;
    for (int j = 0; j < c->h * scale; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= fb->height) continue;
        const uint32_t *src = c->px + (size_t)(j / scale) * c->w;
        // Fast path (the usual case): copy runs of opaque pixels straight into the buffer.
        if (!fb->tiled && scale == 1 && f == 0x80000000 && x >= 0 && x + c->w <= fb->width) {
            uint32_t *dst = (uint32_t *)(fb->base + ((size_t)yy * fb->pitch + x) * 4);
            for (int i = 0; i < c->w;) {
                if (src[i] >> 24 < 128) { i++; continue; }
                int k = i;
                while (k < c->w && src[k] >> 24 >= 128) k++;
                memcpy(dst + i, src + i, (size_t)(k - i) * 4);   // alpha byte is ignored by scan-out
                i = k;
            }
            continue;
        }
        for (int i = 0; i < c->w * scale; i++) {
            uint32_t s = src[i / scale], a = s >> 24;
            if (a == 0) continue;
            int xx = x + i;
            if (xx < 0 || xx >= fb->width) continue;
            if (a >= 128) *(volatile uint32_t *)(fb->base + px_offset(fb, xx, yy)) = pack(f, s);
        }
    }
}
