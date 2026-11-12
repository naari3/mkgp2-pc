/*
 * mkgp2-launcher.exe: the settings program of the Windows build.
 *
 * The game program (mkgp2-gfx.exe) takes its settings from environment
 * variables (docs/port/launcher.md lists them). This program shows them as
 * a window, keeps them in mkgp2-pc.ini next to itself and starts the game
 * with the variables set, the log going to gfx.log as run-gfx.cmd does.
 *
 *   mkgp2-launcher.exe          the window
 *   mkgp2-launcher.exe --run    start the game with the saved settings, no window
 *
 * Plain Win32, no library of the port and no game code: it builds alone
 * (port/launcher/build.sh). This file is UTF-8; the texts are Japanese.
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define GAME_EXE L"mkgp2-gfx.exe"
#define INI_NAME L"mkgp2-pc.ini"
#define LOG_NAME L"gfx.log"
#define LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* ---- settings ---------------------------------------------------------- */

#define KEY_SLOTS 2
typedef struct {
  const wchar_t* id;    /* the name in MKGP2PC_KEYS and in the ini file */
  const wchar_t* label;
  int defaults[KEY_SLOTS]; /* USB keyboard usages = SDL scancodes */
} KeyAction;

static const KeyAction kActions[] = {
    {L"left", L"ハンドル 左", {80, 0}},
    {L"right", L"ハンドル 右", {79, 0}},
    {L"accel", L"アクセル", {82, 29}},
    {L"brake", L"ブレーキ", {81, 27}},
    {L"item", L"アイテム", {44, 40}},
    {L"cancel", L"キャンセル (マリオボタン)", {225, 0}},
    {L"coin", L"コイン", {34, 6}},
    {L"card", L"カード挿入", {12, 0}},
    {L"test", L"テスト", {58, 0}},
    {L"service", L"サービス", {59, 0}},
    {L"pause", L"一時停止", {19, 0}},
    {L"step", L"コマ送り", {18, 0}},
    {L"fast", L"全速 (切り替え)", {67, 0}},
    {L"fasthold", L"全速 (押下中)", {43, 0}},
    {L"dump", L"ダンプ", {66, 0}},
    {L"hires", L"高解像度 (切り替え)", {65, 0}},
    {L"debug", L"デバッグ表示 (切り替え)", {60, 0}},
    {L"debugpage", L"デバッグ表示のページ送り", {61, 0}},
    {L"devup", L"開発用 上", {75, 0}},
    {L"devdown", L"開発用 下", {78, 0}},
    {L"devstart", L"開発用 決定/戻る", {77, 0}},
    {L"devsub", L"開発用 サブ", {74, 0}},
};
#define ACTIONS LEN(kActions)

typedef struct {
  wchar_t iso[MAX_PATH];
  int card;
  wchar_t cardFile[MAX_PATH];
  int closeOnStart;
  int bootSkip;
  int wide;            /* 16:9 instead of 4:3 */
  int scale;           /* window height in hundredths of 480 lines: 150 = 720 */
  wchar_t backend[16]; /* "" = the library's own choice */
  int resizable;
  int shadowSmooth;
  int fast;
  int asyncPipelines;
  int goodsDisc;
  int hires;
  int aa; /* anti-aliasing: bit 0 = MSAA 4x, bit 1 = supersampling 2x */
  int link;
  int pcbId; /* 1..4, 0 = not given: 1, or with a room the first free one */
  wchar_t peers[256];
  wchar_t room[64];
  wchar_t server[200];
  wchar_t port[16];
  int keys[ACTIONS][KEY_SLOTS];
  wchar_t extra[2048]; /* NAME=VALUE lines, CR LF between them */
} Settings;

static Settings g_set;
static wchar_t g_dir[MAX_PATH]; /* where this program is, with the last backslash */
static wchar_t g_ini[MAX_PATH];

static void defaults_keys(Settings* s) {
  for (int i = 0; i < ACTIONS; i++) {
    for (int k = 0; k < KEY_SLOTS; k++) {
      s->keys[i][k] = kActions[i].defaults[k];
    }
  }
}

static void defaults(Settings* s) {
  ZeroMemory(s, sizeof(*s));
  s->card = 1;
  s->scale = 100;
  s->shadowSmooth = 1;
  s->link = 1;
  s->pcbId = 1;
  defaults_keys(s);
}

static int keys_are_default(const Settings* s) {
  for (int i = 0; i < ACTIONS; i++) {
    for (int k = 0; k < KEY_SLOTS; k++) {
      if (s->keys[i][k] != kActions[i].defaults[k]) return 0;
    }
  }
  return 1;
}

static int clampi(int v, int lo, int hi, int def) { return v < lo || v > hi ? def : v; }

static void ini_str(const wchar_t* key, wchar_t* out, int size) {
  GetPrivateProfileStringW(L"settings", key, out, out, (DWORD)size, g_ini);
}
static int ini_int(const wchar_t* key, int def) { return (int)GetPrivateProfileIntW(L"settings", key, def, g_ini); }

static void load_settings(Settings* s) {
  defaults(s);
  ini_str(L"iso", s->iso, LEN(s->iso));
  s->card = ini_int(L"card", 1) != 0;
  ini_str(L"card_file", s->cardFile, LEN(s->cardFile));
  s->closeOnStart = ini_int(L"close_on_start", 0) != 0;
  s->bootSkip = ini_int(L"boot_skip", 0) != 0;
  s->wide = ini_int(L"wide", 0) != 0;
  {
    wchar_t text[16] = L"1";
    ini_str(L"scale", text, LEN(text));
    s->scale = clampi((int)(wcstod(text, NULL) * 100 + 0.5), 25, 800, 100);
  }
  ini_str(L"backend", s->backend, LEN(s->backend));
  s->resizable = ini_int(L"resizable", 0) != 0;
  s->shadowSmooth = ini_int(L"shadow_smooth", 1) != 0;
  s->fast = ini_int(L"fast", 0) != 0;
  s->asyncPipelines = ini_int(L"async_pipelines", 0) != 0;
  s->goodsDisc = ini_int(L"goods_disc", 0) != 0;
  s->hires = ini_int(L"hires", 0) != 0;
  s->aa = clampi(ini_int(L"aa", 0), 0, 3, 0);
  s->link = ini_int(L"link", 1) != 0;
  s->pcbId = clampi(ini_int(L"pcb_id", 1), 0, 4, 1);
  ini_str(L"room", s->room, LEN(s->room));
  ini_str(L"server", s->server, LEN(s->server));
  ini_str(L"peers", s->peers, LEN(s->peers));
  ini_str(L"port", s->port, LEN(s->port));
  for (int i = 0; i < ACTIONS; i++) {
    wchar_t text[64], missing[] = L"?";
    GetPrivateProfileStringW(L"keys", kActions[i].id, missing, text, LEN(text), g_ini);
    if (text[0] == L'?') continue; /* not in the file: the default stays */
    wchar_t* p = text;
    for (int k = 0; k < KEY_SLOTS; k++) {
      s->keys[i][k] = clampi((int)wcstol(p, &p, 10), 0, 511, 0);
      if (*p == L',') p++;
    }
  }
  /* [env]: the section's lines as they are */
  static wchar_t section[4096];
  DWORD n = GetPrivateProfileSectionW(L"env", section, LEN(section), g_ini);
  s->extra[0] = 0;
  for (wchar_t* line = section; n != 0 && *line != 0; line += wcslen(line) + 1) {
    if (wcslen(s->extra) + wcslen(line) + 3 < (size_t)LEN(s->extra)) {
      wcscat(s->extra, line);
      wcscat(s->extra, L"\r\n");
    }
  }
}

