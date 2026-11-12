/*
 * GX calls whose objects the game and Aurora size differently.
 *
 * The game allocates the SDK's own GXTexObj (32 bytes) and GXTlutObj
 * (12 bytes), inside its structures and on its stacks. Aurora's are 64 and
 * 40 bytes on PC. The link sends the game's calls here (ld --wrap, see
 * GFX_WRAPPED in port/tools/link_census.py); the game's object only records
 * what it was initialised with, and Aurora's object is made when the
 * texture is loaded.
 *
 * Aurora tells textures apart by an id it assigns on GXInitTexObj, so its
 * objects are kept, keyed by what describes the texture. GXInvalidateTexAll
 * (the game calls it after changing texture memory) starts a new generation
 * of them.
 *
 * GXSetArray: the SDK call has no size, Aurora's needs one, and uploads that
 * much per draw. For model data the archive conversion knows where each
 * vertex buffer ends (mkgp2pc_array_extent); buffers it has not seen get
 * 64 KB.
 *
 * GXSetChanAmbColor / GXSetChanMatColor: in the SDK, GX_COLORn replaces only
 * the red, green and blue of the channel's register and GX_ALPHAn only its
 * alpha (src/dolphin/gx/GXLight.c). Aurora writes all four components for
 * either id, so the game's "set the alpha" call (HSD_SetupChannel does one
 * per lit material) turned the material colour black. The wrappers keep the
 * register's value and always hand Aurora the whole colour.
 *
 * HSD_ShadowSetSize: before each frame the game draws its 512x512 shadow
 * image into a corner of the frame buffer and copies it out
 * (HSD_ShadowStartRender, GXCopyTex). The console's frame buffer is 528
 * lines high whatever the render mode says; Aurora's is as high as the
 * render mode's efbHeight, 480, so the image lost its last 32 lines, among
 * them the white border that texture clamping repeats over the ground
 * outside the image. The wrapper keeps the image inside 480x480. Everything
 * else (viewport, scissor, copy, texture matrix) follows the image's size.
 */
#include <dolphin/gx.h>
#include <aurora/texture.hpp>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <tuple>
#include <vector>

extern "C" {
void __real_GXInitTexObj(GXTexObj*, const void*, u16, u16, GXTexFmt, GXTexWrapMode, GXTexWrapMode, GXBool);
void __real_GXInitTexObjCI(GXTexObj*, const void*, u16, u16, GXCITexFmt, GXTexWrapMode, GXTexWrapMode, GXBool, u32);
void __real_GXInitTexObjLOD(GXTexObj*, GXTexFilter, GXTexFilter, float, float, float, GXBool, GXBool, GXAnisotropy);
void __real_GXLoadTexObj(GXTexObj*, GXTexMapID);
void __real_GXInitTlutObj(GXTlutObj*, const void*, GXTlutFmt, u16);
void __real_GXLoadTlut(const GXTlutObj*, u32);
void __real_GXSetArray(GXAttr, const void*, u32, u8, bool);
void __real_GXInvalidateTexAll(void);
void __real_GXSetChanAmbColor(GXChannelID, GXColor);
void __real_GXSetChanMatColor(GXChannelID, GXColor);
uint32_t mkgp2pc_array_extent(const void* base);
void mkgp2pc_array_forget(const void* base, uint32_t size);
void* __real_OSAllocFromHeap(int heap, u32 size);
extern unsigned mkgp2pc_data_generation;
}

namespace {
struct GameTexObj {
  const void* data;
  uint16_t width, height;
  uint8_t format, wrapS, wrapT, flags;
  uint8_t minFilt, magFilt, aniso, pad;
  float minLod, maxLod, lodBias;
  uint32_t tlut;
};
static_assert(sizeof(GameTexObj) == 32, "the SDK's GXTexObj is 32 bytes");
enum : uint8_t { kMipmap = 1, kIndexed = 2, kLod = 4, kBiasClamp = 8, kEdgeLod = 16 };

struct GameTlutObj {
  const void* data;
  uint16_t format, entries;
  uint32_t pad;
};
static_assert(sizeof(GameTlutObj) == 12, "the SDK's GXTlutObj is 12 bytes");

using Key = std::tuple<const void*, uint16_t, uint16_t, uint8_t, uint8_t, uint32_t>;
std::map<Key, GXTexObj> sTextures;
} // namespace

extern "C" {
void __wrap_GXInitTexObj(GameTexObj* o, const void* data, u16 width, u16 height, u32 format, u32 wrapS, u32 wrapT,
                         u8 mipmap) {
  std::memset(o, 0, sizeof(*o));
  o->data = data;
  o->width = width;
  o->height = height;
  o->format = static_cast<uint8_t>(format);
  o->wrapS = static_cast<uint8_t>(wrapS);
  o->wrapT = static_cast<uint8_t>(wrapT);
  o->flags = mipmap ? kMipmap : 0;
}

void __wrap_GXInitTexObjCI(GameTexObj* o, const void* data, u16 width, u16 height, u32 format, u32 wrapS, u32 wrapT,
                           u8 mipmap, u32 tlut) {
  __wrap_GXInitTexObj(o, data, width, height, format, wrapS, wrapT, mipmap);
  o->flags |= kIndexed;
  o->tlut = tlut;
}

void __wrap_GXInitTexObjLOD(GameTexObj* o, u32 minFilt, u32 magFilt, float minLod, float maxLod, float lodBias,
                            u8 biasClamp, u8 edgeLod, u32 aniso) {
  o->flags |= kLod;
  o->flags = static_cast<uint8_t>((o->flags & ~(kBiasClamp | kEdgeLod)) | (biasClamp ? kBiasClamp : 0) |
                                  (edgeLod ? kEdgeLod : 0));
  o->minFilt = static_cast<uint8_t>(minFilt);
  o->magFilt = static_cast<uint8_t>(magFilt);
  o->aniso = static_cast<uint8_t>(aniso);
  o->minLod = minLod;
  o->maxLod = maxLod;
  o->lodBias = lodBias;
}

// Images the game copies out of the frame buffer (the kart shadow image,
// HSD_ShadowEndRender). The game samples the shadow with GX_NEAR when it is
// magnified, which shows every texel of the 480x480 image as a hard-edged
// block on the ground; the user compared with the cabinet (2026-11-12) and
// the port looked clearly more jagged. No hardware filter that would explain
// the cabinet's smoother look was found (copy filter is 0,0,21,22,21,0,0,
// progressive output), so this is a deliberate departure: such images are
// magnified with GX_LINEAR. MKGP2PC_SHADOW_NEAREST=1 gives the game's own
// setting back.
static std::set<const void*> sCopyTargets;
static int sSmoothCopies = -1;
static bool smooth_copies() {
  if (sSmoothCopies < 0) sSmoothCopies = getenv("MKGP2PC_SHADOW_NEAREST") == nullptr;
  return sSmoothCopies != 0;
}
extern "C" {
/* the menu's setting (port_settings.h); read at every load of the image, so it holds from the next frame */
int mkgp2pc_shadow_smooth(void) { return smooth_copies(); }
void mkgp2pc_shadow_smooth_set(int on) { sSmoothCopies = on != 0; }
}

bool mkgp2pc_board_texture;
extern const void* mkgp2pc_camera_texture;
extern uint32_t mkgp2pc_camera_texture_serial;
void __wrap_GXLoadTexObj(GameTexObj* o, u32 id) {
  if (id == 0) {
    mkgp2pc_board_texture = o->width == 64 && o->height == 64 && o->format == 5;
  }
  const Key key{o->data, o->width, o->height, o->format, static_cast<uint8_t>(o->flags & (kMipmap | kIndexed)), o->tlut};
  static unsigned generation;
  if (generation != mkgp2pc_data_generation) {
    generation = mkgp2pc_data_generation;
    sTextures.clear();
  }
  auto it = sTextures.find(key);
  if (it == sTextures.end()) {
    GXTexObj real;
    const bool mip = (o->flags & kMipmap) != 0;
    if (o->flags & kIndexed) {
      __real_GXInitTexObjCI(&real, o->data, o->width, o->height, static_cast<GXCITexFmt>(o->format),
                            static_cast<GXTexWrapMode>(o->wrapS), static_cast<GXTexWrapMode>(o->wrapT), mip, o->tlut);
    } else {
      __real_GXInitTexObj(&real, o->data, o->width, o->height, static_cast<GXTexFmt>(o->format),
                          static_cast<GXTexWrapMode>(o->wrapS), static_cast<GXTexWrapMode>(o->wrapT), mip);
    }
    it = sTextures.emplace(key, real).first;
  }
  GXTexObj* real = &it->second;
  {
    /* The camera writes a new picture into the same memory
     * (port/platform/camera/camera.c). */
    static uint32_t cameraSerial;
    if (o->data == mkgp2pc_camera_texture && cameraSerial != mkgp2pc_camera_texture_serial) {
      cameraSerial = mkgp2pc_camera_texture_serial;
      for (auto& [k, cached] : sTextures) {
        if (std::get<0>(k) == o->data) GXInitTexObjData(&cached, o->data);
      }
    }
  }
  GXInitTexObjWrapMode(real, static_cast<GXTexWrapMode>(o->wrapS), static_cast<GXTexWrapMode>(o->wrapT));
  if (o->flags & kLod) {
    u32 magFilt = o->magFilt;
    if (magFilt == GX_NEAR && smooth_copies() && sCopyTargets.count(o->data) != 0) {
      magFilt = GX_LINEAR;
    }
    __real_GXInitTexObjLOD(real, static_cast<GXTexFilter>(o->minFilt), static_cast<GXTexFilter>(magFilt), o->minLod,
                           o->maxLod, o->lodBias, (o->flags & kBiasClamp) != 0, (o->flags & kEdgeLod) != 0,
                           static_cast<GXAnisotropy>(o->aniso));
  }
  __real_GXLoadTexObj(real, static_cast<GXTexMapID>(id));
}

void __wrap_GXInitTlutObj(GameTlutObj* o, const void* data, u32 format, u16 entries) {
  o->data = data;
  o->format = static_cast<uint16_t>(format);
  o->entries = entries;
  o->pad = 0;
}

void __wrap_GXLoadTlut(const GameTlutObj* o, u32 idx) {
  GXTlutObj real;
  __real_GXInitTlutObj(&real, o->data, static_cast<GXTlutFmt>(o->format), o->entries);
  __real_GXLoadTlut(&real, idx);
}

/* Every block of the game's heap comes from here. What the archive
 * conversion recorded about those addresses belongs to an archive that has
 * been freed since. */
void* __wrap_OSAllocFromHeap(int heap, u32 size) {
  void* p = __real_OSAllocFromHeap(heap, size);
  if (p != nullptr) {
    mkgp2pc_array_forget(p, size);
  }
  return p;
}

/* the position array, for the wide picture's look into display lists */
static const u8* mkgp2pc_wide_pos_array;
static u32 mkgp2pc_wide_pos_stride;
void __wrap_GXSetArray(u32 attr, const void* base, u8 stride) {
  const uintptr_t end = 0x83000000u;
  uint32_t size = mkgp2pc_array_extent(base);
  if (size == 0) {
    size = 0x10000;
    const uintptr_t at = reinterpret_cast<uintptr_t>(base);
    if (at < end && at + size > end) {
      size = static_cast<uint32_t>(end - at);
    }
  }
  if (attr == GX_VA_POS) {
    mkgp2pc_wide_pos_array = static_cast<const u8*>(base);
    mkgp2pc_wide_pos_stride = stride;
  }
  __real_GXSetArray(static_cast<GXAttr>(attr), base, size, stride, false);
}

namespace {
/* What the SDK's GXInit leaves in the registers: black ambient, white material. */
GXColor sAmb[2] = {{0, 0, 0, 0}, {0, 0, 0, 0}};
GXColor sMat[2] = {{255, 255, 255, 255}, {255, 255, 255, 255}};

void set_chan_color(GXColor* reg, u32 id, GXColor color, void (*real)(GXChannelID, GXColor)) {
  u32 chan;
  switch (id) {
  case GX_COLOR0:
  case GX_COLOR1:
    chan = id - GX_COLOR0;
    reg[chan].r = color.r;
    reg[chan].g = color.g;
    reg[chan].b = color.b;
    break;
  case GX_ALPHA0:
  case GX_ALPHA1:
    chan = id - GX_ALPHA0;
    reg[chan].a = color.a;
    break;
  case GX_COLOR0A0:
  case GX_COLOR1A1:
    chan = id - GX_COLOR0A0;
    reg[chan] = color;
    break;
  default:
    return;
  }
  real(chan == 0 ? GX_COLOR0A0 : GX_COLOR1A1, reg[chan]);
}
} // namespace

void __wrap_GXSetChanAmbColor(u32 id, GXColor color) { set_chan_color(sAmb, id, color, __real_GXSetChanAmbColor); }
void __wrap_GXSetChanMatColor(u32 id, GXColor color) { set_chan_color(sMat, id, color, __real_GXSetChanMatColor); }

/* The game changes texture memory in place and says so with this call. */
void mkgp2pc_gx_forget_textures(void) { sTextures.clear(); }
void __wrap_GXInvalidateTexAll(void) {
  sTextures.clear();
  __real_GXInvalidateTexAll();
}
}

