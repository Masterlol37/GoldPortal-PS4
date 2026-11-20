#include "menu.h"
#include "portal.h"
#include "log.h"
#include "lang.h"
#include "iconx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <orbis/libkernel.h>
#include <orbis/_types/pad.h>

// ---------------------------------------------------------------------------
// Games. A figure is listed only if it is not newer than the game: its game is the
// later of the year code in the dump's variant (bits 12-15) and the game its figure
// id belongs to (some dumps carry year 0). Figures without an icon (unknown to
// Imaginators, e.g. the Nintendo figures that don't work on PS4) are hidden too.
// ---------------------------------------------------------------------------
enum { K_NORMAL, K_TRAP, K_VEHICLE, K_SWAP_BOTTOM, K_SWAP_TOP };

static const struct { const char *title; int year; const char *name; int special; } k_games[] = {
    { "CUSA00024", 2, "Swap Force",    K_NORMAL  },
    { "CUSA00697", 3, "Trap Team",     K_TRAP    },
    { "CUSA02180", 4, "SuperChargers", K_VEHICLE },
    { "CUSA05158", 5, "Imaginators",   K_NORMAL  },
};
static int  g_year = 5;
static int  g_special = K_NORMAL;          // what the last slot is reserved for
static char g_game_name[32] = "Skylanders";

// First game (year code) a figure id exists in.
static int id_year(unsigned id)
{
    if (id < 100) return 0;
    if (id < 200) return 1;                 // Giants
    if (id < 210) return 0;                 // magic items of Spyro's Adventure / Giants (year decides)
    if (id < 235) return 3;                 // traps, Trap Team items
    if (id < 300) return 5;                 // Imaginators chests
    if (id < 305) return 0;
    if (id < 310) return 3;
    if (id < 400) return 5;
    if (id < 450) return 0;                 // Spyro's Adventure legendaries
    if (id < 500) return 3;                 // Trap Masters + new Trap Team figures
    if (id < 600) return 0;                 // sidekicks / minis (year decides)
    if (id < 1000) return 5;                // Senseis, creation crystals
    if (id < 3220) return 2;                // Swap Force
    if (id < 3300) return 4;                // vehicles
    if (id < 3400) return 2;
    if (id < 3600) return 4;                // SuperChargers
    return 5;
}

static int kind_of(unsigned id)
{
    if (id >= 210 && id <= 220) return K_TRAP;
    if (id >= 3220 && id < 3300) return K_VEHICLE;
    if (id >= 1000 && id < 1999) return K_SWAP_BOTTOM;
    if (id >= 2000 && id < 2999) return K_SWAP_TOP;
    return K_NORMAL;
}

// What a figure is, for the "Art" filter.
enum { CAT_FIGURE, CAT_ITEM, CAT_TRAP, CAT_VEHICLE, CAT_CRYSTAL, CAT_COUNT };
static const str_id k_cat_names[CAT_COUNT] = { S_CAT_FIGURES, S_CAT_ITEMS, S_CAT_TRAPS, S_CAT_VEHICLES, S_CAT_CRYSTALS };
static const char *const k_game_names[6] = { "Spyro's Adventure", "Giants", "Swap Force", "Trap Team",
                                             "SuperChargers", "Imaginators" };

static int cat_of(unsigned id)
{
    if (id >= 210 && id <= 220) return CAT_TRAP;
    if (id >= 3220 && id < 3300) return CAT_VEHICLE;
    if (id >= 680 && id <= 689) return CAT_CRYSTAL;
    if ((id >= 200 && id < 300) || (id >= 300 && id < 400) || (id >= 3200 && id < 3220) ||
        (id >= 3300 && id < 3400) || (id >= 3500 && id < 3600))
        return CAT_ITEM;                      // magic items, adventure packs, chests
    return CAT_FIGURE;
}

static int contains_ci(const char *s, const char *w)
{
    size_t n = strlen(w);
    for (; *s; s++)
        if (strncasecmp(s, w, n) == 0) return 1;
    return 0;
}

// Chase variants: sorted into a "Chase" folder, or a store-exclusive deco (17-30, 33)
// on a figure that is not an in-game variant. Spyro's Adventure chase figures carry
// no variant data at all, so for them only the folder/file name tells.
static int is_chase(const char *path, unsigned id, unsigned var)
{
    if (contains_ci(path, "chase") || contains_ci(path, "employee edition")) return 1;
    unsigned deco = var & 0xFF;
    if (var >> 10 & 1) return 0;
    if (kind_of(id) == K_TRAP || id == 235 || (id >= 680 && id <= 689)) return 0;
    return (deco >= 17 && deco <= 30) || deco == 33;
}

// ---------------------------------------------------------------------------
// Icon index (/data/goldportal/icons/index.txt: "size N" then "id variant icon")
// ---------------------------------------------------------------------------
typedef struct { uint16_t id; int32_t variant; int16_t icon; } icon_key;
static icon_key *g_keys;
static int       g_key_count;
static char    **g_icon_names;
static int       g_icon_count;
static int       g_icon_size = 96;
static int       g_pack_year = -1;     // year code of the game the icon pack came from

#define ICON_CACHE 72
static struct { int icon; uint8_t *rgba; uint32_t used; } g_cache[ICON_CACHE];
static uint32_t g_cache_clock;

static char *read_all(const char *path, size_t *len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    off_t size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    char *buf = size > 0 ? malloc((size_t)size + 1) : NULL;
    ssize_t n = buf ? read(fd, buf, (size_t)size) : -1;
    close(fd);
    if (n != size) { free(buf); return NULL; }
    buf[n] = 0;
    if (len) *len = (size_t)n;
    return buf;
}

