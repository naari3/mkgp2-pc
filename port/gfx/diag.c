/*
 * What a dump needs besides the picture: the memory the game works in, where
 * every thread was, and names for the addresses. Used by the F9 dump
 * (port/gfx/video_aurora.c), and on its own when the program dies: an
 * exception or signal, abort(), a fatal message of the rendering library.
 * docs/port/dumps.md describes the files and port/tools/dump_inspect.py
 * reads them.
 *
 *   stacks.txt       every thread: registers, then the return addresses on
 *                    its stack with the function each lies in
 *   memory.txt       what is in the two memory files and which modules were
 *                    loaded where
 *   mem1.bin.gz      the console's main memory (0x80000000, 48 MB): the
 *                    game's heap and everything loaded from the disc
 *   image.bin.gz     the executable's data sections: the game's globals
 *   symbols.txt.gz   address and name of everything in the executable
 *   crash.dmp        Windows only: a minidump (threads, their stacks,
 *                    modules) for WinDbg or Visual Studio
 *
 * Nothing here needs a debugger or a file beside the executable: names come
 * from the symbol table the executable carries in its own file (ELF .symtab,
 * COFF symbol table of the MinGW link), read once when first needed.
 *
 * Reading a stack. The game's code is compiled without optimisation and
 * keeps frame pointers; the libraries around it do not. Both are used: the
 * frame-pointer chain is followed from the thread's registers, and the stack
 * is searched for every word that points just behind a call instruction in
 * code. Lines of the chain are marked '#', the others '?' (a '?' line may be
 * left over from an earlier call).
 *
 * Other threads. Windows: the thread is suspended while its registers and
 * the top 64 KB of its stack are copied, and nothing else is done in that
 * time (it may hold the heap's or the C library's lock). Linux: the thread is
 * sent a signal and copies the same itself.
 *
 * In a crash this runs in the dying thread and uses the C library (files,
 * some memory). If the crash was inside the C library that may hang or fail;
 * the lines for the log are therefore written first, the files after.
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <direct.h>
#include <signal.h>
#else
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#endif

/* zlib, without its header (the Windows glue is compiled without zlib's
 * include directory; the functions have had these shapes since 1.0).
 * MKGP2PC_DIAG_NO_ZLIB: plain files, for building this file alone. */
#ifndef MKGP2PC_DIAG_NO_ZLIB
typedef struct gzFile_s* gzFile;
gzFile gzopen(const char* path, const char* mode);
int gzwrite(gzFile file, const void* buf, unsigned len);
int gzclose(gzFile file);
#define GZ_SUFFIX ".gz"
#else
typedef FILE* gzFile;
#define gzopen(path, mode) fopen(path, "wb")
#define gzwrite(f, buf, len) ((int)fwrite(buf, 1, len, f))
#define gzclose(f) fclose(f)
#define GZ_SUFFIX ""
#endif

/* port/nogfx/os_min.c maps the console's memory here */
#define MEM1_BASE 0x80000000u
#define MEM1_SIZE 0x03000000u

#define STACK_WORDS 16384 /* 64 KB of each stack are looked at */
#define MAX_THREADS 128
#define MAX_LINES 96 /* per thread */

/* video_aurora.c sets this: writes input.txt and state.txt into a folder */
void (*mkgp2pc_diag_extra)(const char* dir);

