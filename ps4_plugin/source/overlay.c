// On-screen figure menu.
//
// Display: sceVideoOutRegisterBuffers is hooked to learn the game's display buffers
// (address, size, pixel format, tiling). A present thread is woken by a flip event of its
// own and, right after each flip, copies the pre-drawn menu into the buffer now on screen,
// top to bottom and faster than scan-out, so the menu is there before its lines are shown.
// This works the same whether the game flips with sceVideoOutSubmitFlip or through GNM.
// The menu thread handles input and redraws into a second canvas, so redrawing (and loading
// icons) never makes the menu miss a frame.
//
// Input: scePadRead / scePadReadState are hooked. L3+R3 opens and closes the menu; while it
// is open the game gets neutral pad data (no buttons, centred sticks).
#include "overlay.h"
#include "menu.h"
#include "gfx.h"
#include "log.h"
#include "lang.h"
#include "iconx.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <orbis/libkernel.h>
#include <orbis/_types/video.h>
#include <orbis/_types/pad.h>

#define OPEN_COMBO (ORBIS_PAD_BUTTON_L3 | ORBIS_PAD_BUTTON_R3)
#define MAX_BUFFERS 16

typedef int32_t (*RegisterBuffers_t)(int32_t, int32_t, void *const *, int32_t, const OrbisVideoOutBufferAttribute *);
typedef int32_t (*GetFlipStatus_t)(int32_t, OrbisVideoOutFlipStatus *);
typedef int32_t (*AddFlipEvent_t)(OrbisKernelEqueue, int32_t, void *);
typedef int32_t (*PadReadState_t)(int32_t, OrbisPadData *);
typedef int32_t (*PadRead_t)(int32_t, OrbisPadData *, int32_t);

static hook_rec g_h_register, g_h_padstate, g_h_padread;
static int      g_video_hooked, g_pad_hooked;
static GetFlipStatus_t g_GetFlipStatus;
static AddFlipEvent_t  g_AddFlipEvent;
static int      g_disabled;                  // /data/goldportal/nooverlay or a fatal problem

// Display buffers (last registration wins)
static volatile int g_vo_handle = -1;
static void    *g_buf[MAX_BUFFERS];
static int      g_buf_writable[MAX_BUFFERS];   // 0 unknown, 1 ok, -1 not writable
static OrbisVideoOutBufferAttribute g_attr;
static int      g_buf_count;
static pthread_mutex_t g_fb_lock = PTHREAD_MUTEX_INITIALIZER;

// Input
static volatile int g_open;
static volatile int g_wait_release;          // swallow input until all buttons are up
static volatile uint32_t g_held;             // buttons held (any pad) while the menu is open
static pthread_mutex_t g_in_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_pressed;                   // edges not yet handled by the overlay thread
static int      g_toggle_req;
static int      g_stop;

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------
static int32_t h_RegisterBuffers(int32_t handle, int32_t start, void *const *addrs, int32_t num,
                                 const OrbisVideoOutBufferAttribute *attr)
{
    int32_t r = ((RegisterBuffers_t)g_h_register.orig)(handle, start, addrs, num, attr);
    if (r >= 0 && attr && addrs) {
        pthread_mutex_lock(&g_fb_lock);
        g_vo_handle = handle;
        g_attr = *attr;
        for (int i = 0; i < num && start + i < MAX_BUFFERS; i++) {
            g_buf[start + i] = addrs[i];
            g_buf_writable[start + i] = 0;
        }
        if (start + num > g_buf_count) g_buf_count = start + num > MAX_BUFFERS ? MAX_BUFFERS : start + num;
        pthread_mutex_unlock(&g_fb_lock);
        LOG("overlay: game registered %d display buffers (handle %d, start %d): %ux%u pitch %u, "
            "format 0x%08X (%s), tiling %d (%s), first at %p",
            num, handle, start, attr->width, attr->height, attr->pixelPitch, (unsigned)attr->format,
            fb_format_name((uint32_t)attr->format), attr->tmode, attr->tmode == 0 ? "TILE" : "LINEAR",
            num > 0 ? addrs[0] : NULL);
    } else {
        LOG("overlay: sceVideoOutRegisterBuffers returned 0x%08X", (unsigned)r);
    }
    return r;
}

