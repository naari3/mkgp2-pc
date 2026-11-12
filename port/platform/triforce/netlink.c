/*
 * Cabinet link: the network board's socket commands on the host's network.
 *
 * The game talks to the other cabinets through the ac* socket library
 * (src/triforce/acsocket*.c), which turns every call into a media board
 * command (src/triforce/amsock.c). mediaboard.c hands those commands to
 * mkgp2pc_netlink_command() below. What the game does on top of them
 * (src/game/TCPConn*.c, TCPListen_Poll.c, PCBCheck*.c) is unchanged; the
 * protocol is written up in docs/port/cabinet_link.md.
 *
 * The game's network is fixed: cabinet N (1-4, the "PCB ID" of the test
 * menu) is 192.168.29.(149+N), it listens on TCP port 5000-5008 and connects
 * to the other three. It recognises an incoming connection by its source
 * address. None of that exists on a PC, so:
 *
 *  - Every cabinet has one real UDP port, BASE + 64 + k for cabinet k+1
 *    (BASE 29500, MKGP2PC_LINK_PORT). Several cabinets on one PC do not
 *    collide. Everything between two cabinets travels between those two
 *    ports: the announcements and all of the game's connections.
 *  - Where a cabinet runs is learned from announcements: every second each
 *    cabinet sends a small datagram to the broadcast address and to this PC,
 *    to the ports of the other three, and to BASE + 68, where every copy
 *    listens. MKGP2PC_LINK_PEERS ("2=192.168.1.20,3=example.org:29566")
 *    names hosts by hand instead; those are sent the announcement directly.
 *  - The game's TCP connections are streams inside datagrams (see
 *    "connections" below): bytes arrive complete and in order as the game
 *    expects, but a lost datagram does not hold the stream up the way TCP
 *    does, because every datagram repeats what has not been confirmed yet.
 *    A connection is named by the cabinet that opened it and a number, and
 *    carries the game address of its sender, which the accepting side
 *    reports to the game.
 *  - A cabinet that has not been heard from behaves like an address nobody
 *    answers at: the connect fails after the game's connect timeout.
 *
 * So this is a link between PC builds only; it is not the wire format of the
 * arcade board (and the game's own messages are in host byte order).
 *
 * MKGP2PC_LINK=0 turns all of it off (every socket command fails, as before
 * this file existed). MKGP2PC_TRACE_LINK=1 logs connections and once in five
 * seconds the traffic, 2 also the commands.
 *
 * For the game flow (port/platform/triforce/linkflow.c):
 * mkgp2pc_netlink_set_active() is the same switch at run time,
 * mkgp2pc_netlink_heard_mask() says which cabinet numbers are taken, also
 * while the link is off and before this copy has a number, and
 * mkgp2pc_netlink_address_ok() says whether the number just set was free on
 * this PC.
 *
 * All calls come from the game with the port's "interrupt" lock held or from
 * the retrace tick; nothing here blocks and there are no threads.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET HostSocket;
typedef int socklen_t_;
#define NO_SOCKET INVALID_SOCKET
#define close_host closesocket
static int host_error(void) { return WSAGetLastError(); }
static int would_block(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY; }
static void non_blocking(HostSocket s) {
  u_long on = 1;
  ioctlsocket(s, FIONBIO, &on);
}
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
/* A datagram to a port nobody has open must not make the next receive fail. */
static void no_reset_reports(HostSocket s) {
  BOOL off = FALSE;
  DWORD done = 0;
  WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), NULL, 0, &done, NULL, NULL);
}
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int HostSocket;
typedef socklen_t socklen_t_;
#define NO_SOCKET (-1)
#define close_host close
static int host_error(void) { return errno; }
static int would_block(int e) { return e == EWOULDBLOCK || e == EAGAIN || e == EINPROGRESS || e == EALREADY; }
static void non_blocking(HostSocket s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
static void no_reset_reports(HostSocket s) { (void)s; }
#endif

#include "mkgp2pc/triforce.h"

/* Error numbers as the game's log names them (src/game/AcError_LogIfError.c
 * knows 9, 23, 24, 50, 55): the BSD ones. */
enum { E_BADF = 9, E_INVAL = 22, E_NFILE = 23, E_WOULDBLOCK = 35, E_NETDOWN = 50, E_NOTCONN = 57, E_TIMEDOUT = 60, E_CONNREFUSED = 61 };

#define MAX_SOCKETS 64
#define MAX_CABINETS 4
#define ANNOUNCE_SIZE 12

typedef enum { FREE, FRESH, LISTENING, CONNECTING, CONNECTED } Kind;

typedef struct {
  Kind kind;
  int error;
  uint32_t timeout[3]; /* send, receive, connect; milliseconds */
  uint16_t port;       /* game port bound to */
  int chan;            /* CONNECTING, CONNECTED: its connection, or -1 */
  /* connect */
  uint8_t target[4];
  uint16_t targetPort;
  uint64_t since;
} Sock;

typedef struct {
  int known;
  int fixed; /* from MKGP2PC_LINK_PEERS */
  struct in_addr host;
  uint16_t port;  /* network order */
  uint64_t local; /* when it was last heard on this PC */
} Peer;

static Sock s_sock[MAX_SOCKETS];
static Peer s_peer[MAX_CABINETS];
static int s_fixedPeers;
static uint8_t s_address[4]; /* this cabinet's game address; 0 until the game sets it */
static HostSocket s_announce = NO_SOCKET; /* this cabinet's port: announcements and connections */
static int s_announceIndex = -1;
static HostSocket s_observe = NO_SOCKET; /* hears the announcements without a cabinet number */
static uint64_t s_heard[MAX_CABINETS];   /* when each cabinet was last heard; 0 never */
static uint64_t s_probed;
static int s_active = 1; /* mkgp2pc_netlink_set_active */
static uint64_t s_announced;
static int s_state; /* 0 not looked at, 1 on, -1 off */
static int s_trace;
static int s_base = 29500;
static uint64_t s_sent, s_received, s_reported;         /* bytes of the game, for the trace */
static uint64_t s_datagrams, s_repeats, s_lost, s_dropped; /* sent, sent again, not sent (MKGP2PC_LINK_LOSS), not understood */
static int s_probe;
static int s_delay, s_jitter, s_loss; /* measurement options, see wire_send() */

static uint64_t now_ms(void) {
#ifdef _WIN32
  return GetTickCount64();
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

#define TRACE(...) \
  do { \
    if (s_trace) { \
      fprintf(stderr, "link: " __VA_ARGS__); \
      fputc('\n', stderr); \
    } \
  } while (0)

static int configured(void) {
  if (s_state == 0) {
    const char* e = getenv("MKGP2PC_LINK");
    const char* port = getenv("MKGP2PC_LINK_PORT");
    const char* peers = getenv("MKGP2PC_LINK_PEERS");
    s_state = (e != NULL && e[0] == '0') ? -1 : 1;
    s_trace = getenv("MKGP2PC_TRACE_LINK") != NULL ? atoi(getenv("MKGP2PC_TRACE_LINK")) : 0;
    s_delay = getenv("MKGP2PC_LINK_DELAY") != NULL ? atoi(getenv("MKGP2PC_LINK_DELAY")) : 0;
    s_jitter = getenv("MKGP2PC_LINK_JITTER") != NULL ? atoi(getenv("MKGP2PC_LINK_JITTER")) : 0;
    s_loss = getenv("MKGP2PC_LINK_LOSS") != NULL ? atoi(getenv("MKGP2PC_LINK_LOSS")) : 0;
    s_probe = getenv("MKGP2PC_LINK_PROBE") != NULL;
    if (port != NULL && atoi(port) > 0 && atoi(port) < 65000) {
      s_base = atoi(port);
    }
#ifdef _WIN32
    if (s_state == 1) {
      WSADATA data;
      if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        s_state = -1;
      }
    }
#endif
    while (s_state == 1 && peers != NULL && *peers != 0) { /* "2=host,3=host:port" */
      char name[128];
      int id = atoi(peers), n = 0;
      const char* p = strchr(peers, '=');
      const char* end = strchr(peers, ',');
      if (p != NULL && (end == NULL || p < end)) {
        int udp = 0;
        for (p++; *p != 0 && *p != ',' && *p != ':' && n < (int)sizeof(name) - 1; p++) {
          name[n++] = *p;
        }
        name[n] = 0;
        if (*p == ':') {
          udp = atoi(p + 1);
        }
        if (id >= 1 && id <= MAX_CABINETS) {
          struct addrinfo hints, *info = NULL;
          memset(&hints, 0, sizeof(hints));
          hints.ai_family = AF_INET;
          hints.ai_socktype = SOCK_DGRAM;
          if (getaddrinfo(name, NULL, &hints, &info) == 0 && info != NULL) {
            s_peer[id - 1].host = ((struct sockaddr_in*)info->ai_addr)->sin_addr;
            s_peer[id - 1].port = htons((uint16_t)(udp > 0 && udp < 65536 ? udp : s_base + 64 + id - 1));
            s_peer[id - 1].known = s_peer[id - 1].fixed = 1;
            s_fixedPeers = 1;
            freeaddrinfo(info);
          } else {
            fprintf(stderr, "link: MKGP2PC_LINK_PEERS: cannot resolve \"%s\"\n", name);
          }
        }
      }
      peers = end != NULL ? end + 1 : "";
    }
    if (s_state == 1) {
      fprintf(stderr, "link: cabinet link on, UDP ports %d-%d%s\n", s_base + 64, s_base + 64 + MAX_CABINETS,
              s_fixedPeers ? ", peers from MKGP2PC_LINK_PEERS" : "");
    }
  }
  return s_state == 1;
}

static int enabled(void) { return configured() && s_active; }

/* 192.168.29.150 is cabinet index 0. -1: not a cabinet address. */
static int cabinet_index(const uint8_t* a) {
  if (a[0] == 192 && a[1] == 168 && a[2] == 29 && a[3] >= 150 && a[3] < 150 + MAX_CABINETS) {
    return a[3] - 150;
  }
  return -1;
}

/* ---- the wire, and a worse one for measurements --------------------------------
 *
 * MKGP2PC_LINK_DELAY=<ms>   every datagram leaves that much later (one way)
 * MKGP2PC_LINK_JITTER=<ms>  plus a random 0..ms each, so they may overtake
 * MKGP2PC_LINK_LOSS=<%>     that share of the datagrams is not sent at all
 * The waiting ones leave from mkgp2pc_netlink_pump(), which runs on every
 * retrace, with every send, receive and select of the game, and every
 * millisecond while the no-graphics build waits (MKGP2PC_HZ). */
typedef struct Late {
  struct Late* next;
  uint64_t at;
  int peer, length;
  uint8_t data[1];
} Late;
static Late* s_late;

static void wire_out(int peer, const uint8_t* data, int length) {
  struct sockaddr_in to;
  if (s_announce == NO_SOCKET || !s_peer[peer].known) {
    return;
  }
  memset(&to, 0, sizeof(to));
  to.sin_family = AF_INET;
  to.sin_addr = s_peer[peer].host;
  to.sin_port = s_peer[peer].port;
  sendto(s_announce, (const char*)data, length, 0, (struct sockaddr*)&to, sizeof(to));
}

static void wire_send(int peer, const uint8_t* data, int length) {
  Late *e, **p;
  s_datagrams++;
  if (s_loss > 0 && rand() % 100 < s_loss) {
    s_lost++;
    return;
  }
  if (s_delay <= 0 && s_jitter <= 0) {
    wire_out(peer, data, length);
    return;
  }
  e = (Late*)malloc(sizeof(Late) + (size_t)length);
  if (e == NULL) {
    return;
  }
  e->at = now_ms() + (uint64_t)(s_delay > 0 ? s_delay : 0) + (s_jitter > 0 ? (uint64_t)(rand() % (s_jitter + 1)) : 0);
  e->peer = peer;
  e->length = length;
  memcpy(e->data, data, (size_t)length);
  for (p = &s_late; *p != NULL && (*p)->at <= e->at; p = &(*p)->next) {
  }
  e->next = *p;
  *p = e;
}

static void late_release(uint64_t now, int all) {
  while (s_late != NULL && (all || s_late->at <= now)) {
    Late* e = s_late;
    s_late = e->next;
    if (!all) {
      wire_out(e->peer, e->data, e->length);
    }
    free(e);
  }
}

/* ---- connections -------------------------------------------------------------
 *
 * A connection of the game is a pair of byte streams between two cabinets'
 * UDP ports. Datagram: 32 bytes of header, then up to MSS bytes of one
 * stream. Fields are in host byte order, like the game's own messages.
 *
 *    0  "MKL2"
 *    4  u8  type: 1 open, 2 opened, 3 data, 4 no such connection
 *    5  u8  flags: 1 the stream ends after these bytes, 2 the sender is
 *           missing bytes before some it already has, 4 "echo" is valid,
 *           8 the sender is the side that opened the connection
 *    6  u16 number of the connection, chosen by the side that opened it
 *    8  4 bytes: game address of the sender
 *   12  u16 game port connected to
 *   14  u16 number of stream bytes that follow the header
 *   16  u32 offset of those bytes in the sender's stream
 *   20  u32 how far the sender has the other stream, complete
 *   24  u32 sender's clock, milliseconds
 *   28  u32 the other side's clock as last received, plus the time since
 *
 * There is no waiting for a lost datagram: every datagram with new bytes
 * also repeats the unconfirmed bytes before them, as far as they fit, and
 * the game sends every frame. Only when that is not enough (more than MSS
 * bytes open, or nothing new to send) the oldest unconfirmed piece is sent
 * again, on the receiver's hint (flag 2) or after 1.5 round trips. Every
 * datagram with bytes is confirmed on the next pump.
 *
 * The game opens one connection per direction and writes on the one it
 * opened, about 250 bytes a frame; the buffers here are far larger than
 * anything it sends. */
#define HEADER 32
#define MSS 1200
#define RING (1u << 18)
#define WINDOW 0x8000u /* bytes sent and not confirmed, at most */
#define MAX_CHANS 48
#define MAX_RANGES 16

enum { T_OPEN = 1, T_OPENED, T_DATA, T_NONE };
enum { F_END = 1, F_GAP = 2, F_ECHO = 4, F_OPENER = 8 };

typedef enum { CH_FREE, CH_OPENING, CH_INCOMING, CH_OPEN, CH_DEAD } ChanState;

typedef struct {
  ChanState state;
  int peer;   /* cabinet index of the other side */
  int opener; /* this side opened it */
  int owned;  /* a socket of the game has it */
  uint16_t id;
  uint16_t gamePort;
  uint8_t *tx, *rx;        /* RING bytes each, indexed by stream offset */
  uint32_t una, sent, end; /* out: confirmed, sent at least once, written by the game */
  uint32_t rxRead, rxNext; /* in: read by the game, complete up to */
  struct {
    uint32_t from, to;
  } range[MAX_RANGES]; /* pieces received beyond rxNext */
  int ranges;
  int ended; /* the other side closed; its stream ends at endAt */
  uint32_t endAt;
  int needAck;
  uint64_t heard, progress, nudged, openAt, quiet;
  int backoff;
  int rtt; /* round trip, smoothed, ms; -1 not measured */
  int haveStamp;
  uint32_t stamp;
  uint64_t stampAt;
} Chan;

static Chan s_chan[MAX_CHANS];
static uint16_t s_nextId;

static int32_t ahead(uint32_t a, uint32_t b) { return (int32_t)(a - b); } /* > 0: a is after b */

static Chan* chan_new(int peer, int opener, uint16_t id, uint16_t gamePort, ChanState state, uint64_t now) {
  for (int i = 0; i < MAX_CHANS; i++) {
    Chan* c = &s_chan[i];
    if (c->state == CH_FREE) {
      memset(c, 0, sizeof(*c));
      c->tx = (uint8_t*)malloc(RING);
      c->rx = (uint8_t*)malloc(RING);
      if (c->tx == NULL || c->rx == NULL) {
        free(c->tx);
        free(c->rx);
        return NULL;
      }
      c->state = state;
      c->peer = peer;
      c->opener = opener;
      c->id = id;
      c->gamePort = gamePort;
      c->rtt = -1;
      c->heard = c->progress = c->quiet = now;
      return c;
    }
  }
  return NULL;
}

static void chan_free(Chan* c) {
  free(c->tx);
  free(c->rx);
  c->tx = c->rx = NULL;
  c->state = CH_FREE;
}

static Chan* chan_find(int peer, int opener, uint16_t id) {
  for (int i = 0; i < MAX_CHANS; i++) {
    Chan* c = &s_chan[i];
    if (c->state != CH_FREE && c->peer == peer && c->opener == opener && c->id == id) {
      return c;
    }
  }
  return NULL;
}

static void emit_raw(int peer, int type, int flags, uint16_t id, uint16_t gamePort, uint32_t offset, uint32_t ack,
                     uint32_t stamp, uint32_t echo, const uint8_t* ring, uint32_t length) {
  uint8_t d[HEADER + MSS];
  uint16_t length16 = (uint16_t)length;
  memcpy(d, "MKL2", 4);
  d[4] = (uint8_t)type;
  d[5] = (uint8_t)flags;
  memcpy(d + 6, &id, 2);
  memcpy(d + 8, s_address, 4);
  memcpy(d + 12, &gamePort, 2);
  memcpy(d + 14, &length16, 2);
  memcpy(d + 16, &offset, 4);
  memcpy(d + 20, &ack, 4);
  memcpy(d + 24, &stamp, 4);
  memcpy(d + 28, &echo, 4);
  for (uint32_t i = 0; i < length; i++) {
    d[HEADER + i] = ring[(offset + i) & (RING - 1)];
  }
  wire_send(peer, d, (int)(HEADER + length));
}

static void emit(Chan* c, int type, int flags, uint32_t offset, uint32_t length, uint64_t now) {
  uint32_t echo = 0;
  if (c->opener) {
    flags |= F_OPENER;
  }
  if (c->ranges > 0) {
    flags |= F_GAP;
  }
  if (c->haveStamp) {
    flags |= F_ECHO;
    echo = c->stamp + (uint32_t)(now - c->stampAt);
  }
  emit_raw(c->peer, type, flags, c->id, c->gamePort, offset, c->rxNext, (uint32_t)now, echo, c->tx, length);
  c->needAck = 0;
  c->quiet = now;
}

/* The oldest bytes not confirmed, once more. */
static void repeat(Chan* c, uint64_t now) {
  uint32_t n = c->sent - c->una;
  emit(c, T_DATA, 0, c->una, n > MSS ? MSS : n, now);
  s_repeats++;
}

/* New bytes of the game, each datagram filled up with what went before. */
static void flush(Chan* c, uint64_t now) {
  if (c->state != CH_OPEN) {
    return;
  }
  while (ahead(c->end, c->sent) > 0 && ahead(c->una + WINDOW, c->sent) > 0) {
    uint32_t from = c->sent;
    uint32_t n = c->end - c->sent;
    uint32_t room = c->una + WINDOW - c->sent;
    if (n > room) {
      n = room;
    }
    if (n > MSS) {
      n = MSS;
    }
    if (c->una == c->sent) {
      c->progress = now;
    }
    c->sent = from + n;
    if (n < MSS) {
      uint32_t back = from - c->una;
      if (back > MSS - n) {
        back = MSS - n;
      }
      from -= back;
      n += back;
    }
    emit(c, T_DATA, 0, from, n, now);
  }
}

static void chan_close(Chan* c, uint64_t now) {
  if (c->state == CH_OPEN) {
    uint32_t n = c->end - c->una; /* the end of the stream, with what is still open if it fits */
    if (n > MSS) {
      n = MSS;
    }
    emit(c, T_DATA, F_END, c->end - n, n, now);
    emit(c, T_DATA, F_END, c->end - n, n, now);
  } else if (c->state == CH_OPENING || c->state == CH_INCOMING) {
    emit_raw(c->peer, T_NONE, c->opener ? F_OPENER : 0, c->id, c->gamePort, 0, 0, 0, 0, NULL, 0);
  }
  chan_free(c);
}

static int listening_on(uint16_t gamePort) {
  for (int i = 0; i < MAX_SOCKETS; i++) {
    if (s_sock[i].kind == LISTENING && s_sock[i].port == gamePort) {
      return 1;
    }
  }
  return 0;
}

static void chan_data(Chan* c, uint32_t offset, const uint8_t* data, uint32_t length) {
  uint32_t from = offset, to = offset + length;
  if (ahead(c->rxNext, from) > 0) { /* the front is known already */
    uint32_t skip = c->rxNext - from;
    if (skip >= length) {
      return;
    }
    data += skip;
    from = c->rxNext;
  }
  if (ahead(to, c->rxRead + RING) > 0) { /* more than the game has room for: it comes again */
    to = c->rxRead + RING;
    if (ahead(to, from) <= 0) {
      return;
    }
  }
  for (uint32_t i = 0; i < to - from; i++) {
    c->rx[(from + i) & (RING - 1)] = data[i];
  }
  if (from != c->rxNext) {
    int at = -1;
    for (int r = 0; r < c->ranges; r++) {
      if (ahead(from, c->range[r].to) <= 0 && ahead(c->range[r].from, to) <= 0) {
        at = r;
        break;
      }
    }
    if (at >= 0) {
      if (ahead(c->range[at].from, from) > 0) {
        c->range[at].from = from;
      }
      if (ahead(to, c->range[at].to) > 0) {
        c->range[at].to = to;
      }
    } else if (c->ranges < MAX_RANGES) {
      c->range[c->ranges].from = from;
      c->range[c->ranges].to = to;
      c->ranges++;
    }
    return;
  }
  c->rxNext = to;
  for (int r = 0; r < c->ranges;) {
    if (ahead(c->range[r].from, c->rxNext) <= 0) {
      if (ahead(c->range[r].to, c->rxNext) > 0) {
        c->rxNext = c->range[r].to;
      }
      c->range[r] = c->range[--c->ranges];
      r = 0;
    } else {
      r++;
    }
  }
}

static void chan_input(const uint8_t* d, int size, const struct sockaddr_in* from, uint64_t now) {
  uint16_t id, gamePort, length;
  uint32_t offset, ack, stamp, echo;
  int type, flags, peer;
  Chan* c;
  if (size < HEADER) {
    s_dropped++;
    return;
  }
  type = d[4];
  flags = d[5];
  memcpy(&id, d + 6, 2);
  memcpy(&gamePort, d + 12, 2);
  memcpy(&length, d + 14, 2);
  memcpy(&offset, d + 16, 4);
  memcpy(&ack, d + 20, 4);
  memcpy(&stamp, d + 24, 4);
  memcpy(&echo, d + 28, 4);
  peer = cabinet_index(d + 8);
  if (peer < 0 || peer == s_announceIndex || length > MSS || HEADER + (int)length != size) {
    s_dropped++;
    return;
  }
  if (!s_peer[peer].known) { /* it knows this cabinet from its MKGP2PC_LINK_PEERS and no announcement comes through */
    s_peer[peer].known = 1;
    s_peer[peer].host = from->sin_addr;
    s_peer[peer].port = from->sin_port;
  }
  s_heard[peer] = now;
  c = chan_find(peer, !(flags & F_OPENER), id);
  if (type == T_NONE) {
    if (c != NULL) {
      c->state = CH_DEAD;
    }
    return;
  }
  if (type == T_OPEN && c == NULL && listening_on(gamePort)) {
    c = chan_new(peer, 0, id, gamePort, CH_INCOMING, now);
  }
  if (c == NULL) {
    emit_raw(peer, T_NONE, (flags & F_OPENER) ? 0 : F_OPENER, id, gamePort, 0, 0, 0, 0, NULL, 0);
    return;
  }
  if (c->state == CH_DEAD) {
    return;
  }
  c->heard = now;
  c->stamp = stamp;
  c->stampAt = now;
  c->haveStamp = 1;
  if (flags & F_ECHO) {
    int32_t sample = (int32_t)((uint32_t)now - echo);
    if (sample >= 0 && sample < 10000) {
      c->rtt = c->rtt < 0 ? sample : c->rtt + (sample - c->rtt) / 8;
    }
  }
  if (type == T_OPEN) {
    emit(c, T_OPENED, 0, 0, 0, now);
    return;
  }
  if (c->state == CH_OPENING) {
    c->state = CH_OPEN;
  }
  if (type != T_DATA) {
    return;
  }
  if (ahead(ack, c->una) > 0 && ahead(ack, c->sent) <= 0) {
    c->una = ack;
    c->progress = now;
    c->backoff = 0;
  }
  if ((flags & F_GAP) && c->una != c->sent && now - c->nudged >= (uint64_t)(c->rtt > 20 ? c->rtt / 2 : 10)) {
    c->nudged = now;
    repeat(c, now);
  }
  if (length > 0) {
    chan_data(c, offset, d + HEADER, length);
    c->needAck = 1;
  }
  if (flags & F_END) {
    c->ended = 1;
    c->endAt = offset + length;
    c->needAck = 1;
  }
}

static void chan_timers(uint64_t now) {
  for (int i = 0; i < MAX_CHANS; i++) {
    Chan* c = &s_chan[i];
    switch (c->state) {
    case CH_OPENING:
      if (s_peer[c->peer].known && (c->openAt == 0 || now - c->openAt >= 100)) {
        c->openAt = now;
        emit(c, T_OPEN, 0, 0, 0, now);
      }
      break;
    case CH_OPEN:
    case CH_INCOMING:
      if (now - c->heard > 30000) { /* the game gives up long before (180 polls without data) */
        c->state = CH_DEAD;
        break;
      }
      flush(c, now);
      if (c->una != c->sent) {
        uint64_t wait = (uint64_t)((c->rtt < 0 ? 100 : c->rtt) * 3 / 2 + 20) << (c->backoff > 4 ? 4 : c->backoff);
        if (now - c->progress >= (wait > 1000 ? 1000 : wait)) {
          c->progress = now;
          c->backoff++;
          repeat(c, now);
        }
      }
      if (c->needAck || now - c->quiet >= 500) {
        emit(c, T_DATA, 0, c->sent, 0, now);
      }
      break;
    case CH_DEAD:
      if (!c->owned) {
        chan_free(c);
      }
      break;
    default:
      break;
    }
  }
}

static void announce_input(const uint8_t* msg, const struct sockaddr_in* from, uint64_t now);

/* Everything that has arrived at one of the two ports. */
static void wire_input(HostSocket source, uint64_t now) {
  uint8_t d[HEADER + MSS + 16];
  struct sockaddr_in from;
  for (int errors = 0; source != NO_SOCKET && errors < 16;) {
    socklen_t_ size = sizeof(from);
    int n = (int)recvfrom(source, (char*)d, sizeof(d), 0, (struct sockaddr*)&from, &size);
    if (n < 0) {
      if (would_block(host_error())) {
        break;
      }
      errors++; /* Windows reports here that an earlier datagram found no port open */
      continue;
    }
    if (n == ANNOUNCE_SIZE && memcmp(d, "MKGP2PC1", 8) == 0) {
      announce_input(d, &from, now);
    } else if (n >= 4 && memcmp(d, "MKL2", 4) == 0 && source == s_announce && s_active) {
      chan_input(d, n, &from, now);
    } else {
      s_dropped++;
    }
  }
}

void mkgp2pc_netlink_pump(void) {
  uint64_t now;
  if (s_state != 1 || !s_active || s_announce == NO_SOCKET) {
    return;
  }
  now = now_ms();
  late_release(now, 0);
  wire_input(s_announce, now);
  chan_timers(now);
}

/* ---- announcements --------------------------------------------------------- */

#define OBSERVE_PORT (s_base + 64 + MAX_CABINETS)

static int bound_to(HostSocket s, int port) {
  struct sockaddr_in a;
  socklen_t_ size = sizeof(a);
  return getsockname(s, (struct sockaddr*)&a, &size) == 0 && ntohs(a.sin_port) == port;
}

/* Every copy listens here as well, several on one PC side by side: the
 * announcements sent to the broadcast address arrive at all of them. That
 * is how a copy without a cabinet number hears the other PCs. */
static void observe_open(void) {
  struct sockaddr_in a;
  int on = 1;
  if (s_observe != NO_SOCKET) {
    return;
  }
  s_observe = socket(AF_INET, SOCK_DGRAM, 0);
  if (s_observe == NO_SOCKET) {
    return;
  }
  setsockopt(s_observe, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on));
#ifdef SO_REUSEPORT
  setsockopt(s_observe, SOL_SOCKET, SO_REUSEPORT, (const char*)&on, sizeof(on));
#endif
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  a.sin_port = htons((uint16_t)OBSERVE_PORT);
  if (bind(s_observe, (struct sockaddr*)&a, sizeof(a)) != 0) {
    close_host(s_observe);
    s_observe = NO_SOCKET;
    return;
  }
  non_blocking(s_observe);
}

