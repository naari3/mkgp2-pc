/*
 * Triforce baseboard on the serial interface (SI channel 0) with one JVS I/O
 * board behind it, PC side.
 *
 * src/triforce/jvsio.c (compiled unchanged) builds a 0x80-byte request frame
 * and hands it to SITransfer; this file is the other end of that call. All
 * framing below was read off jvsio.c:
 *
 *   request   70 nn  { cmd len payload... }*  ... sum
 *   reply     01 end { cmd len payload... }*  ... with the 0x80 bytes summing to 0xFF
 *
 * A JVS exchange travels in a packet with cmd 0x40|node:
 *   request payload  E0 dest count data... sum      (D0 escapes D0 / E0)
 *   reply payload    E0 00   count 01 reports... sum
 *
 * The I/O board answers the standard JVS commands with neutral inputs (no
 * switch pressed, analog channels centred, no coin). Input from the host
 * (SDL) will be fed in through jvs_si_set_* once the windowed build links
 * this.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mkgp2pc/platform.h>
#include <mkgp2pc/triforce.h>

typedef void (*SICallbackMin)(int32_t chan, uint32_t err, void* context);

/* Host-side input state, neutral by default. */
static uint8_t s_switch[1 + 2 * 2]; /* system byte, then 2 bytes per player */
static uint16_t s_analog[8] = {0x8000, 0, 0, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000};
static uint16_t s_coins[2];
/* Baseboard switches, 1 = on: 0x80 service button, 0x40 test button, 0x01 a
 * DIP switch the game reads at boot (BootDispatcher.cpp: when it is on, the
 * card reader, printer and camera checks are skipped and the card service is
 * disabled). It is on only when the card unit is switched off
 * (MKGP2PC_CARD=0, card_reader.c); -1: not decided yet. */
static int s_operator = -1;
static uint8_t operator_switches(void) {
  if (s_operator < 0) {
    s_operator = mkgp2pc_card_enabled() ? 0 : 0x01;
  }
  return (uint8_t)s_operator;
}
static uint8_t s_address; /* JVS node address assigned by F1, 0 = none yet */

void mkgp2pc_jvs_set_switches(const uint8_t* bytes, int count) {
  memcpy(s_switch, bytes, count > (int)sizeof(s_switch) ? sizeof(s_switch) : (size_t)count);
}
void mkgp2pc_jvs_set_analog(int channel, uint16_t value) {
  if (channel >= 0 && channel < 8) {
    s_analog[channel] = value;
  }
}
void mkgp2pc_baseboard_set_operator_switches(uint8_t pressed) { s_operator = pressed; }
void mkgp2pc_jvs_add_coin(int slot) {
  if (slot >= 0 && slot < 2) {
    s_coins[slot]++;
  }
}

static const char kBoardId[] = "namco ltd.;FCA-1;Ver1.01;JPN,Multipurpose + Rotary Encoder";

/* Feature list: function code, three parameters; terminated by 0. */
static const uint8_t kFeatures[] = {
    0x01, 2, 12, 0, /* switches: 2 players, 12 buttons */
    0x02, 2, 0,  0, /* coin slots */
    0x03, 8, 16, 0, /* analog: 8 channels, 16 bits */
    0x12, 6, 0,  0, /* general purpose outputs */
    0x00,
};