static void put_str(const wchar_t* key, const wchar_t* value) { WritePrivateProfileStringW(L"settings", key, value, g_ini); }
static void put_int(const wchar_t* key, int value) {
  wchar_t text[16];
  swprintf(text, LEN(text), L"%d", value);
  put_str(key, text);
}

static int save_settings(const Settings* s) {
  /* A file that starts with the UTF-16 mark is kept in UTF-16 by the
   * profile functions; otherwise paths outside the ANSI code page are lost. */
  if (GetFileAttributesW(g_ini) == INVALID_FILE_ATTRIBUTES) {
    HANDLE f = CreateFileW(g_ini, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
      static const wchar_t head[] = L"\xFEFF; mkgp2-launcher.exe の設定 (このプログラムが書き換えます)\r\n";
      DWORD done;
      WriteFile(f, head, (DWORD)(wcslen(head) * sizeof(wchar_t)), &done, NULL);
      CloseHandle(f);
    }
  }
  if (!WritePrivateProfileStringW(L"settings", L"iso", s->iso, g_ini)) return 0;
  put_int(L"card", s->card);
  put_str(L"card_file", s->cardFile);
  put_int(L"close_on_start", s->closeOnStart);
  put_int(L"boot_skip", s->bootSkip);
  put_int(L"wide", s->wide);
  {
    wchar_t text[16];
    swprintf(text, LEN(text), L"%g", s->scale / 100.0);
    put_str(L"scale", text);
  }
  put_str(L"backend", s->backend);
  put_int(L"resizable", s->resizable);
  put_int(L"shadow_smooth", s->shadowSmooth);
  put_int(L"fast", s->fast);
  put_int(L"async_pipelines", s->asyncPipelines);
  put_int(L"goods_disc", s->goodsDisc);
  put_int(L"hires", s->hires);
  put_int(L"aa", s->aa);
  put_int(L"link", s->link);
  put_int(L"pcb_id", s->pcbId);
  put_str(L"peers", s->peers);
  put_str(L"room", s->room);
  put_str(L"server", s->server);
  put_str(L"port", s->port);
  for (int i = 0; i < ACTIONS; i++) {
    wchar_t text[32];
    swprintf(text, LEN(text), L"%d,%d", s->keys[i][0], s->keys[i][1]);
    WritePrivateProfileStringW(L"keys", kActions[i].id, text, g_ini);
  }
  static wchar_t section[4096];
  int n = 0;
  for (const wchar_t* p = s->extra; *p != 0;) {
    size_t len = wcscspn(p, L"\r\n");
    if (len != 0 && n + (int)len + 2 < LEN(section)) {
      memcpy(section + n, p, len * sizeof(wchar_t));
      n += (int)len;
      section[n++] = 0;
    }
    p += len;
    while (*p == L'\r' || *p == L'\n') p++;
  }
  section[n] = 0;
  section[n + 1] = 0;
  WritePrivateProfileSectionW(L"env", section, g_ini);
  WritePrivateProfileStringW(NULL, NULL, NULL, g_ini); /* flush */
  return 1;
}

/* ---- keys -------------------------------------------------------------- */

/* Scan code of a Windows key message (set 1) to USB keyboard usage, the
 * number SDL calls scancode. [1] is for keys with the extended bit. */
static const unsigned char kScanToUsage[2][0x80] = {
    {[0x01] = 41, [0x02] = 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, [0x0C] = 45, 46, 42, 43,
     [0x10] = 20, 26, 8, 21, 23, 28, 24, 12, 18, 19, [0x1A] = 47, 48, 40, 224,
     [0x1E] = 4, 22, 7, 9, 10, 11, 13, 14, 15, [0x27] = 51, 52, 53, 225, 49,
     [0x2C] = 29, 27, 6, 25, 5, 17, 16, [0x33] = 54, 55, 56, 229, 85, 226, 44, 57,
     [0x3B] = 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, [0x45] = 83, 71,
     [0x47] = 95, 96, 97, 86, 92, 93, 94, 87, 89, 90, 91, 98, 99,
     [0x57] = 68, 69, [0x70] = 136, [0x73] = 135, [0x79] = 138, [0x7B] = 139, [0x7D] = 137},
    {[0x1C] = 88, [0x1D] = 228, [0x35] = 84, [0x38] = 230, [0x47] = 74, [0x48] = 82, [0x49] = 75,
     [0x4B] = 80, [0x4D] = 79, [0x4F] = 77, [0x50] = 81, [0x51] = 78, [0x52] = 73, [0x53] = 76},
};

static const struct {
  int usage;
  const wchar_t* name;
} kKeyNames[] = {
    {40, L"Enter"}, {41, L"Esc"}, {42, L"Backspace"}, {43, L"Tab"}, {44, L"スペース"},
    {45, L"-"}, {46, L"^ (=)"}, {47, L"@ ([)"}, {48, L"[ (])"}, {49, L"] (\\)"},
    {51, L";"}, {52, L": (')"}, {53, L"半角/全角 (`)"}, {54, L","}, {55, L"."}, {56, L"/"},
    {57, L"CapsLock"}, {71, L"ScrollLock"}, {73, L"Insert"}, {74, L"Home"}, {75, L"PageUp"},
    {76, L"Delete"}, {77, L"End"}, {78, L"PageDown"}, {79, L"→"}, {80, L"←"}, {81, L"↓"}, {82, L"↑"},
    {83, L"NumLock"}, {84, L"テンキー /"}, {85, L"テンキー *"}, {86, L"テンキー -"}, {87, L"テンキー +"},
    {88, L"テンキー Enter"}, {99, L"テンキー ."}, {135, L"ろ (\\)"}, {136, L"カタカナ/ひらがな"},
    {137, L"¥"}, {138, L"変換"}, {139, L"無変換"}, {224, L"左 Ctrl"}, {225, L"左 Shift"},
    {226, L"左 Alt"}, {228, L"右 Ctrl"}, {229, L"右 Shift"}, {230, L"右 Alt"},
};

static const wchar_t* key_name(int usage, wchar_t* buf, int size) {
  if (usage == 0) return L"";
  if (usage >= 4 && usage <= 29) {
    swprintf(buf, size, L"%c", L'A' + usage - 4);
  } else if (usage >= 30 && usage <= 39) {
    swprintf(buf, size, L"%d", (usage - 29) % 10);
  } else if (usage >= 58 && usage <= 69) {
    swprintf(buf, size, L"F%d", usage - 57);
  } else if (usage >= 89 && usage <= 98) {
    swprintf(buf, size, L"テンキー %d", (usage - 88) % 10);
  } else {
    for (int i = 0; i < LEN(kKeyNames); i++) {
      if (kKeyNames[i].usage == usage) return kKeyNames[i].name;
    }
    swprintf(buf, size, L"キー番号 %d", usage);
  }
  return buf;
}

/* ---- starting the game -------------------------------------------------- */

static void env_flag(const wchar_t* name, int on) { SetEnvironmentVariableW(name, on ? L"1" : NULL); }
static void env_text(const wchar_t* name, const wchar_t* value) {
  SetEnvironmentVariableW(name, value != NULL && value[0] != 0 ? value : NULL);
}

