/*
 * Support routines the MWCC-compiled game calls by name (the compiler emits
 * calls to them for `new T[n]`, member arrays, static objects and so on).
 * The decompiled sources keep those calls, so the port provides the
 * routines. Behaviour follows the compiler's documented conventions:
 * constructors and destructors take a hidden second argument (1: complete
 * object; destructor -1: destroy without freeing), and `new[]` blocks carry
 * a 16-byte header {element size, count}.
 *
 * The game's allocator is behind operator new / delete: Alloc and
 * MemoryManager_TimedFree (tools/extab_user_renames.json).
 */
#include <stddef.h>
#include <stdint.h>

void* Alloc(unsigned long size);
void MemoryManager_TimedFree(void* p);

typedef void* (*CtorDtor)(void*, int);

void __construct_array(void* ptr, CtorDtor ctor, CtorDtor dtor, uint32_t size, uint32_t n) {
  (void)dtor; /* only used to unwind when a constructor throws */
  char* p = (char*)ptr;
  for (uint32_t i = 0; i < n; i++, p += size) {
    ctor(p, 1);
  }
}

void __destroy_arr(void* block, CtorDtor dtor, uint32_t size, uint32_t n) {
  char* p = (char*)block + size * n;
  for (; n > 0; n--) {
    p -= size;
    dtor(p, -1);
  }
}

void* __construct_new_array(void* block, CtorDtor ctor, CtorDtor dtor, uint32_t size, uint32_t n) {
  (void)dtor;
  char* p = (char*)block;
  if (p != NULL) {
    ((uint32_t*)p)[0] = size;
    ((uint32_t*)p)[1] = n;
    p += 16;
    if (ctor != NULL) {
      char* q = p;
      for (uint32_t i = 0; i < n; i++, q += size) {
        ctor(q, 1);
      }
    }
  }
  return p;
}

void __destroy_new_array(void* block, CtorDtor dtor) {
  if (block == NULL) {
    return;
  }
  uint32_t* head = (uint32_t*)((char*)block - 16);
  if (dtor != NULL) {
    uint32_t size = head[0];
    uint32_t n = head[1];
    char* p = (char*)block + size * n;
    for (uint32_t i = 0; i < n; i++) {
      p -= size;
      dtor(p, -1);
    }
  }
  MemoryManager_TimedFree(head);
}

void* __copy(char* dest, char* src, uint32_t size) {
  if (dest != NULL) {
    for (uint32_t i = 0; i < size; i++) {
      dest[i] = src[i];
    }
  }
  return dest;
}

/* static objects with destructors: kept on a list, never run (the game
 * does not return from main in normal operation) */
typedef struct DestructorChain {
  struct DestructorChain* next;
  void* destructor;
  void* object;
} DestructorChain;
static DestructorChain* s_chain;

void* __register_global_object(void* object, void* destructor, void* regmem) {
  DestructorChain* c = (DestructorChain*)regmem;
  c->next = s_chain;
  c->destructor = destructor;
  c->object = object;
  s_chain = c;
  return object;
}

/* pointer to member function {this delta, vtable offset, function}: non-null? */
int32_t __ptmf_test(const int32_t* pmf) { return pmf[0] != 0 || pmf[1] != 0 || pmf[2] != 0; }

uint32_t __cvt_fp2unsigned(double d) {
  if (!(d > 0.0)) {
    return 0;
  }
  if (d >= 4294967296.0) {
    return 0xFFFFFFFFu;
  }
  return (uint32_t)d;
}

/* Integer division as the console's CPU does it (port/tools/mwcc_int_div.py
 * routes every division by a non-constant here): no trap. With a zero
 * divisor, or INT_MIN / -1, the quotient register holds -1 for a negative
 * dividend of a signed divide and 0 otherwise. (Behaviour of the Gekko as
 * Dolphin's interpreter documents it; the architecture calls it undefined.)
 * The remainder is what MWCC's `a - (a / b) * b` then gives. */
int32_t __mw_sdiv32(int32_t a, int32_t b) {
  if (b == 0 || (a == INT32_MIN && b == -1)) {
    return a < 0 ? -1 : 0;
  }
  return a / b;
}
int32_t __mw_srem32(int32_t a, int32_t b) {
  return (int32_t)((uint32_t)a - (uint32_t)__mw_sdiv32(a, b) * (uint32_t)b);
}
uint32_t __mw_udiv32(uint32_t a, uint32_t b) { return b != 0 ? a / b : 0; }
uint32_t __mw_urem32(uint32_t a, uint32_t b) { return b != 0 ? a % b : a; }
int64_t __mw_sdiv64(int64_t a, int64_t b) {
  if (b == 0 || (a == INT64_MIN && b == -1)) {
    return a < 0 ? -1 : 0;
  }
  return a / b;
}
int64_t __mw_srem64(int64_t a, int64_t b) {
  return (int64_t)((uint64_t)a - (uint64_t)__mw_sdiv64(a, b) * (uint64_t)b);
}
uint64_t __mw_udiv64(uint64_t a, uint64_t b) { return b != 0 ? a / b : 0; }
uint64_t __mw_urem64(uint64_t a, uint64_t b) { return b != 0 ? a % b : a; }

