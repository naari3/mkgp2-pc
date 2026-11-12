/* See port_ui.hpp. */
#include "port_ui.hpp"

#include <aurora/aurora.h>
#include <aurora/imgui.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace portui {
namespace {

bool g_japanese = false;
ImFont* g_font = nullptr;
float g_fontSize = 18.f;

std::string& announced() {
  static std::string text;
  return text;
}

/*
 * A font with Japanese letters. The port carries none (and must not carry
 * the game's): the system's are tried, first the file MKGP2PC_UI_FONT names.
 * Without one the texts are English in ImGui's built-in font.
 */
std::string find_font() {
  std::vector<std::string> names;
  if (const char* env = std::getenv("MKGP2PC_UI_FONT"); env != nullptr && *env != 0) {
    names.emplace_back(env);
  }
#ifdef _WIN32
  const char* win = std::getenv("WINDIR");
  std::string dir = std::string(win != nullptr && *win != 0 ? win : "C:\\Windows") + "\\Fonts\\";
  for (const char* n : {"meiryo.ttc", "YuGothM.ttc", "msgothic.ttc"}) {
    names.push_back(dir + n);
  }
#else
  for (const char* n : {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
                        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
                        "/usr/share/fonts/opentype/ipafont-gothic/ipag.ttf",
                        "/usr/share/fonts/truetype/fonts-japanese-gothic.ttf",
                        "/System/Library/Fonts/ヒラギノ角ゴシック W4.ttc"}) {
    names.emplace_back(n);
  }
#endif
  for (const auto& n : names) {
    if (SDL_GetPathInfo(n.c_str(), nullptr)) {
      return n;
    }
  }
  return {};
}

} // namespace

const char* Text::get() const { return g_japanese && ja != nullptr && *ja != 0 ? ja : en; }
const char* Text::get_game() const {
  static const bool english = std::getenv("MKGP2PC_UI_ENGLISH") != nullptr;
  if (english) {
    return en;
  }
  return game != nullptr ? game : (ja != nullptr && *ja != 0 ? ja : en);
}
bool japanese() { return g_japanese; }
void announce(const char* utf8) {
  if (utf8 != nullptr) {
    announced() += utf8;
  }
}

Item::Item(Text label_, std::function<void()> act_, std::function<bool()> enabled_, Text note_)
: label(label_), act(std::move(act_)), enabled(std::move(enabled_)), note(note_) {
  announce(label.ja);
  announce(note.ja);
}

Item Item::Sub(Text label, std::function<void()> act, std::function<bool()> enabled, Text note,
               std::function<Text()> value) {
  Item it(label, std::move(act), std::move(enabled), note);
  it.stays = true;
  it.value = std::move(value);
  return it;
}

Item Item::Setting(Text label, std::function<Text()> value, std::function<void(int)> adjust,
                   std::function<bool()> enabled, Text note) {
  Item it(label, [] {}, std::move(enabled), note);
  it.value = std::move(value);
  it.adjust = std::move(adjust);
  it.stays = true;
  return it;
}

ListMenu::ListMenu(Text title, std::vector<Item> items, Text footer)
: m_title(title), m_footer(footer), m_items(std::move(items)) {
  announce(title.ja);
  announce(footer.ja);
}

bool ListMenu::selectable(int i) const {
  const Item& it = m_items[static_cast<size_t>(i)];
  return it.act && (!it.enabled || it.enabled());
}

void ListMenu::reset() {
  m_cursor = 0;
  m_top = 0;
  if (!selectable(0)) {
    move(1);
  }
}

void ListMenu::move(int dir) {
  const int n = static_cast<int>(m_items.size());
  for (int step = 0; step < n; step++) {
    m_cursor = (m_cursor + dir + n) % n;
    if (selectable(m_cursor)) {
      return;
    }
  }
}