static void apply_environment(const Settings* s) {
  wchar_t text[1024];
  env_text(L"MKGP2_ISO", s->iso);
  env_text(L"MKGP2PC_CARD", s->card ? NULL : L"0");
  env_text(L"MKGP2PC_CARD_FILE", s->cardFile);
  env_flag(L"MKGP2PC_BOOT_SKIP", s->bootSkip);
  swprintf(text, LEN(text), L"%g", s->scale / 100.0);
  env_text(L"MKGP2PC_SCALE", s->scale != 100 ? text : NULL);
  env_text(L"MKGP2PC_ASPECT", s->wide ? L"16:9" : NULL);
  env_text(L"MKGP2PC_BACKEND", s->backend);
  env_flag(L"MKGP2PC_RESIZABLE", s->resizable);
  env_flag(L"MKGP2PC_SHADOW_NEAREST", !s->shadowSmooth);
  env_flag(L"MKGP2PC_FAST", s->fast);
  env_flag(L"MKGP2PC_ASYNC_PIPELINES", s->asyncPipelines);
  env_flag(L"MKGP2PC_GOODS_DISC", s->goodsDisc);
  env_flag(L"MKGP2PC_HIRES", s->hires);
  env_text(L"MKGP2PC_MSAA", (s->aa & 1) ? L"4" : NULL);
  env_text(L"MKGP2PC_SSAA", (s->aa & 2) ? L"2" : NULL);
  env_text(L"MKGP2PC_LINK", s->link ? NULL : L"0");
  swprintf(text, LEN(text), L"%d", s->pcbId);
  env_text(L"MKGP2PC_PCB_ID", s->pcbId > 0 ? text : NULL);
  env_text(L"MKGP2PC_LINK_ROOM", s->room);
  env_text(L"MKGP2PC_LINK_SERVER", s->server);
  env_text(L"MKGP2PC_LINK_PEERS", s->peers);
  env_text(L"MKGP2PC_LINK_PORT", s->port);
  if (keys_are_default(s)) {
    env_text(L"MKGP2PC_KEYS", NULL);
  } else {
    int n = 0;
    for (int i = 0; i < ACTIONS; i++) {
      n += swprintf(text + n, LEN(text) - n, L"%ls%ls=", i ? L";" : L"", kActions[i].id);
      for (int k = 0, any = 0; k < KEY_SLOTS; k++) {
        if (s->keys[i][k] != 0) {
          n += swprintf(text + n, LEN(text) - n, L"%ls%02d", any ? L"," : L"", s->keys[i][k]);
          any = 1;
        }
      }
    }
    env_text(L"MKGP2PC_KEYS", text);
  }
  /* the extra lines last: they win over the settings above */
  for (const wchar_t* p = s->extra; *p != 0;) {
    size_t len = wcscspn(p, L"\r\n");
    if (len != 0 && len < (size_t)LEN(text)) {
      memcpy(text, p, len * sizeof(wchar_t));
      text[len] = 0;
      wchar_t* eq = wcschr(text, L'=');
      if (eq != NULL && eq != text) {
        *eq = 0;
        SetEnvironmentVariableW(text, eq[1] != 0 ? eq + 1 : NULL);
      }
    }
    p += len;
    while (*p == L'\r' || *p == L'\n') p++;
  }
}

static void dir_path(wchar_t* out, const wchar_t* name) {
  wcscpy(out, g_dir);
  wcscat(out, name);
}

static int file_exists(const wchar_t* path) {
  DWORD a = GetFileAttributesW(path);
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* Starts the game. Returns the process handle, or NULL with a message for
 * the user in `error`. */
static HANDLE start_game(const Settings* s, wchar_t* error, int errorSize) {
  wchar_t exe[MAX_PATH], log[MAX_PATH], command[MAX_PATH + 8];
  dir_path(exe, GAME_EXE);
  dir_path(log, LOG_NAME);
  if (!file_exists(exe)) {
    swprintf(error, errorSize, L"ゲーム本体が見つかりません。\n%ls", exe);
    return NULL;
  }
  if (s->iso[0] == 0 || !file_exists(s->iso)) {
    swprintf(error, errorSize, L"ISO ファイルが見つかりません。\n%ls", s->iso);
    return NULL;
  }
  apply_environment(s);

  SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
  HANDLE out = CreateFileW(log, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
  HANDLE in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
  if (out == INVALID_HANDLE_VALUE) {
    if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
    swprintf(error, errorSize, L"ログを書けません。\n%ls", log);
    return NULL;
  }
  STARTUPINFOW si;
  PROCESS_INFORMATION pi;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = in;
  si.hStdOutput = out;
  si.hStdError = out;
  swprintf(command, LEN(command), L"\"%ls\"", exe);
  /* The game is a console program: without a console of its own no black
   * window comes up beside the game's. The folder of the program is the
   * current one, as with run-gfx.cmd (card.bin, dumps). */
  BOOL ok = CreateProcessW(exe, command, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, g_dir, &si, &pi);
  DWORD code = GetLastError();
  CloseHandle(out);
  if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
  if (!ok) {
    swprintf(error, errorSize, L"ゲームを起動できませんでした (エラー %lu)。\n%ls", (unsigned long)code, exe);
    return NULL;
  }
  CloseHandle(pi.hThread);
  return pi.hProcess;
}

static void log_exit_code(DWORD code) {
  wchar_t log[MAX_PATH];
  char line[64];
  DWORD done;
  dir_path(log, LOG_NAME);
  HANDLE f = CreateFileW(log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
  if (f == INVALID_HANDLE_VALUE) return;
  if (code >= 0x80000000u) {
    snprintf(line, sizeof(line), "exit code 0x%08lX\r\n", (unsigned long)code);
  } else {
    snprintf(line, sizeof(line), "exit code %lu\r\n", (unsigned long)code);
  }
  WriteFile(f, line, (DWORD)strlen(line), &done, NULL);
  CloseHandle(f);
}

/* ---- window ------------------------------------------------------------- */

enum {
  ID_TAB = 100, ID_SAVE, ID_START, ID_STATUS,
  ID_ISO, ID_ISO_BROWSE, ID_CARD, ID_CARD_FILE, ID_CARD_BROWSE, ID_CLOSE_ON_START, ID_BOOT_SKIP,
  ID_ASPECT, ID_SCALE, ID_BACKEND, ID_RESIZABLE, ID_SHADOW, ID_FAST, ID_ASYNC, ID_GOODS, ID_HIRES, ID_HIRES_MAKE, ID_AA,
  ID_LINK, ID_PCB, ID_PEERS, ID_PORT, ID_ROOM, ID_SERVER,
  ID_KEYS, ID_KEY_SET1, ID_KEY_SET2, ID_KEY_CLEAR1, ID_KEY_CLEAR2, ID_KEY_RESET,
  ID_EXTRA, ID_OPEN_LOG, ID_OPEN_FOLDER,
};
#define TIMER_GAME 1
#define TIMER_HIRES 2
#define HIRES_EXE L"mkgp2-hires.exe"
#define PAGES 5

static HINSTANCE g_inst;
static HWND g_wnd, g_tab;
static HFONT g_font;
static int g_dpi = 96;
static HWND g_pageCtl[PAGES][40];
static int g_pageCount[PAGES];
static HANDLE g_game;

static const wchar_t* const kBackendIds[] = {L"", L"d3d12", L"d3d11", L"vulkan", L"opengl"};
static const wchar_t* const kBackendNames[] = {L"自動", L"Direct3D 12", L"Direct3D 11", L"Vulkan", L"OpenGL"};

static int S(int v) { return MulDiv(v, g_dpi, 96); }

static HWND mk(int page, const wchar_t* cls, const wchar_t* text, DWORD style, DWORD ex, int x, int y, int w, int h, int id) {
  HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | style | (page <= 0 ? WS_VISIBLE : 0), S(x), S(y), S(w), S(h), g_wnd,
                           (HMENU)(INT_PTR)id, g_inst, NULL);
  SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
  if (page >= 0 && g_pageCount[page] < LEN(g_pageCtl[page])) {
    g_pageCtl[page][g_pageCount[page]++] = c;
  }
  return c;
}
static HWND label(int page, const wchar_t* text, int x, int y, int w, int h) {
  return mk(page, L"STATIC", text, SS_LEFT, 0, x, y, w, h, -1);
}
static HWND check(int page, const wchar_t* text, int x, int y, int w, int id) {
  return mk(page, L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, 0, x, y, w, 20, id);
}
static HWND edit(int page, int x, int y, int w, int id) {
  return mk(page, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, x, y, w, 23, id);
}
static HWND button(int page, const wchar_t* text, int x, int y, int w, int id) {
  return mk(page, L"BUTTON", text, BS_PUSHBUTTON | WS_TABSTOP, 0, x, y, w, 26, id);
}
static HWND combo(int page, int x, int y, int w, int id) {
  return mk(page, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, x, y, w, 300, id);
}

static void set_status(const wchar_t* text) { SetDlgItemTextW(g_wnd, ID_STATUS, text); }
static int checked(int id) { return IsDlgButtonChecked(g_wnd, id) == BST_CHECKED; }

static void show_page(int page) {
  for (int p = 0; p < PAGES; p++) {
    for (int i = 0; i < g_pageCount[p]; i++) {
      ShowWindow(g_pageCtl[p][i], p == page ? SW_SHOW : SW_HIDE);
    }
  }
}

static void keys_fill(void) {
  HWND list = GetDlgItem(g_wnd, ID_KEYS);
  wchar_t buf[32];
  for (int i = 0; i < ACTIONS; i++) {
    for (int k = 0; k < KEY_SLOTS; k++) {
      LVITEMW item;
      ZeroMemory(&item, sizeof(item));
      item.iSubItem = k + 1;
      item.pszText = (wchar_t*)key_name(g_set.keys[i][k], buf, LEN(buf));
      SendMessageW(list, LVM_SETITEMTEXTW, (WPARAM)i, (LPARAM)&item);
    }
  }
}

/* The sizes on offer follow the aspect. The game makes the window
 * 480 lines times MKGP2PC_SCALE high and as wide as the ratio says (even
 * number); for 16:9 the factors are the ones that give the usual sizes. */
static const int kScales43[] = {100, 200, 300, 400, 500, 600};
static const int kScales169[] = {100, 150, 200, 225, 300, 450};
static const int* scale_list(int wide) { return wide ? kScales169 : kScales43; }
#define SCALE_CHOICES 6

static void scale_fill(int wide, int scale) {
  HWND c = GetDlgItem(g_wnd, ID_SCALE);
  const int* list = scale_list(wide);
  int best = 0;
  SendMessageW(c, CB_RESETCONTENT, 0, 0);
  for (int i = 0; i < SCALE_CHOICES; i++) {
    wchar_t text[48];
    int height = 480 * list[i] / 100;
    int width = wide ? (height * 16 / 9 + 1) & ~1 : height * 4 / 3;
    swprintf(text, LEN(text), L"%d x %d", width, height);
    SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)text);
    if (abs(list[i] - scale) < abs(list[best] - scale)) best = i;
  }
  SendMessageW(c, CB_SETCURSEL, (WPARAM)best, 0);
}
static int scale_selected(void) {
  int wide = SendDlgItemMessageW(g_wnd, ID_ASPECT, CB_GETCURSEL, 0, 0) == 1;
  int i = (int)SendDlgItemMessageW(g_wnd, ID_SCALE, CB_GETCURSEL, 0, 0);
  return scale_list(wide)[i < 0 || i >= SCALE_CHOICES ? 0 : i];
}

