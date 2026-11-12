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
#include <dolphin/vi.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int OSDisableInterrupts(void);
int OSRestoreInterrupts(int);
void mkgp2pc_mediaboard_tick(void); /* port/platform/triforce/mediaboard.c */
/* port/platform/triforce/baseboard.c: the backup memory and its file */
void mkgp2pc_baseboard_flush(void);
int mkgp2pc_baseboard_save_start(const char* path);
const char* mkgp2pc_baseboard_file(void);
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
 *   left shift     cancel button ("Mario button")
 *   5 or C         coin
 *   I              put the card (card.bin) into the card unit
 *   F1 / F2        test / service button of the cabinet
 *   P              pause / resume
 *   O              while paused: advance one frame
 *   F9             dump (see "Dumps" below); works paused or running
 *   F8             high-resolution 2D textures on / off (docs/port/hires_textures.md)
 * These are the defaults; MKGP2PC_KEYS changes them (see key_event).
 * Once a key has been used the keyboard owns the inputs; until then a
 * script (MKGP2PC_SCRIPT) may drive them.
 */
static bool s_keyLeft, s_keyRight, s_keyAccel, s_keyBrake, s_keyItem, s_keyTest, s_keyService;
/* buttons the cabinet does not have: the developers' scenes read them (docs/port/debug.md) */
static bool s_keyDevUp, s_keyDevDown, s_keyDevStart, s_keyDevSub;
static bool s_keyCancel;
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
 *   frame_small.jpg  the same picture, at most 640 wide
 *   stacks.txt, memory.txt, mem1.bin.gz, image.bin.gz, symbols.txt.gz
 *               where every thread was and the game's memory (diag.c)
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
/* zlib without its header (see port/gfx/diag.c) */
unsigned long compressBound(unsigned long sourceLen);
int compress2(unsigned char* dest, unsigned long* destLen, const unsigned char* source, unsigned long sourceLen, int level);
static bool png_write(const char* path, const uint8_t* rgba, uint32_t w, uint32_t h) {
  FILE* f = fopen(path, "wb");
  if (f == NULL) {
    return false;
  }
  const uint32_t row = 1 + w * 3;
  const uint32_t raw = row * h;
  unsigned long zlen = compressBound(raw);
  uint8_t* z = (uint8_t*)malloc(zlen);
  uint8_t* scan = (uint8_t*)malloc(raw);
  if (z == NULL || scan == NULL) {
    fclose(f);
    free(z);
    free(scan);
    return false;
  }
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
  if (compress2(z, &zlen, scan, raw, 6) != 0) {
    fclose(f);
    free(z);
    free(scan);
    return false;
  }
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
  uint8_t ihdr[13] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                      (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h,
                      8,                  2,                  0,                 0,
                      0};
  fwrite(sig, 1, 8, f);
  png_chunk(f, "IHDR", ihdr, 13);
  png_chunk(f, "IDAT", z, (uint32_t)zlen);
  png_chunk(f, "IEND", ihdr, 0);
  fclose(f);
  free(z);
  free(scan);
  return true;
}

bool mkgp2pc_window_shot(SDL_Window* window, uint8_t** rgba, uint32_t* width, uint32_t* height); /* shot_win32.c */
bool mkgp2pc_jpeg_write(const char* path, const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t maxWidth, int quality); /* jpeg_min.c */
/* diag.c: stacks and memory for a dump, and the same when the program dies */
void mkgp2pc_diag_install(void);
void mkgp2pc_diag_dump(const char* dir);
void mkgp2pc_diag_fatal(const char* why);
extern void (*mkgp2pc_diag_extra)(const char* dir);
static SDL_Window* s_window;
static int s_shotWait; /* polls left before the window's own picture is taken instead */
static AuroraReadbackStream s_shotStream;
static uint8_t* s_shotPixels;
static uint32_t s_shotW, s_shotH;
static char s_shotPath[600];
static int s_dumpCount;
static bool s_fast, s_fastHeld; /* F10 and Tab: no frame limit */
extern int mkgp2pc_boot_fast;   /* the same during start-up (port/platform/runtime/boot_skip.c) */

