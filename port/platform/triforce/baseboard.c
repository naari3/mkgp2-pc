/*
 * Triforce AM baseboard, PC side.
 *
 * Replaces src/triforce/amexi.c function for function. The original talks to
 * the baseboard over EXI (channel 0, device 2): an addressed byte store that
 * the am* libraries use as backup RAM, plus a few 16-bit registers used by
 * the media board network code. Here the store is a plain array and the
 * registers read as zero, so ambackup / amcoin / amsys above run unchanged.
 *
 * The protocol facts come from the decompiled amexi.c itself (command bytes
 * 01 set address, 02 write byte, 03 read, 05 block write, 82/83/86/87/FF
 * registers). The store is not saved to disk yet.
 *
 * Symbol names are the address-based ones upstream uses today; when upstream
 * names them, rename here too (link_census.py reports them as missing).
 */
#include <stdint.h>

#include "mkgp2pc/triforce.h"
#include <string.h>

#include <mkgp2pc/platform.h>

#define STORE_SIZE 0x10000u /* the address field of the protocol is 16 bits */

static uint8_t s_store[STORE_SIZE];
static void (*s_exiCallback[3])(int32_t chan, void* context);

/* exi_disconnect */
int fn_80286B80(int32_t chan) {
  (void)chan;
  return 1;
}

/* exi_connect2: the original checks EXI ID 0x06041000 ("GC-AM BASE PCB"). */
int fn_80286BD4(int32_t chan, uint32_t dev) {
  (void)chan, (void)dev;
  return 1;
}

/* write one byte at addr */
int fn_80286C54(int32_t chan, uint32_t dev, uint16_t addr, uint8_t data) {
  (void)chan, (void)dev;
  s_store[addr] = data;
  return 1;
}

/* read one byte at addr */
int fn_80286E78(int32_t chan, uint32_t dev, uint16_t addr, uint8_t* data) {
  (void)chan, (void)dev;
  *data = s_store[addr];
  return 1;
}

static uint32_t clamp_len(uint16_t addr, uint32_t len) {
  return addr + len > STORE_SIZE ? STORE_SIZE - addr : len;
}

/* block write (EXI DMA in the original); `sync` 0 would complete from an
 * interrupt there, here it is always complete on return */
int fn_802870A4(int32_t chan, uint32_t dev, uint16_t addr, void* buf, uint32_t len, int sync) {
  (void)chan, (void)dev, (void)sync;
  memcpy(&s_store[addr], buf, clamp_len(addr, len));
  return 1;
}

/* block read */
int fn_802873A0(int32_t chan, uint32_t dev, uint16_t addr, void* buf, uint32_t len, int sync) {
  (void)chan, (void)dev, (void)sync;
  memset(buf, 0, len);
  memcpy(buf, &s_store[addr], clamp_len(addr, len));
  return 1;
}

/* wait until the channel is idle */
void fn_802876F4(int32_t chan) { (void)chan; }

/* 16-bit registers (commands 82 / 86 read, 83 / 87 / FF write): media board
 * and interrupt control. No media board here: reads give 0, writes are
 * dropped. */
int fn_8028772C(int32_t chan, uint32_t dev, uint16_t* data) {
  /* interrupt status: bit 2 = the media board has an answer (amnet_nm3.c) */
  *data = (chan == 0 && dev == 2 && mkgp2pc_mediaboard_irq()) ? 2 : 0;
  return 1;
}
int fn_802878B4(int32_t chan, uint32_t dev, uint16_t data) {
  (void)chan, (void)dev, (void)data;
  return 1;
}
int fn_802879FC(int32_t chan, uint32_t dev, uint16_t data) {
  (void)chan, (void)dev, (void)data;
  return 1;
}
int fn_80287B44(int32_t chan, uint32_t dev, uint16_t* data) {
  (void)chan, (void)dev;
  *data = 0;
  return 1;
}
int fn_80287CCC(int32_t chan, uint32_t dev, uint16_t data) {
  (void)chan, (void)dev, (void)data;
  return 1;
}

void mkgp2pc_baseboard_raise_exi(int channel) {
  if (channel >= 0 && channel < 3 && s_exiCallback[channel]) {
    s_exiCallback[channel](channel, 0);
  }
}

/* EXISetExiCallback */
void fn_80287E14(int32_t chan, void (*callback)(int32_t, void*)) {
  if (chan >= 0 && chan < 3) {
    s_exiCallback[chan] = callback;
  }
}
