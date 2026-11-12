/*
 * Screens of the port's own, drawn over the game's picture with Dear ImGui
 * (Aurora brings it along and draws it after the game's frame).
 *
 * What is here is meant to be used by more than one screen:
 *   portui::Text      a text in English and Japanese; the Japanese one is
 *                     shown when a font with those letters could be loaded
 *   portui::ListMenu  a list of choices with a cursor, moved with the keys
 *                     the caller passes on; draws itself as a panel in the
 *                     middle of the window
 * The first user is the navigation menu in port_menu.cpp.
 */
#ifndef MKGP2PC_PORT_UI_HPP
#define MKGP2PC_PORT_UI_HPP

#include <functional>
#include <string>
#include <vector>

namespace portui {

struct Text {
  const char* en = "";
  const char* ja = ""; /* UTF-8 */
  const char* get() const;
};

/* true when Japanese texts are shown */
bool japanese();
/* Every Japanese text has to be announced before the font is built (the
 * font holds only the letters that are used). Text literals of ListMenu
 * items are announced by their constructor when they are static objects;
 * call this for any other. */
void announce(const char* utf8);

enum class Key { None, Up, Down, Decide, Cancel };

struct Item {
  Text label;
  std::function<void()> act;          /* empty: a heading, the cursor passes over it */
  std::function<bool()> enabled = {}; /* empty: always */
  Text note = {};                     /* shown under the list while the cursor is on the item */
  Item(Text label, std::function<void()> act, std::function<bool()> enabled = {}, Text note = {});
};

class ListMenu {
public:
  ListMenu(Text title, std::vector<Item> items, Text footer = {});
  /* cursor to the first choice that can be taken */
  void reset();
  /* returns true when the key closed the menu (Cancel, or a choice was taken) */
  bool key(Key k);
  void draw() const;

private:
  bool selectable(int i) const;
  void move(int dir);
  Text m_title, m_footer;
  std::vector<Item> m_items;
  int m_cursor = 0;
};

} // namespace portui

#endif
