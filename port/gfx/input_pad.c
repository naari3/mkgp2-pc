/*
 * Game controllers and racing wheels as the cabinet's controls
 * (docs/port/controller.md).
 *
 * The cabinet's wheel and pedals are potentiometers read through the JVS
 * board's analog channels; the keyboard can only give them all or nothing.
 * A controller's axes give the positions in between: this file reads one
 * SDL joystick each frame and hands video_aurora.c the wheel position
 * (-1 .. 1) and the two pedals (0 .. 1), plus button presses as the same
 * actions the keys have.
 *
 *   MKGP2PC_PAD=off                      no controller
 *   MKGP2PC_PAD="device=G29;steer=a0;accel=a2~;brake=a3~;item=b0,b4;range=30"
 *
 * Entries are separated by ';'. What is not named keeps its default.
 *   device=<text>   the controller whose name contains the text (letter case
 *                   ignored), or #<n> for the n-th one listed in the log.
 *                   Default: the first wheel, else the first gamepad, else
 *                   the first joystick.
 *   steer=<axis>    the wheel
 *   <action>=<source>[,<source>...]   an action of MKGP2PC_KEYS (accel,
 *                   brake, item, cancel, coin, left, right, ...). For accel
 *                   and brake an axis gives the pedal's position; anywhere
 *                   else an axis counts as pressed beyond half its travel.
 *                   Nothing after '=' leaves the action without a source.
 *   deadzone=<n>    percent of the steering axis around its centre that
 *                   counts as centre (default 10, wheels 0)
 *   range=<n>       percent of the steering axis' travel that is full lock
 *                   (default 100; a 900 degree wheel set to 30 turns like
 *                   the cabinet's 270 degree one)
 * A source is
 *   a<n>            joystick axis n, its whole travel: lowest value =
 *                   released (pedal) or left (wheel)
 *   a<n>~           the same, the other way round
 *   a<n>+  a<n>-    only the upper / lower half of the axis from its centre
 *                   (two pedals sharing one axis, a stick pushed forward)
 *   b<n>            joystick button n
 *   h<n>u h<n>d h<n>l h<n>r   a direction of hat n
 *   leftx lefty rightx righty lefttrigger righttrigger   (with ~ + - too)
 *   a b x y back start guide leftshoulder rightshoulder leftstick rightstick
 *   dpup dpdown dpleft dpright
 *                   the named axis or button of a controller SDL knows the
 *                   layout of
 * Numbers are SDL's; the log lists every controller with its counts at
 * start, and MKGP2PC_PAD_LOG=1 prints each axis and button as it changes.
 *
 * Until the controller has been touched (a button, or an axis moved by more
 * than a seventh of its travel) it gives nothing, so that a script
 * (MKGP2PC_SCRIPT) is not disturbed by one that merely lies there.
 *
 * MKGP2PC_PAD_TEST=wheel|gamepad adds a controller made of nothing (an SDL
 * virtual joystick: wheel = 4 axes, 12 buttons, 1 hat) that the script
 * commands vax<n>=<value>, vbt<n>=<0|1> and vhat<n>=<bits> move: the only
 * way to try this without hardware.
 */
#include <SDL3/SDL.h>

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SRC_NONE, SRC_AXIS, SRC_BUTTON, SRC_HAT, SRC_GP_AXIS, SRC_GP_BUTTON };
enum { AX_WHOLE, AX_REVERSED, AX_UPPER, AX_LOWER };
typedef struct {
  unsigned char kind;
  unsigned char mode; /* axes: AX_*; hats: SDL_HAT_* bit */
  short index;
} Source;

#define MAX_ACTIONS 40
#define SOURCES 4
enum { CTL_STEER = MAX_ACTIONS, CONTROLS };

static Source s_map[CONTROLS][SOURCES];
static bool s_given[CONTROLS]; /* named in MKGP2PC_PAD: no default */
static bool s_held[MAX_ACTIONS];
static const char* const* s_names;
static int s_actions, s_accel = -1, s_brake = -1;
static void (*s_fire)(int action, bool down);

