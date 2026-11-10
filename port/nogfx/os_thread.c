/*
 * Dolphin OS threads, interrupts-as-a-lock, semaphores, message queues and
 * alarms on host threads. Bring-up version (pthreads); the shipped build
 * needs the same on SDL threads in port/sdk.
 *
 * Model. The console has one CPU: code protects shared state by disabling
 * interrupts, and waits for other threads or interrupt handlers in busy
 * loops. Here every OSThread is a host thread that really runs in parallel,
 * and "interrupts disabled" is one global recursive lock. Alarm handlers run
 * on a timer thread with that lock held, like an interrupt would. What this
 * does not reproduce: thread priorities (code that is only correct because a
 * lower-priority thread cannot run is not protected).
 *
 * No OS structure layout is assumed except OSSemaphore.count at offset 0;
 * everything else lives in side tables keyed by the caller's pointer.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int64_t OSGetTime(void);

/* ---- the "interrupt" lock ------------------------------------------------ */
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_wake = PTHREAD_COND_INITIALIZER; /* one condition for every sleeper */
static __thread int s_depth;

int OSDisableInterrupts(void) {
  int was = s_depth == 0;
  if (was) {
    pthread_mutex_lock(&s_lock);
    s_depth = 1;
  }
  return was; /* "interrupts were enabled" */
}
/* Interrupts are a state, not a count: a nested disable returns 0 and its
 * restore changes nothing; the outermost restore (1) unlocks. */
int OSRestoreInterrupts(int wasEnabled) {
  int was = s_depth == 0;
  if (wasEnabled && s_depth > 0) {
    s_depth = 0;
    pthread_mutex_unlock(&s_lock);
  }
  return was;
}
int OSEnableInterrupts(void) { return OSRestoreInterrupts(1); }

/* Sleep until `*done` becomes true; called with any lock depth. */
static void wait_for(int (*done)(void*), void* arg) {
  int depth = s_depth;
  if (depth == 0) {
    pthread_mutex_lock(&s_lock);
  }
  while (!done(arg)) {
    pthread_cond_wait(&s_wake, &s_lock);
  }
  if (depth == 0) {
    pthread_mutex_unlock(&s_lock);
  }
}
static void lock_in(int* depth) {
  *depth = s_depth;
  if (s_depth++ == 0) {
    pthread_mutex_lock(&s_lock);
  }
}
static void lock_out(int depth) {
  s_depth = depth;
  if (depth == 0) {
    pthread_mutex_unlock(&s_lock);
  }
}

/* ---- threads ------------------------------------------------------------- */
#define MAX_THREADS 32
typedef struct {
  void* os; /* the game's OSThread */
  void* (*func)(void*);
  void* param;
  pthread_t host;
  int started;
  int suspended;
  void* sleepingOn; /* thread queue, or NULL */
  int exited;
} Thread;
static Thread s_threads[MAX_THREADS];
static __thread Thread* s_self;
static char s_mainThread; /* stands for the boot thread's OSThread */

static Thread* thread_of(void* os) {
  for (int i = 0; i < MAX_THREADS; i++) {
    if (s_threads[i].os == os) {
      return &s_threads[i];
    }
  }
  return NULL;
}

static int not_suspended(void* t) { return !((Thread*)t)->suspended; }

static void* thread_main(void* arg) {
  Thread* t = (Thread*)arg;
  s_self = t;
  wait_for(not_suspended, t);
  t->func(t->param);
  t->exited = 1;
  return NULL;
}

int OSCreateThread(void* thread, void* (*func)(void*), void* param, void* stack, uint32_t stackSize,
                   int32_t priority, uint16_t attr) {
  int d;
  (void)stack, (void)stackSize, (void)priority, (void)attr;
  lock_in(&d);
  Thread* t = thread_of(thread);
  if (!t) {
    t = thread_of(NULL);
  }
  if (!t) {
    lock_out(d);
    fprintf(stderr, "OSCreateThread: out of thread slots\n");
    return 0;
  }
  memset(t, 0, sizeof(*t));
  t->os = thread;
  t->func = func;
  t->param = param;
  t->suspended = 1;
  lock_out(d);
  return 1;
}

int32_t OSResumeThread(void* thread) {
  int d;
  lock_in(&d);
  Thread* t = thread_of(thread);
  int32_t was = 0;
  if (t) {
    was = t->suspended;
    t->suspended = 0;
    if (!t->started) {
      pthread_attr_t a;
      t->started = 1;
      pthread_attr_init(&a);
      pthread_attr_setstacksize(&a, 1 << 20);
      pthread_create(&t->host, &a, thread_main, t);
      pthread_attr_destroy(&a);
    }
    pthread_cond_broadcast(&s_wake);
  }
  lock_out(d);
  return was;
}