static int icon_by_name(const char *name)
{
    for (int i = 0; i < g_icon_count; i++)
        if (strcmp(g_icon_names[i], name) == 0) return i;
    char **n = realloc(g_icon_names, sizeof(char *) * (g_icon_count + 1));
    if (!n) return -1;
    g_icon_names = n;
    g_icon_names[g_icon_count] = strdup(name);
    return g_icon_count++;
}

static void load_icon_index(void)
{
    char *txt = read_all(GP_ICON_DIR "/index.txt", NULL);
    if (!txt) { LOG("menu: no icon index at " GP_ICON_DIR "/index.txt"); return; }
    int cap = 0;
    g_pack_year = 5;                       // pack made on the PC from Imaginators (no "source" line)
    for (char *line = strtok(txt, "\n"); line; line = strtok(NULL, "\n")) {
        unsigned id; int var; char name[128];
        if (sscanf(line, "size %d", &g_icon_size) == 1) continue;
        if (sscanf(line, "source %127s %d", name, &var) == 2) { g_pack_year = var; continue; }
        if (sscanf(line, "%u %d %127s", &id, &var, name) != 3) continue;
        if (g_key_count == cap) {
            cap = cap ? cap * 2 : 512;
            icon_key *k = realloc(g_keys, sizeof(icon_key) * cap);
            if (!k) break;
            g_keys = k;
        }
        g_keys[g_key_count++] = (icon_key){ (uint16_t)id, var, (int16_t)icon_by_name(name) };
    }
    free(txt);
    LOG("menu: icon index has %d entries, %d icons (%dx%d)", g_key_count, g_icon_count, g_icon_size, g_icon_size);
}

// Icon for a figure: exact variant first, then the figure's default. -1 if none.
static int icon_for(uint16_t id, uint16_t variant)
{
    int base = -1;
    for (int i = 0; i < g_key_count; i++) {
        if (g_keys[i].id != id) continue;
        if (g_keys[i].variant == variant) return g_keys[i].icon;
        if (g_keys[i].variant == -1) base = g_keys[i].icon;
    }
    return base;
}

static const uint8_t *icon_pixels(int icon)
{
    if (icon < 0 || icon >= g_icon_count) return NULL;
    int victim = 0;
    for (int i = 0; i < ICON_CACHE; i++) {
        if (g_cache[i].rgba && g_cache[i].icon == icon) {
            g_cache[i].used = ++g_cache_clock;
            return g_cache[i].rgba;
        }
        if (!g_cache[i].rgba || g_cache[i].used < g_cache[victim].used) victim = i;
    }
    char path[256];
    snprintf(path, sizeof(path), GP_ICON_DIR "/%s.rgba", g_icon_names[icon]);
    size_t len = 0;
    char *data = read_all(path, &len);
    if (!data || len < (size_t)g_icon_size * g_icon_size * 4) { free(data); return NULL; }
    free(g_cache[victim].rgba);
    g_cache[victim].rgba = (uint8_t *)data;
    g_cache[victim].icon = icon;
    g_cache[victim].used = ++g_cache_clock;
    return (uint8_t *)data;
}

// ---------------------------------------------------------------------------
// Library: every .dump below /data/goldportal/figures this game can use
// ---------------------------------------------------------------------------
typedef struct {
    char    *path;
    char    *name;           // file name without .dump
    uint16_t id, variant;
    int16_t  icon;
    uint8_t  kind, chase;
    uint8_t  game, cat;      // game it is from (year code), CAT_*
} figure;

#define MAX_FIGURES 4096
static figure *g_figs;
static int     g_fig_count, g_hidden_count;
static int     g_scanned;

struct gp_dirent {           // FreeBSD dirent as returned by getdents
    uint32_t d_fileno;
    uint16_t d_reclen;
    uint8_t  d_type;
    uint8_t  d_namlen;
    char     d_name[256];
};

static void add_figure(const char *path, const char *fname)
{
    size_t n = strlen(fname);
    if (n < 6 || strcasecmp(fname + n - 5, ".dump") != 0) return;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    uint8_t hdr[0x20];
    ssize_t r = read(fd, hdr, sizeof(hdr));
    close(fd);
    if (r != sizeof(hdr)) return;
    uint16_t id = hdr[0x10] | hdr[0x11] << 8, var = hdr[0x1C] | hdr[0x1D] << 8;
    int icon = icon_for(id, var);
    int year = var >> 12, iy = id_year(id);
    if (iy > year) year = iy;
    // No icon: unknown to the game the pack came from. Only conclusive if that game is not
    // older than this one (else the icons of this game may just not be extracted yet).
    if (year > g_year || (g_key_count && icon < 0 && g_pack_year >= g_year)) { g_hidden_count++; return; }
    if (g_fig_count >= MAX_FIGURES) return;
    figure *f = &g_figs[g_fig_count++];
    f->path = strdup(path);
    f->name = strndup(fname, n - 5);
    f->id = id;
    f->variant = var;
    f->icon = (int16_t)icon;
    f->kind = (uint8_t)kind_of(id);
    f->chase = (uint8_t)is_chase(path, id, var);
    f->game = (uint8_t)(year > 5 ? 5 : year);
    f->cat = (uint8_t)cat_of(id);
}

