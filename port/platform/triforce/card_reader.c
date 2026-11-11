/* Magnetic card unit ("CARD R/W": reader/writer, printer and dispenser in
 * one box) on the baseboard's serial port 1.
 *
 * Stand-in written from the game's side of the protocol
 * (src/game/Sci2Card*.c, card_send_track_data.c, card_eject.c; summary in
 * upstream's docs/wip/notes/card/card_feasibility.md). The unit is a Sanwa
 * CRP-1231LR-10NAB; that model name and the meaning of the command numbers
 * are public facts (the YACardEmu project lists them). No code was taken
 * from anywhere.
 *
 *   game   02 LEN CMD 00 00 00 data.. 03 BCC     LEN = 6 + data, BCC = XOR of LEN..03
 *   unit   06                                    (ACK)
 *   game   05                                    (ENQ, may be repeated)
 *   unit   02 LEN CMD R P S data.. 03 BCC
 *
 * R: where the card is ('0' none, '1' magnetic head, '2' print head,
 * '3' just dispensed, '4' ejected and not yet taken). P: error ('0' none,
 * '2' write error). S: job ('0' none, '2' unknown command, '4' waiting for
 * a card).
 *
 * The card is one file of 69 bytes (track 1; a longer file keeps its other
 * bytes), by default card.bin in the current directory, MKGP2PC_CARD_FILE
 * names another. Every job finishes at once, so the unit never answers
 * "running". The printed face of the card is not kept.
 *
 * Inserting: mkgp2pc_card_insert() (the I key, or "card" in an input
 * script) holds the card at the slot; the unit takes it in the next time
 * the game asks for a read, as the real one does. A new card comes from the
 * dispenser when the game asks for one; the first write then creates the
 * file, and a file already there is kept as card.bin.bak<n>.
 *
 * MKGP2PC_CARD=0 switches the unit off: the game is started with the DIP
 * switch that skips its card, printer and camera checks, as before this
 * file existed. MKGP2PC_TRACE_CARD=1 logs every command.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mkgp2pc/triforce.h>

void VBlankEnable_Set(unsigned char enable); /* src/game/VBlank.c */

enum { kTrack = 69, kFileMax = 3 * kTrack };

static uint8_t s_tx[300]; /* frame being received from the game */
static int s_txLen;
static uint8_t s_rx[600]; /* bytes waiting for the game */
static int s_rxLen;

static uint8_t s_lastCmd;
static uint8_t s_pos = '0', s_err = '0', s_job = '0';
static int s_cameraOff;
static int s_atSlot;      /* the player holds a card at the slot */
static int s_fresh;       /* the card in the unit came from the dispenser */
static int s_ejectPolls;  /* answers left before an ejected card counts as taken */
static uint8_t s_card[kFileMax];
static int s_cardLen;

static const char* card_path(void) {
  const char* p = getenv("MKGP2PC_CARD_FILE");
  return p != NULL && *p != 0 ? p : "card.bin";
}

int mkgp2pc_card_enabled(void) {
  const char* e = getenv("MKGP2PC_CARD");
  return e == NULL || strcmp(e, "0") != 0;
}

static int present(void) { return s_pos == '1' || s_pos == '2' || s_pos == '3'; }

static int file_load(void) {
  FILE* f = fopen(card_path(), "rb");
  if (f == NULL) {
    return 0;
  }
  uint8_t data[kFileMax];
  int n = (int)fread(data, 1, sizeof(data), f);
  fclose(f);
  if (n < kTrack) {
    fprintf(stderr, "card: %s is shorter than %d bytes, not used\n", card_path(), kTrack);
    return 0;
  }
  memcpy(s_card, data, (size_t)n);
  s_cardLen = n;
  return 1;
}

static void file_save(void) {
  const char* path = card_path();
  if (s_fresh) {
    FILE* old = fopen(path, "rb");
    if (old != NULL) {
      fclose(old);
      for (int i = 1; i < 1000; i++) {
        char bak[1024];
        snprintf(bak, sizeof(bak), "%s.bak%d", path, i);
        FILE* t = fopen(bak, "rb");
        if (t != NULL) {
          fclose(t);
          continue;
        }
        if (rename(path, bak) == 0) {
          fprintf(stderr, "card: the card already there was kept as %s\n", bak);
        }
        break;
      }
    }
    s_fresh = 0;
  }
  FILE* f = fopen(path, "wb");
  if (f == NULL || fwrite(s_card, 1, (size_t)s_cardLen, f) != (size_t)s_cardLen) {
    fprintf(stderr, "card: cannot write %s\n", path);
    s_err = '2';
  } else {
    fprintf(stderr, "card: written to %s\n", path);
  }
  if (f != NULL) {
    fclose(f);
  }
}

void mkgp2pc_card_insert(void) {
  if (present()) {
    fprintf(stderr, "card: there is a card in the unit already\n");
    return;
  }
  FILE* f = fopen(card_path(), "rb");
  if (f == NULL) {
    fprintf(stderr, "card: no card file %s (the game makes one when a new card is bought)\n", card_path());
    return;
  }
  fclose(f);
  s_atSlot = 1;
  fprintf(stderr, "card: %s held at the slot\n", card_path());
}

