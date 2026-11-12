/*
 * The port's menu: opened with a key at any time (Esc by default), the game
 * stands still while it is open. Its choices go to the game as requests
 * (port/platform/nav/nav.c) that the game's own loops take up on their next
 * frame; they are written into the input recording like key presses.
 */
#include "port_ui.hpp"

#include <mkgp2pc/nav.h>

#include <cstdio>

extern "C" {
/* video_aurora.c: applies a command of the input script now and records it */
void mkgp2pc_video_command(const char* cmd);
bool mkgp2pc_video_main_loop_runs(void);
void mkgp2pc_video_quit(void);
}

namespace {

using portui::Item;
using portui::Text;

bool g_open;

void flow(int index, int entry) {
  char cmd[40];
  std::snprintf(cmd, sizeof(cmd), "flow=%d,%d", index, entry);
  mkgp2pc_video_command(cmd);
}

bool main_loop() { return mkgp2pc_video_main_loop_runs(); }
bool in_race() { return main_loop() && mkgp2pc_nav_current_flow() == MKGP2PC_FLOW_RACE; }
bool can_back() { return main_loop() && mkgp2pc_nav_can_back(); }

portui::ListMenu& menu() {
  static portui::ListMenu m{
      Text{"MENU", "メニュー"},
      {
          Item{Text{"Resume", "ゲームに戻る"}, [] {}},
          Item{Text{"Back one screen", "一つ前の画面に戻る"}, [] { mkgp2pc_video_command("back"); }, can_back,
               Text{"Selection screens before a race", "レース前の選択画面で使えます"}},
          Item{Text{"Restart this race", "このレースをやり直す"}, [] { flow(MKGP2PC_FLOW_RACE, -1); }, in_race,
               Text{"Starts again from the course introduction", "コース紹介からやり直します"}},
          Item{Text{"Go to", "移動"}, {}},
          Item{Text{"Title", "タイトル"}, [] { flow(MKGP2PC_FLOW_MENU, MKGP2PC_ENTRY_TITLE); }, main_loop,
               Text{"Ends the current play", "いまのプレイを終了します"}},
          Item{Text{"Class selection", "クラス選択"}, [] { flow(MKGP2PC_FLOW_MENU, MKGP2PC_ENTRY_CLASS); }, main_loop,
               Text{"No coin is taken", "コインは消費しません"}},
          Item{Text{"Character selection", "キャラクター選択"}, [] { flow(MKGP2PC_FLOW_MENU, MKGP2PC_ENTRY_CHARACTER); },
               main_loop, Text{"No coin is taken", "コインは消費しません"}},
          Item{Text{"Cup / round selection", "カップ・ラウンド選択"}, [] { flow(MKGP2PC_FLOW_MENU, MKGP2PC_ENTRY_CUP); }, main_loop,
               Text{"No coin is taken", "コインは消費しません"}},
          Item{Text{"", ""}, {}},
          Item{Text{"Quit the game", "ゲームを終了する"}, [] { mkgp2pc_video_quit(); }},
      },
      Text{"Up/Down: choose   Enter: decide   Esc: close", "上下: 選択   Enter: 決定   Esc: 閉じる"}};
  return m;
}

/* The menu's texts have to be known before the font is made: build it with
 * the program's other static objects. */
struct Announce {
  Announce() { menu(); }
} g_announce;

} // namespace

extern "C" {

bool mkgp2pc_menu_is_open(void) { return g_open; }

void mkgp2pc_menu_set_open(bool open) {
  if (open && !g_open) {
    menu().reset();
  }
  g_open = open;
}

/* key: 1 up, 2 down, 3 decide, 4 cancel */
void mkgp2pc_menu_key(int key) {
  if (g_open && menu().key(static_cast<portui::Key>(key))) {
    g_open = false;
  }
}

void mkgp2pc_menu_draw(void) {
  if (g_open) {
    menu().draw();
  }
}

} // extern "C"
