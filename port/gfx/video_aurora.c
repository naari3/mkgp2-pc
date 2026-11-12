/*
 * Frame hand-shake with Aurora for the rendering executable (replaces
 * port/nogfx/video_min.c). The game's loop ends every frame in
 * VIWaitForRetrace; that is where the frame is presented, window events
 * are read and the next frame begins.
 */
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <SDL3/SDL.h>
#include <dolphin/gx.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int OSDisableInterrupts(void);
int OSRestoreInterrupts(int);
void mkgp2pc_mediaboard_tick(void); /* port/platform/triforce/mediaboard.c */
void mkgp2pc_script_tick(uint32_t retrace); /* port/nogfx/input_script.c */

void mkgp2pc_jvs_set_switches(const uint8_t* bytes, int count);
void mkgp2pc_jvs_set_analog(int channel, uint16_t value);
void mkgp2pc_jvs_add_coin(int slot);
void mkgp2pc_baseboard_set_operator_switches(uint8_t pressed);
void mkgp2pc_card_insert(void);
int mkgp2pc_card_enabled(void);

typedef void (*RetraceCallback)(uint32_t retraceCount);

/*
 * Keyboard as the cabinet's controls (the window has to have the focus):
 *   left / right   steering wheel (turns and returns gradually)
 *   up or Z        accelerator pedal
 *   down or X      brake pedal
 *   space, enter   item button (also "decide" in the menus)
 *   5 or C         coin
 *   I              put the card (card.bin) into the card unit
 *   F1 / F2        test / service button of the cabinet
 *   P              pause / resume
 *   O              while paused: advance one frame
 *   F9             dump (see "Dumps" below); works paused or running
 *   Esc            the port's menu (port_menu.cpp); the game stands still
 *   Backspace      one selection screen back (port/platform/nav/nav.c)
 *   F8             high-resolution 2D textures on / off (docs/port/hires_textures.md)
 * These are the defaults; MKGP2PC_KEYS changes them (see key_event).
 * Once a key has been used the keyboard owns the inputs; until then a
 * script (MKGP2PC_SCRIPT) may drive them.
 */
static bool s_keyLeft, s_keyRight, s_keyAccel, s_keyBrake, s_keyItem, s_keyTest, s_keyService;
static bool s_keyboardUsed;
static int s_wheel; /* -0x7FFF .. 0x7FFF */

/*
 * Dumps. F9 writes a folder dumps/<n>_f<retrace>/ under the current
 * directory (MKGP2PC_DUMP_DIR names another place):
 *   frame.png   the frame buffer of the next frame drawn (one frame after
 *               the key when paused: the game is stepped once for it)
 *   input.txt   every input since the start, in the MKGP2PC_SCRIPT syntax.
 *               MKGP2PC_SCRIPT_FILE=<that file> plays it back, which brings
 *               another machine to the same retrace with the same inputs
 *   state.txt   retrace number, whether paused, the environment settings
 * The retrace count does not advance while paused, so a recording made with
 * pauses plays back without them.
 */
static bool s_paused, s_step, s_dumpWanted;
static char* s_rec;
static size_t s_recLen, s_recCap;
static uint32_t s_retrace;

/* at: the retrace whose script tick has to apply the entry on playback */
static void rec(uint32_t at, const char* fmt, unsigned a, unsigned b) {
  char line[64];
  int n = snprintf(line, sizeof(line), "%u ", (unsigned)at);
  n += snprintf(line + n, sizeof(line) - (size_t)n, fmt, a, b);
  line[n++] = '\n';
  if (s_recLen + (size_t)n > s_recCap) {
    s_recCap = s_recCap ? s_recCap * 2 : 1 << 16;
    s_rec = (char*)realloc(s_rec, s_recCap);
  }
  if (s_rec != NULL) {
    memcpy(s_rec + s_recLen, line, (size_t)n);
    s_recLen += (size_t)n;
  }
}

/* Inputs that came from a script are part of the session too. */
extern void (*mkgp2pc_script_record)(uint32_t at, const char* cmd);
static void rec_script(uint32_t at, const char* cmd) {
  char fmt[48];
  snprintf(fmt, sizeof(fmt), "%.40s%%.0u%%.0u", cmd);
  rec(at, fmt, 0, 0);
}

static void rec_analog(int channel, uint16_t value) {
  static int last[3] = {-1, -1, -1};
  if (last[channel] != value) {
    last[channel] = value;
    rec(s_retrace, "an%u=%x", (unsigned)channel, value);
  }
  mkgp2pc_jvs_set_analog(channel, value);
}
static void rec_switch(uint8_t p1) {
  static int last = -1;
  uint8_t sw[5] = {0, 0, p1, 0, 0};
  if (last != p1) {
    last = p1;
    rec(s_retrace, "sw=00,00,%02x%.0u", p1, 0);
  }
  mkgp2pc_jvs_set_switches(sw, 5);
}
static void rec_operator(uint8_t pressed) {
  static int last = -1;
  if (last != pressed) {
    last = pressed;
    rec(s_retrace, "op=%x%.0u", pressed, 0);
  }
  mkgp2pc_baseboard_set_operator_switches(pressed);
}

