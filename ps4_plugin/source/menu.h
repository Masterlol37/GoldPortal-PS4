// On-screen figure menu: library of .dump files on the PS4, icons, portal slots.
#pragma once
#include <stdint.h>
#include "gfx.h"

#define MENU_W 1840      // canvas size at 1080p (centred); doubled on a 4K buffer
#define MENU_H 960

#ifndef GP_FIGURE_DIR
#define GP_FIGURE_DIR "/data/goldportal/figures"
#endif
#ifndef GP_MENU_CFG
#define GP_MENU_CFG   "/data/goldportal/menu.cfg"
#endif
#ifndef GP_ICON_DIR
#define GP_ICON_DIR   "/data/goldportal/icons"
#endif

void menu_init(const char *title_id);
int  menu_year(void);                 // year code of the running game (2 Swap Force ... 5 Imaginators)
void menu_reload_icons(void);         // re-read the icon pack (after iconx wrote a new one)
void menu_preload(void);              // icon index + library scan (background, optional)
void menu_open(void);                 // (re)scans the library the first time
// Buttons that went down this frame (ORBIS_PAD_BUTTON_*), plus repeats while held.
// Returns 1 if the menu wants to close.
int  menu_buttons(uint32_t pressed);
// Redraws the canvas if something changed. Returns 1 if it did.
int  menu_render(canvas *c);
// Autosave of figures placed from the menu; call about once a second.
void menu_tick(void);
// Save every dirty menu-placed figure now (menu closed, plugin unloading).
void menu_save_all(void);