static void create_controls(void) {
  const int X = 24, W = 512; /* inside the tab control */
  int y;
  RECT rc;
  GetClientRect(g_wnd, &rc);

  g_tab = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP, S(8), S(8), S(544), S(404),
                          g_wnd, (HMENU)ID_TAB, g_inst, NULL);
  SendMessageW(g_tab, WM_SETFONT, (WPARAM)g_font, TRUE);
  static const wchar_t* const tabs[PAGES] = {L"基本", L"表示", L"通信", L"キー", L"その他"};
  for (int i = 0; i < PAGES; i++) {
    TCITEMW item;
    ZeroMemory(&item, sizeof(item));
    item.mask = TCIF_TEXT;
    item.pszText = (wchar_t*)tabs[i];
    SendMessageW(g_tab, TCM_INSERTITEMW, (WPARAM)i, (LPARAM)&item);
  }

  /* 基本 */
  y = 48;
  label(0, L"ISO ファイル", X, y, W, 18);
  edit(0, X, y + 20, W - 88, ID_ISO);
  button(0, L"ISO を選ぶ", X + W - 80, y + 19, 80, ID_ISO_BROWSE);
  y += 64;
  check(0, L"カードを使う", X, y, W, ID_CARD);
  label(0, L"カードのファイル", X, y + 28, W, 18);
  {
    /* the empty field shows where the card is then taken from */
    HWND e = edit(0, X, y + 48, W - 88, ID_CARD_FILE);
    SendMessageW(e, 0x1501 /* EM_SETCUEBANNER */, TRUE, (LPARAM)L"card.bin");
  }
  button(0, L"カードを選ぶ", X + W - 80, y + 47, 80, ID_CARD_BROWSE);
  y += 92;
  check(0, L"デバイスチェックと NOTICE を飛ばす", X, y, W, ID_BOOT_SKIP);
  y += 28;
  check(0, L"起動後に閉じる", X, y, W, ID_CLOSE_ON_START);

  /* 表示 */
  y = 48;
  label(1, L"画面の比率", X, y + 3, 110, 18);
  HWND c = combo(1, X + 116, y, 220, ID_ASPECT);
  SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"4:3");
  SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"16:9");
  y += 34;
  label(1, L"窓の大きさ", X, y + 3, 110, 18);
  combo(1, X + 116, y, 220, ID_SCALE);
  y += 34;
  label(1, L"描画方式", X, y + 3, 110, 18);
  c = combo(1, X + 116, y, 220, ID_BACKEND);
  for (int i = 0; i < LEN(kBackendNames); i++) {
    SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)kBackendNames[i]);
  }
  y += 34;
  label(1, L"アンチエイリアス", X, y + 3, 110, 18);
  c = combo(1, X + 116, y, 220, ID_AA);
  SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"なし");
  SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"MSAA 4x");
  SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"SSAA 2x");
  SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"MSAA 4x + SSAA 2x");
  y += 40;
  check(1, L"影をなめらかにする", X, y, W, ID_SHADOW);
  y += 28;
  check(1, L"高解像度の 2D テクスチャ", X, y, 240, ID_HIRES);
  button(1, L"作成...", X + 248, y - 3, 88, ID_HIRES_MAKE);
  y += 28;
  check(1, L"全速で開始", X, y, W, ID_FAST);
  y += 28;
  check(1, L"コイン集計を英語で表示", X, y, W, ID_GOODS);
  y += 28;
  check(1, L"描画の準備を待たない", X, y, W, ID_ASYNC);
  y += 28;
  check(1, L"窓のサイズ変更を許可 (不安定)", X, y, W, ID_RESIZABLE);

  /* 通信 */
  y = 48;
  check(2, L"筐体間通信", X, y, W, ID_LINK);
  y += 34;
  label(2, L"PCB ID", X, y + 3, 130, 18);
  c = combo(2, X + 136, y, 80, ID_PCB);
  SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"自動");
  for (int i = 1; i <= 4; i++) {
    wchar_t text[8];
    swprintf(text, LEN(text), L"%d", i);
    SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)text);
  }
  y += 40;
  label(2, L"接続先", X, y, W, 18);
  SendMessageW(edit(2, X, y + 20, W, ID_PEERS), 0x1501, TRUE, (LPARAM)L"2=192.168.1.20,3=pc3");
  y += 58;
  label(2, L"ポート", X, y + 3, 130, 18);
  SendMessageW(edit(2, X + 136, y, 80, ID_PORT), 0x1501, TRUE, (LPARAM)L"29500");
  y += 40;
  label(2, L"合言葉", X, y + 3, 130, 18);
  edit(2, X + 136, y, W - 136, ID_ROOM);
  y += 34;
  label(2, L"仲介サーバー", X, y + 3, 130, 18);
  SendMessageW(edit(2, X + 136, y, W - 136, ID_SERVER), 0x1501, TRUE, (LPARAM)L"https://...workers.dev");
  y += 40;

  /* キー */
  HWND list = mk(3, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER | WS_TABSTOP, WS_EX_CLIENTEDGE,
                 X, 44, W, 286, ID_KEYS);
  SendMessageW(list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
  static const wchar_t* const columns[] = {L"操作", L"キー 1", L"キー 2"};
  static const int widths[] = {236, 126, 126};
  for (int i = 0; i < 3; i++) {
    LVCOLUMNW col;
    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    col.pszText = (wchar_t*)columns[i];
    col.cx = S(widths[i]);
    col.iSubItem = i;
    SendMessageW(list, LVM_INSERTCOLUMNW, (WPARAM)i, (LPARAM)&col);
  }
  for (int i = 0; i < ACTIONS; i++) {
    LVITEMW item;
    ZeroMemory(&item, sizeof(item));
    item.mask = LVIF_TEXT;
    item.iItem = i;
    item.pszText = (wchar_t*)kActions[i].label;
    SendMessageW(list, LVM_INSERTITEMW, 0, (LPARAM)&item);
  }
  button(3, L"キー 1 を変更", X, 338, 100, ID_KEY_SET1);
  button(3, L"キー 2 を変更", X + 104, 338, 100, ID_KEY_SET2);
  button(3, L"キー 1 を外す", X + 208, 338, 92, ID_KEY_CLEAR1);
  button(3, L"キー 2 を外す", X + 304, 338, 92, ID_KEY_CLEAR2);
  button(3, L"キーを初期化", X + 400, 338, 112, ID_KEY_RESET);

  /* その他 */
  y = 48;
  label(4, L"追加の環境変数 (名前=値)", X, y, W, 18);
  mk(4, L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, X, y + 22,
     W, 200, ID_EXTRA);
  button(4, L"ログを開く", X, y + 262, 120, ID_OPEN_LOG);
  button(4, L"フォルダーを開く", X + 128, y + 262, 140, ID_OPEN_FOLDER);

  /* below the tabs */
  mk(-1, L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS | WS_VISIBLE, 0, 12, 426, 300, 20, ID_STATUS);
  mk(-1, L"BUTTON", L"保存", BS_PUSHBUTTON | WS_TABSTOP | WS_VISIBLE, 0, 320, 420, 90, 30, ID_SAVE);
  mk(-1, L"BUTTON", L"ゲームを起動", BS_DEFPUSHBUTTON | WS_TABSTOP | WS_VISIBLE, 0, 416, 420, 136, 30, ID_START);
  /* Children are made front to back; the tab control has to be behind
   * what stands on it (it clips them out of its own drawing). */
  SetWindowPos(g_tab, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void settings_to_window(void) {
  SetDlgItemTextW(g_wnd, ID_ISO, g_set.iso);
  CheckDlgButton(g_wnd, ID_CARD, g_set.card);
  SetDlgItemTextW(g_wnd, ID_CARD_FILE, g_set.cardFile);
  CheckDlgButton(g_wnd, ID_CLOSE_ON_START, g_set.closeOnStart);
  CheckDlgButton(g_wnd, ID_BOOT_SKIP, g_set.bootSkip);
  SendDlgItemMessageW(g_wnd, ID_ASPECT, CB_SETCURSEL, (WPARAM)g_set.wide, 0);
  scale_fill(g_set.wide, g_set.scale);
  int backend = 0;
  for (int i = 0; i < LEN(kBackendIds); i++) {
    if (wcscmp(kBackendIds[i], g_set.backend) == 0) backend = i;
  }
  SendDlgItemMessageW(g_wnd, ID_BACKEND, CB_SETCURSEL, (WPARAM)backend, 0);
  CheckDlgButton(g_wnd, ID_RESIZABLE, g_set.resizable);
  CheckDlgButton(g_wnd, ID_SHADOW, g_set.shadowSmooth);
  CheckDlgButton(g_wnd, ID_FAST, g_set.fast);
  CheckDlgButton(g_wnd, ID_ASYNC, g_set.asyncPipelines);
  CheckDlgButton(g_wnd, ID_GOODS, g_set.goodsDisc);
  CheckDlgButton(g_wnd, ID_HIRES, g_set.hires);
  SendDlgItemMessageW(g_wnd, ID_AA, CB_SETCURSEL, (WPARAM)g_set.aa, 0);
  CheckDlgButton(g_wnd, ID_LINK, g_set.link);
  SendDlgItemMessageW(g_wnd, ID_PCB, CB_SETCURSEL, (WPARAM)g_set.pcbId, 0);
  SetDlgItemTextW(g_wnd, ID_ROOM, g_set.room);
  SetDlgItemTextW(g_wnd, ID_SERVER, g_set.server);
  SetDlgItemTextW(g_wnd, ID_PEERS, g_set.peers);
  SetDlgItemTextW(g_wnd, ID_PORT, g_set.port);
  SetDlgItemTextW(g_wnd, ID_EXTRA, g_set.extra);
  keys_fill();
}

static void window_to_settings(void) {
  GetDlgItemTextW(g_wnd, ID_ISO, g_set.iso, LEN(g_set.iso));
  g_set.card = checked(ID_CARD);
  GetDlgItemTextW(g_wnd, ID_CARD_FILE, g_set.cardFile, LEN(g_set.cardFile));
  g_set.closeOnStart = checked(ID_CLOSE_ON_START);
  g_set.bootSkip = checked(ID_BOOT_SKIP);
  g_set.scale = scale_selected();
  g_set.wide = SendDlgItemMessageW(g_wnd, ID_ASPECT, CB_GETCURSEL, 0, 0) == 1;
  int backend = (int)SendDlgItemMessageW(g_wnd, ID_BACKEND, CB_GETCURSEL, 0, 0);
  wcscpy(g_set.backend, kBackendIds[backend < 0 || backend >= LEN(kBackendIds) ? 0 : backend]);
  g_set.resizable = checked(ID_RESIZABLE);
  g_set.shadowSmooth = checked(ID_SHADOW);
  g_set.fast = checked(ID_FAST);
  g_set.asyncPipelines = checked(ID_ASYNC);
  g_set.goodsDisc = checked(ID_GOODS);
  g_set.hires = checked(ID_HIRES);
  g_set.aa = clampi((int)SendDlgItemMessageW(g_wnd, ID_AA, CB_GETCURSEL, 0, 0), 0, 3, 0);
  g_set.link = checked(ID_LINK);
  g_set.pcbId = (int)SendDlgItemMessageW(g_wnd, ID_PCB, CB_GETCURSEL, 0, 0);
  GetDlgItemTextW(g_wnd, ID_ROOM, g_set.room, LEN(g_set.room));
  GetDlgItemTextW(g_wnd, ID_SERVER, g_set.server, LEN(g_set.server));
  GetDlgItemTextW(g_wnd, ID_PEERS, g_set.peers, LEN(g_set.peers));
  GetDlgItemTextW(g_wnd, ID_PORT, g_set.port, LEN(g_set.port));
  GetDlgItemTextW(g_wnd, ID_EXTRA, g_set.extra, LEN(g_set.extra));
  if (g_set.pcbId < 0) g_set.pcbId = 0;
}

static int save_from_window(void) {
  window_to_settings();
  if (!save_settings(&g_set)) {
    MessageBoxW(g_wnd, L"設定を保存できませんでした。\n" INI_NAME, L"mkgp2-pc 設定", MB_ICONWARNING);
    return 0;
  }
  set_status(L"保存しました");
  return 1;
}

static void browse(int editId, const wchar_t* title, const wchar_t* filter) {
  wchar_t path[MAX_PATH];
  OPENFILENAMEW ofn;
  GetDlgItemTextW(g_wnd, editId, path, LEN(path));
  if (!file_exists(path)) path[0] = 0; /* a stale name makes the dialog fail */
  ZeroMemory(&ofn, sizeof(ofn));
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = g_wnd;
  ofn.lpstrFilter = filter;
  ofn.lpstrFile = path;
  ofn.nMaxFile = LEN(path);
  ofn.lpstrTitle = title;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
  if (GetOpenFileNameW(&ofn)) {
    SetDlgItemTextW(g_wnd, editId, path);
  }
}

/* ---- asking for a key --------------------------------------------------- */

static int g_captured; /* usage, 0 = given up */
static int g_captureDone;

static LRESULT CALLBACK capture_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
  case WM_KEYDOWN:
  case WM_SYSKEYDOWN: {
    int scan = (int)((lp >> 16) & 0xFF), ext = (int)((lp >> 24) & 1);
    int usage = scan < 0x80 ? kScanToUsage[ext][scan] : 0;
    if (wp == VK_ESCAPE) {
      g_captureDone = 1;
    } else if (usage != 0) {
      g_captured = usage;
      g_captureDone = 1;
    } else {
      SetWindowTextW(GetDlgItem(wnd, 1), L"そのキーは使えません");
    }
    return 0;
  }
  case WM_SYSCHAR:
  case WM_SYSKEYUP:
    return 0; /* Alt and F10 must not open the menu */
  case WM_CTLCOLORSTATIC:
    SetBkMode((HDC)wp, TRANSPARENT);
    return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
  case WM_KILLFOCUS:
  case WM_CLOSE:
    g_captureDone = 1;
    return 0;
  }
  return DefWindowProcW(wnd, msg, wp, lp);
}

