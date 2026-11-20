// UI texts of the on-screen menu in the console's system language.
// The font covers Latin-1 only, so languages that need other scripts fall back to English.
#pragma once

typedef enum {
    S_READY, S_EMPTY, S_SLOT_N, S_TRAP_SLOT, S_VEHICLE_SLOT, S_TOPS, S_BOTTOMS,
    S_PICK_RIGHT, S_SELECT, S_REMOVE, S_RESCAN, S_CLOSE,
    S_FIG_FOR_SLOT, S_TRAP_FOR_SLOT, S_VEHICLE_FOR_SLOT, S_SWAP_FOR_SLOT, S_TOP, S_BOTTOM,
    S_COUNT_CHASE, S_ON, S_OFF, S_GAME, S_KIND, S_ALL,
    S_CAT_FIGURES, S_CAT_ITEMS, S_CAT_TRAPS, S_CAT_VEHICLES, S_CAT_CRYSTALS, S_SWAP_PARTS,
    S_NO_MATCH, S_NO_FIGURES_DIR, S_CHASE_TAG, S_PLACE, S_CHASE_SHOW, S_CHASE_HIDE, S_BACK,
    S_PAGE, S_PAGE_N, S_CANT_READ, S_PLACED, S_SLOT_FREE, S_FOUND_N, S_CHASE_HIDDEN, S_CHASE_SHOWN,
    S_SWAP_NEXT, S_A_BOTTOM, S_A_TOP, S_NO_FIGURES_IN, S_ICONS_LOADING, S_ICONS_FROM,
    S_COUNT_
} str_id;

void lang_init(int system_language);   // SceSystemServiceParamId LANG value, -1 = unknown
const char *T(str_id id);
const char *lang_code(void);