Result ListMenu::key(Key k) {
  const bool on = selectable(m_cursor);
  Item* it = on ? &m_items[static_cast<size_t>(m_cursor)] : nullptr;
  switch (k) {
  case Key::Up:
    move(-1);
    return Result::Stay;
  case Key::Down:
    move(1);
    return Result::Stay;
  case Key::Left:
  case Key::Right:
    if (it != nullptr && it->adjust) {
      it->adjust(k == Key::Left ? -1 : 1);
    }
    return Result::Stay;
  case Key::Decide:
    if (it == nullptr) {
      return Result::Stay;
    }
    if (it->adjust) {
      it->adjust(1);
      return Result::Stay;
    }
    it->act();
    return it->stays ? Result::Stay : Result::Close;
  case Key::Cancel:
    return Result::Back;
  default:
    return Result::Stay;
  }
}

void ListMenu::window(int* first, int* count) const {
  const int n = static_cast<int>(m_items.size());
  if (n <= ROWS_SHOWN) {
    m_top = 0;
  } else {
    if (m_cursor < m_top) {
      m_top = m_cursor;
      /* a heading above the first choice it stands for comes along */
      while (m_top > 0 && !m_items[static_cast<size_t>(m_top - 1)].act) {
        m_top--;
      }
    }
    if (m_cursor >= m_top + ROWS_SHOWN) {
      m_top = m_cursor - ROWS_SHOWN + 1;
    }
    if (m_top > n - ROWS_SHOWN) {
      m_top = n - ROWS_SHOWN;
    }
  }
  *first = m_top;
  *count = n - m_top < ROWS_SHOWN ? n - m_top : static_cast<int>(ROWS_SHOWN);
}

int ListMenu::rows(const char** text, int* kind, const char** value, int max, int* moreAbove, int* moreBelow) const {
  int first, count, n = 0;
  window(&first, &count);
  for (int i = first; i < first + count && n < max; i++, n++) {
    const Item& it = m_items[static_cast<size_t>(i)];
    text[n] = it.label.get_game();
    kind[n] = !it.act ? 2 : (selectable(i) ? 0 : 1);
    value[n] = it.value ? it.value().get_game() : nullptr;
  }
  *moreAbove = first > 0;
  *moreBelow = first + count < static_cast<int>(m_items.size());
  return n;
}

const char* ListMenu::note() const {
  return selectable(m_cursor) ? m_items[static_cast<size_t>(m_cursor)].note.get_game() : "";
}