int32_t OSSuspendThread(void* thread) {
  int d;
  lock_in(&d);
  Thread* t = thread_of(thread);
  int32_t was = 0;
  if (t) {
    was = t->suspended;
    t->suspended = 1;
  }
  lock_out(d);
  if (t && t == s_self) {
    wait_for(not_suspended, t);
  }
  return was;
}

void* OSGetCurrentThread(void) { return s_self ? s_self->os : (void*)&s_mainThread; }
void OSYieldThread(void) { sched_yield(); }
int OSIsThreadTerminated(void* thread) {
  Thread* t = thread_of(thread);
  return !t || t->exited;
}

/* Thread queues: a sleeper records the queue it waits on; wake-up clears it. */
static __thread void* s_sleepQueue;
static struct {
  void* queue;
  unsigned generation;
} s_wakeLog[64];
static unsigned s_wakeGen;

static unsigned queue_generation(void* queue) {
  for (int i = 0; i < 64; i++) {
    if (s_wakeLog[i].queue == queue) {
      return s_wakeLog[i].generation;
    }
  }
  return 0;
}
typedef struct {
  void* queue;
  unsigned seen;
} SleepArg;
static int woken(void* a) { return queue_generation(((SleepArg*)a)->queue) != ((SleepArg*)a)->seen; }

void OSSleepThread(void* queue) {
  int d;
  lock_in(&d);
  SleepArg a = {queue, queue_generation(queue)};
  /* give the lock up completely while asleep, as a context switch would */
  int depth = s_depth;
  s_depth = 0;
  while (!woken(&a)) {
    pthread_cond_wait(&s_wake, &s_lock);
  }
  s_depth = depth;
  lock_out(d);
}

void OSWakeupThread(void* queue) {
  int d;
  lock_in(&d);
  int slot = -1;
  for (int i = 0; i < 64; i++) {
    if (s_wakeLog[i].queue == queue) {
      slot = i;
      break;
    }
    if (slot < 0 && s_wakeLog[i].queue == NULL) {
      slot = i;
    }
  }
  if (slot < 0) {
    slot = (int)(++s_wakeGen % 64); /* table full: recycle */
  }
  s_wakeLog[slot].queue = queue;
  s_wakeLog[slot].generation++;
  if (s_wakeLog[slot].generation == 0) {
    s_wakeLog[slot].generation = 1;
  }
  pthread_cond_broadcast(&s_wake);
  lock_out(d);
}

void OSClearContext(void* context) { (void)context; }
void OSSetCurrentContext(void* context) { (void)context; }

/* ---- semaphores ---------------------------------------------------------- */
typedef struct {
  int32_t count;
} Sem;
static int sem_free(void* s) { return ((Sem*)s)->count > 0; }

void OSInitSemaphore(Sem* s, int32_t count) { s->count = count; }
int32_t OSWaitSemaphore(Sem* s) {
  int d;
  lock_in(&d);
  int depth = s_depth;
  s_depth = 0;
  while (!sem_free(s)) {
    pthread_cond_wait(&s_wake, &s_lock);
  }
  s_depth = depth;
  int32_t was = s->count--;
  lock_out(d);
  return was;
}
int32_t OSTryWaitSemaphore(Sem* s) {
  int d;
  lock_in(&d);
  int32_t was = s->count;
  if (was > 0) {
    s->count--;
  }
  lock_out(d);
  return was;
}
int32_t OSSignalSemaphore(Sem* s) {
  int d;
  lock_in(&d);
  int32_t was = s->count++;
  pthread_cond_broadcast(&s_wake);
  lock_out(d);
  return was;
}

/* ---- message queues ------------------------------------------------------ */
#define MAX_QUEUES 32
typedef struct {
  void* os;
  void** slots;
  int32_t capacity, first, used;
} Queue;
static Queue s_queues[MAX_QUEUES];

static Queue* queue_of(void* os, int create) {
  Queue* free_ = NULL;
  for (int i = 0; i < MAX_QUEUES; i++) {
    if (s_queues[i].os == os) {
      return &s_queues[i];
    }
    if (!free_ && !s_queues[i].os) {
      free_ = &s_queues[i];
    }
  }
  return create ? free_ : NULL;
}

