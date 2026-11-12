# Settings program (mkgp2-launcher.exe)

`mkgp2-launcher.exe` is a small Windows program that sits next to
`mkgp2-gfx.exe`. It shows the port's settings as a window (texts in
Japanese), keeps them in `mkgp2-pc.ini` in the same folder and starts the
game with them. Source: `port/launcher/launcher.c` (plain Win32, one file, no
game code and no library of the port).

State 2026-11-12: built with MinGW and tried under Wine on Linux with a
stand-in for the game program (window, all five pages, saving, the key
dialog, starting, log, exit code). Not yet run on Windows. `MKGP2PC_KEYS`
in the game was checked by compiling and by running its parser alone, not in
a running game.
The コントローラ page (added 2026-11-12) was tried the same way under Wine,
with a controller made in software (controller.md); no real controller yet.

## How it works

The game program reads its settings from environment variables at start-up
and has no settings file. The settings program does not change that: on
"ゲームを起動" it writes the ini file, sets the variables in its own
environment (clearing the ones that are off, so that variables left in the
shell do not leak in), and starts `mkgp2-gfx.exe` with

- the program's folder as the current folder (`card.bin`, `dumps\`),
- standard output and error going to `gfx.log` (overwritten on every start,
  as `run-gfx.cmd` does),
- no console window (`CREATE_NO_WINDOW`).

While the game runs the start button is disabled; when it ends the exit code
is shown and appended to `gfx.log` as `exit code N` (hexadecimal for crash
codes). `mkgp2-launcher.exe --run` starts the game with the saved settings
without showing the window (for a shortcut); it does not wait, so no exit
code is written then. `run-gfx.cmd` still works and ignores the ini file.

## Pages and what they set

