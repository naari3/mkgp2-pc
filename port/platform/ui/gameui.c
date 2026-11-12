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
#define MAX_LABELS 96
#define LABEL_TEXT 128

typedef struct Label {
  int used;
  unsigned stamp; /* when it was last asked for (s_labelClock) */
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

/* SpriteHandle (include/game/types/SpriteHandle.h): a queued sprite. Only
 * what a piece of a picture needs: the four corners on the screen (bottom
 * left, top left, top right, bottom right) and the two texture corners. */
typedef struct Handle {
  char pad0[0x88];
  float corner[4][2];
  float texUV[2][2];
  char padB8[0xDA - 0xB8];
  signed char dirty;
  char padDB;
  int tail[2];
} Handle;
extern Handle g_SpriteHandlePool[];
int SpriteHandle_CreateEx(Sprite* s, int layer, unsigned char keep); /* queue; returns the handle's number */

/* The part (sx0,sy0)-(sx1,sy1) of a picture of w x h, drawn into
 * (dx0,dy0)-(dx1,dy1) on the screen. */
static void piece(int res, float w, float h, float sx0, float sy0, float sx1, float sy1, float dx0, float dy0,
                  float dx1, float dy1, unsigned int rgba) {
  Sprite s;
  Handle* p;
  float u0, v0, du, dv;
  int n;
  hold(res);
  ResCtrl_Init(&s);
  s.resId = res;
  s.flag = 1;
  s.x = (dx0 + dx1) * 0.5f;
  s.y = (dy0 + dy1) * 0.5f;
  s.z = 1.0f;
  s.colors[0] = s.colors[1] = s.colors[2] = s.colors[3] = rgba;
  n = SpriteHandle_CreateEx(&s, s_layer, 0);
  if (n == 0) {
    return;
  }
  p = &g_SpriteHandlePool[(n - 1) % 500];
  p->corner[0][0] = dx0, p->corner[0][1] = dy1;
  p->corner[1][0] = dx0, p->corner[1][1] = dy0;
  p->corner[2][0] = dx1, p->corner[2][1] = dy0;
  p->corner[3][0] = dx1, p->corner[3][1] = dy1;
  u0 = p->texUV[0][0], v0 = p->texUV[0][1];
  du = (p->texUV[1][0] - u0) / w, dv = (p->texUV[1][1] - v0) / h;
  p->texUV[0][0] = u0 + du * sx0, p->texUV[0][1] = v0 + dv * sy0;
  p->texUV[1][0] = u0 + du * sx1, p->texUV[1][1] = v0 + dv * sy1;
  p->dirty = 0;
}

/* A frame picture of w x h drawn at any size without stretching it: the
 * four corners (`edge` pixels each way) as they are, the sides and the
 * middle repeated. mirror: the right side is the left one turned over (for
 * a picture whose right side is something else, like the tail of a speech
 * bubble); the middle then repeats only the picture's left half. */
void mkgp2pc_gameui_frame(int res, float w, float h, float edge, int mirror, float x, float y, float width,
                          float height, unsigned int rgba) {
  const float midW = mirror ? w * 0.5f - edge : w - 2.0f * edge, midH = h - 2.0f * edge;
  const float x0 = x - width * 0.5f, y0 = y - height * 0.5f;
  float dy = y0;
  int row = 0;
  if (!s_open || midW <= 0.0f || midH <= 0.0f || width < 2.0f * edge || height < 2.0f * edge) {
    return;
  }
  while (row < 3) {
    /* one band: the top edge, a strip of the middle, or the bottom edge */
    float bandH, srcY;
    float dx = x0;
    int col = 0;
    if (row == 0) {
      bandH = edge, srcY = 0.0f;
    } else if (row == 1) {
      float left = y0 + height - edge - dy;
      if (left <= 0.01f) {
        row = 2;
        continue;
      }
      bandH = left < midH ? left : midH, srcY = edge;
    } else {
      bandH = edge, srcY = h - edge;
    }
    while (col < 3) {
      float pw, srcX0, srcX1;
      if (col == 0) {
        pw = edge, srcX0 = 0.0f, srcX1 = edge;
      } else if (col == 1) {
        float left = x0 + width - edge - dx;
        if (left <= 0.01f) {
          col = 2;
          continue;
        }
        pw = left < midW ? left : midW, srcX0 = edge, srcX1 = edge + pw;
      } else if (mirror) {
        pw = edge, srcX0 = edge, srcX1 = 0.0f;
      } else {
        pw = edge, srcX0 = w - edge, srcX1 = w;
      }
      piece(res, w, h, srcX0, srcY, srcX1, srcY + bandH, dx, dy, dx + pw, dy + bandH, rgba);
      dx += pw;
      if (col != 1) {
        col++;
      }
    }
    dy += bandH;
    if (row != 1) {
      row++;
    }
  }
}

/* The sheet the port's screens write on: the game's dark dotted caption
 * bubble (0x72D, without its tail), which the game writes on in white, at
 * any size. It is see-through; a dark fill under it keeps a busy picture
 * from showing through the text. */
void mkgp2pc_gameui_sheet(float x, float y, float width, float height) {
  mkgp2pc_gameui_fill(x, y, width - 8.0f, height - 8.0f, 0x000818B0);
  mkgp2pc_gameui_frame(0x72D, 296.0f, 80.0f, 16.0f, 1, x, y, width, height, 0xFFFFFFFF);
}

/* The game's white message window (the upright one of the card screens,
 * 0x72E, over its shadow 0x72F) at any size, for pictures and dark things;
 * the game's text fonts are white with a black outline and read poorly on
 * it. */
void mkgp2pc_gameui_white_sheet(float x, float y, float width, float height) {
  /* half see-through: the stopped game shows through the window */
  mkgp2pc_gameui_sprite(0x72F, x + 8.0f, y + 8.0f, (width + 16.0f) / 272.0f, (height + 8.0f) / 304.0f, 0xFFFFFF60);
  mkgp2pc_gameui_frame(0x72E, 248.0f, 304.0f, 24.0f, 0, x, y, width, height, 0xFFFFFFA8);
}

/* The window of the card screens (0x63F, 628x336 of sprite_card_1.tpl): a
 * blue starred rim with a tab at the top left, where the title goes, and a
 * grey chequered ground. Its width is the picture's; its height is the top
 * (56), the foot (40) and `squares` rows of the ground's 45 pixel squares,
 * so the pattern goes on without a seam. `scale` is for the way in. */
#define CARD_W 628.0f
#define CARD_TOP 56.0f
#define CARD_FOOT 40.0f
#define CARD_SQUARE 45.0f
static float card_height(int squares) {
  return CARD_TOP + CARD_FOOT + CARD_SQUARE * (float)squares;
}
static int card_squares(float content) {
  int n = (int)((content + CARD_SQUARE - 1.0f) / CARD_SQUARE);
  return n < 1 ? 1 : (n > 8 ? 8 : n);
}
void mkgp2pc_gameui_card_sheet(float x, float y, int squares, float scale, unsigned int rgba) {
  /* 4 pixels narrower than the picture: at 628 the 16:9 picture takes the
   * window for something as wide as the screen and stretches it
   * (gx_shim.cpp: 627.2 and more) */
  const float x0 = x - (CARD_W - 4.0f) * 0.5f * scale, x1 = x + (CARD_W - 4.0f) * 0.5f * scale;
  float dy = y - card_height(squares) * 0.5f * scale;
  float left = CARD_SQUARE * (float)squares;
  piece(0x63F, CARD_W, 336.0f, 0.0f, 0.0f, CARD_W, CARD_TOP, x0, dy, x1, dy + CARD_TOP * scale, rgba);
  dy += CARD_TOP * scale;
  while (left > 0.0f) {
    float h = left < 225.0f ? left : 225.0f;
    piece(0x63F, CARD_W, 336.0f, 0.0f, CARD_TOP, CARD_W, CARD_TOP + h, x0, dy, x1, dy + h * scale, rgba);
    dy += h * scale;
    left -= h;
  }
  piece(0x63F, CARD_W, 336.0f, 0.0f, 336.0f - CARD_FOOT, CARD_W, 336.0f, x0, dy, x1, dy + CARD_FOOT * scale, rgba);
}

/* The chosen row: the pink bar of the card screens (0x665, 384x44) at the
 * row's size (its middle is plain), and the striped arrow. */
static void card_cursor(float x, float y, float width, float height, int t) {
  const float x0 = x - width * 0.5f, x1 = x + width * 0.5f, y0 = y - height * 0.5f, y1 = y + height * 0.5f;
  int k = t % 40;
  float beat = (float)(k < 20 ? k : 40 - k) / 20.0f;
  piece(0x665, 384.0f, 44.0f, 0.0f, 0.0f, 20.0f, 44.0f, x0, y0, x0 + 14.0f, y1, 0xFFFFFFFF);
  piece(0x665, 384.0f, 44.0f, 20.0f, 0.0f, 364.0f, 44.0f, x0 + 14.0f, y0, x1 - 14.0f, y1, 0xFFFFFFFF);
  /* the bar's right end runs off its picture: the left end, turned round */
  piece(0x665, 384.0f, 44.0f, 20.0f, 0.0f, 0.0f, 44.0f, x1 - 14.0f, y0, x1, y1, 0xFFFFFFFF);
  mkgp2pc_gameui_sprite(0x8CD, x0 - 8.0f + 4.0f * beat, y, 0.3f, 0.3f, 0xFFFFFFFF);
}

/* The cursor of the game's windows: the glow behind the chosen line and
 * the striped arrow pointing at it from the left. */
static void cursor_glow(float x, float y, float width, float height, int t, unsigned int rgb) {
  int k = t % 40;
  float beat = (float)(k < 20 ? k : 40 - k) / 20.0f;
  mkgp2pc_gameui_sprite(0x8CE, x, y, width / 100.0f * 1.25f, height / 76.0f * 1.5f,
                        (rgb & 0xFFFFFF00) | (unsigned int)(0xC0 + 0x3F * beat));
  mkgp2pc_gameui_sprite(0x8CD, x - width * 0.5f - 6.0f + 4.0f * beat, y, 0.3f, 0.3f, 0xFFFFFFFF);
}
void mkgp2pc_gameui_cursor(float x, float y, float width, float height, int t) {
  /* the game's glow is white; in blue it carries the white text */
  cursor_glow(x, y, width, height, t, 0x2878FF00);
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

/* A screen whose texts change (the menu's settings pages: values, key
 * names) asks for more different texts over time than there are labels.
 * When none is free the one not asked for the longest is given up; a frame
 * shows far fewer texts than there are labels, so it is never one of the
 * frame being queued. The 64 pixel texts hold sprite resources until the
 * screen ends and are not given up. */
static unsigned s_labelClock;
static Label* label_for(int font, const char* utf8) {
  int i;
  Label* l = NULL;
  Label* oldest = NULL;
  const unsigned char* p;
  for (i = 0; i < MAX_LABELS; i++) {
    if (s_labels[i].used && s_labels[i].font == font && strcmp(s_labels[i].utf8, utf8) == 0) {
      s_labels[i].stamp = ++s_labelClock;
      return &s_labels[i];
    }
    if (!s_labels[i].used && l == NULL) {
      l = &s_labels[i];
    }
    if (s_labels[i].used && s_labels[i].font != GAMEUI_FONT_64 &&
        (oldest == NULL || (int)(s_labels[i].stamp - oldest->stamp) < 0)) {
      oldest = &s_labels[i];
    }
  }
  if (strlen(utf8) >= LABEL_TEXT) {
    return NULL;
  }
  if (l == NULL && oldest != NULL) {
    if (oldest->font == GAMEUI_FONT_24) {
      fn_801F9518(&oldest->text);
    } else {
      fn_801F95A0(&oldest->text);
    }
    l = oldest;
  }
  if (l == NULL) {
    return NULL;
  }
  l->used = 1;
  l->stamp = ++s_labelClock;
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

/* The look of the port's windows. */
typedef struct Style {
  unsigned int sheet, title, row, rowOn, rowOff, head, line, note, foot;
  float onScale;
} Style;
/* The card screens' window, half see-through; white text with its outline,
 * headings in the light blue of the game's labels, the chosen row on the
 * pink bar. */
static const Style kStyle = {0xFFFFFFC8, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFF68,
                             0x60D8FFFF, 0xFFFFFF90, 0xFFFFFFFF, 0xFFFFFFD0, 1.12f};

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
  unsigned int dim = age < 10 ? (unsigned int)(0x7F * age / 10) : 0x7F;
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
  pitch = widest + 84.0f;
  if (pitch * (float)count > 520.0f) {
    pitch = 520.0f / (float)count;
  }
  mkgp2pc_gameui_dim(dim);
  mkgp2pc_gameui_card_sheet(cx, cy, 2, s, kStyle.sheet);
  if (age < 12) {
    return; /* the texts come when the window has its size */
  }
  s = 1.0f;
  if (title != NULL && title[0]) {
    mkgp2pc_gameui_text(GAMEUI_FONT_24, GAMEUI_LEFT, cx - CARD_W * 0.5f + 28.0f, cy - card_height(2) * 0.5f + 19.0f,
                        1.0f, kStyle.title, title);
  }
  for (i = 0; i < count; i++) {
    float x = cx + ((float)i - (float)(count - 1) * 0.5f) * pitch * s;
    float y = cy + 18.0f;
    if (i == cursor) {
      card_cursor(x, y, pitch - 36.0f, 36.0f, age);
    }
    mkgp2pc_gameui_text(GAMEUI_FONT_24, GAMEUI_CENTER, x, y, i == cursor ? kStyle.onScale : 1.0f,
                        i == cursor ? kStyle.rowOn : kStyle.row, items[i]);
  }
  if (age >= 15) {
    mkgp2pc_gameui_hint_select(224.0f, 452.0f, age - 15);
    mkgp2pc_gameui_hint_decide(366.0f, 452.0f, age - 15);
  }
}

/* A whole screen: a list of rows one under the other with a cursor, as the
 * port's menu has it, on the game's white message window. kinds: 0 a
 * choice, 1 a choice that cannot be taken (grey), 2 a heading (small; an
 * empty one is some space). values: NULL, or per row NULL or a text that
 * stands at the right of the row (a setting's value); such a list has its
 * labels at the left. more_above / more_below: marks that the list goes on.
 * The note and the footer are small lines under the list. */
void mkgp2pc_gameui_list(const char* title, const char* const* rows, const int* kinds, const char* const* values,
                         int count, int cursor, int more_above, int more_below, const char* note,
                         const char* footer, int age) {
  const float cx = 320.0f, cy = 240.0f;
  const float rowH = 30.0f, headH = 24.0f, gapH = 10.0f;
  unsigned int dim = age < 10 ? (unsigned int)(0x7F * age / 10) : 0x7F;
  float listH = 0.0f, widest = 0.0f, widestHead = 0.0f, widestValue = 0.0f, inner, sheetH, y, grow;
  int i, split = 0, squares;
  const Style* st = &kStyle;
  for (i = 0; i < count; i++) {
    float w = mkgp2pc_gameui_text_width(kinds[i] == 2 ? GAMEUI_FONT_16 : GAMEUI_FONT_24, 1.0f, rows[i]);
    if (kinds[i] == 2) {
      /* a heading spans the row; it does not push the values out */
      if (w > widestHead) {
        widestHead = w;
      }
    } else if (w > widest) {
      widest = w;
    }
    if (values != NULL && values[i] != NULL) {
      float v = mkgp2pc_gameui_text_width(GAMEUI_FONT_24, 1.0f, values[i]);
      split = 1;
      if (v > widestValue) {
        widestValue = v;
      }
    }
    listH += kinds[i] != 2 ? rowH : (rows[i][0] ? headH : gapH);
  }
  inner = widest + (split ? widestValue + 48.0f : 0.0f);
  if (inner < widestHead) {
    inner = widestHead;
  }
  inner += 24.0f;
  if (inner < 300.0f) {
    inner = 300.0f;
  }
  if (inner > 528.0f) {
    inner = 528.0f; /* the window stays inside the 4:3 picture */
  }
  squares = card_squares(listH + 20.0f);
  sheetH = card_height(squares);
  /* the window comes up as the game's do: small, a little too large, right */
  grow = age < 8 ? 0.5f + 0.55f * (float)age / 8.0f : (age < 12 ? 1.05f - 0.05f * (float)(age - 8) / 4.0f : 1.0f);
  mkgp2pc_gameui_dim(dim);
  mkgp2pc_gameui_card_sheet(cx, cy, squares, grow, st->sheet);
  if (age < 12) {
    return; /* the texts come when the window has its size */
  }
  y = cy - sheetH * 0.5f;
  if (title != NULL && title[0]) {
    mkgp2pc_gameui_text(GAMEUI_FONT_24, GAMEUI_LEFT, cx - CARD_W * 0.5f + 28.0f, y + 19.0f, 1.0f, st->title, title);
  }
  y += 50.0f;
  if (more_above) {
    mkgp2pc_gameui_sprite(0x8CD, cx, y - 5.0f, 0.16f, 0.16f, 0xFFFFFFFF);
  }
  for (i = 0; i < count; i++) {
    float mid, sc;
    unsigned int ink;
    if (kinds[i] == 2) {
      if (rows[i][0]) {
        mkgp2pc_gameui_text(GAMEUI_FONT_16, GAMEUI_LEFT, cx - inner * 0.5f + 8.0f, y + headH * 0.5f + 3.0f, 1.0f,
                            st->head, rows[i]);
        y += headH;
      } else {
        y += gapH;
      }
      continue;
    }
    mid = y + rowH * 0.5f;
    ink = kinds[i] == 0 ? st->row : st->rowOff;
    if (i == cursor && kinds[i] == 0) {
      card_cursor(cx, mid, inner + 20.0f, rowH + 2.0f, age);
      ink = st->rowOn;
      sc = st->onScale;
    } else {
      sc = 1.0f;
    }
    if (split) {
      mkgp2pc_gameui_text(GAMEUI_FONT_24, GAMEUI_LEFT, cx - inner * 0.5f + 20.0f, mid, sc, ink, rows[i]);
      if (values[i] != NULL) {
        mkgp2pc_gameui_text(GAMEUI_FONT_24, GAMEUI_RIGHT, cx + inner * 0.5f - 12.0f, mid, sc, ink, values[i]);
      }
    } else {
      mkgp2pc_gameui_text(GAMEUI_FONT_24, GAMEUI_CENTER, cx, mid, sc, ink, rows[i]);
    }
    y += rowH;
  }
  if (more_below) {
    mkgp2pc_gameui_sprite(0x8CD, cx, y + 5.0f, 0.16f, 0.16f, 0xFFFFFFFF);
  }
  y = cy + sheetH * 0.5f - 50.0f;
  mkgp2pc_gameui_fill(cx, y - 13.0f, inner + 24.0f, 2.0f, st->line);
  if (note != NULL && note[0]) {
    mkgp2pc_gameui_text(GAMEUI_FONT_16, GAMEUI_CENTER, cx, y, 1.0f, st->note, note);
  }
  if (footer != NULL && footer[0]) {
    mkgp2pc_gameui_text(GAMEUI_FONT_16, GAMEUI_CENTER, cx, y + 20.0f, 1.0f, st->foot, footer);
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