/* Cabinets on this PC, for a copy that has no number and so no announcement
 * port of its own: a number is taken when its port cannot be bound. */
static void local_probe(uint64_t now) {
  for (int k = 0; k < MAX_CABINETS; k++) {
    struct sockaddr_in a;
    HostSocket s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == NO_SOCKET) {
      return;
    }
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)(s_base + 64 + k));
    if (bind(s, (struct sockaddr*)&a, sizeof(a)) != 0) {
      s_heard[k] = now;
    }
    close_host(s);
  }
}

static void announce_open(void) {
  int index = cabinet_index(s_address);
  struct sockaddr_in a;
  int on = 1;
  if (index == s_announceIndex && (index < 0 || s_announce != NO_SOCKET)) {
    return;
  }
  if (s_announce != NO_SOCKET) {
    close_host(s_announce);
    s_announce = NO_SOCKET;
  }
  s_announceIndex = index;
  if (index < 0) {
    return;
  }
  s_announce = socket(AF_INET, SOCK_DGRAM, 0);
  if (s_announce == NO_SOCKET) {
    return;
  }
  setsockopt(s_announce, SOL_SOCKET, SO_BROADCAST, (const char*)&on, sizeof(on));
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  a.sin_port = htons((uint16_t)(s_base + 64 + index));
  if (bind(s_announce, (struct sockaddr*)&a, sizeof(a)) != 0) {
    uint64_t start = now_ms(); /* a copy without a number may be looking at the port this instant (local_probe) */
    while (now_ms() - start < 3 && bind(s_announce, (struct sockaddr*)&a, sizeof(a)) != 0) {
    }
  }
  if (!bound_to(s_announce, s_base + 64 + index)) {
    fprintf(stderr, "link: UDP port %d is in use: is another copy running as cabinet %d? (MKGP2PC_PCB_ID)\n",
            s_base + 64 + index, index + 1);
    close_host(s_announce);
    s_announce = NO_SOCKET;
    return;
  }
  non_blocking(s_announce);
  no_reset_reports(s_announce);
  on = 1 << 20; /* a frame's worth from three cabinets waits here between two pumps */
  setsockopt(s_announce, SOL_SOCKET, SO_RCVBUF, (const char*)&on, sizeof(on));
  s_announced = 0;
}

