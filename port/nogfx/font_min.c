/*
 * OS ROM font for the bring-up executable: a font header with plausible
 * metrics and blank glyphs, so that the text code runs. The console takes the
 * font from the IPL ROM, which is not on the disc; the shipped build needs a
 * real replacement (decision pending: a dump supplied by the user, or a free
 * font converted to the same sheet layout).
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

static uint8_t s_sheet[512 * 512 / 2] __attribute__((aligned(32))); /* one blank I4 sheet */
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