/*
 * Item shadow record for dumps. The game draws one small textured board
 * (model eff_tip_aura_w: 64x64 RGB5A3 picture, a 32-byte display list of
 * one quad of +-2.5 units) under every item. Each such draw of the frame is
 * kept with the matrices in force, the board's place on the screen worked
 * out from them, and the constant alpha the game asked for, so that a dump
 * (state.txt) can be compared with its picture.
 */
extern "C" {
void __real_GXSetProjection(const void* mtx, u32 type);
void __real_GXLoadPosMtxImm(const void* mtx, u32 id);
void __real_GXCallDisplayList(const void* list, u32 nbytes);
void __real_GXSetTevKColor(u32 id, GXColor color);
}
namespace {
struct ShadowDraw {
  float mv[3][4];
  float box[4]; /* left, top, right, bottom in a 640x480 picture */
  float w;      /* clip-space w of the board's centre */
  uint8_t kAlpha[4];
};
float sProj[4][4];
u32 sProjType;
float sPosMtx[3][4];
GXColor sKColor[4];
ShadowDraw sShadowNow[16], sShadowLast[16];
int sShadowNowCount, sShadowLastCount;
} // namespace

void mkgp2pc_wide_projection(void); /* below: sends sProj, corrected for a wide picture */
bool wide_list_before(const void* list, u32 nbytes);
void wide_list_after(void);
void wide_census_projection(void* caller);
void wide_census_draw(int kind);
extern "C" void __wrap_GXSetProjection(const void* mtx, u32 type) {
  std::memcpy(sProj, mtx, sizeof(sProj));
  sProjType = type;
  wide_census_projection(__builtin_return_address(0));
  mkgp2pc_wide_projection();
}
void mkgp2pc_wide_posmtx(const void* mtx, u32 id);
extern "C" void __wrap_GXLoadPosMtxImm(const void* mtx, u32 id) {
  if (id == 0) {
    std::memcpy(sPosMtx, mtx, sizeof(sPosMtx));
  }
  mkgp2pc_wide_posmtx(mtx, id);
  __real_GXLoadPosMtxImm(mtx, id);
}
extern "C" void __wrap_GXSetTevKColor(u32 id, GXColor color) {
  if (id < 4) {
    sKColor[id] = color;
  }
  __real_GXSetTevKColor(id, color);
}
/*
 * The frame's draw commands, for dumps (docs/port/dumps.md,
 * port/tools/gx_fifo.py). Aurora's GX functions do not draw: they write the
 * console's command stream (BP / CP / XF register loads, vertex data, and
 * Aurora's own commands for what the console passed as addresses) into a
 * buffer that a worker thread works through. That buffer is copied here as
 * it grows, so that a dump can say what was sent for a frame, in order.
 *
 * The buffer is emptied inside some calls (GXDrawDone, the copies, the end
 * of the frame). Before each of those known here the new bytes are taken
 * and the buffer is emptied by hand (AuroraGXSync), so what such a call
 * writes itself starts at zero again; a note names the call. Anywhere else
 * a buffer that became shorter is recorded as "emptied unseen" (the bytes
 * since the last look are missing).
 *
 * File: "MKGX1\0\0\0", then records: u32 kind, u32 length, payload
 * (little-endian). Kind 0: stream bytes. 1: a note (text). 2: the vertex
 * description and formats in force at the frame's start (21 attribute
 * types, then 8 formats x 12 attributes x (count, type, shift)).
 * MKGP2PC_DUMP_GX=0 switches the copying off.
 */
namespace aurora::gx::fifo {
const uint8_t* get_buffer_data();
uint32_t get_buffer_size();
} // namespace aurora::gx::fifo
extern "C" {
typedef struct gzFile_s* gzFile; /* zlib without its header, as in diag.c */
gzFile gzopen(const char* path, const char* mode);
int gzwrite(gzFile file, const void* buf, unsigned len);
int gzclose(gzFile file);
}
namespace {
std::vector<uint8_t> sGxNow, sGxLast;
uint32_t sGxSeen;
bool sGxNeedState = true;
int sGxOn = -1;
bool gxlog_on() {
  if (sGxOn < 0) {
    const char* v = getenv("MKGP2PC_DUMP_GX");
    sGxOn = v == nullptr || strcmp(v, "0") != 0;
  }
  return sGxOn != 0;
}
void gxlog_record(u32 kind, const void* data, u32 size) {
  const u32 head[2] = {kind, size};
  const auto* h = reinterpret_cast<const uint8_t*>(head);
  sGxNow.insert(sGxNow.end(), h, h + 8);
  const auto* d = static_cast<const uint8_t*>(data);
  sGxNow.insert(sGxNow.end(), d, d + size);
}
void gxlog_state() {
  uint8_t st[21 + 8 * 12 * 3];
  for (int a = 0; a < 21; a++) {
    GXAttrType t = GX_NONE;
    GXGetVtxDesc(static_cast<GXAttr>(a), &t);
    st[a] = static_cast<uint8_t>(t);
  }
  uint8_t* o = st + 21;
  for (int f = 0; f < 8; f++) {
    for (int a = GX_VA_POS; a <= GX_VA_TEX7; a++) {
      GXCompCnt cnt = static_cast<GXCompCnt>(0);
      GXCompType type = static_cast<GXCompType>(0);
      u8 shift = 0;
      GXGetVtxAttrFmt(static_cast<GXVtxFmt>(f), static_cast<GXAttr>(a), &cnt, &type, &shift);
      *o++ = static_cast<uint8_t>(cnt);
      *o++ = static_cast<uint8_t>(type);
      *o++ = shift;
    }
  }
  gxlog_record(2, st, sizeof(st));
}
void gxlog_sample() {
  if (!gxlog_on()) {
    return;
  }
  if (sGxNeedState) {
    sGxNeedState = false;
    gxlog_state();
  }
  const uint32_t size = aurora::gx::fifo::get_buffer_size();
  if (size < sGxSeen) {
    static const char text[] = "emptied unseen: commands since the last look are missing";
    gxlog_record(1, text, sizeof(text) - 1);
    sGxSeen = 0;
  }
  if (size > sGxSeen) {
    gxlog_record(0, aurora::gx::fifo::get_buffer_data() + sGxSeen, size - sGxSeen);
    sGxSeen = size;
  }
}
/* around a call that empties the buffer itself */
void gxlog_before_emptying() {
  if (!gxlog_on()) {
    return;
  }
  gxlog_sample();
  AuroraGXSync();
  sGxSeen = 0;
}
void gxlog_after_emptying(const char* fmt, ...) {
  if (!gxlog_on()) {
    return;
  }
  const uint32_t size = aurora::gx::fifo::get_buffer_size();
  if (size != 0) {
    /* all of the call's own commands, or those after its emptying */
    gxlog_record(0, aurora::gx::fifo::get_buffer_data(), size);
  }
  sGxSeen = size;
  char text[160];
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(text, sizeof(text), fmt, ap);
  va_end(ap);
  gxlog_record(1, text, static_cast<u32>(n < 0 ? 0 : n >= static_cast<int>(sizeof(text)) ? sizeof(text) - 1 : n));
}
void gxlog_file(const char* dir, const char* name, const std::vector<uint8_t>& bytes) {
  char path[700];
  snprintf(path, sizeof(path), "%s/%s", dir, name);
  gzFile z = gzopen(path, "wb1");
  if (z == nullptr) {
    return;
  }
  gzwrite(z, "MKGX1\0\0\0", 8);
  for (size_t at = 0; at < bytes.size();) {
    const size_t n = bytes.size() - at < (1u << 24) ? bytes.size() - at : (1u << 24);
    gzwrite(z, bytes.data() + at, static_cast<unsigned>(n));
    at += n;
  }
  gzclose(z);
}
} // namespace
/* video_aurora.c: before the frame is handed over (retrace: for the note) */
extern "C" void mkgp2pc_gxlog_frame_end(void) { gxlog_sample(); }
/* ... and after: the frame's record becomes "the frame before" */
extern "C" void mkgp2pc_gxlog_frame_next(void) {
  if (!gxlog_on()) {
    return;
  }
  sGxLast.swap(sGxNow);
  sGxNow.clear();
  sGxSeen = 0;
  sGxNeedState = true;
}
/* gx_fifo.bin.gz: the frame so far (at F9: the whole frame, the dump is made
 * at its end). withLast (a crash): gx_fifo_prev.bin.gz, the frame before. */
