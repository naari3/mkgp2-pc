# The game's own debug facilities

The arcade program still contains most of what its developers used: about
670 `DebugPrintf` calls, an on-screen text overlay with 13 pages, a list of
all scenes with the developers' test scenes and viewers. The retail build
did not remove the callers, it emptied the ends of the paths. The port fills
those ends in again, behind a switch. Code: `port/platform/debug/debug.c`,
`port/gfx/debug_overlay.cpp`, and the patches of `src/game/Debug.c`,
`src/game/DebugTextOutput.c` and `src/game/main.c` under `port/patches`.

## What the retail program does

| Facility | In the game | Retail state |
|---|---|---|
| `OSReport`, `OSPanic` | the game's own definitions in `src/game/Debug.c` (the SDK's are not linked) | both empty |
| `DebugPrintf(fmt, ...)` | formats and calls `OSReport` when the debug byte is set | byte never set, `OSReport` empty |
| `DebugTextOutput(channel, fmt, ...)` | same, with a channel test | same |
| `OverlayText(layer, column, row, fmt, ...)` | text on a character grid over the picture | formats the string, the drawing is gone |
| debug byte | `lbl_80598A60[0x2A]` (`g_DebugPrintfEnable`), part of the backup settings | set to 0 at initialisation, written nowhere else; the test menu's GAME OPTIONS table still has a `*DEBUG DISPLAY` row that the cursor skips |
| overlay pages | `DebugOverlay_Dispatch`, called every frame by `MainGameLoop`; page 0 = nothing, 1..13 | page changes only with the debug byte set |
| scene list | `SeqMenuScene` (class `clMenu`), drawn with `OverlayText`; shown when a scene returns -4 | `main` always starts scene 3 (normal flow) or 12 (test mode) |

## Switches

| Switch | Effect |
|---|---|
| `MKGP2PC_DEBUG=1` | debugging on from the start |
| key `debug` (F3) | debugging on / off while running |
| key `debugpage` (F4, with Shift backwards) | next overlay page; works with debugging off too, but nothing is drawn then |
| `MKGP2PC_START_SCENE=N` | `main` starts with entry N of the scene table; -4 = the scene list |

Both keys can be reassigned with `MKGP2PC_KEYS` or in the settings program
(docs/port/launcher.md), whose page その他 takes the two variables as
`MKGP2PC_DEBUG=1` and `MKGP2PC_START_SCENE=-4`.

With debugging on:

- the debug byte is 1 (written every retrace, and written back to 0 when
  debugging is off, since the byte is saved with the settings);
- `OSReport` output, and with it `DebugPrintf` and `DebugTextOutput`, goes to
  standard error (`gfx.log` when started by the launcher) with `game: ` in
  front of each line, converted from Shift-JIS to UTF-8;
- `OverlayText` strings are drawn over the picture, and listed in
  `state.txt` of a dump (F9);
- the game's own input for the overlay page works: brake held and the item
  button pressed (input bit 0x80) is the next page.

`OSPanic` is reported always, debugging on or not (`game: OSPanic file:line:
message`), the first three times for each place when debugging is off. As in
the retail program it returns and the game goes on.

The log gets long: several scenes print every frame.

## Overlay pages

From `DebugOverlay_Dispatch` (`src/game/DebugOverlay_Dispatch.cpp`):

| Page | Content |
|---|---|
| 1 | update / draw / other times, averaged over 60 frames |
| 2 to 6, 8, 9 | profiler lines (time of a section as a fraction of 1/60 s); page 2 adds the active object counts |
| 7 | heap: total, used (KB), block count |
| 10 | `fn_801934F4` |
| 11 | state of the link between cabinets (`DebugOverlay_DumpPcbState`) |
| 12 | kart physics of the local player, with the terrain flags under the left and right wheels |
| 13 | a string and one byte of the player data |

The cell size of the grid is not known: the drawing code is not in the
retail program. The port assumes 8 by 16 pixels of the 640 by 480 picture
(80 columns, 30 rows) and draws with Dear ImGui's built-in fixed-width font,
which has ASCII only.

## The scene table

`MKGP2PC_START_SCENE` and the scene list use the table `main` hands to
`FlowDispatcher_Create` (`main_rodata_80328C94`):

