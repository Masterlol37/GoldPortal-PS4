// Renders the on-screen menu on a PC (no PS4 needed) over a stand-in game picture and
// writes it as a 1920x1080 PPM image.
// Build + run on Linux/WSL:  bash test/menu_preview.sh <figures dir> <icon pack dir> <out.ppm> [title] [scene]
// scene: slots | grid | special | swap
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include "../source/menu.h"
#include "../source/portal.h"
#include <orbis/_types/pad.h>
#include "../source/lang.h"

int menu_debug_select(const char *s);

// --- stubs for what the plugin gets from the PS4 / the rest of the plugin ---
int g_trace;
char g_title_id[16];
char g_log_path[96];
void log_write(int sync, const char *fmt, ...)
{
    (void)sync;
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr);
}
int file_exists(const char *p) { return access(p, F_OK) == 0; }

static portal_slot_info g_info[PORTAL_SLOTS];
static uint8_t g_data[PORTAL_SLOTS][FIGURE_SIZE];
int portal_load(int s, const uint8_t *d, size_t n, const char *name)
{
    (void)n; memcpy(g_data[s], d, FIGURE_SIZE);
    g_info[s].loaded = 1;
    g_info[s].figure_id = d[0x10] | d[0x11] << 8;
    g_info[s].variant = d[0x1C] | d[0x1D] << 8;
    snprintf(g_info[s].name, sizeof(g_info[s].name), "%s", name);
    return 0;
}
int portal_remove(int s) { g_info[s].loaded = 0; return 0; }
int portal_get_dump(int s, uint8_t out[FIGURE_SIZE]) { memcpy(out, g_data[s], FIGURE_SIZE); return 0; }
void portal_get_info(int s, portal_slot_info *i) { *i = g_info[s]; }
void portal_clear_dirty(int s) { g_info[s].dirty = 0; }
int iconx_progress(void) { return getenv("PREVIEW_PROGRESS") ? atoi(getenv("PREVIEW_PROGRESS")) : -1; }

// Orbis kernel calls used by menu.c, mapped to POSIX
static DIR *g_dirs[64];
int sceKernelOpen(const char *p, int flags, int mode)
{
    (void)flags; (void)mode;
    DIR *d = opendir(p);
    if (!d) return -1;
    for (int i = 0; i < 64; i++) if (!g_dirs[i]) { g_dirs[i] = d; return i; }
    closedir(d);
    return -1;
}
int sceKernelGetdents(int fd, char *buf, int size)
{
    struct { uint32_t ino; uint16_t reclen; uint8_t type, namlen; char name[256]; } *e;
    int used = 0;
    struct dirent *de;
    while (used + 264 <= size && (de = readdir(g_dirs[fd])) != NULL) {
        e = (void *)(buf + used);
        e->ino = 1; e->reclen = 264; e->type = de->d_type; e->namlen = (uint8_t)strlen(de->d_name);
        memcpy(e->name, de->d_name, e->namlen + 1);
        used += 264;
    }
    return used;
}
int sceKernelClose(int fd) { closedir(g_dirs[fd]); g_dirs[fd] = NULL; return 0; }
uint64_t sceKernelGetProcessTime(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

#define B(x) menu_buttons(x)
static void pick(const char *path_part)
{
    B(ORBIS_PAD_BUTTON_CROSS);                    // open the grid for the current slot
    if (menu_debug_select(path_part) < 0) fprintf(stderr, "preview: %s not listed\n", path_part);
    B(ORBIS_PAD_BUTTON_CROSS);                    // place
}

int main(int argc, char **argv)
{
    const char *title = argc > 2 ? argv[2] : "CUSA00024";
    const char *scene = argc > 3 ? argv[3] : "grid";
    lang_init(getenv("PREVIEW_LANG") ? atoi(getenv("PREVIEW_LANG")) : 4);
    menu_init(title);
    menu_open();
    pick("Star Strike");
    B(ORBIS_PAD_BUTTON_DOWN);
    pick("Spyro (Series 2)");
    B(ORBIS_PAD_BUTTON_DOWN);
    if (!strcmp(scene, "grid")) {
        B(ORBIS_PAD_BUTTON_CROSS);
        menu_debug_select("Wash Buckler");
    } else if (!strcmp(scene, "filter")) {
        B(ORBIS_PAD_BUTTON_CROSS);
        B(ORBIS_PAD_BUTTON_R1); B(ORBIS_PAD_BUTTON_R1);       // game: Giants
        B(ORBIS_PAD_BUTTON_SQUARE); B(ORBIS_PAD_BUTTON_SQUARE); // kind: Magic Items
    } else if (!strcmp(scene, "swap")) {
        pick("Blast Zone Top");                      // should leave the grid open with bottoms only
    } else if (!strcmp(scene, "special")) {
        B(ORBIS_PAD_BUTTON_UP); B(ORBIS_PAD_BUTTON_UP); B(ORBIS_PAD_BUTTON_UP);   // to slot 8
        B(ORBIS_PAD_BUTTON_CROSS);
    }

    canvas c;
    cv_init(&c, MENU_W, MENU_H);
    menu_render(&c);

    // Stand-in game picture: a colourful gradient with stripes.
    int W = 1920, H = 1080, ox = (W - MENU_W) / 2, oy = (H - MENU_H) / 2;
    FILE *f = fopen(argv[1], "wb");
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint32_t r = 40 + x * 160 / W, g = 120 + y * 100 / H, b = 200 - x * 120 / W;
            if (((x + y) / 60) % 2) { r = r * 3 / 4; g = g * 3 / 4; b = b * 3 / 4; }
            int cx = x - ox, cy = y - oy;
            if (cx >= 0 && cy >= 0 && cx < MENU_W && cy < MENU_H) {
                uint32_t p = c.px[cy * MENU_W + cx], a = p >> 24;
                r = ((p >> 16 & 0xFF) * a + r * (255 - a)) / 255;
                g = ((p >> 8 & 0xFF) * a + g * (255 - a)) / 255;
                b = ((p & 0xFF) * a + b * (255 - a)) / 255;
            }
            fputc(r, f); fputc(g, f); fputc(b, f);
        }
    fclose(f);
    return 0;
}
