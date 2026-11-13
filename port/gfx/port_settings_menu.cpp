/*
 * 設定 in the port's menu: the pages グラフィック and キー入力
 * (docs/port/menu.md). What a row changes is either applied to the running
 * game at once, or only written into the settings program's file and in
 * force from the next start; the row's value and note say which
 * (port_settings.h).
 *
 * The rows are the settings program's 表示 and キー pages, without the
 * three the user wanted left there (2026-11-12): making the textures,
 * starting at full speed, and the coin tally in English.
 */
#include "port_settings.h"
#include "port_ui.hpp"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
/* gx_shim.cpp: the race HUD at the edges of a wide picture (1) or where 4:3 has it (0) */
int mkgp2pc_wide_hud_get(void);
void mkgp2pc_wide_hud_set(int edge);
extern float mkgp2pc_wide_k; /* the picture's width over 4:3's; above 1 in a wide picture */
}

namespace {

using portui::Item;
using portui::ListMenu;
using portui::Text;

const Text kOn{"On", "オン"}, kOff{"Off", "オフ"};
const Text kNextStart{"* from the next start", "* は次回の起動から変わります"};
const Text kNoFile{"Not started from the settings program: changes are not kept",
                   "ランチャーから起動していないので保存されません"};
const Text kFooter{"Up/Down: choose   Left/Right: change   Esc: back", "上下: 選択   左右: 変更   Esc: 戻る",
                   "上下: 選択　左右: 変更　Esc: もどる"};
const Text kFooterList{"Up/Down: choose   Enter: decide   Esc: back", "上下: 選択   Enter: 決定   Esc: 戻る",
                       "上下: 選択　Enter: 決定　Esc: もどる"};

/* ---- settings that need a new start ---------------------------------------
 * What the running game was started with is read from the environment the
 * way video_aurora.c reads it; `next` is what the file says after the
 * changes made here. */
struct Start {
  bool wide = false;
  int scale = 100; /* the window's height in hundredths of 480 lines */
  int backend = 0;
  int aa = 0; /* bit 0 MSAA 4x, bit 1 supersampling 2x */
  bool resizable = false;
};
Start g_run, g_next;

const char* const kBackendIds[] = {"", "d3d12", "d3d11", "vulkan", "opengl"};
const Text kBackends[] = {{"Automatic", "自動"}, {"Direct3D 12", "Direct3D 12", "D3D12"}, {"Direct3D 11", "Direct3D 11", "D3D11"},
                          {"Vulkan", "Vulkan"}, {"OpenGL", "OpenGL"}};
const Text kAa[] = {{"None", "なし"}, {"MSAA 4x", "MSAA 4x"}, {"SSAA 2x", "SSAA 2x"}, {"MSAA + SSAA", "MSAA+SSAA"}};
/* the sizes the settings program offers (launcher.c) */
const int kScales43[] = {100, 200, 300, 400, 500, 600};
const int kScales169[] = {100, 150, 200, 225, 300, 450};
enum { SCALES = 6 };
const int* scales(bool wide) { return wide ? kScales169 : kScales43; }

bool env_on(const char* name) {
  const char* v = std::getenv(name);
  return v != nullptr && *v != 0 && std::strcmp(v, "0") != 0;
}

void read_start() {
  Start& s = g_run;
  if (const char* a = std::getenv("MKGP2PC_ASPECT")) {
    int w = 0, h = 0;
    s.wide = std::sscanf(a, "%d:%d", &w, &h) == 2 && w > 0 && h > 0 && w * 3 > h * 4 && w <= h * 4;
  }
  if (const char* t = std::getenv("MKGP2PC_SCALE")) {
    double f = std::strtod(t, nullptr);
    if (f >= 0.5 && f <= 8) {
      s.scale = static_cast<int>(std::lround(f * 100));
    }
  }
  if (const char* b = std::getenv("MKGP2PC_BACKEND")) {
    for (int i = 1; i < 5; i++) {
      if (std::strcmp(b, kBackendIds[i]) == 0) {
        s.backend = i;
      }
    }
  }
  if (const char* m = std::getenv("MKGP2PC_MSAA"); m != nullptr && *m != 0 && std::strcmp(m, "0") != 0 && std::strcmp(m, "1") != 0) {
    s.aa |= 1;
  }
  if (const char* m = std::getenv("MKGP2PC_SSAA"); m != nullptr && std::strtod(m, nullptr) > 1) {
    s.aa |= 2;
  }
  s.resizable = env_on("MKGP2PC_RESIZABLE");
  g_next = s;
}

int cycle(int v, int dir, int count) { return (v + dir + count) % count; }

/* A value's text has to stay where it is until the next one is made: one
 * buffer per row. `changed` adds the mark of a value not yet in force. */
Text shown(int row, const Text& t, bool changed) {
  static char en[16][64], ja[16][64], game[16][64];
  if (!changed) {
    return t;
  }
  std::snprintf(en[row], sizeof(en[row]), "%s *", t.en);
  std::snprintf(ja[row], sizeof(ja[row]), "%s *", t.ja);
  std::snprintf(game[row], sizeof(game[row]), "%s*", t.game != nullptr ? t.game : t.ja);
  return Text{en[row], ja[row], game[row]};
}

Text size_text(bool wide, int scale) {
  static char text[2][32];
  static int which;
  which ^= 1;
  int h = 480 * scale / 100;
  int w = wide ? (h * 16 / 9 + 1) & ~1 : h * 4 / 3;
  std::snprintf(text[which], sizeof(text[which]), "%dx%d", w, h);
  return Text{text[which], text[which]};
}

void save_scale() {
  char text[16];
  std::snprintf(text, sizeof(text), "%g", g_next.scale / 100.0);
  mkgp2pc_settings_save("scale", text);
}

bool can_save() { return mkgp2pc_settings_can_save() != 0; }

Item restart_row(Text label, std::function<Text()> value, std::function<void(int)> adjust, Text note = kNextStart) {
  return Item::Setting(label, std::move(value), std::move(adjust), can_save, note);
}

Item live_row(Text label, int (*get)(), void (*set)(int), const char* key, Text note = {}) {
  return Item::Setting(
      label, [get] { return get() ? kOn : kOff; },
      [get, set, key](int) {
        const int want = !get();
        set(want);
        if (get() == want) { /* the change may be refused (no folder `hires`) */
          mkgp2pc_settings_save_int(key, want);
        }
      },
      {}, note);
}

/* a row of another module (mkgp2pc_settings_add_graphics) */
Item extra_row(const Mkgp2pcSettingRow* r) {
  auto value = [r] {
    int i = r->get();
    if (i < 0 || i >= r->count) {
      i = 0;
    }
    return Text{r->choices_en[i], r->choices_ja[i]};
  };
  auto adjust = [r](int dir) { r->set(cycle(r->get(), dir, r->count)); };
  std::function<bool()> enabled;
  if (r->enabled != nullptr) {
    enabled = [r] { return r->enabled() != 0; };
  }
  return Item::Setting(Text{r->en, r->ja}, value, adjust, enabled, r->restart ? kNextStart : Text{});
}

ListMenu& graphics() {
  static ListMenu* m;
  if (m != nullptr) {
    return *m;
  }
  std::vector<Item> rows;
  if (!can_save()) {
    rows.push_back(Item{kNoFile, {}});
  }
  rows.push_back(restart_row(
      Text{"Aspect", "画面の比率"}, [] { return shown(0, g_next.wide ? Text{"16:9", "16:9"} : Text{"4:3", "4:3"}, g_next.wide != g_run.wide); },
      [](int) {
        g_next.wide = !g_next.wide;
        /* the size on offer nearest to the one set */
        int best = scales(g_next.wide)[0];
        for (int i = 0; i < SCALES; i++) {
          if (std::abs(scales(g_next.wide)[i] - g_next.scale) < std::abs(best - g_next.scale)) {
            best = scales(g_next.wide)[i];
          }
        }
        g_next.scale = best;
        mkgp2pc_settings_save_int("wide", g_next.wide);
        save_scale();
      }));
  rows.push_back(restart_row(
      Text{"Window size", "窓の大きさ", "画面の大きさ"},
      [] { return shown(1, size_text(g_next.wide, g_next.scale), g_next.scale != g_run.scale || g_next.wide != g_run.wide); },
      [](int dir) {
        const int* list = scales(g_next.wide);
        int at = 0;
        for (int i = 0; i < SCALES; i++) {
          if (std::abs(list[i] - g_next.scale) < std::abs(list[at] - g_next.scale)) {
            at = i;
          }
        }
        g_next.scale = list[cycle(at, dir, SCALES)];
        save_scale();
      }));
  /* Where the race HUD and the corner parts of menu screens sit in a wide
   * picture (gx_shim.cpp, widescreen.md):
   * applies at once; there is nothing to choose in a 4:3 picture. */
  rows.push_back(Item::Setting(
      Text{"UI position", "UI の位置", "UIの位置"},
      [] { return mkgp2pc_wide_hud_get() ? Text{"Screen edges", "左右の端", "左右のはし"} : Text{"As in 4:3", "4:3 の位置", "4:3の位置"}; },
      [](int) {
        mkgp2pc_wide_hud_set(!mkgp2pc_wide_hud_get());
        mkgp2pc_settings_save("wide_hud", mkgp2pc_wide_hud_get() ? "edge" : "center");
      },
      [] { return mkgp2pc_wide_k > 1.001f; }));
  rows.push_back(restart_row(
      Text{"Renderer", "描画方式", "レンダリング"}, [] { return shown(2, kBackends[g_next.backend], g_next.backend != g_run.backend); },
      [](int dir) {
        g_next.backend = cycle(g_next.backend, dir, 5);
        mkgp2pc_settings_save("backend", kBackendIds[g_next.backend]);
      }));
  rows.push_back(restart_row(
      Text{"Anti-aliasing", "アンチエイリアス"}, [] { return shown(3, kAa[g_next.aa], g_next.aa != g_run.aa); },
      [](int dir) {
        g_next.aa = cycle(g_next.aa, dir, 4);
        mkgp2pc_settings_save_int("aa", g_next.aa);
      }));
  rows.push_back(live_row(Text{"Smooth kart shadow", "影をなめらかにする", "なめらかな影"}, mkgp2pc_shadow_smooth, mkgp2pc_shadow_smooth_set,
                          "shadow_smooth"));
  rows.push_back(live_row(Text{"High-resolution textures", "高解像度のテクスチャ", "ハイレゾテクスチャ"}, mkgp2pc_hires_on,
                          mkgp2pc_hires_set, "hires",
                          Text{"Needs the folder hires, made in the settings program", "hires フォルダが必要です (ランチャーで作成)",
                               "hires フォルダがないとオンになりません"}));
  rows.push_back(live_row(Text{"Do not wait for pipelines", "描画の準備を待たない", "シェーダーを待たない"},
                          mkgp2pc_video_pipeline_async, mkgp2pc_video_pipeline_async_set, "async_pipelines"));
  rows.push_back(restart_row(
      Text{"Resizable window (unstable)", "窓のサイズ変更を許可 (不安定)", "ウィンドウのリサイズ"},
      [] { return shown(4, g_next.resizable ? kOn : kOff, g_next.resizable != g_run.resizable); },
      [](int) {
        g_next.resizable = !g_next.resizable;
        mkgp2pc_settings_save_int("resizable", g_next.resizable);
      },
      Text{"Unstable on Windows. * from the next start", "Windows では不安定です。* は次回の起動から",
           "ふあんていです。* は次回の起動から変わります"}));
  const Mkgp2pcSettingRow* const* extra;
  const int n = mkgp2pc_settings_graphics_rows(&extra);
  for (int i = 0; i < n; i++) {
    rows.push_back(extra_row(extra[i]));
  }
  m = new ListMenu(Text{"GRAPHICS", "グラフィック"}, std::move(rows), kFooter);
  return *m;
}

/* ---- keys ------------------------------------------------------------------ */

struct ActionLabel {
  const char* id;
  Text label;
};
/* in the settings program's order (launcher.c kActions) */
const ActionLabel kActions[] = {
    {"left", {"Steer left", "ハンドル 左"}},
    {"right", {"Steer right", "ハンドル 右"}},
    {"accel", {"Accelerator", "アクセル"}},
    {"brake", {"Brake", "ブレーキ"}},
    {"item", {"Item", "アイテム"}},
    {"cancel", {"Cancel (Mario button)", "キャンセル (マリオボタン)", "キャンセル"}},
    {"coin", {"Coin", "コイン"}},
    {"card", {"Insert card", "カード挿入", "カードを入れる"}},
    {"test", {"Test", "テスト"}},
    {"service", {"Service", "サービス"}},
    {"pause", {"Pause", "一時停止", "ポーズ"}},
    {"step", {"Frame step", "コマ送り", "コマおくり"}},
    {"fast", {"Full speed (toggle)", "全速 (切り替え)"}},
    {"fasthold", {"Full speed (hold)", "全速 (押下中)", "全速 (押す間)"}},
    {"dump", {"Dump", "ダンプ"}},
    {"hires", {"High resolution (toggle)", "高解像度 (切り替え)", "ハイレゾ切り替え"}},
    {"menu", {"Menu", "メニュー"}},
    {"back", {"Back one screen", "一つ前の画面に戻る", "1つ前の画面にもどる"}},
    {"debug", {"Debug output (toggle)", "デバッグ表示 (切り替え)", "デバッグ切り替え"}},
    {"debugpage", {"Debug output page", "デバッグ表示のページ送り", "デバッグのページ"}},
    {"devup", {"Developer: up", "開発用 上"}},
    {"devdown", {"Developer: down", "開発用 下"}},
    {"devstart", {"Developer: decide / back", "開発用 決定/戻る", "開発用 決定/もどる"}},
    {"devsub", {"Developer: sub", "開発用 サブ"}},
};
enum { ACTIONS = sizeof(kActions) / sizeof(kActions[0]), SLOTS = 2 };

/* the game's number of the action (its table has another order) */
int action_index(const char* id) {
  for (int i = 0; i < mkgp2pc_keys_actions(); i++) {
    if (std::strcmp(mkgp2pc_keys_action_name(i), id) == 0) {
      return i;
    }
  }
  return -1;
}

/* A key's name: SDL's for the physical key, with Japanese for the few that
 * read better so (the game's font has no arrows). */
const char* key_name(int scancode, bool japanese) {
  if (scancode == 0) {
    return japanese ? "なし" : "none";
  }
  if (japanese) {
    switch (scancode) {
    case SDL_SCANCODE_UP: return "上";
    case SDL_SCANCODE_DOWN: return "下";
    case SDL_SCANCODE_LEFT: return "左";
    case SDL_SCANCODE_RIGHT: return "右";
    case SDL_SCANCODE_SPACE: return "スペース";
    case SDL_SCANCODE_RETURN: return "Enter";
    case SDL_SCANCODE_LSHIFT: return "左Shift";
    case SDL_SCANCODE_RSHIFT: return "右Shift";
    case SDL_SCANCODE_LCTRL: return "左Ctrl";
    case SDL_SCANCODE_RCTRL: return "右Ctrl";
    case SDL_SCANCODE_LALT: return "左Alt";
    case SDL_SCANCODE_RALT: return "右Alt";
    default: break;
    }
  }
  const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode));
  if (name == nullptr || *name == 0) {
    static char number[2][24];
    std::snprintf(number[japanese], sizeof(number[0]), japanese ? "キー %d" : "key %d", scancode);
    return number[japanese];
  }
  return name;
}

