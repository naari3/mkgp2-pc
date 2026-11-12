/*
 * MKGP2PC_BOOT_SKIP=1: the start-up screens that only matter on the cabinet
 * are passed over (port/patches/src/game/BootDispatcher.cpp.patch).
 *
 *  - Device check (card reader, printer, camera; "CardTaskManager" in the
 *    game): it still runs, because it is also what brings the card unit
 *    up, but without the frame limit and with most frames not drawn, as
 *    under F10. mkgp2pc_boot_fast is 1 while that is so; the video layer
 *    (port/gfx/video_aurora.c) reads it.
 *  - NOTICE (the ten second "for use in Japan" text): not entered at all,
 *    the way the game itself leaves it out when the byte at 0x2A of its
 *    settings record is set.
 *
 * The board check between the two is a matter of the cabinet link
 * (MKGP2PC_BOOT_CHECK, docs/port/cabinet_link.md); it falls inside the fast
 * stretch as well.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

uint32_t VIGetRetraceCount(void);

int mkgp2pc_boot_fast;

int mkgp2pc_boot_skip(void) {
  static int skip = -1;
  if (skip < 0) {
    const char* e = getenv("MKGP2PC_BOOT_SKIP");
    skip = e != NULL && *e != 0 && *e != '0';
  }
  return skip;
}

/* 1 before the device check, 0 when the start-up screens are over. */
void mkgp2pc_boot_phase(int on) {
  mkgp2pc_boot_fast = on && mkgp2pc_boot_skip();
  fprintf(stderr, "boot: start-up screens %s at retrace %u%s\n", on ? "begin" : "end", (unsigned)VIGetRetraceCount(),
          mkgp2pc_boot_skip() ? " (MKGP2PC_BOOT_SKIP)" : "");
}
