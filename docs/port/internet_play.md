# Play between distant PCs (room name, STUN, hole punching)

[cabinet_link.md](cabinet_link.md) says how the link between cabinets works
and how its data travels (UDP, one port per cabinet). This page is about the
step before: two to four PCs behind home routers finding a way to each other
without anybody opening a port. Code: `port/platform/triforce/netmeet.c`,
hooks in `netlink.c`; server: `port/server/rendezvous/`.

State 2026-11-12: two copies of the no-graphics build on Linux, each behind
its own emulated NAT router (network namespaces, `iptables MASQUERADE`),
with a local STUN server and the server code under Node, meet by room name,
get cabinet numbers 1 and 2 and connect (`port/server/rendezvous/test/`).
Not checked: Windows (the executable links with the WinHTTP code, it has not
run), the Worker on Cloudflare, Cloudflare's STUN server, real routers,
three and four players, a race played this way.

## Using it

Every player sets the same two things and starts the game:

    MKGP2PC_LINK_ROOM=<any text>          the room name, agreed between the players
    MKGP2PC_LINK_SERVER=https://<name>.workers.dev

In the launcher: tab 通信, 合言葉 and 仲介サーバー; PCB ID can stay as it is
(two players with the same number are sorted out by the server) or 自動.
`MKGP2PC_LINK_PEERS` and port forwarding are not needed.

What to look for in the log (`gfx.log` / standard error):

    link: room: this is cabinet 2
    link: room: this cabinet is reached from outside at 203.0.113.12:29565
    link: room: the router keeps one outside port for all destinations; direct connections can work
    link: room: cabinet 1 is at 203.0.113.11:29564

| variable | meaning |
|---|---|
| `MKGP2PC_LINK_ROOM` | room name, any text (sent percent-encoded, UTF-8). Whoever knows it can join, so it is also the password |
| `MKGP2PC_LINK_SERVER` | address of the rendezvous server, `https://...` (or `http://` for tests) |
| `MKGP2PC_LINK_STUN` | `host:port[,host:port]`, default `stun.cloudflare.com:3478,stun.cloudflare.com:53`. The second is only asked to see what kind of router this is |

## Setting up the server (once, by whoever hosts it)

The server is a Cloudflare Worker with one Durable Object class; the free
plan is enough (a player sends one small request every two seconds while
the game runs with a room).

    cd port/server/rendezvous
    npx wrangler login
    npx wrangler deploy

`wrangler deploy` prints the address (`https://mkgp2pc-rendezvous.<account>.workers.dev`);
that is `MKGP2PC_LINK_SERVER` for everybody. `npm test` runs the room logic's
tests, `npm run local` serves the same rooms from Node on 127.0.0.1:8787.

## How it works

1. **Cabinet number.** At boot (`mkgp2pc_boot_cabinet`) the game asks the
   server for a number: the one of `MKGP2PC_PCB_ID` if nobody in the room
   has it, else the first free one. This request waits (5 s at most).
2. **Own public address.** The cabinet's UDP socket (29564 + number - 1)
   sends a STUN Binding request (RFC 8489) to the STUN server. The answer
   names the address and port the router gave that socket. It is asked
   again every 20 s. A second destination (port 53 of the same server) is
   asked once: if it reports another port, the router numbers every
   destination differently (symmetric NAT) and the log says so.
3. **The others' addresses.** A thread asks the server every two seconds:
   `GET /v1/room/<name>?token=<random>&id=<n>&ip=<a.b.c.d>&port=<p>`. The
   answer is text, `id 2` and one `peer <n> <ip> <port>` per other member.
   A member that has not asked for 15 s is dropped from the room.
4. **Hole punching.** `netlink.c` treats a room member like a cabinet named
   in `MKGP2PC_LINK_PEERS`: it sends it the announcement datagram once a
   second. The first one out makes the own router let that member's
   datagrams in; both sides do it, so after a second or two they pass in
   both directions and the game's connect succeeds. From then on the link
   is exactly the one of cabinet_link.md, directly between the players.
   If a member's datagrams come from another port than the server said,
   that port is used.

With a room, datagrams from addresses that are neither a member's nor
private (this PC, 10/8, 172.16/12, 192.168/16, 169.254/16) are ignored.
Source addresses can be forged; the datagrams are still neither encrypted
nor signed.

Members the server stops listing are left alone: once the players have
each other's addresses, a race does not depend on the server.

Two players behind the same router have the same public address; they are
left to the LAN announcements, as without a room.

## What goes where

- The STUN server sees the player's public address (it has to).
- The rendezvous server sees room name, cabinet number and public address
  and gives them to the members of the same room. It keeps them in memory
  only, for 15 s after the last request.
- Every member of a room learns the others' public addresses. That is
  inherent in a direct connection.
- No game data passes through either server.

## When it does not connect

- **Symmetric NAT** (the log line "gives every destination another outside
  port"): mobile connections, some shared-IPv4 access lines. The other side
  aims at a port that is not the one used towards it. It still works when
  the other side's router accepts datagrams from any port of an address it
  has sent to; otherwise not. What would help: a relay (TURN, for example
  Cloudflare's, or a small VPS), not built.
- **Routers that answer instead of dropping.** Found while testing with
  Linux routers: if the first datagram from the other side arrives before
  this side has sent one, and the router does not silently drop it, the
  router remembers that port pair as taken and gives this side's own
  datagrams another outside port. Home routers normally drop.
- **IPv4 only.** Neither netlink.c nor the room handles IPv6. On access
  lines with shared IPv4 (MAP-E, DS-Lite) the IPv4 side is a NAT at the
  provider or with a restricted port range; how those behave here has not
  been tried. IPv6 between such lines would avoid NAT altogether and is the
  obvious next step.

## The test rig

`port/server/rendezvous/test/net.sh` (root, needs `iproute2` and `iptables`)
builds this out of network namespaces:

    inet (203.0.113.1: STUN and rendezvous) -- r1 (203.0.113.11) -- h1 (192.168.1.2)
                                            \_ r2 (203.0.113.12) -- h2 (192.168.2.2)

`NAT1=random` / `NAT2=random` makes that router a symmetric one.
`test/run.sh SECONDS` (with `ISO=`) starts the servers and one copy in h1
and h2. Results 2026-11-12:

| routers | result |
|---|---|
| both keep ports | numbers 1 and 2 given, addresses exchanged, connected, 2 connections, round trip 1-2 ms |
| both keep ports, both players ask for number 1 | the second is told "taken" and becomes 2; connected |
| r2 symmetric | noticed and logged on h2; no connection (r1 is port-restricted) |
| no room, two copies on one PC | found by announcements as before |