// Watches one pad sample. Returns 1 if the game must get neutral data for it.
static int pad_filter(int32_t handle, OrbisPadData *d)
{
    static uint32_t prev[8];
    uint32_t b = d->buttons, p = prev[handle & 7];
    prev[handle & 7] = b;
    uint32_t down = b & ~p;

    if (g_wait_release) {
        if (b == 0) g_wait_release = 0;
        return 1;
    }
    if ((b & OPEN_COMBO) == OPEN_COMBO && (down & OPEN_COMBO)) {
        pthread_mutex_lock(&g_in_lock);
        g_toggle_req = 1;
        pthread_mutex_unlock(&g_in_lock);
        g_wait_release = 1;
        return 1;
    }
    if (!g_open) return 0;
    g_held = b;
    if (down) {
        pthread_mutex_lock(&g_in_lock);
        g_pressed |= down;
        pthread_mutex_unlock(&g_in_lock);
    }
    return 1;
}

static void neutral(OrbisPadData *d)
{
    d->buttons = 0;
    d->leftStick.x = d->leftStick.y = 0x80;
    d->rightStick.x = d->rightStick.y = 0x80;
    d->analogButtons.l2 = d->analogButtons.r2 = 0;
    d->touch.fingers = 0;
}

static int32_t h_PadReadState(int32_t handle, OrbisPadData *d)
{
    int32_t r = ((PadReadState_t)g_h_padstate.orig)(handle, d);
    if (r == 0 && d && pad_filter(handle, d)) neutral(d);
    return r;
}

static int32_t h_PadRead(int32_t handle, OrbisPadData *d, int32_t count)
{
    int32_t r = ((PadRead_t)g_h_padread.orig)(handle, d, count);
    if (r > 0 && d) {
        int swallow = 0;
        for (int i = 0; i < r; i++) swallow |= pad_filter(handle, &d[i]);
        if (swallow)
            for (int i = 0; i < r; i++) neutral(&d[i]);
    }
    return r;
}

static void *dlsym_in(int mod, const char *sym)
{
    void *a = NULL;
    if (mod < 0 || sceKernelDlsym(mod, sym, &a) != 0) return NULL;
    return a;
}

int overlay_try_hook(void)
{
    if (g_disabled) return 1;
    if (!g_video_hooked) {
        int mod = gp_find_module("libSceVideoOut.sprx");
        if (mod >= 0) {
            g_video_hooked = 1;
            g_GetFlipStatus = (GetFlipStatus_t)dlsym_in(mod, "sceVideoOutGetFlipStatus");
            g_AddFlipEvent = (AddFlipEvent_t)dlsym_in(mod, "sceVideoOutAddFlipEvent");
            void *reg = dlsym_in(mod, "sceVideoOutRegisterBuffers");
            LOG("overlay: libSceVideoOut 0x%x, RegisterBuffers %p, GetFlipStatus %p", mod, reg, (void *)g_GetFlipStatus);
            if (reg) gp_install_hook("sceVideoOutRegisterBuffers", &g_h_register, reg, (void *)h_RegisterBuffers);
        }
    }
    if (!g_pad_hooked) {
        int mod = gp_find_module("libScePad.sprx");
        if (mod >= 0) {
            g_pad_hooked = 1;
            void *rs = dlsym_in(mod, "scePadReadState"), *rd = dlsym_in(mod, "scePadRead");
            LOG("overlay: libScePad 0x%x, ReadState %p, Read %p", mod, rs, rd);
            if (rs) gp_install_hook("scePadReadState", &g_h_padstate, rs, (void *)h_PadReadState);
            if (rd) gp_install_hook("scePadRead", &g_h_padread, rd, (void *)h_PadRead);
        }
    }
    return g_video_hooked && g_pad_hooked;
}

