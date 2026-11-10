/* Byte-order conversion of an HSD archive (.dat), in place, before the game
 * relocates it.
 *
 * The relocation table names every pointer field of the data block, so those
 * are swapped first. The other fields are typed by walking the object graph
 * from the public symbols ("scene_data" and the "*_joint" names; the game's
 * 1,917 archives have no other kind) with the sysdolphin descriptor layouts
 * (the headers under libs/melee/src/sysdolphin/baselib in the game tree; same library).
 * Blocks are shared, so each is converted once.
 *
 * A Python prototype of this walk was checked against every archive on the
 * disc: the pointer fields it expects are exactly the relocation entries,
 * apart from blocks nothing points at (docs/port/census.md section 15).
 *
 * Left big-endian on purpose, for the GX layer and the code that feeds it:
 * vertex buffers, display lists, texture images and palettes, shape index
 * lists. Animation track data (HSD_FObjDesc.ad) is a byte stream.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mkgp2pc/data.h>

/* The game's Archive (src/game/DVDLoad_OpenAllocReadRelocSync.c). */
typedef struct {
  uint32_t file_size, data_size, nb_reloc, nb_public, nb_extern, pad[3];
  uint8_t* data;
  uint32_t* reloc_info;
  uint32_t* public_info;
  uint32_t* extern_info;
  char* symbols;
} Archive;

typedef struct {
  uint8_t* d;
  uint32_t size;
  uint8_t* map; /* per byte: 1 = block converted, 2 = pointer field */
  unsigned problems;
} Ctx;

#define NONE 0xFFFFFFFFu

static uint32_t bswap32(uint32_t v) {
  return v >> 24 | (v >> 8 & 0xFF00) | (v << 8 & 0xFF0000) | v << 24;
}
static void s32(Ctx* c, uint32_t o) {
  uint32_t v;
  memcpy(&v, c->d + o, 4);
  v = bswap32(v);
  memcpy(c->d + o, &v, 4);
}
static void s32n(Ctx* c, uint32_t o, uint32_t n) {
  while (n--) {
    s32(c, o);
    o += 4;
  }
}
static void s16(Ctx* c, uint32_t o) {
  uint8_t t = c->d[o];
  c->d[o] = c->d[o + 1];
  c->d[o + 1] = t;
}
/* fields before they are converted */
static uint32_t be32(Ctx* c, uint32_t o) { return mkgp2pc_be32(c->d + o); }
static uint16_t be16(Ctx* c, uint32_t o) { return mkgp2pc_be16(c->d + o); }

/* A pointer field (already in host order): the offset it holds, or NONE. */
static uint32_t ptr(Ctx* c, uint32_t o) {
  uint32_t v;
  if (o + 4 > c->size || !(c->map[o] & 2)) {
    return NONE;
  }
  memcpy(&v, c->d + o, 4);
  return v;
}
/* First visit of a block of `size` bytes? */
static int enter(Ctx* c, uint32_t o, uint32_t size) {
  if (o == NONE) {
    return 0;
  }
  if (o > c->size || size > c->size - o) {
    c->problems++;
    return 0;
  }
  if (c->map[o] & 1) {
    return 0;
  }
  c->map[o] |= 1;
  return 1;
}

static void joint(Ctx* c, uint32_t o);
static void aobj(Ctx* c, uint32_t o);

static void floats(Ctx* c, uint32_t o, uint32_t n) {
  if (enter(c, o, n * 4)) {
    s32n(c, o, n);
  }
}

static void robj(Ctx* c, uint32_t o) {
  while (enter(c, o, 12)) {
    uint32_t next = ptr(c, o);
    uint32_t type = be32(c, o + 4) & 0x70000000;
    s32(c, o + 4);
    if (type == 0x20000000) { /* limit: a float */
      s32(c, o + 8);
    } else if (type == 0x40000000) { /* IK hint: bone length, rotation */
      floats(c, ptr(c, o + 8), 2);
    } else if (type != 0x10000000) { /* expression / bytecode: not in this game's data */
      c->problems++;
    }
    o = next;
  }
}

/* Lists are entered element by element: a shape set may point into the
 * middle of the vertex description list its PObj already went through. */
static void vtxdesc(Ctx* c, uint32_t o) {
  while (enter(c, o, 0x18)) {
    uint32_t attr = be32(c, o);
    s32n(c, o, 4);
    s16(c, o + 0x12);
    if (attr == 0xFF) { /* GX_VA_NULL */
      return;
    }
    o += 0x18;
  }
}

