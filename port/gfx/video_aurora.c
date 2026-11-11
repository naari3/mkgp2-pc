/*
 * Frame hand-shake with Aurora for the rendering executable (replaces
 * port/nogfx/video_min.c). The game's loop ends every frame in
 * VIWaitForRetrace; that is where the frame is presented, window events
 * are read and the next frame begins.
 */
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <SDL3/SDL.h>
#include <dolphin/gx.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int OSDisableInterrupts(void);
int OSRestoreInterrupts(int);
void mkgp2pc_script_tick(uint32_t retrace); /* port/nogfx/input_script.c */

void mkgp2pc_jvs_set_switches(const uint8_t* bytes, int count);
void mkgp2pc_jvs_set_analog(int channel, uint16_t value);
void mkgp2pc_jvs_add_coin(int slot);
void mkgp2pc_baseboard_set_operator_switches(uint8_t pressed);

typedef void (*RetraceCallback)(uint32_t retraceCount);

/*
 * Keyboard as the cabinet's controls (the window has to have the focus):
 *   left / right   steering wheel (turns and returns gradually)
 *   up or Z        accelerator pedal
 *   down or X      brake pedal
 *   space, enter   item button (also "decide" in the menus)
 *   5 or C         coin
 *   F1 / F2        test / service button of the cabinet
 * Once a key has been used the keyboard owns the inputs; until then a
 * script (MKGP2PC_SCRIPT) may drive them.
 */
static bool s_keyLeft, s_keyRight, s_keyAccel, s_keyBrake, s_keyItem, s_keyTest, s_keyService;
static bool s_keyboardUsed;
static int s_wheel; /* -0x7FFF .. 0x7FFF */

static void key_event(const SDL_Event* e) {
  bool down = e->type == SDL_EVENT_KEY_DOWN;
  switch (e->key.scancode) {
  case SDL_SCANCODE_LEFT: s_keyLeft = down; break;
  case SDL_SCANCODE_RIGHT: s_keyRight = down; break;
  case SDL_SCANCODE_UP: case SDL_SCANCODE_Z: s_keyAccel = down; break;
  case SDL_SCANCODE_DOWN: case SDL_SCANCODE_X: s_keyBrake = down; break;
  case SDL_SCANCODE_SPACE: case SDL_SCANCODE_RETURN: s_keyItem = down; break;
  case SDL_SCANCODE_F1: s_keyTest = down; break;
  case SDL_SCANCODE_F2: s_keyService = down; break;
  case SDL_SCANCODE_5: case SDL_SCANCODE_C:
    if (down && !e->key.repeat) {
      mkgp2pc_jvs_add_coin(0);
    }
    break;
  default: return;
  }
  s_keyboardUsed = true;
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
  mkgp2pc_jvs_set_analog(0, (uint16_t)(0x8000 + s_wheel));
  mkgp2pc_jvs_set_analog(1, s_keyAccel ? 0xFFFF : 0);
  mkgp2pc_jvs_set_analog(2, s_keyBrake ? 0xFFFF : 0);
  uint8_t sw[5] = {0, 0, (uint8_t)(s_keyItem ? 0x20 : 0), 0, 0};
  mkgp2pc_jvs_set_switches(sw, 5);
  /* 0x01: the DIP switch that skips the card reader, printer and camera */
  mkgp2pc_baseboard_set_operator_switches((uint8_t)(0x01 | (s_keyTest ? 0x40 : 0) | (s_keyService ? 0x80 : 0)));
}

static RetraceCallback s_pre, s_post;
static uint32_t s_retrace;
static bool s_inFrame;
static long s_frameLimit = -1;

static void log_callback(AuroraLogLevel level, const char* module, const char* message, unsigned int len) {
  static const char* const names[] = {"DEBUG", "INFO", "WARNING", "ERROR", "FATAL"};
  (void)len;
  fprintf(stderr, "[%s] %s: %s\n", names[level], module, message);
  if (level == LOG_FATAL) {
    abort();
  }
}

/* The link wraps main (ld --wrap, port/tools/link_census.py): Aurora has to
 * be up before the game's start-up calls GXInit and VIInit, and after the
 * C++ static objects of Aurora itself exist (so not from a constructor). */
int __real_main(int argc, char** argv);
int __wrap_main(int argc, char** argv) {
  AuroraConfig config;
  memset(&config, 0, sizeof(config));
  config.appName = "mkgp2-pc";
  config.logCallback = &log_callback;
  config.windowWidth = 640;
  config.windowHeight = 480;
  const char* limit = getenv("MKGP2PC_FRAMES");
  if (limit != NULL) {
    s_frameLimit = strtol(limit, NULL, 10);
  }
  aurora_initialize(argc, argv, &config);
  s_inFrame = aurora_begin_frame();
  return __real_main(argc, argv);
}

void mkgp2pc_audio_frame(void); /* audio_musyx.c */

/* The game counts on 59.94 retraces a second: its logic advances once per
 * frame. The display's own rate may be anything (or the presentation may not
 * wait at all), so the pace is kept here. MKGP2PC_NOLIMIT=1 turns it off
 * (scripted runs). */
static void frame_limit(void) {
  static Uint64 next;
  static int off = -1;
  const Uint64 period = 1001000000ull / 60;
  if (off < 0) {
    off = getenv("MKGP2PC_NOLIMIT") != NULL;
  }
  if (off) {
    return;
  }
  Uint64 now = SDL_GetTicksNS();
  if (next == 0 || now > next + 4 * period) {
    next = now; /* first frame, or too far behind to catch up */
  }
  if (now < next) {
    SDL_DelayNS(next - now);
  }
  next += period;
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

void VIWaitForRetrace(void) {
  if (s_inFrame) {
    aurora_end_frame();
    s_inFrame = false;
  }
  const AuroraEvent* event = aurora_update();
  while (event != NULL && event->type != AURORA_NONE) {
    if (event->type == AURORA_EXIT) {
      aurora_shutdown();
      exit(0);
    }
    if (event->type == AURORA_SDL_EVENT &&
        (event->sdl.type == SDL_EVENT_KEY_DOWN || event->sdl.type == SDL_EVENT_KEY_UP)) {
      key_event(&event->sdl);
    }
    ++event;
  }
  mkgp2pc_audio_frame();
  frame_limit();
  int level = OSDisableInterrupts();
  s_retrace++;
  mkgp2pc_script_tick(s_retrace);
  keyboard_tick();
  if (s_pre) {
    s_pre(s_retrace);
  }
  if (s_post) {
    s_post(s_retrace);
  }
  OSRestoreInterrupts(level);
  if (s_frameLimit >= 0 && (long)s_retrace >= s_frameLimit) {
    aurora_shutdown();
    exit(0);
  }
  s_inFrame = aurora_begin_frame();
}