static int capture_key(const wchar_t* what) {
  wchar_t text[160];
  RECT rc;
  GetWindowRect(g_wnd, &rc);
  int w = S(360), h = S(130);
  HWND wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"Mkgp2KeyCapture", L"キーの割り当て", WS_POPUP | WS_CAPTION | WS_SYSMENU,
                             (rc.left + rc.right - w) / 2, (rc.top + rc.bottom - h) / 2, w, h, g_wnd, NULL, g_inst, NULL);
  swprintf(text, LEN(text), L"%ls のキーを押す", what);
  HWND st = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_CENTER, S(10), S(24), w - S(36), S(60), wnd, (HMENU)1,
                            g_inst, NULL);
  SendMessageW(st, WM_SETFONT, (WPARAM)g_font, TRUE);
  g_captured = 0;
  g_captureDone = 0;
  EnableWindow(g_wnd, FALSE);
  ShowWindow(wnd, SW_SHOW);
  SetFocus(wnd);
  MSG msg;
  while (!g_captureDone && GetMessageW(&msg, NULL, 0, 0) > 0) {
    /* no TranslateMessage: the input method must not take the key */
    DispatchMessageW(&msg);
  }
  EnableWindow(g_wnd, TRUE);
  DestroyWindow(wnd);
  SetForegroundWindow(g_wnd);
  SetFocus(GetDlgItem(g_wnd, ID_KEYS));
  return g_captured;
}

