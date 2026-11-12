/*
 * Triforce AM baseboard, PC side.
 *
 * Replaces src/triforce/amexi.c function for function. The original talks to
 * the baseboard over EXI (channel 0, device 2): an addressed byte store that
 * the am* libraries use as backup RAM, plus a few 16-bit registers used by
 * the media board network code. Here the store is a plain array and the
 * registers read as zero, so ambackup / amcoin / amsys above run unchanged.
 *
 * The protocol facts come from the decompiled amexi.c itself (command bytes
 * 01 set address, 02 write byte, 03 read, 05 block write, 82/83/86/87/FF
 * registers).
 *
 * The store is kept in a file (docs/port/backup_memory.md), by default
 * backup_ram.bin in the current directory, MKGP2PC_BACKUP_FILE names another
 * and MKGP2PC_BACKUP=0 gives an empty store that is never saved, as before.
 *   - Layout: 16 bytes (magic "MKGP2BKP", version, CRC-32 of the image),
 *     then the 0x10000 bytes of the store.
 *   - Read once, before the game's first access. A file that fails the check
 *     is set aside as <name>.broken and <name>.bak (the file as it was at the
 *     last good start) is used instead.
 *   - Written by a thread of its own shortly after the game changed the
 *     store, so that the game never waits for the disk: a new file
 *     <name>.new is written, flushed to the disk and renamed over the old
 *     one. The process may be ended at any point of that (it is ended with
 *     TerminateProcess on Windows); the file under the real name is always a
 *     whole one. The ways out that the port controls (window closed, reset)
 *     call mkgp2pc_baseboard_flush first.
 *   - One game per file: the file is guarded by <name>.lock, and a second
 *     copy of the game running from the same folder (two cabinets on one PC)
 *     uses <name>.2, then .3 and .4, so each cabinet keeps records of its own.
 *
 * Symbol names are the address-based ones upstream uses today; when upstream
 * names them, rename here too (link_census.py reports them as missing).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
typedef CRITICAL_SECTION Lock;
#define lock_init(l) InitializeCriticalSection(l)
#define lock(l) EnterCriticalSection(l)
#define unlock(l) LeaveCriticalSection(l)
#else
#include <fcntl.h>
#include <pthread.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>
typedef pthread_mutex_t Lock;
#define lock_init(l) pthread_mutex_init(l, NULL)
#define lock(l) pthread_mutex_lock(l)
#define unlock(l) pthread_mutex_unlock(l)
#endif

#include "mkgp2pc/triforce.h"

#include <mkgp2pc/platform.h>

#define STORE_SIZE 0x10000u /* the address field of the protocol is 16 bits */
#define HEADER_SIZE 16u
#define FILE_SIZE (HEADER_SIZE + STORE_SIZE)
#define FILE_VERSION 1u
#define SETTLE_MS 60u /* a record written byte by byte is saved whole */
#define POLL_MS 30u

static uint8_t s_store[STORE_SIZE];
static void (*s_exiCallback[3])(int32_t chan, void* context);

/* ---- the file ------------------------------------------------------------------- */

static Lock s_storeLock;         /* s_store, s_changed, s_changedAt */
static Lock s_fileLock;          /* one writer at a time (thread, flush) */
static int s_state;              /* 0 not set up yet, 1 kept in a file, 2 memory only */
static char s_path[1024];        /* the file of this copy of the game */
static uint32_t s_changed;       /* counts the game's writes */
static uint32_t s_written;       /* value of s_changed the file holds (s_fileLock) */
static uint64_t s_changedAt;     /* time of the last write, ms */
static uint8_t s_out[FILE_SIZE]; /* the file being written (s_fileLock) */
static uint8_t s_start[STORE_SIZE]; /* the store as this run found it */
static int s_writeFailed;

static uint64_t now_ms(void) {
#ifdef _WIN32
  return GetTickCount64();
#else
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000u + (uint64_t)(t.tv_nsec / 1000000);
#endif
}

