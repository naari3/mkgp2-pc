#ifndef MKGP2PC_DEBUG_H
#define MKGP2PC_DEBUG_H

/*
 * The game's own debug facilities, brought back (port/platform/debug/debug.c,
 * docs/port/debug.md). The arcade program still calls them everywhere, but
 * the retail build emptied the functions at the end of each path: OSReport,
 * OSPanic and the drawing behind OverlayText. Game sources reach this file
 * through the patches of Debug.c, DebugTextOutput.c and main.c.
 */
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MKGP2PC_DEBUG=1 at start, the "debug" key (F3) while running. */
int mkgp2pc_debug_on(void);
void mkgp2pc_debug_set(int on);
/* Once per retrace: keeps the game's "debug display" byte in step. */
void mkgp2pc_debug_frame(void);
/* The game's overlay page (0 = none, 1..13), forwards or backwards. */
void mkgp2pc_debug_page(int step);

/* OSReport and OSPanic of the game (src/game/Debug.c). */
void mkgp2pc_debug_vreport(const char* fmt, va_list args);
void mkgp2pc_debug_vpanic(const char* file, int line, const char* fmt, va_list args);

/* OverlayText: one string on the game's character grid, for this frame. */
void mkgp2pc_debug_overlay_text(int layer, int column, int row, const char* text);
int mkgp2pc_debug_overlay_count(void);
/* text is UTF-8; returns 0 past the end */
int mkgp2pc_debug_overlay_get(int index, int* layer, int* column, int* row, const char** text);
void mkgp2pc_debug_overlay_clear(void);

/* MKGP2PC_START_SCENE: the entry of the game's scene table that main()
 * starts with, in place of 3 (normal flow) or 12 (test mode). */
int mkgp2pc_debug_start_scene(int mode);

#ifdef __cplusplus
}
#endif

#endif