/* Runs the JVS commands in `in` and appends the reports to `out`. */
static int jvs_reports(const uint8_t* in, int n, uint8_t* out) {
  int o = 0;
  int i = 0;
  while (i < n) {
    uint8_t c = in[i++];
    switch (c) {
    case 0xF0: /* reset: no report */
      i++;
      s_address = 0;
      return -1;
    case 0xF1: /* set address */
      s_address = in[i++];
      out[o++] = 1;
      break;
    case 0x10: /* board id */
      out[o++] = 1;
      memcpy(&out[o], kBoardId, sizeof(kBoardId));
      o += sizeof(kBoardId);
      break;
    case 0x11: /* command format revision */
      out[o++] = 1;
      out[o++] = 0x13;
      break;
    case 0x12: /* JVS revision */
      out[o++] = 1;
      out[o++] = 0x30;
      break;
    case 0x13: /* communication version */
      out[o++] = 1;
      out[o++] = 0x10;
      break;
    case 0x14: /* features */
      out[o++] = 1;
      memcpy(&out[o], kFeatures, sizeof(kFeatures));
      o += sizeof(kFeatures);
      break;
    case 0x15: /* main board id: skip the string */
      while (i < n && in[i++] != 0) {
      }
      out[o++] = 1;
      break;
    case 0x20: { /* switches: players, bytes per player */
      int players = in[i++];
      int bytes = in[i++];
      out[o++] = 1;
      out[o++] = s_switch[0];
      for (int p = 0; p < players; p++) {
        for (int b = 0; b < bytes; b++) {
          out[o++] = (p < 2 && b < 2) ? s_switch[1 + p * 2 + b] : 0;
        }
      }
      break;
    }
    case 0x21: { /* coins */
      int slots = in[i++];
      out[o++] = 1;
      for (int s = 0; s < slots; s++) {
        uint16_t v = s < 2 ? (s_coins[s] & 0x3FFF) : 0;
        out[o++] = v >> 8;
        out[o++] = v & 0xFF;
      }
      break;
    }
    case 0x22: { /* analog */
      int channels = in[i++];
      out[o++] = 1;
      for (int ch = 0; ch < channels; ch++) {
        uint16_t v = ch < 8 ? s_analog[ch] : 0;
        out[o++] = v >> 8;
        out[o++] = v & 0xFF;
      }
      break;
    }
    case 0x23: { /* rotary */
      int channels = in[i++];
      out[o++] = 1;
      memset(&out[o], 0, channels * 2);
      o += channels * 2;
      break;
    }
    case 0x24: /* keycode */
      out[o++] = 1;
      out[o++] = 0;
      break;
    case 0x25: /* screen position */
      i++;
      out[o++] = 1;
      memset(&out[o], 0, 4);
      o += 4;
      break;
    case 0x26: { /* misc switches */
      int bytes = in[i++];
      out[o++] = 1;
      memset(&out[o], 0, bytes);
      o += bytes;
      break;
    }
    case 0x2E: /* payout remaining */
      i++;
      out[o++] = 1;
      memset(&out[o], 0, 4);
      o += 4;
      break;
    case 0x30: case 0x31: case 0x35: case 0x36: /* coin / payout adjust: slot, 16-bit amount */
      if (c == 0x30 && in[i] >= 1 && in[i] <= 2) {
        s_coins[in[i] - 1] -= (uint16_t)((in[i + 1] << 8) | in[i + 2]);
      }
      i += 3;
      out[o++] = 1;
      break;
    case 0x32: case 0x34: /* output bytes: count, data */
      if (c == 0x32 && in[i] >= 1) {
        mkgp2pc_jvs_outputs(in[i + 1]);
      }
      i += 1 + in[i];
      out[o++] = 1;
      break;
    case 0x33: /* analog output: count, 16-bit data */
      i += 1 + in[i] * 2;
      out[o++] = 1;
      break;
    case 0x37: case 0x38: /* output byte / bit: two parameters */
      i += 2;
      out[o++] = 1;
      break;
    default: /* unknown command: report "unsupported" and stop */
      out[o++] = 2;
      return o;
    }
  }
  return o;
}

static int put_escaped(uint8_t* dst, uint8_t b) {
  if (b == 0xD0 || b == 0xE0) {
    dst[0] = 0xD0;
    dst[1] = b - 1;
    return 2;
  }
  dst[0] = b;
  return 1;
}

/* One 0x40|node packet: returns bytes written to `rx`. */
static int jvs_packet(uint8_t node, const uint8_t* payload, int len, uint8_t* rx) {
  uint8_t frame[0x100];
  uint8_t reports[0x100];
  int n = 0;
  if (len < 1 || payload[0] != 0xE0) {
    return 0;
  }
  for (int i = 1; i < len; i++) { /* undo D0 escapes */
    frame[n++] = payload[i] == 0xD0 && i + 1 < len ? (uint8_t)(payload[++i] + 1) : payload[i];
  }
  if (n < 3) {
    return 0;
  }
  uint8_t dest = frame[0];
  int count = frame[1]; /* bytes after the count, including the sum */
  if (dest != 0xFF && dest != s_address) {
    return 0;
  }
  int r = jvs_reports(&frame[2], count - 1, reports);
  if (r < 0) {
    return 0; /* reset: nothing comes back */
  }
  int o = 0;
  uint8_t sum = 0;
  rx[o++] = 0x40 | node;
  int lenAt = o++;
  rx[o++] = 0xE0;
  rx[o++] = 0x00; /* destination: the host */
  o += put_escaped(&rx[o], (uint8_t)(r + 2));
  sum += (uint8_t)(r + 2);
  rx[o++] = 0x01; /* status: normal */
  sum += 1;
  for (int i = 0; i < r; i++) {
    o += put_escaped(&rx[o], reports[i]);
    sum += reports[i];
  }
  o += put_escaped(&rx[o], sum);
  rx[lenAt] = (uint8_t)(o - lenAt - 1);
  return o;
}

/* The answer of a JVS node comes back one baseboard frame after its request:
 * the baseboard forwards the packet to the bus and hands over what it has
 * received by the next transfer. jvsio.c depends on it: it matches the
 * packets of a frame against the requests of the frame before (two request
 * banks, switched after each parse). */
static uint8_t s_pending[0x80];
static int s_pendingLen;