static uint32_t crc32_of(const uint8_t* p, uint32_t n) {
  uint32_t c = 0xFFFFFFFFu;
  while (n-- != 0) {
    c ^= *p++;
    for (int k = 0; k < 8; k++) {
      c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
  }
  return ~c;
}

static void put32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}
static uint32_t get32(const uint8_t* p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* 1: `image` holds the store saved in `path`. 0: no such file. -1: a file
 * that is not a whole save. */
static int file_read(const char* path, uint8_t* image) {
  static uint8_t data[FILE_SIZE + 1];
  FILE* f = fopen(path, "rb");
  if (f == NULL) {
    return 0;
  }
  size_t n = fread(data, 1, sizeof(data), f);
  fclose(f);
  if (n != FILE_SIZE || memcmp(data, "MKGP2BKP", 8) != 0 || get32(data + 8) != FILE_VERSION ||
      get32(data + 12) != crc32_of(data + HEADER_SIZE, STORE_SIZE)) {
    return -1;
  }
  memcpy(image, data + HEADER_SIZE, STORE_SIZE);
  return 1;
}

static int replace_file(const char* from, const char* to) {
#ifdef _WIN32
  return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  return rename(from, to) == 0;
#endif
}

/* Writes `image` under `path` so that `path` is the old file or the new one,
 * never part of one. */
static int file_write(const char* path, const uint8_t* image) {
  char fresh[1100];
  snprintf(fresh, sizeof(fresh), "%s.new", path);
  memcpy(s_out, "MKGP2BKP", 8);
  put32(s_out + 8, FILE_VERSION);
  memcpy(s_out + HEADER_SIZE, image, STORE_SIZE);
  put32(s_out + 12, crc32_of(s_out + HEADER_SIZE, STORE_SIZE));
  FILE* f = fopen(fresh, "wb");
  if (f == NULL) {
    return 0;
  }
  int ok = fwrite(s_out, 1, FILE_SIZE, f) == FILE_SIZE && fflush(f) == 0;
#ifdef _WIN32
  ok = ok && _commit(_fileno(f)) == 0;
#else
  ok = ok && fsync(fileno(f)) == 0;
#endif
  ok = fclose(f) == 0 && ok;
  if (!ok || !replace_file(fresh, path)) {
    remove(fresh);
    return 0;
  }
  return 1;
}

/* Takes <path>.lock for as long as the process lives. The operating system
 * gives it back when the process ends, however it ends. */
static int take_lock(const char* path) {
  char name[1100];
  snprintf(name, sizeof(name), "%s.lock", path);
#ifdef _WIN32
  return CreateFileA(name, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, NULL) !=
         INVALID_HANDLE_VALUE;
#else
  int fd = open(name, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) {
    return 0;
  }
  if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
    close(fd);
    return 0;
  }
  return 1;
#endif
}

/* Writes the file when the game changed the store since the last write.
 * `settled`: only when the last change is SETTLE_MS old. */
static void save_if_changed(int settled) {
  static uint8_t image[STORE_SIZE];
  if (s_state != 1) {
    return;
  }
  lock(&s_fileLock);
  lock(&s_storeLock);
  uint32_t changed = s_changed;
  int due = changed != s_written && (!settled || now_ms() - s_changedAt >= SETTLE_MS);
  if (due) {
    memcpy(image, s_store, STORE_SIZE);
  }
  unlock(&s_storeLock);
  if (due) {
    if (file_write(s_path, image)) {
      s_writeFailed = 0;
    } else if (!s_writeFailed) {
      s_writeFailed = 1; /* said once; tried again at the next change */
      fprintf(stderr, "backup: cannot write %s; the records of this run are not kept\n", s_path);
    }
    s_written = changed;
  }
  unlock(&s_fileLock);
}

#ifdef _WIN32
static DWORD WINAPI save_thread(LPVOID unused) {
  (void)unused;
  for (;;) {
    Sleep(POLL_MS);
    save_if_changed(1);
  }
  return 0;
}
#else
static void* save_thread(void* unused) {
  (void)unused;
  for (;;) {
    usleep(POLL_MS * 1000u);
    save_if_changed(1);
  }
  return NULL;
}
#endif

/* Before the first access: finds the file of this copy of the game and
 * reads it. The game starts on one thread, so does this. */
static void store_setup(void) {
  const char* e = getenv("MKGP2PC_BACKUP");
  const char* base = getenv("MKGP2PC_BACKUP_FILE");
  lock_init(&s_storeLock);
  lock_init(&s_fileLock);
  s_state = 2;
  if (e != NULL && strcmp(e, "0") == 0) {
    fprintf(stderr, "backup: MKGP2PC_BACKUP=0, the backup memory starts empty and is not saved\n");
    return;
  }
  if (base == NULL || *base == 0) {
    base = "backup_ram.bin";
  }
  int copy;
  for (copy = 1; copy <= 4; copy++) {
    if (copy == 1) {
      snprintf(s_path, sizeof(s_path), "%s", base);
    } else {
      snprintf(s_path, sizeof(s_path), "%s.%d", base, copy);
    }
    if (take_lock(s_path)) {
      break;
    }
  }
  if (copy > 4) {
    fprintf(stderr, "backup: %s and its copies .2 to .4 are in use or cannot be made; the backup memory of this run is not saved\n",
            base);
    return;
  }
  char other[1100];
  int r = file_read(s_path, s_store);
  if (r > 0) {
    /* a good file: keep it as the one to fall back on */
    snprintf(other, sizeof(other), "%s.bak", s_path);
    file_write(other, s_store);
    fprintf(stderr, "backup: backup memory read from %s\n", s_path);
  } else {
    if (r < 0) {
      snprintf(other, sizeof(other), "%s.broken", s_path);
      remove(other);
      rename(s_path, other);
    }
    snprintf(other, sizeof(other), "%s.bak", s_path);
    int b = file_read(other, s_store);
    if (b <= 0) {
      memset(s_store, 0, STORE_SIZE);
    }
    if (r < 0) {
      fprintf(stderr, "backup: %s is damaged and was set aside as .broken; %s\n", s_path,
              b > 0 ? "the copy of the last good start (.bak) is used" : "the backup memory starts empty");
    } else if (b > 0) {
      fprintf(stderr, "backup: %s is missing; its copy of the last good start (.bak) is used\n", s_path);
    } else {
      fprintf(stderr, "backup: no %s yet, the backup memory starts empty (the game sets it up)\n", s_path);
    }
    s_changed = b > 0; /* the real name gets a file again */
  }
  memcpy(s_start, s_store, STORE_SIZE);
  s_state = 1;
  atexit(mkgp2pc_baseboard_flush);
#ifdef _WIN32
  CloseHandle(CreateThread(NULL, 0, save_thread, NULL, 0, NULL));
#else
  pthread_t t;
  if (pthread_create(&t, NULL, save_thread, NULL) == 0) {
    pthread_detach(t);
  }
#endif
}

