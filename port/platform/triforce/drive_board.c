/* Steering drive board ("STR PCB") on the baseboard's serial port 0.
 *
 * Minimal stand-in, written from the game's side of the protocol
 * (src/game/StrPcb_Tick.c): the game sends 10-byte packets (target position,
 * motor strength, effect) and reads 3-byte messages:
 *   "C01"  ready (idle)          "C06"  initialising (busy)
 *   "Exx"  error code xx; "E00" is "no error"
 *   'H' hi lo   current wheel position, 0..0x3FF, centre 0x1FF
 * It also gives up when nothing has arrived for a while.
 *
 * The game's power-on check (StrPcb_TimerTick) raises output bit 0x80 of the
 * I/O board and waits for "C01", then ramps the strength down, drops the bit
 * and waits for "C06". This stand-in therefore answers every packet with
 * "C01" while the bit is high and "C06" while it is low. That is inferred
 * from the game's code alone, not from a real board; the wheel never moves
 * and force feedback output is dropped.
 */
#include <stdint.h>
#include <string.h>

#include <mkgp2pc/triforce.h>

static uint8_t s_rx[64];
static int s_rxLen;
static int s_txCount;
static int s_enabled;

void mkgp2pc_jvs_outputs(uint8_t bits) { s_enabled = (bits & 0x80) != 0; }

void mkgp2pc_serial_write(int port, const uint8_t* data, int n) {
  if (port != 0) {
    return;
  }
  s_txCount += n;
  while (s_txCount >= 10) {
    s_txCount -= 10;
    if (s_rxLen + 3 <= (int)sizeof(s_rx)) {
      memcpy(&s_rx[s_rxLen], s_enabled ? "C01" : "C06", 3);
      s_rxLen += 3;
    }
  }
}

int mkgp2pc_serial_read(int port, uint8_t* out, int max) {
  if (port != 0) {
    return 0;
  }
  int n = s_rxLen < max ? s_rxLen : max;
  memcpy(out, s_rx, (size_t)n);
  memmove(s_rx, &s_rx[n], (size_t)(s_rxLen - n));
  s_rxLen -= n;
  return n;
}
