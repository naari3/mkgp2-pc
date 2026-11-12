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
Xvfb, real key events): the question after the coin, the hint on the entry
screen, the port's menu (also with `MKGP2PC_ASPECT=16:9`). **Not run on
Windows.** The high-resolution option has not been tried with it.

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
| dark caption bubble | 0x72D, 296x80, `sprite_sel_3_1.tpl` | dark dotted sheet with a white edge and a tail at the right; the game writes on it in white. **The sheet of the port's screens** (without the tail) |
| white windows | 0x72E (248x304, upright), 0x8CC (268x240), 0x731 (472x168); shadow 0x72F | white dotted sheets with a blue edge; the game puts pictures and picture labels on them ("はい" 0x8C3, "いいえ" 0x8C4 are black pictures) |
| window cursor | glow 0x8CE / 0x730 (100x76), striped arrow 0x8CD (68x100) | the glow lies behind the chosen thing, the arrow points at it |
| screen titles | 0x8B0 to 0x8BE, 508x44 | finished pictures ("ゲームモードを選んでください" ...), not text |
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
  screen: the picture darkened, the window coming up as the game's
  message windows do, a title, the items side by side with the cursor
  glow behind the chosen one, and the wheel and pedal hints. Input stays
  with the caller.
- `mkgp2pc_gameui_hint_button(x, y, frame, caption)`: the flashing item
  button with a caption in the 64 pixel font (kana and Latin only).

- `mkgp2pc_gameui_list(title, rows, kinds, values, count, cursor,
  more_above, more_below, note, footer, age)` is a whole screen too: rows
  one under the other on the sheet (choices, pale ones that cannot be taken,
  small headings), optionally a value at the right of a row (then the labels
  stand at the left), marks that the list goes on above or below, the cursor
  on the chosen row, two small lines underneath.
- `mkgp2pc_gameui_white_sheet(x, y, width, height)`: what the port's screens
  write on. `mkgp2pc_gameui_frame(res, w, h, edge, mirror, ...)` draws any frame
  picture at any size without stretching it (corners as they are, sides and
  middle repeated); `mkgp2pc_gameui_cursor` is the glow and the arrow.

### Which frame, and why

The first version (2026-11-12 morning) put the texts on the blue panel of
the name entry screen, stretched to the size needed. The owner, who has the
cabinet: very hard to read, the frame is used badly. What was wrong and
what the rule is now:

- **No frame is stretched.** The panel was drawn at up to twice its height;
  its scan lines and edge went coarse. A frame is now cut into corners,
  sides and middle (`mkgp2pc_gameui_frame`) and the middle is repeated. A
  queued sprite can show a part of its picture: `SpriteHandle_CreateEx`
  returns the handle, whose four screen corners and two texture corners are
  then set (`piece()` in gameui.c).
- **The fonts cannot be made dark.** The 16 and 24 pixel fonts are a white
  (or tinted) body with a black outline, drawn as two textures; a dark tint
  turns a character into a blot, so "black text" is not to be had from
  them.
- The second version (PR 56) therefore used the game's dark caption bubble
  0x72D with a dark fill and white text. The owner: readable, but nothing
  like the game's design. Three variants were shown next to the game's own
  yes/no cards and class selection
  (`/mnt/project-files/port/gameui/menu_variants_linux.png`, not in the
  repository); the owner chose the white window with white outlined text,
  and asked for the window to be half see-through.
- **What it is now** (`kStyle`, `mkgp2pc_gameui_white_sheet`): the white
  dotted window 0x72E of the yes/no cards over its shadow 0x72F, drawn with
  alpha 0xA8 so that the stopped game shows through; text white with its
  outline (the outline is what keeps it readable on the pale window);
  title, headings and rules in the light blue of the game's labels; the
  chosen row yellow, a little larger, on the window glow 0x8CE tinted
  orange, with the striped arrow 0x8CD. `mkgp2pc_gameui_sheet` (the dark
  bubble) is still there but no screen uses it.
- The picture behind is darkened with 0x7F as the game's message windows
  do (`MsgPopup`).

### Over a stopped game

The functions above queue sprites for the frame a running scene draws.
While the game stands still (the port's menu) there is no such frame; the
caller makes its own and uses

```c
mkgp2pc_gameui_overlay_begin();   /* once */
/* per frame: start the frame, draw what lies underneath, then */
mkgp2pc_gameui_list(...);         /* or any of the functions above */
mkgp2pc_gameui_overlay_draw();    /* draws what was queued, empties the queue */
/* end the frame */
mkgp2pc_gameui_overlay_end();     /* once */
```

`overlay_begin` puts aside the sprites the stopped scene has queued for its
next frame (a scene queues in Update what the following Draw shows; they
would be drawn with the overlay and be gone when the game goes on) and
`overlay_end` brings them back. The overlay has its own set of loaded
texts and pictures, so it may come up while a scene's port screen is open.
`port/platform/ui/gamemenu.c` is the user: see [`menu.md`](menu.md), "How
the menu is drawn".

## Where it is used

- The question after the coin (only with `MKGP2PC_LINK_MODE=ask` since the
  mode is chosen in the launcher): `mkgp2pc_linkflow_draw`
  (`port/platform/triforce/linkflow.c`), called from `clFlowTitle::Draw`
  (`port/patches/src/game/fn_801B785C.cpp.patch`).
- The port's menu (Esc): `mkgp2pc_gamemenu_frame`
  (`port/platform/ui/gamemenu.c`), called from the top of `MainGameLoop`
  (`MainGameLoop.cpp.patch`).
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