static void store_begin(void) {
  if (s_state == 0) {
    store_setup();
  }
  lock(&s_storeLock);
}
static void store_end(int wrote) {
  if (wrote) {
    s_changed++;
    s_changedAt = now_ms();
  }
  unlock(&s_storeLock);
}

void mkgp2pc_baseboard_flush(void) {
  if (s_state == 1) {
    save_if_changed(0);
  }
}

int mkgp2pc_baseboard_save_start(const char* path) {
  if (s_state == 0) {
    store_setup();
  }
  lock(&s_fileLock);
  int ok = file_write(path, s_start);
  unlock(&s_fileLock);
  return ok;
}

const char* mkgp2pc_baseboard_file(void) { return s_state == 1 ? s_path : ""; }

/* ---- amexi.c -------------------------------------------------------------------- */

/* exi_disconnect */
int fn_80286B80(int32_t chan) {
  (void)chan;
  return 1;
}

/* exi_connect2: the original checks EXI ID 0x06041000 ("GC-AM BASE PCB"). */
int fn_80286BD4(int32_t chan, uint32_t dev) {
  (void)chan, (void)dev;
  return 1;
}

/* write one byte at addr */
int fn_80286C54(int32_t chan, uint32_t dev, uint16_t addr, uint8_t data) {
  (void)chan, (void)dev;
  store_begin();
  if (s_store[addr] != data) {
    s_store[addr] = data;
    store_end(1);
  } else {
    store_end(0);
  }
  return 1;
}

/* read one byte at addr */
int fn_80286E78(int32_t chan, uint32_t dev, uint16_t addr, uint8_t* data) {
  (void)chan, (void)dev;
  store_begin();
  *data = s_store[addr];
  store_end(0);
  return 1;
}

static uint32_t clamp_len(uint16_t addr, uint32_t len) {
  return addr + len > STORE_SIZE ? STORE_SIZE - addr : len;
}

/* block write (EXI DMA in the original); `sync` 0 would complete from an
 * interrupt there, here it is always complete on return */
int fn_802870A4(int32_t chan, uint32_t dev, uint16_t addr, void* buf, uint32_t len, int sync) {
  (void)chan, (void)dev, (void)sync;
  len = clamp_len(addr, len);
  store_begin();
  if (memcmp(&s_store[addr], buf, len) != 0) {
    memcpy(&s_store[addr], buf, len);
    store_end(1);
  } else {
    store_end(0);
  }
  return 1;
}

/* block read */
int fn_802873A0(int32_t chan, uint32_t dev, uint16_t addr, void* buf, uint32_t len, int sync) {
  (void)chan, (void)dev, (void)sync;
  memset(buf, 0, len);
  store_begin();
  memcpy(buf, &s_store[addr], clamp_len(addr, len));
  store_end(0);
  return 1;
}

/* wait until the channel is idle */
void fn_802876F4(int32_t chan) { (void)chan; }

/* 16-bit registers (commands 82 / 86 read, 83 / 87 / FF write): media board
 * and interrupt control. No media board here: reads give 0, writes are
 * dropped. */
int fn_8028772C(int32_t chan, uint32_t dev, uint16_t* data) {
  /* interrupt status: which media board has an answer (amnet_nm3.c) */
  *data = (chan == 0 && dev == 2) ? (uint16_t)mkgp2pc_mediaboard_irq() : 0;
  return 1;
}
int fn_802878B4(int32_t chan, uint32_t dev, uint16_t data) {
  (void)chan, (void)dev, (void)data;
  return 1;
}
int fn_802879FC(int32_t chan, uint32_t dev, uint16_t data) {
  (void)chan, (void)dev, (void)data;
  return 1;
}
int fn_80287B44(int32_t chan, uint32_t dev, uint16_t* data) {
  (void)chan, (void)dev;
  *data = 0;
  return 1;
}
int fn_80287CCC(int32_t chan, uint32_t dev, uint16_t data) {
  (void)chan, (void)dev, (void)data;
  return 1;
}

void mkgp2pc_baseboard_raise_exi(int channel) {
  if (channel >= 0 && channel < 3 && s_exiCallback[channel]) {
    s_exiCallback[channel](channel, 0);
  }
}

/* EXISetExiCallback */
void fn_80287E14(int32_t chan, void (*callback)(int32_t, void*)) {
  if (chan >= 0 && chan < 3) {
    s_exiCallback[chan] = callback;
  }
}
