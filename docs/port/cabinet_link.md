# Cabinet link (play between cabinets)

Up to four cabinets on one LAN play together. This page says how the arcade
program does it (read from the decompiled sources, file names given) and how
the PC port carries it. Port code: `port/platform/triforce/netlink.c`
(sockets), `port/platform/triforce/mediaboard.c` (the board's mailboxes).

State 2026-11-12: two and three copies of the no-graphics build on one Linux
machine find each other, connect, form one group when they take a coin, pass
the course selection and run a versus race together (scene 0x2C, race clock
running on all), also with delay, jitter and loss added between them (table
below). Since 2026-11-12 everything travels in UDP datagrams, the host's
waiting rule is the port's own and every cabinet drives its own kart
itself, all three with play over the internet in mind.
Not checked: pictures (the rendering build), Windows (the file compiles,
nothing more), two different PCs, a real internet connection, a race played
by people.

## Using it

Nothing has to be set. After the coin the game asks "1 PLAYER" or "NETWORK
VS" (steer to choose, the button or the accelerator to decide). Every copy
that chooses NETWORK VS, on this PC or on the same LAN, meets the others in
the entry screen; it shows how many are there and stays open until one of
them presses the button, then closes for all of them three seconds later. Two
to four copies become a versus group; one alone plays alone. The cabinet
number is taken automatically (the lowest one not in use). The first network
game may bring up the firewall question for the program; it has to be allowed
on private networks. How this is done: "Network versus on demand" below.

`MKGP2PC_LINK_MODE=arcade` gives the cabinet's behaviour instead: the link
is up from the start, there is no question, a coin leads to the entry screen
when another cabinet is connected, and each copy needs its own number:

    MKGP2PC_LINK_MODE=arcade MKGP2PC_PCB_ID=1 ...   (first PC or first copy)
    MKGP2PC_LINK_MODE=arcade MKGP2PC_PCB_ID=2 ...   (second)

| variable | meaning |
|---|---|
| `MKGP2PC_LINK_MODE=arcade` | link always on and no question after the coin, as on the cabinet |
| `MKGP2PC_PCB_ID=1..4` | cabinet number at start-up, for `MKGP2PC_LINK_MODE=arcade`; a network game chosen after the coin takes its own number. Without it every copy starts as cabinet 1, because the backup memory (where the test menu's PCB ID lives) is not kept between runs |
| `MKGP2PC_NET_CHOICE=1` or `2` | answers the question after the coin at once (1 alone, 2 network), for scripted runs |
| `MKGP2PC_BOOT_CHECK=1` | show the arcade program's 60 second screen "PCB ID : n CHECKING..." at start-up; another number is that many seconds. Without it the screen is skipped (see "Start-up without the wait") |
| `MKGP2PC_LINK=0` | no link and no question: every socket command fails, as before 2026-11-11 |
| `MKGP2PC_LINK_PEERS=2=192.168.1.20,3=pc3:29566` | where the other cabinets are, by hand: host name or address, optionally `:port`. For networks that drop broadcasts and for play over the internet. The cabinets not named are still found by their announcements |
| `MKGP2PC_LINK_PORT=29500` | base of the port numbers (see below); the same on all copies |
| `MKGP2PC_LINK_PACE=0` | the arcade program's own rule for when the host waits for a client (see "How a race is kept in step") |
| `MKGP2PC_LINK_START=0` | no cabinet holds its countdown to even out the start (see "The start") |
| `MKGP2PC_LINK_OWNKART=0` | the arcade program's way of moving a client's kart: only the host drives it (see "Every cabinet drives its own kart"). Either side can set it; the race then runs the old way for that client |
| `MKGP2PC_TRACE_LINK=1` | log connections and, every five seconds, the traffic; `2` logs every command |

Ports, with the default base 29500: UDP 29564 + (cabinet - 1) for
everything a cabinet sends and receives, and UDP 29568, where every copy
listens for announcements. No TCP.

### Between two networks (not tried on a real connection yet)

Without forwarding ports: a room name and a rendezvous server, see
[internet_play.md](internet_play.md). By hand:

Each side forwards its cabinet's UDP port (29564 for cabinet 1, 29565 for
cabinet 2, ...) to the PC and names the other side:

    MKGP2PC_LINK_MODE=arcade MKGP2PC_PCB_ID=1 MKGP2PC_LINK_PEERS=2=other.example.org
    MKGP2PC_LINK_MODE=arcade MKGP2PC_PCB_ID=2 MKGP2PC_LINK_PEERS=1=first.example.org

(`MKGP2PC_LINK_MODE=arcade` because the peers are named by cabinet number;
the question after the coin takes numbers by itself and has not been fitted
to named peers yet.)

One side naming the other is enough when only that other side can forward a
port: a cabinet answers to where a datagram came from. The datagrams are
neither encrypted nor signed; whoever can send to the port can take part.

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
packets and `netlink.c` executes them, without threads: accept and connect
are asked about again on every retrace (`mkgp2pc_mediaboard_tick`),
everything else is answered at once. Answers leave one at a time, as the
game clears the answer mailbox.

A PC has neither the 192.168.29.x addresses nor one address per cabinet, and
the game identifies a peer by source address. So:

- Each cabinet has one real UDP port, base + 64 + k for cabinet k+1.
  Several cabinets on one PC do not collide. Everything between two
  cabinets travels between those two ports.
- Each cabinet sends a 12-byte datagram (`MKGP2PC1` and its game address)
  once a second to the broadcast address and to 127.0.0.1, to the ports of
  the other three, to the common port base + 68, and to the cabinets named
  in `MKGP2PC_LINK_PEERS`. A cabinet heard on this PC is reached through
  127.0.0.1. The address and port an announcement came from are where that
  cabinet is.
- The game's TCP connections are streams inside datagrams (until 2026-11-12
  they were real TCP connections). The game still gets its bytes complete
  and in order. What changes is what a lost datagram costs: TCP holds
  everything behind it back until the retransmission, 200 ms or more; here
  every datagram with new bytes repeats the unconfirmed bytes before them
  (up to 1200 bytes a datagram, about five frames of a race), so the next
  frame's datagram, 17 ms later, makes the loss good. Only when that cannot
  work is the oldest open piece sent again, on a hint from the receiver or
  after 1.5 round trips. The header (32 bytes: connection number, game
  address of the sender, offsets of both streams, two clocks for the round
  trip time) is laid out in `netlink.c` under "connections".
