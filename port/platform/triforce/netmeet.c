/*
 * Cabinet link over the internet: finding the others by a room name.
 *
 * netlink.c carries the link in UDP datagrams between one port per cabinet
 * and needs to know, per cabinet, an address its datagrams reach. On a LAN
 * the cabinets announce themselves. Behind home routers each has to learn
 *
 *  1. under which public address and port its own UDP port appears outside.
 *     It asks a STUN server (RFC 8489, Binding request) FROM THAT PORT; the
 *     answer names the sender as the server saw it.
 *  2. the same about the others. Every cabinet tells a small HTTPS server
 *     (port/server/rendezvous, a Cloudflare Worker) its room name, cabinet
 *     number and the address from step 1 and gets the others' back.
 *
 * netlink.c then sends its announcements to those addresses once a second.
 * The first one out opens this router for the answers; as both sides do it,
 * the datagrams pass in both directions after a second or two ("hole
 * punching"). No game data goes through either server. Routers that give
 * every destination another outside port (symmetric NAT) cannot be passed
 * this way; that is noticed (the two STUN destinations report different
 * ports) and logged.
 *
 *   MKGP2PC_LINK_ROOM=<any text>   the room; the same on all players' PCs
 *   MKGP2PC_LINK_SERVER=<url>      the rendezvous server, https://name.workers.dev
 *   MKGP2PC_LINK_STUN=host:port[,host:port]
 *                                  default stun.cloudflare.com:3478,stun.cloudflare.com:53
 *
 * The server gives the cabinet number, asked once at boot: the one of
 * MKGP2PC_PCB_ID if nobody in the room has it, else the first free one.
 *
 * Name lookups and HTTP requests wait, so they run on a thread of their own
 * that only exchanges plain values with the rest under s_lock. The STUN
 * datagrams go through netlink.c's socket on the game's thread.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
typedef CRITICAL_SECTION Lock;
#define lock_init(l) InitializeCriticalSection(l)
#define lock(l) EnterCriticalSection(l)
#define unlock(l) LeaveCriticalSection(l)
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef pthread_mutex_t Lock;
#define lock_init(l) pthread_mutex_init(l, NULL)
#define lock(l) pthread_mutex_lock(l)
#define unlock(l) pthread_mutex_unlock(l)
#endif

#include "netmeet.h"

#define MAX_CABINETS 4
#define STUN_SERVERS 2
#define STUN_COOKIE 0x2112A442u
#define ASK_EVERY_MS 2000 /* the rendezvous server; it forgets a member after 15 s */

static int s_state; /* 0 not looked at, 1 on, -1 off */
static char s_room[400]; /* percent-encoded */
static char s_server[256];
static char s_stunName[STUN_SERVERS][128];
static int s_stunPort[STUN_SERVERS];
static int s_stunCount;
static char s_token[17];

/* shared with the thread */
static Lock s_lock;
static struct sockaddr_in s_stun[STUN_SERVERS];
static int s_stunKnown;
static int s_id;                /* this cabinet's number, 0 none: what the thread reports */
static uint32_t s_mapped;       /* public address (network order), 0 unknown */
static uint16_t s_mappedPort;   /* network order */
static struct {
  int present;
  uint32_t host;
  uint16_t port;
} s_listed[MAX_CABINETS];
static int s_listedNew;

/* the game's thread only */
static uintptr_t s_socket = (uintptr_t)-1;
static uint8_t s_txid[STUN_SERVERS][12];
static uint64_t s_asked[STUN_SERVERS], s_heardStun[STUN_SERVERS];
static uint16_t s_seenPort[STUN_SERVERS];
static uint32_t s_seenHost[STUN_SERVERS];
static int s_natSaid;
static int s_threadStarted;
static struct {
  int placed;
  uint32_t host;
  uint16_t port;
} s_placed[MAX_CABINETS];

