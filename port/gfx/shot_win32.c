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
