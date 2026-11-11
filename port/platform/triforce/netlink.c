/*
 * Cabinet link: the network board's socket commands on host sockets.
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
 *  - Game address 192.168.29.(150+k), port 5000+c is real TCP port
 *    BASE + 16*k + c on the host where cabinet k+1 runs (BASE 29500,
 *    MKGP2PC_LINK_PORT). Several cabinets on one PC do not collide.
 *  - Where a cabinet runs is learned from announcements: every second each
 *    cabinet sends a small UDP datagram to the broadcast address and to this
 *    PC, to ports BASE + 64 + k of the other three. MKGP2PC_LINK_PEERS
 *    ("2=192.168.1.20,3=192.168.1.21") names hosts by hand instead.
 *  - A connection begins with 8 bytes from the connecting side, "MKL1" and
 *    its game address. The accepting side reports that address to the game.
 *    So this is a link between PC builds only; it is not the wire format of
 *    the arcade board (and the game's own messages are in host byte order).
 *  - A cabinet that has not been heard from behaves like an address nobody
 *    answers at: the connect fails after the game's connect timeout.
 *
 * MKGP2PC_LINK=0 turns all of it off (every socket command fails, as before
 * this file existed). MKGP2PC_TRACE_LINK=1 logs the commands.
 *
 * All calls come from the game with the port's "interrupt" lock held or from
 * the retrace tick; nothing here blocks for long and there are no threads.
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
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
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
#endif

#include "mkgp2pc/triforce.h"

/* Error numbers as the game's log names them (src/game/AcError_LogIfError.c
 * knows 9, 23, 24, 50, 55): the BSD ones. */
enum { E_BADF = 9, E_INVAL = 22, E_NFILE = 23, E_WOULDBLOCK = 35, E_NETDOWN = 50, E_NOTCONN = 57, E_TIMEDOUT = 60, E_CONNREFUSED = 61 };

#define MAX_SOCKETS 64
#define MAX_CABINETS 4
#define MAX_GREETING 8
#define GREETING_SIZE 8
#define ANNOUNCE_SIZE 12

typedef enum { FREE, FRESH, LISTENING, CONNECTING, CONNECTED } Kind;

typedef struct {
  HostSocket host;
  uint32_t address; /* game address, bytes in memory order */
  uint8_t data[GREETING_SIZE];
  int have;
  uint64_t since;
} Greeting;

typedef struct {
  Kind kind;
  HostSocket host;
  int error;
  uint32_t timeout[3]; /* send, receive, connect; milliseconds */
  uint16_t port;       /* game port bound to */
  /* connect */
  uint8_t target[4];
  uint16_t targetPort;
  int started; /* the host connect has been issued */
  uint64_t since;
  /* accept: connections that have not sent their greeting yet */
  Greeting greeting[MAX_GREETING];
} Sock;

typedef struct {
  int known;
  struct in_addr host;
  uint64_t local; /* when it was last heard on this PC */
} Peer;

static Sock s_sock[MAX_SOCKETS];
static Peer s_peer[MAX_CABINETS];
static int s_fixedPeers;
static uint8_t s_address[4]; /* this cabinet's game address; 0 until the game sets it */
static HostSocket s_announce = NO_SOCKET;
static int s_announceIndex = -1;
static uint64_t s_announced;
static int s_state; /* 0 not looked at, 1 on, -1 off */
static int s_trace;
static int s_base = 29500;
static uint64_t s_sent, s_received, s_reported; /* bytes, for the trace */
static int s_probe;
static int s_delay, s_jitter, s_loss; /* measurement options, see late_send() */

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