static void scan_dir(const char *dir, int depth)
{
    if (depth > 6) return;
    int fd = sceKernelOpen(dir, 0x20000 /* O_DIRECTORY */, 0);
    if (fd < 0) return;
    char *buf = malloc(8192);
    int n;
    while (buf && (n = sceKernelGetdents(fd, buf, 8192)) > 0) {
        for (int off = 0; off < n;) {
            struct gp_dirent *e = (struct gp_dirent *)(buf + off);
            if (e->d_reclen == 0) break;
            off += e->d_reclen;
            if (e->d_name[0] == '.') continue;
            char path[512];
            snprintf(path, sizeof(path), "%s/%.*s", dir, e->d_namlen, e->d_name);
            if (e->d_type == 4) scan_dir(path, depth + 1);
            else if (e->d_type == 8) add_figure(path, path + strlen(dir) + 1);
        }
    }
    free(buf);
    sceKernelClose(fd);
}

// Natural, case-insensitive order ("Chest 2" before "Chest 10").
static int natcmp(const char *a, const char *b)
{
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            unsigned long x = strtoul(a, (char **)&a, 10), y = strtoul(b, (char **)&b, 10);
            if (x != y) return x < y ? -1 : 1;
            continue;
        }
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}
static int fig_cmp(const void *x, const void *y)
{
    return natcmp(((const figure *)x)->path, ((const figure *)y)->path);
}

static int *g_view;                     // indices into g_figs the grid shows
static int  g_view_count;

static void scan_library(void)
{
    for (int i = 0; i < g_fig_count; i++) { free(g_figs[i].path); free(g_figs[i].name); }
    g_fig_count = g_hidden_count = 0;
    g_view_count = 0;
    if (!g_figs) g_figs = calloc(MAX_FIGURES, sizeof(figure));
    if (!g_view) g_view = calloc(MAX_FIGURES, sizeof(int));
    if (!g_figs || !g_view) return;
    uint64_t t0 = sceKernelGetProcessTime();
    scan_dir(GP_FIGURE_DIR, 0);
    qsort(g_figs, g_fig_count, sizeof(figure), fig_cmp);
    int chase = 0;
    for (int i = 0; i < g_fig_count; i++) chase += g_figs[i].chase;
    LOG("menu: library scanned in %llu ms: %d figures for %s (%d chase variants), %d hidden",
        (unsigned long long)(sceKernelGetProcessTime() - t0) / 1000, g_fig_count, g_game_name, chase, g_hidden_count);
    g_scanned = 1;
}

// ---------------------------------------------------------------------------
// Slots placed from the menu, with the file they came from (for saving back)
// ---------------------------------------------------------------------------
#define MENU_SLOTS 8
#define SPECIAL_SLOT (MENU_SLOTS - 1)
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char     g_slot_path[MENU_SLOTS][512];
static int      g_slot_backed_up[MENU_SLOTS];
static uint8_t  g_snap[MENU_SLOTS][FIGURE_SIZE];
static int      g_snap_valid[MENU_SLOTS];

static void write_file(const char *path, const uint8_t *d, size_t n)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0777);
    if (fd < 0) { LOG("menu: cannot write %s", path); return; }
    write(fd, d, n);
    fsync(fd);
    close(fd);
}

// Writes the slot's current data back to its file (first save keeps a .bak).
static void save_slot(int s, const uint8_t *data)
{
    if (!g_slot_path[s][0]) return;
    if (!g_slot_backed_up[s]) {
        char bak[520];
        snprintf(bak, sizeof(bak), "%s.bak", g_slot_path[s]);
        size_t len;
        char *orig = read_all(g_slot_path[s], &len);
        if (orig) { write_file(bak, (uint8_t *)orig, len); free(orig); }
        g_slot_backed_up[s] = 1;
    }
    write_file(g_slot_path[s], data, FIGURE_SIZE);
    portal_clear_dirty(s);
    LOG("menu: saved slot %d to %s", s + 1, g_slot_path[s]);
}

static char g_slot_name[MENU_SLOTS][FIGURE_NAME_LEN];

// Is the slot still holding what the menu put there? (The PC app or web page may
// have replaced it; then its file is not ours to write.)
static int slot_is_ours(int s, const portal_slot_info *info)
{
    if (!g_slot_path[s][0]) return 0;
    if (strncmp(info->name, g_slot_name[s], FIGURE_NAME_LEN) != 0) {
        g_slot_path[s][0] = 0;
        return 0;
    }
    return 1;
}

static void save_if_dirty(int s)
{
    portal_slot_info info;
    portal_get_info(s, &info);
    uint8_t d[FIGURE_SIZE];
    if (slot_is_ours(s, &info) && info.dirty && portal_get_dump(s, d) == 0)
        save_slot(s, d);
}

void menu_save_all(void)
{
    pthread_mutex_lock(&g_lock);
    for (int s = 0; s < MENU_SLOTS; s++) save_if_dirty(s);
    pthread_mutex_unlock(&g_lock);
}

// Every ~30 s: a dirty slot is saved once two reads a second apart match,
// so a figure is never saved halfway through the game writing it.
void menu_tick(void)
{
    static int ticks;
    pthread_mutex_lock(&g_lock);
    for (int s = 0; s < MENU_SLOTS; s++) {
        portal_slot_info info;
        portal_get_info(s, &info);
        if (!slot_is_ours(s, &info) || !info.dirty) { g_snap_valid[s] = 0; continue; }
        uint8_t d[FIGURE_SIZE];
        if (portal_get_dump(s, d) != 0) continue;
        if (g_snap_valid[s]) {
            if (memcmp(d, g_snap[s], FIGURE_SIZE) == 0) save_slot(s, d);
            g_snap_valid[s] = 0;
        } else if (ticks % 30 == 0) {
            memcpy(g_snap[s], d, FIGURE_SIZE);
            g_snap_valid[s] = 1;
        }
    }
    ticks++;
    pthread_mutex_unlock(&g_lock);
}

