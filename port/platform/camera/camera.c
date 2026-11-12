/*
 * The cabinet's player camera ("NamCam").
 *
 * On the cabinet the camera is a small web server on the cabinet's own
 * network: cabinet N asks 192.168.29.(103+N), port 80, with
 * "GET /img.jpg HTTP/1.0" for one JPEG picture at a time
 * (src/game/NamCam_GetImageHandle.c), decodes it with the THP library's
 * still-picture functions into a 320x240 RGBA8 texture
 * (src/game/Asset_LoadAndDecompressByType.c) and draws from that. Nothing of
 * the game is changed here. The pieces on the PC:
 *
 *  - triforce/netlink.c answers the game's connection to that address and
 *    gets the HTTP answer from mkgp2pc_camera_reply() below.
 *  - This file keeps the newest picture of the source, 320x240, and builds
 *    the answer from it.
 *  - The picture in the answer is NOT a compressed JPEG. It has a JPEG's
 *    first and last bytes and frame header, which is all the game itself
 *    looks at, and the pixels uncompressed in between. The game's decoder
 *    (src/thp/THPDec.c, PowerPC assembly inside) is not part of the PC
 *    build; the four functions of it that the camera path calls are written
 *    here and read this form only. A JPEG from anywhere else is refused.
 *
 * Source of the picture, MKGP2PC_CAMERA:
 *   not set, "", "0", "off"   no camera, as before this file existed: the
 *                             address is one nobody answers at, and the game
 *                             is told to do without its camera
 *                             (triforce/card_reader.c)
 *   "default" or "1"          the first camera of the system
 *   a number N                the Nth camera
 *   any other text            the camera whose name contains the text
 *   "test"                    a moving test picture, needs no device
 *   "file:<path>"             a still picture, an uncompressed 24- or
 *                             32-bit BMP file of any size
 * MKGP2PC_CAMERA_MIRROR=1 shows the picture mirrored.
 * MKGP2PC_TRACE_CAMERA=1 logs requests.
 *
 * A device is open only while the game asks for pictures: it is opened on
 * the first request and let go after kIdleMs without one. Capturing runs on
 * its own thread (the device back end waits for pictures); the game's
 * thread only ever copies the newest picture.
 *
 * Devices: camera_mf.c (Windows, Media Foundation). Other systems have
 * the test picture and the file only.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
typedef CRITICAL_SECTION Lock;
#define lock(l) EnterCriticalSection(l)
#define unlock(l) LeaveCriticalSection(l)
static uint64_t now_ms(void) { return GetTickCount64(); }
#else
#include <pthread.h>
#include <time.h>
typedef pthread_mutex_t Lock;
#define lock(l) pthread_mutex_lock(l)
#define unlock(l) pthread_mutex_unlock(l)
static uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
#endif

#include <mkgp2pc/camera.h>

enum {
  W = MKGP2PC_CAMERA_WIDTH,
  H = MKGP2PC_CAMERA_HEIGHT,
  kPixels = W * H * 3,
  kIdleMs = 5000,    /* a device nobody asks is let go after this */
  kRetryMs = 3000,   /* a device that could not be opened is tried again after this */
  kPatienceMs = 1500 /* a request is answered with what there is after this */
};

typedef enum { OFF, TEST, STILL, DEVICE } Mode;
typedef enum { IDLE, RUNNING, STOPPING } Run;

static int s_state; /* 0 not looked at, 1 looked at */
static Mode s_mode;
static char s_name[512]; /* device name, or the file's path */
static int s_mirror, s_trace;

static Lock s_lock;
static uint8_t s_frame[kPixels]; /* newest picture, rows from the top, red first */
static uint64_t s_serial;        /* pictures stored so far */
static Run s_run;
static uint64_t s_lastUse, s_lastTry;
static int s_tried;

static int s_pending; /* a request is waiting for its picture */
static uint64_t s_requestSerial, s_requestTime, s_requests;
static uint8_t s_reply[128 + kPixels];
static uint32_t s_replySize;

#define LOG(...) \
  do { \
    fprintf(stderr, "camera: " __VA_ARGS__); \
    fputc('\n', stderr); \
  } while (0)