static bool s_off, s_log, s_ready, s_used;
static char s_wanted[128]; /* device= */
static int s_deadzone = -1, s_range = 100;
static SDL_Joystick* s_joy;
static SDL_Gamepad* s_gamepad;
static SDL_JoystickID s_id;
static SDL_Joystick* s_virtual;
static float s_rest[3]; /* steer, accel, brake when the controller was opened */
static bool s_restKnown;

static int find_action(const char* name, size_t len) {
  if (len == 5 && strncmp(name, "steer", 5) == 0) {
    return CTL_STEER;
  }
  for (int i = 0; i < s_actions; i++) {
    if (strlen(s_names[i]) == len && strncmp(s_names[i], name, len) == 0) {
      return i;
    }
  }
  return -1;
}

static bool parse_source(const char* text, Source* out) {
  char name[32];
  size_t len = strlen(text);
  int mode = AX_WHOLE;
  char* stop;
  memset(out, 0, sizeof(*out));
  if (len == 0 || len >= sizeof(name)) {
    return false;
  }
  strcpy(name, text);
  if (name[0] == 'h' && isdigit((unsigned char)name[1])) {
    long n = strtol(name + 1, &stop, 10);
    int bit = *stop == 'u' ? SDL_HAT_UP : *stop == 'd' ? SDL_HAT_DOWN : *stop == 'l' ? SDL_HAT_LEFT
              : *stop == 'r' ? SDL_HAT_RIGHT : 0;
    if (bit == 0 || stop[1] != 0 || n > 255) {
      return false;
    }
    out->kind = SRC_HAT;
    out->mode = (unsigned char)bit;
    out->index = (short)n;
    return true;
  }
  if (name[0] == 'b' && isdigit((unsigned char)name[1])) {
    long n = strtol(name + 1, &stop, 10);
    if (*stop != 0 || n > 1023) {
      return false;
    }
    out->kind = SRC_BUTTON;
    out->index = (short)n;
    return true;
  }
  if (len > 1 && (name[len - 1] == '~' || name[len - 1] == '+' || name[len - 1] == '-')) {
    mode = name[len - 1] == '~' ? AX_REVERSED : name[len - 1] == '+' ? AX_UPPER : AX_LOWER;
    name[--len] = 0;
  }
  if (name[0] == 'a' && isdigit((unsigned char)name[1])) {
    long n = strtol(name + 1, &stop, 10);
    if (*stop != 0 || n > 1023) {
      return false;
    }
    out->kind = SRC_AXIS;
    out->mode = (unsigned char)mode;
    out->index = (short)n;
    return true;
  }
  SDL_GamepadAxis axis = SDL_GetGamepadAxisFromString(name);
  if (axis != SDL_GAMEPAD_AXIS_INVALID) {
    out->kind = SRC_GP_AXIS;
    out->mode = (unsigned char)mode;
    out->index = (short)axis;
    return true;
  }
  SDL_GamepadButton button = mode == AX_WHOLE ? SDL_GetGamepadButtonFromString(name) : SDL_GAMEPAD_BUTTON_INVALID;
  if (button != SDL_GAMEPAD_BUTTON_INVALID) {
    out->kind = SRC_GP_BUTTON;
    out->index = (short)button;
    return true;
  }
  return false;
}

static void set_sources(int control, const char* list) {
  int n = 0;
  memset(s_map[control], 0, sizeof(s_map[control]));
  while (*list != 0) {
    char one[32];
    size_t len = strcspn(list, ",");
    Source src;
    snprintf(one, sizeof(one), "%.*s", (int)len, list);
    if (!parse_source(one, &src)) {
      if (len != 0) fprintf(stderr, "MKGP2PC_PAD: unknown source '%s'\n", one);
    } else if (n < SOURCES) {
      s_map[control][n++] = src;
    }
    list += len;
    if (*list == ',') list++;
  }
}

