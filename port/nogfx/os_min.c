/* Minimal Dolphin OS for the no-graphics bring-up executable. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

/*
 * Main memory at the console's own addresses. A 32-bit process on a 64-bit
 * kernel owns the whole 4 GB, so 0x80000000..0x81800000 can be mapped where
 * the game expects it; address comparisons and the low-memory globals in the
 * decompiled SDK code then keep working unchanged.
 */
#define MEM1_BASE 0x80000000u
#define MEM1_SIZE 0x01800000u
#define ARENA_LO 0x80800000u /* the DOL image would end near 0x806DD000 */
#define ARENA_HI 0x81700000u

static void* s_arenaLo = (void*)ARENA_LO;
static void* s_arenaHi = (void*)ARENA_HI;
static void* s_saveStart;
static void* s_saveEnd;

__attribute__((constructor)) static void map_mem1(void) {
  void* p = mmap((void*)MEM1_BASE, MEM1_SIZE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  if (p != (void*)MEM1_BASE) {
    fprintf(stderr, "nogfx: cannot map MEM1 at 0x%08X\n", MEM1_BASE);
    exit(2);
  }
}

void* OSGetArenaLo(void) { return s_arenaLo; }
void* OSGetArenaHi(void) { return s_arenaHi; }
void OSSetArenaLo(void* p) { s_arenaLo = p; }
void OSSetArenaHi(void* p) { s_arenaHi = p; }

void OSGetSavedRegion(void** start, void** end) {
  *start = s_saveStart;
  *end = s_saveEnd;
}
void OSSetSaveRegion(void* start, void* end) {
  s_saveStart = start;
  s_saveEnd = end;
}

int OSDisableInterrupts(void) { return 0; }
int OSEnableInterrupts(void) { return 0; }
int OSRestoreInterrupts(int level) {
  (void)level;
  return 0;
}

uint32_t PPCMfhid0(void) { return 0; }
void PPCMthid0(uint32_t v) { (void)v; }

void DCFlushRange(void* p, uint32_t n) { (void)p, (void)n; }
void DCStoreRange(void* p, uint32_t n) { (void)p, (void)n; }
void DCInvalidateRange(void* p, uint32_t n) { (void)p, (void)n; }

/* Timer ticks: bus clock 162 MHz / 4. */
#define TICKS_PER_SEC 40500000ll
int64_t OSGetTime(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * TICKS_PER_SEC + (int64_t)ts.tv_nsec * TICKS_PER_SEC / 1000000000ll;
}
uint32_t OSGetTick(void) { return (uint32_t)OSGetTime(); }
int64_t __OSGetSystemTime(void) { return OSGetTime(); }

void* OSAllocFromArenaLo(uint32_t size, uint32_t align) {
  uintptr_t p = ((uintptr_t)s_arenaLo + align - 1) & ~(uintptr_t)(align - 1);
  s_arenaLo = (void*)((p + size + align - 1) & ~(uintptr_t)(align - 1));
  return (void*)p;
}
void* OSAllocFromArenaHi(uint32_t size, uint32_t align) {
  uintptr_t p = ((uintptr_t)s_arenaHi & ~(uintptr_t)(align - 1)) - size;
  p &= ~(uintptr_t)(align - 1);
  s_arenaHi = (void*)p;
  return (void*)p;
}

/* The disc ID lives at the very start of main memory on the console. */
void* DVDGetCurrentDiskID(void) { return (void*)MEM1_BASE; }

/* GXInit hands back the FIFO object; nothing reads its fields here. */
void* GXInit(void* base, uint32_t size) {
  static char fifo[0x80];
  (void)base, (void)size;
  return fifo;
}

void HSD_Panic(const char* file, int line, const char* msg) {
  fprintf(stderr, "HSD_Panic: %s:%d: %s\n", file ? file : "?", line, msg ? msg : "");
  abort();
}
