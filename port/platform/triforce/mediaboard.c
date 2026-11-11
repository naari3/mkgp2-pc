/*
 * Triforce media board with its network board, seen from the game.
 *
 * The decompiled amnet / amsock code (src/triforce) stays as it is; this file
 * is the hardware underneath it. Everything here was read off those sources
 * (amnet_setup_nm.c, amnet_nm3.c, amnet_init.c, amsock.c, amsys.c):
 *
 *  - The board is reached through the DVD interface. Command 0x65 returns the
 *    board id, 0x67 selects a priority, 0x66 writes a block to a board
 *    address, and an ordinary absolute read (DVDReadAbsAsyncPrio) of a board
 *    address reads one. src/triforce/dvdcmd.c issued 0x65-0x67 through dvd.c's
 *    queue; it is replaced by the three functions at the bottom. All board
 *    addresses used are 0x80000000 and up, so they cannot be mistaken for
 *    disc offsets.
 *  - Board id 0x29 is the kind that carries a network board (0xA9). The game
 *    does not finish booting without one: BootDispatcher waits for the
 *    network board's version bytes.
 *  - Registers: 0x80000120 hardware flags, 0x80000140 bit 0 "main firmware
 *    running", 0x80000160 bit 0 "network board busy".
 *  - Each board has two 32-byte mailboxes: main 0x84000000 / 0x84000020,
 *    network 0x89000000 / 0x89000200. The game writes a request packet to the
 *    second and the board answers in the first, raising its interrupt
 *    (baseboard status register: bit 2 main, bit 4 network). The game clears
 *    the first mailbox when it has taken the answer.
 *  - Packet: u8 id, u8 sub, u16 command, u32 result, u32 arg[6]. The game
 *    stores the fields the way a big-endian CPU would, so on this host the
 *    command a request carries is the byte-swapped value of what amsock.c
 *    calls it; an answer is the request with 0x80 set in the raw command, and
 *    raw 0x0080 alone means "error".
 *
 * Implemented: what amsys needs to finish booting (address query, version,
 * serial number, for both boards) and, in netlink.c, the socket commands.
 * Every other command is answered with "error". Values marked "guess" are
 * not known from the sources.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mkgp2pc/triforce.h"

#define BOARD_ID 0x29u

typedef struct {
  uint8_t id;
  uint8_t sub;
  uint16_t command; /* raw, as the game stored it */
  uint32_t result;
  uint32_t arg[6];
} Packet;

typedef struct {
  uint32_t answer, request; /* mailbox addresses */
  int pending;              /* an answer waits */
  uint32_t address;         /* answer to command 0x0100 */
  uint8_t version[2];
  const char* serial;
} Board;

static Board s_boards[2] = {
    {0x84000000u, 0x84000020u, 0, 0xC0A81D02u /* guess: 192.168.29.2 */, {0x01, 0x00}, "PCMB000000000000"},
    {0x89000000u, 0x89000200u, 0, 0xC0A81D02u, {0x01, 0x01} /* both bytes must be non-zero */, "PCNB000000000000"},
};

/* Board memory, allocated in 64 KB pages on first write. */
#define PAGE 0x10000u
static uint8_t* s_pages[0x8000];

static uint8_t* page(uint32_t address, int create) {
  uint32_t i = (address - 0x80000000u) / PAGE;
  if (!s_pages[i] && create) {
    s_pages[i] = (uint8_t*)calloc(1, PAGE);
  }
  return s_pages[i];
}

void mkgp2pc_mediaboard_store(uint32_t address, const uint8_t* data, uint32_t length) {
  while (length) {
    uint32_t off = address % PAGE;
    uint32_t n = PAGE - off < length ? PAGE - off : length;
    memcpy(page(address, 1) + off, data, n);
    address += n, data += n, length -= n;
  }
}

void mkgp2pc_mediaboard_load(uint32_t address, uint8_t* data, uint32_t length) {
  while (length) {
    uint32_t off = address % PAGE;
    uint32_t n = PAGE - off < length ? PAGE - off : length;
    uint8_t* p = page(address, 0);
    if (p) {
      memcpy(data, p + off, n);
    } else {
      memset(data, 0, n);
    }
    address += n, data += n, length -= n;
  }
}

#define store mkgp2pc_mediaboard_store
#define load mkgp2pc_mediaboard_load

static uint32_t swap32(uint32_t v) { return (v << 24) | ((v << 8) & 0xFF0000) | ((v >> 8) & 0xFF00) | (v >> 24); }

int mkgp2pc_mediaboard_owns(uint32_t address) { return address >= 0x80000000u; }
int mkgp2pc_mediaboard_irq(void) { return (s_boards[0].pending ? 4 : 0) | (s_boards[1].pending ? 0x10 : 0); }

