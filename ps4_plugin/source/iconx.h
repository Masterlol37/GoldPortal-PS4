// Figure icons straight from the running game ("dump on first start").
//
// The plugin knows which icon name belongs to which figure (icon_map.h, names only). At game
// start it checks /data/goldportal/icons/index.txt: if there is no pack yet, or the pack comes
// from an older game than this one, the icons are read out of this game's own archives
// (/app0/archives) in the background and the pack is replaced. A pack from a newer game is
// kept, since newer games know more figures.
#pragma once

// year: the game's year code (2 Swap Force ... 5 Imaginators)
void iconx_start(const char *title_id, int year);
int  iconx_progress(void);        // 0..100 while extracting, -1 otherwise
int  iconx_take_changed(void);    // 1 once after a new pack was written
void iconx_stop(void);

// For the PC test: extract from <archives dir> into <out dir> right away (no thread).
int  iconx_extract_now(const char *archives_dir, const char *out_dir, const char *title_id, int year);
