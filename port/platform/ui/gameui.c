/* Port screens drawn with the game's own text and sprites
 * (docs/port/game_ui.md).
 *
 * Everything here goes through the game's sprite system: a text is a
 * TextManager object of the game (fn_801F8A34 queues one sprite per
 * character from the 16 or 24 pixel font, outline included), the 64 pixel
 * characters, panels, cursors and control hints are sprite resources queued
 * with fn_8011FAFC, exactly as the game's menus do. Nothing is read from
 * files by the port and no picture is kept in the repository; the textures
 * come from the disc through the game's resource loader, and the
 * high-resolution option (MKGP2PC_HIRES) replaces them like any other.
 *
 * The functions are called from the game's frame (a scene's Update or
 * Draw), every frame while the screen is up. They queue sprites the way the
 * game's scenes do; the game draws the eight layers of queued sprites in
 * order after the scene's own Draw, so a port screen takes the top layer, 7:
 *
 *     mkgp2pc_gameui_begin(layer);          once, when the screen opens
 *     mkgp2pc_gameui_panel(...);            every frame
 *     mkgp2pc_gameui_text(...);
 *     mkgp2pc_gameui_end();                 once, when it closes
 *
 * Text is UTF-8. A character the game's font does not have is drawn as a
 * space (the font has kana, Latin letters, digits and about 450 kanji; see
 * gameui_chars.h). Coordinates are the game's 640x480 screen; sprites are
 * placed by their centre, text by the centre of its first character or, with
 * GAMEUI_CENTER, by the middle of the line. Colours are 0xRRGGBBAA. */
#include <string.h>

#include "mkgp2pc/gameui.h"
#include "gameui_chars.h"

/* ---- the game's side ---------------------------------------------------- */

/* ResCtrl (include/game/FlowScene.h): what a scene fills in to queue one
 * sprite. */
typedef struct Sprite {
  int resId;
  int field4;
  int mode; /* 2: a plain rectangle of `width` x `height` */
  int fieldC;
  unsigned char flag, pad11[3];
  float x, y, z, width, height, angle, scaleX, scaleY;
  unsigned int colors[4];
  unsigned char flipX, flipY, pad46[2];
  int tail[4];
} Sprite;

/* TextManager (include/game/TextDraw801F.h). */
typedef struct Text {
  float x, y, cursorX, scaleX, scaleY, angle;
  int font; /* 0: 16 pixel, 1: 24 pixel */
  int field1C;
  unsigned int color;
  unsigned short glyphs[256]; /* indices into lbl_804E5590, 0xFFFF ends */
  float z;
  int mode;
  float advance;
  int spare[4];
} Text;

/* lbl_804E5590 (include/game/types/CardCharEntry.h). */
typedef struct Glyph {
  unsigned short card, code;
  int res16, res24, res64;
} Glyph;

extern Glyph lbl_804E5590[];
Sprite* ResCtrl_Init(Sprite* s);
unsigned char fn_8011FAFC(Sprite* s, int layer); /* queue for this frame */
unsigned char PreloadResource(int id);           /* takes a reference */
int fn_80120A08(int id);                         /* gives it back */
void TexMgr_Init(void* text);
void TexMgr_Apply(void* text);                         /* PreloadResource for every character */
void fn_801F8A34(void* unused, void* text, int layer); /* queue the characters */
void fn_801F9518(void* text);                          /* release, 24 pixel font */
void fn_801F95A0(void* text);                          /* release, 16 pixel font */
int isJapanese(void);

#define GLYPH_WIDE 654   /* entries 0..653: full-width characters */
#define GLYPH_COUNT 739  /* 654..738: ASCII */
#define GLYPH_NO_64 10497 /* the 64 pixel font's blank, also named by every entry it lacks */

/* ---- state -------------------------------------------------------------- */

#define MAX_RES 160
#define MAX_LABELS 32
#define LABEL_TEXT 128