- A connection is named by the cabinet that opened it and a number; the
  accepting side reports the opener's game address to the game.
- A cabinet that has not been heard from is like an address nobody answers
  at: the connect fails after the game's 6 s. A known cabinet that is not
  listening on the port refuses at once, so the walk down from port 5008 to
  5000 takes a moment. Any other address (the camera, 192.168.29.104-107)
  is never answered.
- For the game flow, `netlink.c` also offers a switch at run time
  (`mkgp2pc_netlink_set_active`), the cabinet numbers heard
  (`mkgp2pc_netlink_heard_mask`, also while the link is off or the copy has
  no number yet) and whether the number just set was free on this PC
  (`mkgp2pc_netlink_address_ok`).
- Byte order: the game stores command arguments byte-swapped for the board
  (real swaps on every host), so the port swaps them back. The descriptor
  sets of select are words the game swaps itself; the port reads them the
  same way. The port in a socket address is a plain 16-bit number in the
  game's memory.

Because of all that and because the game's messages are structs in host byte
order (bit-fields included), this links PC builds with each other. It is not
the wire format of the arcade board: a real cabinet or an emulator running
the original program speaks TCP between fixed addresses and would need every
message converted.

Dolphin (GPL, facts only, no code used) supports the same link for the
original program by redirecting the fixed addresses to configured ones.

