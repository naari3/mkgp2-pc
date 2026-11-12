# The player camera

The cabinet has a camera above the screen. After the coin the game shows its
picture behind a character frame ("顔をフレームにあわせてください"), takes a
photo, and uses the framed photo for the player (the picture under the
player's name in the race, for one). In the port a
webcam, a still picture or a test picture can stand in for it. Without one
the game runs with its camera option off, as it did before (2026-11-12).

## How the game reads the camera

Found in the decompiled source; nothing here comes from another project.

- The camera is a web server on the cabinet's own network. Cabinet N (PCB
  ID 1-4, address 192.168.29.(149+N)) has its camera at 192.168.29.(103+N),
  port 80 (`g_pcbIpTable`, `src/game/PcbIdToIp.c`, `NamCam_Init.c`).
- `NamCam_GetImageHandle` (`src/game/NamCam_GetImageHandle.c`) is called once
  a frame while a camera scene runs. It opens a socket through the network
  board (the same `ac*` library as the cabinet link), sends
  `GET /img.jpg HTTP/1.0` and an empty line, reads until the camera closes
  the connection (at most 0x80000 bytes), checks that the last two bytes are
  the JPEG end marker FF D9, and hands back the address after the HTTP
  header. Then it starts over: one connection per picture.
- `NamCam_LoadImage` / `Asset_LoadAndDecompressByType` decode that JPEG
  with the THP library's still-picture functions (`src/thp/THPDec.c`) into
  an RGBA8 texture of 320x240.
- The scenes draw that texture (`DrawNamCamImage`), and on the shutter copy
  the framed part out of the frame buffer into the player's photo
  (`src/ui/FrameSelection_Methods.c`, `NamCamFlow.cpp`,
  `CardCreate_Page.cpp`).
- At power-on the device check fetches one picture (`clCameraChecker` in
  `CardTaskManager_RenderOverlay.cpp`). If none comes within 300 frames it
  stops on an error screen until the test button is pressed, and the game
  then turns its camera option off (`VBlankEnable_Set(0)`; the flag's name
  in the source is a misnomer).

## What the port does

| Piece | File |
|---|---|
| The camera's web server: a connection to 192.168.29.104-107 port 80 is answered inside the network stand-in, no host socket involved | `port/platform/triforce/netlink.c`, "the camera" |
| The picture source, the newest picture at 320x240, the HTTP answer; the four THP functions on the camera's path | `port/platform/camera/camera.c` |
| Webcam on Windows (Media Foundation source reader) | `port/platform/camera/camera_mf.c` |
| The shutter's drawing selects the matrix it loads | `port/patches/src/game/DrawNamCamImage.c.patch` |
| Camera option left on when a source is set | `port/platform/triforce/card_reader.c` |
| Refresh of one changed texture | `mkgp2pc_gx_texture_changed`, `port/gfx/gx_shim.cpp` |

The game's code runs unchanged apart from one one-line patch: its
connection state machine, its timeouts and its scenes are the cabinet's.

**The picture is not a real JPEG.** The answer carries a JPEG's first and
last bytes and a baseline frame header for 320x240 (all the game itself
looks at), with a comment segment `mkgp2-pc uncompressed` and the pixels
uncompressed in between. The game's JPEG decoder is PowerPC assembly and is
not part of the PC build; `camera.c` supplies `THPDecGetInfo`,
`THPDecGetWorkSize`, `THPVideoDecode` and `fn_8029BCF0` (the RGBA8 output)
for this form only. A real JPEG, from a real network camera for instance,
is refused with a log line. Writing a JPEG decoder was not worth it for a
picture that never leaves the process.

**The end-of-picture test** in the arcade program is
`end[-2] == -1 && end[-1] == -0x27` on plain `char`. That finds FF D9 only
where plain `char` is signed, which it is in the cabinet's build of the game
code. The port used to compile everything with unsigned `char`, so no
picture was ever accepted; since 2026-11-12 the sign is set per file as in
the matching build (`docs/port/char_sign.md`) and the line works as written.
An earlier version of this page blamed the compiler; that was wrong.

