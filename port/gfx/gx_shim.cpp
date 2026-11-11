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

#include <cstdint>
#include <cstring>
#include <map>
#include <tuple>

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

void __wrap_GXLoadTexObj(GameTexObj* o, u32 id) {
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
  GXInitTexObjWrapMode(real, static_cast<GXTexWrapMode>(o->wrapS), static_cast<GXTexWrapMode>(o->wrapT));
  if (o->flags & kLod) {
    __real_GXInitTexObjLOD(real, static_cast<GXTexFilter>(o->minFilt), static_cast<GXTexFilter>(o->magFilt), o->minLod,
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

extern "C" void __real_HSD_ShadowSetSize(void* shadow, u16 width, u16 height);
extern "C" void __wrap_HSD_ShadowSetSize(void* shadow, u16 width, u16 height) {
  const u16 limit = 480;
  __real_HSD_ShadowSetSize(shadow, width > limit ? limit : width, height > limit ? limit : height);
}
