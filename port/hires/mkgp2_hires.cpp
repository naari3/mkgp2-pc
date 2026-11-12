// mkgp2-hires: makes the high-resolution 2D texture folder of the PC port
// from the player's own disc image (docs/port/hires_textures.md). The same
// work as make_hires.py, as one program without Python, so that it can be
// handed out next to the game and started from the settings program.
//
//   mkgp2-hires --iso <disc image> --out <folder> [--scale 2|4] [--models] [--jobs N]
//   mkgp2-hires --iso ... --out ... [--ai <model>] [--ai-models <model>] --work <folder> [--tool <folder>]
//               [--download] [--tile N]      (--ai: the sprites, --ai-models: the 3D models' textures)
//   mkgp2-hires --iso ... --export-sprites <folder>
//   mkgp2-hires --iso ... --out ... --import-sprites <folder>
//
// Every texture palette file (*.tpl) on the disc is decoded and written back
// enlarged as PNG, named the way Dolphin names custom textures, which is what
// the rendering library looks for (port/gfx/gx_shim.cpp, MKGP2PC_HIRES=1).
//
// Standard output carries lines for the settings program:
//   stage <name>      what is being done (read, fonts, sprites, models, export, download, enlarge-rgb,
//                     enlarge-alpha, enlarge-models-rgb, enlarge-models-alpha, import, import-models)
//   progress <0-100>  how far the stage is
//   error <text>      why it stopped (exit code 1)
//   done <folder>
//
// The output is derived from the game's data: it stays on the player's
// machine. Nothing of it belongs in the repository.
//
// stb_image.h and stb_image_write.h (public domain, Sean Barrett) read and
// write the PNG files and unpack zlib streams.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STB_IMAGE_STATIC
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "stb_image_write.h"

#ifdef _WIN32
#include <windows.h>
#include <urlmon.h>
#endif

namespace fs = std::filesystem;
typedef std::vector<uint8_t> Bytes;

static std::mutex gPrint;

static void say(const char* fmt, ...) {
  std::lock_guard<std::mutex> lock(gPrint);
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  putchar('\n');
  fflush(stdout);
}

[[noreturn]] static void fail(const char* fmt, ...) {
  char text[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(text, sizeof text, fmt, ap);
  va_end(ap);
  say("error %s", text);
  exit(1);
}

// Paths come in as UTF-8 (the settings program passes them so; a Windows
// console start converts in wmain below).
static fs::path upath(const std::string& s) { return fs::u8path(s); }

static FILE* open_file(const fs::path& p, const char* mode) {
#ifdef _WIN32
  wchar_t wmode[8];
  for (int i = 0; i < 8; i++) {
    wmode[i] = (wchar_t)mode[i];
    if (!mode[i]) break;
  }
  return _wfopen(p.c_str(), wmode);
#else
  return fopen(p.c_str(), mode);
#endif
}

// ------------------------------------------------------------------ hashing

static inline uint64_t rotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
static inline uint64_t le64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
  return v;
}

// XXH64 with seed 0 (the hash of Dolphin's custom texture names).
static uint64_t xxh64(const uint8_t* p, size_t n) {
  const uint64_t P1 = 0x9E3779B185EBCA87ull, P2 = 0xC2B2AE3D27D4EB4Full, P3 = 0x165667B19E3779F9ull,
                 P4 = 0x85EBCA77C2B2AE63ull, P5 = 0x27D4EB2F165667C5ull;
  auto round = [&](uint64_t acc, uint64_t lane) { return rotl64(acc + lane * P2, 31) * P1; };
  const uint8_t* end = p + n;
  uint64_t h;
  if (n >= 32) {
    uint64_t v[4] = {P1 + P2, P2, 0, 0 - P1};
    for (; p + 32 <= end; p += 32)
      for (int i = 0; i < 4; i++) v[i] = round(v[i], le64(p + 8 * i));
    h = rotl64(v[0], 1) + rotl64(v[1], 7) + rotl64(v[2], 12) + rotl64(v[3], 18);
    for (int i = 0; i < 4; i++) h = (h ^ round(0, v[i])) * P1 + P4;
  } else {
    h = P5;
  }
  h += n;
  for (; p + 8 <= end; p += 8) h = rotl64(h ^ round(0, le64(p)), 27) * P1 + P4;
  if (p + 4 <= end) {
    uint64_t v = (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24);
    h = rotl64(h ^ (v * P1), 23) * P2 + P3;
    p += 4;
  }
  for (; p < end; p++) h = rotl64(h ^ (*p * P5), 11) * P1;
  h ^= h >> 33;
  h *= P2;
  h ^= h >> 29;
  h *= P3;
  return h ^ (h >> 32);
}

// --------------------------------------------------------------- disc image

static inline uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static inline uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

// The disc header of Mario Kart Arcade GP 2 (Japan); its main.dol is GNLJ82.
static const char GAME_ID[] = "GGPJ02";
// main.dol of GNLJ82 (config/GNLJ82/symbols.txt)
static const uint32_t ADDR_PATHS = 0x80350508, N_PATHS = 4804;          // kResourcePathTable
static const uint32_t ADDR_RESOURCES = 0x80422208, N_RESOURCES = 11008;  // kResourceTableMain, 0x28 bytes each
static const uint32_t ADDR_GLYPHS = 0x804E5590, N_GLYPHS = 739;          // lbl_804E5590, 16 bytes each

struct Disc {
  FILE* f = nullptr;
  std::mutex lock;
  std::map<std::string, std::pair<uint32_t, uint32_t>> files;  // path -> offset, size (sorted)
  std::vector<std::pair<uint32_t, Bytes>> sections;

  Bytes at(uint32_t off, uint32_t size) {
    std::lock_guard<std::mutex> g(lock);
    Bytes b(size);
    if (fseek(f, (long)off, SEEK_SET) != 0 || fread(b.data(), 1, size, f) != size) fail("the disc image is cut short");
    return b;
  }

  void open(const std::string& path) {
    f = open_file(upath(path), "rb");
    if (!f) fail("cannot open the disc image: %s", path.c_str());
    uint8_t head[0x440];
    if (fread(head, 1, sizeof head, f) != sizeof head || memcmp(head, GAME_ID, 6) != 0)
      fail("not a disc image of Mario Kart Arcade GP 2 (Japan): %s", path.c_str());
    uint32_t dol = be32(head + 0x420), fst = be32(head + 0x424), fstSize = be32(head + 0x428);
    Bytes t = at(fst, fstSize);
    uint32_t n = be32(&t[8]);
    const char* names = (const char*)&t[n * 12];
    std::vector<std::pair<uint32_t, std::string>> dirs;
    for (uint32_t i = 1; i < n; i++) {
      while (!dirs.empty() && i >= dirs.back().first) dirs.pop_back();
      uint32_t word = be32(&t[i * 12]), off = be32(&t[i * 12 + 4]), size = be32(&t[i * 12 + 8]);
      std::string name = names + (word & 0xFFFFFF);
      if (word >> 24) {
        dirs.push_back({size, name});
      } else {
        std::string full;
        for (auto& d : dirs) full += d.second + "/";
        files[full + name] = {off, size};
      }
    }
    Bytes h = at(dol, 0x100);
    for (int i = 0; i < 18; i++) {
      uint32_t o = be32(&h[i * 4]), a = be32(&h[0x48 + i * 4]), s = be32(&h[0x90 + i * 4]);
      if (s) sections.push_back({a, at(dol + o, s)});
    }
  }

  const uint8_t* mem(uint32_t addr, uint32_t size) const {
    for (auto& s : sections)
      if (s.first <= addr && addr + size <= s.first + s.second.size()) return &s.second[addr - s.first];
    fail("address %08x is not in main.dol", addr);
  }
};

static std::string lower(std::string s) {
  for (auto& c : s)
    if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
  return s;
}

struct Glyph {
  int ch = 0;  // the character's code in the game's table
  int cw = 0, chh = 0, cls = 0;
  bool outline = false;
};

// {file name (lower case): character, cell size, size class} for the main
// texture and the outline texture ('_a') of every character of the 16 and
// 24 pixel fonts.
static std::map<std::string, Glyph> glyph_table(const Disc& disc) {
  const uint8_t* paths = disc.mem(ADDR_PATHS, N_PATHS * 4);
  const uint8_t* res = disc.mem(ADDR_RESOURCES, N_RESOURCES * 0x28);
  const uint8_t* raw = disc.mem(ADDR_GLYPHS, N_GLYPHS * 16);
  std::map<std::string, Glyph> out;
  for (uint32_t i = 0; i < N_GLYPHS; i++) {
    int code = be16(raw + i * 16 + 2);
    uint32_t r[2] = {be32(raw + i * 16 + 4), be32(raw + i * 16 + 8)};
    for (int k = 0; k < 2; k++) {
      int cls = k ? 24 : 16;
      if (r[k] >= N_RESOURCES) continue;
      const uint8_t* e = res + r[k] * 0x28;
      uint32_t bx = be32(e + 0x0C), by = be32(e + 0x10);
      float sx, sy;
      memcpy(&sx, &bx, 4);
      memcpy(&sy, &by, 4);
      int pi = (int16_t)be16(e + 0x16);
      if (pi < 0 || (uint32_t)pi >= N_PATHS) continue;
      uint32_t addr = be32(paths + pi * 4);
      std::string name = lower((const char*)disc.mem(addr, 1));
      char tag[32];
      snprintf(tag, sizeof tag, "sysfont_%d_", cls);
      if (name.find(tag) == std::string::npos || name.size() < 5) continue;
      // the half-width entries of the table name the same textures
      Glyph g{code, (int)sx, (int)sy, cls, false};
      out.insert({name, g});
      g.outline = true;
      out.insert({name.substr(0, name.size() - 4) + "_a.tpl", g});
    }
  }
  return out;
}