typedef struct Label {
  int used;
  int font;
  char utf8[LABEL_TEXT];
  int count;
  float span; /* first character's centre to the last one's, unscaled */
  Text text;
} Label;

/* Two sets: the screen of the scene that runs, and the one that is laid over
 * a stopped game (mkgp2pc_gameui_overlay_begin), which may come up while the
 * first is open. */
typedef struct State {
  int open;
  int layer;
  int res[MAX_RES];
  int resCount;
  Label labels[MAX_LABELS];
} State;

static State s_states[2];
static State* s_state = &s_states[0];
#define s_open (s_state->open)
#define s_layer (s_state->layer)
#define s_res (s_state->res)
#define s_resCount (s_state->resCount)
#define s_labels (s_state->labels)

static void hold(int res) {
  int i;
  for (i = 0; i < s_resCount; i++) {
    if (s_res[i] == res) {
      return;
    }
  }
  if (s_resCount < MAX_RES && PreloadResource(res)) {
    s_res[s_resCount++] = res;
  }
}

void mkgp2pc_gameui_begin(int layer) {
  if (s_open) {
    mkgp2pc_gameui_end();
  }
  s_open = 1;
  s_layer = layer;
}

int mkgp2pc_gameui_is_open(void) { return s_open; }

void mkgp2pc_gameui_end(void) {
  int i;
  if (!s_open) {
    return;
  }
  for (i = 0; i < MAX_LABELS; i++) {
    if (s_labels[i].used) {
      if (s_labels[i].font == GAMEUI_FONT_24) {
        fn_801F9518(&s_labels[i].text);
      } else if (s_labels[i].font == GAMEUI_FONT_16) {
        fn_801F95A0(&s_labels[i].text);
      }
      s_labels[i].used = 0;
    }
  }
  for (i = 0; i < s_resCount; i++) {
    fn_80120A08(s_res[i]);
  }
  s_resCount = 0;
  s_open = 0;
}

/* Which of the game's two sets of texts is in use. The function the
 * decompilation calls isJapanese returns 0 on this (Japanese) program, and
 * the game's text drawing takes the full-cell spacing then; the other value
 * selects the `E_` resources. */
int mkgp2pc_gameui_english(void) { return isJapanese() != 0; }

/* ---- sprites ------------------------------------------------------------ */

void mkgp2pc_gameui_sprite(int res, float x, float y, float scaleX, float scaleY, unsigned int rgba) {
  Sprite s;
  if (!s_open) {
    return;
  }
  hold(res);
  ResCtrl_Init(&s);
  s.resId = res;
  s.flag = 1;
  s.x = x;
  s.y = y;
  s.z = 1.0f;
  s.scaleX = scaleX;
  s.scaleY = scaleY;
  s.colors[0] = s.colors[1] = s.colors[2] = s.colors[3] = rgba;
  fn_8011FAFC(&s, s_layer);
}

void mkgp2pc_gameui_fill(float x, float y, float width, float height, unsigned int rgba) {
  Sprite s;
  if (!s_open) {
    return;
  }
  ResCtrl_Init(&s);
  s.mode = 2;
  s.flag = 1;
  s.x = x;
  s.y = y;
  s.z = 1.0f;
  s.width = width;
  s.height = height;
  s.colors[0] = s.colors[1] = s.colors[2] = s.colors[3] = rgba;
  fn_8011FAFC(&s, s_layer);
}

/* The whole picture darkened, as under the game's message windows; also
 * left and right of the 640 wide screen, for the 16:9 option. */
void mkgp2pc_gameui_dim(unsigned int alpha) {
  mkgp2pc_gameui_fill(-320.0f, 240.0f, 640.0f, 480.0f, alpha & 0xFF);
  mkgp2pc_gameui_fill(320.0f, 240.0f, 640.0f, 480.0f, alpha & 0xFF);
  mkgp2pc_gameui_fill(960.0f, 240.0f, 640.0f, 480.0f, alpha & 0xFF);
}