static void announce_input(const uint8_t* msg, const struct sockaddr_in* from, uint64_t now) {
  int index = cabinet_index(msg + 8);
  int local;
  if (index < 0 || index == s_announceIndex) {
    return;
  }
  s_heard[index] = now;
  if (s_peer[index].fixed || !s_active) {
    return;
  }
  /* A cabinet on this PC is heard twice, through the broadcast and
   * directly: the direct address wins while it keeps coming. */
  local = from->sin_addr.s_addr == htonl(INADDR_LOOPBACK);
  if (local) {
    s_peer[index].local = now;
  } else if (s_peer[index].known && now - s_peer[index].local < 5000) {
    return;
  }
  if (!s_peer[index].known || s_peer[index].host.s_addr != from->sin_addr.s_addr) {
    char text[32];
    snprintf(text, sizeof(text), "%s", inet_ntoa(from->sin_addr));
    fprintf(stderr, "link: cabinet %d is at %s\n", index + 1, text);
  }
  s_peer[index].known = 1;
  s_peer[index].host = from->sin_addr;
  s_peer[index].port = from->sin_port; /* every announcement leaves from the cabinet's own port */
}

static void announce_tick(uint64_t now) {
  uint8_t msg[ANNOUNCE_SIZE];
  observe_open();
  wire_input(s_observe, now);
  if (s_announce == NO_SOCKET) {
    if (now - s_probed >= 1000) {
      s_probed = now;
      local_probe(now);
    }
    return;
  }
  wire_input(s_announce, now);
  if (s_announced == 0 || now - s_announced >= 1000) {
    static const uint32_t targets[2] = {INADDR_BROADCAST, INADDR_LOOPBACK};
    s_announced = now;
    memcpy(msg, "MKGP2PC1", 8);
    memcpy(msg + 8, s_address, 4);
    for (int t = 0; t < 2; t++) {
      for (int k = 0; k <= MAX_CABINETS; k++) { /* the last one is the port everybody listens on */
        struct sockaddr_in to;
        if (k == s_announceIndex || (k == MAX_CABINETS && t == 1)) {
          continue;
        }
        memset(&to, 0, sizeof(to));
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = htonl(targets[t]);
        to.sin_port = htons((uint16_t)(s_base + 64 + k));
        sendto(s_announce, (const char*)msg, sizeof(msg), 0, (struct sockaddr*)&to, sizeof(to));
      }
    }
    for (int k = 0; k < MAX_CABINETS; k++) { /* the ones named by hand may be out of reach of a broadcast */
      if (s_peer[k].fixed && k != s_announceIndex) {
        struct sockaddr_in to;
        memset(&to, 0, sizeof(to));
        to.sin_family = AF_INET;
        to.sin_addr = s_peer[k].host;
        to.sin_port = s_peer[k].port;
        sendto(s_announce, (const char*)msg, sizeof(msg), 0, (struct sockaddr*)&to, sizeof(to));
      }
    }
  }
}