// ---------------------------------------------------------------------------
// Menu state: the slot list on the left; the figure grid opens for one slot.
// ---------------------------------------------------------------------------
#define COLS 8
#define ROWS 4
#define PER_PAGE (COLS * ROWS)
static int  g_slot;                  // slot under the cursor / being filled
static int  g_grid;                  // 1 while the figure grid is shown
static int  g_sel;                   // index into g_view
static int  g_filter;                // K_* the grid is limited to (K_NORMAL: everything)
static int  g_swap_step;             // grid opened to pick the other half of a swapper
static int  g_hide_chase;
static int  g_game_filter = -1;      // -1 all, else year code
static int  g_cat_filter = -1;       // -1 all, else CAT_*
static int  g_dirty = 1;
static char g_status[128];

static int slot_kind(int s) { return s == SPECIAL_SLOT ? g_special : K_NORMAL; }

static void load_settings(void)
{
    char *t = read_all(GP_MENU_CFG, NULL);
    if (!t) return;
    char *p = strstr(t, "hide_chase=");
    if (p) g_hide_chase = atoi(p + 11) != 0;
    free(t);
}

static void save_settings(void)
{
    char t[64];
    int n = snprintf(t, sizeof(t), "hide_chase=%d\n", g_hide_chase);
    write_file(GP_MENU_CFG, (const uint8_t *)t, (size_t)n);
}

static void build_view(void)
{
    g_view_count = 0;
    if (!g_view) return;
    for (int i = 0; i < g_fig_count; i++) {
        figure *f = &g_figs[i];
        if (g_hide_chase && f->chase) continue;
        if (g_filter != K_NORMAL) {
            // Locked list (trap/vehicle slot, other swap half): the user filters don't apply.
            if (f->kind != g_filter) continue;
        } else {
            if (g_game_filter >= 0 && f->game != g_game_filter) continue;
            if (g_cat_filter >= 0 && f->cat != g_cat_filter) continue;
        }
        g_view[g_view_count++] = i;
    }
    if (g_sel >= g_view_count) g_sel = g_view_count ? g_view_count - 1 : 0;
}

// Opens the grid for slot s; selects the figure that is on it if it came from the menu.
static void open_grid(int s, int filter)
{
    g_slot = s;
    g_grid = 1;
    g_filter = filter;
    build_view();
    g_sel = 0;
    for (int k = 0; k < g_view_count; k++)
        if (g_slot_path[s][0] && strcmp(g_figs[g_view[k]].path, g_slot_path[s]) == 0) { g_sel = k; break; }
}

static void close_grid(void)
{
    g_grid = 0;
    g_swap_step = 0;
    g_filter = K_NORMAL;
}

void menu_init(const char *title_id)
{
    for (size_t i = 0; i < sizeof(k_games) / sizeof(k_games[0]); i++)
        if (strcmp(title_id, k_games[i].title) == 0) {
            g_year = k_games[i].year;
            g_special = k_games[i].special;
            snprintf(g_game_name, sizeof(g_game_name), "%s", k_games[i].name);
        }
    LOG("menu: game %s (%s), lists figures up to year code %d", title_id, g_game_name, g_year);
}

int menu_year(void) { return g_year; }

// A new icon pack was written (iconx): forget the old index and cached pictures, read the
// new ones and rescan (which figures are listed depends on the icons too).
void menu_reload_icons(void)
{
    for (int i = 0; i < ICON_CACHE; i++) { free(g_cache[i].rgba); g_cache[i].rgba = NULL; g_cache[i].used = 0; }
    for (int i = 0; i < g_icon_count; i++) free(g_icon_names[i]);
    free(g_icon_names); g_icon_names = NULL; g_icon_count = 0;
    free(g_keys); g_keys = NULL; g_key_count = 0;
    load_icon_index();
    if (g_scanned) {
        scan_library();
        if (g_grid) build_view();
    }
    g_dirty = 1;
}

void menu_preload(void)
{
    if (!g_scanned) {
        if (!g_key_count) load_icon_index();
        load_settings();
        scan_library();
    }
}

void menu_open(void)
{
    menu_preload();
    if (!g_fig_count)
        snprintf(g_status, sizeof(g_status), T(S_NO_FIGURES_IN), GP_FIGURE_DIR);
    close_grid();
    g_dirty = 1;
}

static int place(int fi, int s)
{
    figure *f = &g_figs[fi];
    size_t len = 0;
    char *data = read_all(f->path, &len);
    if (!data || len < FIGURE_SIZE) {
        free(data);
        snprintf(g_status, sizeof(g_status), T(S_CANT_READ), f->name);
        return -1;
    }
    pthread_mutex_lock(&g_lock);
    save_if_dirty(s);                              // the figure being replaced
    portal_load(s, (uint8_t *)data, FIGURE_SIZE, f->name);
    snprintf(g_slot_path[s], sizeof(g_slot_path[s]), "%s", f->path);
    portal_slot_info info;
    portal_get_info(s, &info);
    memcpy(g_slot_name[s], info.name, FIGURE_NAME_LEN);
    g_slot_backed_up[s] = 0;
    g_snap_valid[s] = 0;
    pthread_mutex_unlock(&g_lock);
    free(data);
    snprintf(g_status, sizeof(g_status), T(S_PLACED), f->name, s + 1);
    LOG("menu: placed %s on slot %d", f->path, s + 1);
    return 0;
}

