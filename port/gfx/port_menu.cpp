/*
 * The port's menu: opened with a key at any time (Esc by default), the game
 * stands still while it is open. Its choices go to the game as requests
 * (port/platform/nav/nav.c) that the game's own loops take up on their next
 * frame; they are written into the input recording like key presses.
 */
#include "port_ui.hpp"

#include <mkgp2pc/debug.h>
#include <mkgp2pc/gamemenu.h>
#include <mkgp2pc/nav.h>

#include <dolphin/gx.h>
#include <dolphin/mtx.h>

#include <cstdio>

extern "C" {
/* video_aurora.c: applies a command of the input script now and records it */
void mkgp2pc_video_command(const char* cmd);
bool mkgp2pc_video_main_loop_runs(void);
void mkgp2pc_video_quit(void);
/* video_aurora.c: true when the game draws the menu itself; tells the
 * frame's end that this frame is one of the stopped game */
bool mkgp2pc_video_menu_frame(void);
/* the game: a texture object for the copy it takes of every frame's picture
 * (Frame_PostDraw_BackupBuffer), which its own screen transitions draw */
GXTexObj* BackupBuffer_InitAsTexture(void);
void HSD_StateInvalidate(int);
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
          Item{Text{"Resume", "ゲームに戻る", "ゲームにもどる"}, [] {}},
          Item{Text{"Back one screen", "一つ前の画面に戻る", "1つ前の画面にもどる"}, [] { mkgp2pc_video_command("back"); }, can_back,
               Text{"Selection screens before a race", "レース前の選択画面で使えます"}},
          Item{Text{"Restart this race", "このレースをやり直す"}, [] { flow(MKGP2PC_FLOW_RACE, -1); }, in_race,
               Text{"Starts again from the course introduction", "コース紹介からやり直します", "コースしょうかいからやり直します"}},
          Item{Text{"Go to", "移動"}, {}},
          Item{Text{"Title", "タイトル"}, [] { flow(MKGP2PC_FLOW_MENU, MKGP2PC_ENTRY_TITLE); }, main_loop,
               Text{"Ends the current play", "いまのプレイを終了します"}},
          Item{Text{"Class selection", "クラス選択"}, [] { flow(MKGP2PC_FLOW_MENU, MKGP2PC_ENTRY_CLASS); }, main_loop,
               Text{"No coin is taken", "コインは消費しません", "コインは使いません"}},
          Item{Text{"Character selection", "キャラクター選択"}, [] { flow(MKGP2PC_FLOW_MENU, MKGP2PC_ENTRY_CHARACTER); },
               main_loop, Text{"No coin is taken", "コインは消費しません", "コインは使いません"}},
          Item{Text{"Cup / round selection", "カップ・ラウンド選択"}, [] { flow(MKGP2PC_FLOW_MENU, MKGP2PC_ENTRY_CUP); }, main_loop,
               Text{"No coin is taken", "コインは消費しません", "コインは使いません"}},
          Item{Text{"Developer scene list", "開発用シーン一覧", "開発用シーンリスト"},
               [] {
                 mkgp2pc_debug_set(1); /* the list is debug text: invisible without it */
                 flow(-4, -1);
               },
               main_loop,
               Text{"The game's own list; some scenes may not work. F2: move, Space: choose",
                    "ゲーム内蔵の一覧です。動かないシーンもあります。F2: 移動、Space: 決定",
                    "動かないシーンもあります。F2: 移動、Space: 決定"}},
          Item{Text{"", ""}, {}},
          Item{Text{"Quit the game", "ゲームを終了する"}, [] { mkgp2pc_video_quit(); }},
      },
      Text{"Up/Down: choose   Enter: decide   Esc: close", "上下: 選択   Enter: 決定   Esc: 閉じる",
           "上下: 選択　Enter: 決定　Esc: とじる"}};
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

/* ---- the menu drawn by the game (port/platform/ui/gamemenu.c) ---- */

static int game_view(Mkgp2pcMenuView* v) {
  if (!g_open || !mkgp2pc_video_menu_frame()) {
    return 0;
  }
  const portui::ListMenu& m = menu();
  v->title = m.title();
  v->note = m.note();
  v->footer = m.footer();
  v->count = m.rows(v->text, v->kind, MKGP2PC_MENU_ROWS);
  v->cursor = m.cursor();
  for (int i = 0; i < MKGP2PC_MENU_ROWS; i++) {
    v->value[i] = nullptr;
  }
  v->more_above = v->more_below = 0;
  return 1;
}

/* The picture the stopped game showed last, over the whole frame. */
static void game_backdrop(void) {
  Mtx44 proj;
  Mtx view;
  C_MTXOrtho(proj, 0.f, 480.f, 0.f, 640.f, 0.f, 1.f);
  GXSetProjection(proj, GX_ORTHOGRAPHIC);
  GXSetViewport(0.f, 0.f, 640.f, 480.f, 0.f, 1.f);
  GXSetScissor(0, 0, 640, 480);
  PSMTXIdentity(view);
  GXLoadPosMtxImm(view, GX_PNMTX0);
  GXSetCurrentMtx(GX_PNMTX0);
  GXSetCullMode(GX_CULL_NONE);
  GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
  GXSetZCompLoc(GX_TRUE);
  GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
  GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_SET);
  GXSetColorUpdate(GX_TRUE);
  GXSetAlphaUpdate(GX_TRUE);
  GXSetNumChans(0);
  GXSetNumTexGens(1);
  GXSetNumIndStages(0);
  GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY, GX_FALSE, GX_PTIDENTITY);
  GXSetNumTevStages(1);
  GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
  GXSetTevOp(GX_TEVSTAGE0, GX_REPLACE);
  GXLoadTexObj(BackupBuffer_InitAsTexture(), GX_TEXMAP0);
  GXClearVtxDesc();
  GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
  GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
  GXBegin(GX_QUADS, GX_VTXFMT0, 4);
  GXPosition3f32(0.f, 0.f, -0.5f);
  GXTexCoord2f32(0.f, 0.f);
  GXPosition3f32(640.f, 0.f, -0.5f);
  GXTexCoord2f32(1.f, 0.f);
  GXPosition3f32(640.f, 480.f, -0.5f);
  GXTexCoord2f32(1.f, 1.f);
  GXPosition3f32(0.f, 480.f, -0.5f);
  GXTexCoord2f32(0.f, 1.f);
  GXEnd();
  HSD_StateInvalidate(-1);
}

struct GameMenuHooks {
  GameMenuHooks() {
    mkgp2pc_gamemenu_view = &game_view;
    mkgp2pc_gamemenu_backdrop = &game_backdrop;
  }
} g_gameMenuHooks;

} // extern "C"