| N | Name in the table | Constructor |
|---|---|---|
| 0 | Title | `GameMode_Title_Ctor` (a developers' title screen, not the attract title) |
| 1 | (stage select) | `GameMode_StageSelect_Ctor` |
| 2 | Game | `fn_801F63A4` |
| 3 | kart2 | `fn_801BA110`: the normal flow, the default |
| 4 | GameMain | `GameMode_DispatchSceneInit` |
| 5 | Cab | `VolumeCalibration_Ctor` |
| 6 | Miyoshi | `VSModeIntro_Ctor` |
| 7 | Miyoshi->main | `clMiyoshiCardCreate_Ctor` |
| 8 | Yamana | `TrampolineTo_801ba110` |
| 9 | Hidaka | `DbgScene00_SpriteViewer_Ctor`: `DbgMenu` with 14 sub-scenes (sprite, map, TPL, animation, screen effect, attribute tests) |
| 10 | Yokome | `fn_8020A3FC` (item viewer among others) |
| 11 | Miyazaki | `fn_8017FD68` (map object tests, sound test, model viewer) |
| 12 | TestMode | `OpenFullServiceMenu`: the test menu |
| 13 | result | `fn_80248248` |
| 14 | miyazaki>cdemo | `fn_80124DF8` |
| 15 | miyazaki>cmain | `MiniGame_CreateByCourseid` |
| 16 | Sprite Viewer | `fn_801D4060` |
| 17 | Flash  Viewer | `fn_801D39BC` |
| 18 | Live Test | `fn_801B7624` |
| 19 | Ending | `GameMode_CharacterSelect_Ctor` |

On the cabinet the list shows 13 of the 20: an entry's third word is read
as bytes, and the byte that hides an entry is set in 0x10000 on a big-endian
machine (Title, the stage select, GameMain, Miyoshi->main, result and the two
miyazaki entries). On the PC that byte is another one of the word, so all 20
are listed. Left as it is: the hidden ones are wanted here.

## Buttons inside the developers' scenes

The scenes read input bits that the cabinet's controls never produce. The
game's table `kRawGameMap_Raw0` maps five bits of player 1's JVS switch word
to input bits; the cabinet wires one (the item button). The port has a key
for each of the other four, passed on only while debugging is on:

| Key action (default) | JVS switch bit | Input bits | Use in the scenes |
|---|---|---|---|
| `item` (Space) | 0x2000 | 0x6001 | select, change a value, next |
| `devup` (PageUp) | 0x8000 | 0x40 | cursor up |
| `devdown` (PageDown) | 0x4000 | 0x80 | cursor down |
| `devstart` (End) | 0x0200 | 0x8400 | start / leave / previous |
| `devsub` (Home) | 0x1000 | 0x6002 | second button (Miyazaki's scenes) |
| `service` (F2) | operator | 0x800 | cursor down in some lists, play / stop in viewers |
| brake held + `item` | | 0x80 | the game's own combination for "down" |

Input bits 4 and 8, which some of Miyazaki's scenes test, come from none of
these (probably a controller of the development machine).

With the debug byte set, `MainGameLoop` also changes the overlay page on
0x40 and 0x80: moving a cursor up or down turns the pages. F4 turns them
back; page 0 is none.

| Scene | Inside it | Leave |
|---|---|---|
| scene list | PageDown / F2 down, PageUp up, Space selects | selecting |
| 0, 1 | go on to 2 by themselves | |
| 2 race set-up | PageUp / PageDown / F2 choose the line, Space changes the value (held: repeats on the three item lines), End starts | End |
| 5 Cab | shows wheel and pedal values | Space or End: back to the scene list |
| 6 Miyoshi | PageUp / PageDown line, Space changes stage / type / player | End: next scene of the table |
| 9 Hidaka | PageUp / PageDown, Space opens the sub-scene | End: scene list; End inside a sub-scene: back to this list |
| 10 Yokome | PageUp / PageDown, Space runs the entry | End: scene list |
| 11 Miyazaki | console menu; its pages use Space, Home and bits 4 / 8 | End: scene list |
| 16 Sprite Viewer | Space next, End previous, steering steps with repeat, accelerator held: steps of 50, F2 back to number 0 | none (use the port's menu) |
| 17 Flash Viewer | Space next, End previous, steering steps, accelerator held: steps of 10, F2 play / stop | none |
| 18 Live Test | steering or F2 changes the clip, accelerator / brake lengthen / shorten it, Space plays, End stops | none |
| 19 Ending | plays | by itself, to the scene list |

Read from the sources; tried on Linux: the scene list (up, down, select),
Yokome's list (up, down, leave), the race set-up page up to End. After End
on the race set-up page the race loaded and the program then crashed or
showed a black picture; not looked into yet.

A scene started from here gets none of the state the normal flow sets up
before it (player data, course, card).

## Tried (Linux, 2026-11-12)

Each entry started with `MKGP2PC_START_SCENE=N MKGP2PC_DEBUG=1`, card
service off, and looked at after about 40 seconds at full speed:

| N | Result |
|---|---|
| -4 | scene list shown; cursor and selection work (entered Hidaka, then one of its sub-scenes) |
| 0, 1, 2 | a race set-up page drawn with the overlay (stage, direction, time zone, character, kart, mode, class, items...) with item icons |
| 3 | the normal game |
| 4 | goes straight into a race |
| 5 | "Volume caribration" page: wheel and pedal values |
| 6 | stage / type / player selection page |
| 7 | crash at the first frame: the scene's `Draw` slot is null |
| 8 | the normal game (attract) |
| 9 | Hidaka's list of 14 sub-scenes |
| 10 | Yokome's list (Menu Disp, Menu test, Result Disp, Item Setting, Cup Select Viewer, TEV Setting test, Game parameter, Screen Adjust, GameFont test, Ez title, Particle test, Performance test, New sprite test, Old sprite sample) |
| 11 | Miyazaki's console menu (item names are Japanese and do not show with the ASCII font) |
| 12 | the test menu |
| 13 | the attract sequence |
| 14, 15 | crash: the constructor returns null unless a challenge game is selected (`g_cupId` 9..), as it would on the cabinet |
| 16 | sprite test mode, shows `adjust.tpl` |
| 17 | flash test mode, plays `ani_100cc_nonsel_end.bin` |
| 18 | plays `live_A01.dsp` and shows the play time |
| 19 | the ending scene, then back to the scene list |

Overlay pages 1, 2, 7 and 12 were looked at during a race.

Scene 11 crashed before this work for a reason of the port's link:
`src/game/mapobject/fn_8017CE1C.cpp` declares the same data labels inside
eleven namespaces (`view9::lbl_806D15FC` ...), one per type, and every such
name had become a stub. `link_census.py` now wires a data name inside a
namespace to the plain name (133 names, all of that unit).

Not tried: the sub-scenes of scenes 9, 10 and 11 one by one, the Windows
build.
