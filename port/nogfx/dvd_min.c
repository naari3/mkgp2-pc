/*
 * DVD for the bring-up executable: files come straight out of a GameCube
 * disc image (environment variable MKGP2_ISO), media board addresses go to
 * the board emulation. Every call completes before it returns. The shipped
 * build uses Aurora's DVD instead; this exists so that the bring-up needs
 * nothing but libc.
 */
#define _FILE_OFFSET_BITS 64
#include <ctype.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mkgp2pc/triforce.h"

typedef struct DVDCommandBlock {
  struct DVDCommandBlock* next;
  struct DVDCommandBlock* prev;
  uint32_t command;
  int32_t state;
  uint32_t offset, length;
  void* addr;
  uint32_t currTransferSize, transferredSize;
  void* id;
  void (*callback)(int32_t, struct DVDCommandBlock*);
  void* userData;
} DVDCommandBlock;
typedef void (*DVDCBCallback)(int32_t result, DVDCommandBlock* block);

typedef struct DVDFileInfo {
  DVDCommandBlock cb;
  uint32_t startAddr, length;
  void (*callback)(int32_t, struct DVDFileInfo*);
} DVDFileInfo;
typedef struct {
  uint32_t entryNum, location, next;
} DVDDir;
typedef struct {
  uint32_t entryNum;
  int isDir;
  char* name;
} DVDDirEntry;

static int s_fd = -1;
static uint8_t* s_fst;
static uint32_t s_entries;

static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static const uint8_t* entry(uint32_t i) { return s_fst + i * 12; }
static int is_dir(uint32_t i) { return entry(i)[0] != 0; }
static const char* name_of(uint32_t i) { return (const char*)s_fst + s_entries * 12 + (be32(entry(i)) & 0xFFFFFF); }

static int disc(void) {
  static int tried;
  uint8_t head[0x440];
  if (tried) {
    return s_fd >= 0;
  }
  tried = 1;
  const char* path = getenv("MKGP2_ISO");
  if (!path) {
    fprintf(stderr, "nogfx: MKGP2_ISO is not set; there is no disc\n");
    return 0;
  }
  s_fd = open(path, O_RDONLY);
  if (s_fd < 0 || pread(s_fd, head, sizeof(head), 0) != (ssize_t)sizeof(head)) {
    fprintf(stderr, "nogfx: cannot read %s\n", path);
    s_fd = -1;
    return 0;
  }
  uint32_t off = be32(head + 0x424), size = be32(head + 0x428);
  s_fst = (uint8_t*)malloc(size);
  if (pread(s_fd, s_fst, size, off) != (ssize_t)size) {
    s_fd = -1;
    return 0;
  }
  s_entries = be32(s_fst + 8);
  memcpy((void*)0x80000000u, head, 0x20); /* disk id, where the game expects it */
  return 1;
}

void DVDInit(void) { disc(); }
void* DVDGetFSTLocation(void) { return disc() ? s_fst : NULL; }

int32_t DVDConvertPathToEntrynum(const char* path) {
  uint32_t dir = 0;
  if (!disc()) {
    return -1;
  }
  while (*path == '/') {
    path++;
  }
  while (*path) {
    size_t n = strcspn(path, "/");
    uint32_t end = be32(entry(dir) + 8), i = dir + 1, found = 0;
    while (i < end) {
      const char* name = name_of(i);
      if (strlen(name) == n && strncasecmp(name, path, n) == 0) {
        found = i;
        break;
      }
      i = is_dir(i) ? be32(entry(i) + 8) : i + 1;
    }
    if (!found) {
      return -1;
    }
    dir = found;
    path += n;
    while (*path == '/') {
      path++;
    }
    if (*path && !is_dir(dir)) {
      return -1;
    }
  }
  return (int32_t)dir;
}

int DVDFastOpen(int32_t entrynum, DVDFileInfo* fi) {
  if (!disc() || entrynum < 0 || (uint32_t)entrynum >= s_entries || is_dir((uint32_t)entrynum)) {
    return 0;
  }
  memset(fi, 0, sizeof(*fi));
  fi->startAddr = be32(entry((uint32_t)entrynum) + 4);
  fi->length = be32(entry((uint32_t)entrynum) + 8);
  return 1;
}

int DVDOpen(const char* name, DVDFileInfo* fi) {
  int32_t e = DVDConvertPathToEntrynum(name);
  if (e < 0) {
    fprintf(stderr, "nogfx: DVDOpen(%s): not found\n", name);
    return 0;
  }
  return DVDFastOpen(e, fi);
}

int DVDClose(DVDFileInfo* fi) {
  (void)fi;
  return 1;
}

static int32_t read_at(void* addr, int32_t length, uint32_t offset) {
  ssize_t n = disc() ? pread(s_fd, addr, (size_t)length, (off_t)offset) : -1;
  if (n < 0) {
    memset(addr, 0, (size_t)length);
    return -1;
  }
  if (n < length) {
    memset((char*)addr + n, 0, (size_t)(length - n));
  }
  return length;
}

int32_t DVDReadPrio(DVDFileInfo* fi, void* addr, int32_t length, int32_t offset, int32_t prio) {
  (void)prio;
  return read_at(addr, length, fi->startAddr + (uint32_t)offset);
}

int DVDReadAsyncPrio(DVDFileInfo* fi, void* addr, int32_t length, int32_t offset,
                     void (*callback)(int32_t, DVDFileInfo*), int32_t prio) {
  int32_t n = DVDReadPrio(fi, addr, length, offset, prio);
  fi->cb.transferredSize = (uint32_t)(n < 0 ? 0 : n);
  if (callback) {
    callback(n, fi);
  }
  return 1;
}

int DVDReadAbsAsyncPrio(DVDCommandBlock* block, void* addr, int32_t length, int32_t offset,
                        DVDCBCallback callback, int32_t prio) {
  (void)prio;
  if (mkgp2pc_mediaboard_owns((uint32_t)offset)) {
    mkgp2pc_mediaboard_read((uint32_t)offset, addr, (uint32_t)length);
  } else {
    read_at(addr, length, (uint32_t)offset);
  }
  if (callback) {
    callback(length, block);
  }
  return 1;
}

int32_t DVDGetCommandBlockStatus(const DVDCommandBlock* block) {
  (void)block;
  return 0; /* everything completes inside the call */
}

int DVDOpenDir(const char* name, DVDDir* dir) {
  int32_t e = DVDConvertPathToEntrynum(name);
  if (e < 0 || !is_dir((uint32_t)e)) {
    return 0;
  }
  dir->entryNum = (uint32_t)e;
  dir->location = (uint32_t)e + 1;
  dir->next = be32(entry((uint32_t)e) + 8);
  return 1;
}

int DVDReadDir(DVDDir* dir, DVDDirEntry* out) {
  uint32_t i = dir->location;
  if (i <= dir->entryNum || i >= dir->next) {
    return 0;
  }
  out->entryNum = i;
  out->isDir = is_dir(i);
  out->name = (char*)name_of(i);
  dir->location = is_dir(i) ? be32(entry(i) + 8) : i + 1;
  return 1;
}

int DVDCloseDir(DVDDir* dir) {
  (void)dir;
  return 1;
}
