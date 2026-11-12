# Backup memory: the cabinet's records and settings between runs

Written 2026-11-12. Code: `port/platform/triforce/baseboard.c`.

## What the cabinet keeps

The Triforce baseboard has a small battery-backed memory that the game
reaches through `amexi.c` (EXI channel 0, device 2: set address, read, write,
block write) and the `ambackup` library above it. Two records live there:

| Record | Written by | Content |
|---|---|---|
| system (`amsys.c`, at address 0) | the `am*` libraries, when credits or settings change | coin settings, credits, network settings of the board |
| game (`GameBoot_Init.cpp`, id and size `0x6A20` given at boot) | `Backup_PublishShadowCopy`, `card_backup`, `FrameUpdate` | test menu settings (cabinet number, volume, calibration, ...), bookkeeping and play statistics, operation clock, and the time attack records: for each of 2 classes x 9 cups x 2 round lengths x 2 directions the 50 best times, the first 10 with name, character and title (`include/game/types/BackupHiScores.h`) |

Each record carries the library's own header (id, length, 16-bit sum). At
boot the game reads its record; when the id, length or sum is wrong it sets
every setting and record to the defaults (`Backup_LoadFromMemoryOrInit`,
`Backup_ClearHiScores`) and writes the record anew.

Where the records show in the game: the attract loop has a page per course
with the best time of its rounds (name, character, time, title;
`DemoScene` in `Scene_8014E0C4.cpp`), and the end of a time attack lists the
ten best of the round just played with the new entry marked
(`RankPanel_80144430.cpp`, `RaceHudResultTA.cpp`).

## The file

`ambackup` and everything above it run unchanged; `baseboard.c` replaces
`amexi.c` with an array of 0x10000 bytes (the protocol's address is 16 bits)
and keeps that array in a file.

- Name: `backup_ram.bin` in the directory the program is started from (the
  launcher starts it in its own folder, so the file sits beside `card.bin`).
  `MKGP2PC_BACKUP_FILE=<path>` names another file. `MKGP2PC_BACKUP=0`: no
  file, the memory starts empty on every run and nothing is saved (how every
  build before this behaved; input recordings made by those builds need it
  to play back in step).
- Layout: `MKGP2BKP`, version (1) and the CRC-32 of the image, each 32 bits
  big-endian, then the 0x10000 bytes. The game's data inside is in the byte
  order of the PC build, so the file is not the memory of a real board.
- Reading: once, before the game's first access. A file of the wrong length
  or with a wrong CRC is renamed `<name>.broken` and `<name>.bak` is used
  instead; without one the memory starts empty and the game sets it up as on
  a new board. `<name>.bak` is the file as it was at the last start that
  found it good.
- Writing: every write of the game that changes a byte marks the memory
  changed. A thread of its own looks every 30 ms and, once the last change is
  60 ms old (so that a record the library writes byte by byte is saved
  whole), writes `<name>.new`, flushes it to the disk and renames it over
  the file. The game never waits for the disk. The program is ended with
  `TerminateProcess` on Windows (window closed, reset) and may be killed at
  any time; whatever the moment, the file under the real name is the old one
  or the new one, never a part. The window's close button and
  `OSResetSystem` bring the file up to date first
  (`mkgp2pc_baseboard_flush`).
- Two copies of the game from one folder (two cabinets on one PC for the
  link): the first takes `<name>.lock` and the file, the next ones find the
  lock taken and use `<name>.2`, `.3`, `.4`, each with records and settings
  of its own. The lock ends with the process.
- The log says which case applied, in lines starting `backup:`.

To start again from an empty memory delete `backup_ram.bin` and
`backup_ram.bin.bak` while the game is not running.

What changes for the player besides the records: test menu settings stay
(cabinet number, volume, ...); the system record holds the credits too, so
coins not used may still be there at the next start (not checked).
`MKGP2PC_PCB_ID` still overrides the saved cabinet number at start-up.

## Dumps and playing them back

A dump folder (`F9`, crash) holds `backup_start.bin`: the memory as that run
found it, in the format of the file. Playing `input.txt` back in step needs
the same start, so copy it somewhere and name the copy with
`MKGP2PC_BACKUP_FILE` (the run writes to it). `state.txt` records
`MKGP2PC_BACKUP` and the name of the file in use.

## What was checked

Linux, the no-graphics executable (2026-11-12):

- First run in an empty folder: the game sets the memory up, the file
  appears; the process was ended with SIGKILL.
- Next run: `backup: backup memory read from backup_ram.bin`, and the game
  accepts its record (after `GameBoot_Init` the "set up anew" countdown
  `lbl_806D1124` is 0 and the record's tag is 15).
- Records and settings: with a debugger a settings byte (`+0x27`), the best
  time of the first table and a name in the last table were changed in the
  game's working copy and saved by the game's own `Backup_PublishShadowCopy`;
  after SIGKILL the next run read all three back. Not done: setting a record
  by playing, or changing a setting in the test menu, by hand.
- A file cut to 30,000 bytes, and one with a single bit changed: set aside
  as `.broken`, `.bak` used, or an empty start without one. A missing file
  with a `.bak`: the `.bak` is used. A stale `.new`: overwritten.
- Two copies at once: the second uses `backup_ram.bin.2`.
- `MKGP2PC_BACKUP=0` and `MKGP2PC_BACKUP_FILE`.

Windows: `baseboard.c` and `reset.c` compile with the MinGW compiler; the
Windows paths (`MoveFileEx` with replace and write-through, `_commit`, the
lock file opened without sharing) have not been run.
