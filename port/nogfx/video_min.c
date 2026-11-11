/*
 * VI retrace and GX draw-done callbacks for the bring-up executable: nothing
 * is drawn, but the game's frame hand-shake (sysdolphin video.c) needs the
 * callbacks to arrive. Both run with "interrupts" disabled, as on the console.
 */
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

int OSDisableInterrupts(void);
int OSRestoreInterrupts(int);
void mkgp2pc_mediaboard_tick(void); /* port/platform/triforce/mediaboard.c */
void mkgp2pc_netlink_pump(void);   /* port/platform/triforce/netlink.c */
void mkgp2pc_script_tick(uint32_t retrace); /* input_script.c */

typedef void (*VIRetraceCallback)(uint32_t retraceCount);
typedef void (*GXDrawDoneCallback)(void);

static VIRetraceCallback s_pre, s_post;
static GXDrawDoneCallback s_drawDone;
static uint32_t s_retrace;

VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback cb) {
  VIRetraceCallback old = s_pre;
  s_pre = cb;
  return old;
}
VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb) {
  VIRetraceCallback old = s_post;
  s_post = cb;
  return old;
}
uint32_t VIGetRetraceCount(void) { return s_retrace; }

void VIWaitForRetrace(void) {
  struct timespec ts = {0, 1000000}; /* 1 ms: fast-forward, not 60 Hz */
  static int hz = -1;
  static struct timespec next;
  int level;
  if (hz < 0) { /* MKGP2PC_HZ=60: real time instead, for measurements of the cabinet link */
    const char* e = getenv("MKGP2PC_HZ");
    hz = e != NULL ? atoi(e) : 0;
    clock_gettime(CLOCK_MONOTONIC, &next);
  }
  if (hz > 0) {
    struct timespec now;
    next.tv_nsec += 1000000000 / hz;
    if (next.tv_nsec >= 1000000000) {
      next.tv_nsec -= 1000000000, next.tv_sec++;
    }
    for (;;) {
      clock_gettime(CLOCK_MONOTONIC, &now);
      if (now.tv_sec > next.tv_sec || (now.tv_sec == next.tv_sec && now.tv_nsec >= next.tv_nsec)) {
        break;
      }
      nanosleep(&ts, 0);
      level = OSDisableInterrupts();
      mkgp2pc_netlink_pump();
      OSRestoreInterrupts(level);
    }
    if (now.tv_sec - next.tv_sec > 1) {
      next = now; /* fell far behind (loading): do not catch up */
    }
  } else {
    nanosleep(&ts, 0);
  }
  level = OSDisableInterrupts();
  s_retrace++;
  mkgp2pc_script_tick(s_retrace);
  mkgp2pc_mediaboard_tick();
  if (s_pre) {
    s_pre(s_retrace);
  }
  if (s_post) {
    s_post(s_retrace);
  }
  OSRestoreInterrupts(level);
}

GXDrawDoneCallback GXSetDrawDoneCallback(GXDrawDoneCallback cb) {
  GXDrawDoneCallback old = s_drawDone;
  s_drawDone = cb;
  return old;
}
static void draw_done(void) {
  if (s_drawDone) {
    int level = OSDisableInterrupts();
    s_drawDone();
    OSRestoreInterrupts(level);
  }
}
void GXSetDrawDone(void) { draw_done(); }
void GXDrawDone(void) { draw_done(); }
void GXWaitDrawDone(void) {}