static void parse_settings(void) {
  const char* p = getenv("MKGP2PC_PAD");
  if (p != NULL && (strcmp(p, "off") == 0 || strcmp(p, "0") == 0)) {
    s_off = true;
    return;
  }
  while (p != NULL && *p != 0) {
    char item[160];
    size_t len = strcspn(p, ";");
    snprintf(item, sizeof(item), "%.*s", (int)len, p);
    p += len;
    if (*p == ';') p++;
    char* eq = strchr(item, '=');
    if (eq == NULL) {
      if (item[0] != 0) fprintf(stderr, "MKGP2PC_PAD: unknown entry '%s'\n", item);
      continue;
    }
    *eq++ = 0;
    if (strcmp(item, "device") == 0) {
      snprintf(s_wanted, sizeof(s_wanted), "%s", eq);
    } else if (strcmp(item, "deadzone") == 0) {
      s_deadzone = atoi(eq) < 0 ? 0 : atoi(eq) > 90 ? 90 : atoi(eq);
    } else if (strcmp(item, "range") == 0) {
      s_range = atoi(eq) < 1 ? 1 : atoi(eq) > 100 ? 100 : atoi(eq);
    } else {
      int control = find_action(item, strlen(item));
      if (control < 0) {
        fprintf(stderr, "MKGP2PC_PAD: unknown entry '%s'\n", item);
      } else {
        s_given[control] = true;
        set_sources(control, eq);
      }
    }
  }
}

static void set_default(const char* action, const char* list) {
  int control = find_action(action, strlen(action));
  if (control >= 0 && !s_given[control]) {
    set_sources(control, list);
  }
}

/* What a controller does when MKGP2PC_PAD does not say. */
static void default_map(bool gamepad) {
  for (int i = 0; i < CONTROLS; i++) {
    if (!s_given[i]) {
      memset(s_map[i], 0, sizeof(s_map[i]));
    }
  }
  if (gamepad) {
    set_default("steer", "leftx");
    set_default("accel", "righttrigger");
    set_default("brake", "lefttrigger");
    set_default("item", "a,start");
    set_default("cancel", "b");
    set_default("coin", "back");
    set_default("card", "y");
    set_default("left", "dpleft");
    set_default("right", "dpright");
  } else {
    /* a plain joystick: the stick, forward to go; wheels differ too much to guess */
    set_default("steer", "a0");
    set_default("accel", "a1-");
    set_default("brake", "a1+");
    set_default("item", "b0");
    set_default("cancel", "b1");
    set_default("coin", "b2");
    set_default("card", "b3");
  }
}

static bool name_has(const char* name, const char* part) {
  size_t n = strlen(part);
  for (; name != NULL && *name != 0; name++) {
    if (SDL_strncasecmp(name, part, n) == 0) {
      return true;
    }
  }
  return false;
}

static void close_device(void) {
  for (int i = 0; i < s_actions; i++) {
    if (s_held[i]) {
      s_held[i] = false;
      s_fire(i, false);
    }
  }
  if (s_gamepad != NULL) SDL_CloseGamepad(s_gamepad);
  if (s_joy != NULL) SDL_CloseJoystick(s_joy);
  s_gamepad = NULL;
  s_joy = NULL;
  s_id = 0;
  s_restKnown = false;
}