/* PNG with stored (uncompressed) blocks: no library needed. */
static uint32_t crc_table[256];
static uint32_t crc_update(uint32_t c, const uint8_t* p, size_t n) {
  if (crc_table[1] == 0) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t v = i;
      for (int k = 0; k < 8; k++) {
        v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
      }
      crc_table[i] = v;
    }
  }
  while (n--) {
    c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
  }
  return c;
}
static void png_chunk(FILE* f, const char* type, const uint8_t* data, uint32_t len) {
  uint8_t head[8] = {(uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len,
                     (uint8_t)type[0],     (uint8_t)type[1],     (uint8_t)type[2],    (uint8_t)type[3]};
  uint32_t c = crc_update(0xFFFFFFFFu, head + 4, 4);
  c = crc_update(c, data, len) ^ 0xFFFFFFFFu;
  uint8_t tail[4] = {(uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c};
  fwrite(head, 1, 8, f);
  fwrite(data, 1, len, f);
  fwrite(tail, 1, 4, f);
}
static bool png_write(const char* path, const uint8_t* rgba, uint32_t w, uint32_t h) {
  FILE* f = fopen(path, "wb");
  if (f == NULL) {
    return false;
  }
  const uint32_t row = 1 + w * 3;
  const uint32_t raw = row * h;
  const uint32_t blocks = (raw + 65534) / 65535;
  const uint32_t zlen = 2 + raw + blocks * 5 + 4;
  uint8_t* z = (uint8_t*)malloc(zlen);
  uint8_t* scan = (uint8_t*)malloc(raw);
  if (z == NULL || scan == NULL) {
    fclose(f);
    free(z);
    free(scan);
    return false;
  }
  uint32_t a = 1, b = 0;
  for (uint32_t y = 0; y < h; y++) {
    uint8_t* d = scan + y * row;
    *d++ = 0;
    for (uint32_t x = 0; x < w; x++) {
      const uint8_t* s = rgba + ((size_t)y * w + x) * 4;
      *d++ = s[0];
      *d++ = s[1];
      *d++ = s[2];
    }
  }
  for (uint32_t i = 0; i < raw; i++) {
    a = (a + scan[i]) % 65521;
    b = (b + a) % 65521;
  }
  uint8_t* o = z;
  *o++ = 0x78;
  *o++ = 0x01;
  for (uint32_t off = 0; off < raw; off += 65535) {
    uint32_t n = raw - off < 65535 ? raw - off : 65535;
    *o++ = off + n == raw;
    *o++ = (uint8_t)n;
    *o++ = (uint8_t)(n >> 8);
    *o++ = (uint8_t)~n;
    *o++ = (uint8_t)(~n >> 8);
    memcpy(o, scan + off, n);
    o += n;
  }
  *o++ = (uint8_t)(b >> 8);
  *o++ = (uint8_t)b;
  *o++ = (uint8_t)(a >> 8);
  *o++ = (uint8_t)a;
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
  uint8_t ihdr[13] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                      (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h,
                      8,                  2,                  0,                 0,
                      0};
  fwrite(sig, 1, 8, f);
  png_chunk(f, "IHDR", ihdr, 13);
  png_chunk(f, "IDAT", z, (uint32_t)(o - z));
  png_chunk(f, "IEND", ihdr, 0);
  fclose(f);
  free(z);
  free(scan);
  return true;
}

bool mkgp2pc_window_shot(SDL_Window* window, uint8_t** rgba, uint32_t* width, uint32_t* height); /* shot_win32.c */
static SDL_Window* s_window;
static int s_shotWait; /* polls left before the window's own picture is taken instead */
static AuroraReadbackStream s_shotStream;
static uint8_t* s_shotPixels;
static uint32_t s_shotW, s_shotH;
static char s_shotPath[600];
static int s_dumpCount;
static bool s_fast, s_fastHeld; /* F10 and Tab: no frame limit */

void mkgp2pc_shadow_record_frame(void);  /* gx_shim.cpp */
void mkgp2pc_shadow_record_write(FILE* f);
/* the game's own debug facilities: port/platform/debug/debug.c, debug_overlay.cpp */
int mkgp2pc_debug_on(void);
void mkgp2pc_debug_set(int on);
void mkgp2pc_debug_frame(void);
void mkgp2pc_debug_page(int step);
int mkgp2pc_debug_overlay_get(int index, int* layer, int* column, int* row, const char** text);
void mkgp2pc_debug_overlay_clear(void);
void mkgp2pc_debug_overlay_draw(void);

static void dump_text(const char* dir) {
  char path[600];
  snprintf(path, sizeof(path), "%s/input.txt", dir);
  FILE* f = fopen(path, "wb");
  if (f != NULL) {
    if (s_rec != NULL) {
      fwrite(s_rec, 1, s_recLen, f);
    }
    fclose(f);
  }
  snprintf(path, sizeof(path), "%s/state.txt", dir);
  f = fopen(path, "w");
  if (f != NULL) {
    static const char* const names[] = {"MKGP2PC_SCRIPT", "MKGP2PC_SCRIPT_FILE", "MKGP2PC_NOLIMIT", "MKGP2PC_FRAMES",
                                       "MKGP2PC_CARD", "MKGP2PC_CARD_FILE"};
    fprintf(f, "retrace %u\npaused %d\nframe.png shows retrace %u\n", (unsigned)s_retrace, (int)s_paused,
            (unsigned)s_retrace + 1);
    for (int i = 0; i < 6; i++) {
      const char* v = getenv(names[i]);
      fprintf(f, "%s=%s\n", names[i], v ? v : "");
    }
    mkgp2pc_shadow_record_write(f);
    /* what the game's debug overlay says on this frame */
    for (int i = 0;; i++) {
      int layer, column, row;
      const char* text;
      if (!mkgp2pc_debug_overlay_get(i, &layer, &column, &row, &text)) {
        break;
      }
      fprintf(f, "overlay layer %d column %d row %d: %s\n", layer, column, row, text);
    }
    fclose(f);
  }
}

/* Called with the frame drawn and not yet handed over. */
static void dump_capture(void) {
  const char* base = getenv("MKGP2PC_DUMP_DIR");
  char dir[512];
  if (base == NULL || *base == 0) {
    base = "dumps";
  }
  SDL_CreateDirectory(base);
  snprintf(dir, sizeof(dir), "%s/%03d_f%u", base, ++s_dumpCount, (unsigned)s_retrace);
  SDL_CreateDirectory(dir);
  dump_text(dir);
  if (s_shotStream == 0) {
    s_shotStream = AuroraReadbackCreateStream();
  }
  free(s_shotPixels);
  s_shotPixels = NULL;
  /* A key of its own for every dump: a stream keeps the result of its last
   * finished capture, and with one fixed key the second dump was handed the
   * first dump's picture at once. */
  if (AuroraReadbackCaptureColor(s_shotStream, (uint64_t)s_dumpCount, &s_shotW, &s_shotH) && s_shotW != 0 &&
      s_shotH != 0) {
    s_shotPixels = (uint8_t*)malloc((size_t)s_shotW * s_shotH * 4);
  }
  snprintf(s_shotPath, sizeof(s_shotPath), "%s/frame.png", dir);
  s_shotWait = 40;
  fprintf(stderr, "dump: %s (inputs and state written)\n", dir);
}

/* The picture arrives when the GPU has finished with that frame. Where
 * Aurora cannot read back (it says so in the log at start-up) the window's
 * own picture is taken after a short wait: the frame on screen then, which
 * is the dumped one when paused and a slightly later one when running. */
static void dump_poll(void) {
  if (s_shotWait == 0) {
    return;
  }
  bool ok = false;
  if (s_shotPixels != NULL) {
    AuroraReadbackDest dest = {(uint64_t)s_dumpCount, s_shotPixels, s_shotW * s_shotH * 4, GX_FALSE};
    if (AuroraReadbackGet(s_shotStream, &dest, 1) && dest.found) {
      ok = png_write(s_shotPath, s_shotPixels, s_shotW, s_shotH);
      s_shotWait = 1;
    }
  }
  if (--s_shotWait != 0) {
    return;
  }
  if (!ok) {
    uint8_t* pixels = NULL;
    uint32_t w = 0, h = 0;
    if (mkgp2pc_window_shot(s_window, &pixels, &w, &h)) {
      ok = png_write(s_shotPath, pixels, w, h);
      free(pixels);
    }
  }
  fprintf(stderr, "dump: %s %s\n", s_shotPath, ok ? "written" : "not written (no way to read the picture here)");
  free(s_shotPixels);
  s_shotPixels = NULL;
}

/* port_menu.cpp, port_ui.cpp */
bool mkgp2pc_menu_is_open(void);
void mkgp2pc_menu_set_open(bool open);
void mkgp2pc_menu_key(int key);
void mkgp2pc_menu_draw(void);
void mkgp2pc_ui_init(const AuroraWindowSize* size);
void mkgp2pc_script_run(const char* cmd, uint32_t at); /* port/nogfx/input_script.c */

static void menu_open(bool open) {
  if (open) {
    /* nothing stays held down behind the menu */
    s_keyLeft = s_keyRight = s_keyAccel = s_keyBrake = s_keyItem = s_keyTest = s_keyService = false;
    s_fastHeld = false;
  }
  mkgp2pc_menu_set_open(open);
}

/* For the menu: a script command applied to the frame that follows, and
 * written into the recording by the script module. */
void mkgp2pc_video_command(const char* cmd) { mkgp2pc_script_run(cmd, s_retrace + 1); }

/* The game's main loop asks for flow requests once per frame; before it (the
 * boot checks) and in loops of its own (test mode) nobody would take one. */
static bool s_mainLoopRuns;
bool mkgp2pc_video_main_loop_runs(void) { return s_mainLoopRuns; }
unsigned mkgp2pc_nav_flow_ticks(void);

extern int (*mkgp2pc_script_extra)(const char* cmd);
static int script_extra(const char* cmd) {
  if (strcmp(cmd, "menu") == 0) {
    menu_open(!mkgp2pc_menu_is_open());
  } else if (strcmp(cmd, "pause") == 0) {
    s_paused = !s_paused;
    fprintf(stderr, "script: %s at retrace %u\n", s_paused ? "paused" : "resumed", (unsigned)s_retrace);
  } else if (strcmp(cmd, "step") == 0) {
    s_step = true;
  } else if (strcmp(cmd, "dump") == 0) {
    s_dumpWanted = true;
  } else if (strcmp(cmd, "debug") == 0) {
    mkgp2pc_debug_set(!mkgp2pc_debug_on());
  } else if (strcmp(cmd, "debugpage") == 0) {
    mkgp2pc_debug_page(1);
  } else {
    return 0;
  }
  return 1;
}

/*
 * Which key does what. MKGP2PC_KEYS replaces the keys of the actions it
 * names and leaves the others as they are:
 *   MKGP2PC_KEYS="accel=A;brake=S,Left Shift;coin="
 * action=key[,key...] separated by ';'. A key is an SDL scancode name
 * ("A", "Left", "F9", "Keypad 5"; a physical key, whatever the layout
 * prints on it) or its number (the USB keyboard usage, 4 for A). Nothing
 * after '=' leaves the action without a key. One character is always a
 * name ("5" is the key 5); the settings program (port/launcher) writes
 * numbers with two digits or more ("04").
 */
enum {
  KEY_LEFT, KEY_RIGHT, KEY_ACCEL, KEY_BRAKE, KEY_ITEM, KEY_TEST, KEY_SERVICE,
  KEY_COIN, KEY_CARD, KEY_PAUSE, KEY_STEP, KEY_FAST, KEY_FASTHOLD, KEY_DUMP,
  KEY_MENU, KEY_BACK, KEY_HIRES, KEY_DEBUG, KEY_DEBUGPAGE,
  KEY_ACTIONS
};
#define KEYS_PER_ACTION 4
static const char* const s_actionNames[KEY_ACTIONS] = {
    "left", "right", "accel", "brake", "item", "test", "service",
    "coin", "card", "pause", "step", "fast", "fasthold", "dump", "menu", "back", "hires",
    "debug", "debugpage"};
static int s_keyMap[KEY_ACTIONS][KEYS_PER_ACTION] = {
    {SDL_SCANCODE_LEFT}, {SDL_SCANCODE_RIGHT}, {SDL_SCANCODE_UP, SDL_SCANCODE_Z},
    {SDL_SCANCODE_DOWN, SDL_SCANCODE_X}, {SDL_SCANCODE_SPACE, SDL_SCANCODE_RETURN},
    {SDL_SCANCODE_F1}, {SDL_SCANCODE_F2}, {SDL_SCANCODE_5, SDL_SCANCODE_C},
    {SDL_SCANCODE_I}, {SDL_SCANCODE_P}, {SDL_SCANCODE_O}, {SDL_SCANCODE_F10},
    {SDL_SCANCODE_TAB}, {SDL_SCANCODE_F9}, {SDL_SCANCODE_ESCAPE}, {SDL_SCANCODE_BACKSPACE},
    {SDL_SCANCODE_F8}, {SDL_SCANCODE_F3}, {SDL_SCANCODE_F4}};

void mkgp2pc_hires_init(void); /* gx_shim.cpp */
void mkgp2pc_hires_toggle(void);

static void key_map_load(void) {
  const char* p = getenv("MKGP2PC_KEYS");
  while (p != NULL && *p != 0) {
    size_t len = strcspn(p, ";");
    const char* eq = memchr(p, '=', len);
    int action = -1;
    for (int i = 0; eq != NULL && i < KEY_ACTIONS; i++) {
      if (strlen(s_actionNames[i]) == (size_t)(eq - p) && strncmp(s_actionNames[i], p, (size_t)(eq - p)) == 0) {
        action = i;
      }
    }
    if (action < 0) {
      if (len != 0) fprintf(stderr, "MKGP2PC_KEYS: unknown entry '%.*s'\n", (int)len, p);
    } else {
      const char* k = eq + 1;
      const char* end = p + len;
      int n = 0;
      memset(s_keyMap[action], 0, sizeof(s_keyMap[action]));
      while (k < end) {
        char name[32];
        size_t klen = strcspn(k, ",;");
        char* stop;
        long code;
        snprintf(name, sizeof(name), "%.*s", (int)klen, k);
        code = strtol(name, &stop, 10);
        if (*stop != 0 || stop == name || klen == 1) {
          code = SDL_GetScancodeFromName(name);
        }
        if (code <= 0 || code >= SDL_SCANCODE_COUNT) {
          if (klen != 0) fprintf(stderr, "MKGP2PC_KEYS: unknown key '%s'\n", name);
        } else if (n < KEYS_PER_ACTION) {
          s_keyMap[action][n++] = (int)code;
        }
        k += klen;
        if (k < end) k++;
      }
    }
    p += len;
    if (*p == ';') p++;
  }
}

static void key_event(const SDL_Event* e) {
  bool down = e->type == SDL_EVENT_KEY_DOWN;
  bool press = down && !e->key.repeat;
  bool control = false;
  if (mkgp2pc_menu_is_open()) {
    /* The menu has the keyboard: the arrow keys, Enter and Esc always work,
     * and so do the keys given to accelerator / brake (up / down), item
     * (decide), menu and back (close). */
    int key = 0;
    if (down) {
      switch (e->key.scancode) {
      case SDL_SCANCODE_UP: key = 1; break;
      case SDL_SCANCODE_DOWN: key = 2; break;
      case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: key = 3; break;
      case SDL_SCANCODE_ESCAPE: key = 4; break;
      default: break;
      }
      static const int same[][2] = {{KEY_ACCEL, 1}, {KEY_BRAKE, 2}, {KEY_ITEM, 3}, {KEY_MENU, 4}, {KEY_BACK, 4}};
      for (int a = 0; key == 0 && a < 5; a++) {
        for (int i = 0; i < KEYS_PER_ACTION; i++) {
          if (s_keyMap[same[a][0]][i] != 0 && s_keyMap[same[a][0]][i] == (int)e->key.scancode) {
            key = same[a][1];
          }
        }
      }
    }
    if (key != 0 && (!e->key.repeat || key <= 2)) {
      mkgp2pc_menu_key(key);
    }
    return;
  }
  for (int action = 0; action < KEY_ACTIONS; action++) {
    bool hit = false;
    for (int i = 0; i < KEYS_PER_ACTION; i++) {
      hit |= s_keyMap[action][i] != 0 && s_keyMap[action][i] == (int)e->key.scancode;
    }
    if (!hit) {
      continue;
    }
    switch (action) {
    case KEY_LEFT: s_keyLeft = down; control = true; break;
    case KEY_RIGHT: s_keyRight = down; control = true; break;
    case KEY_ACCEL: s_keyAccel = down; control = true; break;
    case KEY_BRAKE: s_keyBrake = down; control = true; break;
    case KEY_ITEM: s_keyItem = down; control = true; break;
    case KEY_TEST: s_keyTest = down; control = true; break;
    case KEY_SERVICE: s_keyService = down; control = true; break;
    case KEY_COIN:
      if (press) {
        rec(s_retrace + 1, "coin%.0u%.0u", 0, 0);
        mkgp2pc_jvs_add_coin(0);
      }
      control = true;
      break;
    case KEY_CARD:
      if (press) {
        rec(s_retrace + 1, "card%.0u%.0u", 0, 0);
        mkgp2pc_card_insert();
      }
      control = true;
      break;
    case KEY_PAUSE:
      if (press) {
        s_paused = !s_paused;
        fprintf(stderr, "%s at retrace %u\n", s_paused ? "paused" : "resumed", (unsigned)s_retrace);
      }
      break;
    case KEY_STEP:
      if (down && s_paused) {
        s_step = true;
      }
      break;
    case KEY_FAST: /* as fast as the machine goes, until pressed again */
      if (press) {
        s_fast = !s_fast;
        fprintf(stderr, "frame limit %s at retrace %u\n", s_fast ? "off" : "on", (unsigned)s_retrace);
      }
      break;
    case KEY_FASTHOLD: /* the same while held */
      s_fastHeld = down;
      break;
    case KEY_MENU:
      if (press) {
        menu_open(true);
      }
      break;
    case KEY_BACK:
      if (press) {
        mkgp2pc_video_command("back");
      }
      break;
    case KEY_DUMP:
      if (press) {
        s_dumpWanted = true;
        if (s_paused) {
          s_step = true; /* a frame has to be drawn for the picture */
        }
      }
      break;
    case KEY_HIRES: /* high-resolution 2D textures on and off (gx_shim.cpp) */
      if (press) {
        mkgp2pc_hires_toggle();
      }
      break;
    case KEY_DEBUG: /* the game's own debug output and overlay (docs/port/debug.md) */
      if (press) {
        mkgp2pc_debug_set(!mkgp2pc_debug_on());
      }
      break;
    case KEY_DEBUGPAGE:
      if (press) {
        mkgp2pc_debug_page((SDL_GetModState() & SDL_KMOD_SHIFT) != 0 ? -1 : 1);
      }
      break;
    }
  }
  if (control) {
    s_keyboardUsed = true;
  }
}

static void keyboard_tick(void) {
  if (!s_keyboardUsed) {
    return;
  }
  int target = s_keyLeft == s_keyRight ? 0 : s_keyRight ? 0x7FFF : -0x7FFF;
  int step = target == 0 ? 0x1000 : 0x0C00; /* full lock in about 10 frames */
  if (s_wheel < target) {
    s_wheel = s_wheel + step > target ? target : s_wheel + step;
  } else if (s_wheel > target) {
    s_wheel = s_wheel - step < target ? target : s_wheel - step;
  }
  rec_analog(0, (uint16_t)(0x8000 + s_wheel));
  rec_analog(1, s_keyAccel ? 0xFFFF : 0);
  rec_analog(2, s_keyBrake ? 0xFFFF : 0);
  rec_switch((uint8_t)(s_keyItem ? 0x20 : 0));
  /* 0x01: the DIP switch that skips the card reader, printer and camera */
  rec_operator((uint8_t)((mkgp2pc_card_enabled() ? 0 : 0x01) | (s_keyTest ? 0x40 : 0) | (s_keyService ? 0x80 : 0)));
}

static RetraceCallback s_pre, s_post;
static bool s_inFrame;
static long s_frameLimit = -1;

static char s_cacheMarker[1024]; /* see cache_check */
static void log_callback(AuroraLogLevel level, const char* module, const char* message, unsigned int len) {
  static const char* const names[] = {"DEBUG", "INFO", "WARNING", "ERROR", "FATAL"};
  (void)len;
  fprintf(stderr, "[%s] %s: %s\n", names[level], module, message);
  /* What the start-up check did not catch: both caches are replaced at the
   * next start (cache_check). Aurora itself goes on without the cache. */
  if (level >= LOG_ERROR && s_cacheMarker[0] != 0 && strstr(module, "cache") != NULL &&
      strstr(message, "malformed") != NULL) {
    FILE* f = fopen(s_cacheMarker, "w");
    if (f != NULL) {
      fclose(f);
      fprintf(stderr, "cache: damaged while running; both cache files are replaced at the next start\n");
    }
    s_cacheMarker[0] = 0;
  }
  if (level == LOG_FATAL) {
    abort();
  }
}

/* The link wraps main (ld --wrap, port/tools/link_census.py): Aurora has to
 * be up before the game's start-up calls GXInit and VIInit, and after the
 * C++ static objects of Aurora itself exist (so not from a constructor). */
int __real_main(int argc, char** argv);

/* Aurora compiles a draw's pipeline on a worker thread the first time the
 * draw's state is seen and, by default, drops the draw until that is done:
 * after a scene change the models, each with new state, appeared one after
 * another over the first frames. Blocking draws wait for the pipeline, so
 * the first frame of a scene is complete (it may take longer once; compiled
 * pipelines are kept in the cache file). MKGP2PC_ASYNC_PIPELINES=1 gives the
 * dropping behaviour back. The setting is GX state, sent with each frame. */
static void begin_frame_state(void) {
  static int blocking = -1;
  if (blocking < 0) {
    blocking = getenv("MKGP2PC_ASYNC_PIPELINES") == NULL;
  }
  if (s_inFrame) {
    AuroraSetDrawsBlocking(blocking != 0);
  }
}
/*
 * Aurora keeps two SQLite files in the user directory (%APPDATA%\mkgp2-pc,
 * ~/.local/share/mkgp2-pc): pipeline_cache.db, the list of pipelines seen so
 * far (compiled in the background at the next start), and dawn_cache.db, the
 * compiled shaders and pipelines themselves. When a file is damaged Aurora
 * says so once in the log and goes on WITHOUT that cache, at every start:
 * with dawn_cache.db gone every shader is compiled again in every run, and
 * since draws wait for their pipeline (begin_frame_state) each compilation
 * the game runs into is a stall (found 2026-11-12 on Windows: "database disk
 * image is malformed", docs/port/census.md section 36). So the files are
 * checked before Aurora opens them; a damaged one is set aside as
 * <name>.broken and Aurora starts a new one. sqlite3 is in the link for
 * Aurora; the four declarations stand for its header.
 */
typedef struct sqlite3 sqlite3;
int sqlite3_open_v2(const char* filename, sqlite3** db, int flags, const char* vfs);
int sqlite3_exec(sqlite3* db, const char* sql, int (*callback)(void*, int, char**, char**), void* arg, char** error);
int sqlite3_close(sqlite3* db);
const char* sqlite3_errmsg(sqlite3* db);
static int cache_check_row(void* state, int count, char** values, char** names) {
  (void)names;
  *(int*)state = count >= 1 && values[0] != NULL && strcmp(values[0], "ok") == 0 ? 1 : 2;
  return 0;
}
/* s_cacheMarker: a file whose existence tells the next start that Aurora
 * reported a damaged cache during the run (log_callback). */
static void cache_check_file(const char* dir, const char* name, bool reported) {
  char path[1024], other[1040];
  sqlite3* db = NULL;
  int state = 0; /* 1: the check said ok */
  char why[200] = "";
  snprintf(path, sizeof(path), "%s%s", dir, name);
  if (!SDL_GetPathInfo(path, NULL)) {
    return; /* first start */
  }
  /* integrity_check, not quick_check: the quick one leaves the indexes
   * out, and a file that passed it failed on the first row written
   * (pipeline_cache.db, Windows 2026-11-12). */
  if (reported) {
    snprintf(why, sizeof(why), "reported by Aurora in the last run");
    state = 2;
  } else if (sqlite3_open_v2(path, &db, 2 /* SQLITE_OPEN_READWRITE */, NULL) != 0 ||
             sqlite3_exec(db, "PRAGMA integrity_check(1);", cache_check_row, &state, NULL) != 0) {
    snprintf(why, sizeof(why), "%s", db != NULL ? sqlite3_errmsg(db) : "cannot open");
    state = 2;
  }
  if (db != NULL) {
    sqlite3_close(db);
  }
  if (state == 1) {
    return;
  }
  snprintf(other, sizeof(other), "%s.broken", path);
  SDL_RemovePath(other);
  bool moved = SDL_RenamePath(path, other);
  if (!moved) {
    moved = SDL_RemovePath(path);
  }
  snprintf(other, sizeof(other), "%s-wal", path);
  SDL_RemovePath(other);
  snprintf(other, sizeof(other), "%s-shm", path);
  SDL_RemovePath(other);
  fprintf(stderr, "cache: %s is damaged (%s); %s\n", path, why[0] ? why : "failed the check",
          moved ? "set aside as .broken, a new one is started (shaders are compiled again once)"
                : "and could not be moved away: this run has no shader cache");
}
static void cache_check(const char* appName) {
  if (getenv("MKGP2PC_NO_CACHE_CHECK") != NULL) {
    return;
  }
  char* dir = SDL_GetPrefPath(NULL, appName); /* what Aurora uses when no cache path is given */
  if (dir == NULL) {
    return;
  }
  Uint64 start = SDL_GetTicksNS();
  snprintf(s_cacheMarker, sizeof(s_cacheMarker), "%scache_damaged", dir);
  bool reported = SDL_GetPathInfo(s_cacheMarker, NULL);
  cache_check_file(dir, "dawn_cache.db", reported);
  cache_check_file(dir, "pipeline_cache.db", reported);
  SDL_RemovePath(s_cacheMarker);
  fprintf(stderr, "cache: %s checked in %.0f ms\n", dir, (double)(SDL_GetTicksNS() - start) / 1e6);
  SDL_free(dir);
}

/* Wide picture (MKGP2PC_ASPECT, docs/port/widescreen.md). The rendering
 * library is told to let the frame buffer take the window's shape and to
 * spread the game's 640x480 over it; gx_shim.cpp narrows the game's
 * projections again by mkgp2pc_wide_k, which follows the frame buffer's
 * real shape every frame. */
static bool s_wide;
void mkgp2pc_wide_frame(float k); /* gx_shim.cpp */
static void wide_frame(void) {
  if (!s_wide) {
    return;
  }
  AuroraWindowSize size = aurora_get_window_size();
  if (size.fb_width == 0 || size.fb_height == 0) {
    return;
  }
  float k = ((float)size.fb_width / (float)size.fb_height) / (4.0f / 3.0f);
  mkgp2pc_wide_frame(k > 1.001f ? k : 1.0f);
}
void mkgp2pc_crash_log_install(void); /* shot_win32.c */
static void quit_now(void);
int __wrap_main(int argc, char** argv) {
  mkgp2pc_crash_log_install();
  AuroraConfig config;
  memset(&config, 0, sizeof(config));
  config.appName = "mkgp2-pc";
  config.logCallback = &log_callback;
  config.imGuiInitCallback = &mkgp2pc_ui_init;
  /* The picture shown is the copy the game makes of its frame buffer at
   * the end of a frame (GXCopyDisp), as on the console, not the frame
   * buffer itself: the copy stays when no new frame is drawn, which is what
   * the port's menu stands on while the game is stopped. Without it the
   * menu has a black background. MKGP2PC_XFB=0 turns it off. */
  {
    const char* xfb = getenv("MKGP2PC_XFB");
    config.emulateXfb = xfb == NULL || strcmp(xfb, "0") != 0;
  }
  /* MKGP2PC_SCALE: the window's height in units of 480 lines (2 opens
   * 1280x960); fractions are taken (1.5 is 720 lines). MKGP2PC_ASPECT=16:9
   * (any W:H wider than 4:3) asks for a wide picture: the window gets that
   * shape and wide_frame() above does the rest. Width: height x W / H,
   * rounded down, then up to an even number (854, 1280, 1706, 1920). */
  const char* scaleText = getenv("MKGP2PC_SCALE");
  double scale = scaleText != NULL ? strtod(scaleText, NULL) : 1;
  if (!(scale >= 0.5 && scale <= 8)) {
    scale = 1;
  }
  int aspectW = 4, aspectH = 3;
  const char* aspectText = getenv("MKGP2PC_ASPECT");
  if (aspectText != NULL) {
    int w = 0, h = 0;
    if (sscanf(aspectText, "%d:%d", &w, &h) == 2 && w > 0 && h > 0 && w * 3 > h * 4 && w <= h * 4) {
      aspectW = w;
      aspectH = h;
      s_wide = true;
    } else if (strcmp(aspectText, "4:3") != 0) {
      fprintf(stderr, "MKGP2PC_ASPECT: '%s' is not a shape wider than 4:3 (W:H); using 4:3\n", aspectText);
    }
  }
  config.windowHeight = (uint32_t)(480 * scale + 0.5);
  config.windowWidth = (uint32_t)((uint64_t)config.windowHeight * aspectW / aspectH);
  config.windowWidth += config.windowWidth & 1;
  /* MKGP2PC_BACKEND picks the rendering backend instead of Aurora's own
   * order (Windows: d3d12 first). */
  const char* backend = getenv("MKGP2PC_BACKEND");
  if (backend != NULL) {
    if (strcmp(backend, "d3d12") == 0) config.desiredBackend = BACKEND_D3D12;
    else if (strcmp(backend, "d3d11") == 0) config.desiredBackend = BACKEND_D3D11;
    else if (strcmp(backend, "vulkan") == 0) config.desiredBackend = BACKEND_VULKAN;
    else if (strcmp(backend, "opengl") == 0) config.desiredBackend = BACKEND_OPENGL;
    else fprintf(stderr, "MKGP2PC_BACKEND: unknown backend '%s'\n", backend);
  }
  /* (0, 0) would put the client area in the screen corner and the title
   * bar above the screen: nothing to drag the window by. Negative leaves
   * the place to the system. */
  config.windowPosX = -1;
  config.windowPosY = -1;
  /* a log that survives a crash (gfx.log was empty after one on Windows) */
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  s_fast = getenv("MKGP2PC_FAST") != NULL; /* start at full speed, as after F10 */
  key_map_load();
  const char* limit = getenv("MKGP2PC_FRAMES");
  if (limit != NULL) {
    s_frameLimit = strtol(limit, NULL, 10);
  }
  mkgp2pc_script_extra = &script_extra;
  mkgp2pc_script_record = &rec_script;
  cache_check(config.appName);
  s_window = aurora_initialize(argc, argv, &config).window;
  mkgp2pc_hires_init();
#ifdef _WIN32
  /* Every change of the window's size on Windows (drag, maximize) ended in
   * webgpu_dawn.dll, in the surface reconfiguration that Aurora's present
   * step does after a resize (crash record 2026-11-12, docs/port/build.md).
   * Until that is understood the window keeps its size; MKGP2PC_SCALE sets
   * it, MKGP2PC_RESIZABLE=1 gives the frame back. */
  if (s_window != NULL && getenv("MKGP2PC_RESIZABLE") == NULL) {
    SDL_SetWindowResizable(s_window, false);
  }
#endif
  if (s_wide) {
    AuroraSetViewportPolicy(AURORA_VIEWPORT_STRETCH);
    fprintf(stderr, "wide picture: %d:%d, window %ux%u\n", aspectW, aspectH, config.windowWidth, config.windowHeight);
  }
  s_inFrame = aurora_begin_frame();
  begin_frame_state();
  wide_frame();
  __real_main(argc, argv);
  /* The game's main returned (it does not while OSResetSystem restarts the
   * program). Returning from here would run the static destructors, and
   * Aurora's end in an access violation on Windows: its staging buffers'
   * map callbacks lock a mutex that was destroyed before them (crash record
   * 2026-11-12 after the test button, docs/port/census.md section 37). */
  fprintf(stderr, "main returned; leaving\n");
  quit_now();
  return 0;
}

void mkgp2pc_audio_frame(void); /* audio_musyx.c */

/* The game counts on 59.94 retraces a second: its logic advances once per
 * frame. The display's own rate may be anything (or the presentation may not
 * wait at all), so the pace is kept here. MKGP2PC_NOLIMIT=1 turns it off
 * (scripted runs). */
/* Leaving: the process ends at once, without the rendering library's
 * shutdown. On Windows every close of the window ended in an access
 * violation inside webgpu_dawn.dll during that shutdown (same place five
 * times on 2026-11-11, after "Buffer mapping Aborted" when a dump had been
 * made): Windows then showed the window as not responding and wrote a crash
 * record. Nothing of the game's needs the orderly path: the card file is
 * written when the game writes the card, the logs are unbuffered. */
#ifdef _WIN32
__declspec(dllimport) void* __stdcall GetCurrentProcess(void);
__declspec(dllimport) int __stdcall TerminateProcess(void* process, unsigned int code);
#else
#include <unistd.h>
#endif
void mkgp2pc_video_quit(void);
static void quit_now(void) {
  fflush(NULL);
#ifdef _WIN32
  TerminateProcess(GetCurrentProcess(), 0);
#endif
  _exit(0);
}

void mkgp2pc_video_quit(void) { quit_now(); }

static Uint64 s_limitWanted; /* how long the last frame_limit meant to sleep */
static void frame_limit(void) {
  static Uint64 next;
  static int off = -1;
  const Uint64 period = 1001000000ull / 60;
  if (off < 0) {
    off = getenv("MKGP2PC_NOLIMIT") != NULL;
  }
  s_limitWanted = 0;
  if (off || s_fast || s_fastHeld) {
    next = 0; /* the pace starts afresh when the limit comes back */
    return;
  }
  Uint64 now = SDL_GetTicksNS();
  if (next == 0 || now > next + 4 * period) {
    next = now; /* first frame, or too far behind to catch up */
  }
  if (now < next) {
    s_limitWanted = next - now;
    SDL_DelayNS(next - now);
  }
  next += period;
}

/* Full speed (F10/Tab): the game's frames come as fast as the machine makes
 * them, the window gets one of them about 60 times a second. A frame that is
 * not shown stays inside the rendering library's current frame and its
 * drawing is dropped (mkgp2pc_skip_draw, gx_shim.cpp). */
extern bool mkgp2pc_skip_draw;
static bool show_next_frame(void) {
  static Uint64 shown;
  if (!(s_fast || s_fastHeld) || s_paused || s_dumpWanted) {
    shown = 0;
    return true;
  }
  Uint64 now = SDL_GetTicksNS();
  if (shown != 0 && now - shown < 1001000000ull / 60) {
    return false;
  }
  shown = now;
  return true;
}

RetraceCallback VISetPreRetraceCallback(RetraceCallback cb) {
  RetraceCallback old = s_pre;
  s_pre = cb;
  return old;
}
RetraceCallback VISetPostRetraceCallback(RetraceCallback cb) {
  RetraceCallback old = s_post;
  s_post = cb;
  return old;
}
uint32_t VIGetRetraceCount(void) { return s_retrace; }

static void pump_events(void) {
  const AuroraEvent* event = aurora_update();
  while (event != NULL && event->type != AURORA_NONE) {
    if (event->type == AURORA_EXIT) {
      quit_now();
    }
    if (event->type == AURORA_SDL_EVENT &&
        (event->sdl.type == SDL_EVENT_KEY_DOWN || event->sdl.type == SDL_EVENT_KEY_UP)) {
      key_event(&event->sdl);
    }
    ++event;
  }
}

/*
 * Frame-time log. A frame that takes longer than MKGP2PC_SPIKE_MS
 * milliseconds from one return of VIWaitForRetrace to the next (default 25,
 * one and a half frames; 0 turns the log off) gets one line in the log:
 *
 *   spike: retrace 5120 frame 212.4 ms: game 196.0 end 0.3 events 0.1
 *          sleep 0.0 (late 0.0) retrace 0.2 begin 15.8 | pipelines +3
 *          (12 waiting) disc 0.0 ms 0 KB lock 0.4 ms
 *
 *   game      the game's own frame: logic and its drawing calls. A draw whose
 *             pipeline is not compiled yet waits in here (begin_frame_state);
 *             "pipelines +N" says how many were compiled during the frame.
 *   end       handing the frame to the rendering thread
 *   events    window events, dump bookkeeping
 *   sleep     the frame limiter; "late" is how much longer than asked
 *   retrace   the retrace callbacks (inputs, media board, link, the game's)
 *   begin     waiting for the rendering thread to give a frame back: long
 *             when the GPU side is behind
 *   disc      reads from the disc image during the frame, on any thread
 *   lock      time this thread waited for the "interrupts off" lock while
 *             the sound or another thread held it
 * Scene loads are long frames too and are listed like any other. Frames in
 * which the game was paused or a dump was made are left out.
 */
uint64_t mkgp2pc_perf_lock_wait_ns(void);                   /* port/nogfx/os_thread.c */
extern uint64_t mkgp2pc_perf_dvd_ns, mkgp2pc_perf_dvd_bytes; /* port/nogfx/dvd_min.c */
static void spike_log(const Uint64 t[7], bool skip) {
  static double limitMs = -1.0;
  static uint32_t pipelines, lines;
  static uint64_t dvdNs, dvdBytes, lockNs;
  const AuroraStats* stats = aurora_get_stats();
  uint64_t lockNow = mkgp2pc_perf_lock_wait_ns();
  if (limitMs < 0.0) {
    const char* text = getenv("MKGP2PC_SPIKE_MS");
    limitMs = text != NULL ? strtod(text, NULL) : 25.0;
  }
#define MS(a, b) ((double)((b) - (a)) / 1e6)
  if (!skip && limitMs > 0.0 && t[0] != 0 && MS(t[0], t[6]) > limitMs && lines < 20000) {
    double slept = MS(t[3], t[4]);
    double late = slept - (double)s_limitWanted / 1e6;
    lines++;
    fprintf(stderr,
            "spike: retrace %u frame %.1f ms: game %.1f end %.1f events %.1f sleep %.1f (late %.1f) retrace %.1f "
            "begin %.1f | pipelines +%u (%u waiting) disc %.1f ms %u KB lock %.1f ms%s\n",
            (unsigned)s_retrace, MS(t[0], t[6]), MS(t[0], t[1]), MS(t[1], t[2]), MS(t[2], t[3]), slept,
            late > 0.0 ? late : 0.0, MS(t[4], t[5]), MS(t[5], t[6]), (unsigned)(stats->createdPipelines - pipelines),
            (unsigned)stats->queuedPipelines, (double)(mkgp2pc_perf_dvd_ns - dvdNs) / 1e6,
            (unsigned)((mkgp2pc_perf_dvd_bytes - dvdBytes) >> 10), (double)(lockNow - lockNs) / 1e6,
            s_fast || s_fastHeld ? " [full speed]" : "");
  }
#undef MS
  pipelines = stats->createdPipelines;
  dvdNs = mkgp2pc_perf_dvd_ns;
  dvdBytes = mkgp2pc_perf_dvd_bytes;
  lockNs = lockNow;
}

void VIWaitForRetrace(void) {
  static Uint64 lastReturn;
  Uint64 t[7];
  bool skip = s_paused || s_dumpWanted || s_shotWait != 0 || mkgp2pc_menu_is_open();
  {
    static unsigned ticks;
    unsigned now = mkgp2pc_nav_flow_ticks();
    s_mainLoopRuns = now != ticks;
    ticks = now;
  }
  t[0] = lastReturn;
  t[1] = SDL_GetTicksNS();
  if (s_inFrame && !mkgp2pc_skip_draw) {
    /* One picture at a time: a dump asked for while the last one's picture
     * is still on its way is made on the first frame after it. */
    if (s_dumpWanted && s_shotWait == 0) {
      s_dumpWanted = false;
      dump_capture();
    }
    mkgp2pc_shadow_record_frame();
    mkgp2pc_debug_overlay_draw();
    aurora_end_frame();
    s_inFrame = false;
  }
  t[2] = SDL_GetTicksNS();
  pump_events();
  dump_poll();
  s_step = false;
  skip |= s_paused;
  while ((s_paused && !s_step) || mkgp2pc_menu_is_open()) {
    if (mkgp2pc_menu_is_open()) {
      /* The game's last picture stays (it is the copy the game made of its
       * frame buffer); each pass presents it again with the menu on top. */
      if (!s_inFrame) {
        s_inFrame = aurora_begin_frame();
      }
      if (s_inFrame) {
        mkgp2pc_menu_draw();
        aurora_end_frame();
        s_inFrame = false;
      }
    }
    SDL_Delay(10);
    pump_events();
    dump_poll();
  }
  mkgp2pc_audio_frame();
  t[3] = SDL_GetTicksNS();
  frame_limit();
  t[4] = SDL_GetTicksNS();
  int level = OSDisableInterrupts();
  s_retrace++;
  mkgp2pc_script_tick(s_retrace);
  mkgp2pc_mediaboard_tick();
  keyboard_tick();
  /* the strings of the frame just shown are done with; the game's debug
   * byte follows the switch before the next frame reads it */
  mkgp2pc_debug_overlay_clear();
  mkgp2pc_debug_frame();
  if (s_pre) {
    s_pre(s_retrace);
  }
  if (s_post) {
    s_post(s_retrace);
  }
  OSRestoreInterrupts(level);
  if (s_frameLimit >= 0 && (long)s_retrace >= s_frameLimit) {
    quit_now();
  }
  t[5] = SDL_GetTicksNS();
  if (!s_inFrame) {
    s_inFrame = aurora_begin_frame();
    begin_frame_state();
    wide_frame();
  }
  mkgp2pc_skip_draw = s_inFrame && !show_next_frame();
  t[6] = lastReturn = SDL_GetTicksNS();
  spike_log(t, skip);
}