/* ---- sockets --------------------------------------------------------------- */

static Sock* sock(uint32_t s) {
  if (s >= MAX_SOCKETS || s_sock[s].kind == FREE) {
    return NULL;
  }
  return &s_sock[s];
}

static int new_sock(Kind kind) {
  for (int i = 1; i < MAX_SOCKETS; i++) { /* 0 stays free: the game uses slot 0 for calls without a socket */
    if (s_sock[i].kind == FREE) {
      Sock* k = &s_sock[i];
      memset(k, 0, sizeof(*k));
      k->kind = kind;
      k->chan = -1;
      k->timeout[0] = k->timeout[1] = k->timeout[2] = 6000;
      return i;
    }
  }
  return -1;
}

static void release(Sock* k) {
  uint64_t now = now_ms();
  if (k->chan >= 0) {
    chan_close(&s_chan[k->chan], now);
  }
  if (k->kind == LISTENING) {
    k->kind = FREE;
    if (!listening_on(k->port)) { /* callers nobody will answer any more */
      for (int i = 0; i < MAX_CHANS; i++) {
        if (s_chan[i].state == CH_INCOMING && s_chan[i].gamePort == k->port) {
          chan_close(&s_chan[i], now);
        }
      }
    }
  }
  k->kind = FREE;
}