/* The blue panel of the name entry screen (resource 0x6F0, 588x212). */
void mkgp2pc_gameui_panel(float x, float y, float width, float height, unsigned int rgba) {
  mkgp2pc_gameui_sprite(0x6F0, x, y, width / 588.0f, height / 212.0f, rgba);
}

/* The game's message window: white dotted sheet with a blue edge (0x731,
 * 472x168) over its shadow (0x72F, 272x304, placed as MsgPopup::Tick does). */
void mkgp2pc_gameui_window(float x, float y, float width, float height) {
  float sx = width / 472.0f, sy = height / 168.0f;
  mkgp2pc_gameui_sprite(0x72F, x + 10.0f * sx, y + 10.0f * sy, 1.8f * sx, 0.57f * sy, 0xFFFFFFFF);
  mkgp2pc_gameui_sprite(0x731, x, y, sx, sy, 0xFFFFFFFF);
}

/* The glow the name entry screen puts behind the chosen key (0x715, 64x64),
 * stretched to the given size. */
void mkgp2pc_gameui_glow(float x, float y, float width, float height, unsigned int rgba) {
  mkgp2pc_gameui_sprite(0x715, x, y, width / 64.0f, height / 64.0f, rgba);
}

/* ---- text --------------------------------------------------------------- */

static int next_code(const unsigned char** p) {
  const unsigned char* s = *p;
  int c = *s++;
  if (c >= 0xF0 && s[0] && s[1] && s[2]) {
    c = ((c & 7) << 18) | ((s[0] & 63) << 12) | ((s[1] & 63) << 6) | (s[2] & 63);
    s += 3;
  } else if (c >= 0xE0 && s[0] && s[1]) {
    c = ((c & 15) << 12) | ((s[0] & 63) << 6) | (s[1] & 63);
    s += 2;
  } else if (c >= 0xC0 && s[0]) {
    c = ((c & 31) << 6) | (s[0] & 63);
    s += 1;
  }
  *p = s;
  return c;
}

static int find_glyph(int code) {
  int i;
  if (code < 0x80) {
    for (i = GLYPH_WIDE; i < GLYPH_COUNT; i++) {
      if (kGameUiChars[i] == code) {
        return i;
      }
    }
    /* ASCII the half-width set lacks: the full-width form, if any */
    code = code == ' ' ? 0x3000 : code + 0xFEE0;
  }
  for (i = 1; i < GLYPH_WIDE; i++) {
    if (kGameUiChars[i] == code) {
      return i;
    }
  }
  return 0; /* the table's blank */
}

/* How far the game's text drawing (fn_801F8A34) moves on after a character. */
static float advance_of(int font, int glyph) {
  float wide, narrow;
  if (isJapanese()) {
    wide = font == GAMEUI_FONT_24 ? 20.0f : 12.0f;
    narrow = 10.0f;
  } else {
    wide = font == GAMEUI_FONT_24 ? 24.0f : 16.0f;
    narrow = 12.0f;
  }
  if (glyph == GLYPH_WIDE) {
    return narrow;
  }
  if (glyph < GLYPH_WIDE || font == GAMEUI_FONT_24) {
    return wide;
  }
  return narrow;
}

static Label* label_for(int font, const char* utf8) {
  int i;
  Label* l = NULL;
  const unsigned char* p;
  for (i = 0; i < MAX_LABELS; i++) {
    if (s_labels[i].used && s_labels[i].font == font && strcmp(s_labels[i].utf8, utf8) == 0) {
      return &s_labels[i];
    }
    if (!s_labels[i].used && l == NULL) {
      l = &s_labels[i];
    }
  }
  if (l == NULL || strlen(utf8) >= LABEL_TEXT) {
    return NULL;
  }
  l->used = 1;
  l->font = font;
  strcpy(l->utf8, utf8);
  l->count = 0;
  l->span = 0.0f;
  TexMgr_Init(&l->text);
  l->text.font = font == GAMEUI_FONT_24 ? 1 : 0;
  p = (const unsigned char*)utf8;
  while (*p && l->count < 255) {
    int g = find_glyph(next_code(&p));
    if (*p) {
      l->span += font == GAMEUI_FONT_64 ? 1.0f : advance_of(font, g);
    }
    l->text.glyphs[l->count++] = (unsigned short)g;
  }
  l->text.glyphs[l->count] = 0xFFFF;
  if (font == GAMEUI_FONT_64) {
    for (i = 0; i < l->count; i++) {
      hold(lbl_804E5590[l->text.glyphs[i]].res64);
    }
  } else {
    TexMgr_Apply(&l->text);
  }
  return l;
}