## Start-up without the wait

The arcade program shows "PCB ID : n CHECKING..." for up to 60 seconds after
the card reader check (`BootDispatcher.cpp`: `PCBComm_Init`,
`BootPCBCheck_Init`, then `BootPCBCheck_Poll` each frame). The screen ends
early only when all four boards are connected both ways, or with two buttons
held (input bits 0x2000 and 0x400); with fewer than four cabinets it always
takes the full time.

The screen only displays. Nothing later reads a result of it:

- `PCBComm_Process(1, 1)` runs on every frame of the main loop and keeps
  trying, accepting and polling the connections; the per-cabinet flags the
  game asks about (`polled`, `receiving` in `PCBCheckProcess.c`) are
  refreshed there every tick.
- A cabinet that starts later is taken in at any time: its connection is
  accepted, and the accept revives the outgoing connection that had given up
  (`PcbConn_RecycleIfOver`).
- Whether a coin leads to a linked game is decided when the coin is
  confirmed (`fn_801B76C8.cpp`): with no connected cabinet the game goes
  straight on alone, otherwise to the entry screen (`clFlowEntry`, 15
  seconds, shared countdown), where two or more cabinets become a versus
  group.

So the port skips the screen: `port/patches/src/game/PCBCheckRecv.c.patch`
asks `mkgp2pc_boot_check_frames()` (`port/platform/triforce/linkflow.c`) for
its length, which is 0 unless `MKGP2PC_BOOT_CHECK` is set. Checked on Linux
with the no-graphics build at 60 retraces a second: the cabinet's scene
number is 54 with the timer counting down when the variable is 1, and without
it the game is in the notice (55) and then the attract scenes while the link
is up (`g_pcbCommEnabled` 1).

## Network versus on demand

The arcade program has no menu for versus. With a coin confirmed in the
attract loop (`fn_801B76C8.cpp`) it goes on alone when no other cabinet is
connected, and otherwise to the entry screen (`clFlowEntry`,
`fn_801BA318.cpp` and `fn_801BA1A4.cpp`, scene 7). The entry screen closes
when every connected cabinet has entered or when a timer shared by the
cabinets (15 s, counted in `PCBComm_UpdateSyncPhase1` by the lowest connected
cabinet) runs out; `PCBComm_ConfirmHostLobby` then makes the cabinets in it a
group, and `g_gameMode` becomes 2 (versus). The mode button of the class
select only switches between Grand Prix and time attack.

The port (decisions in `port/platform/triforce/linkflow.c`, game side in
patches):

- `GameBoot_Init.cpp.patch`: the link is switched off at start
  (`mkgp2pc_netlink_set_active(0)`); the game's link objects run as on a
  cabinet nobody answers.
- `fn_801B76C8.cpp.patch`: the confirmed coin puts up the question instead
  of deciding by the connected cabinets; `fn_801B785C.cpp.patch` draws it
  with the boot screens' text (`DrawText`, ASCII only). "1 PLAYER" goes on
  as the arcade program does alone. "NETWORK VS" switches the link on, takes
  the lowest cabinet number that `mkgp2pc_netlink_heard_mask()` does not
  report (the next one if `mkgp2pc_netlink_address_ok()` says the number was
  taken at the same moment), writes it where the test menu keeps the PCB ID,
  and does what the arcade program does around its test menu:
  `PcbIdToIpAndModify`, `PcbComm_Shutdown`, `PCBComm_Init`. Then the entry
  screen.
- `fn_801BA1A4.cpp.patch` and `PCBCheckRecv.c.patch`: in a network game the
  entry screen does not close because everybody connected has entered, and
  the shared timer is held at 10 s until a cabinet in the entry screen sends
  the arcade program's "hurry" flag (its button, `SetPcbScratchFlag_806d11a4`),
  then runs out from 3 s. The button is not taken during the first second
  and a half, and an empty timer table right after the restart of the link
  objects does not count as run out. `fn_801BA318.cpp.patch` draws the
  number of players and the hint.