int g_action; /* the game's number of the action whose page is open */
int g_waitingSlot = -1;

void save_keys() {
  /* the settings program keeps two keys per action: "usage1,usage2" */
  static char text[ACTIONS][24];
  const char* keys[ACTIONS];
  const char* values[ACTIONS];
  for (int i = 0; i < ACTIONS; i++) {
    const int a = action_index(kActions[i].id);
    keys[i] = kActions[i].id;
    std::snprintf(text[i], sizeof(text[i]), "%d,%d", a < 0 ? 0 : mkgp2pc_keys_get(a, 0), a < 0 ? 0 : mkgp2pc_keys_get(a, 1));
    values[i] = text[i];
  }
  mkgp2pc_settings_save_section("keys", keys, values, ACTIONS);
}

/* The menu's own key cannot be lost: without one there is no way back here. */
void keep_menu_key() {
  const int menu = action_index("menu");
  if (menu >= 0 && mkgp2pc_keys_get(menu, 0) == 0 && mkgp2pc_keys_get(menu, 1) == 0) {
    mkgp2pc_keys_set(menu, 0, SDL_SCANCODE_ESCAPE);
  }
}

void keys_changed() {
  keep_menu_key();
  save_keys();
}

Text slot_value(int action, int slot, int row) {
  static char en[32][48], ja[32][48];
  if (mkgp2pc_keys_capturing() && action == g_action && slot == g_waitingSlot) {
    return Text{"press a key", "キーを押してください"};
  }
  const int code = mkgp2pc_keys_get(action, slot);
  std::snprintf(en[row], sizeof(en[row]), "%s", key_name(code, false));
  std::snprintf(ja[row], sizeof(ja[row]), "%s", key_name(code, true));
  return Text{en[row], ja[row]};
}