/* Distance between adjacent 64 pixel characters at scale 1; the name entry
 * screen uses 72. */
#define PITCH_64 60.0f

float mkgp2pc_gameui_text_width(int font, float scale, const char* utf8) {
  Label* l;
  if (!s_open || (l = label_for(font, utf8)) == NULL) {
    return 0.0f;
  }
  if (font == GAMEUI_FONT_64) {
    return (l->span * PITCH_64 + 64.0f) * scale;
  }
  return (l->span + (font == GAMEUI_FONT_24 ? 24.0f : 16.0f)) * scale;
}

void mkgp2pc_gameui_text(int font, int align, float x, float y, float scale, unsigned int rgba, const char* utf8) {
  Label* l;
  float span;
  if (!s_open || (l = label_for(font, utf8)) == NULL) {
    return;
  }
  span = l->span * (font == GAMEUI_FONT_64 ? PITCH_64 : 1.0f) * scale;
  if (align == GAMEUI_CENTER) {
    x -= span * 0.5f;
  } else if (align == GAMEUI_RIGHT) {
    x -= span;
  }
  if (font == GAMEUI_FONT_64) {
    int i;
    for (i = 0; i < l->count; i++) {
      int res = lbl_804E5590[l->text.glyphs[i]].res64;
      if (res != GLYPH_NO_64) {
        mkgp2pc_gameui_sprite(res, x + (float)i * PITCH_64 * scale, y, scale, scale, rgba);
      }
    }
    return;
  }
  l->text.x = x;
  l->text.y = y;
  l->text.scaleX = scale;
  l->text.scaleY = scale;
  l->text.color = rgba;
  fn_801F8A34(NULL, &l->text, s_layer);
}

/* ---- control hints ------------------------------------------------------ */

typedef struct Step {
  int from;
  int res;
} Step;

static int step_res(const Step* steps, int n, int t) {
  int i, res = steps[0].res;
  for (i = 0; i < n && t >= steps[i].from; i++) {
    res = steps[i].res;
  }
  return res;
}

/* The pulse of the hint captions (clMnHandleFont::Draw): scale and alpha
 * over a 60 frame cycle. */
static void caption_pulse(int t, float* scale, unsigned int* alpha) {
  t %= 60;
  if (t < 10) {
    *scale = 0.8f + (0.7f - 0.8f) * (float)t / 10.0f;
    *alpha = 0xCC;
  } else if (t < 26) {
    *scale = 0.7f + (1.2f - 0.7f) * (float)(t - 10) / 16.0f;
    *alpha = 0xCC + (0xFF - 0xCC) * (t - 10) / 16;
  } else if (t < 40) {
    *scale = 1.2f + (0.7f - 1.2f) * (float)(t - 26) / 14.0f;
    *alpha = 0xFF + (0xCC - 0xFF) * (t - 26) / 14;
  } else if (t < 50) {
    *scale = 0.7f + (0.8f - 0.7f) * (float)(t - 40) / 10.0f;
    *alpha = 0xCC;
  } else {
    *scale = 0.8f;
    *alpha = 0xCC;
  }
}

/* The steering wheel turning left and right with the caption "選択" beside
 * it, as on the game's selection screens (clMnHandleTask; frames from its
 * table lbl_803C0074). `t` counts frames. */