extern "C" void mkgp2pc_gxlog_write(const char* dir, int withLast) {
  if (!gxlog_on()) {
    return;
  }
  gxlog_sample();
  gxlog_file(dir, "gx_fifo.bin.gz", sGxNow);
  if (withLast) {
    gxlog_file(dir, "gx_fifo_prev.bin.gz", sGxLast);
  }
}
extern "C" void __real_GXDrawDone(void);
extern "C" void __wrap_GXDrawDone(void) {
  gxlog_before_emptying();
  __real_GXDrawDone();
  gxlog_after_emptying("GXDrawDone");
}

extern "C" bool mkgp2pc_skip_draw;
extern "C" void __wrap_GXCallDisplayList(const void* list, u32 nbytes) {
  if (mkgp2pc_skip_draw) {
    return;
  }
  wide_census_draw(5);
  if (mkgp2pc_board_texture && nbytes == 32 && sShadowNowCount < 16) {
    ShadowDraw& d = sShadowNow[sShadowNowCount++];
    std::memcpy(d.mv, sPosMtx, sizeof(d.mv));
    d.box[0] = d.box[1] = 1e9f;
    d.box[2] = d.box[3] = -1e9f;
    for (int i = 0; i < 5; i++) {
      const float x = i == 4 ? 0.f : ((i & 1) ? 2.5f : -2.5f), y = i == 4 ? 0.f : ((i & 2) ? 2.5f : -2.5f);
      float v[4] = {0, 0, 0, 1}, c[4];
      for (int r = 0; r < 3; r++) {
        v[r] = sPosMtx[r][0] * x + sPosMtx[r][1] * y + sPosMtx[r][3];
      }
      for (int r = 0; r < 4; r++) {
        c[r] = sProj[r][0] * v[0] + sProj[r][1] * v[1] + sProj[r][2] * v[2] + sProj[r][3] * v[3];
      }
      if (i == 4) {
        d.w = c[3];
        break;
      }
      if (c[3] > 0.f) {
        const float sx = (c[0] / c[3] * 0.5f + 0.5f) * 640.f, sy = (0.5f - c[1] / c[3] * 0.5f) * 480.f;
        d.box[0] = sx < d.box[0] ? sx : d.box[0];
        d.box[1] = sy < d.box[1] ? sy : d.box[1];
        d.box[2] = sx > d.box[2] ? sx : d.box[2];
        d.box[3] = sy > d.box[3] ? sy : d.box[3];
      }
    }
    for (int i = 0; i < 4; i++) {
      d.kAlpha[i] = sKColor[i].a;
    }
  }
  const bool wide = wide_list_before(list, nbytes);
  __real_GXCallDisplayList(list, nbytes);
  if (wide) {
    wide_list_after();
  }
  gxlog_sample();
}
/* video_aurora.c: at the end of each frame, and when a dump is written. */
extern "C" void mkgp2pc_shadow_record_frame(void) {
  if (sShadowNowCount > 0) {
    std::memcpy(sShadowLast, sShadowNow, sizeof(sShadowLast));
    sShadowLastCount = sShadowNowCount;
  }
  sShadowNowCount = 0;
}
extern "C" void mkgp2pc_shadow_record_write(FILE* f) {
  /* the frame being finished if it drew any (a dump made while running),
   * else the last one that was drawn (a dump made while paused) */
  const ShadowDraw* list = sShadowNowCount > 0 ? sShadowNow : sShadowLast;
  const int count = sShadowNowCount > 0 ? sShadowNowCount : sShadowLastCount;
  std::fprintf(f, "item shadow boards drawn: %d\n", count);
  for (int i = 0; i < count; i++) {
    const ShadowDraw& d = list[i];
    std::fprintf(f, " board %d: screen x %.0f..%.0f y %.0f..%.0f, w %.1f, scale %.2f, constant alpha K0..K3 %u %u %u %u\n", i,
                 d.box[0], d.box[2], d.box[1], d.box[3], d.w,
                 std::sqrt(d.mv[0][0] * d.mv[0][0] + d.mv[1][0] * d.mv[1][0] + d.mv[2][0] * d.mv[2][0]), d.kAlpha[0],
                 d.kAlpha[1], d.kAlpha[2], d.kAlpha[3]);
  }
}

extern "C" void __real_HSD_ShadowSetSize(void* shadow, u16 width, u16 height);
extern "C" void __wrap_HSD_ShadowSetSize(void* shadow, u16 width, u16 height) {
  const u16 limit = 480;
  __real_HSD_ShadowSetSize(shadow, width > limit ? limit : width, height > limit ? limit : height);
}