ListMenu& one_action() {
  auto change = [](int slot) {
    return [slot] {
      g_waitingSlot = slot;
      mkgp2pc_keys_capture(g_action, slot);
    };
  };
  auto clear = [](int slot) {
    return [slot] {
      mkgp2pc_keys_set(g_action, slot, 0);
      keys_changed();
    };
  };
  auto has = [](int slot) { return [slot] { return mkgp2pc_keys_get(g_action, slot) != 0; }; };
  const Text press{"Enter, then the new key. Esc: leave as it is", "Enter のあと新しいキーを押す。Esc: やめる",
                   "Enter のあと新しいキーを押します。Esc: やめる"};
  static ListMenu m{Text{"", ""},
                    {
                        Item::Sub(Text{"Key 1", "キー 1"}, change(0), {}, press, [] { return slot_value(g_action, 0, 0); }),
                        Item::Sub(Text{"Key 2", "キー 2"}, change(1), {}, press, [] { return slot_value(g_action, 1, 1); }),
                        Item{Text{"", ""}, {}},
                        Item::Sub(Text{"Remove key 1", "キー 1 を外す"}, clear(0), has(0)),
                        Item::Sub(Text{"Remove key 2", "キー 2 を外す"}, clear(1), has(1)),
                    },
                    kFooterList};
  return m;
}

