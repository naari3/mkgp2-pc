/*
 * OSResetSystem: the game restarts itself.
 *
 * The cabinet's test mode is not a scene the game switches to. On the test
 * button the game shuts its devices down (Game_Shutdown), leaves 256 bytes
 * for its next start with OSSetSaveRegion (first word 1: "start in test
 * mode") and calls OSResetSystem(0, 0, 0); the console restarts the program
 * and BootDispatcher reads the bytes back with OSGetSavedRegion. The media
 * board's error path (acLib) resets too, without saved bytes.
 *
 * EXIT in the test menu calls the same Game_Shutdown, first word 1 again:
 * by the game's code alone the next start would be the test mode once more.
 * On the board the program that starts the game (not on the disc, not
 * decompiled) stands between the two; what it does with the bytes is not
 * known. This port's stand-in: a run that was started with saved bytes
 * (the test mode) passes none on, so the reset after EXIT starts the game.
 *
 * Here the reset ends the process and the program is started again with the
 * saved bytes:
 *
 *   Linux    the process replaces itself (execv of /proc/self/exe); the
 *            bytes travel in MKGP2PC_SAVED_REGION.
 *   Windows  a process cannot replace itself, so the program first started
 *            only supervises: it starts itself once more as the game, waits,
 *            and starts it again when the game leaves with RESET_EXIT_CODE;
 *            any other exit code becomes its own. The bytes are in a small
 *            shared mapping whose handle the game inherits
 *            (MKGP2PC_RESET_MAP). What started the program (the launcher,
 *            run-gfx.cmd) sees one process from the first start to the last
 *            exit. MKGP2PC_NO_SUPERVISOR=1 runs the game in the first
 *            process as before; a reset then just ends it.
 *
 * Scripted inputs (MKGP2PC_SCRIPT, MKGP2PC_SCRIPT_FILE) count retraces from
 * the start of the process and would press the same buttons again, so they
 * are dropped at a reset; MKGP2PC_RESET_SCRIPT_FILE names the script file of
 * the run after the reset (it is used once).
 *
 * Like the window's close button (port/gfx/video_aurora.c, quit_now) the
 * reset ends the process at once, without the rendering library's shutdown.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#define REGION_MAX 0x1000u
#define RESET_EXIT_CODE 0x52535431u /* "RST1" */

/* what the game set for its next start */
static void* s_nextStart;
static void* s_nextEnd;
/* what the run before this one left */
static uint8_t s_saved[REGION_MAX];
static uint32_t s_savedSize;

void OSGetSavedRegion(void** start, void** end) {
  *start = s_saved;
  *end = s_saved + s_savedSize;
}
void OSSetSaveRegion(void* start, void* end) {
  s_nextStart = start;
  s_nextEnd = end;
}

static uint32_t next_size(void) {
  uintptr_t a = (uintptr_t)s_nextStart, b = (uintptr_t)s_nextEnd;
  /* (s_savedSize: this run is the test mode; see the top of the file) */
  if (s_savedSize != 0 || s_nextStart == NULL || b <= a) {
    return 0;
  }
  return b - a > REGION_MAX ? REGION_MAX : (uint32_t)(b - a);
}

#ifdef _WIN32

/* the shared mapping: the size of the region, then its bytes */
typedef struct ResetShare {
  uint32_t size;
  uint8_t bytes[REGION_MAX];
} ResetShare;
static ResetShare* s_share;

static void next_script(void) {
  wchar_t path[1024];
  DWORD n = GetEnvironmentVariableW(L"MKGP2PC_RESET_SCRIPT_FILE", path, 1024);
  SetEnvironmentVariableW(L"MKGP2PC_SCRIPT", NULL);
  SetEnvironmentVariableW(L"MKGP2PC_SCRIPT_FILE", n > 0 && n < 1024 ? path : NULL);
  SetEnvironmentVariableW(L"MKGP2PC_RESET_SCRIPT_FILE", NULL);
}

/* Returns only when no game process could be started. */
static void supervise(void) {
  SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
  HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(ResetShare), NULL);
  ResetShare* share = map != NULL ? (ResetShare*)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ResetShare)) : NULL;
  if (share == NULL) {
    return;
  }
  wchar_t text[32];
  swprintf(text, 32, L"%lu", (unsigned long)(uintptr_t)map);
  SetEnvironmentVariableW(L"MKGP2PC_RESET_MAP", text);
  static wchar_t exe[32768];
  DWORD len = GetModuleFileNameW(NULL, exe, 32768);
  if (len == 0 || len >= 32768) {
    SetEnvironmentVariableW(L"MKGP2PC_RESET_MAP", NULL);
    return;
  }
  /* The game ends with the supervisor, however that one ends. */
  HANDLE job = CreateJobObjectW(NULL, NULL);
  if (job != NULL) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    memset(&limits, 0, sizeof(limits));
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
  }
  for (int run = 0;; ++run) {
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    /* CreateProcess may write to the command line it is given. */
    size_t chars = wcslen(GetCommandLineW()) + 1;
    wchar_t* command = (wchar_t*)malloc(chars * sizeof(wchar_t));
    if (command == NULL) {
      break;
    }
    memcpy(command, GetCommandLineW(), chars * sizeof(wchar_t));
    /* Started without a console (the launcher): the game gets none either. */
    DWORD flags = GetConsoleWindow() != NULL ? 0 : CREATE_NO_WINDOW;
    BOOL ok = CreateProcessW(exe, command, NULL, NULL, TRUE, flags, NULL, NULL, &si, &pi);
    free(command);
    if (!ok) {
      if (run == 0) {
        break; /* the game runs in this process, without resets */
      }
      fprintf(stderr, "reset: the game could not be started again (error %lu)\n", (unsigned long)GetLastError());
      fflush(NULL);
      ExitProcess(1);
    }
    if (job != NULL) {
      AssignProcessToJobObject(job, pi.hProcess);
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (code != RESET_EXIT_CODE) {
      ExitProcess(code);
    }
    next_script();
  }
  SetEnvironmentVariableW(L"MKGP2PC_RESET_MAP", NULL);
  if (job != NULL) {
    CloseHandle(job);
  }
  UnmapViewOfFile(share);
  CloseHandle(map);
}