int64_t __div2i(int64_t a, int64_t b) { return b != 0 ? a / b : 0; }
int64_t __mod2i(int64_t a, int64_t b) { return b != 0 ? a % b : 0; }
uint64_t __div2u(uint64_t a, uint64_t b) { return b != 0 ? a / b : 0; }
uint64_t __mod2u(uint64_t a, uint64_t b) { return b != 0 ? a % b : 0; }

/* The game's global operator delete / delete[] / new[], under the names
 * port/tools/link_census.py gives the game's uses of them (GAME_NEW_DELETE). */
void mkgp2pc_operator_delete(void* p) __asm__("mkgp2pc_game_delete");
void mkgp2pc_operator_delete(void* p) {
  if (p != NULL) {
    MemoryManager_TimedFree(p);
  }
}
void mkgp2pc_operator_delete_array(void* p) __asm__("mkgp2pc_game_delete_array");
void mkgp2pc_operator_delete_array(void* p) {
  if (p != NULL) {
    MemoryManager_TimedFree(p);
  }
}
void* mkgp2pc_operator_new_array(unsigned int n) __asm__("mkgp2pc_game_new_array");
void* mkgp2pc_operator_new_array(unsigned int n) { return Alloc(n); }

/*
 * __dynamic_cast of the MWCC runtime (0x80271340 in the image, unnamed in
 * symbols.txt). MWCC's layout: a vtable begins with the type_info pointer
 * and the offset to the complete object; type_info is { name, base list },
 * the base list an array of { type_info, offset } ended by a null entry
 * that lists every base class of the type. One class can have several
 * type_info objects in the image (MinimapHud's table points at one, the
 * cast in HUD_RegisterOverlay names another, both called "clCrsDisp"), so
 * types are the same when their names are.
 *
 * Classes compiled here carry no type_info (built with -fno-rtti, slot 0 of
 * their tables is null); for those the cast is taken to succeed, which is
 * what the game's own flow expects wherever it does not check the result.
 */
typedef struct MwTypeInfo MwTypeInfo;
typedef struct MwBase {
  const MwTypeInfo* type;
  int32_t offset;
} MwBase;
struct MwTypeInfo {
  const char* name;
  const MwBase* bases;
};
static int mw_same_type(const MwTypeInfo* a, const MwTypeInfo* b) {
  if (a == b) return 1;
  const char* x = a->name;
  const char* y = b->name;
  if (x == NULL || y == NULL) return 0;
  while (*x != 0 && *x == *y) x++, y++;
  return *x == *y;
}
void* fn_80271340(void* object, int32_t vptr_offset, const MwTypeInfo* to, const MwTypeInfo* from, int32_t is_reference) {
  (void)from, (void)is_reference;
  if (object == NULL) return NULL;
  void** table = *(void***)((char*)object + vptr_offset);
  const MwTypeInfo* type = (const MwTypeInfo*)table[0];
  if (type == NULL) return object;
  char* complete = (char*)object + (int32_t)(intptr_t)table[1];
  if (mw_same_type(type, to)) return complete;
  for (const MwBase* b = type->bases; b != NULL && b->type != NULL; b++) {
    if (mw_same_type(b->type, to)) return complete + b->offset;
  }
  return NULL;
}

/*
 * `dynamic_cast<T *>` written as such in a unit (compiled with RTTI on; the
 * IR pass sends the host's __dynamic_cast here with T's plain name, see
 * mwcc_cxx_abi.py). Two kinds of table:
 *  - written out as data (MWCC): word 0 is MWCC's type_info, word 1 the
 *    offset to the complete object, as fn_80271340 reads them;
 *  - emitted by the host compiler: word 0 is the offset to the complete
 *    object (null for the first base), word 1 a description in the same
 *    { name, bases } form that the IR pass generates (step 1c), or null
 *    for a class it could not describe.
 * Without a description, or without a name, the cast is taken to succeed,
 * as fn_80271340 does.
 */
void* mkgp2pc_dynamic_cast(void* object, const void* unused, const char* name, int32_t hint) {
  (void)unused, (void)hint;
  if (object == NULL) return NULL;
  void** table = *(void***)object;
  uintptr_t word = (uintptr_t)table[0];
  const MwTypeInfo* type;
  char* complete;
  if (word >= 0x10000u && word <= 0xFFF00000u) {
    type = (const MwTypeInfo*)table[0];
    complete = (char*)object + (int32_t)(intptr_t)table[1];
  } else {
    type = (const MwTypeInfo*)table[1];
    complete = (char*)object + (int32_t)word;
  }
  if (type == NULL || name == NULL) return object;
  const MwTypeInfo want = {name, NULL};
  if (mw_same_type(type, &want)) return complete;
  for (const MwBase* b = type->bases; b != NULL && b->type != NULL; b++) {
    if (mw_same_type(b->type, &want)) return complete + b->offset;
  }
  return NULL;
}