/* ---- output: the dump's text file, the log, or both ---------------------- */
static FILE* s_file;
static bool s_toLog;
static void out(const char* fmt, ...) {
  va_list ap;
  if (s_file != NULL) {
    va_start(ap, fmt);
    vfprintf(s_file, fmt, ap);
    va_end(ap);
  }
  if (s_toLog) {
    va_start(ap, fmt);
    fputs("crash: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
  }
}

/* ---- the executable: sections and symbols -------------------------------- */
typedef struct {
  char name[20];
  uintptr_t addr;
  size_t size;
  bool code;
} Section;
typedef struct {
  uintptr_t addr;
  const char* name;
  char kind; /* T code, D data */
} Sym;
static Section s_sections[96];
static int s_sectionCount;
static Sym* s_syms;
static size_t s_symCount;
static uintptr_t s_imageLo, s_imageHi;
static char s_exeName[64] = "exe";
static bool s_exeRead;

static const Section* section_of(uintptr_t addr) {
  for (int i = 0; i < s_sectionCount; i++) {
    if (addr >= s_sections[i].addr && addr - s_sections[i].addr < s_sections[i].size) {
      return &s_sections[i];
    }
  }
  return NULL;
}
static void section_add(const char* name, size_t nameLen, uintptr_t addr, size_t size, bool code) {
  if (s_sectionCount == (int)(sizeof(s_sections) / sizeof(s_sections[0])) || size == 0) {
    return;
  }
  Section* s = &s_sections[s_sectionCount++];
  if (nameLen > sizeof(s->name) - 1) {
    nameLen = sizeof(s->name) - 1;
  }
  memcpy(s->name, name, nameLen);
  s->name[nameLen] = 0;
  s->addr = addr;
  s->size = size;
  s->code = code;
  if (s_imageLo == 0 || addr < s_imageLo) {
    s_imageLo = addr;
  }
  if (addr + size > s_imageHi) {
    s_imageHi = addr + size;
  }
}
static int sym_cmp(const void* a, const void* b) {
  uintptr_t x = ((const Sym*)a)->addr, y = ((const Sym*)b)->addr;
  return x < y ? -1 : x > y;
}
static void* file_part(FILE* f, long at, size_t size) {
  void* p = malloc(size ? size : 1);
  if (p != NULL && (fseek(f, at, SEEK_SET) != 0 || fread(p, 1, size, f) != size)) {
    free(p);
    p = NULL;
  }
  return p;
}

#ifdef _WIN32
static void exe_read(void) {
  char path[MAX_PATH];
  uint8_t* base = (uint8_t*)GetModuleHandleA(NULL);
  IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
  IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
  const unsigned count = nt->FileHeader.NumberOfSections;
  for (unsigned i = 0; i < count; i++) {
    /* discardable: the debug information, which is not in memory */
    if ((sec[i].Characteristics & IMAGE_SCN_MEM_DISCARDABLE) == 0) {
      section_add((const char*)sec[i].Name, strnlen((const char*)sec[i].Name, 8), (uintptr_t)base + sec[i].VirtualAddress,
                  sec[i].Misc.VirtualSize, (sec[i].Characteristics & IMAGE_SCN_CNT_CODE) != 0);
    }
  }
  if (GetModuleFileNameA(NULL, path, sizeof(path)) == 0) {
    return;
  }
  const char* slash = strrchr(path, '\\');
  snprintf(s_exeName, sizeof(s_exeName), "%s", slash ? slash + 1 : path);
  FILE* f = fopen(path, "rb");
  if (f == NULL) {
    return;
  }
  const uint32_t symAt = nt->FileHeader.PointerToSymbolTable, symCount = nt->FileHeader.NumberOfSymbols;
  uint8_t* tab = symAt != 0 && symCount != 0 ? (uint8_t*)file_part(f, (long)symAt, (size_t)symCount * 18) : NULL;
  uint32_t strSize = 0;
  char* str = NULL;
  if (tab != NULL && fread(&strSize, 1, 4, f) == 4 && strSize >= 4) {
    str = (char*)file_part(f, (long)(symAt + symCount * 18), strSize);
  }
  fclose(f);
  if (tab == NULL || str == NULL) {
    free(tab);
    free(str);
    return;
  }
  /* short names are copied out (they need not end in a zero) */
  char* shortNames = (char*)malloc((size_t)symCount * 9);
  s_syms = (Sym*)malloc((size_t)symCount * sizeof(Sym));
  if (shortNames == NULL || s_syms == NULL) {
    return;
  }
  for (uint32_t i = 0; i < symCount; i++) {
    const uint8_t* e = tab + (size_t)i * 18;
    uint32_t value, zero, strOff;
    int16_t secNo;
    memcpy(&zero, e, 4);
    memcpy(&strOff, e + 4, 4);
    memcpy(&value, e + 8, 4);
    memcpy(&secNo, e + 12, 2);
    const uint8_t storage = e[16], aux = e[17];
    if (secNo > 0 && (unsigned)secNo <= count && (storage == 2 || storage == 3)) {
      const char* name;
      if (zero == 0) {
        name = strOff < strSize ? str + strOff : "";
      } else {
        char* d = shortNames + (size_t)i * 9;
        memcpy(d, e, 8);
        d[8] = 0;
        name = d;
      }
      const IMAGE_SECTION_HEADER* sh = &sec[secNo - 1];
      /* '.': the sections' own symbols and the compiler's local labels */
      if (name[0] != 0 && name[0] != '.' && (sh->Characteristics & IMAGE_SCN_MEM_DISCARDABLE) == 0) {
        Sym* s = &s_syms[s_symCount++];
        s->addr = (uintptr_t)base + sh->VirtualAddress + value;
        s->name = name;
        s->kind = (sh->Characteristics & IMAGE_SCN_CNT_CODE) != 0 ? 'T' : 'D';
      }
    }
    i += aux;
  }
  free(tab);
}
#else
static int phdr_first(struct dl_phdr_info* info, size_t size, void* data) {
  (void)size;
  *(uintptr_t*)data = (uintptr_t)info->dlpi_addr;
  return 1; /* the first entry is the executable */
}
static void exe_read(void) {
  uintptr_t bias = 0;
  dl_iterate_phdr(&phdr_first, &bias);
  char path[512];
  ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (n > 0) {
    path[n] = 0;
    const char* slash = strrchr(path, '/');
    snprintf(s_exeName, sizeof(s_exeName), "%s", slash ? slash + 1 : path);
  }
  FILE* f = fopen("/proc/self/exe", "rb");
  if (f == NULL) {
    return;
  }
  ElfW(Ehdr) eh;
  if (fread(&eh, 1, sizeof(eh), f) != sizeof(eh) || memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_shnum == 0) {
    fclose(f);
    return;
  }
  ElfW(Shdr)* sh = (ElfW(Shdr)*)file_part(f, (long)eh.e_shoff, (size_t)eh.e_shnum * sizeof(ElfW(Shdr)));
  char* shstr = sh != NULL && eh.e_shstrndx < eh.e_shnum
                    ? (char*)file_part(f, (long)sh[eh.e_shstrndx].sh_offset, sh[eh.e_shstrndx].sh_size)
                    : NULL;
  if (shstr == NULL) {
    fclose(f);
    return;
  }
  for (unsigned i = 0; i < eh.e_shnum; i++) {
    /* thread-local sections are templates, not where the data lives */
    if ((sh[i].sh_flags & SHF_ALLOC) != 0 && (sh[i].sh_flags & SHF_TLS) == 0) {
      const char* name = shstr + sh[i].sh_name;
      section_add(name, strlen(name), bias + sh[i].sh_addr, sh[i].sh_size, (sh[i].sh_flags & SHF_EXECINSTR) != 0);
    }
  }
  for (unsigned i = 0; i < eh.e_shnum; i++) {
    if (sh[i].sh_type != SHT_SYMTAB || sh[i].sh_link >= eh.e_shnum) {
      continue;
    }
    ElfW(Sym)* tab = (ElfW(Sym)*)file_part(f, (long)sh[i].sh_offset, sh[i].sh_size);
    char* str = (char*)file_part(f, (long)sh[sh[i].sh_link].sh_offset, sh[sh[i].sh_link].sh_size);
    const size_t count = sh[i].sh_size / sizeof(ElfW(Sym));
    s_syms = (Sym*)malloc((count ? count : 1) * sizeof(Sym));
    if (tab == NULL || str == NULL || s_syms == NULL) {
      break;
    }
    for (size_t k = 0; k < count; k++) {
      const unsigned type = ELF32_ST_TYPE(tab[k].st_info);
      if (tab[k].st_shndx == SHN_UNDEF || tab[k].st_shndx >= eh.e_shnum || tab[k].st_name == 0 ||
          (type != STT_FUNC && type != STT_OBJECT && type != STT_NOTYPE)) {
        continue;
      }
      const ElfW(Shdr)* in = &sh[tab[k].st_shndx];
      if ((in->sh_flags & SHF_ALLOC) == 0 || (in->sh_flags & SHF_TLS) != 0) {
        continue;
      }
      Sym* s = &s_syms[s_symCount++];
      s->addr = bias + tab[k].st_value;
      s->name = str + tab[k].st_name;
      s->kind = (in->sh_flags & SHF_EXECINSTR) != 0 ? 'T' : 'D';
    }
    free(tab);
    break;
  }
  fclose(f);
  free(shstr);
  free(sh);
}
#endif

static void exe_need(void) {
  if (!s_exeRead) {
    s_exeRead = true;
    exe_read();
    if (s_symCount != 0) {
      qsort(s_syms, s_symCount, sizeof(Sym), &sym_cmp);
    }
  }
}
/* the last symbol at or below the address, in the same section */
static const Sym* sym_find(uintptr_t addr) {
  const Section* sec = section_of(addr);
  if (sec == NULL || s_symCount == 0 || addr < s_syms[0].addr) {
    return NULL;
  }
  size_t lo = 0, hi = s_symCount;
  while (hi - lo > 1) {
    size_t mid = lo + (hi - lo) / 2;
    if (s_syms[mid].addr <= addr) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return s_syms[lo].addr >= sec->addr ? &s_syms[lo] : NULL;
}

/* ---- the address space --------------------------------------------------- */
#ifdef _WIN32
/* bytes readable from addr on, within its region (0: not readable) */
static size_t mem_span(uintptr_t addr) {
  MEMORY_BASIC_INFORMATION mi;
  if (VirtualQuery((void*)addr, &mi, sizeof(mi)) == 0 || mi.State != MEM_COMMIT ||
      (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0 || (mi.Protect & 0xFF) == 0) {
    return 0;
  }
  return (uintptr_t)mi.BaseAddress + mi.RegionSize - addr;
}
/* "module+0xoffset" of an address in a loaded module other than the executable */
static bool module_of(uintptr_t addr, char* text, size_t size, bool* code) {
  MEMORY_BASIC_INFORMATION mi;
  char name[MAX_PATH];
  if (VirtualQuery((void*)addr, &mi, sizeof(mi)) == 0 || mi.State != MEM_COMMIT || mi.Type != MEM_IMAGE ||
      mi.AllocationBase == NULL || GetModuleFileNameA((HMODULE)mi.AllocationBase, name, sizeof(name)) == 0) {
    return false;
  }
  const char* slash = strrchr(name, '\\');
  snprintf(text, size, "%s+0x%lx", slash ? slash + 1 : name, (unsigned long)(addr - (uintptr_t)mi.AllocationBase));
  *code = (mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
  return true;
}
static void maps_read(void) {}
#else
typedef struct {
  uintptr_t lo, hi;
  bool read, exec;
  uint32_t name; /* offset into s_mapsText, 0: none */
} Map;
static char s_mapsText[1 << 20];
static Map s_maps[4096];
static int s_mapCount;
/* /proc/self/maps without the C library's files or memory: also called
 * from the crash handler */
static void maps_read(void) {
  int fd = open("/proc/self/maps", O_RDONLY);
  size_t len = 0;
  s_mapCount = 0;
  if (fd < 0) {
    return;
  }
  for (;;) {
    ssize_t n = read(fd, s_mapsText + len, sizeof(s_mapsText) - 1 - len);
    if (n <= 0) {
      break;
    }
    len += (size_t)n;
  }
  close(fd);
  s_mapsText[len] = 0;
  char* p = s_mapsText;
  while (*p != 0 && s_mapCount < (int)(sizeof(s_maps) / sizeof(s_maps[0]))) {
    char* end = strchr(p, '\n');
    if (end != NULL) {
      *end = 0;
    }
    Map* m = &s_maps[s_mapCount];
    char* q;
    m->lo = (uintptr_t)strtoull(p, &q, 16);
    m->hi = (uintptr_t)strtoull(q + 1, &q, 16);
    m->read = q[1] == 'r';
    m->exec = q[3] == 'x';
    char* slash = strchr(q, '/');
    char* bracket = strchr(q, '[');
    m->name = slash != NULL ? (uint32_t)(slash - s_mapsText) : bracket != NULL ? (uint32_t)(bracket - s_mapsText) : 0;
    s_mapCount++;
    if (end == NULL) {
      break;
    }
    p = end + 1;
  }
}
static const Map* map_of(uintptr_t addr) {
  int lo = 0, hi = s_mapCount;
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    if (addr >= s_maps[mid].hi) {
      lo = mid + 1;
    } else if (addr < s_maps[mid].lo) {
      hi = mid;
    } else {
      return &s_maps[mid];
    }
  }
  return NULL;
}
static size_t mem_span(uintptr_t addr) {
  const Map* m = map_of(addr);
  size_t span = 0;
  /* mappings that touch count as one (a stack is often two) */
  while (m != NULL && m->read) {
    span = m->hi - addr;
    const Map* next = m + 1 < s_maps + s_mapCount && m[1].lo == m->hi ? m + 1 : NULL;
    m = next;
  }
  return span;
}
static bool module_of(uintptr_t addr, char* text, size_t size, bool* code) {
  const Map* m = map_of(addr);
  if (m == NULL || m->name == 0 || s_mapsText[m->name] != '/') {
    return false;
  }
  const char* path = s_mapsText + m->name;
  uintptr_t base = m->lo;
  for (const Map* k = m; k >= s_maps && k->name != 0 && strcmp(s_mapsText + k->name, path) == 0; k--) {
    base = k->lo;
  }
  const char* slash = strrchr(path, '/');
  snprintf(text, size, "%s+0x%lx", slash + 1, (unsigned long)(addr - base));
  *code = m->exec;
  return true;
}
#endif

/* Name of an address; says whether it is code. The executable's own pages
 * are all writable and executable (os_min.c, unprotect_image), so for it
 * the section decides. */
static bool describe(uintptr_t addr, char* text, size_t size, bool* code) {
  *code = false;
  if (addr >= s_imageLo && addr < s_imageHi) {
    const Section* sec = section_of(addr);
    const Sym* s = sym_find(addr);
    if (sec == NULL) {
      return false;
    }
    *code = sec->code;
    if (s != NULL) {
      snprintf(text, size, "%s+0x%lx", s->name, (unsigned long)(addr - s->addr));
    } else {
      snprintf(text, size, "%s %s+0x%lx", s_exeName, sec->name, (unsigned long)(addr - sec->addr));
    }
    return true;
  }
  return module_of(addr, text, size, code);
}

/* Does a call instruction end at this address? (x86: E8 rel32, or FF /2
 * with any of its addressing forms) */
static bool after_call(uintptr_t addr) {
#if defined(__i386__) || defined(__x86_64__)
  /* the bytes in front are in the same mapping unless the address is its start */
  if (mem_span(addr - 7) < 8) {
    return false;
  }
  const uint8_t* p = (const uint8_t*)addr;
  if (p[-5] == 0xE8) {
    return true;
  }
  static const int lengths[] = {2, 3, 4, 6, 7};
  for (int i = 0; i < 5; i++) {
    int n = lengths[i];
    if (p[-n] == 0xFF && (p[-n + 1] & 0x38) == 0x10) {
      return true;
    }
  }
  return false;
#else
  (void)addr;
  return true;
#endif
}

/* ---- one thread ---------------------------------------------------------- */
typedef struct {
  unsigned long id;
  bool ok, current;
  bool haveRegs;
  uintptr_t ip, sp, bp;
  uintptr_t regs[6]; /* eax ebx ecx edx esi edi */
  size_t words;      /* copied from sp upwards */
  uintptr_t* stack;
  char name[48];
  const char* note; /* why it could not be read */
} Shot;

static void shot_copy_stack(Shot* t) {
  size_t span = mem_span(t->sp & ~(uintptr_t)(sizeof(uintptr_t) - 1));
  t->sp &= ~(uintptr_t)(sizeof(uintptr_t) - 1);
  t->words = span / sizeof(uintptr_t);
  if (t->words > STACK_WORDS) {
    t->words = STACK_WORDS;
  }
  if (t->stack == NULL) {
    t->words = 0;
  }
  memcpy(t->stack, (const void*)t->sp, t->words * sizeof(uintptr_t));
}

static void shot_print(const Shot* t, const char* what) {
  char text[400];
  bool code;
  out("thread %lu%s%s%s%s%s\n", t->id, t->name[0] ? " \"" : "", t->name, t->name[0] ? "\"" : "", what[0] ? ": " : "", what);
  if (!t->ok) {
    out("  (%s)\n", t->note != NULL ? t->note : "could not be read");
    return;
  }
  if (t->haveRegs) {
    out("  at %08lx %s\n", (unsigned long)t->ip, describe(t->ip, text, sizeof(text), &code) ? text : "");
    out("  eax %08lx ebx %08lx ecx %08lx edx %08lx esi %08lx edi %08lx ebp %08lx esp %08lx\n",
        (unsigned long)t->regs[0], (unsigned long)t->regs[1], (unsigned long)t->regs[2], (unsigned long)t->regs[3],
        (unsigned long)t->regs[4], (unsigned long)t->regs[5], (unsigned long)t->bp, (unsigned long)t->sp);
  }
  /* the frame-pointer chain: where the return addresses of real frames are */
  static uint8_t onChain[STACK_WORDS];
  memset(onChain, 0, sizeof(onChain));
  uintptr_t bp = t->bp;
  for (int guard = 0; guard < 4096; guard++) {
    if (bp < t->sp || (bp & (sizeof(uintptr_t) - 1)) != 0) {
      break;
    }
    size_t at = (bp - t->sp) / sizeof(uintptr_t);
    if (at + 1 >= t->words) {
      break;
    }
    onChain[at + 1] = 1;
    if (t->stack[at] <= bp) {
      break;
    }
    bp = t->stack[at];
  }
  int lines = 0;
  for (size_t i = 0; i < t->words && lines < MAX_LINES; i++) {
    uintptr_t v = t->stack[i];
    if (!describe(v, text, sizeof(text), &code) || !code || !after_call(v)) {
      continue;
    }
    out("  %c +%05lx %08lx %s\n", onChain[i] ? '#' : '?', (unsigned long)(i * sizeof(uintptr_t)), (unsigned long)v, text);
    lines++;
  }
  if (lines == MAX_LINES) {
    out("  (more lines left out)\n");
  }
}

static Shot s_shots[MAX_THREADS];
static int s_shotCount;

static Shot* shot_next(void) {
  Shot* t = &s_shots[s_shotCount++];
  uintptr_t* keep = t->stack;
  memset(t, 0, sizeof(*t));
  t->stack = keep != NULL ? keep : (uintptr_t*)malloc(STACK_WORDS * sizeof(uintptr_t));
  return t;
}

#ifdef _WIN32
static void shot_from_context(Shot* t, const CONTEXT* c) {
#if defined(__i386__) || defined(_M_IX86)
  t->ip = c->Eip;
  t->sp = c->Esp;
  t->bp = c->Ebp;
  t->regs[0] = c->Eax;
  t->regs[1] = c->Ebx;
  t->regs[2] = c->Ecx;
  t->regs[3] = c->Edx;
  t->regs[4] = c->Esi;
  t->regs[5] = c->Edi;
#else
  t->ip = c->Rip;
  t->sp = c->Rsp;
  t->bp = c->Rbp;
#endif
  t->haveRegs = true;
}
/* every other thread of the process */
static void shots_others(void) {
  const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  THREADENTRY32 te;
  te.dwSize = sizeof(te);
  if (snap == INVALID_HANDLE_VALUE) {
    return;
  }
  for (BOOL more = Thread32First(snap, &te); more && s_shotCount < MAX_THREADS; more = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) {
      continue;
    }
    Shot* t = shot_next();
    t->id = te.th32ThreadID;
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION | 0x0800 /* limited information: the name */, FALSE, te.th32ThreadID);
    if (h == NULL) {
      continue;
    }
    /* from here to ResumeThread: no memory allocation, no output */
    if (SuspendThread(h) != (DWORD)-1) {
      CONTEXT c;
      c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
      if (GetThreadContext(h, &c)) {
        shot_from_context(t, &c);
        shot_copy_stack(t);
        t->ok = true;
      }
      ResumeThread(h);
    }
    /* the name a thread was given (SDL and the rendering library do); Windows 10 */
    typedef HRESULT(WINAPI * GetDescription)(HANDLE, PWSTR*);
    static GetDescription getDescription;
    if (getDescription == NULL) {
      getDescription = (GetDescription)(void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
    }
    PWSTR wide = NULL;
    if (getDescription != NULL && getDescription(h, &wide) >= 0 && wide != NULL) {
      WideCharToMultiByte(CP_UTF8, 0, wide, -1, t->name, sizeof(t->name) - 1, NULL, NULL);
      LocalFree(wide);
    }
    CloseHandle(h);
  }
  CloseHandle(snap);
}
#else
static void shot_from_ucontext(Shot* t, const ucontext_t* uc) {
#if defined(__i386__)
  const greg_t* g = uc->uc_mcontext.gregs;
  t->ip = (uintptr_t)g[REG_EIP];
  t->sp = (uintptr_t)g[REG_ESP];
  t->bp = (uintptr_t)g[REG_EBP];
  t->regs[0] = (uintptr_t)g[REG_EAX];
  t->regs[1] = (uintptr_t)g[REG_EBX];
  t->regs[2] = (uintptr_t)g[REG_ECX];
  t->regs[3] = (uintptr_t)g[REG_EDX];
  t->regs[4] = (uintptr_t)g[REG_ESI];
  t->regs[5] = (uintptr_t)g[REG_EDI];
  t->haveRegs = true;
#elif defined(__x86_64__)
  const greg_t* g = uc->uc_mcontext.gregs;
  t->ip = (uintptr_t)g[REG_RIP];
  t->sp = (uintptr_t)g[REG_RSP];
  t->bp = (uintptr_t)g[REG_RBP];
  t->haveRegs = true;
#else
  (void)uc;
  t->sp = (uintptr_t)&uc;
#endif
}
#define SHOT_SIGNAL (SIGRTMIN + 6)
static Shot* volatile s_shotTarget;
static sem_t s_shotDone;
static void shot_signal(int sig, siginfo_t* info, void* uc) {
  (void)sig;
  (void)info;
  Shot* t = s_shotTarget;
  if (t != NULL && t->id == (unsigned long)syscall(SYS_gettid)) {
    shot_from_ucontext(t, (const ucontext_t*)uc);
    shot_copy_stack(t);
    t->ok = true;
    sem_post(&s_shotDone);
  }
}
/* name of a thread, and whether SHOT_SIGNAL is blocked in it */
static bool task_blocks_signal(pid_t tid, char* name, size_t size) {
  char path[64], text[2048];
  bool blocked = false;
  snprintf(path, sizeof(path), "/proc/self/task/%d/status", (int)tid);
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    return false;
  }
  ssize_t n = read(fd, text, sizeof(text) - 1);
  close(fd);
  text[n > 0 ? n : 0] = 0;
  if (strncmp(text, "Name:\t", 6) == 0) {
    const char* end = strchr(text, '\n');
    snprintf(name, size, "%.*s", end != NULL ? (int)(end - text - 6) : 0, text + 6);
  }
  const char* blk = strstr(text, "\nSigBlk:\t");
  if (blk != NULL) {
    blocked = ((strtoull(blk + 9, NULL, 16) >> (SHOT_SIGNAL - 1)) & 1) != 0;
  }
  return blocked;
}
static void shots_others(void) {
  static bool ready;
  if (!ready) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = &shot_signal;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SHOT_SIGNAL, &sa, NULL);
    sem_init(&s_shotDone, 0, 0);
    ready = true;
  }
  const pid_t self = (pid_t)syscall(SYS_gettid);
  DIR* dir = opendir("/proc/self/task");
  if (dir == NULL) {
    return;
  }
  for (struct dirent* e; (e = readdir(dir)) != NULL && s_shotCount < MAX_THREADS;) {
    pid_t tid = (pid_t)strtol(e->d_name, NULL, 10);
    if (tid <= 0 || tid == self) {
      continue;
    }
    Shot* t = shot_next();
    t->id = (unsigned long)tid;
    if (task_blocks_signal(tid, t->name, sizeof(t->name))) {
      /* the graphics driver's workers block every signal */
      t->note = "not read: the thread blocks signals";
      continue;
    }
    s_shotTarget = t;
    if (syscall(SYS_tgkill, getpid(), tid, SHOT_SIGNAL) == 0) {
      struct timespec until;
      clock_gettime(CLOCK_REALTIME, &until);
      until.tv_nsec += 300000000;
      if (until.tv_nsec >= 1000000000) {
        until.tv_nsec -= 1000000000;
        until.tv_sec++;
      }
      int r;
      while ((r = sem_timedwait(&s_shotDone, &until)) != 0 && errno == EINTR) {
      }
      if (r != 0) {
        /* it may still answer later: this record is given up, not reused */
        s_shotTarget = NULL;
        t->ok = false;
        t->stack = NULL;
      }
    }
  }
  s_shotTarget = NULL;
  closedir(dir);
}
#endif