// ---- Full speed (F10/Tab): frames that are not shown ----
// The game runs as fast as the machine can while the window is still given
// about 60 pictures a second. On the frames in between everything that
// draws is dropped here, before it reaches the rendering library: vertex
// data, display lists and copies of the frame buffer. State (matrices,
// textures, TEV) still goes through, so the next shown frame starts from
// what the game last set. Presenting every frame at full speed ended inside
// webgpu_dawn.dll on Windows within seconds (2026-11-11).
bool mkgp2pc_skip_draw = false;
// A wide picture (MKGP2PC_ASPECT, see "Wide picture" below) follows the
// vertex stream: every value goes through mkgp2pc_wide_value, which keeps
// the 2D primitives back (answer true: the value is not to be sent on).
extern "C" bool mkgp2pc_wide_on, mkgp2pc_wide_capture;
extern "C" bool mkgp2pc_wide_value(unsigned size, u32 bits);
static inline u32 wide_bits(f32 v) { u32 b; std::memcpy(&b, &v, 4); return b; }
extern "C" {
void __real_GXPosition3f32(f32 x, f32 y, f32 z);
void __wrap_GXPosition3f32(f32 x, f32 y, f32 z) {
  if (mkgp2pc_wide_on) {
    const bool kept = mkgp2pc_wide_capture;
    mkgp2pc_wide_value(4, wide_bits(x));
    mkgp2pc_wide_value(4, wide_bits(y));
    mkgp2pc_wide_value(4, wide_bits(z));
    if (kept) {
      return;
    }
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXPosition3f32(x, y, z);
  }
}
void __real_GXPosition3u16(u16 x, u16 y, u16 z);
void __wrap_GXPosition3u16(u16 x, u16 y, u16 z) {
  if (mkgp2pc_wide_on) {
    const bool kept = mkgp2pc_wide_capture;
    mkgp2pc_wide_value(2, (u16)x);
    mkgp2pc_wide_value(2, (u16)y);
    mkgp2pc_wide_value(2, (u16)z);
    if (kept) {
      return;
    }
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXPosition3u16(x, y, z);
  }
}
void __real_GXPosition3s16(s16 x, s16 y, s16 z);
void __wrap_GXPosition3s16(s16 x, s16 y, s16 z) {
  if (mkgp2pc_wide_on) {
    const bool kept = mkgp2pc_wide_capture;
    mkgp2pc_wide_value(2, (u16)x);
    mkgp2pc_wide_value(2, (u16)y);
    mkgp2pc_wide_value(2, (u16)z);
    if (kept) {
      return;
    }
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXPosition3s16(x, y, z);
  }
}
void __real_GXPosition3u8(u8 x, u8 y, u8 z);
void __wrap_GXPosition3u8(u8 x, u8 y, u8 z) {
  if (mkgp2pc_wide_on) {
    const bool kept = mkgp2pc_wide_capture;
    mkgp2pc_wide_value(1, (u8)x);
    mkgp2pc_wide_value(1, (u8)y);
    mkgp2pc_wide_value(1, (u8)z);
    if (kept) {
      return;
    }
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXPosition3u8(x, y, z);
  }
}
void __real_GXPosition3s8(s8 x, s8 y, s8 z);
void __wrap_GXPosition3s8(s8 x, s8 y, s8 z) {
  if (mkgp2pc_wide_on) {
    const bool kept = mkgp2pc_wide_capture;
    mkgp2pc_wide_value(1, (u8)x);
    mkgp2pc_wide_value(1, (u8)y);
    mkgp2pc_wide_value(1, (u8)z);
    if (kept) {
      return;
    }
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXPosition3s8(x, y, z);
  }
}
void __real_GXPosition2f32(f32 x, f32 y);
void __wrap_GXPosition2f32(f32 x, f32 y) {
  if (mkgp2pc_wide_on) {
    const bool kept = mkgp2pc_wide_capture;
    mkgp2pc_wide_value(4, wide_bits(x));
    mkgp2pc_wide_value(4, wide_bits(y));
    if (kept) {
      return;
    }
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXPosition2f32(x, y);
  }
}
void __real_GXPosition2u16(u16 x, u16 y);
void __wrap_GXPosition2u16(u16 x, u16 y) {
  if (mkgp2pc_wide_on) {
    const bool kept = mkgp2pc_wide_capture;
    mkgp2pc_wide_value(2, (u16)x);
    mkgp2pc_wide_value(2, (u16)y);
    if (kept) {
      return;
    }
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXPosition2u16(x, y);
  }
}
void __real_GXPosition2s16(s16 x, s16 y);
void __wrap_GXPosition2s16(s16 x, s16 y) {
  if (mkgp2pc_wide_on) {
    const bool kept = mkgp2pc_wide_capture;
    mkgp2pc_wide_value(2, (u16)x);
    mkgp2pc_wide_value(2, (u16)y);
    if (kept) {
      return;
    }
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXPosition2s16(x, y);
  }
}
void __real_GXPosition2u8(u8 x, u8 y);
void __wrap_GXPosition2u8(u8 x, u8 y) {
  if (mkgp2pc_wide_on) {
    const bool kept = mkgp2pc_wide_capture;
    mkgp2pc_wide_value(1, (u8)x);
    mkgp2pc_wide_value(1, (u8)y);
    if (kept) {
      return;
    }
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXPosition2u8(x, y);
  }
}
void __real_GXPosition2s8(s8 x, s8 y);
void __wrap_GXPosition2s8(s8 x, s8 y) {
  if (mkgp2pc_wide_on) {
    const bool kept = mkgp2pc_wide_capture;
    mkgp2pc_wide_value(1, (u8)x);
    mkgp2pc_wide_value(1, (u8)y);
    if (kept) {
      return;
    }
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXPosition2s8(x, y);
  }
}
void __real_GXPosition1x16(u16 index);
void __wrap_GXPosition1x16(u16 index) {
  if (mkgp2pc_wide_on && mkgp2pc_wide_value(2, (u16)index)) {
    return;
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXPosition1x16(index);
  }
}
void __real_GXPosition1x8(u8 index);
void __wrap_GXPosition1x8(u8 index) {
  if (mkgp2pc_wide_on && mkgp2pc_wide_value(1, (u8)index)) {
    return;
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXPosition1x8(index);
  }
}
void __real_GXNormal3f32(f32 x, f32 y, f32 z);
void __wrap_GXNormal3f32(f32 x, f32 y, f32 z) {
  if (mkgp2pc_wide_on) {
    const bool kept = mkgp2pc_wide_capture;
    mkgp2pc_wide_value(4, wide_bits(x));
    mkgp2pc_wide_value(4, wide_bits(y));
    mkgp2pc_wide_value(4, wide_bits(z));
    if (kept) {
      return;
    }
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXNormal3f32(x, y, z);
  }
}
void __real_GXNormal3s16(s16 x, s16 y, s16 z);
void __wrap_GXNormal3s16(s16 x, s16 y, s16 z) {
  if (mkgp2pc_wide_on) {
    const bool kept = mkgp2pc_wide_capture;
    mkgp2pc_wide_value(2, (u16)x);
    mkgp2pc_wide_value(2, (u16)y);
    mkgp2pc_wide_value(2, (u16)z);
    if (kept) {
      return;
    }
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXNormal3s16(x, y, z);
  }
}
void __real_GXNormal3s8(s8 x, s8 y, s8 z);
void __wrap_GXNormal3s8(s8 x, s8 y, s8 z) {
  if (mkgp2pc_wide_on) {
    const bool kept = mkgp2pc_wide_capture;
    mkgp2pc_wide_value(1, (u8)x);
    mkgp2pc_wide_value(1, (u8)y);
    mkgp2pc_wide_value(1, (u8)z);
    if (kept) {
      return;
    }
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXNormal3s8(x, y, z);
  }
}
void __real_GXNormal1x16(u16 index);
void __wrap_GXNormal1x16(u16 index) {
  if (mkgp2pc_wide_on && mkgp2pc_wide_value(2, (u16)index)) {
    return;
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXNormal1x16(index);
  }
}
void __real_GXNormal1x8(u8 index);
void __wrap_GXNormal1x8(u8 index) {
  if (mkgp2pc_wide_on && mkgp2pc_wide_value(1, (u8)index)) {
    return;
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXNormal1x8(index);
  }
}
void __real_GXColor4u8(u8 r, u8 g, u8 b, u8 a);
void __wrap_GXColor4u8(u8 r, u8 g, u8 b, u8 a) {
  if (mkgp2pc_wide_on) {
    const bool kept = mkgp2pc_wide_capture;
    mkgp2pc_wide_value(1, (u8)r);
    mkgp2pc_wide_value(1, (u8)g);
    mkgp2pc_wide_value(1, (u8)b);
    mkgp2pc_wide_value(1, (u8)a);
    if (kept) {
      return;
    }
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXColor4u8(r, g, b, a);
  }
}
void __real_GXColor3u8(u8 r, u8 g, u8 b);
void __wrap_GXColor3u8(u8 r, u8 g, u8 b) {
  if (mkgp2pc_wide_on) {
    const bool kept = mkgp2pc_wide_capture;
    mkgp2pc_wide_value(1, (u8)r);
    mkgp2pc_wide_value(1, (u8)g);
    mkgp2pc_wide_value(1, (u8)b);
    if (kept) {
      return;
    }
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXColor3u8(r, g, b);
  }
}
void __real_GXColor1u32(u32 clr);
void __wrap_GXColor1u32(u32 clr) {
  if (mkgp2pc_wide_on && mkgp2pc_wide_value(4, (u32)clr)) {
    return;
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXColor1u32(clr);
  }
}
void __real_GXColor1u16(u16 clr);
void __wrap_GXColor1u16(u16 clr) {
  if (mkgp2pc_wide_on && mkgp2pc_wide_value(2, (u16)clr)) {
    return;
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXColor1u16(clr);
  }
}
void __real_GXColor1x16(u16 index);
void __wrap_GXColor1x16(u16 index) {
  if (mkgp2pc_wide_on && mkgp2pc_wide_value(2, (u16)index)) {
    return;
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXColor1x16(index);
  }
}
void __real_GXColor1x8(u8 index);
void __wrap_GXColor1x8(u8 index) {
  if (mkgp2pc_wide_on && mkgp2pc_wide_value(1, (u8)index)) {
    return;
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXColor1x8(index);
  }
}
void __real_GXTexCoord2f32(f32 s, f32 t);
void __wrap_GXTexCoord2f32(f32 s, f32 t) {
  if (mkgp2pc_wide_on) {
    const bool kept = mkgp2pc_wide_capture;
    mkgp2pc_wide_value(4, wide_bits(s));
    mkgp2pc_wide_value(4, wide_bits(t));
    if (kept) {
      return;
    }
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXTexCoord2f32(s, t);
  }
}
void __real_GXTexCoord2u16(u16 s, u16 t);
void __wrap_GXTexCoord2u16(u16 s, u16 t) {
  if (mkgp2pc_wide_on) {
    const bool kept = mkgp2pc_wide_capture;
    mkgp2pc_wide_value(2, (u16)s);
    mkgp2pc_wide_value(2, (u16)t);
    if (kept) {
      return;
    }
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXTexCoord2u16(s, t);
  }
}
void __real_GXTexCoord2s16(s16 s, s16 t);
void __wrap_GXTexCoord2s16(s16 s, s16 t) {
  if (mkgp2pc_wide_on) {
    const bool kept = mkgp2pc_wide_capture;
    mkgp2pc_wide_value(2, (u16)s);
    mkgp2pc_wide_value(2, (u16)t);
    if (kept) {
      return;
    }
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXTexCoord2s16(s, t);
  }
}
void __real_GXTexCoord2u8(u8 s, u8 t);
void __wrap_GXTexCoord2u8(u8 s, u8 t) {
  if (mkgp2pc_wide_on) {
    const bool kept = mkgp2pc_wide_capture;
    mkgp2pc_wide_value(1, (u8)s);
    mkgp2pc_wide_value(1, (u8)t);
    if (kept) {
      return;
    }
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXTexCoord2u8(s, t);
  }
}
void __real_GXTexCoord2s8(s8 s, s8 t);
void __wrap_GXTexCoord2s8(s8 s, s8 t) {
  if (mkgp2pc_wide_on) {
    const bool kept = mkgp2pc_wide_capture;
    mkgp2pc_wide_value(1, (u8)s);
    mkgp2pc_wide_value(1, (u8)t);
    if (kept) {
      return;
    }
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXTexCoord2s8(s, t);
  }
}
void __real_GXTexCoord1f32(f32 s);
void __wrap_GXTexCoord1f32(f32 s) {
  if (mkgp2pc_wide_on && mkgp2pc_wide_value(4, wide_bits(s))) {
    return;
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXTexCoord1f32(s);
  }
}
void __real_GXTexCoord1u16(u16 s);
void __wrap_GXTexCoord1u16(u16 s) {
  if (mkgp2pc_wide_on && mkgp2pc_wide_value(2, (u16)s)) {
    return;
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXTexCoord1u16(s);
  }
}
void __real_GXTexCoord1s16(s16 s);
void __wrap_GXTexCoord1s16(s16 s) {
  if (mkgp2pc_wide_on && mkgp2pc_wide_value(2, (u16)s)) {
    return;
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXTexCoord1s16(s);
  }
}
void __real_GXTexCoord1u8(u8 s);
void __wrap_GXTexCoord1u8(u8 s) {
  if (mkgp2pc_wide_on && mkgp2pc_wide_value(1, (u8)s)) {
    return;
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXTexCoord1u8(s);
  }
}
void __real_GXTexCoord1s8(s8 s);
void __wrap_GXTexCoord1s8(s8 s) {
  if (mkgp2pc_wide_on && mkgp2pc_wide_value(1, (u8)s)) {
    return;
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXTexCoord1s8(s);
  }
}
void __real_GXTexCoord1x16(u16 index);
void __wrap_GXTexCoord1x16(u16 index) {
  if (mkgp2pc_wide_on && mkgp2pc_wide_value(2, (u16)index)) {
    return;
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXTexCoord1x16(index);
  }
}
void __real_GXTexCoord1x8(u8 index);
void __wrap_GXTexCoord1x8(u8 index) {
  if (mkgp2pc_wide_on && mkgp2pc_wide_value(1, (u8)index)) {
    return;
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXTexCoord1x8(index);
  }
}
void __real_GXEnd(void);
bool mkgp2pc_wide_begin(GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts);
void mkgp2pc_wide_end(void);
void __wrap_GXEnd(void) {
  if (mkgp2pc_wide_on) {
    const bool kept = mkgp2pc_wide_capture;
    
    if (kept) {
      return;
    }
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXEnd();
    gxlog_sample();
  }
}
void __real_GXBegin(GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts);
void __wrap_GXBegin(GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts) {
  if (!mkgp2pc_skip_draw && !mkgp2pc_wide_begin(type, vtxfmt, nverts)) {
    __real_GXBegin(type, vtxfmt, nverts);
  }
}
static bool s_showHold;
void __real_GXCopyTex(void* dest, GXBool clear);
void __wrap_GXCopyTex(void* dest, GXBool clear) {
  if (!mkgp2pc_skip_draw) {
    sCopyTargets.insert(dest);
    static int show = -1;
    if (show < 0) show = getenv("MKGP2PC_SHOW_COPY") != nullptr;
    if (show) { /* MKGP2PC_SHOW_COPY=1, debugging: the copied image stays on screen instead of the scene */
      gxlog_before_emptying();
      __real_GXCopyTex(dest, GX_FALSE);
      gxlog_after_emptying("GXCopyTex dest %p (shown instead of the scene)", dest);
      mkgp2pc_skip_draw = true;
      s_showHold = true;
      return;
    }
    gxlog_before_emptying();
    __real_GXCopyTex(dest, clear);
    gxlog_after_emptying("GXCopyTex dest %p clear %d", dest, (int)clear);
  }
}
void __real_GXCopyDisp(void* dest, GXBool clear);
void __wrap_GXCopyDisp(void* dest, GXBool clear) {
  if (s_showHold) {
    s_showHold = false;
    mkgp2pc_skip_draw = false;
  }
  if (!mkgp2pc_skip_draw) {
    gxlog_before_emptying();
    __real_GXCopyDisp(dest, clear);
    gxlog_after_emptying("GXCopyDisp dest %p clear %d", dest, (int)clear);
  }
}
}