// ---------------------------------------------------------------------------
// Drawing thread
// ---------------------------------------------------------------------------
static int ensure_writable(int idx, size_t size)
{
    if (g_buf_writable[idx]) return g_buf_writable[idx] > 0;
    void *start = NULL, *end = NULL;
    int prot = 0;
    int r = sceKernelQueryMemoryProtection(g_buf[idx], &start, &end, &prot);
    LOG("overlay: buffer %d at %p: query 0x%X, range %p-%p, prot 0x%X", idx, g_buf[idx], (unsigned)r, start, end, prot);
    if (r == 0 && !(prot & 0x2)) {
        r = sceKernelMprotect(g_buf[idx], size, prot | 0x3);
        LOG("overlay: buffer %d not CPU-writable, mprotect -> 0x%X", idx, (unsigned)r);
        if (r != 0) { g_buf_writable[idx] = -1; return 0; }
    }
    g_buf_writable[idx] = 1;
    return 1;
}

// Two canvases: the menu thread draws into the back one (that can take a while, e.g.
// loading icons from disk) and swaps; the present thread always has a complete front one.
static canvas g_cv[2];
static canvas *volatile g_front;
static volatile int g_front_ready;             // front holds the current menu (0 right after opening)
static pthread_mutex_t g_cv_lock = PTHREAD_MUTEX_INITIALIZER;

static void present_once(int idx, int *logged, uint64_t *pres_time, uint64_t *pres_count)
{
    pthread_mutex_lock(&g_fb_lock);
    if (idx >= 0 && idx < g_buf_count && g_buf[idx]) {
        framebuffer fb = {
            .base = g_buf[idx], .width = (int)g_attr.width, .height = (int)g_attr.height,
            .pitch = (int)(g_attr.pixelPitch ? g_attr.pixelPitch : g_attr.width),
            .format = (uint32_t)g_attr.format, .tiled = g_attr.tmode == 0,
        };
        int scale = fb.width >= MENU_W * 2 + 80 ? 2 : 1;
        int x = (fb.width - MENU_W * scale) / 2;
        int y = (fb.height - MENU_H * scale) / 2;
        if (ensure_writable(idx, (size_t)fb.pitch * fb.height * 4)) {
            uint64_t t0 = sceKernelGetProcessTime();
            pthread_mutex_lock(&g_cv_lock);
            fb_present(&fb, g_front, x, y, scale);
            pthread_mutex_unlock(&g_cv_lock);
            *pres_time += sceKernelGetProcessTime() - t0;
            if (!(*logged)++)
                LOG("overlay: first frame drawn into buffer %d at %d,%d (scale %d)", idx, x, y, scale);
            if (++*pres_count == 120)
                LOG("overlay: drawing the menu takes %llu us per frame",
                    (unsigned long long)(*pres_time / *pres_count));
        }
    }
    pthread_mutex_unlock(&g_fb_lock);
}

// Puts the menu into each new frame. Woken by the system's flip event, so the menu is
// in the buffer before scan-out reaches its first line (falls back to fast polling).
static void *present_thread(void *arg)
{
    (void)arg;
    OrbisKernelEqueue eq = 0;
    int have_eq = sceKernelCreateEqueue(&eq, "gp_flip") == 0, eq_handle = -1;
    uint64_t last_flip = (uint64_t)-1, pres_time = 0, pres_count = 0;
    int logged = 0;
    while (!g_stop) {
        if (!g_open || !g_front_ready || !g_GetFlipStatus || g_vo_handle < 0) {
            usleep(10000);
            last_flip = (uint64_t)-1;
            continue;
        }
        if (have_eq && eq_handle != g_vo_handle && g_AddFlipEvent) {
            int r = g_AddFlipEvent(eq, g_vo_handle, NULL);
            LOG("overlay: flip event for handle %d -> 0x%X", g_vo_handle, (unsigned)r);
            if (r == 0) eq_handle = g_vo_handle; else have_eq = 0;
        }
        if (have_eq && eq_handle >= 0) {
            OrbisKernelEvent ev[4];
            int n = 0;
            OrbisKernelUseconds to = 100000;
            sceKernelWaitEqueue(eq, ev, 4, &n, &to);
        } else {
            usleep(250);
        }
        OrbisVideoOutFlipStatus st;
        memset(&st, 0, sizeof(st));
        if (g_GetFlipStatus(g_vo_handle, &st) != 0 || st.num == last_flip) continue;
        last_flip = st.num;
        if (g_open && g_front_ready) present_once(st.currentBuffer, &logged, &pres_time, &pres_count);
    }
    if (have_eq) sceKernelDeleteEqueue(eq);
    return NULL;
}

