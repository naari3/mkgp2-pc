# Cabinet link (LAN play between cabinets)

Up to four cabinets on one LAN play together. This page says how the arcade
program does it (read from the decompiled sources, file names given) and how
the PC port carries it. Port code: `port/platform/triforce/netlink.c`
(sockets), `port/platform/triforce/mediaboard.c` (the board's mailboxes).

State 2026-11-11: two copies of the no-graphics build on one Linux machine
find each other, connect both ways and exchange their state every frame.
Not checked: pictures (the rendering build), Windows, two different PCs, a
race played to the end over the link.

## Using it

Start each copy with a different cabinet number:

    MKGP2PC_PCB_ID=1 ...   (first PC or first copy)
    MKGP2PC_PCB_ID=2 ...   (second)

On Windows: `set MKGP2PC_PCB_ID=2` before `run-gfx.cmd`. Nothing else is
needed on one LAN or on one PC: the copies announce themselves and find each
other. The first start may bring up the firewall question for the program;
it has to be allowed on private networks.

| variable | meaning |
|---|---|
| `MKGP2PC_PCB_ID=1..4` | cabinet number at start-up. Without it every copy is cabinet 1, because the backup memory (where the test menu's PCB ID lives) is not kept between runs. The test menu (F1) still changes it for the rest of the run |
| `MKGP2PC_LINK=0` | link off: every socket command fails, as before 2026-11-11 |
| `MKGP2PC_LINK_PEERS=2=192.168.1.20,3=pc3` | where the other cabinets are, by hand (no announcements are used then); for networks that drop broadcasts |
| `MKGP2PC_LINK_PORT=29500` | first of the 68 ports used (see below); the same on all copies |
| `MKGP2PC_TRACE_LINK=1` | log connects, accepts and byte counts; `2` logs every command |

Ports, with the default base 29500: TCP 29500 + 16 x (cabinet - 1) + 0..8
for the game's listener, UDP 29564 + (cabinet - 1) for the announcements.

## What the arcade program does

Addresses and ports are fixed in the program:

- `g_pcbIpTable` (`src/game/autodata/data_803FDF50.c`, strings in
  `rodata_80310DE0.c`): cabinet 1-4 is 192.168.29.150-153, netmask
  255.255.255.0. The second address of each entry, 192.168.29.104-107, is
  that cabinet's camera (`NamCam_Init.c`).
- The cabinet number is byte 0x21 of the backup record (`lbl_80598A60`), set
  in the test menu. `GameBoot_Init` gives the board its address with
  `acModifyMyIPaddr`; `PcbIdToIpAndModify` does it again when the number
  changes.
- `PCBComm_Init` (`PCBCheckRecv.c`) makes one listener and, per other
  cabinet, one outgoing and one incoming connection object. It is first
  called in `BootDispatcher.cpp` before the "PCB check" screen and
  `PCBComm_Process(1, 1)` runs every frame from then on.
- Listener (`TCPListen_Poll.c`, `PcbListenerLifetime.c`): TCP socket, time
  limits 10 s, bind to port 5000 + cycle (cycle 0-8, one further after every
  close), listen with backlog 5, accept. An accepted connection is given to
  the cabinet whose address equals the peer address of the accept
  (`PCBComm_Process`): the source address is the only identification.
- Outgoing (`TCPConnPoll.c`, `TCPConnCloseInit.c`): socket, time limits 6 s,
  connect to the cabinet's address at port 5000 + cycle, cycle starting at 8
  and going down after every failure; 5 ticks between tries; after 16 closes
  the connection gives up (state 10) until a connection from that cabinet is
  accepted (`PcbConn_RecycleIfOver`). Once connected the limits become 80 s.
  So with a cabinet switched off the tries take 16 x 6 s.
- A cabinet sends on its outgoing connection and receives on the accepted
  one. Per frame (15 ms apart at least outside races) `TCPConn_PollSend`
  writes all queued messages with one `acSend` after `acSelect` says the
  socket is writable, and `TCPConn_PollRecv` reads with `acRecv` after
  `acSelect` says readable; 180 polls without data close the link.
- Messages (`TCPConnEnqueue.c`, `TCPConn.c`): `u16 type, u16 size` (size
  includes the 4 header bytes), then the payload. Types
  (`PCBCheckRecv.c`): 1 the cabinet's state (0x54 bytes,
  `PCBComm_BuildBroadcastSlot`: scene, role, character, class, name,
  steering, pedals, buttons), 2 input ring (0xDC), 3 a 0x200-byte block with
  its index (0x204), 4 snapshot (0x1C), 5 sync table (0x1C), 6 card record
  (0x45), 0xFFFF size 4 closes a batch.

The socket library (`src/triforce/acsocket*.c`) keeps one outstanding
request per socket. Each call becomes a media board command
(`src/triforce/amsock.c`); buffers travel through board memory, the command
carries their board addresses:

| value in `amsock.c` | call | arguments | 
|---|---|---|
| 0x104 | accept | socket, address of the peer address, address of its length; answered when a connection arrives |
| 0x204 | bind | socket, address of the socket address, length |
| 0x304 | close | socket |
| 0x404 | connect | socket, address of the socket address, length; answered when connected or failed |
| 0x604 | inet_addr | address of the text, length; the answer is not byte-swapped |
| 0x804 | listen | socket, backlog |
| 0x904 | recv | socket, board address, length, flags |
| 0xA04 | send | socket, board address, length, flags |
| 0xB04 | socket | domain 2, type 1, protocol 0 |
| 0xC04 | select | count, addresses of the read / write / except sets (256 bits each) and of the time limit, 0 for none |
| 0x1004 | set time limits | socket, send, receive, connect (milliseconds, by the values the game passes) |
| 0x1104 | last error | socket |
| 0x1504 | set own address | address of the text, length, netmask |
| 0x106 | (`fn_8028F45C`, called once at boot from `fn_80290ED8`; purpose not known) | none; the port answers "error" as before |

Network board layout (`lbl_803F27E8` in `amnet_setup_nm.c`): per-socket
scratch at 0x89040000 + 0x1000 x socket (+0x200), send area 0x89100000,
receive area 0x89180000, 0x80000 bytes each. Socket addresses are the BSD
kind with a length byte: `u8 16, u8 2, u16 port, 4 address bytes, 8 zero`.

## What the port does

The decompiled libraries run unchanged; `mediaboard.c` receives the command
packets and `netlink.c` executes them on host sockets (BSD sockets, Winsock
on Windows), without threads: accept and connect are asked about again on
every retrace (`mkgp2pc_mediaboard_tick`), everything else is answered at
once. Answers leave one at a time, as the game clears the answer mailbox.

A PC has neither the 192.168.29.x addresses nor one address per cabinet, and
the game identifies a peer by source address. So:

- Game address 192.168.29.(150+k), port 5000+c is real TCP port
  base + 16k + c on the host where cabinet k+1 runs. Several cabinets on one
  PC do not collide.
- Each cabinet sends a 12-byte UDP datagram (`MKGP2PC1` and its game
  address) once a second to the broadcast address and to 127.0.0.1, to the
  announcement ports of the other three. A cabinet heard on this PC is
  reached through 127.0.0.1.
- A connection starts with 8 bytes from the connecting side: `MKL1` and its
  game address. The accepting side reports that address to the game.
- A cabinet that has not been heard from is like an address nobody answers
  at: the connect fails after the game's 6 s. A known cabinet whose port is
  closed refuses at once, so the walk down from port 5008 to 5000 takes a
  moment.
- Byte order: the game stores command arguments byte-swapped for the board
  (real swaps on every host), so the port swaps them back. The descriptor
  sets of select are words the game swaps itself; the port reads them the
  same way. The port in a socket address is a plain 16-bit number in the
  game's memory.

Because of the greeting bytes and because the game's messages are structs in
host byte order (bit-fields included), this links PC builds with each other.
It is not the wire format of the arcade board: a real cabinet or an emulator
running the original program would need the greeting dropped, the source
address faked some other way and every message converted.

Dolphin (GPL, facts only, no code used) supports the same link for the
original program by redirecting the fixed addresses to configured ones.

## Found on the way

- `acConnect` was a stub in the PC build: `src/triforce/acsocket_nm4.c`
  switches to C++ with a pragma, the PC build compiles such a unit as C++
  from its first line, and `include/dolphin/card.h` had a parameter called
  `new`. The unit was listed as "NOT BUILT" and the generated stub returned
  0, so every connect "succeeded" without a command reaching the board.
  Upstream request 46 (parameter renamed, mkgp2-decomp 225c9b5).

## How it was checked (Linux, no-graphics build)

    MKGP2_ISO=... MKGP2PC_PCB_ID=1 MKGP2PC_TRACE_LINK=1 build/pc-census/mkgp2-nogfx   # in one folder
    MKGP2_ISO=... MKGP2PC_PCB_ID=2 MKGP2PC_TRACE_LINK=1 build/pc-census/mkgp2-nogfx   # in another

Both log "cabinet N is at 127.0.0.1", the walk down the ports, "connect:
connected" and "accept ... from 192.168.29.15x", then byte counts that grow
on both sides. With a debugger, the link slots (`PCBCheckRecv_bss_805A6154`,
0xA3C bytes each, the first bytes are the other cabinet's state message)
hold the other copy's state in both processes, and follow it when a scripted
coin and button presses move one copy through its menus.
