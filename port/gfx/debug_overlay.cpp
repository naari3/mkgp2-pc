// The game's OverlayText character grid, drawn over the picture
// (docs/port/debug.md). The retail build formats the strings and drops
// them; port/platform/debug/debug.c keeps those of the current frame.
//
// The original drawing code is not in the retail program, so the cell size
// is an assumption: 8 by 16 pixels of the 640 by 480 picture (80 columns,
// 30 rows), which fits every caller found: rows up to 23, the developers'
// scene list starting at row 6, 40-column lines of the kart physics page.
#include <imgui.h>

#include <cfloat>

extern "C" {
// port/platform/debug/debug.c (mkgp2pc/debug.h)
int mkgp2pc_debug_overlay_count(void);
int mkgp2pc_debug_overlay_get(int index, int* layer, int* column, int* row, const char** text);
}

extern "C" void mkgp2pc_debug_overlay_draw(void) {
  const int count = mkgp2pc_debug_overlay_count();
  if (count == 0 || ImGui::GetCurrentContext() == nullptr) {
    return;
  }
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  if (size.x <= 0.f || size.y <= 0.f) {
    return;
  }
  // the game's picture is 4:3 and centred when the window is wider
  float width = size.x;
  if (width > size.y * 4.f / 3.f) {
    width = size.y * 4.f / 3.f;
  }
  const float left = (size.x - width) * .5f;
  const float cellW = width / 80.f;
  const float cellH = size.y / 30.f;
  ImFont* font = ImGui::GetIO().Fonts->Fonts.empty() ? nullptr : ImGui::GetIO().Fonts->Fonts[0];
  if (font == nullptr) {
    return;
  }
  // font index 0 is ImGui's fixed-width ASCII font: 7 wide at size 13
  float fontSize = cellW * 13.f / 7.f;
  if (fontSize > cellH) {
    fontSize = cellH;
  }
  ImDrawList* list = ImGui::GetBackgroundDrawList();
  for (int i = 0; i < count; i++) {
    int layer, column, row;
    const char* text;
    if (!mkgp2pc_debug_overlay_get(i, &layer, &column, &row, &text)) {
      break;
    }
    const ImVec2 pos(left + column * cellW, row * cellH);
    // readable on any picture: a dark band behind the string
    const ImVec2 extent = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, text);
    list->AddRectFilled(ImVec2(pos.x - 1.f, pos.y), ImVec2(pos.x + extent.x + 1.f, pos.y + cellH), IM_COL32(0, 0, 0, 150));
    list->AddText(font, fontSize, pos, IM_COL32(255, 255, 255, 255), text);
  }
}
