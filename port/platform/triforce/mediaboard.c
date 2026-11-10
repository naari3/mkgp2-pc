/*
 * Triforce media board ("GC-AM", device id 0x21), seen from the game.
 *
 * The decompiled amnet / amsock code (src/triforce) stays as it is; this file
 * is the board underneath it. Everything here was read off those sources:
 *
 *  - The board is reached through the DVD interface. Command 0x65 returns the
 *    board id, 0x67 selects a priority, 0x66 writes a block to a board
 *    address, and an ordinary absolute read (DVDReadAbsAsyncPrio) of a board
 *    address reads one. src/triforce/dvdcmd.c issued 0x65-0x67 through dvd.c's
 *    queue; it is replaced by the three functions at the bottom.
 *  - Addresses from 0x80000000 are registers: 0x80000000 hardware flags,
 *    0x80000040 a 32-byte header with "GCAM" at +4 and the base of the
 *    board's memory window in the first four bytes.
 *  - Inside the window, base - 0x01000000 + 0x900000 and + 0x900020 are two
 *    32-byte mailboxes. The game writes a request packet to the second and
 *    the board answers in the first, raising the media board interrupt
 *    (baseboard register bit 2). The game clears the first mailbox when it
 *    has taken the answer.
 *  - Packet: u8 id, u8 sub, u16 command, u32 result, u32 arg[6]. The game
 *    stores the fields the way a big-endian CPU would, so on this host the
 *    command a request carries is the byte-swapped value of what amsock.c
 *    calls it; an answer is the request with 0x80 set in the raw command, and
 *    raw 0x0080 alone means "error".
 *
 * Implemented: what amsys needs to finish booting (base query, version,
 * serial number). Every network command is answered with "error", which the
 * game treats as "no network".
 *
 * The window base is placed above any disc offset so that media board reads
 * and disc reads through the same DVD call cannot be confused.
 */
#include <stdint.h>
#include <string.h>

#include "mkgp2pc/triforce.h"

#define BOARD_ID 0x21u
#define WINDOW_BASE 0x71000000u /* what the header reports */
#define WINDOW_OFF (WINDOW_BASE - 0x01000000u)
#define WINDOW_LO (WINDOW_OFF + 0x00800000u)
#define WINDOW_HI (WINDOW_OFF + 0x00E00000u)
#define MAILBOX_ANSWER (WINDOW_OFF + 0x00900000u)
#define MAILBOX_REQUEST (WINDOW_OFF + 0x00900020u)

typedef struct {
  uint8_t id;
  uint8_t sub;
  uint16_t command; /* raw, as the game stored it */
  uint32_t result;
  uint32_t arg[6];
} Packet;

static uint8_t s_window[WINDOW_HI - WINDOW_LO];
static int s_answerPending;

static uint32_t swap32(uint32_t v) { return (v << 24) | ((v << 8) & 0xFF0000) | ((v >> 8) & 0xFF00) | (v >> 24); }

int mkgp2pc_mediaboard_owns(uint32_t address) { return address >= WINDOW_LO; }
int mkgp2pc_mediaboard_irq(void) { return s_answerPending; }

static void request(const Packet* in) {
  Packet out = *in;
  out.command = (uint16_t)(in->command | 0x80);
  switch (in->command) {
  case 0x0100: /* window base; amsys stores the answer as base[0] */
    out.result = swap32(WINDOW_BASE);
    break;
  case 0x0101: { /* version: two 16-bit numbers */
    static const uint8_t ver[4] = {0x01, 0x00, 0x00, 0x00};
    memcpy(&out.result, ver, 4);
    break;
  }
  case 0x0301: /* serial number: 16 characters, amsys inserts the '-' at 4 */
    memcpy(&out.result, "PC00000000000000", 16);
    break;
  default:
    out.command = 0x0080;
    break;
  }
  memcpy(&s_window[MAILBOX_ANSWER - WINDOW_LO], &out, sizeof(out));
  s_answerPending = 1;
  mkgp2pc_baseboard_raise_exi(2);
}

void mkgp2pc_mediaboard_read(uint32_t address, void* buffer, uint32_t length) {
  memset(buffer, 0, length);
  if (address >= 0x80000000u) {
    if (address == 0x80000040u && length >= 8) {
      /* The game rebuilds the base as (b2 << 24) | (b3 << 16) | (b0 << 8) | b1. */
      uint8_t* b = (uint8_t*)buffer;
      b[0] = (uint8_t)(WINDOW_BASE >> 8);
      b[1] = (uint8_t)WINDOW_BASE;
      b[2] = (uint8_t)(WINDOW_BASE >> 24);
      b[3] = (uint8_t)(WINDOW_BASE >> 16);
      memcpy(b + 4, "GCAM", 4);
    }
    return; /* other registers, hardware flags included, read 0 */
  }
  if (address >= WINDOW_LO && address < WINDOW_HI) {
    uint32_t n = WINDOW_HI - address;
    memcpy(buffer, &s_window[address - WINDOW_LO], length < n ? length : n);
  }
}

void mkgp2pc_mediaboard_write(uint32_t address, const void* buffer, uint32_t length) {
  if (address == MAILBOX_REQUEST && length >= sizeof(Packet)) {
    Packet p;
    memcpy(&p, buffer, sizeof(p));
    if (p.command != 0) {
      request(&p); /* taken at once: the request mailbox always reads empty */
    }
    return;
  }
  if (address >= WINDOW_LO && address < WINDOW_HI) {
    uint32_t n = WINDOW_HI - address;
    memcpy(&s_window[address - WINDOW_LO], buffer, length < n ? length : n);
    if (address == MAILBOX_ANSWER) {
      s_answerPending = 0; /* the game cleared the answer */
    }
  }
}

/* ---- replaces src/triforce/dvdcmd.c ------------------------------------- */
struct DVDCommandBlock;
typedef void (*DVDCBCallback)(int32_t result, struct DVDCommandBlock* block);

/* 0x66: write a block to the board. The block's state stays 0 (finished). */
int fn_80286934(struct DVDCommandBlock* block, void* addr, int32_t length, int32_t offset,
                DVDCBCallback callback, int32_t prio) {
  (void)prio;
  mkgp2pc_mediaboard_write((uint32_t)offset, addr, (uint32_t)length);
  if (callback) {
    callback(length, block);
  }
  return 1;
}

/* 0x65: board id; the game takes the low byte of the byte-swapped value. */
int32_t fn_802869A0(struct DVDCommandBlock* block) {
  (void)block;
  return (int32_t)(BOARD_ID << 24);
}

/* 0x67: priority select before a disc-range transfer. Nothing to do. */
int32_t fn_80286A74(struct DVDCommandBlock* block, uint32_t arg) {
  (void)block, (void)arg;
  return 0;
}
