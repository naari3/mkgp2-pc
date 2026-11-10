/*
 * Scripted input for the bring-up executable, so that it can be driven past
 * the attract screens without a window.
 *
 *   MKGP2PC_SCRIPT="4500 coin; 5000 an1=ffff; 5100 an1=0; 6000 sw=00,80,00; period 2000"
 *
 * Each entry is "<retrace> <command>"; "period N" repeats the whole script
 * every N retraces. Commands:
 *   coin          one coin in slot 1
 *   an<ch>=<hex>  analog channel (0 steering, 1 accelerator, 2 brake), 16 bits
 *   sw=<hex>,...  JVS switch bytes: system, player 1 (two bytes), player 2
 *   op=<hex>      baseboard operator switches (0x80 service, 0x40 test, 0x01 DIP)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mkgp2pc_jvs_set_switches(const uint8_t* bytes, int count);
void mkgp2pc_jvs_set_analog(int channel, uint16_t value);
void mkgp2pc_jvs_add_coin(int slot);
void mkgp2pc_baseboard_set_operator_switches(uint8_t pressed);

typedef struct {
  uint32_t at;
  char cmd[40];
} Entry;

static Entry s_entries[512];
static int s_count = -1;
static uint32_t s_period;

static void parse(void) {
  const char* e = getenv("MKGP2PC_SCRIPT");
  s_count = 0;
  while (e && *e && s_count < 512) {
    char item[64];
    size_t n = strcspn(e, ";");
    snprintf(item, sizeof(item), "%.*s", (int)(n < 63 ? n : 63), e);
    e += n + (e[n] == ';');
    unsigned at;
    char cmd[40];
    if (sscanf(item, " period %u", &at) == 1) {
      s_period = at;
    } else if (sscanf(item, " %u %39s", &at, cmd) == 2) {
      s_entries[s_count].at = at;
      strcpy(s_entries[s_count++].cmd, cmd);
    }
  }
}

static void run(const char* cmd) {
  unsigned a, b;
  if (strcmp(cmd, "coin") == 0) {
    mkgp2pc_jvs_add_coin(0);
  } else if (sscanf(cmd, "an%u=%x", &a, &b) == 2) {
    mkgp2pc_jvs_set_analog((int)a, (uint16_t)b);
  } else if (sscanf(cmd, "op=%x", &a) == 1) {
    mkgp2pc_baseboard_set_operator_switches((uint8_t)a);
  } else if (strncmp(cmd, "sw=", 3) == 0) {
    uint8_t bytes[5] = {0};
    int n = 0;
    for (const char* p = cmd + 3; n < 5 && *p;) {
      bytes[n++] = (uint8_t)strtoul(p, (char**)&p, 16);
      p += *p == ',';
    }
    mkgp2pc_jvs_set_switches(bytes, 5);
  } else {
    fprintf(stderr, "nogfx: unknown script command '%s'\n", cmd);
  }
}

void mkgp2pc_script_tick(uint32_t retrace) {
  if (s_count < 0) {
    parse();
  }
  if (s_count == 0) {
    return;
  }
  uint32_t t = s_period ? retrace % s_period : retrace;
  for (int i = 0; i < s_count; i++) {
    if (s_entries[i].at == t) {
      run(s_entries[i].cmd);
    }
  }
}
