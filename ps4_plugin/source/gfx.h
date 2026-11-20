// Software drawing for the on-screen menu.
//
// The menu is drawn into an off-screen canvas (0xAARRGGBB, straight alpha; alpha 0 = not part of
// the menu) only when it changes, then put into the game's front buffer after every flip (fb_present).
#pragma once
#include <stdint.h>

typedef struct {
    int       w, h;
    uint32_t *px;          // w*h pixels, 0xAARRGGBB
    int       mapped;      // 1: system pages (mmap), 0: malloc
} canvas;

int  cv_init(canvas *c, int w, int h);
void cv_clear(canvas *c);                                       // everything transparent
// Draws over what is there (alpha compositing). An opaque color simply replaces it.
void cv_fill(canvas *c, int x, int y, int w, int h, uint32_t color);
void cv_frame(canvas *c, int x, int y, int w, int h, int t, uint32_t color);
// Rounded rectangle with radius r, anti-aliased. t = 0: filled, else an outline t pixels wide.
void cv_round(canvas *c, int x, int y, int w, int h, int r, int t, uint32_t color);
// Draws an RGBA image (straight alpha, top row first) scaled to dw x dh, blended.
void cv_image(canvas *c, int x, int y, int dw, int dh, const uint8_t *rgba, int sw, int sh);
// UTF-8 text; clipped to maxw pixels with "..." (maxw <= 0: no limit). Returns drawn width.
int  cv_text(canvas *c, int x, int y, const char *utf8, uint32_t color, int maxw);
int  text_width(const char *utf8);
int  text_height(void);

// The game's display buffer.
typedef struct {
    uint8_t *base;
    int      width, height, pitch;   // pitch in pixels
    uint32_t format;                 // SceVideoOutPixelFormat
    int      tiled;                  // tilingMode == 0 (TILE)
} framebuffer;

// Puts the canvas at (x, y) into the frame buffer, each canvas pixel as scale x scale.
// Only (mostly) opaque pixels are written; the game's picture is never read back
// (that is far too slow on the PS4's display memory).
void fb_present(const framebuffer *fb, const canvas *c, int x, int y, int scale);
const char *fb_format_name(uint32_t format);