static void open_device(bool list) {
  int count = 0;
  SDL_JoystickID* ids = SDL_GetJoysticks(&count);
  int pick = -1, rank = -1;
  for (int i = 0; ids != NULL && i < count; i++) {
    const char* name = SDL_GetJoystickNameForID(ids[i]);
    bool wheel = SDL_GetJoystickTypeForID(ids[i]) == SDL_JOYSTICK_TYPE_WHEEL;
    bool pad = SDL_IsGamepad(ids[i]);
    int r = wheel ? 2 : pad ? 1 : 0;
    if (list) {
      fprintf(stderr, "pad: #%d '%s'%s\n", i + 1, name != NULL ? name : "?", wheel ? " (wheel)" : pad ? " (gamepad)" : "");
    }
    if (s_wanted[0] == '#') {
      r = atoi(s_wanted + 1) == i + 1 ? 3 : -1;
    } else if (s_wanted[0] != 0) {
      r = name_has(name, s_wanted) ? 3 : -1;
    }
    if (r > rank) {
      rank = r;
      pick = i;
    }
  }
  if (pick >= 0) {
    s_joy = SDL_OpenJoystick(ids[pick]);
    if (s_joy == NULL) {
      fprintf(stderr, "pad: cannot open '%s': %s\n", SDL_GetJoystickNameForID(ids[pick]), SDL_GetError());
    } else {
      bool wheel = SDL_GetJoystickType(s_joy) == SDL_JOYSTICK_TYPE_WHEEL;
      s_id = ids[pick];
      s_gamepad = SDL_IsGamepad(s_id) ? SDL_OpenGamepad(s_id) : NULL;
      default_map(s_gamepad != NULL);
      fprintf(stderr, "pad: using #%d '%s'%s: %d axes, %d buttons, %d hats; deadzone %d%%, range %d%%\n", pick + 1,
              SDL_GetJoystickName(s_joy), s_gamepad != NULL ? " (layout known)" : "", SDL_GetNumJoystickAxes(s_joy),
              SDL_GetNumJoystickButtons(s_joy), SDL_GetNumJoystickHats(s_joy),
              s_deadzone >= 0 ? s_deadzone : wheel ? 0 : 10, s_range);
    }
  } else if (list && s_wanted[0] != 0) {
    fprintf(stderr, "pad: no controller matches '%s'\n", s_wanted);
  }
  SDL_free(ids);
}

/* -1 .. 1 */
static float axis_value(const Source* src) {
  int v;
  if (src->kind == SRC_AXIS) {
    if (s_joy == NULL || src->index >= SDL_GetNumJoystickAxes(s_joy)) {
      return 0.0f;
    }
    v = SDL_GetJoystickAxis(s_joy, src->index);
  } else {
    if (s_gamepad == NULL) {
      return 0.0f;
    }
    v = SDL_GetGamepadAxis(s_gamepad, (SDL_GamepadAxis)src->index);
    if (src->index == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || src->index == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
      v = v * 2 - 32767; /* triggers rest at 0 */
    }
  }
  return v < 0 ? (float)v / 32768.0f : (float)v / 32767.0f;
}

/* 0 .. 1: how far the source is pressed */
static float pressed(const Source* src) {
  float v;
  switch (src->kind) {
  case SRC_BUTTON:
    return s_joy != NULL && src->index < SDL_GetNumJoystickButtons(s_joy) && SDL_GetJoystickButton(s_joy, src->index) ? 1.0f
                                                                                                                         : 0.0f;
  case SRC_HAT:
    return s_joy != NULL && src->index < SDL_GetNumJoystickHats(s_joy) && (SDL_GetJoystickHat(s_joy, src->index) & src->mode) != 0
               ? 1.0f
               : 0.0f;
  case SRC_GP_BUTTON:
    return s_gamepad != NULL && SDL_GetGamepadButton(s_gamepad, (SDL_GamepadButton)src->index) ? 1.0f : 0.0f;
  case SRC_AXIS:
  case SRC_GP_AXIS:
    v = axis_value(src);
    switch (src->mode) {
    case AX_REVERSED: return (1.0f - v) * 0.5f;
    case AX_UPPER: return v > 0.0f ? v : 0.0f;
    case AX_LOWER: return v < 0.0f ? -v : 0.0f;
    default: return (v + 1.0f) * 0.5f;
    }
  default:
    return 0.0f;
  }
}

