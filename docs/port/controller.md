# Game controllers and racing wheels (analog wheel and pedals)

Written 2026-11-12. Code: `port/gfx/input_pad.c` (reading the controller),
`port/platform/triforce/jvs_controls.c` (positions as the values the game
expects), `keyboard_tick` in `port/gfx/video_aurora.c` (where keys and
controller meet), the コントローラ page of `port/launcher/launcher.c`.

State: checked on 32-bit Linux with a controller made in software (an SDL
virtual joystick moved by the input script), down to the three numbers the
game itself computes from the inputs; the settings page was tried under
Wine with the same kind of device. **No real controller, no real wheel and
no Windows machine has run this yet.**

## What the cabinet has and what the game does with it

The wheel and the two pedals are potentiometers on the JVS board's analog
channels 0 (wheel), 1 (accelerator) and 2 (brake), 16 bits each.
`JvsInput_PollAndUpdate` copies them, `InputObj_ReadRawInput`
(`src/game/InputObj_ReadRawInput.c`) turns them into three floats:

| control | value | from |
|---|---|---|
| wheel | -1 .. 1 | `(raw - centre) / range`, range `0x5000` |
| accelerator, brake | 0 .. 1 | `(raw - centre) / range`, range `0x4000`, centre = released reading + `0x800` |

Centre and released reading come from the calibration in the game's
settings record (test menu; `JvsInput_Set*Calibration`). A fresh record has
wheel centre `0x7FFF` and pedals released at 0, so the wheel's full lock is
`0x7FFF +- 0x5000` and a pedal is to the floor at `0x4800`: the game uses
well under half of the 16 bits. The first `0x800` of a pedal (a ninth of
its travel) is free play. The floats are used as they are (steering angle,
throttle); the game also derives buttons from them (wheel beyond 0.6 = left /
right in the menus, pedal beyond 0.7 = pressed).

The keyboard sends the ends of the 16 bits (wheel `0x8000 +- 0x7FFF` reached
over a few frames, pedals 0 or `0xFFFF`), which the game clips to -1, 0 or 1.
That is unchanged.

## What a controller does

`input_pad.c` reads one SDL joystick every frame. Its wheel position
(-1 .. 1) and pedal positions (0 .. 1) are laid onto the game's window by
`jvs_controls.c`, which reads the game's own centre and range, so a position
arrives as the same float whatever the calibration is: half lock is 0.5.
The pedals keep the cabinet's free play (a pedal at one half arrives as
0.4375), and a calibration run in the test menu with a controller attached
reads back the values it is given.

Keys and controller work side by side: the keys' wheel adds to the
controller's, a pedal key is the pedal to the floor, and a controller's
buttons do what the keys of the same action do. A controller gives nothing
until it has been touched (a button, or an axis moved by more than a seventh
of its travel), so one that merely lies there does not disturb a script.
Controllers may be attached and removed while the game runs. Like the keys,
a controller counts only while the game window has the focus.