// ---------------------------------------------------------- texture palettes

struct Entry {
  int w = 0, h = 0, fmt = 0;
  uint32_t off = 0;
  bool hasPal = false;
  int palN = 0, palFmt = 0;
  uint32_t palOff = 0;
};

// A *.tpl of this game is either plain or 8 bytes followed by a zlib stream.
static bool tpl_bytes(Bytes& d) {
  static const uint8_t magic[4] = {0x00, 0x20, 0xAF, 0x30};
  if (d.size() >= 4 && memcmp(d.data(), magic, 4) == 0) return true;
  if (d.size() < 16) return false;
  int len = 0;
  char* p = stbi_zlib_decode_malloc_guesssize((const char*)d.data() + 8, (int)d.size() - 8, (int)d.size() * 4, &len);
  if (!p) return false;
  d.assign((uint8_t*)p, (uint8_t*)p + len);
  free(p);
  return d.size() >= 4 && memcmp(d.data(), magic, 4) == 0;
}

static std::vector<Entry> tpl_entries(const Bytes& d) {
  std::vector<Entry> out;
  if (d.size() < 12) return out;
  uint32_t count = be32(&d[4]), table = be32(&d[8]);
  for (uint32_t i = 0; i < count; i++) {
    if ((uint64_t)table + i * 8 + 8 > d.size()) break;
    uint32_t image = be32(&d[table + i * 8]), clut = be32(&d[table + i * 8 + 4]);
    if (image == 0 || (uint64_t)image + 12 > d.size()) continue;
    Entry e;
    e.h = be16(&d[image]);
    e.w = be16(&d[image + 2]);
    e.fmt = (int)be32(&d[image + 4]);
    e.off = be32(&d[image + 8]);
    if (clut && (uint64_t)clut + 12 <= d.size()) {
      e.hasPal = true;
      e.palN = be16(&d[clut]);
      e.palFmt = (int)be32(&d[clut + 4]);
      e.palOff = be32(&d[clut + 8]);
    }
    out.push_back(e);
  }
  return out;
}

struct Block {
  int bw, bh, bpp;
};
static bool block_of(int fmt, Block& b) {
  switch (fmt) {
    case 0: case 8: case 14: b = {8, 8, 4}; return true;
    case 1: case 2: case 9: b = {8, 4, 8}; return true;
    case 3: case 4: case 5: case 10: b = {4, 4, 16}; return true;
    case 6: b = {4, 4, 32}; return true;
  }
  return false;
}

static size_t tex_size(int w, int h, int fmt) {
  Block b;
  block_of(fmt, b);
  return (size_t)((w + b.bw - 1) / b.bw) * ((h + b.bh - 1) / b.bh) * b.bw * b.bh * b.bpp / 8;
}

static inline void rgb5a3(uint32_t v, uint8_t* o) {
  if (v & 0x8000) {
    o[0] = (uint8_t)(((v >> 10) & 31) * 255 / 31);
    o[1] = (uint8_t)(((v >> 5) & 31) * 255 / 31);
    o[2] = (uint8_t)((v & 31) * 255 / 31);
    o[3] = 255;
  } else {
    o[0] = (uint8_t)(((v >> 8) & 15) * 17);
    o[1] = (uint8_t)(((v >> 4) & 15) * 17);
    o[2] = (uint8_t)((v & 15) * 17);
    o[3] = (uint8_t)(((v >> 12) & 7) * 255 / 7);
  }
}

static inline void rgb565(uint32_t v, uint8_t* o) {
  o[0] = (uint8_t)(((v >> 11) & 31) * 255 / 31);
  o[1] = (uint8_t)(((v >> 5) & 63) * 255 / 63);
  o[2] = (uint8_t)((v & 31) * 255 / 31);
  o[3] = 255;
}

// RGBA8 picture as the console shows the texture.
static std::vector<uint8_t> tex_decode(const Bytes& d, const Entry& e) {
  int w = e.w, h = e.h;
  Block b;
  block_of(e.fmt, b);
  const uint8_t* raw = &d[e.off];
  std::vector<uint8_t> px((size_t)w * h * 4);
  std::vector<uint8_t> pal;
  int palN = 0;
  if (e.fmt == 8 || e.fmt == 9 || e.fmt == 10) {
    palN = e.palN;
    pal.resize((size_t)std::max(palN, 1) * 4);
    for (int i = 0; i < palN; i++) {
      uint32_t v = be16(&d[e.palOff + i * 2]);
      uint8_t* o = &pal[i * 4];
      if (e.palFmt == 0) {
        o[0] = o[1] = o[2] = (uint8_t)(v & 255);
        o[3] = (uint8_t)(v >> 8);
      } else if (e.palFmt == 1) {
        rgb565(v, o);
      } else {
        rgb5a3(v, o);
      }
    }
  }
  auto put = [&](int x, int y, const uint8_t* c) {
    if (x < w && y < h) memcpy(&px[((size_t)y * w + x) * 4], c, 4);
  };
  auto index = [&](int x, int y, int i) {
    if (palN <= 0) return;
    put(x, y, &pal[(size_t)std::min(i, palN - 1) * 4]);
  };
  int cols = (w + b.bw - 1) / b.bw, rows = (h + b.bh - 1) / b.bh;
  const uint8_t* p = raw;
  for (int by = 0; by < rows; by++)
    for (int bx = 0; bx < cols; bx++) {
      int x0 = bx * b.bw, y0 = by * b.bh;
      switch (e.fmt) {
        case 0: case 8:
          for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x += 2) {
              uint8_t v = *p++;
              int n[2] = {v >> 4, v & 15};
              for (int k = 0; k < 2; k++) {
                if (e.fmt == 8) {
                  index(x0 + x + k, y0 + y, n[k]);
                } else {
                  uint8_t c = (uint8_t)(n[k] * 17);
                  uint8_t o[4] = {c, c, c, c};
                  put(x0 + x + k, y0 + y, o);
                }
              }
            }
          break;
        case 1: case 2: case 9:
          for (int y = 0; y < 4; y++)
            for (int x = 0; x < 8; x++) {
              uint8_t v = *p++;
              if (e.fmt == 9) {
                index(x0 + x, y0 + y, v);
              } else if (e.fmt == 1) {
                uint8_t o[4] = {v, v, v, v};
                put(x0 + x, y0 + y, o);
              } else {
                uint8_t i = (uint8_t)((v & 15) * 17), a = (uint8_t)((v >> 4) * 17);
                uint8_t o[4] = {i, i, i, a};
                put(x0 + x, y0 + y, o);
              }
            }
          break;
        case 3: case 4: case 5: case 10:
          for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
              uint32_t v = be16(p);
              p += 2;
              uint8_t o[4];
              if (e.fmt == 3) {
                o[0] = o[1] = o[2] = (uint8_t)(v & 255);
                o[3] = (uint8_t)(v >> 8);
                put(x0 + x, y0 + y, o);
              } else if (e.fmt == 4) {
                rgb565(v, o);
                put(x0 + x, y0 + y, o);
              } else if (e.fmt == 5) {
                rgb5a3(v, o);
                put(x0 + x, y0 + y, o);
              } else {
                index(x0 + x, y0 + y, (int)(v & 0x3FFF));
              }
            }
          break;
        case 6:
          for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
              const uint8_t* ar = p + (y * 4 + x) * 2;
              const uint8_t* gb = p + 32 + (y * 4 + x) * 2;
              uint8_t o[4] = {ar[1], gb[0], gb[1], ar[0]};
              put(x0 + x, y0 + y, o);
            }
          p += 64;
          break;
        case 14:
          for (int sub = 0; sub < 4; sub++) {
            uint32_t c0 = be16(p), c1 = be16(p + 2);
            int col[4][4];
            uint8_t t[4];
            rgb565(c0, t);
            for (int k = 0; k < 4; k++) col[0][k] = t[k];
            rgb565(c1, t);
            for (int k = 0; k < 4; k++) col[1][k] = t[k];
            for (int k = 0; k < 4; k++) {
              if (c0 > c1) {
                col[2][k] = (2 * col[0][k] + col[1][k]) / 3;
                col[3][k] = (2 * col[1][k] + col[0][k]) / 3;
              } else {
                col[2][k] = (col[0][k] + col[1][k]) / 2;
                col[3][k] = 0;
              }
            }
            col[3][3] = c0 > c1 ? 255 : 0;
            int sx = x0 + (sub & 1) * 4, sy = y0 + (sub >> 1) * 4;
            for (int y = 0; y < 4; y++) {
              uint8_t bits = p[4 + y];
              for (int x = 0; x < 4; x++) {
                int i = (bits >> (6 - 2 * x)) & 3;
                uint8_t o[4] = {(uint8_t)col[i][0], (uint8_t)col[i][1], (uint8_t)col[i][2], (uint8_t)col[i][3]};
                put(sx + x, sy + y, o);
              }
            }
            p += 8;
          }
          break;
      }
    }
  return px;
}