extern "C" {
void __real_GXCmd1u8(const u8 x);
void __wrap_GXCmd1u8(const u8 x) {
  if (mkgp2pc_wide_on && mkgp2pc_wide_value(1, (u8)x)) {
    return;
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXCmd1u8(x);
  }
}
void __real_GXCmd1u16(const u16 x);
void __wrap_GXCmd1u16(const u16 x) {
  if (mkgp2pc_wide_on && mkgp2pc_wide_value(2, (u16)x)) {
    return;
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXCmd1u16(x);
  }
}
void __real_GXCmd1u32(const u32 x);
void __wrap_GXCmd1u32(const u32 x) {
  if (mkgp2pc_wide_on && mkgp2pc_wide_value(4, (u32)x)) {
    return;
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXCmd1u32(x);
  }
}
void __real_GXCmd1u64(const u64 x);
void __wrap_GXCmd1u64(const u64 x) {
  if (mkgp2pc_wide_on) {
    const bool kept = mkgp2pc_wide_capture;
    mkgp2pc_wide_value(4, (u32)(x >> 32));
    mkgp2pc_wide_value(4, (u32)x);
    if (kept) {
      return;
    }
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXCmd1u64(x);
  }
}
void __real_GXParam1u8(const u8 x);
void __wrap_GXParam1u8(const u8 x) {
  if (mkgp2pc_wide_on && mkgp2pc_wide_value(1, (u8)x)) {
    return;
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXParam1u8(x);
  }
}
void __real_GXParam1u16(const u16 x);
void __wrap_GXParam1u16(const u16 x) {
  if (mkgp2pc_wide_on && mkgp2pc_wide_value(2, (u16)x)) {
    return;
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXParam1u16(x);
  }
}
void __real_GXParam1u32(const u32 x);
void __wrap_GXParam1u32(const u32 x) {
  if (mkgp2pc_wide_on && mkgp2pc_wide_value(4, (u32)x)) {
    return;
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXParam1u32(x);
  }
}
void __real_GXParam1s8(const s8 x);
void __wrap_GXParam1s8(const s8 x) {
  if (mkgp2pc_wide_on && mkgp2pc_wide_value(1, (u8)x)) {
    return;
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXParam1s8(x);
  }
}
void __real_GXParam1s16(const s16 x);
void __wrap_GXParam1s16(const s16 x) {
  if (mkgp2pc_wide_on && mkgp2pc_wide_value(2, (u16)x)) {
    return;
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXParam1s16(x);
  }
}
void __real_GXParam1s32(const s32 x);
void __wrap_GXParam1s32(const s32 x) {
  if (mkgp2pc_wide_on && mkgp2pc_wide_value(4, (u32)x)) {
    return;
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXParam1s32(x);
  }
}
void __real_GXParam1f32(const f32 x);
void __wrap_GXParam1f32(const f32 x) {
  if (mkgp2pc_wide_on && mkgp2pc_wide_value(4, wide_bits(x))) {
    return;
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXParam1f32(x);
  }
}
void __real_GXParam3f32(const f32 x, const f32 y, const f32 z);
void __wrap_GXParam3f32(const f32 x, const f32 y, const f32 z) {
  if (mkgp2pc_wide_on) {
    const bool kept = mkgp2pc_wide_capture;
    mkgp2pc_wide_value(4, wide_bits(x));
    mkgp2pc_wide_value(4, wide_bits(y));
    mkgp2pc_wide_value(4, wide_bits(z));
    if (kept) {
      return;
    }
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXParam3f32(x, y, z);
  }
}
void __real_GXParam4f32(const f32 x, const f32 y, const f32 z, const f32 w);
void __wrap_GXParam4f32(const f32 x, const f32 y, const f32 z, const f32 w) {
  if (mkgp2pc_wide_on) {
    const bool kept = mkgp2pc_wide_capture;
    mkgp2pc_wide_value(4, wide_bits(x));
    mkgp2pc_wide_value(4, wide_bits(y));
    mkgp2pc_wide_value(4, wide_bits(z));
    mkgp2pc_wide_value(4, wide_bits(w));
    if (kept) {
      return;
    }
  }
  if (!mkgp2pc_skip_draw) {
    __real_GXParam4f32(x, y, z, w);
  }
}
}

/*
 * Wide picture (MKGP2PC_ASPECT=16:9, docs/port/widescreen.md).
 *
 * The window and the frame buffer are wider than 4:3 and the rendering
 * library spreads the game's 640x480 over all of it (viewport policy
 * "stretch", video_aurora.c). mkgp2pc_wide_k is how much too wide that
 * makes everything: (width / height) / (4 / 3), 1 when the feature is off.
 *
 * Every projection the game sets while its viewport is the whole screen is
 * narrowed by that factor about the centre. The 4:3 picture then sits
 * undistorted in the middle, and because nothing clips at its edges, the 3D
 * scene simply continues into the side areas: same vertical field of view,
 * more to the left and right. Draws into part of the frame buffer (the kart
 * shadow image, pictures rendered for textures) are not touched.
 *
 * What the game still does for 4:3 and what is done about it:
 *  - Its culling frustum (fn_8013879C, built from the camera's aspect) would
 *    drop objects inside the side areas: the aspect it is given is widened.
 *  - 2D primitives (orthographic projection, vertices written one by one)
 *    are kept until their last vertex has come. A quad (or triangle, or
 *    whole strip) that reaches from the left edge of the 4:3 picture to the
 *    right edge is drawn stretched over the whole width: backgrounds,
 *    fades, the saved frame of a transition. The edge tiles of a tiled
 *    background are repeated outwards. Everything else keeps its shape in
 *    the middle and is clipped to the 4:3 picture (the scissor moves with
 *    it), so parts that the arcade screen hid beyond its edges stay hidden.
 *    Except what is moving across the left or right edge: that is moved
 *    outwards so that it leaves or enters at the real edge of the screen
 *    (mkgp2pc_wide_end).
 *  - Whole-screen images cut into several quads (rows) and models drawn as
 *    display lists under the 2D projection (the dash effect) are stretched
 *    like single full-width quads. The full list of rules and of the code
 *    that draws in screen space: docs/port/widescreen.md, "Census".
 *  - MKGP2PC_TRACE_WIDE=1 prints how each kept primitive was classified.
 *  - GXGetProjectionv gives back what the game set, not the narrowed
 *    matrix: the game saves and restores projections with it, and
 *    CObj_ProjectPoint computes 640x480 positions with it, which then land
 *    where the 2D layer draws them.
 */
extern "C" {
float mkgp2pc_wide_k = 1.0f;
unsigned mkgp2pc_wide_retrace;
bool mkgp2pc_wide_on, mkgp2pc_wide_capture;
void __real_GXSetViewport(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz);
void __real_GXSetViewportJitter(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz, u32 field);
void __real_GXSetCurrentMtx(u32 id);
void __real_GXSetScissor(u32 left, u32 top, u32 wd, u32 ht);
void __real_fn_8013879C(float* frustum, float fovy, float aspect, float zfar, float znear);
}
namespace {
bool sFullView = true;
bool sProjSet;
float sMtx[10][3][4];
u32 sCurMtx;
struct WideValue {
  u8 size;
  u32 bits;
};
std::vector<WideValue> sWide;
u32 sWideBytes, sWideWant;
GXPrimitive sWideType;
GXVtxFmt sWideFmt;
u16 sWideVerts;
int sWidePosOff; /* byte offset of the position in a vertex */
int sWidePosN;   /* 2 or 3 floats */

bool wide_on() { return mkgp2pc_wide_k > 1.001f; }

u32 sScis[4] = {0, 0, 640, 480};

/* The game's scissor is in 640x480 units like everything else. For the 2D
 * layer it is moved with the narrowed picture, so that a clipping window
 * stays on its text and nothing drawn outside the 4:3 picture (parts the
 * arcade screen never showed) appears in the side areas. The 3D scene and
 * the stretched whole-screen draws keep the scissor as the game gave it. */
void apply_scissor(bool narrow, bool open = false) {
  if (open && sScis[0] == 0 && sScis[0] + sScis[2] >= 640) {
    narrow = false;
  }
  if (!narrow) {
    __real_GXSetScissor(sScis[0], sScis[1], sScis[2], sScis[3]);
    return;
  }
  const float s = 1.0f / mkgp2pc_wide_k;
  const float l = std::floor(320.0f + (static_cast<float>(sScis[0]) - 320.0f) * s);
  const float r = std::ceil(320.0f + (static_cast<float>(sScis[0] + sScis[2]) - 320.0f) * s);
  __real_GXSetScissor(static_cast<u32>(l), sScis[1], static_cast<u32>(r - l), sScis[3]);
}

/* shift: added to x after the game's projection (in units of the 4:3
 * picture, 2 is its width), before the narrowing. */
void send_projection(bool narrow, float shift = 0.0f, bool clip = true) {
  float m[4][4];
  std::memcpy(m, sProj, sizeof(m));
  if (narrow) {
    const float s = 1.0f / mkgp2pc_wide_k;
    m[0][3] += shift;
    for (int c = 0; c < 4; c++) {
      m[0][c] *= s;
    }
  }
  __real_GXSetProjection(m, sProjType);
  if (wide_on()) {
    apply_scissor(narrow && sProjType != GX_PERSPECTIVE, !clip);
  }
}

void view_changed(f32 left, f32 top, f32 wd, f32 ht) {
  const bool full = left <= 0.5f && top <= 0.5f && wd >= 639.5f && ht >= 479.5f;
  if (full != sFullView) {
    sFullView = full;
    if (sProjSet && wide_on()) {
      send_projection(full);
    }
  }
}

void wide_replay(size_t first, size_t last, u16 nverts) {
  __real_GXBegin(sWideType, sWideFmt, nverts);
  for (size_t i = first; i < last; i++) {
    const WideValue& v = sWide[i];
    if (v.size == 1) {
      __real_GXParam1u8(static_cast<u8>(v.bits));
    } else if (v.size == 2) {
      __real_GXParam1u16(static_cast<u16>(v.bits));
    } else {
      __real_GXParam1u32(v.bits);
    }
  }
  __real_GXEnd();
}
} // namespace

void mkgp2pc_wide_projection(void) {
  sProjSet = true;
  send_projection(wide_on() && sFullView);
}
void mkgp2pc_wide_posmtx(const void* mtx, u32 id) {
  if (id % 3 == 0 && id / 3 < 10) {
    std::memcpy(sMtx[id / 3], mtx, sizeof(sMtx[0]));
  }
}