/* The thread this runs in, from where it is now (no saved registers). */
__attribute__((noinline)) static void shot_here(Shot* t) {
  static uintptr_t own[STACK_WORDS];
  memset(t, 0, sizeof(*t));
  t->stack = own;
#ifdef _WIN32
  t->id = GetCurrentThreadId();
#else
  t->id = (unsigned long)syscall(SYS_gettid);
#endif
  t->current = true;
  t->sp = (uintptr_t)__builtin_frame_address(0);
  t->bp = (uintptr_t)__builtin_frame_address(0);
  shot_copy_stack(t);
  t->ok = true;
}

/* ---- the files ----------------------------------------------------------- */
static void make_dir(const char* path) {
#ifdef _WIN32
  _mkdir(path);
#else
  mkdir(path, 0777);
#endif
}

static void write_stacks(const char* dir, const char* why, const Shot* first) {
  char path[700];
  snprintf(path, sizeof(path), "%s/stacks.txt", dir);
  FILE* f = fopen(path, "w");
  if (f == NULL) {
    return;
  }
  s_file = f;
  s_toLog = false;
  out("%s\n", why);
  out("'#': return address of a frame on the frame-pointer chain; '?': a word on the stack that points behind a "
      "call (may be left over from an earlier call)\n\n");
  shot_print(first, first->haveRegs ? "the thread this is about" : "the thread that wrote this dump");
  s_shotCount = 0;
  shots_others();
  for (int i = 0; i < s_shotCount; i++) {
    out("\n");
    shot_print(&s_shots[i], "");
  }
  s_file = NULL;
  fclose(f);
}

