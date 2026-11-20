// On-screen overlay: hooks libSceVideoOut (to learn the display buffers) and libScePad
// (to open the menu with L3+R3 and keep the game from seeing input while it is open).
#pragma once
#include <stddef.h>
#include <stdint.h>

// Call once from the worker thread, before the game sets up its display.
void overlay_init(const char *title_id);
// Hooks the libraries once they are loaded. Returns 1 when everything is hooked.
int  overlay_try_hook(void);
void overlay_stop(void);

// For the web server: state as JSON, and a copy of the buffer on screen (malloc'ed).
void overlay_info_json(char *out, size_t size);
int  overlay_fb_snapshot(uint8_t **data, size_t *len);

// Provided by main.c: crash-guarded hook install and module lookup.
#include "hook.h"
int gp_install_hook(const char *sym, hook_rec *rec, void *addr, void *fn);
int gp_find_module(const char *name);
