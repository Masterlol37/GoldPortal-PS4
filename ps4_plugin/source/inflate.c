#include "inflate.h"
#include <string.h>

typedef struct {
    const uint8_t *src;
    size_t len, pos;
    uint32_t bitbuf;
    int bitcnt;
    int err;
} bits;

static int getbit(bits *b)
{
    if (!b->bitcnt) {
        if (b->pos >= b->len) { b->err = 1; return 0; }
        b->bitbuf = b->src[b->pos++];
        b->bitcnt = 8;
    }
    int v = b->bitbuf & 1;
    b->bitbuf >>= 1;
    b->bitcnt--;
    return v;
}

static uint32_t getbits(bits *b, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v |= (uint32_t)getbit(b) << i;
    return v;
}

// Canonical Huffman table: counts per length and symbols sorted by code.
typedef struct { uint16_t count[16]; uint16_t sym[320]; } huff;

static int build(huff *h, const uint8_t *lens, int n)
{
    uint16_t offs[16];
    memset(h->count, 0, sizeof(h->count));
    for (int i = 0; i < n; i++) h->count[lens[i]]++;
    h->count[0] = 0;
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = offs[i] + h->count[i];
    for (int i = 0; i < n; i++)
        if (lens[i]) h->sym[offs[lens[i]]++] = (uint16_t)i;
    return 0;
}

static int decode(bits *b, const huff *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= getbit(b);
        int count = h->count[len];
        if (code - count < first) return h->sym[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
        if (b->err) return -1;
    }
    return -1;
}

static const uint16_t k_lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                                      67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t k_lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t k_dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                      1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t k_dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int codes(bits *b, uint8_t *dst, size_t dstlen, size_t *out, const huff *lit, const huff *dist)
{
    for (;;) {
        int s = decode(b, lit);
        if (s < 0 || b->err) return -1;
        if (s < 256) {
            if (*out >= dstlen) return -1;
            dst[(*out)++] = (uint8_t)s;
        } else if (s == 256) {
            return 0;
        } else {
            s -= 257;
            if (s >= 29) return -1;
            size_t len = k_lbase[s] + getbits(b, k_lext[s]);
            int ds = decode(b, dist);
            if (ds < 0 || ds >= 30) return -1;
            size_t d = k_dbase[ds] + getbits(b, k_dext[ds]);
            if (d > *out || *out + len > dstlen || b->err) return -1;
            for (size_t i = 0; i < len; i++, (*out)++) dst[*out] = dst[*out - d];
        }
    }
}

long inflate_raw(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen)
{
    bits b = { src, srclen, 0, 0, 0, 0 };
    size_t out = 0;
    static huff lit, dist;            // only used from one thread at a time
    int last;
    do {
        last = getbit(&b);
        int type = (int)getbits(&b, 2);
        if (b.err) return -1;
        if (type == 0) {                                  // stored
            b.bitcnt = 0;
            if (b.pos + 4 > b.len) return -1;
            unsigned len = b.src[b.pos] | b.src[b.pos + 1] << 8;
            b.pos += 4;
            if (b.pos + len > b.len || out + len > dstlen) return -1;
            memcpy(dst + out, b.src + b.pos, len);
            b.pos += len;
            out += len;
        } else if (type == 1) {                           // fixed Huffman
            uint8_t l[288];
            int i = 0;
            for (; i < 144; i++) l[i] = 8;
            for (; i < 256; i++) l[i] = 9;
            for (; i < 280; i++) l[i] = 7;
            for (; i < 288; i++) l[i] = 8;
            build(&lit, l, 288);
            for (i = 0; i < 30; i++) l[i] = 5;
            build(&dist, l, 30);
            if (codes(&b, dst, dstlen, &out, &lit, &dist)) return -1;
        } else if (type == 2) {                           // dynamic Huffman
            static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
            int nlen = (int)getbits(&b, 5) + 257, ndist = (int)getbits(&b, 5) + 1, ncode = (int)getbits(&b, 4) + 4;
            if (nlen > 286 || ndist > 30) return -1;
            uint8_t l[320];
            memset(l, 0, sizeof(l));
            for (int i = 0; i < ncode; i++) l[order[i]] = (uint8_t)getbits(&b, 3);
            huff lc;
            build(&lc, l, 19);
            int i = 0;
            while (i < nlen + ndist) {
                int s = decode(&b, &lc);
                if (s < 0 || b.err) return -1;
                if (s < 16) { l[i++] = (uint8_t)s; continue; }
                int rep = 0, val = 0;
                if (s == 16) { if (!i) return -1; val = l[i - 1]; rep = 3 + (int)getbits(&b, 2); }
                else if (s == 17) rep = 3 + (int)getbits(&b, 3);
                else rep = 11 + (int)getbits(&b, 7);
                if (i + rep > nlen + ndist) return -1;
                while (rep--) l[i++] = (uint8_t)val;
            }
            build(&lit, l, nlen);
            build(&dist, l + nlen, ndist);
            if (codes(&b, dst, dstlen, &out, &lit, &dist)) return -1;
        } else {
            return -1;
        }
    } while (!last);
    return (long)out;
}