Text both_keys(int action, int row) {
  static char en[32][64], ja[32][64];
  const int a = mkgp2pc_keys_get(action, 0), b = mkgp2pc_keys_get(action, 1);
  if (a != 0 && b != 0) {
    std::snprintf(en[row], sizeof(en[row]), "%s / %s", key_name(a, false), key_name(b, false));
    std::snprintf(ja[row], sizeof(ja[row]), "%s / %s", key_name(a, true), key_name(b, true));
  } else {
    std::snprintf(en[row], sizeof(en[row]), "%s", key_name(a != 0 ? a : b, false));
    std::snprintf(ja[row], sizeof(ja[row]), "%s", key_name(a != 0 ? a : b, true));
  }
  return Text{en[row], ja[row]};
}

ListMenu& keys() {
  static ListMenu* m;
  if (m != nullptr) {
    return *m;
  }
  std::vector<Item> rows;
  if (!can_save()) {
    rows.push_back(Item{kNoFile, {}});
  }
  for (int i = 0; i < ACTIONS; i++) {
    const int a = action_index(kActions[i].id);
    if (a < 0) {
      continue;
    }
    const Text label = kActions[i].label;
    rows.push_back(Item::Sub(
        label,
        [a, label] {
          g_action = a;
          one_action().set_title(label);
          portui::open_list(one_action());
        },
        {}, {}, [a, i] { return both_keys(a, i); }));
  }
  rows.push_back(Item{Text{"", ""}, {}});
  rows.push_back(Item::Sub(Text{"All keys back to the defaults", "キーを初期化", "すべて初期設定にもどす"}, [] {
    mkgp2pc_keys_reset();
    keys_changed();
  }));
  m = new ListMenu(Text{"KEYS", "キー入力"}, std::move(rows), kFooterList);
  return *m;
}

