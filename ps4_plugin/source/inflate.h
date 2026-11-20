// Small raw-deflate decoder (RFC 1951), enough for the game archives' 32 KiB blocks.
#pragma once
#include <stddef.h>
#include <stdint.h>

// Decodes src into dst. Returns the number of bytes written, or -1 on bad data / overflow.
long inflate_raw(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen);
