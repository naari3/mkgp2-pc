#ifndef MKGP2PC_STUB_H
#define MKGP2PC_STUB_H

/*
 * Marker for SDK functions that exist only so the game links.
 *
 * Every stub body starts with MKGP2PC_STUB(). The first call of each stub is
 * logged ("stub: <function> (<file>:<line>)"), later calls are silent, so a
 * run log doubles as the list of SDK calls that still need a real
 * implementation.
 *
 *     void AISetStreamVolLeft(u8 vol) { MKGP2PC_STUB(); }
 */

#ifdef __cplusplus
extern "C" {
#endif

void mkgp2pc_stub_hit(int* seen, const char* func, const char* file, int line);

/* Number of distinct stubs reached so far in this run. */
int mkgp2pc_stub_count(void);

#ifdef __cplusplus
}
#endif

#define MKGP2PC_STUB()                                                         \
  do {                                                                         \
    static int mkgp2pc_stub_seen_;                                             \
    mkgp2pc_stub_hit(&mkgp2pc_stub_seen_, __func__, __FILE__, __LINE__);       \
  } while (0)

#endif