static int enabled(void) {
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
    while (s_state == 1 && peers != NULL && *peers != 0) { /* "2=host,3=host" */
      char name[128];
      int id = atoi(peers), n = 0;
      const char* p = strchr(peers, '=');
      const char* end = strchr(peers, ',');
      if (p != NULL && (end == NULL || p < end)) {
        for (p++; *p != 0 && *p != ',' && n < (int)sizeof(name) - 1; p++) {
          name[n++] = *p;
        }
        name[n] = 0;
        if (id >= 1 && id <= MAX_CABINETS) {
          struct addrinfo hints, *info = NULL;
          memset(&hints, 0, sizeof(hints));
          hints.ai_family = AF_INET;
          hints.ai_socktype = SOCK_STREAM;
          if (getaddrinfo(name, NULL, &hints, &info) == 0 && info != NULL) {
            s_peer[id - 1].host = ((struct sockaddr_in*)info->ai_addr)->sin_addr;
            s_peer[id - 1].known = 1;
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
      fprintf(stderr, "link: cabinet link on, ports %d-%d%s\n", s_base, s_base + 64 + MAX_CABINETS - 1,
              s_fixedPeers ? ", peers from MKGP2PC_LINK_PEERS" : "");
    }
  }
  return s_state == 1;
}

/* 192.168.29.150 is cabinet index 0. -1: not a cabinet address. */
static int cabinet_index(const uint8_t* a) {
  if (a[0] == 192 && a[1] == 168 && a[2] == 29 && a[3] >= 150 && a[3] < 150 + MAX_CABINETS) {
    return a[3] - 150;
  }
  return -1;
}

static int real_port(int index, uint16_t gamePort) {
  if (index >= 0 && gamePort >= 5000 && gamePort < 5016) {
    return s_base + 16 * index + (gamePort - 5000);
  }
  return gamePort; /* not the cabinet link (the camera, 192.168.29.104-107): as it is */
}

static int ready(HostSocket s, int forWrite, int* failed) {
  fd_set set, bad;
  struct timeval tv = {0, 0};
  int n;
  FD_ZERO(&set);
  FD_ZERO(&bad);
  FD_SET(s, &set);
  FD_SET(s, &bad);
  n = select((int)s + 1, forWrite ? NULL : &set, forWrite ? &set : NULL, &bad, &tv);
  if (failed != NULL) {
    *failed = n > 0 && FD_ISSET(s, &bad);
  }
  return n > 0 && FD_ISSET(s, &set);
}

static void tune(HostSocket s) {
  int on = 1;
  non_blocking(s);
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&on, sizeof(on)); /* one small message a frame */
}

/* ---- announcements --------------------------------------------------------- */

static void announce_open(void) {
  int index = cabinet_index(s_address);
  struct sockaddr_in a;
  int on = 1;
  if (index == s_announceIndex) {
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
    fprintf(stderr, "link: UDP port %d is in use: is another copy running as cabinet %d? (MKGP2PC_PCB_ID)\n",
            s_base + 64 + index, index + 1);
    close_host(s_announce);
    s_announce = NO_SOCKET;
    return;
  }
  non_blocking(s_announce);
  s_announced = 0;
}

static void announce_tick(uint64_t now) {
  uint8_t msg[ANNOUNCE_SIZE];
  struct sockaddr_in from;
  if (s_announce == NO_SOCKET) {
    return;
  }
  for (;;) {
    socklen_t_ size = sizeof(from);
    int n = (int)recvfrom(s_announce, (char*)msg, sizeof(msg), 0, (struct sockaddr*)&from, &size);
    int index, local;
    if (n < 0) {
      break;
    }
    if (n != ANNOUNCE_SIZE || memcmp(msg, "MKGP2PC1", 8) != 0) {
      continue;
    }
    index = cabinet_index(msg + 8);
    if (index < 0 || index == s_announceIndex || s_fixedPeers) {
      continue;
    }
    /* A cabinet on this PC is heard twice, through the broadcast and
     * directly: the direct address wins while it keeps coming. */
    local = from.sin_addr.s_addr == htonl(INADDR_LOOPBACK);
    if (local) {
      s_peer[index].local = now;
    } else if (s_peer[index].known && now - s_peer[index].local < 5000) {
      continue;
    }
    if (!s_peer[index].known || s_peer[index].host.s_addr != from.sin_addr.s_addr) {
      char text[32];
      snprintf(text, sizeof(text), "%s", inet_ntoa(from.sin_addr));
      fprintf(stderr, "link: cabinet %d is at %s\n", index + 1, text);
    }
    s_peer[index].known = 1;
    s_peer[index].host = from.sin_addr;
  }
  if (s_announced == 0 || now - s_announced >= 1000) {
    static const uint32_t targets[2] = {INADDR_BROADCAST, INADDR_LOOPBACK};
    s_announced = now;
    memcpy(msg, "MKGP2PC1", 8);
    memcpy(msg + 8, s_address, 4);
    for (int t = 0; t < 2; t++) {
      for (int k = 0; k < MAX_CABINETS; k++) {
        struct sockaddr_in to;
        if (k == s_announceIndex) {
          continue;
        }
        memset(&to, 0, sizeof(to));
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = htonl(targets[t]);
        to.sin_port = htons((uint16_t)(s_base + 64 + k));
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

static int new_sock(HostSocket host, Kind kind) {
  for (int i = 1; i < MAX_SOCKETS; i++) { /* 0 stays free: the game uses slot 0 for calls without a socket */
    if (s_sock[i].kind == FREE) {
      Sock* k = &s_sock[i];
      memset(k, 0, sizeof(*k));
      k->kind = kind;
      k->host = host;
      k->timeout[0] = k->timeout[1] = k->timeout[2] = 6000;
      for (int g = 0; g < MAX_GREETING; g++) {
        k->greeting[g].host = NO_SOCKET;
      }
      return i;
    }
  }
  return -1;
}

static void late_drop(int socket);

static void release(Sock* k) {
  late_drop((int)(k - s_sock));
  for (int g = 0; g < MAX_GREETING; g++) {
    if (k->greeting[g].host != NO_SOCKET) {
      close_host(k->greeting[g].host);
    }
  }
  if (k->host != NO_SOCKET) {
    close_host(k->host);
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
  struct sockaddr_in a;
  uint8_t address[4];
  uint16_t port;
  int on = 1;
  read_address(at, address, &port);
  if (k->kind != FRESH) {
    return fail(k, E_INVAL);
  }
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  a.sin_port = htons((uint16_t)real_port(cabinet_index(s_address), port));
#ifndef _WIN32
  /* a listener closed a moment ago must not block the next one */
  setsockopt(k->host, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on));
#else
  (void)on;
#endif
  if (bind(k->host, (struct sockaddr*)&a, sizeof(a)) != 0) {
    TRACE("bind to real port %d failed (%d)", ntohs(a.sin_port), host_error());
    return fail(k, E_INVAL);
  }
  k->port = port;
  TRACE("bind game port %u = real port %d", port, ntohs(a.sin_port));
  return 0;
}

static int connect_start(Sock* k) {
  int index = cabinet_index(k->target);
  struct sockaddr_in a;
  if (index < 0 || !s_peer[index].known) {
    return 0; /* not heard from yet: keep waiting, up to the game's time limit */
  }
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr = s_peer[index].host;
  a.sin_port = htons((uint16_t)real_port(index, k->targetPort));
  k->started = 1;
  if (connect(k->host, (struct sockaddr*)&a, sizeof(a)) != 0 && !would_block(host_error())) {
    return -1;
  }
  return 1;
}

/* 1 connected, 0 not yet, -1 failed (k->error set) */
static int connect_poll(Sock* k, uint64_t now) {
  int bad = 0;
  if (!k->started) {
    if (connect_start(k) < 0) {
      k->error = E_CONNREFUSED;
      return -1;
    }
  }
  if (k->started && ready(k->host, 1, &bad) && !bad) {
    int error = 0;
    socklen_t_ size = sizeof(error);
    getsockopt(k->host, SOL_SOCKET, SO_ERROR, (char*)&error, &size);
    if (error == 0) {
      uint8_t hello[GREETING_SIZE];
      memcpy(hello, "MKL1", 4);
      memcpy(hello + 4, s_address, 4);
      if (send(k->host, (const char*)hello, sizeof(hello), 0) != (int)sizeof(hello)) {
        k->error = E_NOTCONN;
        return -1;
      }
      k->kind = CONNECTED;
      return 1;
    }
    bad = 1;
  }
  if (bad) {
    k->error = E_CONNREFUSED;
    return -1;
  }
  if (now - k->since >= k->timeout[2]) {
    k->error = E_TIMEDOUT;
    return -1;
  }
  return 0;
}

/* A connection that has sent its greeting: its index, or -1. */
static int accept_poll(Sock* k, uint64_t now) {
  for (int g = 0; g < MAX_GREETING; g++) {
    Greeting* e = &k->greeting[g];
    if (e->host == NO_SOCKET) {
      struct sockaddr_in from;
      socklen_t_ size = sizeof(from);
      HostSocket s = accept(k->host, (struct sockaddr*)&from, &size);
      if (s == NO_SOCKET) {
        continue;
      }
      tune(s);
      e->host = s;
      e->have = 0;
      e->since = now;
    }
    if (e->have < GREETING_SIZE) {
      int n = (int)recv(e->host, (char*)e->data + e->have, GREETING_SIZE - e->have, 0);
      if (n > 0) {
        e->have += n;
      } else if (n == 0 || !would_block(host_error()) || now - e->since > 3000) {
        close_host(e->host); /* not one of ours */
        e->host = NO_SOCKET;
        continue;
      }
    }
    if (e->have == GREETING_SIZE) {
      if (memcmp(e->data, "MKL1", 4) != 0) {
        close_host(e->host);
        e->host = NO_SOCKET;
        continue;
      }
      memcpy(&e->address, e->data + 4, 4);
      return g;
    }
  }
  return -1;
}

static int32_t accept_finish(Sock* k, int g, uint32_t addressAt, uint32_t lengthAt) {
  Greeting* e = &k->greeting[g];
  int s = new_sock(e->host, CONNECTED);
  if (s < 0) {
    close_host(e->host);
    e->host = NO_SOCKET;
    return fail(k, E_NFILE);
  }
  e->host = NO_SOCKET;
  if (addressAt != 0) {
    uint8_t raw[16];
    uint8_t length[4] = {0, 0, 0, 16}; /* the game reads the length byte-swapped (acsocket_m8.c) */
    memset(raw, 0, sizeof(raw));
    raw[0] = 16;
    raw[1] = 2;
    memcpy(raw + 2, &k->port, 2);
    memcpy(raw + 4, e->data + 4, 4);
    mkgp2pc_mediaboard_store(addressAt, raw, sizeof(raw));
    if (lengthAt != 0) {
      mkgp2pc_mediaboard_store(lengthAt, length, sizeof(length));
    }
  }
  TRACE("accept on %d: socket %d from %u.%u.%u.%u", (int)(k - s_sock), s, e->data[4], e->data[5], e->data[6], e->data[7]);
  return s;
}

/* The game's descriptor sets: 8 words, each stored byte-swapped
 * (src/game/TCPConnSend.c TCPConn_SendMask). */
static uint32_t swap32(uint32_t v) { return (v << 24) | ((v << 8) & 0xFF0000) | ((v >> 8) & 0xFF00) | (v >> 24); }

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
        on = ready(k->host, forWrite, NULL);
      } else if (k != NULL && k->kind == LISTENING && !forWrite) {
        on = accept_poll(k, now_ms()) >= 0;
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

/* ---- a worse network, for measurements ---------------------------------------
 *
 * MKGP2PC_LINK_DELAY=<ms>   every send leaves that much later (one way)
 * MKGP2PC_LINK_JITTER=<ms>  plus a random 0..ms
 * MKGP2PC_LINK_LOSS=<%>     that share of the sends waits for a retransmission
 *                           (200 ms or twice the delay, whichever is longer)
 * Order is kept, as TCP keeps it: data behind a late piece is late too. The
 * queue is emptied from the retrace tick and from mkgp2pc_netlink_pump(),
 * which the no-graphics build calls every millisecond while it waits. */
typedef struct Late {
  struct Late* next;
  int socket;
  uint64_t at;
  uint32_t length, done;
  uint8_t data[1];
} Late;
static Late *s_late, *s_lateEnd;
static uint64_t s_lateLast;

static void late_drop(int socket) {
  Late** p = &s_late;
  s_lateEnd = NULL;
  while (*p != NULL) {
    if ((*p)->socket == socket) {
      Late* dead = *p;
      *p = dead->next;
      free(dead);
    } else {
      s_lateEnd = *p;
      p = &(*p)->next;
    }
  }
}

void mkgp2pc_netlink_pump(void) {
  uint64_t now;
  if (s_late == NULL) {
    return;
  }
  now = now_ms();
  while (s_late != NULL && s_late->at <= now) {
    Late* e = s_late;
    Sock* k = &s_sock[e->socket];
    int n = (int)send(k->host, (const char*)e->data + e->done, (int)(e->length - e->done), 0);
    if (n > 0) {
      e->done += (uint32_t)n;
      s_sent += (uint32_t)n;
    } else if (would_block(host_error())) {
      return;
    } else {
      e->done = e->length; /* the link is gone; the game notices on its own */
    }
    if (e->done == e->length) {
      s_late = e->next;
      if (s_late == NULL) {
        s_lateEnd = NULL;
      }
      free(e);
    }
  }
}

static int32_t late_send(Sock* k, const uint8_t* data, uint32_t length) {
  Late* e = (Late*)malloc(sizeof(Late) + length);
  uint64_t at = now_ms() + (uint64_t)s_delay + (s_jitter > 0 ? (uint64_t)(rand() % (s_jitter + 1)) : 0);
  if (e == NULL) {
    return fail(k, E_WOULDBLOCK);
  }
  if (s_loss > 0 && rand() % 100 < s_loss) {
    at += 2 * s_delay > 200 ? (uint64_t)(2 * s_delay) : 200;
  }
  if (at < s_lateLast) {
    at = s_lateLast;
  }
  s_lateLast = at;
  e->next = NULL;
  e->socket = (int)(k - s_sock);
  e->at = at;
  e->length = length;
  e->done = 0;
  memcpy(e->data, data, length);
  if (s_lateEnd != NULL) {
    s_lateEnd->next = e;
  } else {
    s_late = e;
  }
  s_lateEnd = e;
  return (int32_t)length;
}

static int32_t do_send(Sock* k, uint32_t at, uint32_t length) {
  static uint8_t data[0x80000];
  uint64_t start = now_ms();
  uint32_t done = 0;
  if (k->kind != CONNECTED) {
    return fail(k, E_NOTCONN);
  }
  if (length > sizeof(data)) {
    return fail(k, E_INVAL);
  }
  mkgp2pc_mediaboard_load(at, data, length);
  if (s_delay > 0 || s_jitter > 0 || s_loss > 0) {
    return late_send(k, data, length);
  }
  while (done < length) {
    int n = (int)send(k->host, (const char*)data + done, (int)(length - done), 0);
    if (n > 0) {
      done += (uint32_t)n;
      s_sent += (uint32_t)n;
    } else if (!would_block(host_error())) {
      return fail(k, E_NOTCONN);
    } else if (now_ms() - start > 200) { /* the other side has stopped reading */
      return done > 0 ? (int32_t)done : fail(k, E_WOULDBLOCK);
    }
  }
  return (int32_t)done;
}

static int32_t do_recv(Sock* k, uint32_t at, uint32_t length) {
  static uint8_t data[0x80000];
  int n;
  if (k->kind != CONNECTED) {
    return fail(k, E_NOTCONN);
  }
  if (length > sizeof(data)) {
    length = sizeof(data);
  }
  n = (int)recv(k->host, (char*)data, (int)length, 0);
  if (n < 0) {
    return fail(k, would_block(host_error()) ? E_WOULDBLOCK : E_NOTCONN);
  }
  s_received += (uint32_t)n;
  mkgp2pc_mediaboard_store(at, data, (uint32_t)n);
  return n; /* 0: closed by the other side */
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
  case CMD_SOCKET: {
    HostSocket s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == NO_SOCKET) {
      break;
    }
    tune(s);
    r = new_sock(s, FRESH);
    if (r < 0) {
      close_host(s);
    }
    break;
  }
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
    if (k != NULL && k->kind == FRESH && listen(k->host, (int)arg[1]) == 0) {
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
    k->kind = CONNECTING;
    k->started = 0;
    k->since = now;
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
    }
  } else {
    int g = k->kind == LISTENING ? accept_poll(k, now) : -1;
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

/* ---- MKGP2PC_LINK_PROBE=1: what a race over the link feels like -------------
 *
 * Once a second, from the game's own variables (src/game/VSMode_FrameUpdate.c,
 * PCBCheckRecv.c): how fast the race runs (seconds of race clock per second),
 * how many retraces passed, and how long this cabinet's pedals and wheel take
 * to come back in the frame the race is run from (the host's frame; on the
 * host its own). The delay is measured whenever the local input changes. */
extern float g_raceTimeRemaining;
extern unsigned char g_isCommHost;
extern int g_currentSceneState;
extern int lbl_806CF140; /* this cabinet's kart */
extern unsigned char PCBComm_BuildBroadcastSlot_bss_805A60C4[];
void* RingBuffer_GetKartSlot(int kart);

static void probe_tick(uint64_t now) {
  static uint64_t second, changed, sum;
  static float clockThen, local[3];
  static int waiting, count, worst, retraces;
  float* mine = (float*)(PCBComm_BuildBroadcastSlot_bss_805A60C4 + 0x2C);
  retraces++;
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
      fprintf(stderr, "probe: %s race clock %.2f s/s, %d retraces, input comes back after %d ms (worst %d, %d changes)\n",
              g_isCommHost ? "host" : "client", (double)(clockThen - g_raceTimeRemaining) * 1000.0 / (double)(now - second),
              retraces, count ? (int)(sum / (uint64_t)count) : -1, worst, count);
    }
    second = now;
    clockThen = g_raceTimeRemaining;
    retraces = count = worst = 0;
    sum = 0;
  }
}

void mkgp2pc_netlink_tick(void) {
  if (s_state == 1) {
    uint64_t now = now_ms();
    announce_tick(now);
    mkgp2pc_netlink_pump();
    if (s_probe) {
      probe_tick(now);
    }
    if (s_trace && now - s_reported >= 5000) {
      s_reported = now;
      TRACE("%llu bytes sent, %llu received", (unsigned long long)s_sent, (unsigned long long)s_received);
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