The window carries labels and controls only, no explanatory sentences (the
user's wish, 2026-11-12): what a setting does is written here, not in the
window. Empty fields show their default as grey text.

| page | item | variable |
|---|---|---|
| 基本 | ISO file | `MKGP2_ISO` |
| 基本 | use the card unit (off = `0`) | `MKGP2PC_CARD` |
| 基本 | card file (empty = `card.bin` in the folder) | `MKGP2PC_CARD_FILE` |
| 基本 | skip the device check and the NOTICE at start-up (see below) | `MKGP2PC_BOOT_SKIP` |
| 基本 | close this window after starting | (launcher only) |
| 基本 | camera: なし, or one of the cameras connected when the list is opened (by name); [camera.md](camera.md) | `MKGP2PC_CAMERA` |
| 表示 | aspect 4:3 or 16:9 (4:3 = not set); what 16:9 does is in widescreen.md | `MKGP2PC_ASPECT` |
| 表示 | window size: height 480 x factor, width from the aspect. 4:3 offers 1 to 6, 16:9 offers 1, 1.5 (1280x720), 2, 2.25 (1920x1080), 3, 4.5. | `MKGP2PC_SCALE` |
| 表示 | rendering backend (automatic, d3d12, d3d11, vulkan, opengl) | `MKGP2PC_BACKEND` |
| 表示 | anti-aliasing: none, MSAA 4x, SSAA 2x, both (antialiasing.md) | `MKGP2PC_MSAA`, `MKGP2PC_SSAA` |
| 表示 | smooth kart shadow (off = `1`) | `MKGP2PC_SHADOW_NEAREST` |
| 表示 | high-resolution 2D textures from the folder `hires` (hires_textures.md says how the folder is made) | `MKGP2PC_HIRES` |
| 表示 | 作成...: a window that makes the folder `hires` with `mkgp2-hires.exe` (method, size, progress, stop; hires_textures.md) | (launcher only) |
| 表示 | start at full speed | `MKGP2PC_FAST` |
| 表示 | coin tally popups as on the disc (English) | `MKGP2PC_GOODS_DISC` |
| 表示 | do not wait for pipelines | `MKGP2PC_ASYNC_PIPELINES` |
| 表示 | resizable window (crashes are known, see build.md) | `MKGP2PC_RESIZABLE` |
| 通信 | mode: solo (default; also sets `MKGP2PC_LINK=0`), network, ask, arcade. What each does is in cabinet_link.md | `MKGP2PC_LINK_MODE`, `MKGP2PC_LINK` |
| 通信 | cabinet number 1-4; 自動 leaves the variable unset | `MKGP2PC_PCB_ID` |
| 通信 | where the other cabinets are | `MKGP2PC_LINK_PEERS` |
| 通信 | first port | `MKGP2PC_LINK_PORT` |
| 通信 | room name and rendezvous server for play between distant PCs (internet_play.md) | `MKGP2PC_LINK_ROOM`, `MKGP2PC_LINK_SERVER` |
| キー | two keys per action | `MKGP2PC_KEYS` |
| コントローラ | game controller or wheel: on / off, which one, one source per action, steering dead zone and range (controller.md) | `MKGP2PC_PAD` |
| その他 | free lines `NAME=VALUE` (applied last, so they win) | any |

Variables for investigation (`MKGP2PC_TRACE_*`, `MKGP2PC_SCRIPT`,
`MKGP2PC_FRAMES`, ...) have no control of their own; they go into the free
lines of the last page.

## Start-up screens: `MKGP2PC_BOOT_SKIP`

Read by `port/platform/runtime/boot_skip.c`, used from
`port/patches/src/game/BootDispatcher.cpp.patch`. Off by default: the game
starts as the cabinet does.

- Device check (card reader, printer, camera): it still runs, since it also
  brings the card unit up, but without the frame limit and with only about
  60 of its frames a second drawn, like full speed (F10). It is over as fast
  as the machine does its frames.
- NOTICE (ten seconds of text): left out, the way the game itself leaves it
  out when byte 0x2A of its settings record is set.

The board check between the two (60 seconds unless `MKGP2PC_BOOT_CHECK`
handling of the cabinet link shortens it, cabinet_link.md) lies inside the
fast stretch too. The log says `boot: start-up screens begin` and `... end` (without a
retrace number: a port object that calls into the video layer does not link
on Windows). Measured 2026-11-12 with the no-graphics Linux build: the screens end
at retrace 4653 without the setting and 4054 with it (the 599 frames of the
NOTICE); the fast stretch itself needs the rendering build and has not been
run yet.

## Keys: `MKGP2PC_KEYS`

Read by `key_map_load` in `port/gfx/video_aurora.c`:

    MKGP2PC_KEYS=accel=A;brake=S,Left Shift;coin=

`action=key[,key...]` separated by `;`, up to four keys per action. Actions
not named keep their default keys; nothing after `=` leaves an action without
a key. Actions: `left right accel brake item cancel coin card test service pause
step fast fasthold dump hires debug debugpage devup devdown devstart devsub`
(from `debug` on: the game's own debug output and the buttons of the
developers' scenes, debug.md). A key is an SDL scancode name (`A`, `Left`, `F9`,
`Keypad 5`) or the scancode number, which is the USB keyboard usage (4 = A).
A single character is always a name (`5` is the key 5), so numbers are
written with two digits (`04`). Scancodes name physical keys: the layout does
not matter.

The settings program writes numbers. It gets them from the scan code of the
Windows key message through its own table (`kScanToUsage`), and shows them
with its own names (Japanese keyboard legends where they differ). A key
given to one action is taken away from the action that had it. If all keys
are the defaults the variable is not set at all.

## The ini file

`mkgp2-pc.ini`, UTF-16 with a byte order mark (the Windows profile functions
keep a file in the encoding it starts with; this keeps non-ASCII paths
intact). Sections: `[settings]`, `[keys]` (`action=usage1,usage2`, 0 = none),
`[pad]` (controller.md), `[env]` (the free lines). Deleting the file gives the defaults back. The
build never writes it, so it survives rebuilding into the same folder.

## Building

    port/launcher/build.sh [output directory]

needs only `i686-w64-mingw32-gcc` and `i686-w64-mingw32-windres` (both in
the container of `port/docker`); a few seconds. `port/docker/build-win32.sh`
runs it and puts the program into `dist`. The manifest (`launcher.manifest`)
asks for the themed common controls and system DPI awareness.

Looking at it without Windows: `wine mkgp2-launcher.exe` under Xvfb. Wine
has no Japanese font of its own; copying a Japanese font into the prefix's
`windows/Fonts` and naming it under
`HKCU\Software\Wine\Fonts\Replacements` for `Tahoma` makes the texts
readable.

## Why a separate program

The alternative was a menu inside the game window (Aurora carries Dear
ImGui). Window size and backend are needed before the window exists, the ISO
path before anything runs, and a menu drawn through the same renderer does
not help when rendering is what fails. A separate program also needs no
change in the game beyond the key table.