static bool is_axis(const Source* src) { return src->kind == SRC_AXIS || src->kind == SRC_GP_AXIS; }

static float steer_now(void) {
  const Source* src = &s_map[CTL_STEER][0];
  int dead = s_deadzone >= 0 ? s_deadzone : s_joy != NULL && SDL_GetJoystickType(s_joy) == SDL_JOYSTICK_TYPE_WHEEL ? 0 : 10;
  float v, a, d = (float)dead / 100.0f;
  if (!is_axis(src)) {
    return 0.0f;
  }
  v = axis_value(src);
  if (src->mode == AX_REVERSED) {
    v = -v;
  }
  a = v < 0.0f ? -v : v;
  a = a <= d ? 0.0f : (a - d) / (1.0f - d);
  a = a * 100.0f / (float)s_range;
  if (a > 1.0f) {
    a = 1.0f;
  }
  return v < 0.0f ? -a : a;
}

static float pedal_now(int action) {
  float best = 0.0f;
  for (int i = 0; action >= 0 && i < SOURCES; i++) {
    if (is_axis(&s_map[action][i])) {
      float v = pressed(&s_map[action][i]);
      best = v > best ? v : best;
    }
  }
  return best;
}

static void log_changes(void) {
  static short axes[16];
  static bool buttons[64];
  static unsigned char hats[4];
  static SDL_JoystickID seen;
  bool first = seen != s_id;
  seen = s_id;
  for (int i = 0; i < 16 && i < SDL_GetNumJoystickAxes(s_joy); i++) {
    int v = SDL_GetJoystickAxis(s_joy, i);
    if (first || abs(v - axes[i]) > 3000) {
      axes[i] = (short)v;
      fprintf(stderr, "pad: a%d = %d\n", i, v);
    }
  }
  for (int i = 0; i < 64 && i < SDL_GetNumJoystickButtons(s_joy); i++) {
    bool v = SDL_GetJoystickButton(s_joy, i);
    if (v != buttons[i]) {
      buttons[i] = v;
      fprintf(stderr, "pad: b%d %s\n", i, v ? "down" : "up");
    }
  }
  for (int i = 0; i < 4 && i < SDL_GetNumJoystickHats(s_joy); i++) {
    unsigned char v = SDL_GetJoystickHat(s_joy, i);
    if (v != hats[i]) {
      hats[i] = v;
      fprintf(stderr, "pad: h%d =%s%s%s%s%s\n", i, v == 0 ? " centre" : "", v & SDL_HAT_UP ? " u" : "",
              v & SDL_HAT_DOWN ? " d" : "", v & SDL_HAT_LEFT ? " l" : "", v & SDL_HAT_RIGHT ? " r" : "");
    }
  }
}

static void make_virtual(const char* kind) {
  SDL_VirtualJoystickDesc desc;
  SDL_INIT_INTERFACE(&desc);
  if (strcmp(kind, "gamepad") == 0) {
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = 15;
    desc.name = "mkgp2pc test gamepad";
  } else {
    desc.type = SDL_JOYSTICK_TYPE_WHEEL;
    desc.naxes = 4;
    desc.nbuttons = 12;
    desc.nhats = 1;
    desc.name = "mkgp2pc test wheel";
  }
  SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
  s_virtual = id != 0 ? SDL_OpenJoystick(id) : NULL;
  if (s_virtual == NULL) {
    fprintf(stderr, "pad: no test controller: %s\n", SDL_GetError());
  }
}

/* vax<n>=<value>, vbt<n>=<0|1>, vhat<n>=<bits>: 1 if the command was one of these */
int mkgp2pc_pad_script(const char* cmd) {
  unsigned n;
  int v;
  if (sscanf(cmd, "vax%u=%d", &n, &v) == 2) {
    if (s_virtual != NULL) SDL_SetJoystickVirtualAxis(s_virtual, (int)n, (Sint16)v);
  } else if (sscanf(cmd, "vbt%u=%d", &n, &v) == 2) {
    if (s_virtual != NULL) SDL_SetJoystickVirtualButton(s_virtual, (int)n, v != 0);
  } else if (sscanf(cmd, "vhat%u=%d", &n, &v) == 2) {
    if (s_virtual != NULL) SDL_SetJoystickVirtualHat(s_virtual, (int)n, (Uint8)v);
  } else {
    return 0;
  }
  return 1;
}