static uint64_t now_ms(void) {
#ifdef _WIN32
  return GetTickCount64();
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

static void sleep_ms(int ms) {
#ifdef _WIN32
  Sleep((DWORD)ms);
#else
  usleep((useconds_t)ms * 1000);
#endif
}

static uint64_t s_seed;
static uint32_t random32(void) {
  if (s_seed == 0) {
#ifdef _WIN32
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    s_seed = (uint64_t)t.QuadPart ^ ((uint64_t)GetCurrentProcessId() << 32) ^ GetTickCount64();
#else
    FILE* f = fopen("/dev/urandom", "rb");
    if (f != NULL) {
      if (fread(&s_seed, sizeof(s_seed), 1, f) != 1) {
        s_seed = 0;
      }
      fclose(f);
    }
    s_seed ^= now_ms() ^ ((uint64_t)getpid() << 32);
#endif
    s_seed |= 1;
  }
  s_seed ^= s_seed << 13;
  s_seed ^= s_seed >> 7;
  s_seed ^= s_seed << 17;
  return (uint32_t)(s_seed >> 16);
}

/* The room name as UTF-8, whatever the code page of the process. */
static void read_text(const char* name, char* out, int cap) {
  out[0] = 0;
#ifdef _WIN32
  {
    wchar_t wide[256], wname[64];
    DWORD n;
    MultiByteToWideChar(CP_ACP, 0, name, -1, wname, 64);
    n = GetEnvironmentVariableW(wname, wide, 256);
    if (n > 0 && n < 256) {
      WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, cap, NULL, NULL);
      out[cap - 1] = 0;
    }
  }
#else
  {
    const char* e = getenv(name);
    if (e != NULL) {
      snprintf(out, (size_t)cap, "%s", e);
    }
  }
#endif
}

static int configured(void) {
  if (s_state == 0) {
    char room[128], stun[256];
    const char* p;
    int n = 0;
    read_text("MKGP2PC_LINK_ROOM", room, sizeof(room));
    read_text("MKGP2PC_LINK_SERVER", s_server, sizeof(s_server));
    read_text("MKGP2PC_LINK_STUN", stun, sizeof(stun));
    for (p = room; *p != 0 && n < (int)sizeof(s_room) - 4; p++) {
      unsigned char c = (unsigned char)*p;
      if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_') {
        s_room[n++] = (char)c;
      } else {
        n += snprintf(s_room + n, 4, "%%%02X", c);
      }
    }
    s_room[n] = 0;
    n = (int)strlen(s_server);
    while (n > 0 && s_server[n - 1] == '/') {
      s_server[--n] = 0;
    }
    if (stun[0] == 0) {
      /* Two destinations, to see whether the router keeps one outside port
       * for both (Cloudflare's STUN answers on 3478 and on 53). */
      snprintf(stun, sizeof(stun), "stun.cloudflare.com:3478,stun.cloudflare.com:53");
    }
    for (p = stun; *p != 0 && s_stunCount < STUN_SERVERS;) {
      const char* end = strchr(p, ',');
      const char* colon;
      int length = end != NULL ? (int)(end - p) : (int)strlen(p);
      char one[128];
      snprintf(one, sizeof(one), "%.*s", length, p);
      colon = strrchr(one, ':');
      s_stunPort[s_stunCount] = colon != NULL ? atoi(colon + 1) : 3478;
      if (colon != NULL) {
        one[colon - one] = 0;
      }
      if (one[0] != 0 && s_stunPort[s_stunCount] > 0 && s_stunPort[s_stunCount] < 65536) {
        snprintf(s_stunName[s_stunCount], sizeof(s_stunName[0]), "%s", one);
        s_stunCount++;
      }
      p = end != NULL ? end + 1 : p + length;
    }
    s_state = -1;
    if (s_room[0] != 0 && s_server[0] == 0) {
      fprintf(stderr, "link: room: MKGP2PC_LINK_ROOM is set but MKGP2PC_LINK_SERVER is not; no room\n");
    } else if (s_room[0] != 0 && (strchr(s_server, '\'') != NULL || strncmp(s_server, "http", 4) != 0)) {
      fprintf(stderr, "link: room: MKGP2PC_LINK_SERVER must be an http(s) address; no room\n");
    } else if (s_room[0] != 0 && s_stunCount > 0) {
      snprintf(s_token, sizeof(s_token), "%08x%08x", (unsigned)random32(), (unsigned)random32());
      lock_init(&s_lock);
      s_state = 1;
      fprintf(stderr, "link: room: on, server %s, STUN %s:%d\n", s_server, s_stunName[0], s_stunPort[0]);
    }
  }
  return s_state == 1;
}