- The link is switched off again when the entry screen ends with nobody
  else, and when the attract loop starts (`clFlowTitle`).

Checked 2026-11-12 with two copies of the no-graphics build on one Linux
machine, inputs from scripts: both choose NETWORK VS by steering, get
cabinets 1 and 2, connect, stay in the entry screen for 20 s, one button
press closes it on both, both are in one group (`g_gameMode` 2, the first is
host) and go on to the class and character selection; a copy that chooses
1 PLAYER sends nothing; a copy alone in the entry screen plays alone after
the button. Not checked: how the two texts look (the no-graphics build draws
nothing), the rendering build, Windows, two PCs, three or four copies, a
second network game after the first one has ended.

Known gap: a copy only hears about a network game after choosing NETWORK VS
itself; the arcade program's invitation in the attract loop (scene 6) does
not appear, because the link of a waiting copy is off.

## How a race is kept in step

Read from `src/game/VSMode_FrameUpdate.c` (`VSMode_FrameUpdate`) and
`PCBCheckRecv.c` (`PCBCheck_RecvLinkData`, `PCBComm_SendToAllPeers`).

- One cabinet of a group is the host (`g_isCommHost`; `PCBComm_ConfirmHost*`
  picks the one whose number is the group's role number minus one, in
  practice the lowest PCB ID that joined). There is no rollback and no
  prediction: the host runs the race, the others replay it.
- Every cabinet sends its state message (type 1) each frame; the last 0x28
  bytes are its input sample: three floats (pedals and wheel, quantised),
  buttons, and `queued`, the number of host frames it holds
  (`PCBComm_BuildBroadcastSlot`).
- The host keeps a ring of 64 samples per cabinet, uses the next one each
  frame and jumps to the newest when more than 3 wait. Its own samples go
  through the same ring, two entries ahead. It builds one frame of 0xDC
  bytes: per kart the input and, after its own simulation step, the kart's
  position, a speed value and the roulette result; for the CPU karts
  position and state (0x18 bytes each); rank, laps and the race clock. That
  frame (type 2) goes to every client each frame.
- A client queues host frames (ring of 64) and takes one per frame of its
  own. For each kart it first puts the kart at the host's position, then
  runs the same step with the host's inputs; CPU karts and the race clock
  are copied. With no new frame it runs the last one again. It only stops
  and waits when its ring is empty, and then until two frames have come.
- Flow control: when a client reports `queued` of 2 or more, the host waits
  one retrace before its next step, up to 4 retraces per step. While it
  waits it keeps sending. The port replaces this rule, see "The host's
  waiting rule" below.
- Time limits: 180 polls without data close a connection; a connected
  outgoing link with no incoming one for 180 ticks is reset; the socket
  limits are 80 s.

So a client sees its own kart move only after its input has gone to the host
and come back in a frame, and the host's player has no such wait.

### The host's waiting rule in the port (2026-11-12)

Between cabinets on one cable `queued` only grows when a client runs slower
than the host. Over a longer way it also jumps whenever frames arrive
bunched: with jitter, or when a lost datagram is made good by the next one.
The arcade rule then makes the host wait for a backlog that is gone before
the wait is over, the client runs dry, waits for two frames, reports two,
and so on; the first measurement below shows the race at 0.7 to 0.8 of real
time from that alone.