/* Game socket address: u8 length, u8 family, u16 port, 4 address bytes. The
 * game stores the port as a number, so it is in host order here. */
static void read_address(uint32_t at, uint8_t* address, uint16_t* port) {
  uint8_t raw[8];
  mkgp2pc_mediaboard_load(at, raw, sizeof(raw));
  memcpy(port, raw + 2, 2);
  memcpy(address, raw + 4, 4);
}

static int32_t fail(Sock* k, int error) {
  if (k != NULL) {
    k->error = error;
  }
  return -1;
}

static int32_t do_bind(Sock* k, uint32_t at) {
  uint8_t address[4];
  uint16_t port;
  read_address(at, address, &port);
  if (k->kind != FRESH) {
    return fail(k, E_INVAL);
  }
  k->port = port;
  TRACE("bind game port %u", port);
  return 0;
}

static void do_connect(Sock* k, uint64_t now) {
  int index = cabinet_index(k->target);
  k->kind = CONNECTING;
  k->since = now;
  k->chan = -1;
  /* Any other address (the camera, 192.168.29.104-107) is one nobody answers at. */
  if (index >= 0 && index != s_announceIndex && s_announce != NO_SOCKET) {
    Chan* c;
    if (s_nextId == 0) {
      s_nextId = (uint16_t)(((uint32_t)now * 2654435761u) >> 16);
    }
    c = chan_new(index, 1, s_nextId++, k->targetPort, CH_OPENING, now);
    if (c != NULL) {
      c->owned = 1;
      k->chan = (int)(c - s_chan);
    }
  }
}