/* names: the action names of MKGP2PC_KEYS; fire: what a key of the action does */
void mkgp2pc_pad_init(const char* const* names, int count, void (*fire)(int action, bool down)) {
  s_names = names;
  s_actions = count < MAX_ACTIONS ? count : MAX_ACTIONS;
  s_fire = fire;
  s_accel = find_action("accel", 5);
  s_brake = find_action("brake", 5);
  s_log = getenv("MKGP2PC_PAD_LOG") != NULL;
  parse_settings();
}

/* After aurora_initialize, before the first frame. */
void mkgp2pc_pad_start(void) {
  const char* test = getenv("MKGP2PC_PAD_TEST");
  if (s_off) {
    fprintf(stderr, "pad: off (MKGP2PC_PAD)\n");
    return;
  }
  if (!SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD)) {
    fprintf(stderr, "pad: SDL has no joystick support here: %s\n", SDL_GetError());
    s_off = true;
    return;
  }
  s_ready = true;
  if (test != NULL && *test != 0) {
    make_virtual(test);
  }
  open_device(true);
  if (s_joy == NULL && s_wanted[0] == 0) {
    fprintf(stderr, "pad: no controller found\n");
  }
}

/* Controllers coming and going. */
void mkgp2pc_pad_event(const SDL_Event* e) {
  if (!s_ready) {
    return;
  }
  if (e->type == SDL_EVENT_JOYSTICK_REMOVED && s_joy != NULL && e->jdevice.which == s_id) {
    fprintf(stderr, "pad: controller removed\n");
    close_device();
    open_device(false);
  } else if (e->type == SDL_EVENT_JOYSTICK_ADDED && s_joy == NULL) {
    open_device(false);
  }
}

/* Once per pass over the window's events: buttons become actions. */
void mkgp2pc_pad_poll(void) {
  if (s_joy == NULL) {
    return;
  }
  if (s_log) {
    log_changes();
  }
  if (!s_restKnown) {
    s_restKnown = true;
    s_rest[0] = steer_now();
    s_rest[1] = pedal_now(s_accel);
    s_rest[2] = pedal_now(s_brake);
  }
  if (!s_used) {
    float now[3] = {steer_now(), pedal_now(s_accel), pedal_now(s_brake)};
    for (int i = 0; i < 3; i++) {
      float d = now[i] - s_rest[i];
      /* the wheel's value spans twice what a pedal's does */
      if ((d < 0.0f ? -d : d) > (i == 0 ? 0.3f : 0.15f)) {
        s_used = true;
      }
    }
  }
  for (int action = 0; action < s_actions; action++) {
    bool down = false;
    bool pedal = action == s_accel || action == s_brake;
    for (int i = 0; i < SOURCES; i++) {
      const Source* src = &s_map[action][i];
      if (src->kind != SRC_NONE && !(pedal && is_axis(src))) {
        down |= pressed(src) > 0.5f;
      }
    }
    if (down != s_held[action]) {
      s_held[action] = down;
      s_used = true;
      s_fire(action, down);
    }
  }
}

/* The wheel (-1 left .. 1 right) and the pedals (0 .. 1). false: no
 * controller, or one nobody has touched yet. */
bool mkgp2pc_pad_axes(float* steer, float* accel, float* brake) {
  if (s_joy == NULL || !s_used) {
    return false;
  }
  *steer = steer_now();
  *accel = pedal_now(s_accel);
  *brake = pedal_now(s_brake);
  return true;
}