static void configure(void) {
  const char* e;
  if (s_state != 0) {
    return;
  }
  s_state = 1;
#ifdef _WIN32
  InitializeCriticalSection(&s_lock);
#else
  pthread_mutex_init(&s_lock, NULL);
#endif
  memset(s_frame, 0x40, sizeof(s_frame));
  e = getenv("MKGP2PC_CAMERA_MIRROR");
  s_mirror = e != NULL && e[0] != 0 && e[0] != '0';
  e = getenv("MKGP2PC_TRACE_CAMERA");
  s_trace = e != NULL && e[0] != 0 && e[0] != '0';
  e = getenv("MKGP2PC_CAMERA");
  if (e == NULL || e[0] == 0 || strcmp(e, "0") == 0 || strcmp(e, "off") == 0) {
    s_mode = OFF;
    return;
  }
  if (strcmp(e, "test") == 0) {
    s_mode = TEST;
    LOG("test picture");
    return;
  }
  if (strncmp(e, "file:", 5) == 0) {
    s_mode = STILL;
    snprintf(s_name, sizeof(s_name), "%s", e + 5);
    LOG("still picture %s", s_name);
    return;
  }
  s_mode = DEVICE;
  snprintf(s_name, sizeof(s_name), "%s", strcmp(e, "default") == 0 ? "" : e);
  LOG("device %s", s_name[0] ? s_name : "(the first one)");
}

int mkgp2pc_camera_enabled(void) {
  configure();
  return s_mode != OFF;
}

/* ---- pictures --------------------------------------------------------------- */

/* Any size to 320x240: the middle 4:3 part of the source, every target
 * pixel the average of the source pixels it covers. */
static void scale(uint8_t* out, const uint8_t* pixels, int width, int height, int stride, int bpp) {
  int cropW = width, cropH = height, left, top;
  if (width * 3 > height * 4) {
    cropW = height * 4 / 3;
  } else {
    cropH = width * 3 / 4;
  }
  if (cropW < 1 || cropH < 1) {
    return;
  }
  left = (width - cropW) / 2;
  top = (height - cropH) / 2;
  for (int y = 0; y < H; y++) {
    int y0 = top + y * cropH / H, y1 = top + (y + 1) * cropH / H;
    if (y1 <= y0) {
      y1 = y0 + 1;
    }
    for (int x = 0; x < W; x++) {
      int x0 = left + x * cropW / W, x1 = left + (x + 1) * cropW / W;
      unsigned sum[3] = {0, 0, 0}, n;
      uint8_t* o = out + (y * W + (s_mirror ? W - 1 - x : x)) * 3;
      if (x1 <= x0) {
        x1 = x0 + 1;
      }
      n = (unsigned)((x1 - x0) * (y1 - y0));
      for (int sy = y0; sy < y1; sy++) {
        const uint8_t* p = pixels + (ptrdiff_t)sy * stride + x0 * bpp;
        for (int sx = x0; sx < x1; sx++, p += bpp) {
          sum[0] += p[2], sum[1] += p[1], sum[2] += p[0];
        }
      }
      o[0] = (uint8_t)(sum[0] / n), o[1] = (uint8_t)(sum[1] / n), o[2] = (uint8_t)(sum[2] / n);
    }
  }
}

void mkgp2pc_camera_store(const uint8_t* pixels, int width, int height, int stride, int bytesPerPixel) {
  static uint8_t next[kPixels]; /* capture thread only */
  if (pixels == NULL || width < 1 || height < 1 || (bytesPerPixel != 3 && bytesPerPixel != 4)) {
    return;
  }
  scale(next, pixels, width, height, stride, bytesPerPixel);
  lock(&s_lock);
  memcpy(s_frame, next, kPixels);
  s_serial++;
  unlock(&s_lock);
}

/* Colour bars with a block that moves, so that one picture can be told from
 * the next. */
static void test_picture(uint8_t* out, uint64_t step) {
  static const uint8_t bars[8][3] = {{235, 235, 235}, {235, 235, 16}, {16, 235, 235}, {16, 235, 16},
                                     {235, 16, 235},  {235, 16, 16},  {16, 16, 235},  {16, 16, 16}};
  int bx = (int)(step * 4 % (W - 48)), by = H / 2 - 24;
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      const uint8_t* c = bars[x * 8 / W];
      uint8_t* o = out + (y * W + (s_mirror ? W - 1 - x : x)) * 3;
      if (y >= H * 3 / 4) { /* a ramp below the bars */
        o[0] = o[1] = o[2] = (uint8_t)(x * 255 / (W - 1));
      } else if (x >= bx && x < bx + 48 && y >= by && y < by + 48) {
        o[0] = 255, o[1] = 128, o[2] = 0;
      } else {
        o[0] = c[0], o[1] = c[1], o[2] = c[2];
      }
    }
  }
}