Not done: force feedback (the cabinet's wheel motor, `StrPcb`), a second
player's controls, curves other than the straight line.

### Defaults

Which controller: the first wheel SDL reports, else the first gamepad, else
the first joystick. The log lists all of them (`pad: #1 '...'`) and says
which one is used.

| | controller SDL knows the layout of (gamepad) | anything else |
|---|---|---|
| wheel | left stick, left / right | axis 0 |
| accelerator | right trigger | axis 1, upper half pushed forward (`a1-`) |
| brake | left trigger | axis 1, pulled back (`a1+`) |
| item (decide) | A, Start | button 0 |
| cancel | B | button 1 |
| coin | Back | button 2 |
| card | Y | button 3 |
| wheel left / right as keys | D-pad left / right | |

Wheels with pedals differ too much to guess (which axis, which end is
released, one shared axis or two), so they are meant to be set on the
settings page. Steering dead zone: 10 % of the stick, none for a device SDL
reports as a wheel.

### `MKGP2PC_PAD`

    MKGP2PC_PAD=off
    MKGP2PC_PAD=device=G29;steer=a0;accel=a2~;brake=a3~;item=b0,b4;range=30

Entries separated by `;`; what is not named keeps its default.

| entry | meaning |
|---|---|
| `device=<text>` | the controller whose name contains the text (letter case ignored), or `#<n>` for the n-th in the log's list |
| `steer=<axis>` | the wheel |
| `<action>=<source>[,<source>...]` | an action of `MKGP2PC_KEYS` (launcher.md), up to four sources. For `accel` and `brake` an axis gives the pedal's position; for any other action an axis counts as pressed beyond half its travel. Nothing after `=`: no source |
| `deadzone=<n>` | percent of the steering axis around its centre that counts as centre |
| `range=<n>` | percent of the steering axis' travel that is full lock (default 100). The cabinet's wheel turns about 270 degrees; a 900 degree wheel set to 30 turns the same way |

Sources:

| source | meaning |
|---|---|
| `a<n>` | joystick axis n, its whole travel; lowest value = pedal released / wheel left |
| `a<n>~` | the same, the other way round |
| `a<n>+`, `a<n>-` | the upper / lower half of the axis from its centre (two pedals on one axis, a stick) |
| `b<n>` | joystick button n |
| `h<n>u`, `h<n>d`, `h<n>l`, `h<n>r` | a direction of hat n |
| `leftx lefty rightx righty lefttrigger righttrigger` (also with `~ + -`) | named axis of a gamepad |
| `a b x y back start guide leftshoulder rightshoulder leftstick rightstick dpup dpdown dpleft dpright` | named button of a gamepad |

The numbers are SDL's. `MKGP2PC_PAD_LOG=1` prints every axis and button of
the controller in use as it changes (`pad: a2 = -32768`, `pad: b3 down`),
which is the way to find them without the settings page.

## The settings page (コントローラ)

| item | writes |
|---|---|
| コントローラを使う (off) | `MKGP2PC_PAD=off` |
| デバイス: 自動 or a name; 再検索 looks again | `device=` |
| the list: one source per action. 変更 asks for it (turn the wheel to the right / press the pedal / press the button), 外す leaves the action without one, 既定に戻す gives the default back | `steer=`, `<action>=` |
| ハンドルの遊び (%) | `deadzone=` |
| ハンドルの切れ幅 (%) | `range=` |

Only what differs from the defaults is written; with nothing changed the
variable is not set. Ini file: section `[pad]` (`on`, `device`, `deadzone`,
`range`, `steer` and the action names; empty = default, `-` = none).

The numbers have to be the game's, and the game gets them from SDL, so the
page reads the controllers through the `SDL3.dll` that lies next to the game
program, loaded when the page is first shown (the settings program still
builds without any library). 変更 takes the first button or hat direction
that goes down or the first axis that moves by a quarter of its travel, and
works out the form from where the axis was at rest: at its lowest value
`a<n>`, at its highest `a<n>~`, in the middle `a<n>+` or `a<n>-`. For the
wheel it is `a<n>` or `a<n>~` by the direction turned. With 自動 and several
controllers attached, the one that answered is chosen in the list, since the
numbers belong to it.

## Trying it without hardware

`MKGP2PC_PAD_TEST=wheel` (4 axes, 12 buttons, 1 hat, reported as a wheel) or
`=gamepad` (standard layout) adds an SDL virtual joystick to the game; the
script commands `vax<n>=<value>` (-32768 .. 32767), `vbt<n>=<0|1>` and
`vhat<n>=<bits>` move it. Checked 2026-11-12 (Linux, `MKGP2PC_START_SCENE=4`):

| setting | script | channels 0, 1, 2 | the game's floats |
|---|---|---|---|
| `steer=a0;accel=a2;brake=a3~;range=50`, wheel | `vax0=4096 vax2=8192 vax3=-16384` | `93ff 2d00 3600` | 0.25, 0.578125, 0.71875 |
| defaults, gamepad | left stick x -20000, right trigger three quarters, left trigger full | `52a2 3600 4800` | -0.567, 0.71875, 1 |

(read from the running program with gdb: `g_jvsOperatorBits[4..6]` and the
three floats at 0x1C of the input object). The settings program takes
`MKGP2PC_PAD_TEST=<axis>,<from>,<to>` or `b<n>`: a virtual wheel whose axis
moves, or whose button goes down, a second after 変更. Under Wine that gave
`a0` (wheel), `a2` (axis resting at its lowest), `a3~` (resting at its
highest), `a1-` (from the middle downwards) and `b5`, and the game program's
stand-in received `MKGP2PC_PAD=deadzone=5;range=30;steer=a0;accel=a2;brake=a3~;item=b5;cancel=a1-;`.