void mkgp2pc_shadow_record_frame(void);  /* gx_shim.cpp */
void mkgp2pc_gxlog_frame_end(void);      /* gx_shim.cpp: the frame's draw commands, for dumps */
void mkgp2pc_gxlog_frame_next(void);
void mkgp2pc_gxlog_write(const char* dir, int withLast);
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
  /* the backup memory as this run found it (a file MKGP2PC_BACKUP_FILE can
   * name): with input.txt, what a replay needs to start from the same
   * records and settings */
  snprintf(path, sizeof(path), "%s/backup_start.bin", dir);
  mkgp2pc_baseboard_save_start(path);
  snprintf(path, sizeof(path), "%s/state.txt", dir);
  f = fopen(path, "w");
  if (f != NULL) {
    static const char* const names[] = {"MKGP2PC_SCRIPT", "MKGP2PC_SCRIPT_FILE", "MKGP2PC_NOLIMIT", "MKGP2PC_FRAMES",
                                       "MKGP2PC_CARD", "MKGP2PC_CARD_FILE", "MKGP2PC_BACKUP"};
    fprintf(f, "retrace %u\npaused %d\nframe.png shows retrace %u\n", (unsigned)s_retrace, (int)s_paused,
            (unsigned)s_retrace + 1);
    for (int i = 0; i < 7; i++) {
      const char* v = getenv(names[i]);
      fprintf(f, "%s=%s\n", names[i], v ? v : "");
    }
    fprintf(f, "backup memory file=%s\n", mkgp2pc_baseboard_file());
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

/* diag.c calls this for a crash folder */
static void crash_extra(const char* dir) {
  dump_text(dir);
  mkgp2pc_gxlog_write(dir, 1);
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
  mkgp2pc_gxlog_write(dir, 0);
  mkgp2pc_diag_dump(dir);
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
  fprintf(stderr, "dump: %s (inputs, state, stacks and memory written)\n", dir);
}

/* The picture arrives when the GPU has finished with that frame. Where
 * Aurora cannot read back (it says so in the log at start-up) the window's
 * own picture is taken after a short wait: the frame on screen then, which
 * is the dumped one when paused and a slightly later one when running. */
static bool shot_write(const uint8_t* rgba, uint32_t w, uint32_t h) {
  char small[640];
  size_t n = strlen(s_shotPath);
  /* beside frame.png: at most 640 wide, some tens of KB */
  snprintf(small, sizeof(small), "%.*s_small.jpg", (int)(n > 4 ? n - 4 : n), s_shotPath);
  mkgp2pc_jpeg_write(small, rgba, w, h, 640, 80);
  return png_write(s_shotPath, rgba, w, h);
}
static void dump_poll(void) {
  if (s_shotWait == 0) {
    return;
  }
  bool ok = false;
  if (s_shotPixels != NULL) {
    AuroraReadbackDest dest = {(uint64_t)s_dumpCount, s_shotPixels, s_shotW * s_shotH * 4, GX_FALSE};
    if (AuroraReadbackGet(s_shotStream, &dest, 1) && dest.found) {
      ok = shot_write(s_shotPixels, s_shotW, s_shotH);
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
      ok = shot_write(pixels, w, h);
      free(pixels);
    }
  }
  fprintf(stderr, "dump: %s %s\n", s_shotPath, ok ? "written" : "not written (no way to read the picture here)");
  free(s_shotPixels);
  s_shotPixels = NULL;
}

extern int (*mkgp2pc_script_extra)(const char* cmd);
static int script_extra(const char* cmd) {
  if (strcmp(cmd, "pause") == 0) {
    s_paused = !s_paused;
    fprintf(stderr, "script: %s at retrace %u\n", s_paused ? "paused" : "resumed", (unsigned)s_retrace);
  } else if (strcmp(cmd, "step") == 0) {
    s_step = true;
  } else if (strcmp(cmd, "dump") == 0) {
    s_dumpWanted = true;
  } else if (strcmp(cmd, "crash") == 0) {
    /* to try the crash folder (docs/port/dumps.md): a write to address 8 */
    *(volatile int*)8 = 1;
  } else if (strcmp(cmd, "abort") == 0) {
    abort();
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
  KEY_HIRES, KEY_DEBUG, KEY_DEBUGPAGE,
  KEY_DEVUP, KEY_DEVDOWN, KEY_DEVSTART, KEY_DEVSUB, KEY_CANCEL,
  KEY_ACTIONS
};
#define KEYS_PER_ACTION 4
static const char* const s_actionNames[KEY_ACTIONS] = {
    "left", "right", "accel", "brake", "item", "test", "service",
    "coin", "card", "pause", "step", "fast", "fasthold", "dump", "hires",
    "debug", "debugpage",
    "devup", "devdown", "devstart", "devsub", "cancel"};
static int s_keyMap[KEY_ACTIONS][KEYS_PER_ACTION] = {
    {SDL_SCANCODE_LEFT}, {SDL_SCANCODE_RIGHT}, {SDL_SCANCODE_UP, SDL_SCANCODE_Z},
    {SDL_SCANCODE_DOWN, SDL_SCANCODE_X}, {SDL_SCANCODE_SPACE, SDL_SCANCODE_RETURN},
    {SDL_SCANCODE_F1}, {SDL_SCANCODE_F2}, {SDL_SCANCODE_5, SDL_SCANCODE_C},
    {SDL_SCANCODE_I}, {SDL_SCANCODE_P}, {SDL_SCANCODE_O}, {SDL_SCANCODE_F10},
    {SDL_SCANCODE_TAB}, {SDL_SCANCODE_F9}, {SDL_SCANCODE_F8},
    {SDL_SCANCODE_F3}, {SDL_SCANCODE_F4},
    {SDL_SCANCODE_PAGEUP}, {SDL_SCANCODE_PAGEDOWN}, {SDL_SCANCODE_END}, {SDL_SCANCODE_HOME},
    {SDL_SCANCODE_LSHIFT}};

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
    case KEY_DEVUP: s_keyDevUp = down; control = true; break;
    case KEY_DEVDOWN: s_keyDevDown = down; control = true; break;
    case KEY_DEVSTART: s_keyDevStart = down; control = true; break;
    case KEY_DEVSUB: s_keyDevSub = down; control = true; break;
    case KEY_CANCEL: s_keyCancel = down; control = true; break;
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
  /* Second switch byte of player 1: the item button (0x20) and the cancel
   * button (0x02, the "Mario button" by the user's word 2026-11-12; game
   * bits 0x8400 in kRawGameMap_Raw0, "leave / previous" in the developers'
   * scenes). The table also knows 0x80, 0x40 and 0x10, which the
   * developers' scenes use as up, down and a second button; those and the
   * End key's 0x02 are passed on only while debugging is on, so that a
   * stray key cannot do anything in a normal game. */
  uint8_t dev = 0;
  if (mkgp2pc_debug_on()) {
    dev = (uint8_t)((s_keyDevUp ? 0x80 : 0) | (s_keyDevDown ? 0x40 : 0) | (s_keyDevSub ? 0x10 : 0) |
                    (s_keyDevStart ? 0x02 : 0));
  }
  rec_switch((uint8_t)((s_keyItem ? 0x20 : 0) | (s_keyCancel ? 0x02 : 0) | dev));
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
    /* where the game was when the library gave up: the log and a crash folder */
    char why[400];
    snprintf(why, sizeof(why), "fatal message of %s: %s", module, message);
    mkgp2pc_diag_fatal(why);
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
 * another over the first frames. Blocking draws wait for the pipeline
 * instead, which makes the first frame of a scene complete, and makes a
 * pipeline first met in the middle of a race a stall of 60 ms and more
 * (docs/port/census.md section 36). So draws wait only around loading: at
 * the start and for WAIT_FRAMES frames after the game last read a
 * substantial amount from the disc (a scene being loaded reads hundreds of
 * kilobytes and more a frame, a running race next to nothing). At any other
 * time a draw whose pipeline is not ready is left out for the few frames the
 * compilation takes, once per pipeline and machine.
 *   MKGP2PC_PIPELINE_WAIT=always   wait for every pipeline (before 2026-11-12)
 *   MKGP2PC_PIPELINE_WAIT=never    never wait (Aurora's own behaviour);
 *                                  MKGP2PC_ASYNC_PIPELINES=1 says the same
 * The setting is GX state, sent with each frame. */
extern uint64_t mkgp2pc_perf_dvd_bytes; /* port/nogfx/dvd_min.c */
static bool s_drawsWait;                /* this frame's setting, for the frame-time log */
static void begin_frame_state(void) {
  enum { WAIT_FRAMES = 120, LOAD_BYTES = 128 << 10 };
  static int mode = -1; /* 0 never, 1 always, 2 around loading */
  static uint64_t seenBytes;
  static int left = WAIT_FRAMES;
  if (mode < 0) {
    const char* text = getenv("MKGP2PC_PIPELINE_WAIT");
    mode = 2;
    if (getenv("MKGP2PC_ASYNC_PIPELINES") != NULL || (text != NULL && strcmp(text, "never") == 0)) {
      mode = 0;
    } else if (text != NULL && strcmp(text, "always") == 0) {
      mode = 1;
    }
  }
  if (mkgp2pc_perf_dvd_bytes - seenBytes >= LOAD_BYTES) {
    left = WAIT_FRAMES;
  } else if (left > 0) {
    left--;
  }
  seenBytes = mkgp2pc_perf_dvd_bytes;
  s_drawsWait = mode == 1 || (mode == 2 && left > 0);
  if (s_inFrame) {
    AuroraSetDrawsBlocking(s_drawsWait);
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
/* keepCopy (pipeline_cache.db, the list of pipelines seen so far): a file
 * that passes the check is copied to <name>.backup, and a damaged one is
 * replaced by that copy instead of an empty file. With the list back, the
 * pipelines are compiled in the background at the start, as always, and not
 * one by one when the game first draws with them. */
static void cache_check_file(const char* dir, const char* name, bool reported, bool keepCopy) {
  char path[1024], other[1040], copy[1040];
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
  snprintf(copy, sizeof(copy), "%s.backup", path);
  if (state == 1 && keepCopy && strchr(path, '\'') == NULL) {
    /* VACUUM INTO writes a compact file of its own from the rows; it does
     * not replace an existing one, so: new file first, then swap. */
    char sql[2200];
    snprintf(other, sizeof(other), "%s.backup.new", path);
    SDL_RemovePath(other);
    snprintf(sql, sizeof(sql), "VACUUM INTO '%s';", other);
    if (sqlite3_exec(db, sql, NULL, NULL, NULL) == 0) {
      SDL_RemovePath(copy);
      SDL_RenamePath(other, copy);
    } else {
      fprintf(stderr, "cache: no backup of %s (%s)\n", name, sqlite3_errmsg(db));
    }
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
  if (moved && keepCopy && SDL_GetPathInfo(copy, NULL) && SDL_CopyFile(copy, path)) {
    fprintf(stderr, "cache: %s is damaged (%s); set aside as .broken and replaced by the backup of the last good start\n",
            path, why[0] ? why : "failed the check");
    return;
  }
  fprintf(stderr, "cache: %s is damaged (%s); %s\n", path, why[0] ? why : "failed the check",
          moved ? "set aside as .broken, a new one is started (shaders are compiled again once)"
                : "and could not be moved away: this run has no shader cache");
}
/*
 * One game at a time per cache folder. Two copies running together (two
 * cabinets on one PC for the link) used to share the two files; SQLite is
 * meant to cope with that, but the copies are also ended with
 * TerminateProcess (quit_now, a reset) in the middle of whatever they are
 * writing, and the files did get damaged on the one PC where both happened
 * (2026-11-12, cause not proven). The first copy takes a lock file in the
 * user directory and uses the directory itself; a copy that finds the lock
 * taken uses the sub-folder copy2 (then copy3, copy4) with caches of its
 * own. The operating system drops the lock when the process ends, however
 * it ends.
 */
#ifdef _WIN32
__declspec(dllimport) void* __stdcall CreateFileA(const char* name, unsigned long access, unsigned long share,
                                                  void* security, unsigned long creation, unsigned long flags,
                                                  void* templateFile);
static bool cache_lock(const char* path) {
  /* GENERIC_WRITE, no sharing, OPEN_ALWAYS; the handle is kept for good */
  return CreateFileA(path, 0x40000000ul, 0, NULL, 4, 0x80, NULL) != (void*)(intptr_t)-1;
}
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
static bool cache_lock(const char* path) {
  int fd = open(path, O_RDWR | O_CREAT, 0644);
  if (fd < 0) {
    return true; /* cannot tell: behave as the only copy */
  }
  if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
    close(fd);
    return false;
  }
  return true;
}
#endif
/* The folder for this copy's caches; NULL when it is Aurora's default (the
 * user directory). The string stays allocated: Aurora keeps the pointer. */
static const char* cache_check(const char* appName) {
  char* base = SDL_GetPrefPath(NULL, appName); /* what Aurora uses when no cache path is given */
  char* dir = base;
  char lock[1100];
  if (base == NULL) {
    return NULL;
  }
  for (int copy = 1; copy <= 4; copy++) {
    if (copy > 1) {
      size_t size = strlen(base) + 16;
      dir = (char*)SDL_malloc(size);
#ifdef _WIN32
      snprintf(dir, size, "%scopy%d\\", base, copy);
#else
      snprintf(dir, size, "%scopy%d/", base, copy);
#endif
      SDL_CreateDirectory(dir);
    }
    snprintf(lock, sizeof(lock), "%srunning.lock", dir);
    if (cache_lock(lock)) {
      break;
    }
    if (copy > 1) {
      SDL_free(dir);
    }
    dir = NULL;
  }
  if (dir == NULL) {
    fprintf(stderr, "cache: four copies are running already; this one shares the first one's caches\n");
    dir = base;
  } else if (dir != base) {
    fprintf(stderr, "cache: another copy is running; this one keeps its caches in %s\n", dir);
  }
  if (getenv("MKGP2PC_NO_CACHE_CHECK") == NULL) {
    Uint64 start = SDL_GetTicksNS();
    snprintf(s_cacheMarker, sizeof(s_cacheMarker), "%scache_damaged", dir);
    bool reported = SDL_GetPathInfo(s_cacheMarker, NULL);
    cache_check_file(dir, "dawn_cache.db", reported, false);
    cache_check_file(dir, "pipeline_cache.db", reported, true);
    SDL_RemovePath(s_cacheMarker);
    fprintf(stderr, "cache: %s checked in %.0f ms\n", dir, (double)(SDL_GetTicksNS() - start) / 1e6);
  }
  return dir != base ? dir : NULL;
}

/* Wide picture (MKGP2PC_ASPECT, docs/port/widescreen.md). The rendering
 * library is told to let the frame buffer take the window's shape and to
 * spread the game's 640x480 over it; gx_shim.cpp narrows the game's
 * projections again by mkgp2pc_wide_k, which follows the frame buffer's
 * real shape every frame. */
static bool s_wide;
void mkgp2pc_wide_frame(float k); /* gx_shim.cpp */
extern unsigned mkgp2pc_wide_retrace; /* for its trace lines */
static void wide_frame(void) {
  if (!s_wide) {
    return;
  }
  mkgp2pc_wide_retrace = (unsigned)s_retrace;
  AuroraWindowSize size = aurora_get_window_size();
  if (size.fb_width == 0 || size.fb_height == 0) {
    return;
  }
  float k = ((float)size.fb_width / (float)size.fb_height) / (4.0f / 3.0f);
  mkgp2pc_wide_frame(k > 1.001f ? k : 1.0f);
}
/* Anti-aliasing (docs/port/antialiasing.md). Two means, both the rendering
 * library's own, both off unless asked for:
 *   MKGP2PC_MSAA=4  four samples per pixel at the edges of polygons;
 *   MKGP2PC_SSAA=F  the whole picture drawn F times as wide and as high as
 *                   the window (1 < F <= 4, fractions allowed) and brought
 *                   down to it when shown.
 * The frame buffer's size is given to the library as a multiple of the
 * game's own frame buffer height, so the factor is worked out from the
 * window's real height, again whenever that changes. */
static double s_ssaa = 1;
static void ssaa_frame(void) {
  static uint32_t wantW, wantH;
  static float scaleSet;
  static int tries;
  if (s_ssaa <= 1) {
    return;
  }
  AuroraWindowSize size = aurora_get_window_size();
  if (size.native_fb_height == 0 || size.fb_height == 0) {
    return;
  }
  uint32_t want = (uint32_t)(size.native_fb_height * s_ssaa + 0.5);
  if (size.native_fb_height != wantH || size.native_fb_width != wantW) {
    wantH = size.native_fb_height;
    wantW = size.native_fb_width;
    tries = 0;
  }
  uint32_t off = size.fb_height > want ? size.fb_height - want : want - size.fb_height;
  if (off <= 1 || tries >= 4) {
    return;
  }
  /* the game's frame buffer height, seen through the scale in force */
  double base = scaleSet > 0 ? size.fb_height / (double)scaleSet : 480.0;
  scaleSet = (float)(want / base);
  tries++;
  VISetFrameBufferScale(scaleSet);
  fprintf(stderr, "anti-aliasing: frame buffer %ux%u for a window of %ux%u (factor %g)\n",
          (unsigned)(size.native_fb_width * s_ssaa + 0.5), want, size.native_fb_width, size.native_fb_height, s_ssaa);
}
static void quit_now(void);
int __wrap_main(int argc, char** argv) {
  mkgp2pc_diag_install();
  mkgp2pc_diag_extra = &crash_extra;
  AuroraConfig config;
  memset(&config, 0, sizeof(config));
  config.appName = "mkgp2-pc";
  config.logCallback = &log_callback;
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
  const char* msaaText = getenv("MKGP2PC_MSAA");
  if (msaaText != NULL && msaaText[0] != 0 && strcmp(msaaText, "0") != 0 && strcmp(msaaText, "1") != 0) {
    /* WebGPU knows one and four samples only */
    if (strcmp(msaaText, "4") != 0) {
      fprintf(stderr, "MKGP2PC_MSAA: '%s' taken as 4 (the only count there is)\n", msaaText);
    }
    config.msaa = 4;
  }
  const char* ssaaText = getenv("MKGP2PC_SSAA");
  if (ssaaText != NULL && ssaaText[0] != 0) {
    double f = strtod(ssaaText, NULL);
    if (f >= 1 && f <= 4) {
      s_ssaa = f;
    } else {
      fprintf(stderr, "MKGP2PC_SSAA: '%s' is not a factor from 1 to 4; not used\n", ssaaText);
    }
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
  config.cachePath = cache_check(config.appName);
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
  if (config.msaa > 1 || s_ssaa > 1) {
    fprintf(stderr, "anti-aliasing: MSAA %s, supersampling %g\n", config.msaa > 1 ? "4x" : "off", s_ssaa);
  }
  if (s_ssaa > 1) {
    /* each window pixel is the mean of the frame buffer pixels under it */
    aurora_set_resampler(SAMPLER_AREA);
  }
  ssaa_frame();
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
static void quit_now(void) {
  mkgp2pc_baseboard_flush(); /* the backup memory file is up to date */
  fflush(NULL);
#ifdef _WIN32
  TerminateProcess(GetCurrentProcess(), 0);
#endif
  _exit(0);
}

static Uint64 s_limitWanted; /* how long the last frame_limit meant to sleep */
static void frame_limit(void) {
  static Uint64 next;
  static int off = -1;
  const Uint64 period = 1001000000ull / 60;
  if (off < 0) {
    off = getenv("MKGP2PC_NOLIMIT") != NULL;
  }
  s_limitWanted = 0;
  if (off || s_fast || s_fastHeld || mkgp2pc_boot_fast) {
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
  if (!(s_fast || s_fastHeld || mkgp2pc_boot_fast) || s_paused || s_dumpWanted) {
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
 *   [loading] draws waited for their pipelines in this frame (the game had
 *             just read from the disc, begin_frame_state)
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
            "begin %.1f | pipelines +%u (%u waiting) disc %.1f ms %u KB lock %.1f ms%s%s\n",
            (unsigned)s_retrace, MS(t[0], t[6]), MS(t[0], t[1]), MS(t[1], t[2]), MS(t[2], t[3]), slept,
            late > 0.0 ? late : 0.0, MS(t[4], t[5]), MS(t[5], t[6]), (unsigned)(stats->createdPipelines - pipelines),
            (unsigned)stats->queuedPipelines, (double)(mkgp2pc_perf_dvd_ns - dvdNs) / 1e6,
            (unsigned)((mkgp2pc_perf_dvd_bytes - dvdBytes) >> 10), (double)(lockNow - lockNs) / 1e6,
            s_drawsWait ? " [loading]" : "", s_fast || s_fastHeld ? " [full speed]" : "");
  }
#undef MS
  pipelines = stats->createdPipelines;
  dvdNs = mkgp2pc_perf_dvd_ns;
  dvdBytes = mkgp2pc_perf_dvd_bytes;
  lockNs = lockNow;
}

/*
 * Shaders before the game. Aurora queues every pipeline of its list
 * (pipeline_cache.db, merged at each start with initial_pipeline_cache.db
 * from the executable's folder) for its pipeline thread when it starts. The
 * game is held at its first retraces until that queue is empty, so that
 * none of the listed pipelines is compiled while the game runs. With the
 * compiled blobs in dawn_cache.db this is a second or less; without them (first
 * start on a machine, new driver) every pipeline is compiled, 60 to 90 ms
 * each on D3D12. The title bar counts down meanwhile.
 * MKGP2PC_NO_SHADER_WAIT=1 lets the game start at once as before.
 */
static void shader_wait(void) {
  static int off = -1;
  const AuroraStats* stats = aurora_get_stats();
  if (off < 0) {
    off = getenv("MKGP2PC_NO_SHADER_WAIT") != NULL;
  }
  uint32_t first = stats->queuedPipelines;
  if (off || first == 0) {
    static bool said;
    if (!off && !said) {
      /* Say so, or a log without a "shaders:" line cannot be told from a
         start where the wait never ran. */
      fprintf(stderr, "shaders: nothing to prepare at retrace %u (%u pipelines ready)\n",
              s_retrace, stats->createdPipelines);
    }
    said = true;
    return;
  }
  Uint64 start = SDL_GetTicksNS(), moved = start, titled = 0;
  uint32_t last = first;
  char* title = s_window != NULL ? SDL_strdup(SDL_GetWindowTitle(s_window)) : NULL;
  for (;;) {
    uint32_t left = stats->queuedPipelines;
    Uint64 now = SDL_GetTicksNS();
    if (left == 0) {
      break;
    }
    if (left != last) {
      last = left;
      moved = now;
    } else if (now - moved > 20000000000ull) {
      fprintf(stderr, "shaders: no progress for 20 s with %u left; the game starts without waiting\n", (unsigned)left);
      break;
    }
    if (s_window != NULL && now - titled > 200000000ull) {
      char text[128];
      titled = now;
      snprintf(text, sizeof(text), "%s - preparing shaders, %u left", title != NULL ? title : "mkgp2-pc",
               (unsigned)left);
      SDL_SetWindowTitle(s_window, text);
    }
    pump_events();
    SDL_Delay(5);
  }
  if (s_window != NULL && title != NULL) {
    SDL_SetWindowTitle(s_window, title);
  }
  SDL_free(title);
  fprintf(stderr, "shaders: %u pipelines prepared before the game in %.1f s\n", (unsigned)(first - stats->queuedPipelines),
          (double)(SDL_GetTicksNS() - start) / 1e9);
}

/* The stall watch. The spike log says how long the game's own part of a
   frame took but not where; a stall there that is neither pipelines nor the
   disc leaves no trace. A second thread looks every 20 ms at when the game
   last left VIWaitForRetrace and, once that is longer ago than
   MKGP2PC_STALL_MS (150; 0 turns the watch off), writes every thread's stack
   to dumps/stall_<retrace>/stacks.txt, at most 20 times a run. Only the time
   between two calls of VIWaitForRetrace is watched, so pauses, dumps and
   the shader hold at the start do not count. */
void mkgp2pc_diag_prepare(void);
void mkgp2pc_diag_stall(const char* dir, const char* why);
static SDL_AtomicInt s_stallBeat; /* ms tick the game got control at, 0: not watched */
static SDL_AtomicInt s_stallRetrace;
static int s_stallMs = -1;

static int SDLCALL stall_thread(void* arg) {
  int written = 0, reported = 0;
  (void)arg;
  mkgp2pc_diag_prepare();
  while (written < 20) {
    SDL_Delay(20);
    int beat = SDL_GetAtomicInt(&s_stallBeat);
    int now = (int)(SDL_GetTicks() & 0x7fffffff);
    if (beat == 0 || beat == reported || now - beat < s_stallMs) {
      continue;
    }
    unsigned retrace = (unsigned)SDL_GetAtomicInt(&s_stallRetrace);
    const char* base = getenv("MKGP2PC_DUMP_DIR");
    char dir[512], why[128];
    if (base == NULL || *base == 0) {
      base = "dumps";
    }
    SDL_CreateDirectory(base);
    snprintf(dir, sizeof(dir), "%s/stall_f%u", base, retrace);
    SDL_CreateDirectory(dir);
    snprintf(why, sizeof(why), "the game's thread has not reached the next retrace for %d ms (after retrace %u)",
             now - beat, retrace);
    mkgp2pc_diag_stall(dir, why);
    fprintf(stderr, "stall: after retrace %u the game is stuck for %d ms, stacks in %s\n", retrace, now - beat, dir);
    reported = beat;
    written++;
  }
  return 0;
}

static void stall_beat(bool gameRuns) {
  if (s_stallMs < 0) {
    const char* v = getenv("MKGP2PC_STALL_MS");
    s_stallMs = v != NULL ? atoi(v) : 150;
    if (s_stallMs > 0) {
      SDL_Thread* th = SDL_CreateThread(stall_thread, "stall watch", NULL);
      if (th != NULL) {
        SDL_DetachThread(th);
      } else {
        s_stallMs = 0;
      }
    }
  }
  if (s_stallMs > 0) {
    int now = (int)(SDL_GetTicks() & 0x7fffffff);
    SDL_SetAtomicInt(&s_stallRetrace, (int)s_retrace);
    SDL_SetAtomicInt(&s_stallBeat, gameRuns ? (now != 0 ? now : 1) : 0);
  }
}

void VIWaitForRetrace(void) {
  static Uint64 lastReturn;
  Uint64 t[7];
  bool skip = s_paused || s_dumpWanted || s_shotWait != 0;
  t[0] = lastReturn;
  t[1] = SDL_GetTicksNS();
  stall_beat(false);
  if (s_inFrame && !mkgp2pc_skip_draw) {
    /* One picture at a time: a dump asked for while the last one's picture
     * is still on its way is made on the first frame after it. */
    if (s_dumpWanted && s_shotWait == 0) {
      s_dumpWanted = false;
      dump_capture();
    }
    mkgp2pc_shadow_record_frame();
    mkgp2pc_gxlog_frame_end();
    mkgp2pc_debug_overlay_draw();
    aurora_end_frame();
    mkgp2pc_gxlog_frame_next();
    s_inFrame = false;
  }
  t[2] = SDL_GetTicksNS();
  pump_events();
  dump_poll();
  s_step = false;
  skip |= s_paused;
  while (s_paused && !s_step) {
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
  if (s_retrace <= 3) {
    shader_wait();
    skip = true;
  }
  t[5] = SDL_GetTicksNS();
  if (!s_inFrame) {
    s_inFrame = aurora_begin_frame();
    begin_frame_state();
    ssaa_frame();
    wide_frame();
  }
  mkgp2pc_skip_draw = s_inFrame && !show_next_frame();
  t[6] = lastReturn = SDL_GetTicksNS();
  spike_log(t, skip);
  stall_beat(true);
}