// Menu texts in the console's language (SceSystemService LANG parameter). Returns 1 when set.
static int set_language(void)
{
    typedef int32_t (*ParamGetInt_t)(int32_t, int32_t *);
    int mod = gp_find_module("libSceSystemService.sprx");
    ParamGetInt_t get = (ParamGetInt_t)dlsym_in(mod, "sceSystemServiceParamGetInt");
    int32_t lang = -1;
    if (!get || get(1 /* LANG */, &lang) != 0) return 0;
    lang_init(lang);
    LOG("overlay: system language %d -> menu texts '%s'", lang, lang_code());
    return 1;
}

static void *overlay_thread(void *arg)
{
    (void)arg;
    if (cv_init(&g_cv[0], MENU_W, MENU_H) != 0) {
        LOG("overlay: out of memory for the menu canvas");
        return NULL;
    }
    int doubled = cv_init(&g_cv[1], MENU_W, MENU_H) == 0;
    LOG("overlay: menu canvases: %s, %s", g_cv[0].mapped ? "mmap" : "malloc",
        doubled ? (g_cv[1].mapped ? "mmap" : "malloc") : "none");
    if (!doubled) LOG("overlay: only one menu canvas (low memory) - may blink while redrawing");
    g_front = &g_cv[0];
    canvas *back = doubled ? &g_cv[1] : &g_cv[0];

    pthread_t pt;
    pthread_attr_t pa;
    pthread_attr_init(&pa);
    pthread_attr_setstacksize(&pa, 64 * 1024);
    if (pthread_create(&pt, &pa, present_thread, NULL) == 0) pthread_detach(pt);
    pthread_attr_destroy(&pa);

    uint64_t last_tick = 0, rep_next = 0;
    uint32_t rep_btn = 0;
    int warned = 0;
    int lang_done = set_language();

    // Read the icon index and the figure library in the background, so the first
    // L3+R3 opens the menu at once instead of after a multi-second scan.
    for (int i = 0; i < 100 && !g_stop && !g_toggle_req; i++) usleep(100000);
    if (!lang_done) lang_done = set_language();
    if (!g_stop) menu_preload();

    while (!g_stop) {
        uint64_t now = sceKernelGetProcessTime();
        if (now - last_tick > 1000000) {
            last_tick = now;
            menu_tick();
            if (iconx_take_changed()) menu_reload_icons();
        }

        // Input from the pad hooks
        pthread_mutex_lock(&g_in_lock);
        int toggle = g_toggle_req;
        uint32_t pressed = g_pressed;
        g_toggle_req = 0;
        g_pressed = 0;
        pthread_mutex_unlock(&g_in_lock);
        if (toggle) {
            if (!g_open) {
                if (g_vo_handle < 0 || !g_buf_count) {
                    if (!warned++) LOG("overlay: L3+R3 pressed, but the display buffers are unknown (registered before the hook?)");
                } else {
                    menu_open();
                    g_front_ready = 0;
                    g_open = 1;
                    // Loading can take seconds the first time; a second L3+R3 pressed
                    // meanwhile ("nothing happens") must not close the menu right away.
                    pthread_mutex_lock(&g_in_lock);
                    g_toggle_req = 0;
                    g_pressed = 0;
                    pthread_mutex_unlock(&g_in_lock);
                    pressed = 0;
                    LOG("overlay: menu opened");
                }
            } else {
                g_open = 0;
                menu_save_all();
                LOG("overlay: menu closed");
            }
        }
        if (g_open) {
            // Auto-repeat for d-pad and L2/R2 while held
            uint32_t rep_mask = ORBIS_PAD_BUTTON_UP | ORBIS_PAD_BUTTON_DOWN | ORBIS_PAD_BUTTON_LEFT |
                                ORBIS_PAD_BUTTON_RIGHT | ORBIS_PAD_BUTTON_L2 | ORBIS_PAD_BUTTON_R2;
            uint32_t held = g_held & rep_mask;
            if (pressed & rep_mask) { rep_btn = pressed & rep_mask; rep_next = now + 400000; }
            else if (held && held == rep_btn && now >= rep_next) { pressed |= held; rep_next = now + 110000; }
            else if (!held) rep_btn = 0;
            if (pressed && menu_buttons(pressed)) {
                g_open = 0;
                g_wait_release = 1;
                menu_save_all();
                LOG("overlay: menu closed");
            }
        }
        if (!g_open) { usleep(16000); continue; }

        // Redraw into the back canvas, then make it the front one.
        // (menu_render always draws the whole picture, so a stale back canvas is fine.)
        if (doubled) {
            if (menu_render(back)) {
                pthread_mutex_lock(&g_cv_lock);
                canvas *t = g_front; g_front = back; back = t;
                pthread_mutex_unlock(&g_cv_lock);
                g_front_ready = 1;
            }
        } else {
            pthread_mutex_lock(&g_cv_lock);
            menu_render(back);
            pthread_mutex_unlock(&g_cv_lock);
            g_front_ready = 1;
        }
        usleep(4000);
    }
    return NULL;
}

