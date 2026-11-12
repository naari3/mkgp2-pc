/* Port-side decisions about when the cabinet link shows itself to the
 * player (docs/port/cabinet_link.md, "Start-up without the wait"). The
 * sockets are in netlink.c; this file only answers the game's flow. */
#include <stdlib.h>

/* Called from BootPCBCheck_Init (port/patches/src/game/PCBCheckRecv.c.patch)
 * with the arcade program's length of the "PCB ID : n CHECKING..." screen
 * in frames (0xE10, 60 seconds). The screen only shows which boards have
 * connected; the link is kept up by PCBComm_Process on every frame of the
 * game afterwards and nothing reads a result of the wait. The port skips
 * it. MKGP2PC_BOOT_CHECK=1 gives the arcade length, any other number that
 * many seconds. */
int mkgp2pc_boot_check_frames(int arcade) {
  const char* e = getenv("MKGP2PC_BOOT_CHECK");
  int seconds;
  if (e == NULL) {
    return 0;
  }
  seconds = atoi(e);
  if (seconds == 1) {
    return arcade;
  }
  return seconds > 0 && seconds <= 600 ? seconds * 60 : 0;
}
