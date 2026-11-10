/* PowerPC floating-point helpers that the decompiled code reaches through
 * MWCC intrinsics. */
#include <math.h>
#include <stdint.h>
#include <string.h>

/*
 * frsqrte. PLACEHOLDER: returns the correctly rounded 1/sqrt(x), not the
 * Gekko table estimate. The callers (inlined MSL sqrtf) refine the estimate
 * with three Newton steps, so results agree with the console to within the
 * last bits but are not guaranteed bit-identical. Replace with the 32-entry
 * base/decrement table before relying on replay or network determinism.
 */
double mkgp2pc_frsqrte(double x) { return 1.0 / sqrt(x); }