// File name stem of the replacement: Dolphin's convention, as the rendering
// library reads it (aurora lib/gfx/texture_replacement.cpp). Empty when the
// texture cannot be named.
static std::string tex_key(const Bytes& d, const Entry& e) {
  size_t size = tex_size(e.w, e.h, e.fmt);
  if ((uint64_t)e.off + size > d.size()) return "";
  const uint8_t* t = &d[e.off];
  char name[128];
  int n = snprintf(name, sizeof name, "tex1_%dx%d_%016llx", e.w, e.h, (unsigned long long)xxh64(t, size));
  if (e.fmt == 8 || e.fmt == 9 || e.fmt == 10) {
    if (!e.hasPal) return "";
    int lo = 0x7FFFFFFF, hi = -1;
    if (e.fmt == 8) {
      for (size_t i = 0; i < size; i++) {
        int a = t[i] >> 4, b = t[i] & 15;
        lo = std::min(lo, std::min(a, b));
        hi = std::max(hi, std::max(a, b));
      }
    } else if (e.fmt == 9) {
      for (size_t i = 0; i < size; i++) {
        lo = std::min(lo, (int)t[i]);
        hi = std::max(hi, (int)t[i]);
      }
    } else {
      for (size_t i = 0; i + 1 < size; i += 2) {
        int a = be16(t + i) & 0x3FFF;
        lo = std::min(lo, a);
        hi = std::max(hi, a);
      }
    }
    if (hi >= e.palN || hi < 0) return "";
    if ((uint64_t)e.palOff + 2 * (hi + 1) > d.size()) return "";
    n += snprintf(name + n, sizeof name - n, "_%016llx",
                  (unsigned long long)xxh64(&d[e.palOff + 2 * lo], (size_t)2 * (hi + 1 - lo)));
  }
  snprintf(name + n, sizeof name - n, "_%d", e.fmt);
  return name;
}

// ------------------------------------------------------------------ filters

struct Plane {
  int w = 0, h = 0;
  std::vector<float> v;
  Plane() {}
  Plane(int w_, int h_, float fill = 0) : w(w_), h(h_), v((size_t)w_ * h_, fill) {}
  float& at(int x, int y) { return v[(size_t)y * w + x]; }
  float at(int x, int y) const { return v[(size_t)y * w + x]; }
};

// Gaussian blur, edges repeated.
static void blur(Plane& a, float radius) {
  int n = std::max(1, (int)std::ceil(radius * 3));
  std::vector<float> k(2 * n + 1);
  float sum = 0;
  for (int i = -n; i <= n; i++) sum += k[i + n] = std::exp(-0.5f * (i / radius) * (i / radius));
  for (auto& x : k) x /= sum;
  Plane t(a.w, a.h);
  for (int y = 0; y < a.h; y++) {  // along y, a row at a time
    float* dst = &t.v[(size_t)y * a.w];
    for (int i = -n; i <= n; i++) {
      const float* src = &a.v[(size_t)std::min(std::max(y + i, 0), a.h - 1) * a.w];
      float w = k[i + n];
      for (int x = 0; x < a.w; x++) dst[x] += w * src[x];
    }
  }
  std::vector<float> row((size_t)a.w + 2 * n);
  for (int y = 0; y < a.h; y++) {  // along x
    for (int x = -n; x < a.w + n; x++) row[x + n] = t.at(std::min(std::max(x, 0), a.w - 1), y);
    for (int x = 0; x < a.w; x++) {
      float acc = 0;
      for (int i = 0; i <= 2 * n; i++) acc += k[i] * row[x + i];
      a.at(x, y) = acc;
    }
  }
}

enum { BOX, BILINEAR, BICUBIC, LANCZOS };

static double filter_at(int kind, double x) {
  x = std::fabs(x);
  switch (kind) {
    case BOX: return x <= 0.5 ? 1.0 : 0.0;  // (-0.5, 0.5]
    case BILINEAR: return x < 1 ? 1 - x : 0;
    case BICUBIC: {
      const double a = -0.5;
      if (x < 1) return ((a + 2) * x - (a + 3)) * x * x + 1;
      if (x < 2) return (((x - 5) * x + 8) * x - 4) * a;
      return 0;
    }
    default: {
      if (x >= 3) return 0;
      if (x == 0) return 1;
      const double pi = 3.14159265358979323846;
      double px = x * pi;
      return std::sin(px) / px * std::sin(px / 3) / (px / 3);
    }
  }
}

// Resampling as the Pillow library does it: one pass per axis, the filter
// widened when reducing, weights normalised, edges cut off.
static void resample_axis(const std::vector<float>& in, int inN, int stride, int lines, int lineStride,
                          std::vector<float>& out, int outN, int kind) {
  static const double support0[] = {0.5, 1.0, 2.0, 3.0};
  double scale = (double)inN / outN, fscale = std::max(scale, 1.0), support = support0[kind] * fscale;
  std::vector<int> first(outN), count(outN);
  int kmax = (int)std::ceil(support) * 2 + 1;
  std::vector<float> weights((size_t)outN * kmax);
  for (int x = 0; x < outN; x++) {
    double center = (x + 0.5) * scale;
    int lo = std::max(0, (int)(center - support + 0.5)), hi = std::min(inN, (int)(center + support + 0.5));
    double sum = 0;
    for (int i = lo; i < hi; i++) sum += filter_at(kind, (i - center + 0.5) / fscale);
    for (int i = lo; i < hi; i++)
      weights[(size_t)x * kmax + (i - lo)] = (float)(filter_at(kind, (i - center + 0.5) / fscale) / (sum != 0 ? sum : 1));
    first[x] = lo;
    count[x] = hi - lo;
  }
  for (int l = 0; l < lines; l++) {
    const float* src = &in[(size_t)l * lineStride];
    float* dst = &out[(size_t)l * (stride == 1 ? outN : lineStride)];
    for (int x = 0; x < outN; x++) {
      const float* wt = &weights[(size_t)x * kmax];
      float acc = 0;
      for (int i = 0; i < count[x]; i++) acc += wt[i] * src[(size_t)(first[x] + i) * stride];
      dst[(size_t)x * stride] = acc;
    }
  }
}

static Plane resize(const Plane& a, int ow, int oh, int kind) {
  Plane t(ow, a.h);
  resample_axis(a.v, a.w, 1, a.h, a.w, t.v, ow, kind);
  Plane o(ow, oh);
  // columns: treat each column as a line with stride ow
  static const double support0[] = {0.5, 1.0, 2.0, 3.0};
  double scale = (double)a.h / oh, fscale = std::max(scale, 1.0), support = support0[kind] * fscale;
  std::vector<float> wt;
  for (int y = 0; y < oh; y++) {
    double center = (y + 0.5) * scale;
    int lo = std::max(0, (int)(center - support + 0.5)), hi = std::min(a.h, (int)(center + support + 0.5));
    wt.assign(hi - lo, 0);
    double sum = 0;
    for (int i = lo; i < hi; i++) sum += filter_at(kind, (i - center + 0.5) / fscale);
    for (int i = lo; i < hi; i++) wt[i - lo] = (float)(filter_at(kind, (i - center + 0.5) / fscale) / (sum != 0 ? sum : 1));
    float* dst = &o.v[(size_t)y * ow];
    for (int x = 0; x < ow; x++) dst[x] = 0;
    for (int i = lo; i < hi; i++) {
      const float* src = &t.v[(size_t)i * ow];
      float k = wt[i - lo];
      for (int x = 0; x < ow; x++) dst[x] += k * src[x];
    }
  }
  return o;
}

// Minimum and maximum over each texel's 3x3 neighbourhood.
static void local_range(const Plane& a, Plane& lo, Plane& hi) {
  lo = Plane(a.w, a.h);
  hi = Plane(a.w, a.h);
  for (int y = 0; y < a.h; y++)
    for (int x = 0; x < a.w; x++) {
      float mn = 1e30f, mx = -1e30f;
      for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
          float v = a.at(std::min(std::max(x + dx, 0), a.w - 1), std::min(std::max(y + dy, 0), a.h - 1));
          mn = std::min(mn, v);
          mx = std::max(mx, v);
        }
      lo.at(x, y) = mn;
      hi.at(x, y) = mx;
    }
}

static Plane halve(const Plane& a) {
  Plane o(a.w / 2, a.h / 2);
  for (int y = 0; y < o.h; y++)
    for (int x = 0; x < o.w; x++)
      o.at(x, y) = (a.at(2 * x, 2 * y) + a.at(2 * x + 1, 2 * y) + a.at(2 * x, 2 * y + 1) + a.at(2 * x + 1, 2 * y + 1)) * 0.25f;
  return o;
}

struct Picture {  // four planes, 0..255
  Plane c[4];
  int w() const { return c[0].w; }
  int h() const { return c[0].h; }
};

static Picture to_picture(const std::vector<uint8_t>& px, int w, int h) {
  Picture p;
  for (int k = 0; k < 4; k++) {
    p.c[k] = Plane(w, h);
    for (size_t i = 0; i < (size_t)w * h; i++) p.c[k].v[i] = px[i * 4 + k];
  }
  return p;
}