extern "C" {
void __wrap_GXSetViewport(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz) {
  view_changed(left, top, wd, ht);
  __real_GXSetViewport(left, top, wd, ht, nearz, farz);
}
void __wrap_GXSetViewportJitter(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz, u32 field) {
  view_changed(left, top, wd, ht);
  __real_GXSetViewportJitter(left, top, wd, ht, nearz, farz, field);
}
void __wrap_GXSetScissor(u32 left, u32 top, u32 wd, u32 ht) {
  sScis[0] = left;
  sScis[1] = top;
  sScis[2] = wd;
  sScis[3] = ht;
  apply_scissor(wide_on() && sFullView && sProjSet && sProjType != GX_PERSPECTIVE);
}
void __wrap_GXGetScissor(u32* left, u32* top, u32* wd, u32* ht) {
  *left = sScis[0];
  *top = sScis[1];
  *wd = sScis[2];
  *ht = sScis[3];
}
void __wrap_GXSetCurrentMtx(u32 id) {
  sCurMtx = id / 3 < 10 ? id / 3 : 0;
  __real_GXSetCurrentMtx(id);
}
/* The game's own values, in the layout of the SDK (type, then six
 * numbers). */
void __wrap_GXGetProjectionv(f32* p) {
  const bool ortho = sProjType != GX_PERSPECTIVE;
  p[0] = ortho ? 1.0f : 0.0f;
  p[1] = sProj[0][0];
  p[2] = ortho ? sProj[0][3] : sProj[0][2];
  p[3] = sProj[1][1];
  p[4] = ortho ? sProj[1][3] : sProj[1][2];
  p[5] = sProj[2][2];
  p[6] = sProj[2][3];
}
void __wrap_GXSetProjectionv(const f32* p) {
  const bool ortho = p[0] != 0.0f;
  std::memset(sProj, 0, sizeof(sProj));
  sProj[0][0] = p[1];
  sProj[0][ortho ? 3 : 2] = p[2];
  sProj[1][1] = p[3];
  sProj[1][ortho ? 3 : 2] = p[4];
  sProj[2][2] = p[5];
  sProj[2][3] = p[6];
  if (ortho) {
    sProj[3][3] = 1.0f;
  } else {
    sProj[3][2] = -1.0f;
  }
  sProjType = ortho ? GX_ORTHOGRAPHIC : GX_PERSPECTIVE;
  wide_census_projection(__builtin_return_address(0));
  mkgp2pc_wide_projection();
}
void __wrap_fn_8013879C(float* frustum, float fovy, float aspect, float zfar, float znear) {
  /* MKGP2PC_WIDE_CULL=x widens the frustum x times more (investigation:
   * does something missing at the sides come back). */
  static float extra = 0.0f;
  if (extra == 0.0f) {
    const char* e = getenv("MKGP2PC_WIDE_CULL");
    extra = e != nullptr && atof(e) > 0 ? static_cast<float>(atof(e)) : 1.0f;
  }
  __real_fn_8013879C(frustum, fovy, aspect * mkgp2pc_wide_k * (mkgp2pc_wide_on ? extra : 1.0f), zfar, znear);
}

}
/*
 * MKGP2PC_WIDE_CENSUS=1: which code sets the projections of a session, and
 * what is drawn under each (docs/port/widescreen.md, "Census"). A line when
 * a projection is first seen and the whole table every 1000 retraces:
 *   census: <caller> ortho|persp <m00> <m11> full|part: frames N first A
 *   last B kept K stretched S moved M tiles T passed P lists L
 * <caller> is the offset of the call in the executable (addr2line -f -e).
 * kept, stretched, moved, tiles: 2D primitives by how mkgp2pc_wide_end
 * treated them; passed: primitives it did not look at; lists: display
 * lists.
 */
