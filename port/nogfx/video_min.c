/*
 * VI retrace and GX draw-done callbacks for the bring-up executable: nothing
 * is drawn, but the game's frame hand-shake (sysdolphin video.c) needs the
 * callbacks to arrive. Both run with "interrupts" disabled, as on the console.
 */
#include <stdint.h>
#include <time.h>

int OSDisableInterrupts(void);
int OSRestoreInterrupts(int);

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
  nanosleep(&ts, 0);
  int level = OSDisableInterrupts();
  s_retrace++;
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
