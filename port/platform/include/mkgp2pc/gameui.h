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

float mkgp2pc_gameui_text_width(int font, float scale, const char* utf8);
void mkgp2pc_gameui_text(int font, int align, float x, float y, float scale, unsigned int rgba, const char* utf8);

void mkgp2pc_gameui_hint_select(float x, float y, int t);
void mkgp2pc_gameui_hint_decide(float x, float y, int t);
void mkgp2pc_gameui_hint_button(float x, float y, int t, const char* caption);

void mkgp2pc_gameui_choice(const char* title, const char* const* items, int count, int cursor, int age);

#ifdef __cplusplus
}
#endif

#endif