/* memory in pieces; what cannot be read is written as zeros */
static void write_memory(gzFile z, uintptr_t addr, size_t size) {
  static const uint8_t zeros[4096];
  while (size != 0) {
    size_t n = mem_span(addr);
    if (n == 0) {
      n = 4096 - (addr & 4095);
      if (n > size) {
        n = size;
      }
      gzwrite(z, zeros, (unsigned)n);
    } else {
      if (n > size) {
        n = size;
      }
      if (n > (1u << 24)) {
        n = 1u << 24;
      }
      gzwrite(z, (const void*)addr, (unsigned)n);
    }
    addr += n;
    size -= n;
  }
}

static void write_modules(FILE* f) {
#ifdef _WIN32
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
  MODULEENTRY32 me;
  me.dwSize = sizeof(me);
  if (snap == INVALID_HANDLE_VALUE) {
    return;
  }
  for (BOOL more = Module32First(snap, &me); more; more = Module32Next(snap, &me)) {
    fprintf(f, "module %08lx %08lx %s\n", (unsigned long)(uintptr_t)me.modBaseAddr, (unsigned long)me.modBaseSize,
            me.szModule);
  }
  CloseHandle(snap);
#else
  for (int i = 0; i < s_mapCount; i++) {
    const Map* m = &s_maps[i];
    if (m->name == 0 || s_mapsText[m->name] != '/') {
      continue;
    }
    const char* path = s_mapsText + m->name;
    if (i > 0 && s_maps[i - 1].name != 0 && strcmp(s_mapsText + s_maps[i - 1].name, path) == 0) {
      continue;
    }
    uintptr_t hi = m->hi;
    for (int k = i + 1; k < s_mapCount && s_maps[k].name != 0 && strcmp(s_mapsText + s_maps[k].name, path) == 0; k++) {
      hi = s_maps[k].hi;
    }
    fprintf(f, "module %08lx %08lx %s\n", (unsigned long)m->lo, (unsigned long)(hi - m->lo), strrchr(path, '/') + 1);
  }
#endif
}

