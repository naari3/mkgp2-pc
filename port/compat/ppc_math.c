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

/* MSL float.h constants, referenced as objects by HSD. */
float __float_nan = __builtin_nanf("");
float __float_huge = __builtin_huge_valf();
float __float_min = 1.17549435e-38f;
float __float_epsilon = 1.1920929e-07f;
