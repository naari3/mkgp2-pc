/*
 * Stand-in for the OS ROM font. The console takes the font from the boot
 * ROM, which is not on the disc. Here: a font header with plausible metrics
 * and one sheet that holds the ASCII characters, drawn from a free font
 * (os_font_ascii.inc, port/tools/gen_os_font.py). The game uses it for the
 * boot screen that waits for the other boards ("PCB ID : n CHECKING...").
 * Every other character (kana, kanji) is a blank cell so far.
 */
#include <stdint.h>
#include <string.h>

typedef struct {
  uint16_t fontType, firstChar, lastChar, invalChar;
  uint16_t ascent, descent, width, leading;
  uint16_t cellWidth, cellHeight;
  uint32_t sheetSize;
  uint16_t sheetFormat, sheetColumn, sheetRow, sheetWidth, sheetHeight, widthTable;
  uint32_t sheetImage, sheetFullSize;
  uint8_t c0, c1, c2, c3;
} OSFontHeader;

static uint8_t s_sheet[512 * 512 / 2] __attribute__((aligned(32))); /* one I4 sheet */
static const uint8_t s_ascii[95 * 24 * 6] = {
#include "os_font_ascii.inc"
};

/* I4: 8x8 blocks, four bytes a row, the left pixel in the high nibble.
 * Cell 0 stays blank (every character without a glyph); ASCII character c
 * is cell c - 0x20 + 1, 21 cells of 24x24 a row. */
static void ascii_cell(int c, int* x, int* y) {
  int cell = c - 0x20 + 1;
  *x = cell % 21 * 24;
  *y = cell / 21 * 24;
}
static void draw_sheet(void) {
  for (int c = 0x20; c < 0x7F; c++) {
    int cx, cy;
    ascii_cell(c, &cx, &cy);
    for (int row = 0; row < 24; row++) {
      for (int col = 0; col < 12; col++) {
        uint8_t b = s_ascii[((c - 0x20) * 24 + row) * 6 + col / 2];
        uint8_t v = col & 1 ? b & 15 : b >> 4;
        int x = cx + col, y = cy + row;
        uint8_t* p = &s_sheet[((y / 8) * (512 / 8) + x / 8) * 32 + (y % 8) * 4 + (x % 8) / 2];
        *p = x & 1 ? (*p & 0xF0) | v : (*p & 0x0F) | (uint8_t)(v << 4);
      }
    }
  }
}
static OSFontHeader* s_font;

uint16_t OSGetFontEncode(void) { return 1; } /* Shift-JIS, as on a Japanese board */

int OSInitFont(OSFontHeader* font) {
  memset(font, 0, sizeof(*font));
  font->fontType = 1;
  font->firstChar = 0x20;
  font->lastChar = 0xFFFF;
  font->invalChar = 0x20;
  font->ascent = 20;
  font->descent = 4;
  font->width = 24;
  font->leading = 24;
  font->cellWidth = 24;
  font->cellHeight = 24;
  font->sheetSize = sizeof(s_sheet);
  font->sheetFormat = 0; /* I4 */
  font->sheetColumn = 21;
  font->sheetRow = 21;
  font->sheetWidth = 512;
  font->sheetHeight = 512;
  font->c0 = 0x00, font->c1 = 0x55, font->c2 = 0xAA, font->c3 = 0xFF;
  draw_sheet();
  s_font = font;
  return 1;
}

static const char* next_char(const char* s) {
  uint8_t c = (uint8_t)*s;
  if (c == 0) {
    return s;
  }
  if (((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) && s[1]) {
    return s + 2;
  }
  return s + 1;
}

char* OSGetFontTexture(const char* string, void** image, int32_t* x, int32_t* y, int32_t* width) {
  const char* next = next_char(string);
  *image = s_sheet;
  *x = 0;
  *y = 0;
  if (next - string == 1 && (uint8_t)*string >= 0x20 && (uint8_t)*string < 0x7F) {
    int cx, cy;
    ascii_cell((uint8_t)*string, &cx, &cy);
    *x = cx;
    *y = cy;
  }
  if (width) {
    *width = next - string == 2 ? 24 : 12;
  }
  return (char*)next;
}

char* OSGetFontWidth(const char* string, int32_t* width) {
  const char* next = next_char(string);
  if (width) {
    *width = next - string == 2 ? 24 : 12;
  }
  return (char*)next;
}