void OSInitMessageQueue(void* mq, void** msgArray, int32_t msgCount) {
  int d;
  lock_in(&d);
  Queue* q = queue_of(mq, 1);
  if (q) {
    q->os = mq;
    q->slots = msgArray;
    q->capacity = msgCount;
    q->first = q->used = 0;
  }
  lock_out(d);
}

int OSSendMessage(void* mq, void* msg, int32_t flags) {
  int d;
  int ok = 0;
  lock_in(&d);
  Queue* q = queue_of(mq, 0);
  if (q) {
    int depth = s_depth;
    s_depth = 0;
    while (q->used >= q->capacity && (flags & 1)) {
      pthread_cond_wait(&s_wake, &s_lock);
    }
    s_depth = depth;
    if (q->used < q->capacity) {
      q->slots[(q->first + q->used++) % q->capacity] = msg;
      ok = 1;
      pthread_cond_broadcast(&s_wake);
    }
  }
  lock_out(d);
  return ok;
}

int OSReceiveMessage(void* mq, void** msg, int32_t flags) {
  int d;
  int ok = 0;
  lock_in(&d);
  Queue* q = queue_of(mq, 0);
  if (q) {
    int depth = s_depth;
    s_depth = 0;
    while (q->used == 0 && (flags & 1)) {
      pthread_cond_wait(&s_wake, &s_lock);
    }
    s_depth = depth;
    if (q->used > 0) {
      if (msg) {
        *msg = q->slots[q->first];
      }
      q->first = (q->first + 1) % q->capacity;
      q->used--;
      ok = 1;
      pthread_cond_broadcast(&s_wake);
    }
  }
  lock_out(d);
  return ok;
}

/* ---- alarms -------------------------------------------------------------- */
#define MAX_ALARMS 64
#define TICKS_PER_SEC 40500000ll
typedef void (*AlarmHandler)(void* alarm, void* context);
typedef struct {
  void* os;
  AlarmHandler handler;
  int64_t fire, period;
  int armed;
} Alarm;
static Alarm s_alarms[MAX_ALARMS];
static pthread_t s_timer;
static int s_timerStarted;

static void* timer_main(void* arg) {
  (void)arg;
  for (;;) {
    struct timespec ts = {0, 1000000}; /* 1 ms resolution */
    nanosleep(&ts, NULL);
    int64_t now = OSGetTime();
    for (int i = 0; i < MAX_ALARMS; i++) {
      int d;
      lock_in(&d);
      Alarm a = s_alarms[i];
      int due = a.armed && a.fire <= now;
      if (due) {
        if (a.period > 0) {
          s_alarms[i].fire += a.period;
          if (s_alarms[i].fire <= now) {
            s_alarms[i].fire = now + a.period;
          }
        } else {
          s_alarms[i].armed = 0;
        }
        a.handler(a.os, NULL); /* with the lock held, like an interrupt handler */
      }
      lock_out(d);
    }
  }
  return NULL;
}

static Alarm* alarm_of(void* os) {
  Alarm* free_ = NULL;
  for (int i = 0; i < MAX_ALARMS; i++) {
    if (s_alarms[i].os == os) {
      return &s_alarms[i];
    }
    if (!free_ && !s_alarms[i].os) {
      free_ = &s_alarms[i];
    }
  }
  if (free_) {
    memset(free_, 0, sizeof(*free_));
    free_->os = os;
  }
  return free_;
}

static void arm(void* alarm, int64_t fire, int64_t period, AlarmHandler handler) {
  int d;
  lock_in(&d);
  Alarm* a = alarm_of(alarm);
  if (a) {
    a->handler = handler;
    a->fire = fire;
    a->period = period;
    a->armed = 1;
  }
  if (!s_timerStarted) {
    s_timerStarted = 1;
    pthread_create(&s_timer, NULL, timer_main, NULL);
  }
  lock_out(d);
}

void OSInitAlarm(void) {}
void OSCreateAlarm(void* alarm) { (void)alarm; }
void OSSetAlarm(void* alarm, int64_t tick, AlarmHandler handler) { arm(alarm, OSGetTime() + tick, 0, handler); }
void OSSetPeriodicAlarm(void* alarm, int64_t start, int64_t period, AlarmHandler handler) {
  (void)start;
  arm(alarm, OSGetTime() + period, period, handler);
}
void OSCancelAlarm(void* alarm) {
  int d;
  lock_in(&d);
  Alarm* a = alarm_of(alarm);
  if (a) {
    a->armed = 0;
    a->os = NULL;
  }
  lock_out(d);
}