`port/patches/src/game/VSMode_FrameUpdate.c.patch` lets the host ask the
port (`mkgp2pc_link_backlog` in `netlink.c`) what a report means. A client
counts as behind only when its `queued` has stayed up without a break: at 2
or more for two seconds, 3 for two thirds of a second, 4 for a quarter, 6
for four frames. Then the host waits one retrace and the counting starts
again. What a client holds in between is its reserve against late frames.
It grows by itself, because a client that runs dry waits until two frames
have come (the arcade program's own rule, unchanged), and it shrinks through
the rule above when it is more than the connection needs. A client that
really is slower still slows the host, a few frames later than on the
cabinet. `MKGP2PC_LINK_PACE=0` gives the arcade rule back.

### Measured again with UDP and the port's waiting rule (2026-11-12)

Same set-up as the first measurement below (two copies, no graphics, 60
retraces a second, scripted input, 110 to 150 s a row), but the delay,
jitter and loss now act on single datagrams: jitter lets them overtake each
other and a lost one is really gone. "Stood still" counts the retraces in
which the race clock did not move on that copy.

| one way delay, jitter, loss | race speed, mean (worst second) | input delay, host | input delay, client (mean / worst) | stood still, per second (worst second) |
|---|---|---|---|---|
| 0 ms | 1.00 (1.00) | 17 ms | 81 / 84 ms | 0 |
| 20 ms, 0..10 ms | 1.00 (0.98) | 17 ms | 134 / 151 ms | 0.1 (1) |
| 20 ms, 2 % | 1.00 (0.98) | 16 ms | 136 / 151 ms | not counted |
| 50 ms, 0..20 ms, 5 % | 0.99 (0.87) | 17 ms | 230 / 318 ms | 0.6 (8) |
| 100 ms, 0..30 ms, 10 % | 0.99 (0.75) | 17 ms | 348 / 434 ms | 0.7 (15) |

With UDP alone and the arcade rule (`MKGP2PC_LINK_PACE=0`): steady delays of
0, 20, 30, 50 and 100 ms ran at 0.98 to 1.00, also with both copies started
at the same instant (the 0.72 of the first table is gone); 20 ms with
0..10 ms jitter 0.82, 20 ms with 2 % loss 0.88, 50 ms with 0..20 ms and 5 %
0.71. So the transport removes the stalls behind a lost segment and the
waiting rule removes the slow race.

Three copies with 20 ms, 0..5 ms jitter and 1 % loss: 1.00 on all three,
133 ms input delay on both clients.

What is left after these two is the input delay of a client: about 80 ms
plus twice the way, plus its reserve of frames, while the host's player has
one frame. The next section removes it.

### Every cabinet drives its own kart (2026-11-12)

In the arcade program only the host moves karts on its own account. A
client's pedals and wheel travel to the host, the host steps that kart, and
the client steps its copy when the frame with the same input comes back.
Here every cabinet steps its own kart with the input of the very frame, as
the host has always done for its own, and the kart's position travels the
other way:

- With every input sample a client reports where its kart is after its last
  step. The 18 bytes of the sample between the buttons and `queued` are
  unused on the way to the host (`pad3C` in `PCBCommBroadcast.c`) and the
  host copies samples whole: +0x10 three floats position, +0x1C the float
  the host sends at +0x1C of a kart's entry, +0x20 a mark (0x5A "can",
  0xA5 "does"), +0x21 a sample counter.
- The host puts its copy of that kart at the reported place before it steps
  it with that sample, once per sample (with no new sample the copy runs on
  by itself). This is what clients have always done with the positions in
  the host's frame. The host's frame carries the kart's position on to the
  other clients as before, so they need not know.
- The host answers in byte +0x23 of the kart's entry in its frame (unused,
  passed through from the sample): 0xA5 "positions are taken". A client
  drives its kart only while it sees that. A host or a client without this
  (`MKGP2PC_LINK_OWNKART=0`) leaves that client's race as it was.
- Buttons still take the long way. The host says which item a kart holds
  (+0x24 of its entry), and an item used before the host knows of it would
  come back for a moment.

Code: `mkgp2pc_link_kart` and `mkgp2pc_link_kart_done` in `netlink.c`,
called from `port/patches/src/game/VSMode_FrameUpdate.c.patch`.

