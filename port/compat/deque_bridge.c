/* The game's std::deque<DriveStep> (8-byte elements), for the callers that
 * reach its member functions by address name.
 *
 * In the original the waypoint queue of the drive behaviour is one MSL C++
 * std::deque instantiation (0x801E2BD0..0x801E4524, see
 * src/game/fn_801DEF94.cpp). Units written as C, and C++ units that model
 * the queue as an opaque member, call those member functions as fn_801Exxxx.
 * On the host the instantiation comes from port/compat/include/deque under
 * its Itanium names, so nothing defines the address names; as stubs they
 * left the queue uninitialised, and the tracker's destructor then freed
 * whatever stale pointer sat in the block map field.
 *
 * Same object layout and same algorithms as port/compat/include/deque. */
#include <stddef.h>

typedef struct DriveStep {
  int waypoint;
  int duration;
} DriveStep;

typedef struct DriveQueue {
  unsigned bufsize;
  unsigned mapCapacity;
  unsigned mapSize;
  unsigned mapStart; /* stays 0: the block map is a plain array here */
  DriveStep** map;
  unsigned start;
  unsigned size;
} DriveQueue;

void* mkgp2pc_new(unsigned n) __asm__("mkgp2pc_game_new");
void mkgp2pc_delete(void* p) __asm__("mkgp2pc_game_delete");

static void add_block(DriveQueue* q, unsigned where) {
  if (q->mapSize == q->mapCapacity) {
    unsigned cap = q->mapCapacity ? q->mapCapacity * 2 : 4;
    DriveStep** m = (DriveStep**)mkgp2pc_new(cap * sizeof(DriveStep*));
    for (unsigned i = 0; i < q->mapSize; i++) m[i] = q->map[i];
    mkgp2pc_delete(q->map);
    q->map = m;
    q->mapCapacity = cap;
  }
  for (unsigned i = q->mapSize; i > where; i--) q->map[i] = q->map[i - 1];
  q->map[where] = (DriveStep*)mkgp2pc_new(q->bufsize * sizeof(DriveStep));
  q->mapSize++;
}
static DriveStep* at(DriveQueue* q, unsigned i) { return q->map[i / q->bufsize] + i % q->bufsize; }

/* clear(); the block map itself stays allocated (the destructor's caller
 * frees it: PathStorage in src/game/fn_801DEE38.cpp) */
void fn_801E2DF0(DriveQueue* q) {
  for (unsigned i = 0; i < q->mapSize; i++) mkgp2pc_delete(q->map[i]);
  q->mapSize = 0;
  q->start = 0;
  q->size = 0;
}
/* the element part of ~deque() */
void fn_801E2C40(DriveQueue* q) { fn_801E2DF0(q); }

/* pop_front() */
void fn_801E2F28(DriveQueue* q) {
  if (q->size == 0) return;
  q->start++;
  q->size--;
  if (q->start >= q->bufsize && q->mapSize > 1) { /* give the first block back */
    mkgp2pc_delete(q->map[0]);
    for (unsigned i = 1; i < q->mapSize; i++) q->map[i - 1] = q->map[i];
    q->mapSize--;
    q->start -= q->bufsize;
  }
}

/* push_front(value) */
void fn_801E2FD4(DriveQueue* q, const DriveStep* value) {
  if (q->start == 0) {
    add_block(q, 0);
    q->start = q->bufsize;
  }
  q->start--;
  *at(q, q->start) = *value;
  q->size++;
}

/* deque(n): n value-initialised elements */
DriveQueue* fn_801E3260(DriveQueue* q, unsigned n) {
  q->bufsize = 512 / sizeof(DriveStep);
  q->mapCapacity = q->mapSize = q->mapStart = 0;
  q->map = NULL;
  q->start = q->size = 0;
  while (n--) {
    if (q->start + q->size == q->mapSize * q->bufsize) add_block(q, q->mapSize);
    DriveStep* e = at(q, q->start + q->size);
    e->waypoint = 0;
    e->duration = 0;
    q->size++;
  }
  return q;
}

/* the block map's operator[] (Metrowerks::cdeque<const void*>) */
typedef struct DriveRing {
  unsigned capacity, count, head;
  DriveStep** data;
} DriveRing;
DriveStep** fn_801E40C0(DriveRing* ring, unsigned i) {
  unsigned at = ring->head + i;
  if (ring->capacity != 0 && at >= ring->capacity) at -= ring->capacity;
  return &ring->data[at];
}