static void envelope(Ctx* c, uint32_t o) {
  while (enter(c, o, 8)) {
    s32(c, o + 4); /* weight */
    if (ptr(c, o) == NONE) {
      return;
    }
    o += 8;
  }
}

static void pobj(Ctx* c, uint32_t o) {
  while (enter(c, o, 0x18)) {
    uint32_t next = ptr(c, o + 4);
    uint32_t type = be16(c, o + 12) & 0x3000;
    uint32_t u = ptr(c, o + 20);
    vtxdesc(c, ptr(c, o + 8));
    s16(c, o + 12);
    s16(c, o + 14);
    if (type == 0x1000 && enter(c, u, 0x1C)) { /* shape set */
      s16(c, u);
      s16(c, u + 2);
      s32(c, u + 4);
      s32(c, u + 0x10);
      vtxdesc(c, ptr(c, u + 8));
      vtxdesc(c, ptr(c, u + 20));
    } else if (type == 0x2000 && enter(c, u, 4)) { /* envelope list */
      uint32_t e;
      for (; (e = ptr(c, u)) != NONE; u += 4) {
        envelope(c, e);
      }
    }
    o = next;
  }
}

static void image(Ctx* c, uint32_t o) {
  if (enter(c, o, 0x18)) {
    s16(c, o + 4);
    s16(c, o + 6);
    s32n(c, o + 8, 4);
  }
}
static void tlut(Ctx* c, uint32_t o) {
  if (enter(c, o, 0x10)) {
    s32n(c, o + 4, 2);
    s16(c, o + 12);
  }
}

static void tobj(Ctx* c, uint32_t o) {
  while (enter(c, o, 0x5C)) {
    uint32_t next = ptr(c, o + 4);
    uint32_t t;
    s32n(c, o + 8, 13); /* id, src, rotate, scale, translate, wrap_s, wrap_t */
    s32n(c, o + 0x40, 3); /* flags, blending, mag filter */
    image(c, ptr(c, o + 0x4C));
    tlut(c, ptr(c, o + 0x50));
    if (enter(c, t = ptr(c, o + 0x54), 0x10)) { /* LOD */
      s32n(c, t, 2);
      s32(c, t + 12);
    }
    if (enter(c, t = ptr(c, o + 0x58), 0x20)) { /* TEV */
      s32(c, t + 0x1C);
    }
    o = next;
  }
}

static void dobj(Ctx* c, uint32_t o) {
  while (enter(c, o, 0x10)) {
    uint32_t next = ptr(c, o + 4);
    uint32_t m = ptr(c, o + 8);
    if (enter(c, m, 0x18)) {
      uint32_t mat = ptr(c, m + 12);
      s32(c, m + 4); /* render mode */
      tobj(c, ptr(c, m + 8));
      if (enter(c, mat, 0x14)) {
        s32n(c, mat + 12, 2); /* alpha, shininess */
      }
    }
    pobj(c, ptr(c, o + 12));
    o = next;
  }
}

static void spline(Ctx* c, uint32_t o) {
  if (enter(c, o, 0x18)) {
    uint32_t type = c->d[o];
    uint32_t n = be16(c, o + 2);
    s16(c, o + 2);
    s32(c, o + 4);
    s32(c, o + 12);
    floats(c, ptr(c, o + 8), (n + (type != 0 ? 2 : 0)) * 3);
    floats(c, ptr(c, o + 0x10), n);
    if (n > 0) {
      floats(c, ptr(c, o + 0x14), (n - 1) * 5);
    }
  }
}

static void joint(Ctx* c, uint32_t o) {
  while (enter(c, o, 0x40)) {
    uint32_t flags = be32(c, o + 4);
    uint32_t next = ptr(c, o + 12);
    uint32_t u = ptr(c, o + 16);
    s32(c, o + 4);
    s32n(c, o + 0x14, 9); /* rotation, scale, position */
    joint(c, ptr(c, o + 8));
    if (flags & (1u << 14)) {
      spline(c, u);
    } else if (flags & (1u << 5)) {
      c->problems++; /* particle joint: not in this game's data */
    } else {
      dobj(c, u);
    }
    floats(c, ptr(c, o + 0x38), 12); /* inverse bind matrix */
    robj(c, ptr(c, o + 0x3C));
    o = next;
  }
}

static void aobj(Ctx* c, uint32_t o) {
  if (enter(c, o, 0x10)) {
    uint32_t f = ptr(c, o + 8);
    s32n(c, o, 2); /* flags, end frame */
    while (enter(c, f, 0x14)) {
      uint32_t next = ptr(c, f);
      s32n(c, f + 4, 2); /* length, start frame */
      f = next;
    }
    joint(c, ptr(c, o + 12)); /* the object animated along a path, if any */
  }
}