// Texels that are fully transparent and black carry no colour. Give them the
// colour of what is near, so that enlarging does not draw a dark rim into
// the edge of every shape.
static void fill_unknown(Picture& p) {
  size_t n = p.c[0].v.size();
  std::vector<uint8_t> todo(n);
  size_t count = 0;
  for (size_t i = 0; i < n; i++) {
    todo[i] = p.c[3].v[i] == 0 && p.c[0].v[i] == 0 && p.c[1].v[i] == 0 && p.c[2].v[i] == 0;
    count += todo[i];
  }
  if (count == 0 || count == n) return;
  Plane known(p.w(), p.h());
  for (size_t i = 0; i < n; i++) known.v[i] = todo[i] ? 0.f : 1.f;
  Plane src[3] = {p.c[0], p.c[1], p.c[2]};
  for (int k = 0; k < 3; k++)
    for (size_t i = 0; i < n; i++) src[k].v[i] *= known.v[i];
  static const float radii[] = {1, 2, 4, 8, 16};
  for (float radius : radii) {
    Plane den = known;
    blur(den, radius);
    bool any = false;
    for (size_t i = 0; i < n && !any; i++) any = todo[i] && den.v[i] > 0.02f;
    if (any) {
      for (int k = 0; k < 3; k++) {
        Plane num = src[k];
        blur(num, radius);
        for (size_t i = 0; i < n; i++)
          if (todo[i] && den.v[i] > 0.02f) p.c[k].v[i] = num.v[i] / std::max(den.v[i], 1e-6f);
      }
      count = 0;
      for (size_t i = 0; i < n; i++) {
        if (todo[i] && den.v[i] > 0.02f) todo[i] = 0;
        count += todo[i];
      }
    }
    if (count == 0) break;
  }
}

static const int WORK_SCALE = 4;  // edges are found at four times, whatever size is written

// Enlargement of one plane that keeps the picture's own shapes and makes its
// edges crisp. Bicubic interpolation at four times, kept between the
// smallest and largest value of the source's 3x3 neighbourhood (no ringing,
// no new colours); then, where that neighbourhood spans an edge (more than
// `low` levels, fully from `high`), the value is pushed toward the nearer
// side, `rounds` times. A gradient stays a gradient. For twice the size the
// result is averaged down, which keeps the edges where they were found.
static Plane enlarge_plane(const Plane& a, int scale, int rounds, float low, float high) {
  float mn = a.v[0], mx = a.v[0];
  for (float v : a.v) {
    mn = std::min(mn, v);
    mx = std::max(mx, v);
  }
  if (mn == mx) return Plane(a.w * scale, a.h * scale, mn);
  int f = WORK_SCALE, W = a.w * f, H = a.h * f;
  Plane up = resize(a, W, H, BICUBIC);
  blur(up, f * 0.2f);
  Plane lo, hi;
  {
    Plane l0, h0;
    local_range(a, l0, h0);
    lo = resize(l0, W, H, BILINEAR);
    hi = resize(h0, W, H, BILINEAR);
  }
  blur(lo, f * 0.5f);
  blur(hi, f * 0.5f);
  for (size_t i = 0; i < up.v.size(); i++) {
    float l = lo.v[i], h = hi.v[i], u = std::min(std::max(up.v[i], l), h);
    if (rounds > 0) {
      float span = h - l, t = (u - l) / std::max(span, 1e-3f), s = t;
      for (int r = 0; r < rounds; r++) s = s * s * (3 - 2 * s);
      float k = std::min(std::max((span - low) / (high - low), 0.f), 1.f);
      u = l + span * (t + (s - t) * k);
    }
    up.v[i] = u;
  }
  lo = Plane();
  hi = Plane();
  while (f > scale) {
    up = halve(up);
    f /= 2;
  }
  return up;
}

static Picture enlarge_sprite(const std::vector<uint8_t>& px, int w, int h, int scale, int fmt) {
  // Block-compressed and 16-bit colour textures are mostly rendered
  // pictures: tightening edges hard there brings the compression blocks out.
  int rounds = 2;
  float low = 12, high = 40;
  if (fmt == 14) rounds = 1, low = 32, high = 96;
  if (fmt == 4) rounds = 1, low = 24, high = 64;
  Picture src = to_picture(px, w, h);
  fill_unknown(src);
  bool grey = src.c[0].v == src.c[1].v && src.c[0].v == src.c[2].v;
  Picture out;
  for (int k = 0; k < 4; k++) {
    if (grey && (k == 1 || k == 2)) {
      out.c[k] = out.c[0];
    } else {
      out.c[k] = enlarge_plane(src.c[k], scale, rounds, low, high);
    }
    src.c[k] = Plane();
  }
  return out;
}

// -------------------------------------------------------------------- fonts

static const int FONT_SCALE = 4;  // the characters are small: always four times, with mip levels
static const float MAIN_GAIN = 3.0f;     // the character itself: a crisp edge
static const float OUTLINE_GAIN = 1.6f;  // the outline texture keeps most of its soft fall-off
static const float LIKENESS = 0.80f;

