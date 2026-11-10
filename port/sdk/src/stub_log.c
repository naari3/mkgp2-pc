#include <mkgp2pc/stub.h>

#include <stdio.h>

static int s_count;

void mkgp2pc_stub_hit(int* seen, const char* func, const char* file, int line) {
  if (*seen) {
    return;
  }
  *seen = 1;
  ++s_count;
  fprintf(stderr, "stub: %s (%s:%d)\n", func, file, line);
}

int mkgp2pc_stub_count(void) { return s_count; }