static uint32_t le32(const uint8_t* p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* An uncompressed 24- or 32-bit BMP file. */
static int load_bmp(const char* path, uint8_t* out) {
  FILE* f = fopen(path, "rb");
  uint8_t head[54];
  uint8_t* pixels;
  int32_t width, height;
  uint32_t offset, bits, compression;
  int bpp, rows, stride, ok = 0;
  if (f == NULL) {
    LOG("%s cannot be opened", path);
    return 0;
  }
  if (fread(head, 1, sizeof(head), f) != sizeof(head) || head[0] != 'B' || head[1] != 'M') {
    LOG("%s is not a BMP file", path);
    fclose(f);
    return 0;
  }
  offset = le32(head + 10);
  width = (int32_t)le32(head + 18);
  height = (int32_t)le32(head + 22);
  bits = head[28] | (uint32_t)head[29] << 8;
  compression = le32(head + 30);
  rows = height < 0 ? -height : height;
  bpp = (int)bits / 8;
  /* 3 = bit fields, which for 32 bits is in practice the plain layout */
  if ((bits != 24 && bits != 32) || (compression != 0 && !(compression == 3 && bits == 32)) || width < 1 || rows < 1 ||
      width > 8192 || rows > 8192) {
    LOG("%s: only uncompressed 24- and 32-bit BMP files are read (this one: %u bits, compression %u)", path, bits, compression);
    fclose(f);
    return 0;
  }
  stride = (width * bpp + 3) & ~3;
  pixels = (uint8_t*)malloc((size_t)stride * (size_t)rows);
  if (pixels != NULL && fseek(f, (long)offset, SEEK_SET) == 0 && fread(pixels, (size_t)stride, (size_t)rows, f) == (size_t)rows) {
    if (height > 0) { /* rows from the bottom */
      scale(out, pixels + (size_t)stride * (size_t)(rows - 1), width, rows, -stride, bpp);
    } else {
      scale(out, pixels, width, rows, stride, bpp);
    }
    ok = 1;
  } else {
    LOG("%s is cut short", path);
  }
  free(pixels);
  fclose(f);
  return ok;
}

/* ---- the capture thread ------------------------------------------------------- */

static void capture(void) {
  for (;;) {
    char error[200] = "";
    int opened = mkgp2pc_camera_device_open(s_name, error, sizeof(error)) == 0;
    if (opened) {
      for (;;) {
        Run run;
        lock(&s_lock);
        run = s_run;
        unlock(&s_lock);
        if (run != RUNNING) {
          break;
        }
        if (mkgp2pc_camera_device_grab() != 0) {
          LOG("the device stopped giving pictures");
          opened = 0;
          break;
        }
      }
      mkgp2pc_camera_device_close();
    } else {
      LOG("%s", error[0] ? error : "the device cannot be opened");
    }
    lock(&s_lock);
    if (s_run == STOPPING || !opened) {
      s_run = IDLE;
      unlock(&s_lock);
      if (s_trace) {
        LOG("device closed");
      }
      return;
    }
    unlock(&s_lock); /* asked for again while it was closing: open it again */
  }
}

#ifdef _WIN32
static DWORD WINAPI capture_thread(LPVOID unused) {
  (void)unused;
  capture();
  return 0;
}
#else
static void* capture_thread(void* unused) {
  (void)unused;
  capture();
  return NULL;
}
#endif

/* The game wants pictures: the device runs from now on. */
static void wake(uint64_t now) {
  int start = 0;
  lock(&s_lock);
  s_lastUse = now;
  if (s_run == STOPPING) {
    s_run = RUNNING; /* its thread is still there and sees this */
  } else if (s_run == IDLE && (!s_tried || now - s_lastTry >= kRetryMs)) {
    s_run = RUNNING;
    s_tried = 1;
    s_lastTry = now;
    start = 1;
  }
  unlock(&s_lock);
  if (start) {
    int ok;
#ifdef _WIN32
    HANDLE t = CreateThread(NULL, 0, capture_thread, NULL, 0, NULL);
    ok = t != NULL;
    if (ok) {
      CloseHandle(t);
    }
#else
    pthread_t t;
    ok = pthread_create(&t, NULL, capture_thread, NULL) == 0;
    if (ok) {
      pthread_detach(t);
    }
#endif
    if (!ok) {
      lock(&s_lock);
      s_run = IDLE;
      unlock(&s_lock);
    } else if (s_trace) {
      LOG("device opening");
    }
  }
}

void mkgp2pc_camera_tick(void) {
  if (s_state != 1 || s_mode != DEVICE) {
    return;
  }
  lock(&s_lock);
  if (s_run == RUNNING && now_ms() - s_lastUse >= kIdleMs) {
    s_run = STOPPING;
  }
  unlock(&s_lock);
}

/* ---- the answer ---------------------------------------------------------------- */

/* What stands for the JPEG: start of image, a comment naming the form, a
 * baseline frame header for 320x240 in three components (the game's
 * THPDecGetInfo reads width, height and sampling from it), a scan marker,
 * the pixels (red, green, blue; rows from the top), end of image. */
static const char kComment[] = "mkgp2-pc uncompressed";
static const uint8_t kFrameHeader[] = {0xFF, 0xC0, 0x00, 0x11, 0x08, H >> 8, H & 0xFF, W >> 8, W & 0xFF,
                                       0x03, 0x01, 0x11, 0x00, 0x02, 0x11, 0x00, 0x03, 0x11, 0x00};
static const uint8_t kScan[] = {0xFF, 0xDA, 0x00, 0x02};

static void build_reply(const uint8_t* rgb) {
  static const char http[] = "HTTP/1.0 200 OK\r\nContent-Type: image/jpeg\r\n\r\n";
  uint8_t* p = s_reply;
  memcpy(p, http, sizeof(http) - 1), p += sizeof(http) - 1;
  *p++ = 0xFF, *p++ = 0xD8;
  *p++ = 0xFF, *p++ = 0xFE, *p++ = 0, *p++ = (uint8_t)(2 + sizeof(kComment));
  memcpy(p, kComment, sizeof(kComment)), p += sizeof(kComment);
  memcpy(p, kFrameHeader, sizeof(kFrameHeader)), p += sizeof(kFrameHeader);
  memcpy(p, kScan, sizeof(kScan)), p += sizeof(kScan);
  memcpy(p, rgb, kPixels), p += kPixels;
  *p++ = 0xFF, *p++ = 0xD9;
  s_replySize = (uint32_t)(p - s_reply);
}

void mkgp2pc_camera_request(void) {
  configure();
  if (s_mode == OFF) {
    return;
  }
  s_pending = 1;
  s_requestTime = now_ms();
  s_requests++;
  if (s_mode == DEVICE) {
    wake(s_requestTime);
    lock(&s_lock);
    s_requestSerial = s_serial;
    unlock(&s_lock);
  }
  if (s_trace) {
    LOG("request %llu", (unsigned long long)s_requests);
  }
}

int mkgp2pc_camera_reply(const uint8_t** data, uint32_t* size) {
  static uint8_t picture[kPixels];
  static int stillLoaded;
  if (s_state != 1 || s_mode == OFF) {
    return 0;
  }
  if (s_pending) {
    if (s_mode == TEST) {
      test_picture(picture, s_requests);
    } else if (s_mode == STILL) {
      if (!stillLoaded) {
        stillLoaded = 1;
        memset(picture, 0x40, sizeof(picture));
        load_bmp(s_name, picture);
      }
    } else {
      /* a picture taken after the game asked; an older one, or the grey
       * one, when the device gives none in time */
      uint64_t now = now_ms();
      int have;
      wake(now);
      lock(&s_lock);
      have = s_serial != s_requestSerial || s_run == IDLE || now - s_requestTime >= kPatienceMs;
      if (have) {
        memcpy(picture, s_frame, kPixels);
      }
      unlock(&s_lock);
      if (!have) {
        return 0;
      }
    }
    build_reply(picture);
    s_pending = 0;
  }
  if (s_replySize == 0) {
    return 0;
  }
  *data = s_reply;
  *size = s_replySize;
  return 1;
}

/* ---- the game's still-picture decoder ------------------------------------------
 *
 * The four THP library functions on the camera's path
 * (Asset_LoadAndDecompressByType, type 3). The other three output forms
 * (THPDecDecode, fn_8029B54C, fn_8029BA2C) have no caller that runs.
 */
typedef struct {
  uint32_t x0;
  uint16_t width;
  uint16_t height;
  int32_t format;
} THPDecDesc;

typedef struct {
  void* data;
  void* info;
} THPDecHandle;

/* The frame header after the start-of-image marker: its address, or NULL. */
static const uint8_t* frame_header(const uint8_t* data, int* ours) {
  *ours = 0;
  if (data[0] != 0xFF || data[1] != 0xD8) {
    return NULL;
  }
  data += 2;
  for (int guard = 0; guard < 64; guard++) {
    uint8_t marker;
    if (*data++ != 0xFF) {
      return NULL;
    }
    while (*data == 0xFF) {
      data++;
    }
    marker = *data++;
    if (marker == 0xC0) {
      return data;
    }
    if (marker < 0xC0 || marker == 0xFF) {
      return NULL;
    }
    if (marker == 0xFE && memcmp(data + 2, kComment, sizeof(kComment)) == 0) {
      *ours = 1;
    }
    data += (data[0] << 8) | data[1];
  }
  return NULL;
}

int32_t THPDecGetInfo(uint8_t* data, THPDecDesc* out) {
  int ours;
  const uint8_t* f = frame_header(data, &ours);
  memset(out, 0, sizeof(*out));
  if (f == NULL || f[7] != 3) {
    return 0;
  }
  out->height = (uint16_t)(f[3] << 8 | f[4]);
  out->width = (uint16_t)(f[5] << 8 | f[6]);
  out->format = 1; /* all three components at full size */
  return 1;
}

uint32_t THPDecGetWorkSize(THPDecDesc* desc) { return (uint32_t)desc->width * 0x30 + 0x2400; }

int32_t THPVideoDecode(THPDecHandle* handle, uint8_t* statusOut, void* work, void* data, THPDecDesc* desc) {
  int ours;
  const uint8_t* f = frame_header((const uint8_t*)data, &ours);
  if (f == NULL || !ours || desc->width != W || desc->height != H || memcmp(f + 17, kScan, sizeof(kScan)) != 0) {
    static int said;
    if (!said) {
      said = 1;
      LOG("a picture that is not in the port's own form was not decoded");
    }
    *statusOut = 1;
    return 0;
  }
  handle->data = (void*)(f + 17 + sizeof(kScan)); /* the pixels */
  handle->info = work;
  *statusOut = 0;
  return 1;
}

/* The texture memory last written here and how often: the drawing layer
 * (port/gfx/gx_shim.cpp) keeps a copy of each texture and looks at these to
 * see that this one has a new picture. Read from there, not called: an
 * object in port/platform cannot refer to port/gfx in the Windows link. */
const void* mkgp2pc_camera_texture;
uint32_t mkgp2pc_camera_texture_serial;

/* RGBA8 texture: tiles of 4x4 pixels, each 16 times (alpha, red), then 16
 * times (green, blue). */
void fn_8029BCF0(THPDecHandle* handle, uint8_t* dst, uint8_t alpha) {
  const uint8_t* rgb = (const uint8_t*)handle->data;
  uint8_t* texture = dst;
  for (int ty = 0; ty < H; ty += 4) {
    for (int tx = 0; tx < W; tx += 4) {
      for (int y = 0; y < 4; y++) {
        const uint8_t* p = rgb + ((ty + y) * W + tx) * 3;
        for (int x = 0; x < 4; x++, p += 3) {
          dst[(y * 4 + x) * 2] = alpha;
          dst[(y * 4 + x) * 2 + 1] = p[0];
          dst[32 + (y * 4 + x) * 2] = p[1];
          dst[32 + (y * 4 + x) * 2 + 1] = p[2];
        }
      }
      dst += 64;
    }
  }
  mkgp2pc_camera_texture = texture;
  mkgp2pc_camera_texture_serial++;
}

#ifndef _WIN32
int mkgp2pc_camera_device_open(const char* name, char* error, int errorSize) {
  (void)name;
  snprintf(error, (size_t)errorSize, "camera devices are read on Windows only; MKGP2PC_CAMERA=test and file:<path> work here");
  return -1;
}
int mkgp2pc_camera_device_grab(void) { return -1; }
void mkgp2pc_camera_device_close(void) {}
#endif