struct Box {
  bool ok = false;
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// Ink box of a coverage picture (0..1) with sub-texel edges, of the cell
// (cw x ch) at the picture's top left.
static Box ink_box(const Plane& cov, int cw, int ch) {
  cw = std::min(cw, cov.w);
  ch = std::min(ch, cov.h);
  std::vector<float> cols(cw, 0), rows(ch, 0);
  for (int y = 0; y < ch; y++)
    for (int x = 0; x < cw; x++) {
      cols[x] = std::max(cols[x], cov.at(x, y));
      rows[y] = std::max(rows[y], cov.at(x, y));
    }
  const float level = 0.5f;
  auto edges = [&](const std::vector<float>& v, float& lo, float& hi) {
    int a = -1, b = -1, n = (int)v.size();
    for (int i = 0; i < n; i++)
      if (v[i] >= level) {
        if (a < 0) a = i;
        b = i;
      }
    if (a < 0) return false;
    // where the profile crosses the level, between texel centres
    lo = a + 0.5f - ((a > 0 && v[a - 1] < level) ? (v[a] - level) / std::max(v[a] - v[a - 1], 1e-3f) : 0.5f);
    hi = b + 0.5f + ((b + 1 < n && v[b + 1] < level) ? (v[b] - level) / std::max(v[b] - v[b + 1], 1e-3f) : 0.5f);
    return true;
  };
  Box box;
  box.ok = edges(cols, box.x0, box.x1) && edges(rows, box.y0, box.y1);
  return box;
}

// A character's shape (coverage 0..1) enlarged: smooth interpolation, then
// the soft edge narrowed around the half-coverage line, which is where the
// eye puts the outline of the original bitmap. The shape stays the game's
// own; gain 1 would be plain interpolation.
static Plane enlarge_coverage(const Plane& cov, int scale, float gain) {
  Plane up = resize(cov, cov.w * scale, cov.h * scale, BICUBIC);
  blur(up, scale * 0.25f);
  for (auto& v : up.v) v = std::min(std::max((v - 0.5f) * gain + 0.5f, 0.f), 1.f);
  return up;
}

// The same character from the larger font, already enlarged FONT_SCALE times
// (src), brought to where and how large the small font's character is: ink
// box onto ink box, one scale for both directions. Bicubic, nothing outside.
static Plane from_larger(const Plane& src, const Box& sb, const Box& db, int dw, int dh) {
  float k = ((db.x1 - db.x0) / (sb.x1 - sb.x0) + (db.y1 - db.y0) / (sb.y1 - sb.y0)) / 2;
  float scx = (sb.x0 + sb.x1) / 2, scy = (sb.y0 + sb.y1) / 2, dcx = (db.x0 + db.x1) / 2, dcy = (db.y0 + db.y1) / 2;
  int f = FONT_SCALE;
  double ax = 1 / k, cx = (scx - dcx / k) * f, cy = (scy - dcy / k) * f;
  Plane out(dw * f, dh * f);
  auto cubic = [](double d, double* w) {
    const double a = -0.5;
    for (int i = 0; i < 4; i++) {
      double x = std::fabs(d - (i - 1));
      w[i] = x < 1 ? ((a + 2) * x - (a + 3)) * x * x + 1 : x < 2 ? (((x - 5) * x + 8) * x - 4) * a : 0;
    }
  };
  for (int y = 0; y < out.h; y++)
    for (int x = 0; x < out.w; x++) {
      double xin = ax * (x + 0.5) + cx - 0.5, yin = ax * (y + 0.5) + cy - 0.5;
      if (xin < 0 || yin < 0 || xin >= src.w || yin >= src.h) continue;
      int x0 = (int)std::floor(xin), y0 = (int)std::floor(yin);
      double wx[4], wy[4];
      cubic(xin - x0, wx);
      cubic(yin - y0, wy);
      double acc = 0;
      for (int j = 0; j < 4; j++) {
        int yy = std::min(std::max(y0 - 1 + j, 0), src.h - 1);
        double row = 0;
        for (int i = 0; i < 4; i++) row += wx[i] * src.at(std::min(std::max(x0 - 1 + i, 0), src.w - 1), yy);
        acc += wy[j] * row;
      }
      out.at(x, y) = (float)std::min(std::max(acc, 0.0), 1.0);
    }
  return out;
}

// Overlap of the candidate (reduced FONT_SCALE times) with the original
// bitmap, 1 = the same shape.
static float likeness(const Plane& cand, const Plane& cov) {
  int f = FONT_SCALE;
  double mins = 0, sa = 0, sb = 0;
  for (int y = 0; y < cov.h; y++)
    for (int x = 0; x < cov.w; x++) {
      float a = 0;
      for (int j = 0; j < f; j++)
        for (int i = 0; i < f; i++) a += cand.at(x * f + i, y * f + j);
      a /= f * f;
      float b = std::min(std::max((cov.at(x, y) - 0.5f) * MAIN_GAIN + 0.5f, 0.f), 1.f);
      mins += std::min(a, b);
      sa += a;
      sb += b;
    }
  return (float)(2 * mins / std::max(sa + sb, 1e-6));
}

// --------------------------------------------------------------------- work

static void write_png(const fs::path& path, const Picture& p, int channels = 4) {
  int w = p.w(), h = p.h();
  std::vector<uint8_t> px((size_t)w * h * channels);
  for (int k = 0; k < channels; k++)
    for (size_t i = 0; i < (size_t)w * h; i++)
      px[i * channels + k] = (uint8_t)std::min(std::max(std::nearbyint(p.c[k].v[i]), 0.f), 255.f);
  int len = 0;
  unsigned char* png = stbi_write_png_to_mem(px.data(), w * channels, w, h, channels, &len);
  if (!png) fail("cannot encode %s", path.u8string().c_str());
  FILE* f = open_file(path, "wb");
  if (!f || fwrite(png, 1, len, f) != (size_t)len) fail("cannot write %s (disk full?)", path.u8string().c_str());
  fclose(f);
  STBIW_FREE(png);
}

static void write_with_mips(const fs::path& dir, const std::string& stem, Picture& p, int levels) {
  write_png(dir / (stem + ".png"), p);
  for (int level = 1; level <= levels; level++) {
    for (int k = 0; k < 4; k++) p.c[k] = halve(p.c[k]);
    write_png(dir / (stem + "_mip" + std::to_string(level) + ".png"), p);
  }
}

struct Tex {
  std::shared_ptr<Bytes> data;
  Entry e;
  std::string stem;
  // textures of 3D models (from the *.dat files): how they repeat along each
  // axis (GX: 0 clamp, 1 repeat, 2 mirror) and whether they carry mip levels
  bool model = false, mipped = false;
  int wrapS = 0, wrapT = 0;
};

struct Options {
  std::string iso, out, exportDir, importDir, only, ai, aiModels, work, tool;
  int scale = 2, jobs = 0, tile = 0;
  bool download = false, models = false;
};

static const int MODEL_PAD = 4;     // texels of surrounding added to a model texture before enlarging
static const int MODEL_PAD_AI = 8;  // the same for an outside enlarger, which looks further

static bool read_png_plane(const fs::path& path, int w, int h, int scale, Plane out[3], int channels) {
  FILE* f = open_file(path, "rb");
  if (!f) return false;
  int iw = 0, ih = 0, n = 0;
  unsigned char* px = stbi_load_from_file(f, &iw, &ih, &n, 3);
  fclose(f);
  if (!px) return false;
  bool ok = iw >= w && (int64_t)iw * h == (int64_t)ih * w;
  if (!ok) say("note %s: %dx%d does not fit a %dx%d texture, ignored", path.u8string().c_str(), iw, ih, w, h);
  if (ok) {
    for (int k = 0; k < channels; k++) {
      Plane p(iw, ih);
      for (size_t i = 0; i < (size_t)iw * ih; i++) p.v[i] = px[i * 3 + k];
      int ow = w * scale, oh = h * scale;
      out[k] = (iw == ow) ? p : resize(p, ow, oh, (iw % ow == 0) ? BOX : LANCZOS);
    }
  }
  stbi_image_free(px);
  return ok;
}

// The picture an outside enlarger made of an exported texture, brought to
// scale times the texture's size; false when there is none (the built-in
// filter is used then).
static bool import_sprite(const fs::path& dir, const Tex& t, const std::vector<uint8_t>& px, int scale, Picture& out) {
  int w = t.e.w, h = t.e.h;
  // a model texture was exported with its surroundings (see enlarge_model)
  int P = t.model ? MODEL_PAD_AI : 0, W = w + 2 * P, H = h + 2 * P;
  const char* rgbDir = t.model ? "models-rgb" : "rgb";
  const char* alphaDir = t.model ? "models-alpha" : "alpha";
  Plane rgb[3];
  if (!read_png_plane(dir / rgbDir / (t.stem + ".png"), W, H, scale, rgb, 3)) return false;
  auto cut = [&](const Plane& a) {
    if (P == 0) return a;
    Plane c(w * scale, h * scale);
    for (int y = 0; y < c.h; y++)
      memcpy(&c.v[(size_t)y * c.w], &a.v[(size_t)(y + P * scale) * a.w + P * scale], sizeof(float) * c.w);
    return c;
  };
  uint8_t a0 = px[3];
  bool flat = true;
  for (size_t i = 0; i < (size_t)w * h && flat; i++) flat = px[i * 4 + 3] == a0;
  if (flat) {
    out.c[3] = Plane(w * scale, h * scale, a0);
  } else {
    Plane a[3];
    if (!read_png_plane(dir / alphaDir / (t.stem + ".png"), W, H, scale, a, 1)) return false;
    out.c[3] = cut(a[0]);
  }
  for (int k = 0; k < 3; k++) out.c[k] = cut(rgb[k]);
  return true;
}

static inline int wrap_index(int i, int n, int mode) {
  if (mode == 1) return ((i % n) + n) % n;
  if (mode == 2) {
    int m = ((i % (2 * n)) + 2 * n) % (2 * n);
    return m < n ? m : 2 * n - 1 - m;
  }
  return std::min(std::max(i, 0), n - 1);
}

// A model texture is drawn repeating or mirrored, and the game filters
// across its edge into the other side. The enlargement has to see the same
// neighbours or a seam shows where two copies meet: the picture is extended
// by what the wrap mode puts there, enlarged, and cut back.
static std::vector<uint8_t> with_surroundings(const Tex& t, const std::vector<uint8_t>& px, int P) {
  int w = t.e.w, h = t.e.h, W = w + 2 * P, H = h + 2 * P;
  std::vector<uint8_t> padded((size_t)W * H * 4);
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++)
      memcpy(&padded[((size_t)y * W + x) * 4],
             &px[((size_t)wrap_index(y - P, h, t.wrapT) * w + wrap_index(x - P, w, t.wrapS)) * 4], 4);
  return padded;
}

static Picture enlarge_model(const Tex& t, const std::vector<uint8_t>& px, int scale) {
  int w = t.e.w, h = t.e.h, P = MODEL_PAD, W = w + 2 * P, H = h + 2 * P;
  std::vector<uint8_t> padded = with_surroundings(t, px, P);
  Picture big = enlarge_sprite(padded, W, H, scale, t.e.fmt);
  Picture out;
  for (int k = 0; k < 4; k++) {
    out.c[k] = Plane(w * scale, h * scale);
    for (int y = 0; y < h * scale; y++)
      memcpy(&out.c[k].v[(size_t)y * w * scale], &big.c[k].v[(size_t)(y + P * scale) * W * scale + P * scale],
             sizeof(float) * w * scale);
  }
  return out;
}

static void do_sprite(const Tex& t, const Options& o, const fs::path& outDir, int scale, bool allowImport) {
  std::vector<uint8_t> px = tex_decode(*t.data, t.e);
  int w = t.e.w, h = t.e.h;
  if (!o.exportDir.empty() && allowImport) {
    // for an outside enlarger: the colour as an opaque picture, and the
    // alpha (when it is not the same everywhere) as a grey one
    int P = t.model ? MODEL_PAD_AI : 0;
    Picture p = P ? to_picture(with_surroundings(t, px, P), w + 2 * P, h + 2 * P) : to_picture(px, w, h);
    fill_unknown(p);
    write_png(upath(o.exportDir) / (t.model ? "models-rgb" : "rgb") / (t.stem + ".png"), p, 3);
    bool flat = true;
    for (size_t i = 0; i < (size_t)w * h && flat; i++) flat = px[i * 4 + 3] == px[3];
    if (!flat) {
      Picture a;
      a.c[0] = a.c[1] = a.c[2] = p.c[3];
      write_png(upath(o.exportDir) / (t.model ? "models-alpha" : "alpha") / (t.stem + ".png"), a, 3);
    }
    return;
  }
  Picture big;
  if (!(allowImport && !o.importDir.empty() && import_sprite(upath(o.importDir), t, px, scale, big)))
    big = t.model ? enlarge_model(t, px, scale) : enlarge_sprite(px, w, h, scale, t.e.fmt);
  int levels = scale >= 4 ? 1 : 0;
  if (t.model) {
    // A model texture gets mip levels only where the original has them
    // (then all the way down). Given levels where the original has none,
    // the game drew it from a smaller level and it came out softer than the
    // original (seen on the start banner of Yoshi Park, Linux, 2026-11-12).
    levels = 0;
    if (t.mipped)
      for (int m = std::min(w, h) * scale; m > 4; m /= 2) levels++;
  }
  write_with_mips(t.model ? outDir / "models" : outDir, t.stem, big, levels);
}

