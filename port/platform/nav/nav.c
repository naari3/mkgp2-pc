/*
 * Going somewhere else than the game would by itself: the port's menu
 * (port/gfx/port_menu.cpp), its keys and the script reach the game through
 * the requests kept here. Two places in the game's sources ask, through
 * port/patches:
 *
 *   MainGameLoop          at the top of every frame: is another of the
 *                         top-level flows wanted (mkgp2pc_nav_flow_hook)
 *   MenuScene_Dispatch    after the current selection screen's update: is
 *                         the screen before it wanted (mkgp2pc_nav_scene_hook)
 *
 * No game symbol is used here; what the game has to set for a request is
 * done by the patched source with the values handed back.
 */
#include <mkgp2pc/nav.h>

#include <stdio.h>
#include <string.h>

/* ---- top-level flows -------------------------------------------------- */

static int s_flowWanted, s_flowPending;
static int s_flowEntry;
static int s_flowNow = -1;      /* the flow MainGameLoop is running */
static unsigned s_flowTicks;    /* counts the hook's calls */

void mkgp2pc_nav_request_flow(int flow, int entry) {
  s_flowWanted = flow;
  s_flowEntry = entry;
  s_flowPending = 1;
}

int mkgp2pc_nav_flow_pending(void) { return s_flowPending; }
int mkgp2pc_nav_current_flow(void) { return s_flowNow; }
unsigned mkgp2pc_nav_flow_ticks(void) { return s_flowTicks; }

static void scene_reset(void);
static int s_scene; /* the selection screen running now, 0: unknown */
static int s_menuLeft; /* the menu flow has given up its screen for a flow request */
static unsigned s_sceneTick = ~0u; /* s_flowTicks when the dispatcher last asked */

int mkgp2pc_nav_flow_hook(int current, int* entry) {
  int wanted;
  s_flowNow = current;
  s_flowTicks++;
  *entry = -1;
  if (!s_flowPending) {
    return -2;
  }
  /* The menu flow's dispatcher ran in the frame before (under whichever
   * index of the flow table: 3, or 8 from the developers' scene list): its
   * screen goes first, see mkgp2pc_nav_scene_hook. */
  if (s_sceneTick == s_flowTicks - 1 && !s_menuLeft) {
    return -2;
  }
  fprintf(stderr, "nav: flow %d -> %d (entry %d)\n", current, s_flowWanted, s_flowEntry);
  wanted = s_flowWanted;
  *entry = s_flowEntry;
  s_flowPending = 0;
  s_menuLeft = 0;
  scene_reset();
  s_flowNow = wanted;
  return wanted;
}

/* ---- selection screens: one step back ---------------------------------- */

/*
 * The selection screens between the coin and the race are children of one
 * flow ("kart2", MenuScene_Dispatch): each one's update returns -1 while it
 * stays and the number of the next screen when it is done; the dispatcher
 * deletes it and builds the next from that number. Every screen builds
 * itself from the choices made so far (they are globals), so going back is
 * the same operation with the number of the screen before: the numbers
 * passed are kept on a stack here.
 */
enum { STACK = 16 };
static int s_stack[STACK];
static int s_top;
static int s_backWanted;

static void scene_reset(void) {
  s_top = 0;
  s_scene = 0;
  s_backWanted = 0;
}

/* Screens one can go back from and back to. The others (entry, title,
 * attract, card, the "please wait" of a linked play, ranking) either have
 * nothing before them or belong to a step that must not be repeated. */
static int backable(int scene) {
  switch (scene) {
  case MKGP2PC_SCENE_CLASS:
  case MKGP2PC_SCENE_CHARACTER:
  case MKGP2PC_SCENE_KART:
  case MKGP2PC_SCENE_CUP:
  case MKGP2PC_SCENE_ROUND:
  case MKGP2PC_SCENE_COURSE:
  case MKGP2PC_SCENE_ITEM:
  case MKGP2PC_SCENE_GP_PLEASE:
  case MKGP2PC_SCENE_TA_PLEASE:
    return 1;
  default:
    return 0;
  }
}

/* The menu flow has built its first screen (its constructor says which). */
void mkgp2pc_nav_scene_begin(int scene) {
  scene_reset();
  s_menuLeft = 0;
  s_scene = scene;
}

void mkgp2pc_nav_request_back(void) { s_backWanted = 1; }
int mkgp2pc_nav_can_back(void) { return backable(s_scene) && s_top > 0; }
int mkgp2pc_nav_current_scene(void) { return s_scene; }

int mkgp2pc_nav_scene_hook(int next) {
  int back = s_backWanted;
  s_backWanted = 0;
  s_sceneTick = s_flowTicks;
  /*
   * Another flow is wanted while the menu flow runs. Its destructor does not
   * delete the screen it is showing (the game only ever leaves this flow
   * through the dispatcher, with the screen already gone) but does purge the
   * resources that screen draws with: the screen has to be deleted first.
   * 0 makes the dispatcher delete it and build none; the flow is changed at
   * the top of the next frame.
   */
  if (s_flowPending) {
    if (!s_menuLeft) {
      s_menuLeft = 1;
      scene_reset();
      return 0;
    }
    return -1;
  }
  if (next == -1) {
    if (back && mkgp2pc_nav_can_back()) {
      int to = s_stack[--s_top];
      fprintf(stderr, "nav: back, screen %d -> %d\n", s_scene, to);
      s_scene = to;
      return to;
    }
    return -1;
  }
  /* forward: screens that cannot be gone back to are passed over */
  fprintf(stderr, "nav: screen %d -> %d\n", s_scene, next);
  if (backable(s_scene)) {
    if (s_top == STACK) {
      memmove(s_stack, s_stack + 1, sizeof(s_stack) - sizeof(s_stack[0]));
      s_top--;
    }
    s_stack[s_top++] = s_scene;
  }
  if (next == MKGP2PC_SCENE_ENTRY || next == MKGP2PC_SCENE_DONE) {
    s_top = 0; /* a play has ended or the race begins */
  }
  s_scene = next;
  return next;
}
