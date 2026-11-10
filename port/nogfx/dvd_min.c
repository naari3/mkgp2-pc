/* DVD for the bring-up executable: media board addresses go to the board
 * emulation; there is no disc yet, disc reads return zeros. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mkgp2pc/triforce.h"

struct DVDCommandBlock;
typedef void (*DVDCBCallback)(int32_t result, struct DVDCommandBlock* block);

int DVDReadAbsAsyncPrio(struct DVDCommandBlock* block, void* addr, int32_t length, int32_t offset,
                        DVDCBCallback callback, int32_t prio) {
  static int warned;
  (void)prio;
  if (mkgp2pc_mediaboard_owns((uint32_t)offset)) {
    mkgp2pc_mediaboard_read((uint32_t)offset, addr, (uint32_t)length);
  } else {
    if (!warned++) {
      fprintf(stderr, "nogfx: disc read at 0x%08X (no disc; zeros)\n", (unsigned)offset);
    }
    memset(addr, 0, (size_t)length);
  }
  if (callback) {
    callback(length, block);
  }
  return 1;
}

int32_t DVDGetCommandBlockStatus(const struct DVDCommandBlock* block) {
  (void)block;
  return 0; /* everything completes inside the call */
}
