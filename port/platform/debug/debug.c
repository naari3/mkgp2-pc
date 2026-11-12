/*
 * The game's own debug facilities (docs/port/debug.md).
 *
 * What the arcade program has: DebugPrintf (about 670 call sites) and
 * DebugTextOutput format a string and hand it to OSReport when the "debug
 * display" byte of the backup settings (lbl_80598A60[0x2A]) is set;
 * OverlayText(layer, column, row, format, ...) formats a string for a
 * character grid on screen under the same condition; OSPanic reports a
 * failed check. In the retail build OSReport and OSPanic are empty, the
 * drawing of OverlayText is gone, and nothing sets the byte (the test menu
 * still lists "*DEBUG DISPLAY" but the cursor never stops there).
 *
 * Here: the byte follows a switch, reports go to stderr (gfx.log), and the
 * overlay strings of a frame are kept for the renderer
 * (port/gfx/debug_overlay.cpp) and for the state dump.
 */
#include <mkgp2pc/debug.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <iconv.h>
#endif

/* the game's backup settings and its flow dispatcher (FlowDispatcher_private.h) */
extern unsigned char lbl_80598A60[];
extern void* FlowDispatcher_Create_bss_806D0F80;
#define DEBUG_DISPLAY_BYTE 0x2A
#define DISPATCHER_OVERLAY 5 /* int index of FlowDispatcher::overlay */
#define OVERLAY_PAGES 14

static int s_on = -1;

int mkgp2pc_debug_on(void) {
  if (s_on < 0) {
    const char* e = getenv("MKGP2PC_DEBUG");
    s_on = e != NULL && atoi(e) != 0;
    if (s_on) {
      fprintf(stderr, "debug: on (MKGP2PC_DEBUG)\n");
    }
  }
  return s_on;
}

void mkgp2pc_debug_set(int on) {
  s_on = on != 0;
  fprintf(stderr, "debug: %s\n", s_on ? "on" : "off");
}

void mkgp2pc_debug_frame(void) {
  /* Written both ways: the byte is part of the saved settings, and a 1
   * left there by an earlier run must not outlive the switch. */
  lbl_80598A60[DEBUG_DISPLAY_BYTE] = (unsigned char)mkgp2pc_debug_on();
}

void mkgp2pc_debug_page(int step) {
  int* dispatcher = (int*)FlowDispatcher_Create_bss_806D0F80;
  if (dispatcher == NULL) {
    return;
  }
  int page = (dispatcher[DISPATCHER_OVERLAY] + step) % OVERLAY_PAGES;
  if (page < 0) {
    page += OVERLAY_PAGES;
  }
  dispatcher[DISPATCHER_OVERLAY] = page;
  fprintf(stderr, "debug: overlay page %d\n", page);
}

/* The game's strings are Shift-JIS. Returns out. */
static const char* to_utf8(const char* in, char* out, size_t size) {
  size_t n = strlen(in);
  size_t i;
  for (i = 0; i < n && (unsigned char)in[i] < 0x80; i++) {
  }
  if (i == n || size < 8) {
    snprintf(out, size, "%s", in);
    return out;
  }
#ifdef _WIN32
  {
    wchar_t wide[1024];
    int w = MultiByteToWideChar(932, 0, in, (int)n, wide, 1024);
    int len = w > 0 ? WideCharToMultiByte(CP_UTF8, 0, wide, w, out, (int)size - 1, NULL, NULL) : 0;
    if (len > 0) {
      out[len] = 0;
      return out;
    }
  }
#else
  {
    static iconv_t cd = (iconv_t)-1;
    static int tried;
    if (!tried) {
      tried = 1;
      cd = iconv_open("UTF-8", "CP932");
    }
    if (cd != (iconv_t)-1) {
      char* src = (char*)in;
      char* dst = out;
      size_t left = n, room = size - 1;
      iconv(cd, NULL, NULL, NULL, NULL);
      if (iconv(cd, &src, &left, &dst, &room) != (size_t)-1) {
        *dst = 0;
        return out;
      }
    }
  }
#endif
  /* no converter, or not Shift-JIS: keep the ASCII part readable */
  for (i = 0; i + 1 < size && in[i] != 0; i++) {
    out[i] = (unsigned char)in[i] < 0x80 ? in[i] : '?';
  }
  out[i] = 0;
  return out;
}