**The shutter.** `DrawNamCamImage` draws the camera texture as one quad
into the top left 128x128 pixels of the frame buffer and copies that out as
the player's photo. It loads its matrix into slot 0 but does not select
slot 0. On the PC another slot was current at that moment, the quad was
not in the copied square, and the photo was a piece of the scene's
background (seen in the race, under the player's name). The patch adds
`GXSetCurrentMtx(0)`. Why the cabinet arrives there with slot 0 current
and the port does not has not been established.

**While the cabinet link is off** every socket command fails, as before.
The camera's connection still has to work, and the `socket` command does
not say what the socket is for, so `netlink.c` looks at the game's camera
state (`g_namCam`, connection step 1 is "make the socket") and lets through
only that socket and only a connection to a camera address.

**The device** is open only while the game asks for pictures: opened at the
first request, closed after five seconds without one, on a thread of its
own. A request is answered with a picture captured after it; if the device
gives none within 1.5 seconds (still starting, unplugged, in use by another
program) the previous picture or a grey one is sent, so no scene waits for
the camera. A device that cannot be opened is tried again every three
seconds.

## Settings

`MKGP2PC_CAMERA` (the settings program: page 基本, list カメラ, which shows
the cameras connected when it is opened):

| Value | Source |
|---|---|
| not set, empty, `0`, `off` | none; the game is told to do without its camera |
| `default` or `1` | the first camera of the system |
| a number N | the Nth camera |
| other text | the first camera whose name contains it (letter case ignored) |
| `test` | colour bars with a moving block; needs no device |
| `file:<path>` | a still picture: an uncompressed 24- or 32-bit BMP of any size |

The middle 4:3 part of the source is used and shrunk to 320x240.
`MKGP2PC_CAMERA_MIRROR=1` mirrors it. `MKGP2PC_TRACE_CAMERA=1` logs every
request, the opening and closing of the device and, on Windows, the list of
cameras.

With a source set the power-on device check includes the camera test
again, and after a coin the photo scene comes before the menus.

Cameras are read on Windows only. On other systems `test` and `file:` work.

## Privacy

Pictures stay in the process: they are not written to disk, not put into
dumps apart from what is visible in `frame.png`, and the port sends them
nowhere. Whether the game itself passes the framed photo to the other
cabinets of a linked race has not been looked at. Camera pictures do not
belong in this repository, like dumps and cards.

## Checked, and not

Checked on 32-bit Linux, 2026-11-12, with `MKGP2PC_CAMERA=test`:

- no-graphics build: the power-on camera check passes (`namcam check`,
  one request, `namcam end`) and the game goes on to the attract loop; with
  the variable unset nothing changes (the camera option is turned off as
  before);
- rendering build under Xvfb: after a coin the photo scene shows the test
  picture behind the frame, moving, through the countdown and the shutter,
  and the race then shows the framed part of it under the player's name;
  the cabinet link switched off (`MKGP2PC_LINK=0`) throughout;
- no-graphics build with the cabinet link on: the power-on check passes
  the same way;
- the Windows no-graphics program links with the new files
  (`link_census.py --os windows --link`);
- the BMP reader and the shrinking on their own, with a 24-bit and a
  32-bit file of 800x600: right way up, right colours;
- the settings program under Wine: the list shows, keeps a saved name that
  is no connected device, and opening it does not disturb the program (Wine
  had no camera to list).

Not checked:

- **Everything in `camera_mf.c`.** It compiles and links with MinGW; it has
  never run, for want of Windows and of a camera. Things that may need a
  second look on a real machine: the row order of the pictures (taken from
  the buffer itself where the driver offers that, otherwise assumed from the
  top), cameras that deliver only compressed formats, the time the device
  needs to start.
- The settings program's camera list on Windows, with real devices.
- `file:` inside the game (see above for the reader alone).
- The photo on a card, and in a linked race.

Known: at full speed (F10, Tab, `MKGP2PC_FAST=1`) frames that are not shown
are not drawn, and the copy the game makes of the framed photo at the
shutter is one of those drawings. A photo taken at full speed is therefore
usually black (seen with the test picture).
