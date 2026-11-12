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

/* ---- Network versus on demand ------------------------------------------
 *
 * The arcade program keeps the link up from start-up and decides at the coin
 * whether the entry screen (clFlowEntry, scene 7) is shown: only when another
 * cabinet is connected. The port keeps the link switched off instead and
 * asks the player after the coin. "Network versus" switches the link on,
 * takes a free cabinet number, restarts the game's link objects and enters
 * the entry screen, which then stays open until somebody presses the button
 * (see mkgp2pc_linkflow_lobby_timer). Back in the attract loop the link is
 * switched off again.
 *
 * This file holds the decisions only; what touches game data is in the
 * patches (fn_801B76C8.cpp, fn_801B785C.cpp, fn_801BA1A4.cpp, fn_801BA318.cpp,
 * PCBCheckRecv.c, GameBoot_Init.cpp). MKGP2PC_LINK_MODE=arcade gives the
 * arcade behaviour: link always on with the cabinet number of MKGP2PC_PCB_ID,
 * no question. MKGP2PC_LINK=0: no link and no question. */
#include <string.h>
#include <stdio.h>

#include "mkgp2pc/gameui.h"

void mkgp2pc_netlink_set_active(int on);
unsigned mkgp2pc_netlink_heard_mask(void);
int mkgp2pc_netlink_address_ok(void);

static int s_onDemand = -1;
static int s_choosing; /* the question is on the screen */
static int s_age;      /* frames the question has been up */
static int s_drawn;    /* its sprites and text are loaded */
static int s_armed;    /* the pedal has been let go since the question came */
static int s_up;       /* frames in a row the pedal has been up */
#define ARM_FRAMES 3
#define CHOICE_GUARD 15 /* frames the window takes to open */
static int s_cursor;   /* 0 alone, 1 network */
static int s_network;  /* this game was started as network versus */
static int s_go;       /* somebody closed the entry screen */
static int s_counting; /* the entry screen's timer has been seen running */
static int s_frames;   /* frames of the entry screen so far */
static float s_timer;  /* the entry screen's timer as last seen */
static int s_lobbyDrawn;

/* MKGP2PC_LINK_MODE, set before the start (the launcher's 通信 page):
 *   solo     no question, a coin goes on alone (also when not set)
 *   network  no question, a coin goes to the network entry screen
 *   ask      the question after the coin
 *   arcade   the cabinet's behaviour: link always on, MKGP2PC_PCB_ID
 * MKGP2PC_LINK=0 (no link at all) plays alone whatever the mode says. */
static int s_preset; /* 0 ask, 1 alone, 2 network */

int mkgp2pc_linkflow_on_demand(void) {
  if (s_onDemand < 0) {
    const char* mode = getenv("MKGP2PC_LINK_MODE");
    const char* link = getenv("MKGP2PC_LINK");
    s_onDemand = 1;
    s_preset = 1;
    if (mode != NULL && strcmp(mode, "arcade") == 0) {
      s_onDemand = 0;
    } else if (mode != NULL && strcmp(mode, "network") == 0) {
      s_preset = 2;
    } else if (mode != NULL && strcmp(mode, "ask") == 0) {
      s_preset = 0;
    } else if (mode != NULL && mode[0] != 0 && strcmp(mode, "solo") != 0) {
      fprintf(stderr, "link: MKGP2PC_LINK_MODE=%s is not solo, network, ask or arcade; taken as solo\n", mode);
    }
    if (link != NULL && link[0] == '0') {
      s_onDemand = 0;
    }
  }
  return s_onDemand;
}

/* The coin was confirmed with the link on demand: 1 on alone, 2 network
 * versus, 0 put the question up. */
int mkgp2pc_linkflow_preset(void) {
  mkgp2pc_linkflow_on_demand();
  return s_preset;
}

/* GameBoot_Init, before the game gives the board its address. */
void mkgp2pc_linkflow_boot(void) {
  if (mkgp2pc_linkflow_on_demand()) {
    mkgp2pc_netlink_set_active(0);
  }
}

/* The attract loop starts (clFlowTitle): a network game, if any, is over. */
void mkgp2pc_linkflow_idle(void) {
  s_choosing = 0;
  s_network = 0;
  s_go = 0;
  if (mkgp2pc_linkflow_on_demand()) {
    mkgp2pc_netlink_set_active(0);
  }
}

int mkgp2pc_linkflow_network(void) { return s_network; }
int mkgp2pc_linkflow_choosing(void) { return s_choosing; }
int mkgp2pc_linkflow_cursor(void) { return s_cursor; }

/* The coin was confirmed: put the question up. */
void mkgp2pc_linkflow_ask(void) {
  s_choosing = 1;
  s_cursor = 0;
  s_age = 0;
  s_armed = 0;
  s_up = 0;
}

/* clFlowTitle::Draw, every frame: the question, drawn with the game's own
 * text and sprites (port/platform/ui/gameui.c). */