struct GlyphJob {
  Glyph g;
  Tex main;
  bool hasOutline = false;
  Tex outline;
  bool hasBig = false;
  int bcw = 0, bch = 0;
  Tex bigMain;
  bool bigHasOutline = false;
  Tex bigOutline;
};

static void planes_of(const Tex& t, Plane& intensity, Plane& alpha) {
  std::vector<uint8_t> px = tex_decode(*t.data, t.e);
  intensity = Plane(t.e.w, t.e.h);
  alpha = Plane(t.e.w, t.e.h);
  for (size_t i = 0; i < intensity.v.size(); i++) {
    intensity.v[i] = px[i * 4] / 255.f;
    alpha.v[i] = px[i * 4 + 3] / 255.f;
  }
}

// One character of a text font: its main texture (intensity = the shape,
// alpha opaque) and its outline texture (the grown shape, in intensity and
// alpha). Returns whether it came from the larger font.
//
// A character of the 16 pixel font is taken from the 24 pixel font's bitmap
// of the same character when that, reduced to 16, is the same drawing (kana
// and kanji are; the Latin letters and digits of the two fonts are different
// designs). Otherwise its own bitmap is enlarged.
static bool do_glyph(const GlyphJob& j, const fs::path& outDir) {
  Plane cov, unused;
  planes_of(j.main, cov, unused);
  int w = cov.w, h = cov.h;
  Plane drawn, oi, oa;
  bool larger = false;
  if (j.hasBig) {
    Plane bcov;
    planes_of(j.bigMain, bcov, unused);
    Box db = ink_box(cov, j.g.cw, j.g.chh), sb = ink_box(bcov, j.bcw, j.bch);
    bool same = false;
    if (db.ok && sb.ok) {
      // the two fonts' Latin letters and digits are different designs: the
      // 16 pixel ones are narrower (ink box 0.66 to 0.79 of the 24 pixel
      // one's proportions), kana and kanji 0.94 to 1.06
      float shape = ((db.x1 - db.x0) * (sb.y1 - sb.y0)) / ((db.y1 - db.y0) * (sb.x1 - sb.x0));
      same = shape >= 0.88f && shape <= 1.14f;
    }
    if (same) {
      Plane cand = from_larger(enlarge_coverage(bcov, FONT_SCALE, MAIN_GAIN), sb, db, w, h);
      if (likeness(cand, cov) >= LIKENESS) {
        drawn = cand;
        larger = true;
        if (j.hasOutline && j.bigHasOutline) {
          Plane bi, ba;
          planes_of(j.bigOutline, bi, ba);
          oi = from_larger(enlarge_coverage(bi, FONT_SCALE, OUTLINE_GAIN), sb, db, w, h);
          oa = from_larger(enlarge_coverage(ba, FONT_SCALE, OUTLINE_GAIN), sb, db, w, h);
        }
      }
    }
  }
  if (drawn.v.empty()) drawn = enlarge_coverage(cov, FONT_SCALE, MAIN_GAIN);
  Picture p;
  for (auto& v : drawn.v) v *= 255;
  p.c[0] = p.c[1] = p.c[2] = drawn;
  p.c[3] = Plane(drawn.w, drawn.h, 255);
  write_with_mips(outDir, j.main.stem, p, 2);
  if (j.hasOutline) {
    if (oi.v.empty()) {
      Plane i0, a0;
      planes_of(j.outline, i0, a0);
      oi = enlarge_coverage(i0, FONT_SCALE, OUTLINE_GAIN);
      oa = enlarge_coverage(a0, FONT_SCALE, OUTLINE_GAIN);
    }
    for (auto& v : oi.v) v *= 255;
    for (auto& v : oa.v) v *= 255;
    Picture q;
    q.c[0] = q.c[1] = q.c[2] = oi;
    q.c[3] = oa;
    write_with_mips(outDir, j.outline.stem, q, 2);
  }
  return larger;
}

// Runs fn(i) for i in [0, n) on several threads. `weight` is the job's size
// in texels; no more than `budget` texels are in work at a time (one job
// always may run), which bounds the memory of a 32-bit process.
template <typename F, typename W>
static void run_jobs(size_t n, int threads, uint64_t budget, W weight, F fn, const char* stage) {
  say("stage %s", stage);
  uint64_t total = 0;
  for (size_t i = 0; i < n; i++) total += weight(i);
  std::mutex m;
  std::condition_variable cv;
  size_t next = 0;
  uint64_t inWork = 0, done = 0;
  int lastPercent = -1;
  auto worker = [&]() {
    for (;;) {
      size_t i;
      uint64_t wgt;
      {
        std::unique_lock<std::mutex> lock(m);
        cv.wait(lock, [&] { return next >= n || inWork == 0 || inWork + weight(next) <= budget; });
        if (next >= n) return;
        i = next++;
        wgt = weight(i);
        inWork += wgt;
      }
      fn(i);
      {
        std::lock_guard<std::mutex> lock(m);
        inWork -= wgt;
        done += wgt;
        int percent = (int)(100 * done / std::max<uint64_t>(total, 1));
        if (percent != lastPercent) {
          lastPercent = percent;
          say("progress %d", percent);
        }
      }
      cv.notify_all();
    }
  };
  std::vector<std::thread> pool;
  for (int t = 0; t < threads; t++) pool.emplace_back(worker);
  for (auto& t : pool) t.join();
}

static bool dat_bytes(Bytes& d);
static void dat_textures(const Bytes& d, std::vector<Tex>& out, std::set<std::string>& seen, int& skipped);

struct Work {
  std::vector<Tex> sprites;
  std::vector<Tex> models;         // textures of the 3D models, written to <out>/models
  std::vector<Tex> strayOutlines;  // enlarged like sprites, at the font scale
  std::vector<GlyphJob> glyphs;
  int skipped = 0;
};

static Work collect(Disc& disc, const Options& o) {
  say("stage read");
  std::map<std::string, Glyph> glyphOf = glyph_table(disc);
  Work wk;
  std::set<std::string> seen;
  struct Found {
    Glyph g;
    Tex t;
  };
  std::map<std::string, Found> glyphMain, glyphOutline;
  std::string only = lower(o.only);
  size_t count = 0, total = disc.files.size();
  int lastPercent = -1;
  for (auto& f : disc.files) {
    int percent = (int)(100 * ++count / total);
    if (percent != lastPercent) say("progress %d", lastPercent = percent);
    std::string base = lower(f.first.substr(f.first.rfind('/') + 1));
    if (!only.empty() && base.find(only) == std::string::npos) continue;
    if (o.models && base.size() >= 4 && base.compare(base.size() - 4, 4, ".dat") == 0) {
      Bytes d = disc.at(f.second.first, f.second.second);
      if (dat_bytes(d)) {
        dat_textures(d, wk.models, seen, wk.skipped);
      } else {
        wk.skipped++;
      }
      continue;
    }
    if (base.size() < 4 || base.compare(base.size() - 4, 4, ".tpl") != 0) continue;
    auto data = std::make_shared<Bytes>(disc.at(f.second.first, f.second.second));
    if (!tpl_bytes(*data)) {
      wk.skipped++;
      continue;
    }
    for (const Entry& e : tpl_entries(*data)) {
      Block b;
      if (!block_of(e.fmt, b) || e.w == 0 || e.h == 0) {
        wk.skipped++;
        continue;
      }
      std::string stem = tex_key(*data, e);
      if (stem.empty()) {
        wk.skipped++;
        continue;
      }
      if (e.hasPal && (uint64_t)e.palOff + 2 * (uint64_t)e.palN > data->size()) {
        wk.skipped++;
        continue;
      }
      if (!seen.insert(stem).second) continue;
      Tex t{data, e, stem};
      auto g = glyphOf.find(base);
      if (g != glyphOf.end()) {
        std::string key = g->second.outline ? base.substr(0, base.size() - 6) + ".tpl" : base;
        (g->second.outline ? glyphOutline : glyphMain)[key] = Found{g->second, t};
      } else {
        wk.sprites.push_back(t);
      }
    }
  }
  // the 24 pixel font's textures of each character, for the 16 pixel font
  std::map<int, const Found*> bigMain;
  std::map<int, const Found*> bigOutline;
  for (auto& m : glyphMain)
    if (m.second.g.cls == 24) {
      bigMain[m.second.g.ch] = &m.second;
      auto o2 = glyphOutline.find(m.first);
      bigOutline[m.second.g.ch] = o2 != glyphOutline.end() ? &o2->second : nullptr;
    }
  std::set<std::string> usedOutlines;
  for (auto& m : glyphMain) {
    GlyphJob j;
    j.g = m.second.g;
    j.main = m.second.t;
    auto o2 = glyphOutline.find(m.first);
    if (o2 != glyphOutline.end()) {
      j.hasOutline = true;
      j.outline = o2->second.t;
      usedOutlines.insert(m.first);
    }
    if (j.g.cls == 16) {
      auto b = bigMain.find(j.g.ch);
      if (b != bigMain.end()) {
        j.hasBig = true;
        j.bcw = b->second->g.cw;
        j.bch = b->second->g.chh;
        j.bigMain = b->second->t;
        if (bigOutline[j.g.ch]) {
          j.bigHasOutline = true;
          j.bigOutline = bigOutline[j.g.ch]->t;
        }
      }
    }
    wk.glyphs.push_back(j);
  }
  // an outline texture whose main texture is the same picture as another
  // character's (dropped above as a duplicate) is enlarged like a sprite
  for (auto& m : glyphOutline)
    if (!usedOutlines.count(m.first)) wk.strayOutlines.push_back(m.second.t);
  // A picture that is both in a sprite file and in a model is made once, as
  // whichever was read first; the folder it lands in does not matter to the game.
  std::stable_sort(wk.sprites.begin(), wk.sprites.end(),
                   [](const Tex& a, const Tex& b) { return a.e.w * a.e.h > b.e.w * b.e.h; });
  std::stable_sort(wk.models.begin(), wk.models.end(),
                   [](const Tex& a, const Tex& b) { return a.e.w * a.e.h > b.e.w * b.e.h; });
  return wk;
}

