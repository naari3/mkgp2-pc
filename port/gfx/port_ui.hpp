/*
 * Screens of the port's own, drawn over the game's picture with Dear ImGui
 * (Aurora brings it along and draws it after the game's frame).
 *
 * What is here is meant to be used by more than one screen:
 *   portui::Text      a text in English and Japanese; the Japanese one is
 *                     shown when a font with those letters could be loaded
 *   portui::ListMenu  a list of choices with a cursor, moved with the keys
 *                     the caller passes on; draws itself as a panel in the
 *                     middle of the window. A row may show a value at its
 *                     right that Left / Right change (a setting), or stay
 *                     open when taken (it opens another list); a list
 *                     longer than ROWS_SHOWN scrolls.
 * The users are the menu and its settings pages in port_menu.cpp.
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
  /* Japanese for the game's own font (port/platform/ui/gameui.c), where
   * `ja` has letters that font lacks (it has about 450 kanji); nullptr: `ja` */
  const char* game = nullptr;
  const char* get() const;
  /* the text for a screen drawn with the game's font */
  const char* get_game() const;
};

/* true when Japanese texts are shown */
bool japanese();
/* Every Japanese text has to be announced before the font is built (the
 * font holds only the letters that are used). Text literals of ListMenu
 * items are announced by their constructor when they are static objects;
 * call this for any other. */
void announce(const char* utf8);

enum class Key { None, Up, Down, Decide, Cancel, Left, Right };
/* what a key did to a list */
enum class Result { Stay, Close, Back };

struct Item {
  Text label;
  std::function<void()> act;          /* empty: a heading, the cursor passes over it */
  std::function<bool()> enabled = {}; /* empty: always */
  Text note = {};                     /* shown under the list while the cursor is on the item */
  /* A setting: `value` is shown at the right of the row, `adjust` gets -1
   * for Left and +1 for Right and Decide. Such a row never closes the list. */
  std::function<Text()> value = {};
  std::function<void(int)> adjust = {};
  bool stays = false; /* taking the row leaves the list open (it opens another one) */
  Item(Text label, std::function<void()> act, std::function<bool()> enabled = {}, Text note = {});
  /* a row that opens something and leaves the list open */
  static Item Sub(Text label, std::function<void()> act, std::function<bool()> enabled = {}, Text note = {},
                  std::function<Text()> value = {});
  static Item Setting(Text label, std::function<Text()> value, std::function<void(int)> adjust,
                      std::function<bool()> enabled = {}, Text note = {});
};

class ListMenu {
public:
  ListMenu(Text title, std::vector<Item> items, Text footer = {});
  /* cursor to the first choice that can be taken */
  void reset();
  /* Close: a choice was taken that ends the menu. Back: Cancel. */
  Result key(Key k);
  void draw() const;
  /* For a drawing of another kind (the game's own sprites, port_menu.cpp):
   * what the list shows now. kind: 0 a choice, 1 a choice that cannot be
   * taken, 2 a heading. Returns the number of rows. */
  int rows(const char** text, int* kind, const char** value, int max, int* moreAbove, int* moreBelow) const;
  void set_title(Text title) { m_title = title; }
  enum { ROWS_SHOWN = 10 };
  /* the cursor's row among the rows that rows() gave */
  int cursor() const { return m_cursor - m_top; }
  const char* title() const { return m_title.get_game(); }
  const char* footer() const { return m_footer.get_game(); }
  const char* note() const;

private:
  bool selectable(int i) const;
  void move(int dir);
  /* the rows on screen: [first, first + count) */
  void window(int* first, int* count) const;
  Text m_title, m_footer;
  std::vector<Item> m_items;
  int m_cursor = 0;
  mutable int m_top = 0;
};

/* port_menu.cpp: puts a list over the one shown; Cancel in it goes back */
void open_list(ListMenu& m);
/* port_settings_menu.cpp: 設定 */
ListMenu& settings_menu();

} // namespace portui

#endif