#if defined(__linux__)
#include <dlfcn.h>
#endif
namespace {
struct CensusKey {
  uintptr_t caller;
  u32 type;
  int m00, m11;
  bool full;
  bool operator<(const CensusKey& o) const {
    return std::tie(caller, type, m00, m11, full) < std::tie(o.caller, o.type, o.m00, o.m11, o.full);
  }
};
struct CensusRow {
  unsigned frames, first, last, n[6];
};
std::map<CensusKey, CensusRow> sCensus;
uintptr_t sCensusCaller;
bool census_on() {
  static int v = -1;
  if (v < 0) v = getenv("MKGP2PC_WIDE_CENSUS") != nullptr;
  return v != 0;
}
void census_print(const CensusKey& k, const CensusRow& r) {
  fprintf(stderr, "census: %lx %s %.4f %.4f %s: frames %u first %u last %u kept %u stretched %u moved %u tiles %u passed %u lists %u\n",
          static_cast<unsigned long>(k.caller), k.type == GX_PERSPECTIVE ? "persp" : "ortho", k.m00 / 10000.0, k.m11 / 10000.0,
          k.full ? "full" : "part", r.frames, r.first, r.last, r.n[0], r.n[1], r.n[2], r.n[3], r.n[4], r.n[5]);
}
} // namespace
void wide_census_projection(void* caller) {
  if (!census_on()) {
    return;
  }
  uintptr_t a = reinterpret_cast<uintptr_t>(caller);
#if defined(__linux__)
  Dl_info info;
  if (dladdr(caller, &info) != 0) {
    a -= reinterpret_cast<uintptr_t>(info.dli_fbase);
  }
#endif
  sCensusCaller = a;
}
void wide_census_draw(int kind) {
  if (!census_on()) {
    return;
  }
  const CensusKey k{sCensusCaller, sProjType, static_cast<int>(std::lround(sProj[0][0] * 10000)),
                    static_cast<int>(std::lround(sProj[1][1] * 10000)), sFullView};
  auto it = sCensus.find(k);
  if (it == sCensus.end()) {
    it = sCensus.insert({k, CensusRow{}}).first;
    it->second.first = mkgp2pc_wide_retrace;
    it->second.last = ~0u;
    fprintf(stderr, "census: new at retrace %u: ", mkgp2pc_wide_retrace);
    census_print(k, it->second);
  }
  CensusRow& r = it->second;
  if (r.last != mkgp2pc_wide_retrace) {
    r.last = mkgp2pc_wide_retrace;
    r.frames++;
  }
  r.n[kind]++;
}
static void wide_census_frame() {
  static unsigned next = 1000;
  if (census_on() && mkgp2pc_wide_retrace >= next) {
    next = mkgp2pc_wide_retrace + 1000;
    fprintf(stderr, "census: table at retrace %u\n", mkgp2pc_wide_retrace);
    for (const auto& e : sCensus) {
      census_print(e.first, e.second);
    }
  }
}
extern "C" {
void mkgp2pc_wide_end(void);
static u32 wide_vertex_size(GXVtxFmt fmt);
static bool wide_start(GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts);
/* The game also starts primitives by writing the command itself (0x80 and
 * up, then the vertex count) instead of calling GXBegin, and sends register
 * loads the same way. To tell a draw command from data, the commands of the
 * stream are followed: sRawStage says what the next value is, sRawLeft how
 * many bytes of the current command are still to come. A value that does
 * not fit ends the following until the next frame (sRawStage = kRawLost);
 * primitives started with GXBegin are still handled then. */
enum { kRawCommand, kRawData, kRawXfHead, kRawCountKept, kRawCount, kRawLost };
static int sRawStage = kRawCommand;
static u32 sRawLeft;
static u8 sRawDraw;
static bool wide_trace();
bool mkgp2pc_wide_value(unsigned size, u32 bits) {
  if (mkgp2pc_skip_draw) {
    return false; /* a frame that is not shown: the caller drops the value */
  }
  if (sRawStage != kRawLost && wide_trace()) {
    static int was = kRawCommand;
    if (was != kRawLost && sRawStage == kRawLost) fprintf(stderr, "wide: command stream lost\n");
    was = sRawStage;
  }
  if (mkgp2pc_wide_capture) {
    sWide.push_back({static_cast<u8>(size), bits});
    sWideBytes += size;
    /* The game does not always call GXEnd (it is empty in the SDK): the
     * primitive is complete when its last vertex is. */
    if (sWideBytes >= sWideWant) {
      mkgp2pc_wide_end();
    }
    return true;
  }
  switch (sRawStage) {
  case kRawData:
    if (size > sRawLeft) {
      sRawStage = kRawLost;
    } else if ((sRawLeft -= size) == 0) {
      sRawStage = kRawCommand;
    }
    return false;
  case kRawXfHead:
    if (size != 4) {
      sRawStage = kRawLost;
    } else {
      sRawLeft = 4 * (((bits >> 16) & 0xF) + 1);
      sRawStage = kRawData;
    }
    return false;
  case kRawCountKept:
  case kRawCount: {
    const bool kept = sRawStage == kRawCountKept;
    const GXPrimitive type = static_cast<GXPrimitive>(sRawDraw & 0xF8);
    const GXVtxFmt fmt = static_cast<GXVtxFmt>(sRawDraw & 7);
    sRawStage = kRawCommand;
    if (size != 2) {
      sRawStage = kRawLost;
    } else if (kept && bits != 0 && wide_start(type, fmt, static_cast<u16>(bits))) {
      return true;
    } else if (bits != 0) {
      sRawLeft = wide_vertex_size(fmt) * bits;
      sRawStage = sRawLeft != 0 ? kRawData : kRawCommand;
    }
    if (kept) { /* the command was held back: send it after all */
      __real_GXParam1u8(sRawDraw);
    }
    return false;
  }
  case kRawCommand:
    if (size != 1) {
      sRawStage = kRawLost;
      return false;
    }
    if (bits >= 0x80 && bits < 0xC0) {
      if (wide_trace()) fprintf(stderr, "wide: draw command %x written directly (full view %d, projection %u)\n", bits, sFullView, sProjType);
      sRawDraw = static_cast<u8>(bits);
      const bool keep = sFullView && sProjType != GX_PERSPECTIVE;
      sRawStage = keep ? kRawCountKept : kRawCount;
      return keep;
    }
    sRawLeft = 0;
    if (bits == 0x10) {
      sRawStage = kRawXfHead;
    } else if (bits == 0x08) {
      sRawLeft = 5;
    } else if (bits == 0x61 || bits == 0x20 || bits == 0x28 || bits == 0x30 || bits == 0x38) {
      sRawLeft = 4;
    } else if (bits == 0x40) {
      sRawLeft = 8;
    } else if (bits != 0x00 && bits != 0x48) {
      if (wide_trace()) fprintf(stderr, "wide: command stream lost at %x\n", bits);
      sRawStage = kRawLost;
    }
    if (sRawLeft != 0) {
      sRawStage = kRawData;
    }
    return false;
  default:
    return false;
  }
}
/* Called at the start of every frame (video_aurora.c). */
void wide_tiles_frame(void);
void mkgp2pc_wide_frame(float k) {
  wide_tiles_frame();
  wide_census_frame();
  mkgp2pc_wide_k = k;
  mkgp2pc_wide_on = k > 1.001f;
  if (!mkgp2pc_wide_capture) {
    sRawStage = kRawCommand;
    sRawLeft = 0;
  }
}

/* Bytes of one vertex in the stream, from the vertex description. */
static u32 wide_vertex_size(GXVtxFmt fmt) {
  static const u8 compSize[] = {1, 1, 2, 2, 4};
  static const u8 colorSize[] = {2, 3, 4, 2, 3, 4};
  u32 size = 0;
  for (int a = GX_VA_PNMTXIDX; a <= GX_VA_TEX7; a++) {
    GXAttrType t;
    GXGetVtxDesc(static_cast<GXAttr>(a), &t);
    if (t == GX_NONE) {
      continue;
    }
    if (a < GX_VA_POS || t == GX_INDEX8) {
      size += 1;
      continue;
    }
    if (t == GX_INDEX16) {
      size += 2;
      continue;
    }
    GXCompCnt cnt;
    GXCompType comp;
    u8 shift;
    GXGetVtxAttrFmt(fmt, static_cast<GXAttr>(a), &cnt, &comp, &shift);
    const u32 c = static_cast<u32>(comp);
    if (a == GX_VA_CLR0 || a == GX_VA_CLR1) {
      size += c < 6 ? colorSize[c] : 4;
    } else {
      const u32 n = a == GX_VA_POS ? (cnt == GX_POS_XY ? 2 : 3) : a == GX_VA_NRM ? (cnt == GX_NRM_XYZ ? 3 : 9) : (cnt == GX_TEX_S ? 1 : 2);
      size += n * (c < 5 ? compSize[c] : 4);
    }
  }
  return size;
}

static bool wide_trace() {
  static int v = -1;
  if (v < 0) v = getenv("MKGP2PC_TRACE_WIDE") != nullptr;
  return v != 0;
}
bool mkgp2pc_wide_begin(GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts) {
  if (!wide_on()) {
    return false;
  }
  if (mkgp2pc_wide_capture) {
    mkgp2pc_wide_end(); /* an unfinished primitive: sent as it is */
  }
  if (!sFullView || sProjType == GX_PERSPECTIVE || nverts == 0 || nverts == GX_AUTO || !wide_start(type, vtxfmt, nverts)) {
    /* not kept: its vertices pass through as data */
    wide_census_draw(4);
    sRawLeft = nverts == GX_AUTO ? 0 : wide_vertex_size(vtxfmt) * nverts;
    sRawStage = nverts == GX_AUTO ? kRawLost : sRawLeft != 0 ? kRawData : kRawCommand;
    return false;
  }
  return true;
}
static bool wide_start(GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts) {
  /* where the position is in a vertex: after the matrix indices */
  int off = 0;
  for (int a = GX_VA_PNMTXIDX; a < GX_VA_POS; a++) {
    GXAttrType t;
    GXGetVtxDesc(static_cast<GXAttr>(a), &t);
    off += t != GX_NONE;
  }
  GXAttrType posType;
  GXGetVtxDesc(GX_VA_POS, &posType);
  GXCompCnt cnt;
  GXCompType comp;
  u8 shift;
  GXGetVtxAttrFmt(vtxfmt, GX_VA_POS, &cnt, &comp, &shift);
  if (posType != GX_DIRECT || comp != GX_F32) {
    if (wide_trace()) {
      fprintf(stderr, "wide: begin %x n %d not kept (position type %d comp %d)\n", type, nverts, posType, comp);
    }
    return false;
  }
  sWideWant = wide_vertex_size(vtxfmt) * nverts;
  sWidePosOff = off;
  sWidePosN = cnt == GX_POS_XY ? 2 : 3;
  sWideType = type;
  sWideFmt = vtxfmt;
  sWideVerts = nverts;
  sWide.clear();
  sWideBytes = 0;
  mkgp2pc_wide_capture = true;
  return true;
}

/*
 * Backgrounds laid out of tiles (the star pattern behind the menus is a
 * grid of 64x64 quads, one GXBegin each). No single quad spans the picture,
 * so nothing is stretched; instead the tiles of the outermost columns are
 * drawn again further out until the side areas are covered. A tile is a
 * quad that touches the left or right edge of the 4:3 picture exactly and
 * has a neighbour of the same size and height next to it, drawn right
 * before or after it. Whether a quad has such a neighbour is only known
 * after the frame, so the tiles found in one frame are the ones repeated in
 * the next.
 */
struct WideQuad {
  float lo, hi, ylo, yhi;
};
static std::vector<WideQuad> sQuads;
static std::set<std::tuple<int, int, int, int>> sTiles;
static std::set<std::tuple<int, int, int, int>> sRows;
static std::tuple<int, int, int, int> tile_key(const WideQuad& q) {
  return {static_cast<int>(std::lround(q.lo * 500)), static_cast<int>(std::lround(q.hi * 500)),
          static_cast<int>(std::lround(q.ylo * 500)), static_cast<int>(std::lround(q.yhi * 500))};
}
typedef std::tuple<int, int, int, int> WideStillKey;
static std::map<WideStillKey, int> sStillWas, sStillNow;
enum { kStillFrames = 6 };
void wide_tiles_frame(void) {
  if (!sStillNow.empty() || !sQuads.empty()) { /* a frame that drew something */
    sStillWas.swap(sStillNow);
    sStillNow.clear();
  }
  sTiles.clear();
  /* Rows: consecutive quads of the same height that continue each other
   * from the left edge of the 4:3 picture to the right one. A whole-screen
   * image is drawn like that when it is cut into textures (640 = 512 +
   * 128). Its quads are stretched together like a single full-width one.
   * Rows of three or more equal quads are tile grids and left to the tile
   * rule below. */
  sRows.clear();
  for (size_t i = 0; i < sQuads.size();) {
    size_t j = i + 1;
    float lo = sQuads[i].lo, hi = sQuads[i].hi;
    bool equal = true;
    while (j < sQuads.size()) {
      const WideQuad& a = sQuads[j - 1];
      const WideQuad& n = sQuads[j];
      const bool level = std::fabs(n.ylo - a.ylo) < 0.004f && std::fabs(n.yhi - a.yhi) < 0.004f;
      if (!level || !(std::fabs(n.lo - a.hi) < 0.004f || std::fabs(n.hi - a.lo) < 0.004f)) {
        break;
      }
      equal = equal && std::fabs((n.hi - n.lo) - (a.hi - a.lo)) < 0.004f;
      lo = n.lo < lo ? n.lo : lo;
      hi = n.hi > hi ? n.hi : hi;
      j++;
    }
    const bool grid = equal && j - i >= 3 && sQuads[i].hi - sQuads[i].lo <= 0.5f;
    if (j - i >= 2 && !grid && lo <= -0.98f && hi >= 0.98f) {
      for (size_t q = i; q < j; q++) {
        sRows.insert(tile_key(sQuads[q]));
      }
    }
    i = j;
  }
  for (size_t i = 0; i < sQuads.size(); i++) {
    const WideQuad& q = sQuads[i];
    const float w = q.hi - q.lo;
    const bool left = std::fabs(q.lo + 1.0f) < 0.004f, right = std::fabs(q.hi - 1.0f) < 0.004f;
    if (!(left || right) || w > 0.5f || w < 0.02f) {
      continue;
    }
    for (int d = -1; d <= 1; d += 2) {
      if ((d < 0 && i == 0) || (d > 0 && i + 1 == sQuads.size())) {
        continue;
      }
      const WideQuad& n = sQuads[i + d];
      const bool same = std::fabs((n.hi - n.lo) - w) < 0.004f && std::fabs(n.ylo - q.ylo) < 0.004f && std::fabs(n.yhi - q.yhi) < 0.004f;
      if (same && ((left && std::fabs(n.lo - q.hi) < 0.004f) || (right && std::fabs(n.hi - q.lo) < 0.004f))) {
        sTiles.insert(tile_key(q));
      }
    }
  }
  sQuads.clear();
}

void mkgp2pc_wide_end(void) {
  mkgp2pc_wide_capture = false;
  if (sWideBytes != sWideWant) {
    wide_replay(0, sWide.size(), sWideVerts);
    return;
  }
  const u32 vsize = sWideBytes / sWideVerts;
  const u16 per = sWideType == GX_QUADS ? 4 : sWideType == GX_TRIANGLES ? 3 : sWideVerts;
  bool ok = sWideBytes % sWideVerts == 0 && vsize >= static_cast<u32>(sWidePosOff + 4 * sWidePosN) &&
            sWideVerts % per == 0;
  /* first value of each vertex, and of its position */
  std::vector<u32> start(sWideVerts + 1u, 0), pos(sWideVerts, 0);
  if (ok) {
    u32 byte = 0, v = 0;
    for (u32 i = 0; i < sWide.size() && ok; i++) {
      const u32 in = byte - v * vsize;
      if (in == 0) {
        start[v] = i;
      }
      if (in == static_cast<u32>(sWidePosOff)) {
        pos[v] = i;
        for (int c = 0; c < sWidePosN; c++) {
          ok = ok && i + c < sWide.size() && sWide[i + c].size == 4;
        }
      }
      byte += sWide[i].size;
      if (byte > (v + 1) * vsize) {
        ok = false; /* a value across two vertices */
      } else if (byte == (v + 1) * vsize) {
        v++;
      }
    }
    start[sWideVerts] = static_cast<u32>(sWide.size());
  }
  if (!ok) {
    wide_replay(0, sWide.size(), sWideVerts);
    return;
  }
  const float (*mv)[4] = sMtx[sCurMtx];
  const int groups = sWideVerts / per;
  /* 0: keeps its place, 1: stretched over the whole width, 2: moved outwards */
  std::vector<u8> kind(groups + 1u, 0);
  float outLo = 1e9f, outHi = -1e9f, outYlo = 1e9f, outYhi = -1e9f;
  for (int g = 0; g < groups; g++) {
    float lo = 1e9f, hi = -1e9f, ylo = 1e9f, yhi = -1e9f;
    for (int v = g * per; v < (g + 1) * per; v++) {
      float p[3] = {0, 0, 0};
      for (int c = 0; c < sWidePosN; c++) {
        std::memcpy(&p[c], &sWide[pos[v] + c].bits, 4);
      }
      const float ex = mv[0][0] * p[0] + mv[0][1] * p[1] + mv[0][2] * p[2] + mv[0][3];
      const float ey = mv[1][0] * p[0] + mv[1][1] * p[1] + mv[1][2] * p[2] + mv[1][3];
      const float x = sProj[0][0] * ex + sProj[0][1] * ey + sProj[0][3];
      const float y = sProj[1][0] * ex + sProj[1][1] * ey + sProj[1][3];
      lo = x < lo ? x : lo;
      hi = x > hi ? x : hi;
      ylo = y < ylo ? y : ylo;
      yhi = y > yhi ? y : yhi;
    }
    if (per == 4 && groups == 1 && sQuads.size() < 4096) {
      const WideQuad q{lo, hi, ylo, yhi};
      sQuads.push_back(q);
      if (sTiles.count(tile_key(q)) != 0) {
        /* an edge tile of a background grid: draw it, then its copies */
        wide_census_draw(3);
        wide_replay(start[0], start[per], per);
        const float w = hi - lo, dir = lo < 0 ? -1.0f : 1.0f;
        for (int n = 1; n < 32 && 1.0f + (n - 1) * w < mkgp2pc_wide_k; n++) {
          send_projection(true, dir * n * w, false);
          wide_replay(start[0], start[per], per);
        }
        send_projection(true);
        return;
      }
    }
    /* As wide as the picture: stretched, wherever it is, so that a band
     * or a curtain that slides away keeps covering up to the real edge. */
    kind[g] = hi - lo >= 1.96f || (per == 4 && groups == 1 && sRows.count(tile_key(WideQuad{lo, hi, ylo, yhi})) != 0);
    if (kind[g] == 0) {
      outLo = lo < outLo ? lo : outLo;
      outHi = hi > outHi ? hi : outHi;
      outYlo = ylo < outYlo ? ylo : outYlo;
      outYhi = yhi > outYhi ? yhi : outYhi;
    }
    if (wide_trace()) {
      fprintf(stderr, "wide: %x n %d group %d x %.3f..%.3f mtx %u%s\n", sWideType, sWideVerts, g, lo, hi, sCurMtx, kind[g] ? " FULL" : "");
    }
  }
  /* Something narrower that reaches beyond the left or right edge of the
   * 4:3 picture is on its way out of (or into) the screen, or parked
   * outside. It is moved outwards by the share of it that is beyond the
   * edge, times the width of a side area: unchanged while it is inside,
   * just outside the real edge when it is just outside the 4:3 one. The
   * primitive moves as a whole.
   * Only while it moves, though. Parts of the screen layout that rest
   * across the edge (the left end of a title bar, made of several pieces)
   * would come apart, so a primitive that has been in the same place for
   * kStillFrames frames keeps its place and is clipped at the 4:3 edge. */
  float shift = 0.0f;
  if (outHi > outLo && (outHi > 1.0f || outLo < -1.0f)) {
    const float w = outHi - outLo;
    float over = (outHi > 1.0f ? outHi - 1.0f : 0.0f) - (outLo < -1.0f ? -1.0f - outLo : 0.0f);
    over = over > w ? w : over < -w ? -w : over;
    shift = (mkgp2pc_wide_k - 1.0f) * over / w;
    const WideStillKey key{static_cast<int>(std::lround(outLo * 4000)), static_cast<int>(std::lround(outHi * 4000)),
                           static_cast<int>(std::lround(outYlo * 4000)), static_cast<int>(std::lround(outYhi * 4000))};
    const auto was = sStillWas.find(key);
    const int frames = was != sStillWas.end() ? was->second + 1 : 1;
    int& now = sStillNow[key];
    now = frames > now ? frames : now;
    if (frames > kStillFrames) {
      shift = 0.0f;
    }
    if (wide_trace()) {
      fprintf(stderr, "wide: moved by %.3f (x %.3f..%.3f) retrace %u\n", shift, outLo, outHi, mkgp2pc_wide_retrace);
    }
  }
  if (shift != 0.0f) {
    for (int g = 0; g < groups; g++) {
      kind[g] = kind[g] == 0 ? 2 : kind[g];
    }
  }
  int runFirst = 0;
  for (int g = 1; g <= groups; g++) {
    if (g < groups && kind[g] == kind[runFirst]) {
      continue;
    }
    const u8 k = kind[runFirst];
    wide_census_draw(k);
    if (k == 1) {
      send_projection(false);
    } else if (k == 2) {
      send_projection(true, shift, false);
    }
    wide_replay(start[runFirst * per], start[g * per], static_cast<u16>((g - runFirst) * per));
    if (k != 0) {
      send_projection(true);
    }
    runFirst = g;
  }
}
}

