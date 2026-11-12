/* Port screens drawn with the game's own text and sprites
 * (port/platform/ui/gameui.c, docs/port/game_ui.md). */
#ifndef MKGP2PC_GAMEUI_H
#define MKGP2PC_GAMEUI_H

#ifdef __cplusplus
extern "C" {
#endif

enum { GAMEUI_FONT_16, GAMEUI_FONT_24, GAMEUI_FONT_64 };
enum { GAMEUI_LEFT, GAMEUI_CENTER, GAMEUI_RIGHT };

void mkgp2pc_gameui_begin(int layer);
void mkgp2pc_gameui_end(void);
int mkgp2pc_gameui_is_open(void);
int mkgp2pc_gameui_english(void);

void mkgp2pc_gameui_sprite(int res, float x, float y, float scaleX, float scaleY, unsigned int rgba);
void mkgp2pc_gameui_fill(float x, float y, float width, float height, unsigned int rgba);
void mkgp2pc_gameui_dim(unsigned int alpha);
void mkgp2pc_gameui_panel(float x, float y, float width, float height, unsigned int rgba);
void mkgp2pc_gameui_window(float x, float y, float width, float height);
void mkgp2pc_gameui_glow(float x, float y, float width, float height, unsigned int rgba);
void mkgp2pc_gameui_frame(int res, float w, float h, float edge, int mirror, float x, float y, float width,
                          float height, unsigned int rgba);
void mkgp2pc_gameui_sheet(float x, float y, float width, float height);
void mkgp2pc_gameui_white_sheet(float x, float y, float width, float height);
void mkgp2pc_gameui_card_sheet(float x, float y, int squares, float scale, unsigned int rgba);
void mkgp2pc_gameui_cursor(float x, float y, float width, float height, int t);

float mkgp2pc_gameui_text_width(int font, float scale, const char* utf8);
void mkgp2pc_gameui_text(int font, int align, float x, float y, float scale, unsigned int rgba, const char* utf8);

void mkgp2pc_gameui_hint_select(float x, float y, int t);
void mkgp2pc_gameui_hint_decide(float x, float y, int t);
void mkgp2pc_gameui_hint_button(float x, float y, int t, const char* caption);

void mkgp2pc_gameui_choice(const char* title, const char* const* items, int count, int cursor, int age);
void mkgp2pc_gameui_list(const char* title, const char* const* rows, const int* kinds, const char* const* values,
                         int count, int cursor, int more_above, int more_below, const char* note,
                         const char* footer, int age);

/* A screen over a game that stands still (the port's menu, gamemenu.c): the
 * caller makes the frame and draws what lies underneath; between begin and
 * end the functions above queue for the overlay, and overlay_draw draws
 * what was queued. The stopped scene's own queued sprites are put aside and
 * come back with overlay_end. */
void mkgp2pc_gameui_overlay_begin(void);
void mkgp2pc_gameui_overlay_draw(void);
void mkgp2pc_gameui_overlay_end(void);

#ifdef __cplusplus
}
#endif

#endif