/* Answers go out one at a time: the game clears the answer mailbox when it
 * has taken one. Requests whose answer is not known yet (accept, connect)
 * wait in s_later and are asked about again on every retrace. */
#define MAX_WAITING 64
typedef struct {
  Board* board;
  Packet packet;
} Waiting;
static Waiting s_answers[MAX_WAITING], s_later[MAX_WAITING];
static int s_answerCount, s_laterCount;

static void deliver(void) {
  for (int i = 0; i < s_answerCount; i++) {
    Board* b = s_answers[i].board;
    if (!b->pending) {
      Packet out = s_answers[i].packet;
      memmove(&s_answers[i], &s_answers[i + 1], (size_t)(s_answerCount - i - 1) * sizeof(Waiting));
      s_answerCount--;
      store(b->answer, (const uint8_t*)&out, sizeof(out));
      b->pending = 1;
      mkgp2pc_baseboard_raise_exi(2);
      i--;
    }
  }
}

static void answer(Board* b, const Packet* in, int ok, uint32_t result) {
  if (s_answerCount < MAX_WAITING) {
    Waiting* w = &s_answers[s_answerCount++];
    w->board = b;
    w->packet = *in;
    w->packet.command = ok ? (uint16_t)(in->command | 0x80) : 0x0080;
    if (ok) {
      w->packet.result = result;
    }
  }
  deliver();
}

static void host_args(const Packet* in, uint32_t* arg) {
  for (int i = 0; i < 6; i++) {
    arg[i] = swap32(in->arg[i]); /* amsock.c stores them byte-swapped */
  }
}

static void request(Board* b, const Packet* in) {
  Packet out = *in;
  uint32_t arg[6];
  switch (in->command) {
  case 0x0100: /* amsys stores this as the board's address */
    out.result = swap32(b->address);
    break;
  case 0x0101: /* version: two 16-bit numbers, high byte first */
    memset(&out.result, 0, 4);
    memcpy(&out.result, b->version, 2);
    break;
  case 0x0301: /* serial number: 16 characters, amsys inserts the '-' at 4 */
    memcpy(&out.result, b->serial, 16);
    break;
  default:
    host_args(in, arg);
    switch (mkgp2pc_netlink_command(in->command, arg, &out.result)) {
    case MKGP2PC_NETLINK_DONE:
      break;
    case MKGP2PC_NETLINK_LATER:
      if (s_laterCount < MAX_WAITING) {
        s_later[s_laterCount].board = b;
        s_later[s_laterCount++].packet = *in;
        return;
      }
      /* fall through */
    default:
      answer(b, in, 0, 0);
      return;
    }
    break;
  }
  answer(b, &out, 1, out.result);
}

void mkgp2pc_mediaboard_tick(void) {
  mkgp2pc_netlink_tick();
  for (int i = 0; i < s_laterCount; i++) {
    Waiting w = s_later[i];
    uint32_t arg[6], result = 0;
    host_args(&w.packet, arg);
    if (mkgp2pc_netlink_poll(w.packet.command, arg, &result) != MKGP2PC_NETLINK_LATER) {
      memmove(&s_later[i], &s_later[i + 1], (size_t)(s_laterCount - i - 1) * sizeof(Waiting));
      s_laterCount--;
      i--;
      answer(w.board, &w.packet, 1, result);
    }
  }
  deliver();
}

void mkgp2pc_mediaboard_read(uint32_t address, void* buffer, uint32_t length) {
  if (address < 0x84000000u) { /* registers; the game reads them byte-swapped */
    memset(buffer, 0, length);
    if (address == 0x80000140u && length >= 4) {
      ((uint8_t*)buffer)[3] = 1; /* main firmware running */
    }
    return;
  }
  load(address, (uint8_t*)buffer, length);
}

static void board_write(uint32_t address, const void* buffer, uint32_t length) {
  if (address < 0x84000000u) {
    return;
  }
  for (int i = 0; i < 2; i++) {
    Board* b = &s_boards[i];
    if (address == b->request && length >= sizeof(Packet)) {
      Packet p;
      memcpy(&p, buffer, sizeof(p));
      if (p.command != 0) {
        request(b, &p); /* taken at once: the request mailbox always reads empty */
      }
      return;
    }
    if (address == b->answer) {
      b->pending = 0; /* the game cleared the answer */
      store(address, (const uint8_t*)buffer, length);
      deliver();
      return;
    }
  }
  store(address, (const uint8_t*)buffer, length);
}

/* The game writes from its main thread and from its network thread; the
 * retrace tick comes in between. One at a time, under the port's
 * "interrupt" lock. */
int OSDisableInterrupts(void);
int OSRestoreInterrupts(int level);

void mkgp2pc_mediaboard_write(uint32_t address, const void* buffer, uint32_t length) {
  int level = OSDisableInterrupts();
  board_write(address, buffer, length);
  OSRestoreInterrupts(level);
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
