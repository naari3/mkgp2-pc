# platform/triforce

PC replacements for the Triforce arcade board services the game uses through
the `am*` / `jvs*` / `ac*` libraries (`src/triforce`).

Planned split, one file per device, all behind the `mkgp2pc_triforce_*` hooks
in `../include/mkgp2pc/platform.h`:

| Device | Game side | PC replacement (planned) |
|---|---|---|
| JVS I/O board (wheel, pedals, item button, coin, test/service) | `amjvs*`, `jvsio*`, `jvsboot` | SDL3 gamepad / wheel / keyboard |
| Coin and credit bookkeeping | `amcoin*` | in-memory, free play by default |
| Backup RAM (settings, bookkeeping, records) | `ambackup*` | done: `ambackup` kept as it is, `baseboard.c` replaces `amexi.c` and keeps the memory in `backup_ram.bin` (docs/port/backup_memory.md) |
| Magnetic card unit (69-byte track) | `amuart*` and the card code in `src/game` | done: `card_reader.c`, the unit's serial protocol; one file per card (docs/port/build.md "Magnetic card") |
| Media board (network, boot, DIMM) | `amnet*`, `amsock`, `acsocket*`, `amboot`, `dvdlow*`, `dvdcmd` | done: `mediaboard.c` (mailboxes, replaces `dvdcmd.c`) and `netlink.c` (socket commands carried in UDP datagrams, cabinet link; docs/port/cabinet_link.md) |
| Board system services | `amsys`, `amsrv*`, `amexi`, `dikey`, `random*` | small host implementations |

Only the `null` back end exists (no devices). Whether a library is replaced at
its public API (`am*` functions reimplemented here) or kept as C with only its
EXI / SI register access replaced is decided per library when its turn comes;
record the decision in this table.

OpenGP (GPL-3) documents much of this hardware. Its facts are usable, its code
and text are not.
