/*
 * mkgp2-pc entry point.
 *
 * For now this only proves the build: it starts Aurora, brings the (null)
 * platform back ends up, and presents a cleared frame until the window is
 * closed. The game's own main loop replaces draw() once game code compiles
 * for the host.
 *
 *   --headless     no window, render offscreen with the null GPU back end
 *   --frames N     exit after N frames (used by the CTest smoke test)
 */
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/main.h>
#include <dolphin/gx.h>

#include <mkgp2pc/platform.h>
#include <mkgp2pc/stub.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void log_callback(AuroraLogLevel level, const char* module, const char* message, unsigned int len) {
  static const char* const names[] = {"DEBUG", "INFO", "WARNING", "ERROR", "FATAL"};
  FILE* out = level >= LOG_ERROR ? stderr : stdout;
  (void)len;
  fprintf(out, "[%s] %s: %s\n", names[level], module, message);
  if (level == LOG_FATAL) {
    fflush(out);
    abort();
  }
}

static void draw(void) {
  /* Aurora clears to this colour at the start of the next frame. */
  GXSetCopyClear((GXColor){.r = 0, .g = 40, .b = 120, .a = 255}, GX_MAX_Z24);
}

int main(int argc, char* argv[]) {
  bool headless = false;
  long frameLimit = -1;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--headless") == 0) {
      headless = true;
    } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
      frameLimit = strtol(argv[++i], NULL, 10);
    }
  }

  AuroraConfig config = {
      .appName = "mkgp2-pc",
      .logCallback = &log_callback,
      .windowWidth = 640,
      .windowHeight = 480,
  };
  if (headless) {
    config.desiredBackend = BACKEND_NULL;
    config.surface.type = AURORA_SURFACE_HEADLESS;
    config.surface.width = 640;
    config.surface.height = 480;
  }
  aurora_initialize(argc, argv, &config);

  mkgp2pc_platform_init();
  int backendCount = 0;
  const Mkgp2PcBackendInfo* backends = mkgp2pc_platform_backends(&backendCount);
  for (int i = 0; i < backendCount; ++i) {
    printf("platform: %s = %s\n", backends[i].family, backends[i].backend);
  }

  bool exiting = false;
  bool paused = false;
  long frames = 0;
  while (!exiting) {
    const AuroraEvent* event = aurora_update();
    while (event != NULL && event->type != AURORA_NONE) {
      switch (event->type) {
      case AURORA_EXIT:
        exiting = true;
        break;
      case AURORA_PAUSED:
        paused = true;
        break;
      case AURORA_UNPAUSED:
        paused = false;
        break;
      default:
        break;
      }
      ++event;
    }
    if (exiting || paused || !aurora_begin_frame()) {
      continue;
    }
    draw();
    aurora_end_frame();
    if (frameLimit >= 0 && ++frames >= frameLimit) {
      exiting = true;
    }
  }

  printf("frames presented: %ld, SDK stubs reached: %d\n", frames, mkgp2pc_stub_count());
  mkgp2pc_platform_shutdown();
  aurora_shutdown();
  return 0;
}