ListMenu& settings() {
  static ListMenu m{Text{"SETTINGS", "設定"},
                    {
                        Item::Sub(Text{"Graphics", "グラフィック"}, [] { portui::open_list(graphics()); }),
                        Item::Sub(Text{"Keys", "キー入力"}, [] { portui::open_list(keys()); }),
                    },
                    kFooterList};
  return m;
}

/* Texts of the pages, announced for the menu's font before it is made
 * (port_ui.hpp): the pages themselves are built when first opened, since
 * rows of other modules may be added until then. */
struct Announce {
  Announce() {
    mkgp2pc_keys_changed = &keys_changed;
    settings();
    one_action();
    for (const Text* t : {&kOn, &kOff, &kNextStart, &kNoFile, &kFooter, &kFooterList}) {
      portui::announce(t->ja);
    }
    for (const ActionLabel& a : kActions) {
      portui::announce(a.label.ja);
    }
    for (const Text& t : kBackends) {
      portui::announce(t.ja);
    }
    for (const Text& t : kAa) {
      portui::announce(t.ja);
    }
    portui::announce("画面の比率窓の大きさ描画方式アンチエイリアス影をなめらかにする高解像度のテクスチャ描画の準備を待たない"
                     "窓のサイズ変更を許可 (不安定)レース中の UI の位置左右の端hires フォルダが必要です (ランチャーで作成)グラフィックキー入力"
                     "Windows では不安定です。* は次回の起動からキーを初期化キーを押してくださいなし上下左右スペースは次回の起動から変わります");
    read_start();
  }
} g_announce;

} // namespace

namespace portui {
ListMenu& settings_menu() { return settings(); }
} // namespace portui
