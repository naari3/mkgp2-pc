/* The port's menu drawn by the game itself (port/platform/ui/gamemenu.c,
 * docs/port/game_ui.md): what the menu shows is asked from whoever has one
 * (port/gfx/port_menu.cpp), the drawing is the game's fonts and panels. */
#ifndef MKGP2PC_GAMEMENU_H
#define MKGP2PC_GAMEMENU_H

#ifdef __cplusplus
extern "C" {
#endif

#define MKGP2PC_MENU_ROWS 16

typedef struct Mkgp2pcMenuView {
  const char* title;  /* UTF-8, like all texts here */
  const char* note;   /* about the row under the cursor; may be empty */
  const char* footer; /* the keys */
  int count, cursor;
  const char* text[MKGP2PC_MENU_ROWS];
  int kind[MKGP2PC_MENU_ROWS]; /* 0 a choice, 1 one that cannot be taken, 2 a heading */
  const char* value[MKGP2PC_MENU_ROWS]; /* NULL, or the text at the right of the row (a setting's value) */
  int more_above, more_below;           /* the rows are a part of a longer list that goes on there */
} Mkgp2pcMenuView;

/* Set by the program that has a menu. view: nonzero while the menu is open
 * and the game is to draw it, and fills in what it shows. backdrop: draws
 * the stopped game's last picture over the whole frame. */
extern int (*mkgp2pc_gamemenu_view)(Mkgp2pcMenuView* view);
extern void (*mkgp2pc_gamemenu_backdrop)(void);

/* From the top of the game's main loop: draws one frame of the menu and
 * returns nonzero while the game has to stand still. */
int mkgp2pc_gamemenu_frame(void);

#ifdef __cplusplus
}
#endif

#endif
