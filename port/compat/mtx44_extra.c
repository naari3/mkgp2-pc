/* The two 4x4 matrix functions of the SDK that Aurora's mtx library does
 * not have. */
#include <string.h>

void PSMTX44Identity(float m[4][4]) {
  memset(m, 0, sizeof(float[4][4]));
  m[0][0] = m[1][1] = m[2][2] = m[3][3] = 1.0f;
}

void PSMTX44Copy(const float src[4][4], float dst[4][4]) { memmove(dst, src, sizeof(float[4][4])); }
