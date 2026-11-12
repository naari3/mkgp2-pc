/* The port's menu (Esc) drawn with the game's own fonts and panels.
 *
 * The game stands still while the menu is open, so nothing of a scene runs
 * that could queue the menu's sprites. Instead the game's main loop calls
 * mkgp2pc_gamemenu_frame() at the top of every pass
 * (port/patches/src/game/MainGameLoop.cpp.patch) and stays there while it
 * returns nonzero. One such frame is: the game's own start of a frame, the
 * stopped game's last picture (the copy the game takes of every frame for
 * its screen transitions), the menu queued and drawn through the game's
 * sprite system (gameui.c), and the game's own end of a frame. No scene is
 * updated or drawn, and no game time passes.
 *
 * Before the main loop runs (the boot checks) and in loops of the game's own
 * (test mode) nobody calls this; the menu is then drawn the other way
 * (port/gfx/port_ui.cpp). */
#include <stddef.h>

#include "mkgp2pc/gamemenu.h"
#include "mkgp2pc/gameui.h"

int (*mkgp2pc_gamemenu_view)(Mkgp2pcMenuView* view);
void (*mkgp2pc_gamemenu_backdrop)(void);

void HSD_StartRender(int pass);     /* the game's start of a frame */
void Frame_PostDrawOverlay(void);   /* its end: copy to the display, wait for the retrace */

static int s_up;
static int s_age;

int mkgp2pc_gamemenu_frame(void) {
  Mkgp2pcMenuView v;
  if (mkgp2pc_gamemenu_view == NULL || !mkgp2pc_gamemenu_view(&v)) {
    if (s_up) {
      mkgp2pc_gameui_overlay_end();
      s_up = 0;
    }
    return 0;
  }
  if (!s_up) {
    mkgp2pc_gameui_overlay_begin();
    s_up = 1;
    s_age = 0;
  }
  HSD_StartRender(0);
  if (mkgp2pc_gamemenu_backdrop != NULL) {
    mkgp2pc_gamemenu_backdrop();
  }
  mkgp2pc_gameui_list(v.title, v.text, v.kind, v.value, v.count, v.cursor, v.more_above, v.more_below, v.note, v.footer,
                      s_age++);
  mkgp2pc_gameui_overlay_draw();
  Frame_PostDrawOverlay();
  return 1;
}