void mkgp2pc_gameui_hint_select(float x, float y, int t) {
  static const Step wheel[] = {{0, 0x89D},  {24, 0x89E}, {28, 0x89F},  {32, 0x8A0},  {45, 0x89F},
                               {50, 0x89E}, {58, 0x89D}, {84, 0x89C},  {88, 0x89B},  {92, 0x8A1},
                               {105, 0x89B}, {110, 0x89C}, {116, 0x89D}};
  float s;
  unsigned int a;
  mkgp2pc_gameui_sprite(step_res(wheel, 13, t % 120), x, y, 1.0f, 1.0f, 0xFFFFFFFF);
  caption_pulse(t, &s, &a);
  mkgp2pc_gameui_sprite(0x8A2, x + 60.0f * s, y - 4.0f * s, s, s, 0xFFFFFF00 | a);
}

/* The pedals with the right one going down and the caption "決定"
 * (clMnPedalSprTask type 0, frames from lbl_803C00DC). */
void mkgp2pc_gameui_hint_decide(float x, float y, int t) {
  static const Step pedal[] = {{0, 0x883}, {10, 0x887}, {14, 0x888}, {18, 0x889}, {40, 0x888}, {44, 0x887}, {50, 0x883}};
  float s;
  unsigned int a;
  t %= 60;
  mkgp2pc_gameui_sprite(step_res(pedal, 7, t), x, y, 1.0f, 1.0f, 0xFFFFFFFF);
  if (t >= 18 && t < 44) {
    /* the pressed pedal lights up: fades in over 12 frames, out over 14 */
    int k = t - 18;
    unsigned int glow = k < 12 ? (unsigned int)(k * 255 / 12) : (unsigned int)((26 - k) * 255 / 14);
    mkgp2pc_gameui_sprite(0x88B, x, y, 1.0f, 1.0f, 0xFFFFFF00 | glow);
  }
  caption_pulse(t, &s, &a);
  mkgp2pc_gameui_sprite(0x88D, x + 61.0f * s, y - 4.0f * s, s, s, 0xFFFFFF00 | a);
}

/* The cabinet's item button flashing (resources 0x896 dark to 0x899 lit),
 * with a caption to its right in the 64 pixel font at 0.4, which has kana,
 * Latin letters and digits only; the pair is centred on x. The game has no
 * caption picture for its button, so this one is text. */
void mkgp2pc_gameui_hint_button(float x, float y, int t, const char* caption) {
  static const Step button[] = {{0, 0x896}, {30, 0x897}, {34, 0x898}, {38, 0x899}, {52, 0x898}, {56, 0x897}};
  const float scale = 0.4f, gap = 6.0f;
  float w = mkgp2pc_gameui_text_width(GAMEUI_FONT_64, scale, caption);
  float left = x - (52.0f + gap + w) * 0.5f;
  mkgp2pc_gameui_sprite(step_res(button, 6, t % 60), left + 26.0f, y, 1.0f, 1.0f, 0xFFFFFFFF);
  mkgp2pc_gameui_text(GAMEUI_FONT_64, GAMEUI_LEFT, left + 52.0f + gap + 32.0f * scale, y, scale, 0xFFFFFFFF, caption);
}

/* ---- a ready-made screen: choose one of a few things -------------------- */

/* The window's way in, after the game's message window (MsgPopup): from
 * small past full size and back, 15 frames. */
static float open_scale(int age) {
  if (age < 10) {
    return 0.4f + (1.1f - 0.4f) * (float)age / 10.0f;
  }
  if (age < 15) {
    return 1.1f + (1.0f - 1.1f) * (float)(age - 10) / 5.0f;
  }
  return 1.0f;
}