int mkgp2pc_meet_on(void) { return configured(); }

/* ---- HTTP ---------------------------------------------------------------------
 * One GET; the answer's text in `out`. 0 when nothing came. Waits up to
 * about five seconds. */
#ifdef _WIN32
static int http_get(const char* url, char* out, int cap) {
  wchar_t wide[1024], host[256], path[1024], extra[768];
  URL_COMPONENTS parts;
  HINTERNET session = NULL, connection = NULL, request = NULL;
  int total = 0;
  out[0] = 0;
  if (MultiByteToWideChar(CP_UTF8, 0, url, -1, wide, 1024) == 0) {
    return 0;
  }
  memset(&parts, 0, sizeof(parts));
  parts.dwStructSize = sizeof(parts);
  parts.lpszHostName = host;
  parts.dwHostNameLength = 256;
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = 256;
  parts.lpszExtraInfo = extra;
  parts.dwExtraInfoLength = 768;
  if (!WinHttpCrackUrl(wide, 0, 0, &parts)) {
    return 0;
  }
  wcsncat(path, extra, 1023 - wcslen(path));
  session = WinHttpOpen(L"mkgp2-pc", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (session != NULL) {
    WinHttpSetTimeouts(session, 4000, 4000, 4000, 4000);
    connection = WinHttpConnect(session, host, parts.nPort, 0);
  }
  if (connection != NULL) {
    request = WinHttpOpenRequest(connection, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
  }
  if (request != NULL && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
      WinHttpReceiveResponse(request, NULL)) {
    DWORD got = 0;
    while (total < cap - 1 && WinHttpReadData(request, out + total, (DWORD)(cap - 1 - total), &got) && got > 0) {
      total += (int)got;
    }
    out[total] = 0;
  }
  if (request != NULL) {
    WinHttpCloseHandle(request);
  }
  if (connection != NULL) {
    WinHttpCloseHandle(connection);
  }
  if (session != NULL) {
    WinHttpCloseHandle(session);
  }
  return total > 0;
}
#else
/* No HTTPS library in the Linux build: the curl program does it. The
 * address is made of the server (no quote in it, checked), the
 * percent-encoded room, hex digits and numbers. */
static int http_get(const char* url, char* out, int cap) {
  char command[1200];
  FILE* f;
  int total = 0;
  out[0] = 0;
  snprintf(command, sizeof(command), "curl -sS -m 5 -A mkgp2-pc '%s' 2>/dev/null", url);
  f = popen(command, "r");
  if (f == NULL) {
    return 0;
  }
  total = (int)fread(out, 1, (size_t)cap - 1, f);
  out[total > 0 ? total : 0] = 0;
  pclose(f);
  return total > 0;
}
#endif

/* The answer's "id N" (0 none); what else it says goes to the log once. */
static int parse_answer(const char* text, int wantPeers) {
  static char last[64];
  int id = 0, seen[MAX_CABINETS] = {0, 0, 0, 0};
  uint32_t host[MAX_CABINETS];
  uint16_t port[MAX_CABINETS];
  const char* p = text;
  char problem[64] = "";
  while (*p != 0) {
    int k, a, b, c, d, udp;
    if (sscanf(p, "id %d", &k) == 1 && k >= 1 && k <= MAX_CABINETS) {
      id = k;
    } else if (sscanf(p, "peer %d %d.%d.%d.%d %d", &k, &a, &b, &c, &d, &udp) == 6 && k >= 1 && k <= MAX_CABINETS &&
               (unsigned)a < 256 && (unsigned)b < 256 && (unsigned)c < 256 && (unsigned)d < 256 && udp > 0 && udp < 65536) {
      seen[k - 1] = 1;
      host[k - 1] = htonl(((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d);
      port[k - 1] = htons((uint16_t)udp);
    } else if (problem[0] == 0 && (strncmp(p, "taken", 5) == 0 || strncmp(p, "full", 4) == 0 || strncmp(p, "bad", 3) == 0)) {
      snprintf(problem, sizeof(problem), "%.*s", (int)strcspn(p, "\r\n"), p);
    }
    p += strcspn(p, "\n");
    if (*p == '\n') {
      p++;
    }
  }
  if (id == 0 && problem[0] == 0) {
    snprintf(problem, sizeof(problem), "the server's answer was not understood");
  }
  if (strcmp(problem, last) != 0) {
    snprintf(last, sizeof(last), "%s", problem);
    if (strncmp(problem, "taken", 5) == 0) {
      fprintf(stderr, "link: room: cabinet number%s is another player's\n", problem + 5);
    } else if (strncmp(problem, "full", 4) == 0) {
      fprintf(stderr, "link: room: the room has four players already\n");
    } else if (problem[0] != 0) {
      fprintf(stderr, "link: room: %s\n", problem);
    }
  }
  if (id != 0 && wantPeers) {
    lock(&s_lock);
    for (int k = 0; k < MAX_CABINETS; k++) {
      s_listed[k].present = seen[k];
      if (seen[k]) {
        s_listed[k].host = host[k];
        s_listed[k].port = port[k];
      }
    }
    s_listedNew = 1;
    unlock(&s_lock);
  }
  return id;
}

static void ask(int id, uint32_t mapped, uint16_t mappedPort, char* answer, int cap) {
  char url[1024];
  int n = snprintf(url, sizeof(url), "%s/v1/room/%s?token=%s&id=%d", s_server, s_room, s_token, id);
  if (mapped != 0) {
    const uint8_t* a = (const uint8_t*)&mapped;
    snprintf(url + n, sizeof(url) - (size_t)n, "&ip=%u.%u.%u.%u&port=%u", a[0], a[1], a[2], a[3], (unsigned)ntohs(mappedPort));
  }
  http_get(url, answer, cap);
}

int mkgp2pc_meet_boot_id(int wanted) {
  char answer[512];
  int id;
  if (!configured()) {
    return 0;
  }
  ask(wanted, 0, 0, answer, sizeof(answer));
  if (answer[0] == 0) {
    fprintf(stderr, "link: room: no answer from %s; the cabinet number stays as it is\n", s_server);
    return 0;
  }
  id = parse_answer(answer, 0);
  if (id == 0 && wanted != 0 && strncmp(answer, "taken", 5) == 0) { /* then whichever is free */
    ask(0, 0, 0, answer, sizeof(answer));
    id = parse_answer(answer, 0);
  }
  return id;
}

/* ---- the thread ----------------------------------------------------------------- */

#ifdef _WIN32
static DWORD WINAPI meet_thread(LPVOID unused) {
#else
static void* meet_thread(void* unused) {
#endif
  int silent = 0;
  (void)unused;
  for (;;) {
    int known, id;
    uint32_t mapped;
    uint16_t mappedPort;
    lock(&s_lock);
    known = s_stunKnown;
    id = s_id;
    mapped = s_mapped;
    mappedPort = s_mappedPort;
    unlock(&s_lock);
    if (!known) {
      struct sockaddr_in found[STUN_SERVERS];
      int ok = 1;
      for (int i = 0; i < s_stunCount && ok; i++) {
        struct addrinfo hints, *info = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(s_stunName[i], NULL, &hints, &info) == 0 && info != NULL) {
          found[i] = *(struct sockaddr_in*)info->ai_addr;
          found[i].sin_port = htons((uint16_t)s_stunPort[i]);
          freeaddrinfo(info);
        } else {
          ok = 0;
          if (silent++ == 0) {
            fprintf(stderr, "link: room: cannot find the STUN server \"%s\"\n", s_stunName[i]);
          }
        }
      }
      if (ok) {
        lock(&s_lock);
        memcpy(s_stun, found, sizeof(found));
        s_stunKnown = 1;
        unlock(&s_lock);
      } else {
        sleep_ms(3000);
        continue;
      }
    }
    if (id != 0 && mapped != 0) {
      char answer[512];
      ask(id, mapped, mappedPort, answer, sizeof(answer));
      if (answer[0] != 0) {
        parse_answer(answer, 1);
      }
      sleep_ms(ASK_EVERY_MS);
    } else {
      sleep_ms(100);
    }
  }
#ifdef _WIN32
  return 0;
#else
  return NULL;
#endif
}

static void start_thread(void) {
  if (!s_threadStarted) {
    s_threadStarted = 1;
#ifdef _WIN32
    CloseHandle(CreateThread(NULL, 0, meet_thread, NULL, 0, NULL));
#else
    pthread_t t;
    if (pthread_create(&t, NULL, meet_thread, NULL) == 0) {
      pthread_detach(t);
    }
#endif
  }
}

/* ---- STUN ----------------------------------------------------------------------- */

static void stun_ask(uintptr_t socket_, int server, const struct sockaddr_in* to, uint64_t now) {
  uint8_t msg[20];
  uint32_t a = random32(), b = random32();
  memset(msg, 0, sizeof(msg));
  msg[1] = 1; /* Binding request, no attributes */
  msg[4] = 0x21;
  msg[5] = 0x12;
  msg[6] = 0xA4;
  msg[7] = 0x42;
  memcpy(s_txid[server], "MK2", 3);
  s_txid[server][3] = (uint8_t)server;
  memcpy(s_txid[server] + 4, &a, 4);
  memcpy(s_txid[server] + 8, &b, 4);
  memcpy(msg + 8, s_txid[server], 12);
  s_asked[server] = now;
#ifdef _WIN32
  sendto((SOCKET)socket_, (const char*)msg, sizeof(msg), 0, (const struct sockaddr*)to, sizeof(*to));
#else
  sendto((int)socket_, msg, sizeof(msg), 0, (const struct sockaddr*)to, sizeof(*to));
#endif
}

int mkgp2pc_meet_datagram(const uint8_t* d, int n, const void* from_) {
  int server, length, found = 0;
  uint32_t host = 0;
  uint16_t port = 0;
  (void)from_;
  if (s_state != 1 || n < 20 || d[0] != 0x01 || d[1] != 0x01 || d[4] != 0x21 || d[5] != 0x12 || d[6] != 0xA4 || d[7] != 0x42 ||
      memcmp(d + 8, "MK2", 3) != 0) {
    return 0;
  }
  server = d[11];
  length = (d[2] << 8) | d[3];
  if (server >= s_stunCount || memcmp(d + 8, s_txid[server], 12) != 0 || 20 + length > n) {
    return 1; /* an answer to an older question */
  }
  for (int at = 20; at + 4 <= 20 + length;) {
    int type = (d[at] << 8) | d[at + 1];
    int size = (d[at + 2] << 8) | d[at + 3];
    const uint8_t* v = d + at + 4;
    if (at + 4 + size > 20 + length) {
      break;
    }
    if ((type == 0x0020 || (type == 0x0001 && found == 0)) && size >= 8 && v[1] == 1) {
      uint8_t raw[6];
      memcpy(raw, v + 2, 6);
      if (type == 0x0020) { /* XOR-MAPPED-ADDRESS */
        raw[0] ^= 0x21;
        raw[1] ^= 0x12;
        raw[2] ^= 0x21;
        raw[3] ^= 0x12;
        raw[4] ^= 0xA4;
        raw[5] ^= 0x42;
        found = 2;
      } else {
        found = 1;
      }
      memcpy(&port, raw, 2);
      memcpy(&host, raw + 2, 4);
    }
    at += 4 + ((size + 3) & ~3);
  }
  if (found != 0 && host != 0 && port != 0) {
    uint64_t now = now_ms();
    s_heardStun[server] = now;
    s_seenHost[server] = host;
    s_seenPort[server] = port;
    if (server == 0) {
      int changed;
      lock(&s_lock);
      changed = s_mapped != host || s_mappedPort != port;
      s_mapped = host;
      s_mappedPort = port;
      unlock(&s_lock);
      if (changed) {
        const uint8_t* a = (const uint8_t*)&host;
        fprintf(stderr, "link: room: this cabinet is reached from outside at %u.%u.%u.%u:%u\n", a[0], a[1], a[2], a[3],
                (unsigned)ntohs(port));
        s_natSaid = 0;
      }
    }
    if (!s_natSaid && s_stunCount > 1 && s_heardStun[0] != 0 && s_heardStun[1] != 0) {
      s_natSaid = 1;
      if (s_seenPort[0] != s_seenPort[1] || s_seenHost[0] != s_seenHost[1]) {
        fprintf(stderr,
                "link: room: this router gives every destination another outside port (%u and %u): the others "
                "cannot aim at it, and a direct connection will often fail\n",
                (unsigned)ntohs(s_seenPort[0]), (unsigned)ntohs(s_seenPort[1]));
      } else {
        fprintf(stderr, "link: room: the router keeps one outside port for all destinations; direct connections can work\n");
      }
    }
  }
  return 1;
}

int mkgp2pc_meet_allows(uint32_t host) {
  const uint8_t* a = (const uint8_t*)&host;
  if (s_state != 1) {
    return 1;
  }
  /* this PC and the LAN: found by announcements as without a room */
  if (a[0] == 127 || a[0] == 10 || (a[0] == 192 && a[1] == 168) || (a[0] == 172 && (a[1] & 0xF0) == 16) ||
      (a[0] == 169 && a[1] == 254)) {
    return 1;
  }
  for (int k = 0; k < MAX_CABINETS; k++) {
    if (s_placed[k].placed && s_placed[k].host == host) {
      return 1;
    }
  }
  return 0;
}

void mkgp2pc_meet_tick(uintptr_t socket_, int index, uint64_t now) {
  struct sockaddr_in stun[STUN_SERVERS];
  int known, fresh;
  uint32_t mapped;
  struct {
    int present;
    uint32_t host;
    uint16_t port;
  } listed[MAX_CABINETS];
  if (!configured()) {
    return;
  }
  start_thread();
  if (socket_ != s_socket || index < 0) { /* another port now, or none: what was learned is void */
    s_socket = index < 0 ? (uintptr_t)-1 : socket_;
    memset(s_asked, 0, sizeof(s_asked));
    memset(s_heardStun, 0, sizeof(s_heardStun));
    s_natSaid = 0;
    lock(&s_lock);
    s_mapped = 0;
    s_mappedPort = 0;
    s_id = index < 0 ? 0 : index + 1;
    unlock(&s_lock);
  }
  lock(&s_lock);
  known = s_stunKnown;
  memcpy(stun, s_stun, sizeof(stun));
  fresh = s_listedNew;
  s_listedNew = 0;
  memcpy(listed, s_listed, sizeof(listed));
  mapped = s_mapped;
  unlock(&s_lock);
  if (index >= 0 && known) {
    /* Until answered once a second, then every 20 s: the router may give
     * the port another outside number after a pause. The second server is
     * only asked to compare. */
    for (int i = 0; i < s_stunCount; i++) {
      uint64_t every = s_heardStun[i] != 0 && s_heardStun[i] >= s_asked[i] ? (i == 0 ? 20000u : 300000u) : 1000u;
      if (i > 0 && s_heardStun[0] == 0) {
        break;
      }
      if (s_asked[i] == 0 || now - s_asked[i] >= every) {
        stun_ask(socket_, i, &stun[i], now);
      }
    }
  }
  if (fresh) {
    /* A member the server no longer lists is left where it is: a race in
     * progress must not depend on the server, and the game notices a
     * cabinet that has gone by itself. */
    for (int k = 0; k < MAX_CABINETS; k++) {
      /* A player behind this very router has the same public address;
       * routers seldom turn such datagrams around. The LAN announcements
       * find that one. */
      if (k != index && listed[k].present && listed[k].host != mapped &&
          (!s_placed[k].placed || s_placed[k].host != listed[k].host || s_placed[k].port != listed[k].port)) {
        const uint8_t* a = (const uint8_t*)&listed[k].host;
        fprintf(stderr, "link: room: cabinet %d is at %u.%u.%u.%u:%u\n", k + 1, a[0], a[1], a[2], a[3],
                (unsigned)ntohs(listed[k].port));
        s_placed[k].placed = 1;
        s_placed[k].host = listed[k].host;
        s_placed[k].port = listed[k].port;
        mkgp2pc_netlink_room_peer(k, listed[k].host, listed[k].port);
      }
    }
  }
}
