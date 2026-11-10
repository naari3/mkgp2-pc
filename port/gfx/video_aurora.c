/*
 * Frame hand-shake with Aurora for the rendering executable (replaces
 * port/nogfx/video_min.c). The game's loop ends every frame in
 * VIWaitForRetrace; that is where the frame is presented, window events
 * are read and the next frame begins.
 */
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <dolphin/gx.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int OSDisableInterrupts(void);
int OSRestoreInterrupts(int);
void mkgp2pc_script_tick(uint32_t retrace); /* port/nogfx/input_script.c */

typedef void (*RetraceCallback)(uint32_t retraceCount);

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
    ++event;
  }
  int level = OSDisableInterrupts();
  s_retrace++;
  mkgp2pc_script_tick(s_retrace);
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