void mkgp2pc_gameui_choice(const char* title, const char* const* items, int count, int cursor, int age) {
  const float cx = 320.0f, cy = 228.0f;
  float s = open_scale(age);
  unsigned int dim = age < 10 ? (unsigned int)(0x90 * age / 10) : 0x90;
  float pitch, widest = 0.0f;
  int i;
  if (count < 1) {
    return;
  }
  for (i = 0; i < count; i++) {
    float w = mkgp2pc_gameui_text_width(GAMEUI_FONT_24, 1.0f, items[i]);
    if (w > widest) {
      widest = w;
    }
  }
  pitch = widest + 56.0f;
  if (pitch * (float)count > 520.0f) {
    pitch = 520.0f / (float)count;
  }
  mkgp2pc_gameui_dim(dim);
  /* the panel is see-through (on the name entry screen it lies on a plain
   * background); twice over each other it carries text over any picture */
  mkgp2pc_gameui_panel(cx, cy, 548.0f * s, 196.0f * s, 0xFFFFFFFF);
  mkgp2pc_gameui_panel(cx, cy, 548.0f * s, 196.0f * s, 0xFFFFFFFF);
  if (title != NULL && title[0]) {
    mkgp2pc_gameui_text(GAMEUI_FONT_24, GAMEUI_CENTER, cx, cy - 50.0f * s, s, 0xFFFFFFFF, title);
  }
  for (i = 0; i < count; i++) {
    float x = cx + ((float)i - (float)(count - 1) * 0.5f) * pitch * s;
    float y = cy + 26.0f * s;
    if (i == cursor) {
      /* the name entry screen's cursor: the glow breathing behind the text */
      int k = age % 40;
      float beat = (float)(k < 20 ? k : 40 - k) / 20.0f;
      float w = mkgp2pc_gameui_text_width(GAMEUI_FONT_24, 1.0f, items[i]);
      mkgp2pc_gameui_glow(x, y, (w + 56.0f + 8.0f * beat) * s, (58.0f + 6.0f * beat) * s, 0xFFFFFF00 | (unsigned int)(0xC0 + 0x3F * beat));
      mkgp2pc_gameui_text(GAMEUI_FONT_24, GAMEUI_CENTER, x, y, s * 1.1f, 0xFFFFFFFF, items[i]);
    } else {
      mkgp2pc_gameui_text(GAMEUI_FONT_24, GAMEUI_CENTER, x, y, s * 0.9f, 0xFFFFFFB0, items[i]);
    }
  }
  if (age >= 15) {
    mkgp2pc_gameui_hint_select(224.0f, 452.0f, age - 15);
    mkgp2pc_gameui_hint_decide(366.0f, 452.0f, age - 15);
  }
}

/* A whole screen: a list of rows one under the other with a cursor, as the
 * port's menu has it. kinds: 0 a choice, 1 a choice that cannot be taken
 * (pale), 2 a heading (small; an empty one is half a row of space). The
 * note and the footer are small lines under the list. */