void ListMenu::draw() const {
  if (ImGui::GetCurrentContext() == nullptr) {
    return;
  }
  const ImGuiIO& io = ImGui::GetIO();
  const ImVec2 screen = io.DisplaySize;
  if (g_font != nullptr) {
    ImGui::PushFont(g_font);
  }
  /* the built-in font has one size: it is magnified instead */
  const float scale = g_font != nullptr ? 1.f : (screen.y / 480.f);
  ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), screen, IM_COL32(0, 0, 0, 150));
  ImGui::SetNextWindowPos(ImVec2(screen.x * .5f, screen.y * .5f), ImGuiCond_Always, ImVec2(.5f, .5f));
  const float pad = 14.f * screen.y / 480.f;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad * 1.4f, pad));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, pad * .5f);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(pad * .5f, pad * .35f));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(16, 24, 48, 235));
  ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(255, 255, 255, 90));
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                 ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoFocusOnAppearing;
  if (ImGui::Begin("##portui-list", nullptr, flags)) {
    ImGui::SetWindowFontScale(scale);
    ImGui::TextColored(ImVec4(1.f, .85f, .3f, 1.f), "%s", m_title.get());
    ImGui::Separator();
    const float mark = ImGui::CalcTextSize("> ").x;
    int first, count;
    window(&first, &count);
    /* the widest row decides the panel's width, so that scrolling does not change it */
    float labelW = 0, valueW = 0;
    for (const Item& it : m_items) {
      labelW = std::max(labelW, ImGui::CalcTextSize(it.label.get()).x);
      if (it.value) {
        valueW = std::max(valueW, ImGui::CalcTextSize(it.value().get()).x + ImGui::CalcTextSize("< >").x);
      }
    }
    if (valueW > 0) {
      ImGui::Dummy(ImVec2(mark + labelW + pad * 2.f + valueW, 0));
    }
    if (first > 0) {
      ImGui::TextColored(ImVec4(1, 1, 1, .5f), "  ...");
    }
    for (int i = first; i < first + count; i++) {
      const Item& it = m_items[static_cast<size_t>(i)];
      if (!it.act) { /* heading */
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(.6f, .75f, 1.f, 1.f), "%s", it.label.get());
        continue;
      }
      const bool on = selectable(i);
      const bool cur = i == m_cursor && on;
      if (cur) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const float h = ImGui::GetTextLineHeight();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x - pad * .4f, p.y - pad * .12f),
                                                  ImVec2(p.x + w + pad * .4f, p.y + h + pad * .12f),
                                                  IM_COL32(70, 110, 220, 255), pad * .25f);
        ImGui::TextUnformatted(">");
        ImGui::SameLine(0, 0);
      }
      ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + mark);
      ImGui::TextColored(on ? ImVec4(1, 1, 1, 1) : ImVec4(1, 1, 1, .35f), "%s", it.label.get());
      if (it.value) {
        const char* v = it.value().get();
        const bool arrows = cur && static_cast<bool>(it.adjust);
        char text[160];
        std::snprintf(text, sizeof(text), arrows ? "< %s >" : "%s", v);
        ImGui::SameLine(ImGui::GetStyle().WindowPadding.x + mark + labelW + pad * 2.f + valueW -
                        ImGui::CalcTextSize(text).x);
        ImGui::TextColored(on ? ImVec4(1.f, .92f, .6f, 1.f) : ImVec4(1, 1, 1, .35f), "%s", text);
      }
    }
    if (first + count < static_cast<int>(m_items.size())) {
      ImGui::TextColored(ImVec4(1, 1, 1, .5f), "  ...");
    }
    const Text* note = selectable(m_cursor) ? &m_items[static_cast<size_t>(m_cursor)].note : nullptr;
    if ((note != nullptr && *note->get() != 0) || *m_footer.get() != 0) {
      ImGui::Spacing();
      ImGui::Separator();
      /* the line is always there, so that the panel keeps its size */
      ImGui::TextColored(ImVec4(1, 1, 1, .8f), "%s", note != nullptr && *note->get() != 0 ? note->get() : " ");
      if (*m_footer.get() != 0) {
        ImGui::TextColored(ImVec4(1, 1, 1, .5f), "%s", m_footer.get());
      }
    }
  }
  ImGui::End();
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(3);
  if (g_font != nullptr) {
    ImGui::PopFont();
  }
}

} // namespace portui

/* Aurora calls this once, when its ImGui context exists and before the
 * font image is made (AuroraConfig::imGuiInitCallback). */
extern "C" void mkgp2pc_ui_init(const AuroraWindowSize* size) {
  using namespace portui;
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr; /* nothing of ours is worth a settings file */
  io.Fonts->AddFontDefault();
  if (std::getenv("MKGP2PC_UI_ENGLISH") != nullptr) {
    return;
  }
  const std::string path = find_font();
  if (path.empty()) {
    std::fprintf(stderr, "ui: no Japanese font found, the port's screens are in English (MKGP2PC_UI_FONT names one)\n");
    return;
  }
  static ImVector<ImWchar> ranges; /* has to outlive the font image's making */
  ImFontGlyphRangesBuilder builder;
  builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
  builder.AddText(announced().c_str());
  builder.BuildRanges(&ranges);
  const uint32_t h = size != nullptr && size->fb_height != 0 ? size->fb_height : 480;
  g_fontSize = 17.f * static_cast<float>(h) / 480.f;
  if (g_fontSize < 13.f) {
    g_fontSize = 13.f;
  }
  g_font = io.Fonts->AddFontFromFileTTF(path.c_str(), g_fontSize, nullptr, ranges.Data);
  g_japanese = g_font != nullptr;
  std::fprintf(stderr, "ui: font %s %s\n", path.c_str(), g_japanese ? "loaded" : "could not be read");
}
