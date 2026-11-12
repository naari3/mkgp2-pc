#ifndef MKGP2PC_NAV_H
#define MKGP2PC_NAV_H

/*
 * Requests to go to another flow or one selection screen back
 * (port/platform/nav/nav.c). The menu, the keys and the script make them;
 * the game's loops take them through the two hooks at the end.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* The game's top-level flows, by their place in its table
 * (main_rodata_80328C94; the names in quotes are the game's own). */
enum {
  MKGP2PC_FLOW_MENU = 3,       /* "kart2": attract loop, coin, the selection screens */
  MKGP2PC_FLOW_RACE = 4,       /* "GameMain": the race of the current choices */
  MKGP2PC_FLOW_SPRITE_VIEW = 16,
  MKGP2PC_FLOW_FLASH_VIEW = 17,
};

/* Where the menu flow begins (the game's own three "continue" flags). */
enum {
  MKGP2PC_ENTRY_TITLE = 0,     /* as after a finished play: attract loop */
  MKGP2PC_ENTRY_CLASS = 1,     /* class selection, as after "continue" */
  MKGP2PC_ENTRY_CUP = 2,       /* cup selection, or round selection while a cup is being played */
  MKGP2PC_ENTRY_CHARACTER = 3, /* character selection */
};

/* The selection screens, by the numbers MenuScene_Dispatch switches on. */
enum {
  MKGP2PC_SCENE_ENTRY = 2,
  MKGP2PC_SCENE_CLASS = 11,
  MKGP2PC_SCENE_CHARACTER = 13,
  MKGP2PC_SCENE_KART = 15,
  MKGP2PC_SCENE_CUP = 21,
  MKGP2PC_SCENE_COURSE = 23,
  MKGP2PC_SCENE_ROUND = 27,
  MKGP2PC_SCENE_ITEM = 33,
  MKGP2PC_SCENE_GP_PLEASE = 37,
  MKGP2PC_SCENE_TA_PLEASE = 38,
  MKGP2PC_SCENE_DONE = 42,     /* the flow ends, the race follows */
};

void mkgp2pc_nav_request_flow(int flow, int entry);
int mkgp2pc_nav_flow_pending(void);
int mkgp2pc_nav_current_flow(void);  /* -1 before the main loop */
unsigned mkgp2pc_nav_flow_ticks(void); /* advances while the main loop runs */

void mkgp2pc_nav_request_back(void);
int mkgp2pc_nav_can_back(void);
int mkgp2pc_nav_current_scene(void);

/* Called by the patched game sources. */
int mkgp2pc_nav_flow_hook(int current, int* entry); /* -2: nothing wanted */
int mkgp2pc_nav_scene_hook(int next);
void mkgp2pc_nav_scene_begin(int scene);

#ifdef __cplusplus
}
#endif

#endif