static int selected_action(void) {
  return (int)SendDlgItemMessageW(g_wnd, ID_KEYS, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
}

static void key_change(int slot, int ask) {
  int action = selected_action();
  if (action < 0 || action >= ACTIONS) {
    set_status(L"操作を選んでください");
    return;
  }
  int usage = 0;
  if (ask) {
    usage = capture_key(kActions[action].label);
    if (usage == 0) return;
    /* one key, one action: it leaves wherever else it was */
    for (int i = 0; i < ACTIONS; i++) {
      for (int k = 0; k < KEY_SLOTS; k++) {
        if (g_set.keys[i][k] == usage) g_set.keys[i][k] = 0;
      }
    }
  }
  g_set.keys[action][slot] = usage;
  keys_fill();
  set_status(L"");
}

/* ---- making the high-resolution textures --------------------------------- */

/* mkgp2-hires.exe (port/hires/mkgp2_hires.cpp) makes the folder "hires" from
 * the disc image; this window starts it, shows the lines it prints ("stage",
 * "progress", "error", "done") as a bar and can stop it. The program runs in
 * a job object, so that stopping it also stops the enlarger it may have
 * started. */

enum { HR_METHOD = 1, HR_SCALE, HR_MODELS, HR_BAR, HR_TEXT, HR_START, HR_CLOSE };

static HWND g_hr;
static HANDLE g_hrProc, g_hrJob, g_hrPipe;
static char g_hrLine[1024];
static int g_hrLen, g_hrDone, g_hrClosed, g_hrPercent;
static wchar_t g_hrStage[64], g_hrError[400];

static const struct { const char* id; const wchar_t* text; } kHiresStages[] = {
    {"read", L"読み込み"},        {"fonts", L"フォント"},          {"sprites", L"スプライト"},
    {"export", L"書き出し"},      {"download", L"ダウンロード"},   {"enlarge-rgb", L"AI 拡大 (色)"},
    {"enlarge-alpha", L"AI 拡大 (透明度)"}, {"import", L"取り込み"}, {"models", L"3D のテクスチャ"},
    {"import-models", L"取り込み (3D)"}, {"enlarge-models-rgb", L"AI 拡大 (3D、色)"},
    {"enlarge-models-alpha", L"AI 拡大 (3D、透明度)"},
};

static void hires_text(void) {
  wchar_t text[128];
  swprintf(text, LEN(text), L"%ls  %d%%", g_hrStage, g_hrPercent);
  SetDlgItemTextW(g_hr, HR_TEXT, text);
  SendDlgItemMessageW(g_hr, HR_BAR, PBM_SETPOS, (WPARAM)g_hrPercent, 0);
}

static void hires_line(const char* line) {
  if (strncmp(line, "stage ", 6) == 0) {
    MultiByteToWideChar(CP_UTF8, 0, line + 6, -1, g_hrStage, LEN(g_hrStage));
    for (int i = 0; i < LEN(kHiresStages); i++) {
      if (strcmp(line + 6, kHiresStages[i].id) == 0) wcscpy(g_hrStage, kHiresStages[i].text);
    }
    g_hrPercent = 0;
    hires_text();
  } else if (strncmp(line, "progress ", 9) == 0) {
    g_hrPercent = clampi(atoi(line + 9), 0, 100, 0);
    hires_text();
  } else if (strncmp(line, "error ", 6) == 0) {
    MultiByteToWideChar(CP_UTF8, 0, line + 6, -1, g_hrError, LEN(g_hrError));
  } else if (strncmp(line, "done ", 5) == 0) {
    g_hrDone = 1;
  }
}

static void hires_running(int running) {
  EnableWindow(GetDlgItem(g_hr, HR_METHOD), !running);
  EnableWindow(GetDlgItem(g_hr, HR_SCALE), !running);
  EnableWindow(GetDlgItem(g_hr, HR_MODELS), !running);
  EnableWindow(GetDlgItem(g_hr, HR_START), !running);
  SetDlgItemTextW(g_hr, HR_CLOSE, running ? L"中止" : L"閉じる");
}

static void hires_stop(void) {
  if (g_hrJob != NULL) TerminateJobObject(g_hrJob, 1);
}

static void hires_poll(void) {
  if (g_hrProc == NULL) return;
  for (;;) {
    char buf[512];
    DWORD avail = 0, got = 0;
    if (!PeekNamedPipe(g_hrPipe, NULL, 0, NULL, &avail, NULL) || avail == 0) break;
    if (!ReadFile(g_hrPipe, buf, avail < sizeof(buf) ? avail : sizeof(buf), &got, NULL) || got == 0) break;
    for (DWORD i = 0; i < got; i++) {
      if (buf[i] == '\n') {
        g_hrLine[g_hrLen] = 0;
        hires_line(g_hrLine);
        g_hrLen = 0;
      } else if (buf[i] != '\r' && g_hrLen < (int)sizeof(g_hrLine) - 1) {
        g_hrLine[g_hrLen++] = buf[i];
      }
    }
  }
  if (WaitForSingleObject(g_hrProc, 0) != WAIT_OBJECT_0) return;
  DWORD avail = 0;
  if (PeekNamedPipe(g_hrPipe, NULL, 0, NULL, &avail, NULL) && avail != 0) return; /* the last lines first */
  KillTimer(g_hr, TIMER_HIRES);
  CloseHandle(g_hrProc);
  CloseHandle(g_hrPipe);
  CloseHandle(g_hrJob);
  g_hrProc = g_hrPipe = g_hrJob = NULL;
  hires_running(0);
  if (g_hrDone) {
    SendDlgItemMessageW(g_hr, HR_BAR, PBM_SETPOS, 100, 0);
    SetDlgItemTextW(g_hr, HR_TEXT, L"完了");
  } else {
    SendDlgItemMessageW(g_hr, HR_BAR, PBM_SETPOS, 0, 0);
    SetDlgItemTextW(g_hr, HR_TEXT, g_hrError[0] ? g_hrError : L"中止しました");
  }
}

static void hires_start(void) {
  static const wchar_t* const models[] = {NULL, L"realesrgan-x4plus-anime", L"realesrgan-x4plus"};
  wchar_t exe[MAX_PATH], out[MAX_PATH], work[MAX_PATH], tool[MAX_PATH], cmd[2600], iso[MAX_PATH];
  int method = (int)SendDlgItemMessageW(g_hr, HR_METHOD, CB_GETCURSEL, 0, 0);
  int scale = SendDlgItemMessageW(g_hr, HR_SCALE, CB_GETCURSEL, 0, 0) == 1 ? 4 : 2;
  GetDlgItemTextW(g_wnd, ID_ISO, iso, LEN(iso));
  dir_path(exe, HIRES_EXE);
  dir_path(out, L"hires");
  dir_path(work, L"hires-ai");
  dir_path(tool, L"hires-ai\\tool");
  if (!file_exists(exe)) {
    SetDlgItemTextW(g_hr, HR_TEXT, HIRES_EXE L" がありません");
    return;
  }
  if (!file_exists(iso)) {
    SetDlgItemTextW(g_hr, HR_TEXT, L"ISO ファイルがありません");
    return;
  }
  if (g_game != NULL) {
    SetDlgItemTextW(g_hr, HR_TEXT, L"ゲームの終了後に実行してください");
    return;
  }
  if (method < 0 || method > 2) method = 0;
  int models3d = IsDlgButtonChecked(g_hr, HR_MODELS) == BST_CHECKED;
  int n = swprintf(cmd, LEN(cmd), L"\"%ls\" --iso \"%ls\" --out \"%ls\" --scale %d%ls", exe, iso, out, scale,
                   models3d ? L" --models" : L"");
  if (method != 0) {
    if (GetFileAttributesW(tool) == INVALID_FILE_ATTRIBUTES) {
      /* the one place with a sentence: fetching a program needs a yes */
      wchar_t ask[900];
      swprintf(ask, LEN(ask),
               L"AI 拡大には Real-ESRGAN (realesrgan-ncnn-vulkan) が必要です。\n"
               L"次の場所からダウンロードして %ls に置きます。\n\n"
               L"https://github.com/xinntao/Real-ESRGAN/releases/tag/v0.2.5.0\n\n"
               L"ダウンロードしますか?",
               tool);
      if (MessageBoxW(g_hr, ask, L"高解像度テクスチャの作成", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
    }
    /* the 3D models' textures are rendered and painted pictures: always the general model */
    swprintf(cmd + n, LEN(cmd) - n, L" --ai %ls%ls --work \"%ls\" --download", models[method],
             models3d ? L" --ai-models realesrgan-x4plus" : L"", work);
  }

  SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
  HANDLE rd = NULL, wr = NULL;
  if (!CreatePipe(&rd, &wr, &sa, 0)) return;
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  HANDLE job = CreateJobObjectW(NULL, NULL);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;
  ZeroMemory(&info, sizeof(info));
  info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
  STARTUPINFOW si;
  PROCESS_INFORMATION pi;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = si.hStdError = wr;
  si.hStdInput = INVALID_HANDLE_VALUE;
  BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, g_dir, &si, &pi);
  CloseHandle(wr);
  if (!ok) {
    CloseHandle(rd);
    CloseHandle(job);
    SetDlgItemTextW(g_hr, HR_TEXT, L"開始できませんでした");
    return;
  }
  AssignProcessToJobObject(job, pi.hProcess);
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);
  g_hrProc = pi.hProcess;
  g_hrJob = job;
  g_hrPipe = rd;
  g_hrLen = g_hrDone = g_hrPercent = 0;
  g_hrError[0] = 0;
  wcscpy(g_hrStage, L"開始");
  hires_text();
  hires_running(1);
  SetTimer(g_hr, TIMER_HIRES, 200, NULL);
}

static LRESULT CALLBACK hires_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
  case WM_COMMAND:
    if (LOWORD(wp) == HR_START && g_hrProc == NULL) {
      hires_start();
    } else if (LOWORD(wp) == HR_CLOSE || LOWORD(wp) == IDCANCEL) {
      if (g_hrProc != NULL) {
        hires_stop();
      } else {
        g_hrClosed = 1;
      }
    }
    return 0;
  case WM_TIMER:
    if (wp == TIMER_HIRES) hires_poll();
    return 0;
  case WM_CTLCOLORSTATIC:
    SetBkMode((HDC)wp, TRANSPARENT);
    return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
  case WM_CLOSE:
    if (g_hrProc != NULL) {
      hires_stop();
    } else {
      g_hrClosed = 1;
    }
    return 0;
  }
  return DefWindowProcW(wnd, msg, wp, lp);
}