/* 1 connected, 0 not yet, -1 failed (k->error set) */
static int connect_poll(Sock* k, uint64_t now) {
  Chan* c = k->chan >= 0 ? &s_chan[k->chan] : NULL;
  if (c != NULL && c->state == CH_OPEN) {
    k->kind = CONNECTED;
    return 1;
  }
  if (c != NULL && c->state == CH_DEAD) {
    chan_free(c);
    k->chan = -1;
    k->error = E_CONNREFUSED;
    return -1;
  }
  if (now - k->since >= k->timeout[2]) {
    if (c != NULL) {
      chan_close(c, now);
      k->chan = -1;
    }
    k->error = E_TIMEDOUT;
    return -1;
  }
  return 0;
}

/* A connection waiting to be accepted on this listener: its index, or -1. */
static int accept_poll(Sock* k) {
  for (int i = 0; i < MAX_CHANS; i++) {
    if (s_chan[i].state == CH_INCOMING && s_chan[i].gamePort == k->port) {
      return i;
    }
  }
  return -1;
}

static int32_t accept_finish(Sock* k, int chan, uint32_t addressAt, uint32_t lengthAt) {
  Chan* c = &s_chan[chan];
  int s = new_sock(CONNECTED);
  if (s < 0) {
    chan_close(c, now_ms());
    return fail(k, E_NFILE);
  }
  s_sock[s].chan = chan;
  c->state = CH_OPEN;
  c->owned = 1;
  if (addressAt != 0) {
    uint8_t raw[16];
    uint8_t length[4] = {0, 0, 0, 16}; /* the game reads the length byte-swapped (acsocket_m8.c) */
    memset(raw, 0, sizeof(raw));
    raw[0] = 16;
    raw[1] = 2;
    memcpy(raw + 2, &k->port, 2);
    raw[4] = 192, raw[5] = 168, raw[6] = 29, raw[7] = (uint8_t)(150 + c->peer);
    mkgp2pc_mediaboard_store(addressAt, raw, sizeof(raw));
    if (lengthAt != 0) {
      mkgp2pc_mediaboard_store(lengthAt, length, sizeof(length));
    }
  }
  TRACE("accept on %d: socket %d from cabinet %d", (int)(k - s_sock), s, c->peer + 1);
  return s;
}

/* The game's descriptor sets: 8 words, each stored byte-swapped
 * (src/game/TCPConnSend.c TCPConn_SendMask). */
static uint32_t swap32(uint32_t v) { return (v << 24) | ((v << 8) & 0xFF0000) | ((v >> 8) & 0xFF00) | (v >> 24); }

static int chan_readable(const Chan* c) {
  return c->rxNext != c->rxRead || c->state == CH_DEAD || (c->ended && c->rxRead == c->endAt);
}

static int chan_writable(const Chan* c) { return c->state == CH_DEAD || c->end - c->una < RING / 2; }

static int select_set(uint32_t at, uint32_t count, int forWrite) {
  uint32_t set[8];
  int n = 0;
  if (at == 0) {
    return 0;
  }
  mkgp2pc_mediaboard_load(at, (uint8_t*)set, sizeof(set));
  for (uint32_t s = 0; s < 256; s++) {
    uint32_t bit = swap32(1u << (s & 31));
    if (set[s >> 5] & bit) {
      Sock* k = s < count ? sock(s) : NULL;
      int on = 0;
      if (k != NULL && k->kind == CONNECTED) {
        on = forWrite ? chan_writable(&s_chan[k->chan]) : chan_readable(&s_chan[k->chan]);
      } else if (k != NULL && k->kind == LISTENING && !forWrite) {
        on = accept_poll(k) >= 0;
      }
      if (on) {
        n++;
      } else {
        set[s >> 5] &= ~bit;
      }
    }
  }
  mkgp2pc_mediaboard_store(at, (const uint8_t*)set, sizeof(set));
  return n;
}

static int32_t do_send(Sock* k, uint32_t at, uint32_t length) {
  static uint8_t data[0x80000];
  Chan* c;
  uint32_t room;
  if (k->kind != CONNECTED || s_chan[k->chan].state != CH_OPEN) {
    return fail(k, E_NOTCONN);
  }
  if (length > sizeof(data)) {
    return fail(k, E_INVAL);
  }
  c = &s_chan[k->chan];
  room = RING - (c->end - c->una);
  if (room == 0 && length > 0) {
    return fail(k, E_WOULDBLOCK); /* the other side has stopped confirming */
  }
  if (length > room) {
    length = room;
  }
  mkgp2pc_mediaboard_load(at, data, length);
  for (uint32_t i = 0; i < length; i++) {
    c->tx[(c->end + i) & (RING - 1)] = data[i];
  }
  c->end += length;
  s_sent += length;
  flush(c, now_ms());
  return (int32_t)length;
}

static int32_t do_recv(Sock* k, uint32_t at, uint32_t length) {
  static uint8_t data[0x80000];
  Chan* c;
  uint32_t have;
  if (k->kind != CONNECTED) {
    return fail(k, E_NOTCONN);
  }
  c = &s_chan[k->chan];
  have = c->rxNext - c->rxRead;
  if (have == 0) {
    if (c->state == CH_DEAD || (c->ended && c->rxRead == c->endAt)) {
      return 0; /* closed by the other side */
    }
    return fail(k, E_WOULDBLOCK);
  }
  if (length > sizeof(data)) {
    length = sizeof(data);
  }
  if (length > have) {
    length = have;
  }
  for (uint32_t i = 0; i < length; i++) {
    data[i] = c->rx[(c->rxRead + i) & (RING - 1)];
  }
  c->rxRead += length;
  s_received += length;
  mkgp2pc_mediaboard_store(at, data, length);
  return (int32_t)length;
}

/* ---- commands --------------------------------------------------------------- */

/* Commands by the value amsock.c stores in the packet. */
enum {
  CMD_ACCEPT = 0x104,
  CMD_BIND = 0x204,
  CMD_CLOSE = 0x304,
  CMD_CONNECT = 0x404,
  CMD_INET_ADDR = 0x604,
  CMD_LISTEN = 0x804,
  CMD_RECV = 0x904,
  CMD_SEND = 0xA04,
  CMD_SOCKET = 0xB04,
  CMD_SELECT = 0xC04,
  CMD_SET_TIMEOUTS = 0x1004,
  CMD_GET_LAST_ERROR = 0x1104,
  CMD_MODIFY_MY_IP = 0x1504,
};

