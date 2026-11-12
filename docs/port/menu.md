# The port's menu and "one screen back"

Two things the cabinet does not have, added on the port side (2026-11-12).
Checked on 32-bit Linux only (Xvfb, keys sent with xdotool); not yet run on
Windows.

## What the player gets

| Key (default) | Action name in `MKGP2PC_KEYS` | Does |
|---|---|---|
| `Esc` | `menu` | opens the port's menu; the game stands still while it is open |
| `Backspace` | `back` | in the selection screens before a race: back to the screen before |

Both can be given other keys in the settings program
(`mkgp2-launcher.exe`, rows メニュー and 一つ前の画面に戻る) or with
`MKGP2PC_KEYS` (docs/port/launcher.md). The settings program cannot assign
`Esc` itself (it is its "cancel" while waiting for a key); the default stays
`Esc` unless another key is chosen.

Inside the menu: up / down (the arrow keys, or the keys of accelerator and
brake) move the cursor, `Enter` (or the item key) takes the choice, `Esc`
(or the menu or back key) closes it.

| Choice | Does |
|---|---|
| ゲームに戻る / Resume | closes the menu |
| 一つ前の画面に戻る / Back one screen | the same as the back key; grey outside the selection screens |
| このレースをやり直す / Restart this race | during a race: the race is built again from the current choices and begins with the course introduction |
| タイトル / Title | ends the play and goes to the attract loop (title, demo, ranking) |
| クラス選択 / Class selection | the class selection, as the game shows it after "continue" |
| キャラクター選択 / Character selection | the character selection |
| カップ・ラウンド選択 / Cup / round selection | the cup selection; the round selection while a cup is being played (the game's own rule) |
| 開発用シーン一覧 / Developer scene list | the game's own list of its 20 flows (index -4 of the flow table), with the debug display switched on so that its text shows (docs/port/debug.md). In the list F2 (service) moves the cursor and Space (item) chooses. Only the list itself and the way back to the title were tried; the developers' scenes behind it may not work |
| ゲームを終了する / Quit the game | closes the program |

The four destinations take no coin and do not look at the credits. They are
grey before the game's main loop runs (the boot checks) and in loops of the
game's own (test mode).

The following is about the plain drawing (see "How the menu is drawn").
The texts are Japanese when a font with Japanese letters is found on the
machine (Windows: `meiryo.ttc`, `YuGothM.ttc`, `msgothic.ttc` in the Fonts
folder; Linux: Noto Sans CJK or IPA Gothic; `MKGP2PC_UI_FONT=<file>` names
another one), English in Dear ImGui's built-in font otherwise.
`MKGP2PC_UI_ENGLISH=1` asks for English. The port ships no font.

## Settings in the menu (設定)

Added 2026-11-12 at the user's wish: some of the settings program's
settings can be changed from the menu while the game runs. Checked on 32-bit
Linux only (Xvfb, real key events: every row changed, the file read back,
a key captured); not run on Windows.

    設定
      グラフィック
      キー入力

The rows are those of the settings program's 表示 and キー pages
(launcher.md), except three the user wanted left in the settings program:
making the textures (作成...), starting at full speed, and the coin tally in
English. The network, card, controller and ISO settings stay there too.

| row (グラフィック) | ini key | takes effect |
|---|---|---|
| 画面の比率 4:3 / 16:9 | `wide` (and `scale`, moved to the nearest size on offer) | next start |
| 画面の大きさ (the settings program's six sizes for the aspect) | `scale` | next start |
| レース中のUIの位置: 4:3の位置 / 左右のはし (widescreen.md); grey in a 4:3 picture | `wide_hud` | at once |
| レンダリング: 自動, D3D12, D3D11, Vulkan, OpenGL | `backend` | next start |
| アンチエイリアス: なし, MSAA 4x, SSAA 2x, both | `aa` | next start |
| なめらかな影 | `shadow_smooth` | at once |
| ハイレゾテクスチャ (stays off when there is no folder `hires`) | `hires` | at once |
| シェーダーを待たない | `async_pipelines` | at once |
| ウィンドウのリサイズ (unstable on Windows, build.md) | `resizable` | next start |

Left / Right (or Enter) change a row. A value that is not in force yet has a
`*` behind it and the row's note says "* は次回の起動から変わります". Window
size, aspect, backend and anti-aliasing are given to the rendering library
when the window is made; changing the window's size in a running game is
what crashed on Windows (build.md), so none of them is tried live.

キー入力 lists the 24 actions with their two keys. Enter on an action opens
its page: キー 1 / キー 2 (Enter, then the new key; Esc leaves it), and
removing either. A key given to one action is taken from the action that
had it, as in the settings program. The change holds at once. Esc cannot be
given to an action here (it leaves the capture), and the `menu` action
always keeps a key: when its last one is taken away it gets Esc back.
"すべて初期設定にもどす" puts every action back to the built-in keys.

Where it is kept: the settings program names its file to the game as
`MKGP2PC_INI` (launcher.md). Each change is written into that file at once
(`port/gfx/port_settings.cpp`: only the lines concerned are replaced or
added, the file keeps its UTF-16 encoding and everything else in it). A game
started without the settings program has no file: live rows still work for
that run, the rows that need a new start are grey, and a line at the top of
the page says that nothing is kept.

The pages are lists of `portui::ListMenu` like the menu itself
(`port/gfx/port_settings_menu.cpp`): a row can show a value at its right, be
changed with Left / Right, or open another list; Esc goes one list back. A
list longer than eleven rows scrolls. Another module can add a row to
グラフィック with `mkgp2pc_settings_add_graphics` (`port/gfx/port_settings.h`).

The game font has about 450 kanji, so some rows are worded differently from
the settings program (no 解 像 描 窓 許 戻 端); the plain menu
(`MKGP2PC_MENU=plain`) shows the settings program's wording.

Also fixed here: a controller button pressed while the menu was open read a
key event that was not there (null pointer). Controller buttons now move
the menu like the keys of the same actions.

## One screen back

The selection screens between the coin and the race are children of one of
the game's top-level flows (the game calls it "kart2"). Its update function,
`MenuScene_Dispatch` (0x801B8EC0), calls the current screen's update; that
returns -1 while the screen stays and the number of the next screen when it
is done, and the dispatcher deletes the screen and builds the next one from
the number. Every screen builds itself from the choices made so far, which
are globals. Going back is therefore the same operation with the number of
the screen before, and needs nothing from the screens themselves.

`port/platform/nav/nav.c` keeps the numbers that went by on a stack;
`port/patches/src/game/fn_801B7EF8.c.patch` passes the dispatcher's value
through `mkgp2pc_nav_scene_hook()` and tells from the flow's constructor
which screen the flow begins with (`mkgp2pc_nav_scene_begin()`).

Screens seen in a play without a card, with their numbers:

    9 card -> 11 class -> 13 character -> 19 photo frame -> 21 cup -> 27 round
      -> 37 "items used in this race" -> 42 (the flow ends, the race follows)

With a card the item selection (33) comes in as well.

- One can go back from and to: 11, 13, 21, 23 (course), 27, 33, 37, 38 (the
  time attack counterpart of 37). This is the list of the owner's earlier
  patch for the original program
  ([naari3/mkgp2-patch](https://github.com/naari3/mkgp2-patch),
  `features/brake_back`), which put the operation on the brake pedal.
- Screens outside the list are passed over: back from the cup selection
  goes to the character selection, not to the photo frame.
- Nothing lies before the first screen of a visit: no going back to the
  card screens or the attract loop (the menu's "Title" does that).
- The item selection writes its three choices to the player record when it
  is deleted, also when nothing was chosen yet (they are -1 then), and the
  record goes to the card. `port/patches/src/game/ItemSelectState.cpp.patch`
  skips that write for an unchosen selection. The earlier patch did the same
  with a branch over the six stores.
- A screen that is gone back to starts afresh: its timer begins again and
  its cursor is where the game puts it for the choices in force.

Not checked: the item selection (needs a card), time attack, a linked play
(several cabinets). In a linked play the screens are kept in step between
the cabinets; going back on one of them alone is likely to confuse the
others, so leave it alone there.

## Going to another flow

The game's top level is a table of 20 flows (`main_rodata_80328C94`:
`Title`, `ModeSelect`, `Game`, `kart2`, `GameMain`, ... the names are the
game's own) and `MainGameLoop`, which runs the current one and calls
`Flow_TransitionTo(dispatcher, index)` when its update returns something
else than -2. The cabinet only ever alternates between `kart2` (3: attract
loop, coin, selection screens) and `GameMain` (4: the race), plus the test
mode (12).

`port/patches/src/game/MainGameLoop.cpp.patch` asks
`mkgp2pc_nav_flow_hook()` at the top of every frame whether another flow is
wanted and, if so, calls `Flow_TransitionTo` itself. The menu flow's
constructor chooses its first screen from three flags that the game sets
before a "continue" (`lbl_806D17F0..2`); the patch sets them from the
request:

| menu choice | request | flags F0 F1 F2 | first screen |
|---|---|---|---|
| Title | `flow=3,0` | 0 0 0 | entry (attract loop) |
| Class selection | `flow=3,1` | 1 1 0 | 11 |
| Cup / round selection | `flow=3,2` | 1 0 1 | 21, or 27 while a cup is unfinished |
| Character selection | `flow=3,3` | 1 0 0 | 13 |
| Restart this race | `flow=4,-1` | unchanged | |

Two things had to be right (both found as crashes inside the rendering
library's command reader, with drawing commands read from freed memory):

- The change is made at the top of the frame, before anything is drawn, not
  after the flow's update where the game makes its own: the flow that is
  deleted has drawn into the current frame by then.
- The menu flow's destructor does not delete the screen it is showing. The
  game leaves that flow only through the dispatcher, which has deleted the
  screen before; the destructor then purges the sprite and texture
  resources. Leaving from outside left a live screen (the attract demo, for
  one) drawing with purged resources. A flow request therefore first makes
  the dispatcher delete the screen (`mkgp2pc_nav_scene_hook()` returns 0:
  delete, build nothing) and the flow changes one frame later. The rule
  holds whenever the dispatcher ran in the frame before, whatever the
  flow's number: the same flow is entry 3 and, through a trampoline, entry
  8 (`Yamana`) of the table.

Restarting a race runs the race flow's constructor again, which counts a
play in the statistics and writes the backup record, as every race start
does.

Other entries of the flow table (the developers' viewers, `Ending`) are
reached through the scene list or with the script command below.

## Script commands

For tests and in the recorded inputs of a dump (`input.txt`), in the
`MKGP2PC_SCRIPT` syntax:

    <frame> back             one screen back
    <frame> flow=<n>,<e>     flow n; for the menu flow, entry e of the table above
    <frame> menu             the port's menu opens or closes (not recorded)

What the menu and the keys do is recorded as these commands, so a dump
plays back the same way.

## How the menu is drawn

With the game's own fonts and panels (`port/platform/ui/gameui.c`,
[`game_ui.md`](game_ui.md)): the white window of the yes/no cards, half
see-through (cut into pieces, not stretched), the window cursor in orange,
the 24 and 16 pixel text fonts in white with their outline. (The first version used the blue panel of the name entry
screen, stretched; it was hard to read, see game_ui.md, "Which frame, and
why".) The game stands still meanwhile, so
no scene is there to queue the sprites; instead the game's main loop stays
at the top of its pass while the menu is open
(`mkgp2pc_gamemenu_frame()`, `port/platform/ui/gamemenu.c`, called from
`MainGameLoop.cpp.patch`) and makes frames of this kind:

1. the game's start of a frame (`HSD_StartRender`);
2. the stopped game's last picture over the whole frame. The game copies
   every finished frame into a texture of its own (`g_backupBufferTexture`,
   `Frame_PostDraw_BackupBuffer`), which its screen transitions draw; the
   menu draws the same texture (`game_backdrop`, `port_menu.cpp`);
3. the menu, queued and drawn through the game's sprite system. The sprites
   the stopped scene has queued for its next frame are put aside for the
   time and come back when the menu closes
   (`mkgp2pc_gameui_overlay_begin` / `_end`);
4. the game's end of a frame (`Frame_PostDrawOverlay`: copy to the display,
   wait for the retrace). The credit display the game draws there stays
   bright over the darkened picture.

No scene is updated or drawn, and `VIWaitForRetrace` counts no retrace for
such a frame and reads no input, so a recording of the inputs does not see
the time spent in the menu. The game's retrace callbacks do run: its video
library hands its display buffers on in them and stops copying frames to
the display without.

The game's font has kana, Latin letters and about 450 kanji; the menu's
Japanese texts have a second wording for it where one is missing (戻, 覧,
閉, 紹介, 費: `portui::Text::game`).

Where nobody comes by the top of the main loop, the menu is drawn the other
way, with Dear ImGui over the picture as described below: during the boot
checks, and whenever the menu is open for three frames without the main
loop asking for it. `MKGP2PC_MENU=plain` asks for that drawing always.

Tried on 32-bit Linux (2026-11-12): attract loop, class selection, a race
(4:3 and `MKGP2PC_ASPECT=16:9`), the test mode after F1; "restart this
race" and "title" taken from it. Not run on Windows; not tried with
`MKGP2PC_HIRES=1`.

## The picture behind the plain menu

While the menu is open the game's loop waits inside `VIWaitForRetrace`
(`port/gfx/video_aurora.c`) and frames with nothing but the menu are
presented. For the game's last picture to stay behind it, the rendering
library now shows the copy the game makes of its frame buffer at the end of
each frame (`GXCopyDisp`; `AuroraConfig.emulateXfb`), as the console does,
instead of the frame buffer itself. `MKGP2PC_XFB=0` goes back to the frame
buffer; the menu then has a black background.

## Code

| File | What |
|---|---|
| `port/gfx/port_ui.hpp`, `port_ui.cpp` | for any screen of the port's own: two-language texts, the font, `portui::ListMenu` (a list with a cursor, drawn with Dear ImGui, which the rendering library links and draws after the game's frame) |
| `port/gfx/port_menu.cpp` | this menu: a `ListMenu` and its choices |
| `port/platform/ui/gamemenu.c`, `port/platform/include/mkgp2pc/gamemenu.h` | the frames of the stopped game with the menu in the game's fonts |
| `port/platform/ui/gameui.c` | the drawing itself (`mkgp2pc_gameui_list`, the overlay functions) |
| `port/platform/nav/nav.c`, `port/platform/include/mkgp2pc/nav.h` | the requests and the back stack; no game symbols |
| `port/gfx/video_aurora.c` | the two keys, the menu's loop |
| `port/nogfx/input_script.c` | `back`, `flow=` |
| `port/patches/src/game/MainGameLoop.cpp.patch`, `fn_801B7EF8.c.patch`, `ItemSelectState.cpp.patch` | the three places in the game's sources |