// ------------------------------------------------------------ model files
//
// The textures of the 3D models are inside the model archives (*.dat, the
// HSD library's format: a 0x20 byte header, the data, then a table of the
// offsets in the data that hold pointers). A texture is an image record
// {pointer to the texels, u16 width, u16 height, u32 format, u32 mip flag,
// ...} that a texture object points at from its offset 0x4C; the texture
// object has its wrap modes at 0x34 and 0x38. They are found by looking at
// every pointer in the table, without walking the model tree.

static bool dat_bytes(Bytes& d) {
  if (d.size() >= 0x20 && be32(&d[0]) == d.size()) return true;
  if (d.size() < 16) return false;
  int len = 0;
  char* p = stbi_zlib_decode_malloc_guesssize((const char*)d.data() + 8, (int)d.size() - 8, (int)d.size() * 4, &len);
  if (!p) return false;
  d.assign((uint8_t*)p, (uint8_t*)p + len);
  free(p);
  return d.size() >= 0x20 && be32(&d[0]) == d.size();
}

static void dat_textures(const Bytes& d, std::vector<Tex>& out, std::set<std::string>& seen, int& skipped) {
  uint32_t ds = be32(&d[4]), nrel = be32(&d[8]);
  if ((uint64_t)0x20 + ds + (uint64_t)nrel * 4 > d.size()) return;
  auto data = std::make_shared<Bytes>(d.begin() + 0x20, d.begin() + 0x20 + ds);
  const Bytes& b = *data;
  std::map<uint32_t, uint32_t> target;  // where a pointer is -> where it points
  for (uint32_t i = 0; i < nrel; i++) {
    uint32_t at = be32(&d[0x20 + ds + i * 4]);
    if ((uint64_t)at + 4 <= ds) target[at] = be32(&b[at]);
  }
  std::map<uint32_t, Tex> found;  // by the image record's offset
  for (auto& t : target) {
    uint32_t at = t.first, texels = t.second;
    if ((uint64_t)at + 0x18 > ds || target.count(at + 4) || target.count(at + 8)) continue;
    Entry e;
    e.w = be16(&b[at + 4]);
    e.h = be16(&b[at + 6]);
    e.fmt = (int)be32(&b[at + 8]);
    e.off = texels;
    uint32_t mip = be32(&b[at + 12]);
    Block blk;
    if (e.w < 1 || e.h < 1 || e.w > 1024 || e.h > 1024 || mip > 1 || !block_of(e.fmt, blk)) continue;
    if (e.fmt == 8 || e.fmt == 9 || e.fmt == 10) continue;  // none on this disc; they would need the palette record
    if ((uint64_t)texels + tex_size(e.w, e.h, e.fmt) > ds) continue;
    Tex tex;
    tex.data = data;
    tex.e = e;
    tex.model = true;
    tex.mipped = mip != 0;
    found[at] = tex;
  }
  std::set<uint32_t> used;
  for (auto& t : target) {
    auto f = found.find(t.second);
    if (f == found.end()) continue;
    used.insert(t.second);
    if (t.first >= 0x4C) {  // the texture object that points here
      uint32_t ws = be32(&b[t.first - 0x4C + 0x34]), wt = be32(&b[t.first - 0x4C + 0x38]);
      // drawn both ways somewhere: repeating wins, a clamped use shows less of the edge
      auto merge = [](int have, uint32_t now) { return now == 1 || have == 1 ? 1 : now == 2 || have == 2 ? 2 : 0; };
      if (ws <= 2) f->second.wrapS = merge(f->second.wrapS, ws);
      if (wt <= 2) f->second.wrapT = merge(f->second.wrapT, wt);
    }
  }
  for (uint32_t at : used) {
    Tex& tex = found[at];
    tex.stem = tex_key(*data, tex.e);
    if (tex.stem.empty()) {
      skipped++;
      continue;
    }
    if (seen.insert(tex.stem).second) out.push_back(tex);
  }
}

static uint64_t texels(const Tex& t) { return (uint64_t)t.e.w * t.e.h; }

static const char* const EXPORT_KINDS[] = {"rgb", "alpha", "models-rgb", "models-alpha"};

static void export_sprites(const Work& wk, const Options& o, int threads, uint64_t budget, bool sprites, bool models) {
  for (const char* kind : EXPORT_KINDS) fs::create_directories(upath(o.exportDir) / kind);
  std::vector<const Tex*> all;
  if (sprites)
    for (auto& t : wk.sprites) all.push_back(&t);
  if (models)
    for (auto& t : wk.models) all.push_back(&t);
  run_jobs(all.size(), threads, budget, [&](size_t i) { return texels(*all[i]); },
           [&](size_t i) { do_sprite(*all[i], o, fs::path(), o.scale, true); }, "export");
  say("note %d sprite textures exported", (int)all.size());
}

static void make_pack(const Work& wk, const Options& o, const fs::path& outDir, int threads, uint64_t budget) {
  fs::create_directories(outDir);
  if (!wk.models.empty()) fs::create_directories(outDir / "models");
  say("note %d sprite textures (x%d), %d characters (x%d), %d model textures, %d files skipped",
      (int)(wk.sprites.size() + wk.strayOutlines.size()), o.scale, (int)wk.glyphs.size(), FONT_SCALE, (int)wk.models.size(),
      wk.skipped);
  std::atomic<int> larger{0};
  run_jobs(wk.glyphs.size(), threads, budget, [&](size_t) { return (uint64_t)1; },
           [&](size_t i) { larger += do_glyph(wk.glyphs[i], outDir) ? 1 : 0; }, "fonts");
  Options plain = o;
  plain.exportDir.clear();
  run_jobs(wk.strayOutlines.size(), threads, budget, [&](size_t i) { return texels(wk.strayOutlines[i]); },
           [&](size_t i) { do_sprite(wk.strayOutlines[i], plain, outDir, FONT_SCALE, false); }, "fonts");
  run_jobs(wk.sprites.size(), threads, budget, [&](size_t i) { return texels(wk.sprites[i]); },
           [&](size_t i) { do_sprite(wk.sprites[i], plain, outDir, o.scale, true); },
           o.importDir.empty() ? "sprites" : "import");
  run_jobs(wk.models.size(), threads, budget, [&](size_t i) { return texels(wk.models[i]); },
           [&](size_t i) { do_sprite(wk.models[i], plain, outDir, o.scale, true); },
           o.importDir.empty() ? "models" : "import-models");
  say("note characters: %d enlarged from their own bitmap, %d of the 16 pixel font from the 24 pixel bitmap",
      (int)wk.glyphs.size() - larger.load(), larger.load());
}

// ------------------------------------------------- the outside enlarger (AI)
//
// realesrgan-ncnn-vulkan (Real-ESRGAN, BSD 3-clause): a Vulkan program that
// enlarges a folder of pictures four times with a neural network. It is not
// part of the port; with --download it is fetched from the project's GitHub
// releases into the tool folder, on the player's say-so.

static const char TOOL_URL[] =
    "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-windows.zip";
static const char TOOL_EXE[] = "realesrgan-ncnn-vulkan.exe";

static fs::path find_tool(const fs::path& dir) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return fs::path();
  for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    if (it->path().filename() == TOOL_EXE) return it->path();
  return fs::path();
}