static void baseboard_frame(const uint8_t* tx, uint8_t* rx) {
  int end = tx[1] + 2;
  int o = 2;
  uint8_t answers[0x80];
  int answersLen = 0;
  memset(rx, 0, 0x80);
  rx[0] = 1;
  memcpy(&rx[o], s_pending, (size_t)s_pendingLen);
  o += s_pendingLen;
  s_pendingLen = 0;
  for (int i = 2; i + 1 < end && i < 0x7F;) {
    uint8_t c = tx[i];
    uint8_t len = tx[i + 1];
    const uint8_t* p = &tx[i + 2];
    i += 2 + len;
    if (o > 0x60) {
      break;
    }
    if ((c & 0xE0) == 0x40) {
      if (answersLen < 0x40) {
        answersLen += jvs_packet(c & 0x1F, p, len, &answers[answersLen]);
      }
      continue;
    }
    switch (c) {
    case 0x10: /* status pair. First byte: the baseboard's operator switches,
                * active low (the game inverts it; 0x80 is the service
                * button, 0x40 the test button). Second byte: bit 0x10 must be set for amsys init;
                * bit 0x01 is the JVS sense line (a node without an address is
                * on the bus), which amjvs init waits for and then clears by
                * assigning addresses. */
      rx[o++] = c, rx[o++] = 2, rx[o++] = (uint8_t)~operator_switches(), rx[o++] = (uint8_t)(0x10 | (s_address == 0));
      break;
    case 0x11: /* 16-byte serial / id */
      rx[o++] = c, rx[o++] = 16;
      memcpy(&rx[o], "AAAA000000000000", 16);
      o += 16;
      break;
    case 0x14: /* four baseboard analog inputs */
      rx[o++] = c, rx[o++] = 8;
      memset(&rx[o], 0, 8);
      o += 8;
      break;
    case 0x15: case 0x16: /* version / status words */
      rx[o++] = c, rx[o++] = 2, rx[o++] = 0, rx[o++] = 0;
      break;
    case 0x31: case 0x32: { /* serial port 0 / 1: bytes to send; received bytes come back */
      mkgp2pc_serial_write(c - 0x31, p, len);
      int got = mkgp2pc_serial_read(c - 0x31, &rx[o + 2], 16);
      if (got > 0) {
        rx[o] = c, rx[o + 1] = (uint8_t)got;
        o += 2 + got;
      }
      break;
    }
    default: /* settings: accepted, no reply */
      break;
    }
  }
  memcpy(s_pending, answers, (size_t)answersLen);
  s_pendingLen = answersLen;
  rx[1] = (uint8_t)o;
  uint8_t sum = 0;
  for (int i = 0; i < 0x7F; i++) {
    sum += rx[i];
  }
  rx[0x7F] = (uint8_t)(0xFF - sum);
}

int SITransfer(int32_t chan, void* out, uint32_t outBytes, void* in, uint32_t inBytes,
               SICallbackMin callback, int64_t delay) {
  const uint8_t* tx = (const uint8_t*)out;
  uint8_t rx[0x80];
  (void)delay;
  if (chan != 0) {
    if (callback) {
      callback(chan, 8 /* no response */, 0);
    }
    return 1;
  }
  memset(rx, 0, sizeof(rx));
  if (outBytes >= 2 && tx[0] == 0x70) {
    baseboard_frame(tx, rx);
    /* MKGP2PC_TRACE_JVS=n: print the first n frames that carry a JVS packet */
    static int trace = -1;
    if (trace < 0) {
      const char* e = getenv("MKGP2PC_TRACE_JVS");
      trace = e ? atoi(e) : 0;
    }
    if (trace > 0) {
      int jvs = 0;
      for (int i = 2; i + 1 < tx[1] + 2 && i < 0x7F; i += 2 + tx[i + 1]) {
        jvs |= (tx[i] & 0xE0) == 0x40 || getenv("MKGP2PC_TRACE_JVS_ALL") != NULL;
      }
      if (jvs) {
        trace--;
        fprintf(stderr, "jvs tx:");
        for (int i = 0; i < tx[1] + 2 && i < 0x80; i++) fprintf(stderr, " %02x", tx[i]);
        fprintf(stderr, "\njvs rx:");
        for (int i = 0; i < 0x40; i++) fprintf(stderr, " %02x", rx[i]);
        fprintf(stderr, "\n");
      }
    }
  } else if (outBytes == 1 && tx[0] == 0x00) {
    /* SI device type probe. amjvs init (fn_8028BFD4) requires first byte
     * 0x10 and the high nibble of the second byte to be 1. */
    rx[0] = 0x10;
    rx[1] = 0x10;
  }
  memcpy(in, rx, inBytes > sizeof(rx) ? sizeof(rx) : inBytes);
  if (callback) {
    callback(chan, 0, 0);
  }
  return 1;
}

int SIIsChanBusy(int32_t chan) {
  (void)chan;
  return 0;
}