Measured as above, with the wheel moved every 0.8 s as well:

| one way delay, jitter, loss | race speed, mean (worst second) | pedals and wheel reach the own kart after, host | client (mean / worst) |
|---|---|---|---|
| 0 ms | 1.00 (0.98) | 17 ms | 1 / 66 ms |
| 50 ms, 0..20 ms, 5 % | 0.99 (0.85) | 16 ms | 2 / 84 ms |

(The 1 to 2 ms are from the retrace at which the probe sees the change to
the step in the same frame; the worst cases are frames in which the client
waited for the host.) How far the host's copy of the client's kart is from
the place reported, just before it is put there: on average under a tenth of
what the kart moves in a frame (0.0 to 0.2 against 1.7 units a frame at
20 ms; 0.05 to 0.7 at 50 ms with jitter and loss), at most a few frames'
worth when samples were skipped.

What this does not solve, and what only play by people can judge:

- Each cabinet sees the other karts where they were one way (plus reserve)
  ago, and itself now. Whether a kart is hit by something moving, or two
  karts touch, is worked out on each cabinet from what it sees, as before,
  but the views now differ by that time. A hit that one cabinet sees and
  another does not shows as a kart that spins on one screen and not on the
  other; the positions stay those its own cabinet reports.
- The host judges order and the finish from what it sees, and it sees its
  own kart now and the others late. The next section evens that out at the
  start; a connection whose delay changes during the race shifts it again.
- A client that has no host frame still stands still for those retraces,
  its own kart included.

### The start (2026-11-12)

The host releases the countdown on all cabinets with one signal. It sees it
at once, a client one way later, and where the client's kart is reaches the
host another way later. With every cabinet driving its own kart, the host's
kart would leave a round trip before a client's on the host, where order
and finish are judged (measured at 50 ms one way: 166 ms, ten frames).

So every cabinet holds its countdown a little longer after the signal
(`mkgp2pc_link_start_due` in `netlink.c`, called from the same patch): all
of them one second, in which they find out who drives its own kart; then the
host by the longest trip of all clients, and a client by what its own trip
is shorter than that. A trip is the round trip the connections measure plus
50 ms for the frames in which the signal and a sample wait their turn
(measured: 150 to 167 ms at a round trip of 109 ms, 100 ms at about 50).
The host tells each client its time in the two unused bytes at +0x26 of the
kart's entry in its frame. At most 500 ms. `MKGP2PC_LINK_START=0`: nobody
holds anything.

Measured on the host (`MKGP2PC_TRACE_LINK=1` logs when each kart first
leaves its place; full throttle on all copies from before the countdown):

| network | host held | client held | on the host, the karts leave |
|---|---|---|---|
| 50 ms one way, `MKGP2PC_LINK_START=0` | 0 | 0 | client 166 ms after the host |
| 20 ms, 0..5 ms jitter | 99 ms | 0 | in the same frame |
| 50 ms | 157 ms | 0 | 15 ms apart (one frame) |
| 100 ms, 0..20 ms jitter, 3 % loss | 269 ms | 0 | in the same frame |
| three copies: one client 10 ms from the host, one 45 ms (mean) | 147 ms | 70 ms and 0 | within one frame (both clients' the frame before the host's) |

On a client's screen its own kart leaves about a round trip before the
host's: every screen shows its own kart now and the others earlier. The
host's view is the one the ranks come from.

### Measured with delay added, TCP and the arcade waiting rule (2026-11-11)

Two copies of the no-graphics build on one Linux machine, both at 60
retraces a second (`MKGP2PC_HZ=60`), the same scripted input on both (coin,
button presses, pedal changed every 1.5 s), versus race on the first course,
about 40 to 110 s of race per row. Delay, jitter and loss were added inside
the port to every send of both copies (`MKGP2PC_LINK_DELAY` and so on, see
below); the transport stays TCP on the loopback. `MKGP2PC_LINK_PROBE=1`
printed, once a second, how much the race clock advanced and how long a
change of the local input took to appear in the frame the race runs from.