/* (priority 101: with the first start-up functions, before the game's
 * static initialisers; the supervisor never gets to those) */
__attribute__((constructor(101))) static void reset_start(void) {
  const char* text = getenv("MKGP2PC_RESET_MAP");
  if (text == NULL) {
    if (getenv("MKGP2PC_NO_SUPERVISOR") == NULL) {
      supervise();
    }
    return;
  }
  HANDLE map = (HANDLE)(uintptr_t)strtoul(text, NULL, 10);
  s_share = (ResetShare*)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ResetShare));
  if (s_share == NULL) {
    return;
  }
  if (s_share->size > 0 && s_share->size <= REGION_MAX) {
    s_savedSize = s_share->size;
    memcpy(s_saved, s_share->bytes, s_savedSize);
    fprintf(stderr, "reset: started again, %u bytes saved, first word %u\n", (unsigned)s_savedSize, (unsigned)*(uint32_t*)s_saved);
  }
  s_share->size = 0;
}

static void restart(void) {
  uint32_t size = next_size();
  if (s_share == NULL) {
    fprintf(stderr, "reset: nothing supervises this process (MKGP2PC_NO_SUPERVISOR?); leaving\n");
    fflush(NULL);
    TerminateProcess(GetCurrentProcess(), 0);
    _exit(0);
  }
  memcpy(s_share->bytes, s_nextStart, size);
  s_share->size = size;
  fprintf(stderr, "reset: starting again, %u bytes saved\n", (unsigned)size);
  fflush(NULL);
  TerminateProcess(GetCurrentProcess(), RESET_EXIT_CODE);
  _exit(0);
}

#else

__attribute__((constructor(101))) static void reset_start(void) {
  const char* text = getenv("MKGP2PC_SAVED_REGION");
  if (text == NULL) {
    return;
  }
  uint32_t n = 0;
  unsigned byte;
  while (n < REGION_MAX && sscanf(text + 2 * n, "%2x", &byte) == 1) {
    s_saved[n++] = (uint8_t)byte;
  }
  s_savedSize = n;
  unsetenv("MKGP2PC_SAVED_REGION");
  if (n >= 4) {
    fprintf(stderr, "reset: started again, %u bytes saved, first word %u\n", (unsigned)n, (unsigned)*(uint32_t*)s_saved);
  }
}

static void restart(void) {
  uint32_t size = next_size();
  static char text[2 * REGION_MAX + 1];
  for (uint32_t i = 0; i < size; ++i) {
    sprintf(text + 2 * i, "%02x", ((const uint8_t*)s_nextStart)[i]);
  }
  setenv("MKGP2PC_SAVED_REGION", text, 1);
  const char* script = getenv("MKGP2PC_RESET_SCRIPT_FILE");
  unsetenv("MKGP2PC_SCRIPT");
  if (script != NULL) {
    setenv("MKGP2PC_SCRIPT_FILE", script, 1);
    unsetenv("MKGP2PC_RESET_SCRIPT_FILE");
  } else {
    unsetenv("MKGP2PC_SCRIPT_FILE");
  }
  /* the arguments of this process, as the kernel keeps them */
  static char args[4096];
  static char* argv[64];
  int argc = 0;
  FILE* f = fopen("/proc/self/cmdline", "rb");
  if (f != NULL) {
    size_t got = fread(args, 1, sizeof(args) - 1, f);
    fclose(f);
    for (size_t at = 0; at < got && argc < 63; at += strlen(args + at) + 1) {
      argv[argc++] = args + at;
    }
  }
  if (argc == 0) {
    argv[argc++] = "mkgp2-pc";
  }
  argv[argc] = NULL;
  fprintf(stderr, "reset: starting again, %u bytes saved\n", (unsigned)size);
  fflush(NULL);
  execv("/proc/self/exe", argv);
  perror("reset: execv");
  _exit(1);
}

#endif

void OSResetSystem(int reset, uint32_t resetCode, int forceMenu) {
  (void)reset, (void)resetCode, (void)forceMenu; /* the game only ever asks for a restart: (0, 0, 0) */
  restart();
}