static void remove_slot(int s)
{
    pthread_mutex_lock(&g_lock);
    save_if_dirty(s);
    g_slot_path[s][0] = 0;
    pthread_mutex_unlock(&g_lock);
    if (portal_remove(s) == 0)
        snprintf(g_status, sizeof(g_status), T(S_SLOT_FREE), s + 1);
}

// After half a swapper went on slot s: the slot for the other half (the first free
// normal slot, else the next normal one).
static int swap_partner_slot(int s)
{
    for (int k = 1; k < MENU_SLOTS; k++) {
        int t = (s + k) % MENU_SLOTS;
        portal_slot_info info;
        portal_get_info(t, &info);
        if (slot_kind(t) == K_NORMAL && !info.loaded) return t;
    }
    for (int k = 1; k < MENU_SLOTS; k++) {
        int t = (s + k) % MENU_SLOTS;
        if (slot_kind(t) == K_NORMAL) return t;
    }
    return s;
}

// Is the other half of a swapper on the portal already?
static int other_half_present(int kind, int except)
{
    int want = kind == K_SWAP_TOP ? K_SWAP_BOTTOM : K_SWAP_TOP;
    for (int t = 0; t < MENU_SLOTS; t++) {
        if (t == except) continue;
        portal_slot_info info;
        portal_get_info(t, &info);
        if (info.loaded && kind_of(info.figure_id) == want) return 1;
    }
    return 0;
}

// Filter values that have at least one figure (with the other filter as it is).
static int game_present(int g)
{
    for (int i = 0; i < g_fig_count; i++)
        if (g_figs[i].game == g && (g_cat_filter < 0 || g_figs[i].cat == g_cat_filter)) return 1;
    return 0;
}
static int cat_present(int c)
{
    for (int i = 0; i < g_fig_count; i++)
        if (g_figs[i].cat == c && (g_game_filter < 0 || g_figs[i].game == g_game_filter)) return 1;
    return 0;
}
// Cycles -1 (all) -> first present value -> ... -> -1.
static int next_filter(int cur, int dir, int count, int (*present)(int))
{
    for (int k = 0; k < count + 1; k++) {
        cur += dir;
        if (cur >= count) cur = -1;
        if (cur < -1) cur = count - 1;
        if (cur == -1 || present(cur)) return cur;
    }
    return -1;
}

static void grid_buttons(uint32_t b)
{
    int n = g_view_count;
    if (n) {
        if (b & ORBIS_PAD_BUTTON_RIGHT) g_sel = (g_sel + 1) % n;
        if (b & ORBIS_PAD_BUTTON_LEFT)  g_sel = (g_sel + n - 1) % n;
        if (b & ORBIS_PAD_BUTTON_DOWN)  g_sel = g_sel + COLS < n ? g_sel + COLS : g_sel;
        if (b & ORBIS_PAD_BUTTON_UP)    g_sel = g_sel >= COLS ? g_sel - COLS : g_sel;
        if (b & ORBIS_PAD_BUTTON_R2) g_sel = g_sel + PER_PAGE < n ? g_sel + PER_PAGE : n - 1;
        if (b & ORBIS_PAD_BUTTON_L2) g_sel = g_sel >= PER_PAGE ? g_sel - PER_PAGE : 0;
    }
    if (g_filter == K_NORMAL && (b & (ORBIS_PAD_BUTTON_L1 | ORBIS_PAD_BUTTON_R1 | ORBIS_PAD_BUTTON_SQUARE))) {
        if (b & (ORBIS_PAD_BUTTON_L1 | ORBIS_PAD_BUTTON_R1))
            g_game_filter = next_filter(g_game_filter, b & ORBIS_PAD_BUTTON_R1 ? 1 : -1, 6, game_present);
        if (b & ORBIS_PAD_BUTTON_SQUARE)
            g_cat_filter = next_filter(g_cat_filter, 1, CAT_COUNT, cat_present);
        g_sel = 0;
        build_view();
        return;
    }
    if (b & ORBIS_PAD_BUTTON_TRIANGLE) {
        g_hide_chase = !g_hide_chase;
        int cur = n ? g_view[g_sel] : -1;
        build_view();
        for (int k = 0; k < g_view_count; k++) if (g_view[k] == cur) { g_sel = k; break; }
        snprintf(g_status, sizeof(g_status), "%s", T(g_hide_chase ? S_CHASE_HIDDEN : S_CHASE_SHOWN));
        save_settings();
        return;
    }
    if (b & ORBIS_PAD_BUTTON_CIRCLE) { close_grid(); return; }
    if ((b & ORBIS_PAD_BUTTON_CROSS) && n) {
        figure *f = &g_figs[g_view[g_sel]];
        int kind = f->kind, s = g_slot;
        if (place(g_view[g_sel], s) != 0) return;
        if ((kind == K_SWAP_TOP || kind == K_SWAP_BOTTOM) && !g_swap_step && !other_half_present(kind, s)) {
            // Show only the other halves, for the next slot.
            int t = swap_partner_slot(s);
            g_swap_step = 1;
            open_grid(t, kind == K_SWAP_TOP ? K_SWAP_BOTTOM : K_SWAP_TOP);
            snprintf(g_status, sizeof(g_status), T(S_SWAP_NEXT), f->name, s + 1,
                     T(kind == K_SWAP_TOP ? S_A_BOTTOM : S_A_TOP));
            return;
        }
        close_grid();
    }
}