static void write_images(const char* dir) {
  char path[700];
  snprintf(path, sizeof(path), "%s/memory.txt", dir);
  FILE* f = fopen(path, "w");
  if (f == NULL) {
    return;
  }
  fprintf(f, "# region <name> <address> <size> <file> <offset in the unpacked file>\n");
  fprintf(f, "exe %s\n", s_exeName);
  fprintf(f, "pointer %u\n", (unsigned)sizeof(void*));
  if (mem_span(MEM1_BASE) != 0) {
    snprintf(path, sizeof(path), "%s/mem1.bin" GZ_SUFFIX, dir);
    gzFile z = gzopen(path, "wb1");
    if (z != NULL) {
      write_memory(z, MEM1_BASE, MEM1_SIZE);
      gzclose(z);
      fprintf(f, "region mem1 %08lx %08lx mem1.bin" GZ_SUFFIX " 0\n", (unsigned long)MEM1_BASE, (unsigned long)MEM1_SIZE);
    }
  }
  snprintf(path, sizeof(path), "%s/image.bin" GZ_SUFFIX, dir);
  gzFile z = gzopen(path, "wb1");
  if (z != NULL) {
    unsigned long at = 0;
    for (int i = 0; i < s_sectionCount; i++) {
      const Section* s = &s_sections[i];
      if (s->code) {
        fprintf(f, "code %s %08lx %08lx\n", s->name, (unsigned long)s->addr, (unsigned long)s->size);
        continue;
      }
      /* the linker's own tables: megabytes that say nothing about the game */
      static const char* const skip[] = {".interp", ".note", ".gnu", ".dyn", ".rel", ".eh_fram", ".gcc_except", ".got",
                                         ".reloc", ".rsrc",  ".idata", ".pdata", ".xdata", ".edata", NULL};
      bool skipped = false;
      for (int k = 0; skip[k] != NULL; k++) {
        skipped = skipped || strncmp(s->name, skip[k], strlen(skip[k])) == 0;
      }
      if (skipped) {
        continue;
      }
      write_memory(z, s->addr, s->size);
      fprintf(f, "region %s %08lx %08lx image.bin" GZ_SUFFIX " %lu\n", s->name, (unsigned long)s->addr,
              (unsigned long)s->size, at);
      at += (unsigned long)s->size;
    }
    gzclose(z);
  }
  write_modules(f);
  fclose(f);

  snprintf(path, sizeof(path), "%s/symbols.txt" GZ_SUFFIX, dir);
  z = gzopen(path, "wb6");
  if (z != NULL) {
    char line[600];
    for (size_t i = 0; i < s_symCount; i++) {
      int n = snprintf(line, sizeof(line), "%08lx %c %s\n", (unsigned long)s_syms[i].addr, s_syms[i].kind, s_syms[i].name);
      gzwrite(z, line, (unsigned)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
    }
    gzclose(z);
  }
}

#ifdef _WIN32
/* dbghelp.dll is part of Windows; loaded only when a dump is written */
typedef struct {
  DWORD ThreadId;
  EXCEPTION_POINTERS* ExceptionPointers;
  BOOL ClientPointers;
} DiagMinidumpException;
typedef BOOL(WINAPI* DiagMiniDumpWriteDump)(HANDLE process, DWORD pid, HANDLE file, int type, DiagMinidumpException* exc,
                                           void* user, void* callback);
static bool write_minidump(const char* dir, EXCEPTION_POINTERS* info) {
  char path[700];
  HMODULE lib = LoadLibraryA("dbghelp.dll");
  DiagMiniDumpWriteDump write = lib != NULL ? (DiagMiniDumpWriteDump)(void*)GetProcAddress(lib, "MiniDumpWriteDump") : NULL;
  if (write == NULL) {
    return false;
  }
  snprintf(path, sizeof(path), "%s/crash.dmp", dir);
  HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  DiagMinidumpException exc = {GetCurrentThreadId(), info, FALSE};
  /* MiniDumpNormal | WithUnloadedModules (0x20) | WithIndirectlyReferencedMemory
   * (0x40: what registers and stack words point at) | WithThreadInfo (0x1000) */
  BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), file, 0x1060, info != NULL ? &exc : NULL, NULL, NULL);
  CloseHandle(file);
  return ok != 0;
}
#endif