static bool read_file(const fs::path& p, Bytes& out) {
  FILE* f = open_file(p, "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  out.resize(n > 0 ? (size_t)n : 0);
  bool ok = n >= 0 && fread(out.data(), 1, out.size(), f) == out.size();
  fclose(f);
  return ok;
}

// Unpacks a zip archive (stored and deflated members) into a folder.
static bool unzip(const fs::path& zip, const fs::path& dest) {
  Bytes z;
  if (!read_file(zip, z) || z.size() < 22) return false;
  auto le16 = [&](size_t o) { return (uint32_t)z[o] | (z[o + 1] << 8); };
  auto le32 = [&](size_t o) { return (uint32_t)z[o] | (z[o + 1] << 8) | (z[o + 2] << 16) | ((uint32_t)z[o + 3] << 24); };
  size_t eocd = z.size() - 22;
  while (le32(eocd) != 0x06054b50) {
    if (eocd == 0 || z.size() - eocd > 66000) return false;
    eocd--;
  }
  uint32_t count = le16(eocd + 10);
  size_t p = le32(eocd + 16);
  for (uint32_t i = 0; i < count; i++) {
    if (p + 46 > z.size() || le32(p) != 0x02014b50) return false;
    uint32_t method = le16(p + 10), csize = le32(p + 20), usize = le32(p + 24);
    uint32_t nlen = le16(p + 28), xlen = le16(p + 30), clen = le16(p + 32), local = le32(p + 42);
    std::string name((const char*)&z[p + 46], nlen);
    p += 46 + nlen + xlen + clen;
    if (name.find("..") != std::string::npos || name.empty() || name[0] == '/' || name.find(':') != std::string::npos) return false;
    fs::path target = dest / upath(name);
    if (name.back() == '/') {
      fs::create_directories(target);
      continue;
    }
    if (local + 30 > z.size() || le32(local) != 0x04034b50) return false;
    size_t data = local + 30 + le16(local + 26) + le16(local + 28);
    if (data + csize > z.size()) return false;
    fs::create_directories(target.parent_path());
    FILE* f = open_file(target, "wb");
    if (!f) return false;
    bool ok = true;
    if (method == 0) {
      ok = fwrite(&z[data], 1, csize, f) == csize;
    } else if (method == 8) {
      int len = 0;
      char* u = stbi_zlib_decode_malloc_guesssize_headerflag((const char*)&z[data], (int)csize, (int)usize + 16, &len, 0);
      ok = u && (uint32_t)len == usize && fwrite(u, 1, len, f) == (size_t)len;
      free(u);
    } else {
      ok = false;
    }
    fclose(f);
    if (!ok) return false;
  }
  return true;
}

static size_t count_png(const fs::path& dir) {
  size_t n = 0;
  std::error_code ec;
  for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
    if (it->path().extension() == ".png") n++;
  return n;
}

#ifdef _WIN32
static std::wstring quote(const std::wstring& s) { return L"\"" + s + L"\""; }

// Runs the enlarger on one folder; progress from the number of pictures it
// has written. The child dies with this process (job object), so that a
// cancel from the settings program stops the GPU work too.
static bool run_tool(const fs::path& exe0, const fs::path& in0, const fs::path& out0, const std::string& model, int tile) {
  // it is started in its own folder, so no path may be relative to ours
  fs::path exe = fs::absolute(exe0), in = fs::absolute(in0), out = fs::absolute(out0);
  size_t total = std::max<size_t>(count_png(in), 1);
  std::wstring cmd = quote(exe.wstring()) + L" -i " + quote(in.wstring()) + L" -o " + quote(out.wstring()) + L" -n " +
                     std::wstring(model.begin(), model.end()) + L" -m " + quote((exe.parent_path() / "models").wstring()) +
                     L" -s 4 -f png -t " + std::to_wstring(tile);
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
  info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof info);
  STARTUPINFOW si = {};
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES;
  SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
  HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
  si.hStdOutput = si.hStdError = nul;  // it prints a line per tile
  PROCESS_INFORMATION pi = {};
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(0);
  if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                      exe.parent_path().c_str(), &si, &pi)) {
    CloseHandle(nul);
    CloseHandle(job);
    return false;
  }
  AssignProcessToJobObject(job, pi.hProcess);
  ResumeThread(pi.hThread);
  int last = -1;
  while (WaitForSingleObject(pi.hProcess, 1000) == WAIT_TIMEOUT) {
    int percent = (int)(100 * std::min(count_png(out), total) / total);
    if (percent != last) say("progress %d", last = percent);
  }
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  CloseHandle(nul);
  CloseHandle(job);
  // the program's exit code is not dependable; what counts is that every picture is there
  return count_png(out) >= count_png(in);
}

static bool download(const fs::path& dest) {
  std::wstring url(TOOL_URL, TOOL_URL + strlen(TOOL_URL));
  return URLDownloadToFileW(nullptr, url.c_str(), dest.c_str(), 0, nullptr) == S_OK;
}
#endif

static void ai_enlarge(const Options& o, const fs::path& work) {
#ifdef _WIN32
  fs::path toolDir = o.tool.empty() ? work / "tool" : upath(o.tool);
  fs::path exe = find_tool(toolDir);
  if (exe.empty()) {
    if (!o.download) fail("the enlarger is not in %s (start with --download to fetch it from %s)", toolDir.u8string().c_str(), TOOL_URL);
    say("stage download");
    fs::create_directories(toolDir);
    fs::path zip = work / "realesrgan.zip";
    if (!download(zip)) fail("the download failed: %s", TOOL_URL);
    if (!unzip(zip, toolDir)) fail("the downloaded archive could not be unpacked");
    std::error_code ec;
    fs::remove(zip, ec);
    exe = find_tool(toolDir);
    if (exe.empty()) fail("the downloaded archive holds no %s", TOOL_EXE);
  }
  for (const char* kind : EXPORT_KINDS) {
    const std::string& model = strncmp(kind, "models", 6) == 0 ? o.aiModels : o.ai;
    fs::path in = work / "src" / kind, out = work / "up" / kind;
    std::error_code ec;
    fs::remove_all(out, ec);
    fs::create_directories(out);
    say("stage enlarge-%s", kind);
    if (count_png(in) == 0 || model.empty()) continue;
    if (!run_tool(exe, in, out, model, o.tile))
      fail("the enlarger stopped before it was done (%s). It needs a graphics card with Vulkan.", kind);
    say("progress 100");
  }
#else
  (void)o;
  (void)work;
  fail("--ai is only built for Windows; use --export-sprites and --import-sprites");
#endif
}

// ---------------------------------------------------------------------- main

static int run(const std::vector<std::string>& args) {
  Options o;
  if (args.size() == 3 && args[0] == "--unzip") {  // testing aid: the archive reader alone
    bool ok = unzip(upath(args[1]), upath(args[2]));
    say(ok ? "done %s" : "error cannot unpack %s", args[1].c_str());
    return ok ? 0 : 1;
  }
  for (size_t i = 0; i < args.size(); i++) {
    const std::string& a = args[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= args.size()) fail("%s needs a value", a.c_str());
      return args[++i];
    };
    if (a == "--iso") o.iso = value();
    else if (a == "--out") o.out = value();
    else if (a == "--scale") o.scale = atoi(value().c_str());
    else if (a == "--jobs") o.jobs = atoi(value().c_str());
    else if (a == "--only") o.only = value();
    else if (a == "--export-sprites") o.exportDir = value();
    else if (a == "--import-sprites") o.importDir = value();
    else if (a == "--ai") o.ai = value();
    else if (a == "--ai-models") o.aiModels = value();
    else if (a == "--work") o.work = value();
    else if (a == "--tool") o.tool = value();
    else if (a == "--tile") o.tile = atoi(value().c_str());
    else if (a == "--download") o.download = true;
    else if (a == "--models") o.models = true;
    else fail("unknown argument %s", a.c_str());
  }
  if (o.iso.empty()) fail("--iso <disc image> is needed");
  if (o.scale != 2 && o.scale != 4) fail("--scale is 2 or 4");
  if (o.exportDir.empty() && o.out.empty()) o.out = "hires";
  for (const std::string* m : {&o.ai, &o.aiModels})
    if (!m->empty() && *m != "realesrgan-x4plus-anime" && *m != "realesrgan-x4plus") fail("unknown model %s", m->c_str());
  if (!o.aiModels.empty()) o.models = true;
  bool ai = !o.ai.empty() || !o.aiModels.empty();
  int threads = o.jobs > 0 ? o.jobs : (int)std::max(1u, std::thread::hardware_concurrency());
  // About 100 bytes of working memory per source texel at the peak; a 32-bit
  // process has 2 to 4 GB of address space.
  uint64_t budget = sizeof(void*) == 4 ? 6u * 1024 * 1024 : 64u * 1024 * 1024;

  auto started = std::chrono::steady_clock::now();
  Disc disc;
  disc.open(o.iso);
  Work wk = collect(disc, o);

  if (!o.exportDir.empty() && !ai) {
    export_sprites(wk, o, threads, budget, true, true);
    say("done %s", o.exportDir.c_str());
    return 0;
  }

  fs::path work;
  if (ai) {
    work = o.work.empty() ? upath(o.out).parent_path() / "hires-ai" : upath(o.work);
    std::error_code ec;
    fs::remove_all(work / "src", ec);
    fs::remove_all(work / "up", ec);
    fs::create_directories(work);
    o.exportDir = (work / "src").u8string();
    export_sprites(wk, o, threads, budget, !o.ai.empty(), !o.aiModels.empty());
    ai_enlarge(o, work);
    o.exportDir.clear();
    o.importDir = (work / "up").u8string();
  }

  // Made beside the destination and renamed at the end, so that a run that
  // is stopped half way leaves the old folder as it was.
  fs::path out = upath(o.out), tmp = out;
  tmp += ".making";
  std::error_code ec;
  fs::remove_all(tmp, ec);
  make_pack(wk, o, tmp, threads, budget);
  fs::remove_all(out, ec);
  fs::rename(tmp, out, ec);
  if (ec) fail("cannot put the folder in place: %s", out.u8string().c_str());
  if (ai) {
    fs::remove_all(work / "src", ec);
    fs::remove_all(work / "up", ec);
  }
  double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  say("note done in %.0f s", seconds);
  say("done %s", o.out.c_str());
  return 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
  std::vector<std::string> args;
  for (int i = 1; i < argc; i++) {
    int n = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, s.data(), n, nullptr, nullptr);
    args.push_back(s);
  }
  return run(args);
}
#else
int main(int argc, char** argv) { return run(std::vector<std::string>(argv + 1, argv + argc)); }
#endif
