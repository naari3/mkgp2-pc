/* PC replacement for include/libc/math.h: same declarations, with the two
 * PowerPC `asm("frsqrte ...")` statements routed to mkgp2pc_frsqrte so the
 * inlined MSL sqrtf keeps its original arithmetic. Keep in step with
 * include/libc/math.h when upstream changes it. */
#ifndef _MATH_H_
#define _MATH_H_

#define M_PI 3.14159265358979323846f

#define NAN       (0.0f / 0.0f)
#define HUGE_VALF (1.0f / 0.0f)
#define INFINITY  (1.0f / 0.0f)

double fabs(double x);
double sin(double x);
double cos(double x);

float sinf(float x);
float cosf(float x);
float tanf(float x);
float acosf(float x);
float powf(float base, float exponent);

double ldexp(double x, int exp);

double scalbn(double x, int n);

double copysign(double x, double y);

#ifdef __MWERKS__
#pragma cplusplus on
#endif

double floor(double x);

static inline float sqrtf(float x)
{
    static const double _half = .5;
    static const double _three = 3.0;
    volatile float y;
    if (x > 0.0f)
    {
        double guess = mkgp2pc_frsqrte((double)x);
        guess = _half*guess*(_three - guess*guess*x);  // now have 12 sig bits
        guess = _half*guess*(_three - guess*guess*x);  // now have 24 sig bits
        guess = _half*guess*(_three - guess*guess*x);  // now have 32 sig bits
        y = (float)(x*guess);
        return y ;
    }
    return x;
}

/* upstream declares `float sqrt(float)` here for GXDraw.o only; the name
 * would clash with the real double sqrt, so it is renamed on PC. */
static inline float sqrt_f32(float x)
{
    static const double _half = .5;
    static const double _three = 3.0;
    volatile float y;
    if (x > 0.0f)
    {
        double guess = mkgp2pc_frsqrte((double)x);
        guess = _half*guess*(_three - guess*guess*x);  // now have 12 sig bits
        guess = _half*guess*(_three - guess*guess*x);  // now have 24 sig bits
        guess = _half*guess*(_three - guess*guess*x);  // now have 32 sig bits
        y = (float)(x*guess);
        return y ;
    }
    return x;
}

#ifdef __MWERKS__
#define fabs(x) __fabs(x)
#define fabsf(x) __fabsf(x)
#else
double fabs(double x);
float fabsf(float x);
#endif

long __fpclassifyf(float x);
long __fpclassifyd(double x);

#define FP_NAN       1
#define FP_INFINITE  2
#define FP_ZERO      3
#define FP_NORMAL    4
#define FP_SUBNORMAL 5

#define fpclassify(x) (sizeof(x) == sizeof(float) ? __fpclassifyf((float)(x)) : __fpclassifyd((double)(x)))
#define isfinite(x) ((fpclassify(x) > FP_INFINITE))

static inline float fmodf(float x, float m)
{
    float a = fabsf(m);
    float b = fabsf(x);
    if (a > b)
        return x;
    else
    {
        long long c = (long long)(x / m);
        return x - m * c;
    }
}

#ifdef __MWERKS__
#pragma cplusplus reset
#endif

#endif