static int parse_ip(uint32_t at, uint32_t length, uint8_t* out) {
  char text[0x41];
  unsigned a, b, c, d;
  if (length > 0x40) {
    length = 0x40;
  }
  mkgp2pc_mediaboard_load(at, (uint8_t*)text, length);
  text[length] = 0;
  if (sscanf(text, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
    return 0;
  }
  out[0] = (uint8_t)a, out[1] = (uint8_t)b, out[2] = (uint8_t)c, out[3] = (uint8_t)d;
  return 1;
}

int mkgp2pc_netlink_command(uint16_t command, const uint32_t* arg, uint32_t* result) {
  Sock* k;
  int32_t r = -1;
  uint64_t now;
  switch (command) {
  case CMD_ACCEPT: case CMD_BIND: case CMD_CLOSE: case CMD_CONNECT: case CMD_INET_ADDR: case CMD_LISTEN:
  case CMD_RECV: case CMD_SEND: case CMD_SOCKET: case CMD_SELECT: case CMD_SET_TIMEOUTS:
  case CMD_GET_LAST_ERROR: case CMD_MODIFY_MY_IP:
    break;
  default:
    return MKGP2PC_NETLINK_UNKNOWN;
  }
  if (!enabled()) {
    return MKGP2PC_NETLINK_UNKNOWN;
  }
  now = now_ms();
  k = sock(arg[0]);
  if (command == CMD_SELECT || command == CMD_SEND || command == CMD_RECV) {
    mkgp2pc_netlink_pump(); /* what has arrived since the retrace */
  }
  if (s_trace > 1 && command != CMD_SELECT && command != CMD_SEND && command != CMD_RECV) {
    TRACE("%8llu command %04x %x %x %x %x", (unsigned long long)now, command, arg[0], arg[1], arg[2], arg[3]);
  }
  switch (command) {
  case CMD_MODIFY_MY_IP:
    if (!parse_ip(arg[0], arg[1], s_address)) {
      break;
    }
    fprintf(stderr, "link: this is %u.%u.%u.%u (cabinet %d)\n", s_address[0], s_address[1], s_address[2], s_address[3],
            cabinet_index(s_address) + 1);
    announce_open();
    r = 0;
    break;
  case CMD_INET_ADDR: {
    uint8_t a[4];
    if (!parse_ip(arg[0], arg[1], a)) {
      memset(a, 0xFF, 4);
    }
    memcpy(result, a, 4); /* address bytes as they are: the game does not swap this answer */
    return MKGP2PC_NETLINK_DONE;
  }
  case CMD_SOCKET:
    r = new_sock(FRESH);
    break;
  case CMD_SET_TIMEOUTS:
    if (k != NULL) {
      k->timeout[0] = arg[1], k->timeout[1] = arg[2], k->timeout[2] = arg[3];
      r = 0;
    }
    break;
  case CMD_GET_LAST_ERROR:
    r = k != NULL ? k->error : E_BADF;
    break;
  case CMD_CLOSE:
    if (k != NULL) {
      TRACE("close %u", arg[0]);
      release(k);
      r = 0;
    }
    break;
  case CMD_BIND:
    r = k != NULL ? do_bind(k, arg[1]) : -1;
    break;
  case CMD_LISTEN:
    if (k != NULL && k->kind == FRESH) {
      k->kind = LISTENING;
      r = 0;
    } else {
      fail(k, E_INVAL);
    }
    break;
  case CMD_SELECT: {
    /* The time limit (arg[4]) is not waited for: the game passes zero. */
    r = select_set(arg[1], arg[0], 0) + select_set(arg[2], arg[0], 1);
    if (arg[3] != 0) {
      uint8_t none[0x20] = {0};
      mkgp2pc_mediaboard_store(arg[3], none, sizeof(none));
    }
    break;
  }
  case CMD_SEND:
    r = k != NULL ? do_send(k, arg[1], arg[2]) : -1;
    break;
  case CMD_RECV:
    r = k != NULL ? do_recv(k, arg[1], arg[2]) : -1;
    break;
  case CMD_CONNECT:
    if (k == NULL || k->kind != FRESH) {
      fail(k, E_INVAL);
      break;
    }
    read_address(arg[1], k->target, &k->targetPort);
    do_connect(k, now);
    TRACE("connect %u to %u.%u.%u.%u:%u", arg[0], k->target[0], k->target[1], k->target[2], k->target[3], k->targetPort);
    return MKGP2PC_NETLINK_LATER;
  case CMD_ACCEPT:
    if (k == NULL || k->kind != LISTENING) {
      fail(k, E_INVAL);
      break;
    }
    k->since = now;
    return MKGP2PC_NETLINK_LATER;
  }
  *result = swap32((uint32_t)r);
  return MKGP2PC_NETLINK_DONE;
}

/* A command that answered MKGP2PC_NETLINK_LATER: has it finished? */
int mkgp2pc_netlink_poll(uint16_t command, const uint32_t* arg, uint32_t* result) {
  Sock* k = sock(arg[0]);
  uint64_t now = now_ms();
  int32_t r = -1;
  if (k == NULL) {
    *result = swap32((uint32_t)-1);
    return MKGP2PC_NETLINK_DONE;
  }
  if (command == CMD_CONNECT) {
    int state = k->kind == CONNECTING ? connect_poll(k, now) : -1;
    if (state == 0) {
      return MKGP2PC_NETLINK_LATER;
    }
    r = state > 0 ? 0 : -1;
    TRACE("connect %u: %s", arg[0], state > 0 ? "connected" : k->error == E_TIMEDOUT ? "nobody there" : "refused");
    if (state < 0) {
      k->kind = FRESH; /* the game closes it */
      k->chan = -1;
    }
  } else {
    int g = k->kind == LISTENING ? accept_poll(k) : -1;
    if (g >= 0) {
      r = accept_finish(k, g, arg[1], arg[2]);
    } else if (k->kind == LISTENING && now - k->since < k->timeout[1]) {
      return MKGP2PC_NETLINK_LATER;
    } else {
      k->error = E_TIMEDOUT;
    }
  }
  *result = swap32((uint32_t)r);
  return MKGP2PC_NETLINK_DONE;
}

/* ---- pace of a linked race ----------------------------------------------------
 *
 * Every client tells the host with each input how many of the host's frames
 * it holds unplayed ("queued"). The arcade program's host waits a retrace,
 * up to four per frame, whenever any client reports two or more
 * (src/game/VSMode_FrameUpdate.c). Between cabinets on one cable that count
 * only grows when a client is slower than the host. Over a longer way it
 * also jumps every time datagrams arrive bunched (jitter, a loss made up by
 * the next datagram), and the host's waiting then slows the race for
 * everybody while the count it reacts to is gone again.
 *
 * So the host asks here (port/patches/src/game/VSMode_FrameUpdate.c.patch).
 * A count only tells that the client is behind when it has stayed up without
 * a break: at 2 or more for two seconds, 3 for two thirds of a second, 4 for
 * a quarter, 6 for four frames. Then the host waits one retrace and the
 * counting starts again. What a client holds in between is its reserve
 * against late frames: it grows by itself when the client runs dry (the
 * client then waits for two frames, as on the cabinet) and shrinks through
 * this rule when it is more than the connection needs.
 *
 * MKGP2PC_LINK_PACE=0: the arcade program's rule. */
static uint64_t s_holds;

int mkgp2pc_link_backlog(int kart, int queued) {
  static const int level[4] = {2, 3, 4, 6}, frames[4] = {120, 40, 15, 4};
  static int run[MAX_CABINETS][4];
  static int pace = -1;
  int behind = 0;
  if (pace < 0) {
    const char* e = getenv("MKGP2PC_LINK_PACE");
    pace = e == NULL || e[0] != '0';
  }
  if (!pace || kart < 0 || kart >= MAX_CABINETS) {
    if (queued >= 2) {
      s_holds++;
    }
    return queued;
  }
  for (int t = 0; t < 4; t++) {
    if (queued < level[t]) {
      run[kart][t] = 0;
    } else if (++run[kart][t] >= frames[t]) {
      behind = 1;
    }
  }
  if (!behind) {
    return 0;
  }
  memset(run[kart], 0, sizeof(run[kart]));
  s_holds++;
  return queued;
}

/* ---- MKGP2PC_LINK_PROBE=1: what a race over the link feels like -------------
 *
 * Once a second, from the game's own variables (src/game/VSMode_FrameUpdate.c,
 * PCBCheckRecv.c): how fast the race runs (seconds of race clock per second),
 * how many retraces passed, and how long this cabinet's pedals and wheel take
 * to come back in the frame the race is run from (the host's frame; on the
 * host its own), and in how many retraces the race clock stood still. The
 * delay is measured whenever the local input changes. */
extern float g_raceTimeRemaining;
extern unsigned char g_isCommHost;
extern int g_currentSceneState;
extern int lbl_806CF140; /* this cabinet's kart */
extern unsigned char PCBComm_BuildBroadcastSlot_bss_805A60C4[];
void* RingBuffer_GetKartSlot(int kart);

static void probe_tick(uint64_t now) {
  static uint64_t second, changed, sum;
  static float clockThen, local[3];
  static int waiting, count, worst, retraces, still;
  static float clockLast;
  float* mine = (float*)(PCBComm_BuildBroadcastSlot_bss_805A60C4 + 0x2C);
  retraces++;
  if (g_raceTimeRemaining == clockLast) {
    still++; /* a retrace in which the race did not move: the host waited, or this client had no frame */
  }
  clockLast = g_raceTimeRemaining;
  if (g_currentSceneState == 0x2C && lbl_806CF140 >= 0 && lbl_806CF140 < 4) {
    float* back = (float*)RingBuffer_GetKartSlot(lbl_806CF140);
    if (memcmp(local, mine, sizeof(local)) != 0) {
      memcpy(local, mine, sizeof(local));
      changed = now;
      waiting = 1;
    }
    if (waiting && back != NULL && memcmp(local, back, sizeof(local)) == 0) {
      int ms = (int)(now - changed);
      waiting = 0;
      sum += (uint64_t)ms;
      count++;
      if (ms > worst) {
        worst = ms;
      }
    }
  }
  if (now - second >= 1000) {
    if (second != 0 && g_currentSceneState == 0x2C) {
      fprintf(stderr,
              "probe: %s race clock %.2f s/s, %d retraces, input comes back after %d ms (worst %d, %d changes), "
              "%d retraces without a step\n",
              g_isCommHost ? "host" : "client", (double)(clockThen - g_raceTimeRemaining) * 1000.0 / (double)(now - second),
              retraces, count ? (int)(sum / (uint64_t)count) : -1, worst, count, still);
    }
    second = now;
    clockThen = g_raceTimeRemaining;
    retraces = count = worst = still = 0;
    sum = 0;
  }
}

void mkgp2pc_netlink_tick(void) {
  if (configured()) {
    uint64_t now = now_ms();
    announce_tick(now);
    if (!s_active) {
      return;
    }
    mkgp2pc_netlink_pump();
    if (s_probe) {
      probe_tick(now);
    }
    if (s_trace && now - s_reported >= 5000) {
      int open = 0, rtt = -1;
      for (int i = 0; i < MAX_CHANS; i++) {
        if (s_chan[i].state == CH_OPEN) {
          open++;
          if (s_chan[i].rtt > rtt) {
            rtt = s_chan[i].rtt;
          }
        }
      }
      s_reported = now;
      TRACE("%llu bytes sent, %llu received; %llu datagrams, %llu repeats, %llu withheld, %llu not understood; "
            "%d connections, round trip up to %d ms; the host waited %llu times",
            (unsigned long long)s_sent, (unsigned long long)s_received, (unsigned long long)s_datagrams,
            (unsigned long long)s_repeats, (unsigned long long)s_lost, (unsigned long long)s_dropped, open, rtt,
            (unsigned long long)s_holds);
    }
  }
}

/* Called from GameBoot_Init (port/patches/src/game/GameBoot_Init.cpp.patch)
 * with the cabinet number of the settings just loaded. The backup memory
 * starts empty on every run, which makes every copy cabinet 1;
 * MKGP2PC_PCB_ID=1..4 says otherwise. The test menu changes it for the rest
 * of the run as on the cabinet. */
void mkgp2pc_boot_cabinet(signed char* id) {
  const char* e = getenv("MKGP2PC_PCB_ID");
  if (e != NULL && atoi(e) >= 1 && atoi(e) <= MAX_CABINETS) {
    *id = (signed char)atoi(e);
  }
}

/* ---- for the game flow (linkflow.c) ------------------------------------------ */

/* The link at run time. Off: every link socket is closed, this copy stops
 * announcing itself and every socket command fails, as with MKGP2PC_LINK=0
 * (which stays off whatever is asked here). On: commands work again; the
 * game sets its address anew (MODIFY_MY_IP) and opens its sockets. */
void mkgp2pc_netlink_set_active(int on) {
  on = on != 0;
  if (on == s_active) {
    return;
  }
  s_active = on;
  if (!on && s_state == 1) {
    for (int i = 0; i < MAX_SOCKETS; i++) {
      if (s_sock[i].kind != FREE) {
        release(&s_sock[i]);
      }
    }
    for (int i = 0; i < MAX_CHANS; i++) {
      if (s_chan[i].state != CH_FREE) {
        chan_close(&s_chan[i], now_ms());
      }
    }
    late_release(0, 1);
    if (s_announce != NO_SOCKET) {
      close_host(s_announce);
      s_announce = NO_SOCKET;
    }
    s_announceIndex = -1;
    memset(s_address, 0, sizeof(s_address));
    for (int k = 0; k < MAX_CABINETS; k++) {
      if (!s_peer[k].fixed) {
        memset(&s_peer[k], 0, sizeof(s_peer[k]));
      }
    }
  }
  TRACE("link %s", on ? "on" : "off");
}

/* Bit k: cabinet k+1 was heard in the last 3 s, on this PC or on the network.
 * This copy's own number is not in it (unless mkgp2pc_netlink_address_ok()
 * says it belongs to somebody else). Also answers while the link is off
 * and before the game has set an address; the first answer after a start may
 * come up to a second too early to be complete. */
unsigned mkgp2pc_netlink_heard_mask(void) {
  unsigned mask = 0;
  uint64_t now;
  if (!configured()) {
    return 0;
  }
  now = now_ms();
  if (s_announce == NO_SOCKET && (s_probed == 0 || now - s_probed >= 1000)) {
    s_probed = now;
    local_probe(now);
  }
  for (int k = 0; k < MAX_CABINETS; k++) {
    if ((k != s_announceIndex || s_announce == NO_SOCKET) && s_heard[k] != 0 && now - s_heard[k] < 3000) {
      mask |= 1u << k;
    }
  }
  return mask;
}

/* After the game has set its address: 0 when that cabinet number's port
 * could not be taken, because another copy on this PC has the number. */
int mkgp2pc_netlink_address_ok(void) {
  return s_state != 1 || !s_active || s_announceIndex < 0 || s_announce != NO_SOCKET;
}