/*
 * Display lists under the 2D projection: models the game draws into its
 * overlay (screen effects made as models rather than sprites). The list is
 * read for the positions it draws, as far as it only holds draw commands,
 * and treated as one primitive: as wide as the 4:3 picture, stretched.
 */
static bool wide_list_extent(const u8* p, u32 n, float& lo, float& hi) {
  GXAttrType posType, mtxType;
  GXGetVtxDesc(GX_VA_POS, &posType);
  GXGetVtxDesc(GX_VA_PNMTXIDX, &mtxType);
  int off = 0;
  for (int a = GX_VA_PNMTXIDX; a < GX_VA_POS; a++) {
    GXAttrType t;
    GXGetVtxDesc(static_cast<GXAttr>(a), &t);
    off += t != GX_NONE;
  }
  if (posType == GX_NONE || (posType != GX_DIRECT && mkgp2pc_wide_pos_array == nullptr)) {
    return false;
  }
  lo = 1e9f;
  hi = -1e9f;
  u32 i = 0;
  while (i < n) {
    const u8 op = p[i];
    if (op == 0) {
      i++;
      continue;
    }
    if (op < 0x80 || op >= 0xC0 || i + 3 > n) {
      return false;
    }
    const GXVtxFmt fmt = static_cast<GXVtxFmt>(op & 7);
    const u32 count = (p[i + 1] << 8) | p[i + 2];
    const u32 vsize = wide_vertex_size(fmt);
    i += 3;
    if (vsize == 0 || i + count * vsize > n) {
      return false;
    }
    GXCompCnt cnt;
    GXCompType comp;
    u8 shift;
    GXGetVtxAttrFmt(fmt, GX_VA_POS, &cnt, &comp, &shift);
    const int ncomp = cnt == GX_POS_XY ? 2 : 3;
    for (u32 v = 0; v < count; v++, i += vsize) {
      const u8* at = p + i + off;
      if (posType == GX_INDEX8) {
        at = mkgp2pc_wide_pos_array + at[0] * mkgp2pc_wide_pos_stride;
      } else if (posType == GX_INDEX16) {
        at = mkgp2pc_wide_pos_array + ((at[0] << 8) | at[1]) * mkgp2pc_wide_pos_stride;
      }
      float c[3] = {0, 0, 0};
      for (int k = 0; k < ncomp; k++) {
        if (comp == GX_F32) {
          const u32 bits = (at[0] << 24) | (at[1] << 16) | (at[2] << 8) | at[3];
          std::memcpy(&c[k], &bits, 4);
          at += 4;
        } else if (comp == GX_S16 || comp == GX_U16) {
          const u16 raw = static_cast<u16>((at[0] << 8) | at[1]);
          c[k] = (comp == GX_S16 ? static_cast<float>(static_cast<s16>(raw)) : static_cast<float>(raw)) / static_cast<float>(1 << shift);
          at += 2;
        } else {
          c[k] = (comp == GX_S8 ? static_cast<float>(static_cast<s8>(at[0])) : static_cast<float>(at[0])) / static_cast<float>(1 << shift);
          at += 1;
        }
      }
      u32 m = sCurMtx;
      if (mtxType != GX_NONE) {
        m = p[i] / 3;
        if (m >= 10) {
          return false;
        }
      }
      const float (*mv)[4] = sMtx[m];
      const float ex = mv[0][0] * c[0] + mv[0][1] * c[1] + mv[0][2] * c[2] + mv[0][3];
      const float ey = mv[1][0] * c[0] + mv[1][1] * c[1] + mv[1][2] * c[2] + mv[1][3];
      const float x = sProj[0][0] * ex + sProj[0][1] * ey + sProj[0][3];
      lo = x < lo ? x : lo;
      hi = x > hi ? x : hi;
    }
  }
  return hi >= lo;
}
bool wide_list_before(const void* list, u32 nbytes) {
  if (!wide_on() || !sFullView || sProjType == GX_PERSPECTIVE) {
    return false;
  }
  float lo, hi;
  const bool ok = wide_list_extent(static_cast<const u8*>(list), nbytes, lo, hi);
  if (wide_trace()) {
    if (ok) {
      fprintf(stderr, "wide: list %u bytes x %.3f..%.3f retrace %u\n", nbytes, lo, hi, mkgp2pc_wide_retrace);
    } else {
      fprintf(stderr, "wide: list %u bytes not read, retrace %u\n", nbytes, mkgp2pc_wide_retrace);
    }
  }
  if (!ok || hi - lo < 1.96f) {
    return false;
  }
  send_projection(false);
  return true;
}
void wide_list_after(void) { send_projection(true); }

/*
 * High-resolution 2D textures (docs/port/hires_textures.md).
 *
 * Aurora can stand in an image file for a texture the game loads, found by
 * the texture's size, format and a hash of its texels (the file naming of
 * Dolphin's custom textures). The files are made on the user's machine from
 * their own disc image (port/hires) and are never part of this repository.
 * Off unless MKGP2PC_HIRES=1; MKGP2PC_HIRES_DIR names the folder ("hires"
 * under the launch folder otherwise). The key action "hires" (F8) switches
 * while the game runs, for comparing.
 */
namespace {
aurora::texture::ReplacementGroup sHiresGroup;
bool sHiresOn;
const char* hires_dir() {
  const char* dir = getenv("MKGP2PC_HIRES_DIR");
  return dir != nullptr && *dir != 0 ? dir : "hires";
}
void hires_set(bool on) {
  if (on == sHiresOn) {
    return;
  }
  sHiresOn = on;
  if (on) {
    std::error_code ec;
    if (!std::filesystem::is_directory(hires_dir(), ec)) {
      fprintf(stderr, "hires: no folder '%s' (make it with mkgp2-hires, docs/port/hires_textures.md)\n", hires_dir());
      sHiresOn = false;
      return;
    }
    sHiresGroup = aurora::texture::load_replacement_directory(hires_dir());
    fprintf(stderr, "hires: on, %u textures from '%s'\n", static_cast<unsigned>(sHiresGroup.registrations.size()),
            hires_dir());
  } else {
    aurora::texture::unregister_replacements(sHiresGroup);
    sHiresGroup.registrations.clear();
    fprintf(stderr, "hires: off\n");
  }
}
} // namespace

extern "C" {
/* after aurora_initialize */
void mkgp2pc_hires_init(void) {
  const char* e = getenv("MKGP2PC_HIRES");
  hires_set(e != nullptr && *e != 0 && strcmp(e, "0") != 0);
}
void mkgp2pc_hires_toggle(void) { hires_set(!sHiresOn); }
/* the menu's setting (port_settings.h) */
int mkgp2pc_hires_on(void) { return sHiresOn; }
void mkgp2pc_hires_set(int on) { hires_set(on != 0); }
}