/* A read job takes the card at the slot in. */
static void take_in(void) {
  if (!present() && s_atSlot && s_pos == '0') {
    s_atSlot = 0;
    if (file_load()) {
      s_pos = '1';
      s_fresh = 0;
      fprintf(stderr, "card: taken in\n");
    }
  }
}

static void reply(void) {
  uint8_t data[kTrack];
  int n = 0;
  if (s_job == '4') {
    take_in();
    if (present()) {
      s_job = '0';
    }
  }
  if (s_lastCmd == 0x33 && present()) {
    memcpy(data, s_card, kTrack);
    n = kTrack;
  }
  if (s_rxLen + n + 8 > (int)sizeof(s_rx)) {
    return;
  }
  uint8_t* p = &s_rx[s_rxLen];
  p[0] = 0x02;
  p[1] = (uint8_t)(6 + n);
  p[2] = s_lastCmd;
  p[3] = s_pos;
  p[4] = s_err;
  p[5] = s_job;
  memcpy(&p[6], data, (size_t)n);
  p[6 + n] = 0x03;
  uint8_t sum = 0;
  for (int i = 1; i <= 6 + n; i++) {
    sum ^= p[i];
  }
  p[7 + n] = sum;
  s_rxLen += n + 8;
  if (s_pos == '4' && --s_ejectPolls <= 0) {
    s_pos = '0';
    fprintf(stderr, "card: ejected card taken out\n");
  }
}

static void command(uint8_t cmd, const uint8_t* data, int n) {
  s_lastCmd = cmd;
  s_err = '0';
  s_job = '0';
  switch (cmd) {
  case 0x10: /* initialise */
    /* The game sends this from its power-on checks, before the camera
     * check. There is no camera here (it is a network camera on the
     * cabinet), and the check would stop with an error screen until the
     * operator presses the test button and the game turns the camera
     * off. Do that last step now, with the game's own switch. */
    if (!s_cameraOff) {
      s_cameraOff = 1;
      VBlankEnable_Set(0);
    }
    break;
  case 0x20: /* status */
  case 0x40: /* cancel the job */
  case 0xA0: /* cleaning */
    break;
  case 0x33: /* read; takes a card in when there is none */
    take_in();
    if (present()) {
      s_pos = '1';
    } else {
      s_job = '4';
    }
    break;
  case 0x53: /* write: three option characters, then the track */
    if (!present() || n < 3 + kTrack) {
      s_err = '2';
      break;
    }
    memcpy(s_card, &data[3], kTrack);
    if (s_cardLen < kTrack) {
      s_cardLen = kTrack;
    }
    s_pos = '1';
    file_save();
    break;
  case 0x7A: /* register a font page */
    break;
  case 0x7C: /* print a line */
  case 0x7D: /* erase lines */
    if (present()) {
      s_pos = '2';
    }
    break;
  case 0x80: /* eject */
    if (present()) {
      s_pos = '4';
      s_ejectPolls = 3;
      fprintf(stderr, "card: ejected\n");
    }
    break;
  case 0xB0: /* a new card from the dispenser */
    if (!present()) {
      memset(s_card, 0, sizeof(s_card));
      s_cardLen = kTrack;
      s_fresh = 1;
      s_pos = '3';
      fprintf(stderr, "card: new card dispensed\n");
    }
    break;
  default:
    fprintf(stderr, "card: unknown command %02x\n", cmd);
    s_job = '2';
    break;
  }
  if (getenv("MKGP2PC_TRACE_CARD") != NULL) {
    fprintf(stderr, "card: cmd %02x len %d -> %c%c%c\n", cmd, n, s_pos, s_err, s_job);
  }
}

void mkgp2pc_card_serial_write(const uint8_t* bytes, int n) {
  for (int i = 0; i < n; i++) {
    uint8_t b = bytes[i];
    if (s_txLen == 0) {
      if (b == 0x05) {
        reply();
      } else if (b == 0x02) {
        s_tx[s_txLen++] = b;
      }
      continue;
    }
    s_tx[s_txLen++] = b;
    if (s_txLen >= 2 && (s_tx[1] < 6 || s_txLen == s_tx[1] + 2)) {
      int len = s_tx[1];
      uint8_t sum = 0;
      for (int k = 1; k <= len; k++) {
        sum ^= s_tx[k];
      }
      s_txLen = 0;
      if (len < 6 || sum != s_tx[len + 1] || s_rxLen >= (int)sizeof(s_rx)) {
        continue; /* no ACK: the game sends the frame again */
      }
      s_rx[s_rxLen++] = 0x06;
      command(s_tx[2], &s_tx[6], len - 6);
    }
  }
}

int mkgp2pc_card_serial_read(uint8_t* out, int max) {
  int n = s_rxLen < max ? s_rxLen : max;
  memcpy(out, s_rx, (size_t)n);
  memmove(s_rx, &s_rx[n], (size_t)(s_rxLen - n));
  s_rxLen -= n;
  return n;
}