/* MKGP2PC_DUMP_MEMORY=0: no memory files (a dump is then a few hundred KB
 * smaller than the picture) */
static bool memory_wanted(void) {
  const char* v = getenv("MKGP2PC_DUMP_MEMORY");
  return v == NULL || strcmp(v, "0") != 0;
}

/* F9: called by video_aurora.c with the dump's folder. */
void mkgp2pc_diag_dump(const char* dir) {
  Shot here;
  exe_need();
  maps_read();
  shot_here(&here);
  write_stacks(dir, "dump on request", &here);
  if (memory_wanted()) {
    write_images(dir);
  }
}

/* ---- dying --------------------------------------------------------------- */
static void crash_dir(char* dir, size_t size) {
  const char* base = getenv("MKGP2PC_DUMP_DIR");
  if (base == NULL || *base == 0) {
    base = "dumps";
  }
  make_dir(base);
  /* not time(): in the game's link that name is one of the game's own */
  char stamp[32] = "unknown";
#ifdef _WIN32
  SYSTEMTIME st;
  GetLocalTime(&st);
  snprintf(stamp, sizeof(stamp), "%04u%02u%02u_%02u%02u%02u", (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
           (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond);
#else
  struct timespec ts;
  struct tm tm;
  clock_gettime(CLOCK_REALTIME, &ts);
  if (localtime_r(&ts.tv_sec, &tm) != NULL) {
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm);
  }
#endif
  snprintf(dir, size, "%s/crash_%s", base, stamp);
  make_dir(dir);
}

/* The lines for the log first, then the folder. `t` holds the dying thread
 * with its stack already copied; `osInfo` is the EXCEPTION_POINTERS on
 * Windows. */
static void crash_write(const char* why, const Shot* t, void* osInfo) {
  char dir[600];
  exe_need();
  s_file = NULL;
  s_toLog = true;
  out("%s\n", why);
  shot_print(t, "");
  s_toLog = false;
  fflush(stderr);
  crash_dir(dir, sizeof(dir));
  fprintf(stderr, "crash: writing %s\n", dir);
  write_stacks(dir, why, t);
#ifdef _WIN32
  fprintf(stderr, "crash: crash.dmp %s\n", write_minidump(dir, (EXCEPTION_POINTERS*)osInfo) ? "written" : "not written");
#else
  (void)osInfo;
#endif
  if (mkgp2pc_diag_extra != NULL) {
    mkgp2pc_diag_extra(dir);
  }
  write_images(dir);
  fprintf(stderr, "crash: %s complete\n", dir);
  fflush(stderr);
}

static volatile long s_dying;
static bool dying_first(void) {
#ifdef _WIN32
  return InterlockedExchange((LONG volatile*)&s_dying, 1) == 0;
#else
  return __sync_lock_test_and_set(&s_dying, 1) == 0;
#endif
}

/* A fatal message of the rendering library (video_aurora.c, log_callback,
 * which calls abort() next), or any other place that gives up on purpose. */
void mkgp2pc_diag_fatal(const char* why) {
  Shot here;
  if (!dying_first()) {
    return;
  }
  maps_read();
  exe_need();
  shot_here(&here);
  crash_write(why, &here, NULL);
}

#ifdef _WIN32
static bool exception_is_crash(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_BREAKPOINT: /* what a library's own abort often is */
      return true;
    default:
      return false;
  }
}
static void exception_report(EXCEPTION_POINTERS* info) {
  static Shot t;
  static uintptr_t stack[STACK_WORDS];
  char why[200];
  const EXCEPTION_RECORD* rec = info->ExceptionRecord;
  int n = snprintf(why, sizeof(why), "exception %08lx in thread %lu", (unsigned long)rec->ExceptionCode,
                   (unsigned long)GetCurrentThreadId());
  if ((rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || rec->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
      rec->NumberParameters >= 2) {
    snprintf(why + n, sizeof(why) - (size_t)n, ", %s address %08lx",
             rec->ExceptionInformation[0] == 1 ? "writing" : rec->ExceptionInformation[0] == 8 ? "executing" : "reading",
             (unsigned long)rec->ExceptionInformation[1]);
  }
  memset(&t, 0, sizeof(t));
  t.stack = stack;
  t.id = GetCurrentThreadId();
  t.current = true;
  shot_from_context(&t, info->ContextRecord);
  shot_copy_stack(&t);
  t.ok = true;
  crash_write(why, &t, info);
}
static LONG CALLBACK exception_first(EXCEPTION_POINTERS* info) {
  if (exception_is_crash(info->ExceptionRecord->ExceptionCode) && dying_first()) {
    exception_report(info);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}
/* whatever else nobody handled (a C++ exception, for one) */
static LONG WINAPI exception_unhandled(EXCEPTION_POINTERS* info) {
  if (dying_first()) {
    exception_report(info);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}
static void abort_signal(int sig) {
  (void)sig;
  mkgp2pc_diag_fatal("abort() called");
  signal(SIGABRT, SIG_DFL);
}
void mkgp2pc_diag_install(void) {
  AddVectoredExceptionHandler(1, &exception_first);
  SetUnhandledExceptionFilter(&exception_unhandled);
  signal(SIGABRT, &abort_signal);
}
#else
static void crash_signal(int sig, siginfo_t* info, void* uc) {
  static Shot t;
  static uintptr_t stack[STACK_WORDS];
  /* the default action is back in place: a second fault ends the process */
  if (dying_first()) {
    char why[200];
    static const struct {
      int sig;
      const char* name;
    } names[] = {{SIGSEGV, "SIGSEGV"}, {SIGBUS, "SIGBUS"}, {SIGILL, "SIGILL"}, {SIGFPE, "SIGFPE"}, {SIGABRT, "SIGABRT (abort)"}};
    const char* name = "signal";
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
      if (names[i].sig == sig) {
        name = names[i].name;
      }
    }
    int n = snprintf(why, sizeof(why), "%s in thread %ld", name, (long)syscall(SYS_gettid));
    if (sig == SIGSEGV || sig == SIGBUS) {
      snprintf(why + n, sizeof(why) - (size_t)n, ", address %08lx", (unsigned long)(uintptr_t)info->si_addr);
    }
    maps_read();
    memset(&t, 0, sizeof(t));
    t.stack = stack;
    t.id = (unsigned long)syscall(SYS_gettid);
    t.current = true;
    shot_from_ucontext(&t, (const ucontext_t*)uc);
    shot_copy_stack(&t);
    t.ok = true;
    crash_write(why, &t, NULL);
  }
  raise(sig);
}
void mkgp2pc_diag_install(void) {
  static const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = &crash_signal;
  sa.sa_flags = SA_SIGINFO | SA_RESETHAND | SA_NODEFER;
  for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
    sigaction(sigs[i], &sa, NULL);
  }
}
#endif

#ifdef MKGP2PC_DIAG_TEST
/* Built alone (see docs/port/dumps.md): `diag_test dump|crash|abort|fatal`. */
#ifdef _WIN32
static DWORD WINAPI test_thread(LPVOID arg) {
  (void)arg;
  Sleep(100000);
  return 0;
}
#else
#include <pthread.h>
static void* test_thread(void* arg) {
  (void)arg;
  sleep(100000);
  return NULL;
}
#endif
int mkgp2pc_test_global = 0x12345678;
__attribute__((noinline)) int mkgp2pc_test_inner(const char* mode, volatile int* p) {
  if (strcmp(mode, "dump") == 0) {
    make_dir("dumps");
    make_dir("dumps/test");
    mkgp2pc_diag_dump("dumps/test");
  } else if (strcmp(mode, "crash") == 0) {
    *p = 1;
  } else if (strcmp(mode, "abort") == 0) {
    abort();
  } else {
    mkgp2pc_diag_fatal("fatal message (test)");
  }
  return 1;
}
__attribute__((noinline)) int mkgp2pc_test_outer(const char* mode) { return mkgp2pc_test_inner(mode, (volatile int*)8) + 1; }
int main(int argc, char** argv) {
  mkgp2pc_diag_install();
#ifdef _WIN32
  CreateThread(NULL, 0, &test_thread, NULL, 0, NULL);
  Sleep(100);
#else
  pthread_t th;
  pthread_create(&th, NULL, &test_thread, NULL);
  usleep(100000);
#endif
  return mkgp2pc_test_outer(argc > 1 ? argv[1] : "dump") != 2;
}
#endif