int menu_buttons(uint32_t b)
{
    if (!b) return 0;
    g_dirty = 1;
    if (b & ORBIS_PAD_BUTTON_OPTIONS) return 1;
    if (g_grid) { grid_buttons(b); return 0; }

    if (b & ORBIS_PAD_BUTTON_CIRCLE) return 1;
    if (b & ORBIS_PAD_BUTTON_DOWN) g_slot = (g_slot + 1) % MENU_SLOTS;
    if (b & ORBIS_PAD_BUTTON_UP)   g_slot = (g_slot + MENU_SLOTS - 1) % MENU_SLOTS;
    if (b & ORBIS_PAD_BUTTON_SQUARE) remove_slot(g_slot);
    if (b & ORBIS_PAD_BUTTON_CROSS) open_grid(g_slot, slot_kind(g_slot));
    if (b & ORBIS_PAD_BUTTON_TRIANGLE) {
        scan_library();
        snprintf(g_status, sizeof(g_status), T(S_FOUND_N), g_fig_count);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Drawing (canvas MENU_W x MENU_H; see-through background, opaque text/icons)
// ---------------------------------------------------------------------------
#define C_PANEL    0xFF0E1322u     // opaque: reading the game's picture back for see-through is too slow
#define C_ROW      0x90222C46u
#define C_ROW_SEL  0xE0304068u
#define C_CELL     0x80222C46u
#define C_CELL_SEL 0xE8344470u
#define C_SLOT_BG  0x60000000u
#define C_LINE     0x40FFFFFFu
#define C_GOLD     0xFFE8B23Au
#define C_TEXT     0xFFF2F2F2u
#define C_DIM      0xFFA0A8BCu
#define C_TRAP     0xFF4FD1C5u
#define C_VEHICLE  0xFFF29A4Au
#define C_SWAP     0xFF9D8CFFu

#define LP_W   430               // slot panel width
#define GP_X   450               // grid panel
#define PANEL_R 20

static const char *kind_label(int k)
{
    switch (k) {
    case K_TRAP:        return T(S_TRAP_SLOT);
    case K_VEHICLE:     return T(S_VEHICLE_SLOT);
    case K_SWAP_TOP:    return T(S_TOPS);
    case K_SWAP_BOTTOM: return T(S_BOTTOMS);
    default:            return "";
    }
}
static uint32_t kind_color(int k)
{
    switch (k) {
    case K_TRAP:    return C_TRAP;
    case K_VEHICLE: return C_VEHICLE;
    case K_SWAP_TOP: case K_SWAP_BOTTOM: return C_SWAP;
    default:        return C_GOLD;
    }
}

// A PlayStation button as a small round badge ('X' cross, 'O' circle, 'S' square, 'T' triangle),
// followed by a label. Returns the x after it.
static int hint(canvas *c, int x, int y, char btn, const char *label)
{
    int th = text_height(), d = 28, cy = y + th / 2, cx = x + d / 2;
    cv_round(c, x, cy - d / 2, d, d, d / 2, 0, 0xFF2A3350u);
    switch (btn) {
    case 'X': {
        uint32_t col = 0xFF8FB4FFu;
        for (int i = -6; i <= 6; i++) {
            cv_fill(c, cx + i - 1, cy + i - 1, 3, 2, col);
            cv_fill(c, cx + i - 1, cy - i - 1, 3, 2, col);
        }
        break;
    }
    case 'O': cv_round(c, cx - 8, cy - 8, 16, 16, 8, 2, 0xFFFF7A7Au); break;
    case 'S': cv_round(c, cx - 7, cy - 7, 14, 14, 2, 2, 0xFFF59BD6u); break;
    case 'T': {
        uint32_t col = 0xFF6EE2B8u;
        int top = cy - 7, base = cy + 6;
        for (int r = top; r <= base; r++) {
            int half = (r - top) * 8 / (base - top);
            if (r >= base - 1) cv_fill(c, cx - half, r, 2 * half + 1, 1, col);
            else { cv_fill(c, cx - half - 1, r, 2, 1, col); cv_fill(c, cx + half, r, 2, 1, col); }
        }
        break;
    }
    }
    x += d + 8;
    x += cv_text(c, x, y, label, C_DIM, 0);
    return x + 26;
}

// A shoulder button ("L1", "R2" ...) as a small rounded badge. Returns the x after it.
static int badge(canvas *c, int x, int y, const char *txt)
{
    int th = text_height(), w = text_width(txt) + 14;
    cv_round(c, x, y + th / 2 - 14, w, 28, 8, 0, 0xFF2A3350u);
    cv_text(c, x + 7, y, txt, C_TEXT, 0);
    return x + w;
}

// Filter chip: "<name>: <value>" with the value highlighted when a filter is set.
static int chip(canvas *c, int x, int y, const char *name, const char *value, int active, int locked)
{
    int th = text_height();
    char a[48];
    snprintf(a, sizeof(a), "%s: ", name);
    int w = text_width(a) + text_width(value) + 28;
    cv_round(c, x, y - 6, w, th + 12, (th + 12) / 2, 0, active ? 0xFF3A3420u : 0xFF1E2640u);
    cv_round(c, x, y - 6, w, th + 12, (th + 12) / 2, 2, locked ? 0xFF606880u : active ? C_GOLD : 0xFF3A4566u);
    int tx = x + 14 + cv_text(c, x + 14, y, a, C_DIM, 0);
    cv_text(c, tx, y, value, locked ? C_DIM : active ? C_GOLD : C_TEXT, 0);
    return x + w;
}

// Centered name in up to two lines inside width w.
static void draw_name2(canvas *c, int x, int y, int w, const char *name, uint32_t color)
{
    int tw = text_width(name);
    if (tw <= w) { cv_text(c, x + (w - tw) / 2, y + 12, name, color, 0); return; }
    char a[160], b[160];
    int best = -1;
    for (int i = 0; name[i] && i < (int)sizeof(a) - 1; i++) {
        if (name[i] != ' ') continue;
        memcpy(a, name, (size_t)i); a[i] = 0;
        if (text_width(a) <= w) best = i; else break;
    }
    if (best < 0) { cv_text(c, x, y + 12, name, color, w); return; }
    memcpy(a, name, (size_t)best); a[best] = 0;
    snprintf(b, sizeof(b), "%s", name + best + 1);
    int aw = text_width(a), bw = text_width(b);
    cv_text(c, x + (w - aw) / 2, y, a, color, 0);
    cv_text(c, x + (bw < w ? (w - bw) / 2 : 0), y + 25, b, color, w);
}

static void draw_slots(canvas *c, const portal_slot_info *info)
{
    int th = text_height();
    cv_round(c, 0, 0, LP_W, c->h, PANEL_R, 0, C_PANEL);
    cv_round(c, 0, 0, LP_W, c->h, PANEL_R, 2, 0x50E8B23Au);
    cv_text(c, 24, 16, "Goldportal", C_GOLD, 0);
    cv_text(c, LP_W - 24 - text_width(g_game_name), 16, g_game_name, C_DIM, 0);
    char st[128];
    int prog = iconx_progress();
    if (prog >= 0) snprintf(st, sizeof(st), T(S_ICONS_LOADING), prog);
    else snprintf(st, sizeof(st), "%s", g_status[0] ? g_status : T(S_READY));
    cv_text(c, 24, 16 + th + 4, st, prog >= 0 ? C_GOLD : C_TEXT, LP_W - 48);
    cv_fill(c, 20, 16 + 2 * th + 14, LP_W - 40, 1, C_LINE);

    int y0 = 100, rh = 88, gap = 8;
    for (int s = 0; s < MENU_SLOTS; s++) {
        int ry = y0 + s * (rh + gap), cur = s == g_slot;
        int k = slot_kind(s);
        cv_round(c, 14, ry, LP_W - 28, rh, 14, 0, cur ? C_ROW_SEL : C_ROW);
        if (cur) cv_round(c, 14, ry, LP_W - 28, rh, 14, 3, g_grid ? kind_color(g_filter) : C_GOLD);
        else if (k != K_NORMAL) cv_round(c, 14, ry, LP_W - 28, rh, 14, 2, kind_color(k) & 0x90FFFFFFu);
        cv_round(c, 24, ry + 8, 72, 72, 12, 0, C_SLOT_BG);
        char label[32];
        if (k != K_NORMAL) snprintf(label, sizeof(label), "%d  %s", s + 1, kind_label(k));
        else snprintf(label, sizeof(label), T(S_SLOT_N), s + 1);
        cv_text(c, 110, ry + 12, label, k != K_NORMAL ? kind_color(k) : (cur ? C_GOLD : C_DIM), 0);
        if (info[s].loaded) {
            cv_image(c, 24, ry + 8, 72, 72, icon_pixels(icon_for(info[s].figure_id, info[s].variant)),
                     g_icon_size, g_icon_size);
            cv_text(c, 110, ry + 14 + th, info[s].name, C_TEXT, LP_W - 28 - 110);
        } else {
            cv_text(c, 50, ry + 8 + (72 - th) / 2, "+", C_DIM, 0);
            cv_text(c, 110, ry + 14 + th, T(S_EMPTY), C_DIM, 0);
        }
    }
    int hy = y0 + MENU_SLOTS * (rh + gap) + 10;
    if (g_grid) {
        cv_text(c, 24, hy, T(S_PICK_RIGHT), C_DIM, LP_W - 48);
    } else {
        int x = hint(c, 24, hy, 'X', T(S_SELECT));
        hint(c, x, hy, 'S', T(S_REMOVE));
        x = hint(c, 24, hy + th + 6, 'T', T(S_RESCAN));
        hint(c, x, hy + th + 6, 'O', T(S_CLOSE));
    }
}

static void draw_grid(canvas *c)
{
    int th = text_height(), gw = c->w - GP_X;
    cv_round(c, GP_X, 0, gw, c->h, PANEL_R, 0, C_PANEL);
    cv_round(c, GP_X, 0, gw, c->h, PANEL_R, 2, (kind_color(g_filter) & 0x00FFFFFF) | 0x60000000u);

    char title[96];
    if (g_swap_step) snprintf(title, sizeof(title), T(S_SWAP_FOR_SLOT),
                              T(g_filter == K_SWAP_TOP ? S_TOP : S_BOTTOM), g_slot + 1);
    else if (g_filter == K_TRAP) snprintf(title, sizeof(title), T(S_TRAP_FOR_SLOT), g_slot + 1);
    else if (g_filter == K_VEHICLE) snprintf(title, sizeof(title), T(S_VEHICLE_FOR_SLOT), g_slot + 1);
    else snprintf(title, sizeof(title), T(S_FIG_FOR_SLOT), g_slot + 1);
    cv_text(c, GP_X + 24, 16, title, kind_color(g_filter), 0);
    char info[96];
    snprintf(info, sizeof(info), T(S_COUNT_CHASE), g_view_count, T(g_hide_chase ? S_OFF : S_ON));
    cv_text(c, c->w - 24 - text_width(info), 16, info, C_DIM, 0);
    cv_fill(c, GP_X + 20, 16 + th + 10, gw - 40, 1, C_LINE);

    // Filter bar
    int fyb = 16 + th + 26, locked = g_filter != K_NORMAL;
    int bx = badge(c, GP_X + 24, fyb, "L1") + 8;
    bx = chip(c, bx, fyb, T(S_GAME), locked || g_game_filter < 0 ? T(S_ALL) : k_game_names[g_game_filter],
              !locked && g_game_filter >= 0, locked) + 8;
    bx = badge(c, bx, fyb, "R1") + 36;
    {
        int th2 = text_height(), d = 28, cy = fyb + th2 / 2;
        cv_round(c, bx, cy - d / 2, d, d, d / 2, 0, 0xFF2A3350u);
        cv_round(c, bx + d / 2 - 7, cy - 7, 14, 14, 2, 2, 0xFFF59BD6u);
        bx += d + 8;
    }
    const char *catv = locked ? T(g_filter == K_TRAP ? S_CAT_TRAPS : g_filter == K_VEHICLE ? S_CAT_VEHICLES : S_SWAP_PARTS)
                              : g_cat_filter < 0 ? T(S_ALL) : T(k_cat_names[g_cat_filter]);
    chip(c, bx, fyb, T(S_KIND), catv, !locked && g_cat_filter >= 0, locked);

    int gx = GP_X + 20, gy = fyb + th + 18, cw = (gw - 40) / COLS, ch = 184;
    int page = g_sel / PER_PAGE, first = page * PER_PAGE;
    for (int k = 0; k < PER_PAGE && first + k < g_view_count; k++) {
        figure *f = &g_figs[g_view[first + k]];
        int cx = gx + (k % COLS) * cw, cy = gy + (k / COLS) * ch;
        int sel = first + k == g_sel;
        cv_round(c, cx + 5, cy + 5, cw - 10, ch - 10, 14, 0, sel ? C_CELL_SEL : C_CELL);
        if (sel) cv_round(c, cx + 5, cy + 5, cw - 10, ch - 10, 14, 3, C_GOLD);
        cv_image(c, cx + (cw - 104) / 2, cy + 12, 104, 104, icon_pixels(f->icon), g_icon_size, g_icon_size);
        draw_name2(c, cx + 10, cy + 118, cw - 20, f->name, sel ? C_TEXT : C_DIM);
    }
    if (!g_view_count) {
        char msg[200];
        if (g_fig_count) snprintf(msg, sizeof(msg), "%s", T(S_NO_MATCH));
        else snprintf(msg, sizeof(msg), T(S_NO_FIGURES_DIR), GP_FIGURE_DIR);
        cv_text(c, GP_X + 40, gy + 40, msg, C_TEXT, gw - 80);
    }

    int fy = gy + ROWS * ch + 8;
    cv_fill(c, GP_X + 20, fy, gw - 40, 1, C_LINE);
    if (g_view_count) {
        figure *f = &g_figs[g_view[g_sel]];
        char line[200];
        snprintf(line, sizeof(line), "%s%s%s", f->name, f->chase ? "   " : "", f->chase ? T(S_CHASE_TAG) : "");
        cv_text(c, GP_X + 24, fy + 12, line, C_TEXT, gw - 48);
    }
    int hx = hint(c, GP_X + 24, fy + 18 + th, 'X', T(S_PLACE));
    hx = hint(c, hx, fy + 18 + th, 'T', T(g_hide_chase ? S_CHASE_SHOW : S_CHASE_HIDE));
    hx = hint(c, hx, fy + 18 + th, 'O', T(S_BACK));
    hx = badge(c, hx, fy + 18 + th, "L2") + 6;
    hx = badge(c, hx, fy + 18 + th, "R2") + 8;
    cv_text(c, hx, fy + 18 + th, T(S_PAGE), C_DIM, 0);
    char pg[32];
    int pages = (g_view_count + PER_PAGE - 1) / PER_PAGE;
    snprintf(pg, sizeof(pg), T(S_PAGE_N), page + 1, pages ? pages : 1);
    cv_text(c, c->w - 24 - text_width(pg), fy + 18 + th, pg, C_GOLD, 0);
}

#ifdef MENU_PREVIEW
// PC preview only: put the grid cursor on the first figure whose path contains s.
int menu_debug_select(const char *s)
{
    for (int k = 0; k < g_view_count; k++)
        if (strstr(g_figs[g_view[k]].path, s)) { g_sel = k; g_dirty = 1; return k; }
    return -1;
}
#endif

int menu_render(canvas *c)
{
    // Slot contents change when the PC app or the game acts too: redraw if they did.
    static portal_slot_info last[MENU_SLOTS];
    portal_slot_info info[MENU_SLOTS];
    for (int s = 0; s < MENU_SLOTS; s++) {
        portal_get_info(s, &info[s]);
        info[s].dirty = 0;
        if (memcmp(&info[s], &last[s], sizeof(info[s])) != 0) g_dirty = 1;
    }
    static int last_prog = -2;
    int prog = iconx_progress();
    if (prog != last_prog) { last_prog = prog; g_dirty = 1; }
    if (!g_dirty) return 0;
    memcpy(last, info, sizeof(last));
    g_dirty = 0;

    cv_clear(c);
    draw_slots(c, info);
    if (g_grid) draw_grid(c);
    return 1;
}
