/*
 * Settings changed from the port's menu while the game runs (設定 in the Esc
 * menu, port_menu.cpp; docs/port/menu.md).
 *
 * The game program takes its settings from environment variables; the
 * settings program (port/launcher) keeps them in mkgp2-pc.ini and names
 * that file to the game as MKGP2PC_INI. A change made in the menu is
 * written into that file, so that the settings program and the next start
 * have it, and is applied at once where the running game can take it.
 * Without MKGP2PC_INI nothing is written: changes last until the game ends.
 */
#ifndef MKGP2PC_PORT_SETTINGS_H
#define MKGP2PC_PORT_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the file (port_settings.cpp) ---- */

/* nonzero when changes are kept (MKGP2PC_INI names the file) */
int mkgp2pc_settings_can_save(void);
/* [settings] key=value; the settings program's names (port/launcher/launcher.c) */
void mkgp2pc_settings_save(const char* key, const char* value);
void mkgp2pc_settings_save_int(const char* key, int value);
/* count lines key=value of one section in one write */
void mkgp2pc_settings_save_section(const char* section, const char* const* keys, const char* const* values, int count);

/* ---- a row of another module on the グラフィック page ---- */

typedef struct Mkgp2pcSettingRow {
  const char* en; /* the label */
  const char* ja; /* UTF-8; only letters the game's font has (docs/port/game_ui.md) */
  int count;      /* choices */
  const char* const* choices_en;
  const char* const* choices_ja;
  int (*get)(void);        /* the choice in force, 0 .. count - 1 */
  void (*set)(int choice); /* applies it (and saves it, mkgp2pc_settings_save) */
  int (*enabled)(void);    /* NULL: always. 0: the row is shown grey and cannot be changed */
  int restart;             /* nonzero: takes effect at the next start; the row's note says so */
} Mkgp2pcSettingRow;

/* The row has to stay alive. Before the page is first opened. */
void mkgp2pc_settings_add_graphics(const Mkgp2pcSettingRow* row);
int mkgp2pc_settings_graphics_rows(const Mkgp2pcSettingRow* const** rows);

/* ---- what the menu changes in the running game ---- */

/* video_aurora.c: the key table. An action has a name (MKGP2PC_KEYS) and up
 * to four keys, of which the menu and the settings program handle two. */
int mkgp2pc_keys_actions(void);
const char* mkgp2pc_keys_action_name(int action);
int mkgp2pc_keys_get(int action, int slot);
/* scancode 0 takes the key away; a key given to one action is taken from
 * the action that had it */
void mkgp2pc_keys_set(int action, int slot, int scancode);
void mkgp2pc_keys_reset(void);
/* The next key pressed goes to that action (Esc: none). While this waits
 * the menu gets no keys. */
void mkgp2pc_keys_capture(int action, int slot);
int mkgp2pc_keys_capturing(void);
/* called when a captured key was stored */
extern void (*mkgp2pc_keys_changed)(void);
/* draws wait for their pipeline around loading (0) or never (1) */
int mkgp2pc_video_pipeline_async(void);
void mkgp2pc_video_pipeline_async_set(int async);

/* gx_shim.cpp */
int mkgp2pc_hires_on(void);
void mkgp2pc_hires_set(int on);
int mkgp2pc_shadow_smooth(void);
void mkgp2pc_shadow_smooth_set(int on);

#ifdef __cplusplus
}
#endif

#endif