void mkgp2pc_gameui_list(const char* title, const char* const* rows, const int* kinds, int count, int cursor,
                         const char* note, const char* footer, int age) {
  const float cx = 320.0f, cy = 240.0f;
  const float rowH = 29.0f, headH = 22.0f, gapH = 10.0f;
  float s = open_scale(age);
  unsigned int dim = age < 10 ? (unsigned int)(0x90 * age / 10) : 0x90;
  float listH = 0.0f, widest = 0.0f, panelW, panelH, y;
  int i;
  for (i = 0; i < count; i++) {
    float w = mkgp2pc_gameui_text_width(kinds[i] == 2 ? GAMEUI_FONT_16 : GAMEUI_FONT_24, 1.0f, rows[i]);
    if (w > widest) {
      widest = w;
    }
    listH += kinds[i] != 2 ? rowH : (rows[i][0] ? headH : gapH);
  }
  panelW = widest + 150.0f;
  if (panelW < 380.0f) {
    panelW = 380.0f;
  }
  panelH = listH + 128.0f;
  mkgp2pc_gameui_dim(dim);
  mkgp2pc_gameui_panel(cx, cy, panelW * s, panelH * s, 0xFFFFFFFF);
  mkgp2pc_gameui_panel(cx, cy, panelW * s, panelH * s, 0xFFFFFFFF);
  if (age < 12) {
    return; /* the texts come when the panel has its size */
  }
  y = cy - panelH * 0.5f + 34.0f;
  if (title != NULL && title[0]) {
    mkgp2pc_gameui_text(GAMEUI_FONT_24, GAMEUI_CENTER, cx, y, 1.0f, 0xFFE860FF, title);
  }
  y += 36.0f;
  for (i = 0; i < count; i++) {
    if (kinds[i] == 2) {
      if (rows[i][0]) {
        mkgp2pc_gameui_text(GAMEUI_FONT_16, GAMEUI_CENTER, cx, y + headH * 0.5f + 2.0f, 1.0f, 0xA0D0FFFF, rows[i]);
        y += headH;
      } else {
        y += gapH;
      }
      continue;
    }
    if (i == cursor && kinds[i] == 0) {
      int k = age % 40;
      float beat = (float)(k < 20 ? k : 40 - k) / 20.0f;
      mkgp2pc_gameui_glow(cx, y + rowH * 0.5f, widest + 64.0f + 8.0f * beat, rowH + 14.0f + 4.0f * beat,
                          0xFFFFFF00 | (unsigned int)(0xC0 + 0x3F * beat));
      mkgp2pc_gameui_text(GAMEUI_FONT_24, GAMEUI_CENTER, cx, y + rowH * 0.5f, 1.0f, 0xFFFFFFFF, rows[i]);
    } else {
      mkgp2pc_gameui_text(GAMEUI_FONT_24, GAMEUI_CENTER, cx, y + rowH * 0.5f, 0.9f,
                          kinds[i] == 0 ? 0xFFFFFFD0 : 0xFFFFFF50, rows[i]);
    }
    y += rowH;
  }
  y = cy + panelH * 0.5f - 42.0f;
  if (note != NULL && note[0]) {
    mkgp2pc_gameui_text(GAMEUI_FONT_16, GAMEUI_CENTER, cx, y, 1.0f, 0xFFFFFFFF, note);
  }
  if (footer != NULL && footer[0]) {
    mkgp2pc_gameui_text(GAMEUI_FONT_16, GAMEUI_CENTER, cx, y + 20.0f, 1.0f, 0xFFFFFFA0, footer);
  }
}

/* ---- over a stopped game -------------------------------------------------- */

/* The game's list of queued sprites (SpriteList in src/game/SpriteHandle.cpp).
 * A scene queues in its Update what the next frame's Draw shows, so while
 * the game is stopped between two frames the list holds the sprites of the
 * frame to come. */
typedef struct SpriteList {
  int capacity, count;
  void *nodes, *freeHead, *freeTail, *head, *tail;
} SpriteList;
extern SpriteList* lbl_806D1500;
void fn_8011F994(int layer);      /* draw one layer of the queue now */
void SpriteHandlePool_GC(void);   /* empty the queue */

static void *s_keptHead, *s_keptTail;
static int s_overlay;

/* A screen over a game that stands still: the sprites the stopped scene has
 * queued are put aside (they are neither drawn with the overlay nor used
 * up), and the overlay's own queue is drawn by mkgp2pc_gameui_overlay_draw.
 * The caller provides the frame around it and whatever lies underneath. */
void mkgp2pc_gameui_overlay_begin(void) {
  if (s_overlay) {
    return;
  }
  s_overlay = 1;
  s_keptHead = lbl_806D1500->head;
  s_keptTail = lbl_806D1500->tail;
  lbl_806D1500->head = lbl_806D1500->tail = NULL;
  s_state = &s_states[1];
  mkgp2pc_gameui_begin(7);
}

void mkgp2pc_gameui_overlay_draw(void) {
  if (s_overlay) {
    fn_8011F994(7);
    SpriteHandlePool_GC();
  }
}

void mkgp2pc_gameui_overlay_end(void) {
  if (!s_overlay) {
    return;
  }
  SpriteHandlePool_GC();
  mkgp2pc_gameui_end();
  s_state = &s_states[0];
  lbl_806D1500->head = s_keptHead;
  lbl_806D1500->tail = s_keptTail;
  s_overlay = 0;
}