| one way delay | race speed (1.00 = real time) | input delay, host | input delay, client (mean / worst) |
|---|---|---|---|
| 0 ms, copies started together | 0.72 | 20 ms | 82 / 101 ms |
| 0 ms, second copy 8 ms later | 0.98 | 17 ms | 83 / 117 ms |
| 1 ms | 1.00 | 17 ms | 74 / 99 ms |
| 5 ms | 1.00 | 18 ms | 91 / 134 ms |
| 10 ms | 1.00 | 17 ms | 100 / 131 ms |
| 20 ms | 1.00 | 17 ms | 117 / 133 ms |
| 30 ms | 0.98 | 17 ms | 148 / 200 ms |
| 50 ms | 0.70 | 22 ms | 184 / 234 ms |
| 100 ms | 0.75 | 20 ms | 287 / 317 ms |
| 20 ms + 0..10 ms jitter | 0.78 | 20 ms | 135 / 184 ms |
| 20 ms, 2 % of sends 200 ms late (a lost segment) | 0.72, worst second 0.43 | 21 ms | 199 / 467 ms |

What it shows:

- The host's player always has about one frame of input delay; a client
  has about 80 ms with no network delay at all, plus twice the one way
  delay.
- Up to about 30 ms one way with a steady delay the race runs at full
  speed. From 50 ms, or with 10 ms of jitter, or with losses, the whole
  race slows to 0.7 to 0.8 of real time for everybody: the client's
  `queued` report reaches the host late, the host waits on reports of a
  backlog that is already gone, and the two keep swinging.
- The same swing shows without any delay when the two copies' frames fall
  on the same instant (first row): it depends on the phase between the two
  machines' frames.
- Nothing is rolled back; what a client sees is its kart being put where
  the host says, every frame.

Limits of this measurement: emulated delay in the port (1 ms resolution),
one machine, no graphics, one course, scripted input, the race clock as the
measure of speed. How the arcade board itself behaves at these delays is
not known from it.

Options of the port for such measurements:

| variable | meaning |
|---|---|
| `MKGP2PC_LINK_DELAY=<ms>` | every datagram leaves that much later (2026-11-11: every send) |
| `MKGP2PC_LINK_JITTER=<ms>` | plus a random 0..ms each, so they may overtake one another (2026-11-11: order kept) |
| `MKGP2PC_LINK_LOSS=<percent>` | that share of the datagrams is not sent (2026-11-11: that share of the sends waited 200 ms or twice the delay, and everything behind it) |
| `MKGP2PC_LINK_PROBE=1` | the once-a-second line described above, since 2026-11-12 with the retraces in which the race clock stood still |
| `MKGP2PC_HZ=60` | no-graphics build only: real time instead of fast-forward; the delayed sends are released every millisecond there, once a frame in the rendering build |

## Found on the way

- Course selection crashed both linked copies: the course voted for is read
  through a 16-bit view of two bytes (`clFlowCourse.cpp`), which gave 32
  instead of 0..17 here. Upstream request 47 (`PCB_CHOICE_TRACK`,
  mkgp2-decomp 00de9ef).
- The start of a versus race never came: the string pool of
  `VSMode_FrameUpdate.c` was written as words, so `VSMode_Init` asked for the
  file "guyj_umeratsad.t" (`jyugemu_start.dat`) for ever. Upstream request
  48 (`TEXT_WORD`, mkgp2-decomp 81e7e44).
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
connected" and "accept ... from cabinet N", then byte counts that grow on
both sides. With a debugger, the link slots (`PCBCheckRecv_bss_805A6154`,
0xA3C bytes each, the first bytes are the other cabinet's state message)
hold the other copy's state in both processes, and follow it when a scripted
coin and button presses move one copy through its menus.