static HWND hires_ctl(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
  HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_hr, (HMENU)(INT_PTR)id, g_inst,
                           NULL);
  SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
  return c;
}

static void hires_window(void) {
  RECT rc, need = {0, 0, S(400), S(190)};
  DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
  AdjustWindowRectEx(&need, style, FALSE, WS_EX_DLGMODALFRAME);
  int w = need.right - need.left, h = need.bottom - need.top;
  GetWindowRect(g_wnd, &rc);
  g_hr = CreateWindowExW(WS_EX_DLGMODALFRAME, L"Mkgp2Hires", L"高解像度テクスチャの作成", style, (rc.left + rc.right - w) / 2,
                         (rc.top + rc.bottom - h) / 2, w, h, g_wnd, NULL, g_inst, NULL);
  hires_ctl(L"STATIC", L"方式", SS_LEFT, 16, 20, 60, 20, -1);
  HWND c = hires_ctl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 84, 16, 300, 200, HR_METHOD);
  SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"内蔵フィルタ");
  SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"AI (Real-ESRGAN、イラスト向け)");
  SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"AI (Real-ESRGAN、汎用)");
  SendMessageW(c, CB_SETCURSEL, 0, 0);
  hires_ctl(L"STATIC", L"倍率", SS_LEFT, 16, 54, 60, 20, -1);
  c = hires_ctl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 84, 50, 120, 200, HR_SCALE);
  SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"2 倍");
  SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"4 倍");
  SendMessageW(c, CB_SETCURSEL, 0, 0);
  hires_ctl(L"BUTTON", L"3D のテクスチャも", BS_AUTOCHECKBOX | WS_TABSTOP, 220, 52, 164, 20, HR_MODELS);
  c = hires_ctl(PROGRESS_CLASSW, L"", 0, 16, 90, 368, 18, HR_BAR);
  SendMessageW(c, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
  hires_ctl(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 16, 114, 368, 20, HR_TEXT);
  hires_ctl(L"BUTTON", L"開始", BS_DEFPUSHBUTTON | WS_TABSTOP, 196, 148, 90, 28, HR_START);
  hires_ctl(L"BUTTON", L"閉じる", BS_PUSHBUTTON | WS_TABSTOP, 294, 148, 90, 28, HR_CLOSE);

  g_hrClosed = 0;
  EnableWindow(g_wnd, FALSE);
  ShowWindow(g_hr, SW_SHOW);
  MSG msg;
  while (!g_hrClosed && GetMessageW(&msg, NULL, 0, 0) > 0) {
    if (!IsDialogMessageW(g_hr, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  EnableWindow(g_wnd, TRUE);
  DestroyWindow(g_hr);
  g_hr = NULL;
  SetForegroundWindow(g_wnd);
}

/* ---- main window -------------------------------------------------------- */

static void game_started(void) {
  EnableWindow(GetDlgItem(g_wnd, ID_START), FALSE);
  set_status(L"実行中");
  SetTimer(g_wnd, TIMER_GAME, 500, NULL);
}

static void game_poll(void) {
  DWORD code = 0;
  if (g_game == NULL || WaitForSingleObject(g_game, 0) != WAIT_OBJECT_0) return;
  GetExitCodeProcess(g_game, &code);
  CloseHandle(g_game);
  g_game = NULL;
  KillTimer(g_wnd, TIMER_GAME);
  log_exit_code(code);
  wchar_t text[96];
  if (code >= 0x80000000u) {
    swprintf(text, LEN(text), L"異常終了 (0x%08lX)", (unsigned long)code);
  } else {
    swprintf(text, LEN(text), L"終了 (コード %lu)", (unsigned long)code);
  }
  set_status(text);
  EnableWindow(GetDlgItem(g_wnd, ID_START), TRUE);
}

static void open_path(const wchar_t* name) {
  wchar_t path[MAX_PATH];
  dir_path(path, name);
  if ((INT_PTR)ShellExecuteW(g_wnd, L"open", path, NULL, g_dir, SW_SHOWNORMAL) <= 32) {
    set_status(L"開けませんでした");
  }
}

static LRESULT CALLBACK main_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
  case WM_COMMAND:
    switch (LOWORD(wp)) {
    case ID_ISO_BROWSE:
      browse(ID_ISO, L"ISO ファイルを選ぶ", L"ディスクイメージ (*.iso;*.gcm)\0*.iso;*.gcm\0すべてのファイル\0*.*\0");
      break;
    case ID_CARD_BROWSE:
      browse(ID_CARD_FILE, L"カードのファイルを選ぶ", L"カード (*.bin)\0*.bin\0すべてのファイル\0*.*\0");
      break;
    case ID_ASPECT:
      if (HIWORD(wp) == CBN_SELCHANGE) {
        /* the list still shows the other ratio's sizes: read with that one */
        int wide = SendDlgItemMessageW(wnd, ID_ASPECT, CB_GETCURSEL, 0, 0) == 1;
        int i = (int)SendDlgItemMessageW(wnd, ID_SCALE, CB_GETCURSEL, 0, 0);
        scale_fill(wide, scale_list(!wide)[i < 0 || i >= SCALE_CHOICES ? 0 : i]);
      }
      break;
    case ID_HIRES_MAKE: hires_window(); break;
    case ID_SAVE: save_from_window(); break;
    case ID_START:
      if (g_game == NULL && save_from_window()) {
        wchar_t error[600];
        g_game = start_game(&g_set, error, LEN(error));
        if (g_game == NULL) {
          MessageBoxW(wnd, error, L"mkgp2-pc 設定", MB_ICONWARNING);
        } else if (g_set.closeOnStart) {
          DestroyWindow(wnd);
        } else {
          game_started();
        }
      }
      break;
    case ID_KEY_SET1: key_change(0, 1); break;
    case ID_KEY_SET2: key_change(1, 1); break;
    case ID_KEY_CLEAR1: key_change(0, 0); break;
    case ID_KEY_CLEAR2: key_change(1, 0); break;
    case ID_KEY_RESET:
      defaults_keys(&g_set);
      keys_fill();
      set_status(L"");
      break;
    case ID_OPEN_LOG: open_path(LOG_NAME); break;
    case ID_OPEN_FOLDER: open_path(L""); break;
    }
    return 0;
  case WM_NOTIFY: {
    const NMHDR* hdr = (const NMHDR*)lp;
    if (hdr->idFrom == ID_TAB && hdr->code == TCN_SELCHANGE) {
      show_page((int)SendMessageW(g_tab, TCM_GETCURSEL, 0, 0));
    } else if (hdr->idFrom == ID_KEYS && hdr->code == NM_DBLCLK) {
      const NMITEMACTIVATE* act = (const NMITEMACTIVATE*)lp;
      key_change(act->iSubItem == 2 ? 1 : 0, 1);
    }
    return 0;
  }
  case WM_TIMER:
    if (wp == TIMER_GAME) game_poll();
    return 0;
  case WM_CTLCOLORSTATIC:
    SetBkMode((HDC)wp, TRANSPARENT);
    return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(wnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show) {
  (void)prev;
  g_inst = inst;
  GetModuleFileNameW(NULL, g_dir, LEN(g_dir));
  wchar_t* slash = wcsrchr(g_dir, L'\\');
  if (slash != NULL) slash[1] = 0;
  dir_path(g_ini, INI_NAME);
  load_settings(&g_set);

  if (cmd != NULL && wcsstr(cmd, L"--run") != NULL) {
    wchar_t error[600];
    HANDLE game = start_game(&g_set, error, LEN(error));
    if (game == NULL) {
      MessageBoxW(NULL, error, L"mkgp2-pc 設定", MB_ICONWARNING);
      return 1;
    }
    CloseHandle(game);
    return 0;
  }

  INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS};
  InitCommonControlsEx(&icc);
  HDC dc = GetDC(NULL);
  g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
  ReleaseDC(NULL, dc);
  NONCLIENTMETRICSW ncm;
  ZeroMemory(&ncm, sizeof(ncm));
  ncm.cbSize = sizeof(ncm);
  if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
  }
  if (g_font == NULL) g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

  WNDCLASSEXW wc;
  ZeroMemory(&wc, sizeof(wc));
  wc.cbSize = sizeof(wc);
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
  wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wc.lpfnWndProc = main_proc;
  wc.lpszClassName = L"Mkgp2Launcher";
  RegisterClassExW(&wc);
  wc.lpfnWndProc = capture_proc;
  wc.lpszClassName = L"Mkgp2KeyCapture";
  RegisterClassExW(&wc);
  wc.lpfnWndProc = hires_proc;
  wc.lpszClassName = L"Mkgp2Hires";
  RegisterClassExW(&wc);

  DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
  RECT rc = {0, 0, S(560), S(460)};
  AdjustWindowRectEx(&rc, style, FALSE, 0);
  g_wnd = CreateWindowExW(0, L"Mkgp2Launcher", L"mkgp2-pc 設定", style, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left,
                          rc.bottom - rc.top, NULL, NULL, inst, NULL);
  create_controls();
  settings_to_window();
  show_page(0);
  wchar_t exe[MAX_PATH];
  dir_path(exe, GAME_EXE);
  if (!file_exists(exe)) {
    set_status( GAME_EXE L" がありません");
  }
  ShowWindow(g_wnd, show);
  UpdateWindow(g_wnd);

  MSG msg;
  while (GetMessageW(&msg, NULL, 0, 0) > 0) {
    if (!IsDialogMessageW(g_wnd, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  return 0;
}
