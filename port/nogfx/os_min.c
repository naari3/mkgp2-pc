/* Minimal Dolphin OS for the no-graphics bring-up executable. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

/*
 * Main memory at the console's own addresses. A 32-bit process on a 64-bit
 * kernel owns the whole 4 GB, so 0x80000000..0x81800000 can be mapped where
 * the game expects it; address comparisons and the low-memory globals in the
 * decompiled SDK code then keep working unchanged.
 */
#define MEM1_BASE 0x80000000u
#define MEM1_SIZE 0x01800000u
/* The arena starts where it does on the console: the end of the image's
 * last section (.sbss2 ends at 0x806DCF90) plus the 64 KB stack, rounded up
 * to 32 bytes. The race scenes need the heap to be as large as it was; with
 * the arena 1.1 MB shorter the first real race ran out of memory. */
#define ARENA_LO 0x806ECFA0u
#define ARENA_HI 0x81700000u

static void* s_arenaLo = (void*)ARENA_LO;
static void* s_arenaHi = (void*)ARENA_HI;
static void* s_saveStart;
static void* s_saveEnd;

#ifdef _WIN32
/* The executable is linked large-address-aware, so a 32-bit process on
 * 64-bit Windows owns the upper 2 GB as well. */
static void* map_fixed(uintptr_t at, size_t size) {
  return VirtualAlloc((void*)at, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}
static void unprotect_image(void) {
  uint8_t* base = (uint8_t*)GetModuleHandleA(NULL);
  IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
  IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
    DWORD old;
    if (sec->Misc.VirtualSize != 0) {
      VirtualProtect(base + sec->VirtualAddress, sec->Misc.VirtualSize, PAGE_EXECUTE_READWRITE, &old);
    }
  }
}
#else
static void* map_fixed(uintptr_t at, size_t size) {
  void* p = mmap((void*)at, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  return p == MAP_FAILED ? NULL : p;
}
static void unprotect_image(void) {
  extern char __executable_start[], _end[];
  uintptr_t lo = (uintptr_t)__executable_start & ~0xFFFu;
  uintptr_t hi = ((uintptr_t)_end + 0xFFFu) & ~0xFFFu;
  mprotect((void*)lo, hi - lo, PROT_READ | PROT_WRITE | PROT_EXEC);
}
#endif

/* (priority 101: before every other start-up function, the game's static
 * initialisers included) */
__attribute__((constructor(101))) static void map_mem1(void) {
  if (map_fixed(MEM1_BASE, MEM1_SIZE) != (void*)MEM1_BASE) {
    fprintf(stderr, "nogfx: cannot map MEM1 at 0x%08X\n", MEM1_BASE);
    exit(2);
  }
  /* Hardware registers, write-gather pipe (0xCC008000) included: plain
   * memory here, so direct accesses do nothing instead of faulting. */
  map_fixed(0xCC000000u, 0x10000);
  /* The console has no read-only memory: the game writes to objects that the
   * decompiled sources define as const (.sdata2 pointers and the like). */
  unprotect_image();
  /* Low-memory globals the game reads directly (host byte order). */
  *(uint32_t*)(MEM1_BASE + 0x28) = MEM1_SIZE;   /* physical memory size */
  *(uint32_t*)(MEM1_BASE + 0xF0) = MEM1_SIZE;   /* simulated memory size */
  *(uint32_t*)(MEM1_BASE + 0xF8) = 162000000u;  /* bus clock */
  *(uint32_t*)(MEM1_BASE + 0xFC) = 486000000u;  /* CPU clock */
}
uint32_t OSGetPhysicalMemSize(void) { return MEM1_SIZE; }
uint32_t OSGetConsoleSimulatedMemSize(void) { return MEM1_SIZE; }

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

#ifndef MKGP2PC_GFX /* the rendering executable has Aurora's */
/* GXInit hands back the FIFO object; nothing reads its fields here. */
void* GXInit(void* base, uint32_t size) {
  static char fifo[0x80];
  (void)base, (void)size;
  return fifo;
}
#endif

void HSD_Panic(const char* file, int line, const char* msg) {
  fprintf(stderr, "HSD_Panic: %s:%d: %s\n", file ? file : "?", line, msg ? msg : "");
  abort();
}

/* sysdolphin's assert handler (baselib/debug.c, which holds PowerPC assembly
 * and is not built here). Without this definition the game's calls bind to
 * the C library's __assert, which takes (expression, file, line). */
void __assert(const char* file, uint32_t line, const char* expr) {
  fprintf(stderr, "assertion \"%s\" failed\n", expr ? expr : "");
  HSD_Panic(file, (int)line, "");
}