static void robjanim(Ctx* c, uint32_t o) {
  while (enter(c, o, 8)) {
    uint32_t next = ptr(c, o);
    aobj(c, ptr(c, o + 4));
    o = next;
  }
}

static void animjoint(Ctx* c, uint32_t o) {
  while (enter(c, o, 0x14)) {
    uint32_t next = ptr(c, o + 4);
    s32(c, o + 0x10);
    animjoint(c, ptr(c, o));
    aobj(c, ptr(c, o + 8));
    robjanim(c, ptr(c, o + 12));
    o = next;
  }
}

static void texanim(Ctx* c, uint32_t o) {
  while (enter(c, o, 0x18)) {
    uint32_t next = ptr(c, o);
    uint32_t ni = be16(c, o + 0x14), nt = be16(c, o + 0x16);
    uint32_t t, i;
    s32(c, o + 4);
    s16(c, o + 0x14);
    s16(c, o + 0x16);
    aobj(c, ptr(c, o + 8));
    if (enter(c, t = ptr(c, o + 12), ni * 4)) {
      for (i = 0; i < ni; i++) {
        image(c, ptr(c, t + i * 4));
      }
    }
    if (enter(c, t = ptr(c, o + 16), nt * 4)) {
      for (i = 0; i < nt; i++) {
        tlut(c, ptr(c, t + i * 4));
      }
    }
    o = next;
  }
}

static void matanimjoint(Ctx* c, uint32_t o) {
  while (enter(c, o, 12)) {
    uint32_t next = ptr(c, o + 4);
    uint32_t m = ptr(c, o + 8);
    matanimjoint(c, ptr(c, o));
    while (enter(c, m, 16)) {
      uint32_t mnext = ptr(c, m);
      aobj(c, ptr(c, m + 4));
      texanim(c, ptr(c, m + 8));
      if (ptr(c, m + 12) != NONE) {
        c->problems++; /* render animation: not in this game's data */
      }
      m = mnext;
    }
    o = next;
  }
}

static void shapeanimjoint(Ctx* c, uint32_t o) {
  while (enter(c, o, 12)) {
    uint32_t next = ptr(c, o + 4);
    uint32_t s = ptr(c, o + 8);
    shapeanimjoint(c, ptr(c, o));
    while (enter(c, s, 8)) {
      uint32_t snext = ptr(c, s);
      uint32_t a = ptr(c, s + 4);
      while (enter(c, a, 8)) {
        uint32_t anext = ptr(c, a);
        aobj(c, ptr(c, a + 4));
        a = anext;
      }
      s = snext;
    }
    o = next;
  }
}

static void wobj(Ctx* c, uint32_t o) {
  if (enter(c, o, 0x14)) {
    s32n(c, o + 4, 3);
    robj(c, ptr(c, o + 0x10));
  }
}
static void wobjanim(Ctx* c, uint32_t o) {
  if (enter(c, o, 8)) {
    aobj(c, ptr(c, o));
    robjanim(c, ptr(c, o + 4));
  }
}

static void camera(Ctx* c, uint32_t o) {
  if (enter(c, o, 0x38)) {
    int i;
    if (be16(c, o + 6) != 1) {
      c->problems++; /* only perspective cameras are in this game's data */
    }
    for (i = 4; i < 0x18; i += 2) { /* flags, projection, viewport, scissor */
      s16(c, (uint32_t)(o + i));
    }
    s32(c, o + 0x20); /* roll */
    s32n(c, o + 0x28, 4); /* near, far, field of view, aspect */
    wobj(c, ptr(c, o + 0x18));
    wobj(c, ptr(c, o + 0x1C));
    floats(c, ptr(c, o + 0x24), 3); /* up vector */
  }
}

static void light(Ctx* c, uint32_t o) {
  while (enter(c, o, 0x1C)) {
    uint32_t next = ptr(c, o + 4);
    uint32_t type = be16(c, o + 8) & 3; /* ambient, infinite, point, spot */
    uint32_t attn = be16(c, o + 10) & 1;
    s16(c, o + 8);
    s16(c, o + 10);
    wobj(c, ptr(c, o + 0x10));
    wobj(c, ptr(c, o + 0x14));
    floats(c, ptr(c, o + 0x18), type < 2 ? 1 : attn ? 6 : 5);
    o = next;
  }
}

