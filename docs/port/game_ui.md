# Port screens drawn with the game's own fonts and panels

Screens the port adds (the question after the coin, hints, later the menu,
the room name entry) are drawn with what the game draws its own menus with:
its text fonts, its panels and cursors, its control hints. The code is
`port/platform/ui/gameui.c` (`mkgp2pc/gameui.h`), plain C, used from the
game's frame.

Nothing is read from files by the port and no picture is in the repository.
The textures come from the disc through the game's resource loader at run
time, so the high-resolution option (`MKGP2PC_HIRES`,
[`hires_textures.md`](hires_textures.md)) replaces them like the game's own.

State 2026-11-12: written and tried on 32-bit Linux only (software GL under
Xvfb, real key events): the question after the coin and the hint on the
entry screen. **Not run on Windows.** `MKGP2PC_ASPECT=16:9` and the
high-resolution option have not been tried with it.

## What the game has

Found in the decompiled sources and on the disc (resource numbers index
`kResourceTableMain`; the files are named in `kResourcePathTable`):

| Thing | Where | Notes |
| --- | --- | --- |
| 16 pixel text font | `tpl2_sysFONT_16_*`, table `lbl_804E5590` | one texture per character and one for its outline; menu descriptions |
| 24 pixel text font | `tpl2_sysFONT_24_*`, same table | the keys of the name entry screen, the captions of the card questions |
| 64 pixel font | `tpl2_sysFONT_64_*`, third column of the table | white with a black outline; the name being typed on the name entry screen. Kana, Latin letters, digits and signs only (252 characters), no kanji |
| character set | `lbl_804E5590`, 739 entries | 654 full-width (kana, Latin, signs, about 450 kanji) and 85 ASCII. The ASCII entries name the same textures as the full-width ones |
| text object | `TextManager` (`TexMgr_Init`, `TexMgr_SetTexture`, `TexMgr_Apply`, `fn_801F8A34`) | holds up to 255 character numbers, position, scale, colour; drawing queues one sprite per character |
| blue panel | 0x6F0, 588x212, `sprite_sel_2_1.tpl` | the key field of the name entry screen; see-through |
| cursor glow | 0x715, 64x64, same file | behind the chosen key of the name entry screen |
| message window | 0x731 (472x168) over shadow 0x72F, `sprite_sel_3_1.tpl` | `MsgPopup`; white dotted sheet with a blue edge |
| steering wheel hint | 0x89B to 0x8A1, caption "選択" 0x8A2, `sprite_sel_kihon.tpl` | `clMnHandleTask`; frame table `lbl_803C0074` |
| pedal hint | 0x883 to 0x88B, captions "決定" 0x88D, "キャンセル" 0x88C, "スキップ" 0x88E, "←消す" 0x8A7, "確認終了" 0x88F, "1つ戻る" 0x890, "ページ切替え" 0x8A8 | `clMnPedalSprTask`; frame table `lbl_803C00DC` |
| item button | 0x896 (dark) to 0x899 (lit) | no caption picture of its own |

How the game's sprite system takes them (`src/game/SpriteHandle.cpp`):

- A scene fills in a `ResCtrl` (resource number, centre position, scale,
  four corner colours 0xRRGGBBAA) and queues it with `fn_8011FAFC(ctrl,
  layer)`, every frame. There are eight layers, 0 to 7; **after the scene's
  own Draw the game draws all queued layers in order**, so what is queued in
  layer 7 during Update or Draw lies on top. (Drawing a layer oneself with
  `fn_8011F994` uses the queue up before the scene's background is drawn and
  the sprites end up underneath: seen on the entry screen.)
- A resource has to be loaded: `PreloadResource(n)` takes a reference,
  `fn_80120A08(n)` gives it back. `TexMgr_Apply` does the first for every
  character of a text, `fn_801F9518` / `fn_801F95A0` the second (24 / 16
  pixel).
- `ResCtrl.mode = 2` is a plain rectangle of `width` x `height` in the
  corner colours.
- The function the decompilation calls `isJapanese` returns 0 on this
  (Japanese) program; the text drawing then advances a full cell per
  character (16 or 24). The other value selects the `E_` resources and a
  narrower spacing.

## Using it

```c
#include "mkgp2pc/gameui.h"

mkgp2pc_gameui_begin(7);                 /* once, when the screen opens */
/* every frame, from the scene's Update or Draw: */
mkgp2pc_gameui_dim(0x90);
mkgp2pc_gameui_panel(320, 228, 548, 196, 0xFFFFFFFF);
mkgp2pc_gameui_text(GAMEUI_FONT_24, GAMEUI_CENTER, 320, 178, 1.0f, 0xFFFFFFFF, "遊び方を選んでください");
mkgp2pc_gameui_hint_select(224, 452, frame);
mkgp2pc_gameui_end();                    /* once, when it closes */
```

- Coordinates are the game's 640x480 screen. Sprites are placed by their
  centre, text by the centre of its first character, or with `GAMEUI_CENTER`
  / `GAMEUI_RIGHT` by the middle / the end of the line.
- Text is UTF-8. `gameui_chars.h` lists the Unicode character of every entry
  of the game's table (made by `port/tools/gen_gameui_chars.py` from the
  Shift-JIS codes in `src/game/autodata/data_804E549C.c`); a character the
  font does not have is drawn as a blank. ASCII the half-width set lacks
  falls back to the full-width form.
- A text is kept between frames (up to 32 different ones per screen, 127
  bytes each), so its characters are loaded once. `begin` / `end` bracket
  what is loaded; one screen at a time.
- `mkgp2pc_gameui_choice(title, items, count, cursor, age)` is a whole
  screen: the picture darkened, the blue panel coming up as the game's
  message windows do, a title, the items side by side with the name entry
  cursor behind the chosen one, and the wheel and pedal hints. Input stays
  with the caller.
- `mkgp2pc_gameui_hint_button(x, y, frame, caption)`: the flashing item
  button with a caption in the 64 pixel font (kana and Latin only).

A scene that is not the game's own (the port's Esc menu is drawn by Dear
ImGui over the finished picture, `port/gfx/port_ui.hpp` on its branch) can
only use this while the game's frame runs and its sprite system draws, that
is, not while the game is paused.

## Where it is used

- The question after the coin: `mkgp2pc_linkflow_draw`
  (`port/platform/triforce/linkflow.c`), called from `clFlowTitle::Draw`
  (`port/patches/src/game/fn_801B785C.cpp.patch`).
- The entry screen of a network game: button and "スタート"
  (`mkgp2pc_linkflow_lobby_draw`, from `clFlowEntry::Draw`,
  `fn_801BA318.cpp.patch`). The count of players and "しばらくお待ちください"
  on that screen are the game's own.

## Found on the way

- `InputPlayer::IsPressed(mask)` as the decompilation names it is "is
  down", and `IsHeld(mask)` is the moment of going down. The question after
  the coin took the pedal's state as its answer, so a pedal held a quarter
  of a second past the coin answered it unseen; it now wants the pedal up
  first (`mkgp2pc_linkflow_choose`).
- String literals in the game sources are UTF-8 in the PC build (the
  matching build converts them to Shift-JIS with sjiswrap). The game's text
  drawing looks characters up by Shift-JIS code, so every Japanese text the
  game sets from a literal (`TexMgr_SetTexture(..., "...")`, the message
  window texts of `anon_80202BCC_nm1.cpp`) would not be found on the PC.
  Seen in the compiled unit (`lbl_803C0390` is 13 bytes of UTF-8, 9 in the
  original); not looked at on a screen and not fixed here.
