/*
 * Picture of the game's window for a dump (port/gfx/video_aurora.c), used
 * when Aurora cannot read its frame buffer back. Windows only: the window
 * is asked to draw itself into a bitmap (PrintWindow with
 * PW_RENDERFULLCONTENT, which also works for a Direct3D swap chain).
 * Elsewhere the function reports that it has nothing.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#ifdef _WIN32
#include <windows.h>

bool mkgp2pc_window_shot(SDL_Window* window, uint8_t** rgba, uint32_t* width, uint32_t* height) {
  if (window == NULL) {
    return false;
  }
  HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
  RECT rc;
  if (hwnd == NULL || !GetClientRect(hwnd, &rc) || rc.right <= 0 || rc.bottom <= 0) {
    return false;
  }
  const int w = rc.right, h = rc.bottom;
  HDC wdc = GetDC(hwnd);
  HDC mdc = CreateCompatibleDC(wdc);
  BITMAPINFO bi;
  ZeroMemory(&bi, sizeof(bi));
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h; /* top-down */
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = NULL;
  HBITMAP bmp = CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
  bool ok = false;
  if (bmp != NULL && bits != NULL) {
    HGDIOBJ old = SelectObject(mdc, bmp);
    ok = PrintWindow(hwnd, mdc, 1 /* PW_CLIENTONLY */ | 2 /* PW_RENDERFULLCONTENT */) != 0;
    GdiFlush();
    if (ok) {
      uint8_t* out = (uint8_t*)malloc((size_t)w * (size_t)h * 4);
      if (out != NULL) {
        const uint8_t* s = (const uint8_t*)bits;
        for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
          out[i * 4 + 0] = s[i * 4 + 2];
          out[i * 4 + 1] = s[i * 4 + 1];
          out[i * 4 + 2] = s[i * 4 + 0];
          out[i * 4 + 3] = 255;
        }
        *rgba = out;
        *width = (uint32_t)w;
        *height = (uint32_t)h;
      } else {
        ok = false;
      }
    }
    SelectObject(mdc, old);
  }
  if (bmp != NULL) {
    DeleteObject(bmp);
  }
  DeleteDC(mdc);
  ReleaseDC(hwnd, wdc);
  return ok;
}
#else
bool mkgp2pc_window_shot(SDL_Window* window, uint8_t** rgba, uint32_t* width, uint32_t* height) {
  (void)window;
  (void)rgba;
  (void)width;
  (void)height;
  return false;
}
#endif


/*
 * Crash record. The Windows build dies inside webgpu_dawn.dll now and then
 * (access violation, always the same place; 2026-11-11: at exit, after the
 * frame limit was lifted, on a window resize, in the middle of play) and the
 * system's record names only the module. This writes where the faulting
 * thread was to stderr (gfx.log): the faulting address, then every value on
 * its stack that points into code, each as module+offset. Addresses in
 * mkgp2-gfx.exe resolve with addr2line against the executable. The
 * exception is then left to the system as before.
 */
#ifdef _WIN32
#include <stdio.h>

static void crash_where(const char* label, uintptr_t addr) {
  MEMORY_BASIC_INFORMATION mi;
  char name[MAX_PATH];
  if (VirtualQuery((void*)addr, &mi, sizeof(mi)) == 0 || mi.Type != MEM_IMAGE || mi.AllocationBase == NULL ||
      GetModuleFileNameA((HMODULE)mi.AllocationBase, name, sizeof(name)) == 0) {
    fprintf(stderr, "crash: %s %08lx\n", label, (unsigned long)addr);
    return;
  }
  const char* base = strrchr(name, '\\');
  fprintf(stderr, "crash: %s %s+0x%lx\n", label, base ? base + 1 : name,
          (unsigned long)(addr - (uintptr_t)mi.AllocationBase));
}

static bool crash_is_code(uintptr_t addr) {
  MEMORY_BASIC_INFORMATION mi;
  return VirtualQuery((void*)addr, &mi, sizeof(mi)) != 0 && mi.State == MEM_COMMIT && mi.Type == MEM_IMAGE &&
         (mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

static LONG CALLBACK crash_handler(EXCEPTION_POINTERS* info) {
  static LONG done;
  const EXCEPTION_RECORD* rec = info->ExceptionRecord;
  if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION && rec->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION &&
      rec->ExceptionCode != EXCEPTION_STACK_OVERFLOW && rec->ExceptionCode != EXCEPTION_INT_DIVIDE_BY_ZERO) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  if (InterlockedExchange(&done, 1) != 0) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  fprintf(stderr, "crash: exception %08lx in thread %lu", (unsigned long)rec->ExceptionCode,
          (unsigned long)GetCurrentThreadId());
  if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
    fprintf(stderr, ", %s address %08lx", rec->ExceptionInformation[0] ? "writing" : "reading",
            (unsigned long)rec->ExceptionInformation[1]);
  }
  fprintf(stderr, "\n");
  crash_where("at", (uintptr_t)rec->ExceptionAddress);
#if defined(__i386__) || defined(_M_IX86)
  const CONTEXT* c = info->ContextRecord;
  fprintf(stderr, "crash: eax %08lx ebx %08lx ecx %08lx edx %08lx esi %08lx edi %08lx ebp %08lx esp %08lx\n",
          (unsigned long)c->Eax, (unsigned long)c->Ebx, (unsigned long)c->Ecx, (unsigned long)c->Edx,
          (unsigned long)c->Esi, (unsigned long)c->Edi, (unsigned long)c->Ebp, (unsigned long)c->Esp);
  /* every stack word that points into code: return addresses, with some
   * stale ones in between */
  const uintptr_t* sp = (const uintptr_t*)(uintptr_t)c->Esp;
  MEMORY_BASIC_INFORMATION mi;
  int shown = 0;
  for (int i = 0; i < 1024 && shown < 48; i++) {
    if ((i & 255) == 0 && (VirtualQuery(sp + i, &mi, sizeof(mi)) == 0 || mi.State != MEM_COMMIT ||
                           (uintptr_t)mi.BaseAddress + mi.RegionSize < (uintptr_t)(sp + i + 256))) {
      break;
    }
    if (crash_is_code(sp[i])) {
      char label[24];
      snprintf(label, sizeof(label), "stack+%03x", (unsigned)(i * sizeof(uintptr_t)));
      crash_where(label, sp[i]);
      shown++;
    }
  }
#endif
  fflush(stderr);
  return EXCEPTION_CONTINUE_SEARCH;
}

void mkgp2pc_crash_log_install(void) { AddVectoredExceptionHandler(1, &crash_handler); }
#else
void mkgp2pc_crash_log_install(void) {}
#endif