static void scene(Ctx* c, uint32_t o) {
  uint32_t list, e, t;
  if (!enter(c, o, 16)) {
    return;
  }
  if (enter(c, list = ptr(c, o), 4)) { /* model sets */
    for (; (e = ptr(c, list)) != NONE; list += 4) {
      if (enter(c, e, 16)) {
        joint(c, ptr(c, e));
        if (enter(c, t = ptr(c, e + 4), 4)) {
          for (; ptr(c, t) != NONE; t += 4) {
            animjoint(c, ptr(c, t));
          }
        }
        if (enter(c, t = ptr(c, e + 8), 4)) {
          for (; ptr(c, t) != NONE; t += 4) {
            matanimjoint(c, ptr(c, t));
          }
        }
        if (enter(c, t = ptr(c, e + 12), 4)) {
          for (; ptr(c, t) != NONE; t += 4) {
            shapeanimjoint(c, ptr(c, t));
          }
        }
      }
    }
  }
  if (enter(c, e = ptr(c, o + 4), 8)) { /* camera and its animations */
    camera(c, ptr(c, e));
    if (enter(c, t = ptr(c, e + 4), 4)) {
      uint32_t a;
      for (; (a = ptr(c, t)) != NONE; t += 4) {
        if (enter(c, a, 12)) {
          aobj(c, ptr(c, a));
          wobjanim(c, ptr(c, a + 4));
          wobjanim(c, ptr(c, a + 8));
        }
      }
    }
  }
  if (enter(c, list = ptr(c, o + 8), 4)) { /* lights and their animations */
    for (; (e = ptr(c, list)) != NONE; list += 4) {
      if (enter(c, e, 8)) {
        light(c, ptr(c, e));
        if (enter(c, t = ptr(c, e + 4), 4)) {
          uint32_t a;
          for (; (a = ptr(c, t)) != NONE; t += 4) {
            while (enter(c, a, 16)) {
              uint32_t anext = ptr(c, a);
              aobj(c, ptr(c, a + 4));
              wobjanim(c, ptr(c, a + 8));
              wobjanim(c, ptr(c, a + 12));
              a = anext;
            }
          }
        }
      }
    }
  }
  if (enter(c, e = ptr(c, o + 12), 8)) { /* fog and its animations (two archives have one) */
    uint32_t f = ptr(c, e);
    if (enter(c, f, 0x14)) {
      s32(c, f);
      s32n(c, f + 8, 2); /* start, end */
      if (enter(c, t = ptr(c, f + 4), 0x48)) { /* fog adjust */
        s32(c, t);
        s16(c, t + 4);
        s16(c, t + 6);
        s32n(c, t + 8, 16);
      }
    }
    if (enter(c, t = ptr(c, e + 4), 4)) {
      uint32_t a;
      for (; (a = ptr(c, t)) != NONE; t += 4) {
        if (enter(c, a, 4)) {
          aobj(c, ptr(c, a));
        }
      }
    }
  }
}

void mkgp2pc_hsd_archive_header(void* header) {
  Ctx c = {(uint8_t*)header, 0x20, NULL, 0};
  s32n(&c, 0, 5);
}

void mkgp2pc_hsd_archive(void* archive) {
  Archive* a = (Archive*)archive;
  Ctx c = {a->data, a->data_size, NULL, 0};
  uint32_t i;
  size_t len;
  if (a->data == NULL) {
    return;
  }
  c.map = (uint8_t*)calloc(a->data_size + 1, 1);
  if (c.map == NULL) {
    return;
  }
  for (i = 0; i < a->nb_reloc; i++) {
    uint32_t o = a->reloc_info[i] = bswap32(a->reloc_info[i]);
    if (o + 4 <= c.size && !(c.map[o] & 2)) {
      c.map[o] |= 2;
      s32(&c, o);
    }
  }
  for (i = 0; i < a->nb_public * 2; i++) {
    a->public_info[i] = bswap32(a->public_info[i]);
  }
  for (i = 0; i < a->nb_extern * 2; i++) {
    a->extern_info[i] = bswap32(a->extern_info[i]);
  }
  for (i = 0; i < a->nb_public; i++) {
    const char* name = a->symbols + a->public_info[i * 2 + 1];
    if (strcmp(name, "scene_data") == 0) {
      scene(&c, a->public_info[i * 2]);
    }
  }
  for (i = 0; i < a->nb_public; i++) {
    const char* name = a->symbols + a->public_info[i * 2 + 1];
    len = strlen(name);
    if (len >= 6 && strcmp(name + len - 6, "_joint") == 0) {
      joint(&c, a->public_info[i * 2]);
    } else if (strcmp(name, "scene_data") != 0) {
      c.problems++;
    }
  }
  if (c.problems) {
    fprintf(stderr, "mkgp2pc: HSD archive: %u things this converter does not know\n", c.problems);
  }
  free(c.map);
}