void overlay_init(const char *title_id)
{
    if (file_exists(GP_DATA_DIR "/nooverlay")) {
        g_disabled = 1;
        LOG("overlay: disabled (/data/goldportal/nooverlay exists)");
        return;
    }
    menu_init(title_id);
    iconx_start(title_id, menu_year());
    overlay_try_hook();
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    if (pthread_create(&th, &attr, overlay_thread, NULL) == 0)
        pthread_detach(th);
    pthread_attr_destroy(&attr);
}

void overlay_stop(void)
{
    g_stop = 1;
    iconx_stop();
    g_open = 0;
    if (g_h_register.orig) hook_remove(&g_h_register);
    if (g_h_padstate.orig) hook_remove(&g_h_padstate);
    if (g_h_padread.orig) hook_remove(&g_h_padread);
    menu_save_all();
}

void overlay_info_json(char *out, size_t size)
{
    pthread_mutex_lock(&g_fb_lock);
    snprintf(out, size,
             "{\"disabled\":%d,\"video_hooked\":%d,\"pad_hooked\":%d,\"open\":%d,\"handle\":%d,"
             "\"buffers\":%d,\"width\":%u,\"height\":%u,\"pitch\":%u,\"format\":\"0x%08X\","
             "\"format_name\":\"%s\",\"tiling\":%d}",
             g_disabled, g_h_register.orig != NULL, g_h_padstate.orig != NULL || g_h_padread.orig != NULL,
             g_open, g_vo_handle, g_buf_count, g_attr.width, g_attr.height, g_attr.pixelPitch,
             (unsigned)g_attr.format, fb_format_name((uint32_t)g_attr.format), g_attr.tmode);
    pthread_mutex_unlock(&g_fb_lock);
}

int overlay_fb_snapshot(uint8_t **data, size_t *len)
{
    pthread_mutex_lock(&g_fb_lock);
    int ok = -1;
    OrbisVideoOutFlipStatus st;
    memset(&st, 0, sizeof(st));
    if (g_GetFlipStatus && g_vo_handle >= 0 && g_GetFlipStatus(g_vo_handle, &st) == 0 &&
        st.currentBuffer >= 0 && st.currentBuffer < g_buf_count && g_buf[st.currentBuffer]) {
        size_t n = (size_t)(g_attr.pixelPitch ? g_attr.pixelPitch : g_attr.width) * g_attr.height * 4;
        if (ensure_writable(st.currentBuffer, n) && (*data = malloc(n)) != NULL) {
            memcpy(*data, g_buf[st.currentBuffer], n);
            *len = n;
            ok = 0;
        }
    }
    pthread_mutex_unlock(&g_fb_lock);
    return ok;
}