/* "game: " in front of every line; the game's strings end lines as they like. */
static void report_text(const char* text) {
  static int midLine;
  char utf8[4200];
  const char* p = to_utf8(text, utf8, sizeof(utf8));
  while (*p != 0) {
    size_t len = strcspn(p, "\n");
    if (!midLine) {
      fputs("game: ", stderr);
    }
    fwrite(p, 1, len, stderr);
    p += len;
    midLine = 1;
    if (*p == '\n') {
      fputc('\n', stderr);
      midLine = 0;
      p++;
    }
  }
}

void mkgp2pc_debug_vreport(const char* fmt, va_list args) {
  char buf[2048];
  if (!mkgp2pc_debug_on() || fmt == NULL) {
    return;
  }
  vsnprintf(buf, sizeof(buf), fmt, args);
  report_text(buf);
}

/*
 * A failed check of the game. The retail OSPanic returns and the game goes
 * on, so this does too. Reported whether debugging is on or not, because it
 * is the one message that says the port did something the game did not
 * expect; each place a few times only, since some sit in per-frame code.
 */
void mkgp2pc_debug_vpanic(const char* file, int line, const char* fmt, va_list args) {
  enum { SITES = 64, TIMES = 3 };
  static struct {
    const char* file;
    int line;
    int count;
  } sites[SITES];
  char buf[1024];
  char utf8[2100];
  int i;
  for (i = 0; i < SITES && sites[i].file != NULL; i++) {
    if (sites[i].file == file && sites[i].line == line) {
      break;
    }
  }
  if (i < SITES) {
    sites[i].file = file;
    sites[i].line = line;
    if (++sites[i].count > TIMES && !mkgp2pc_debug_on()) {
      return;
    }
  }
  buf[0] = 0;
  if (fmt != NULL) {
    vsnprintf(buf, sizeof(buf), fmt, args);
  }
  fprintf(stderr, "game: OSPanic %s:%d: %s%s\n", file != NULL ? file : "?", line, to_utf8(buf, utf8, sizeof(utf8)),
          i < SITES && sites[i].count == TIMES && !mkgp2pc_debug_on() ? " (not reported again)" : "");
}

enum { OVERLAY_MAX = 512, OVERLAY_TEXT = 256 };
static struct {
  short layer, column, row;
  char text[OVERLAY_TEXT];
} s_overlay[OVERLAY_MAX];
static int s_overlayCount;

void mkgp2pc_debug_overlay_text(int layer, int column, int row, const char* text) {
  if (s_overlayCount >= OVERLAY_MAX || text == NULL) {
    return;
  }
  s_overlay[s_overlayCount].layer = (short)layer;
  s_overlay[s_overlayCount].column = (short)column;
  s_overlay[s_overlayCount].row = (short)row;
  to_utf8(text, s_overlay[s_overlayCount].text, OVERLAY_TEXT);
  s_overlayCount++;
}

int mkgp2pc_debug_overlay_count(void) { return s_overlayCount; }

int mkgp2pc_debug_overlay_get(int index, int* layer, int* column, int* row, const char** text) {
  if (index < 0 || index >= s_overlayCount) {
    return 0;
  }
  *layer = s_overlay[index].layer;
  *column = s_overlay[index].column;
  *row = s_overlay[index].row;
  *text = s_overlay[index].text;
  return 1;
}

void mkgp2pc_debug_overlay_clear(void) { s_overlayCount = 0; }

int mkgp2pc_debug_start_scene(int mode) {
  const char* e = getenv("MKGP2PC_START_SCENE");
  char* end;
  long scene;
  if (e == NULL || *e == 0) {
    return mode;
  }
  scene = strtol(e, &end, 10);
  /* -4 is the developers' scene list; the table has entries 0..19 */
  if (*end != 0 || (scene != -4 && (scene < 0 || scene > 19))) {
    fprintf(stderr, "MKGP2PC_START_SCENE: '%s' is not -4 or 0..19\n", e);
    return mode;
  }
  fprintf(stderr, "debug: starting with scene %ld (MKGP2PC_START_SCENE) in place of %d\n", scene, mode);
  return (int)scene;
}