#define CHOICE_LAYER 7
void mkgp2pc_linkflow_draw(void) {
  static const char* const ja[] = {"ひとりで遊ぶ", "通信対戦"};
  static const char* const en[] = {"1 PLAYER", "NETWORK VS"};
  if (!s_choosing) {
    if (s_drawn) {
      mkgp2pc_gameui_end();
      s_drawn = 0;
    }
    return;
  }
  if (!s_drawn) {
    mkgp2pc_gameui_begin(CHOICE_LAYER);
    s_drawn = 1;
  }
  if (mkgp2pc_gameui_english()) {
    mkgp2pc_gameui_choice("SELECT GAME", en, 2, s_cursor, s_age);
  } else {
    mkgp2pc_gameui_choice("遊び方を選んでください", ja, 2, s_cursor, s_age);
  }
  s_age++;
}

/* One frame of the question. Returns 0 while open, 1 for a game alone, 2 for
 * network versus. MKGP2PC_NET_CHOICE=1 or 2 answers at once (for scripted
 * runs). */
int mkgp2pc_linkflow_choose(int left, int right, int decide) {
  const char* e = getenv("MKGP2PC_NET_CHOICE");
  int scripted = e != NULL && (e[0] == '1' || e[0] == '2');
  if (scripted) {
    s_cursor = e[0] - '1';
    decide = 1;
  }
  if (left) {
    s_cursor = 0;
  }
  if (right) {
    s_cursor = 1;
  }
  /* The pedal that confirmed the coin is usually still down when the
   * question comes up, and `decide` is its state, not the moment it goes
   * down (the game's InputPlayer::IsPressed says "is down"). The answer has
   * to be a press made after the pedal was let go, with the window open. */
  s_up = decide || s_age < CHOICE_GUARD ? 0 : s_up + 1;
  if (s_up >= ARM_FRAMES) {
    s_armed = 1;
  }
  if (!scripted && (!decide || !s_armed)) {
    return 0;
  }
  s_choosing = 0;
  if (s_drawn) {
    mkgp2pc_gameui_end();
    s_drawn = 0;
  }
  return s_cursor + 1;
}

/* Network versus was chosen: link on, and the cabinet number to take: the
 * lowest one not heard, starting after `taken` (0 for the first try; the
 * number just tried when mkgp2pc_netlink_address_ok() said no). 0: all four
 * are in use. */
int mkgp2pc_linkflow_begin(int taken) {
  unsigned heard;
  int id;
  mkgp2pc_netlink_set_active(1);
  heard = mkgp2pc_netlink_heard_mask();
  for (id = taken + 1; id <= 4; id++) {
    if ((heard & (1u << (id - 1))) == 0) {
      s_network = 1;
      s_go = 0;
      s_counting = 0;
      s_frames = 0;
      fprintf(stderr, "link: network versus, this copy is cabinet %d\n", id);
      return id;
    }
  }
  s_network = 0;
  mkgp2pc_netlink_set_active(0);
  fprintf(stderr, "link: network versus: four cabinets are already there\n");
  return 0;
}

/* PCBComm_UpdateSyncPhase1, on the cabinet that counts the entry screen's
 * shared timer down (15 s on the cabinet). In a network game the timer stays
 * at 10 s until a player in the entry screen presses the button (the arcade
 * program's "hurry" flag, `pressed`), then runs out from 3 s. */
float mkgp2pc_linkflow_lobby_timer(float timer, int pressed) {
  if (!s_network) {
    return timer;
  }
  if (pressed) {
    s_go = 1;
  }
  if (!s_go) {
    return timer < 10.0f ? 10.0f : timer;
  }
  return timer > 3.0f ? 3.0f : timer;
}

/* fn_801BA1A4 (entry screen), every frame, with this cabinet's value of the
 * shared timer: has it run out? Right after the link objects were started
 * again the timer table is empty (0) until the first exchange; in a network
 * game that does not count as run out. */
int mkgp2pc_linkflow_lobby_over(float timer) {
  if (!s_network) {
    return timer <= 0.0f;
  }
  s_frames++;
  s_timer = timer;
  if (timer > 0.0f) {
    s_counting = 1;
  }
  return s_counting && timer <= 0.0f;
}

/* The entry screen's button. The press that answered the question may still
 * be down when the entry screen starts and would close it at once, so the
 * first second and a half of a network game's entry screen does not take it. */
int mkgp2pc_linkflow_lobby_button(void) { return !s_network || s_frames > 90; }

/* PCBComm_UpdateSyncPhase1: no cabinet is in the entry screen. The next
 * entry screen starts held again, also on a cabinet that is racing and only
 * counts the timer for the others. */
void mkgp2pc_linkflow_lobby_empty(void) { s_go = 0; }

/* clFlowEntry::Draw, every frame. The entry screen of a network game stays
 * open until somebody presses the button; the hint for that, in the game's
 * own text, under the game's "please wait". It goes away once the timer is
 * running out (below the 10 s it is held at). */
void mkgp2pc_linkflow_lobby_draw(void) {
  if (!s_network) {
    return;
  }
  if (!s_lobbyDrawn) {
    mkgp2pc_gameui_begin(CHOICE_LAYER);
    s_lobbyDrawn = 1;
  }
  if (!s_counting || s_timer >= 10.0f) {
    mkgp2pc_gameui_hint_button(320.0f, 436.0f, s_frames, mkgp2pc_gameui_english() ? "START" : "スタート");
  }
}

/* clFlowEntry's destructor. */
void mkgp2pc_linkflow_lobby_closed(void) {
  if (s_lobbyDrawn) {
    mkgp2pc_gameui_end();
    s_lobbyDrawn = 0;
  }
}
