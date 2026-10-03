// A stand-in for "an implementation under test", so ulpscope has something to
// point at out of the box.
//
// The functions here deliberately vary in quality: some are correctly rounded,
// some truncate instead of rounding, some compute through a narrower
// intermediate. That spread is the point, because it is what makes the ULP plot
// and the worst-case table show something on first run.
//
// Replace this target with a real build of your math functions and the tool
// behaves identically, since it only ever reaches the library through dlsym.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

using bf16 = __bf16;
using f16 = _Float16;

// fp8 storage types. The compiler has no native e4m3/e5m2 arithmetic, so these
// are plain 8-bit integers and the conversions are written out by hand. That is
// exactly how a freestanding libc carries them anyway.
using fp8e4m3 = uint8_t;
using fp8e5m2 = uint8_t;

// ------------------------------------------------------------ conversions

template <typename T> static uint16_t bitsOf(T v) {
  uint16_t b;
  std::memcpy(&b, &v, sizeof(b));
  return b;
}

template <typename T> static T fromBits(uint16_t b) {
  T v;
  std::memcpy(&v, &b, sizeof(b));
  return v;
}

// --------------------------------------------------- deliberately wrong rounding
//
// A note on how subtle this is: naively "truncating" by discarding the low
// half of a *float* is not a rounding error at all. Round-to-nearest and
// truncation only disagree when the discarded bits are exactly one half, which
// essentially never happens for a transcendental result. An earlier version of
// this file used that trick and ulpscope correctly reported zero mismatches
// for it.
//
// The realistic defect is to compute the correctly rounded answer and then
// adjust it, which is what a real implementation that keeps an explicit
// rounding direction ends up doing. These two helpers are wrong by up to 1 ULP
// on every inexact result, which is what makes the error plot interesting.

// One ULP toward zero from the correctly rounded answer.
static bf16 roundTowardZero(double d) {
  if (!std::isfinite(d))
    return static_cast<bf16>(d);
  const bf16 rne = static_cast<bf16>(d);
  if (static_cast<double>(rne) == d)
    return rne; // exact, nothing to adjust
  const bool neg = std::signbit(d);
  uint16_t mag = static_cast<uint16_t>(bitsOf(rne) & 0x7FFFu);
  if (mag == 0)
    return rne; // already flushed to zero
  --mag; // toward zero always reduces the magnitude
  return fromBits<bf16>(static_cast<uint16_t>(neg ? (mag | 0x8000u) : mag));
}

// One ULP away from zero from the correctly rounded answer.
static bf16 roundAwayFromZero(double d) {
  if (!std::isfinite(d))
    return static_cast<bf16>(d);
  const bf16 rne = static_cast<bf16>(d);
  if (static_cast<double>(rne) == d)
    return rne;
  const bool neg = std::signbit(d);
  uint16_t mag = static_cast<uint16_t>(bitsOf(rne) & 0x7FFFu);
  if (mag >= 0x7F80u)
    return rne;
  ++mag;
  return fromBits<bf16>(static_cast<uint16_t>(neg ? (mag | 0x8000u) : mag));
}

// ------------------------------------------------------------------- fp8
//
// e4m3 is the S.8M3 layout: 1 sign, 4 exponent, 3 mantissa, bias 7, and no
// infinities, so the all-ones exponent with a nonzero mantissa is NaN. There is
// no native fp8 arithmetic on this target, so these are plain bytes and the
// conversions are written out - which is how a freestanding libc carries them
// anyway.
static double e4m3ToDouble(fp8e4m3 v) {
  const uint8_t sign = v & 0x80u;
  const int e = (v >> 3) & 0x0Fu;
  const int m = v & 0x07u;
  double out;
  if (e == 0)
    out = std::ldexp(static_cast<double>(m), 1 - 7 - 3); // denormal: 2^-9
  else if (e == 0x0F && m == 0)
    out = std::numeric_limits<double>::infinity();
  else if (e == 0x0F)
    out = std::numeric_limits<double>::quiet_NaN();
  else
    out = std::ldexp(8.0 + m, e - 7 - 3);
  return sign ? -out : out;
}

static fp8e4m3 doubleToE4m3(double d) {
  if (d != d)
    return 0x7Cu; // quiet NaN
  if (std::isinf(d))
    return 0x78u;
  const uint8_t sign = d < 0 ? 0x80u : 0x00u;
  d = std::fabs(d);
  if (d == 0.0)
    return sign;

  const int bias = 7;
  int exp = 0;
  const double m = std::frexp(d, &exp); // d = m * 2^exp, m in [0.5, 1)
  const int biased = exp - 1 + bias;

  if (biased <= 0)
    return static_cast<fp8e4m3>(
        sign | (static_cast<uint32_t>(std::lround(d * 512.0)) & 0x07u));
  if (biased >= 0x0F)
    return static_cast<fp8e4m3>(sign | 0x78u); // overflow to infinity

  uint32_t frac = static_cast<uint32_t>(std::lround((m * 2.0 - 1.0) * 8.0));
  if (frac > 7)
    frac = 7;
  return static_cast<fp8e4m3>(sign | (static_cast<uint32_t>(biased) << 3) | frac);
}

static double e5m2ToDouble(fp8e5m2 v) {
  const uint8_t sign = v & 0x80u;
  const int e = (v >> 2) & 0x1Fu;
  const int m = v & 0x03u;
  double out;
  if (e == 0)
    out = std::ldexp(static_cast<double>(m), 1 - 15 - 2); // denormal: 2^-16
  else if (e == 0x1F)
    out = m ? std::numeric_limits<double>::quiet_NaN()
            : std::numeric_limits<double>::infinity();
  else
    out = std::ldexp(4.0 + m, e - 15 - 2);
  return sign ? -out : out;
}

// Correctly rounded: evaluated in double and rounded once into 4 significand
// bits, which cannot double-round because 53 >= 2*4 + 2.
extern "C" __attribute__((visibility("default"))) fp8e4m3 sqrte4m3(fp8e4m3 x) {
  return doubleToE4m3(std::sqrt(e4m3ToDouble(x)));
}

extern "C" __attribute__((visibility("default"))) fp8e4m3 expe4m3(fp8e4m3 x) {
  return doubleToE4m3(std::exp(e4m3ToDouble(x)));
}

// Widens through f16 and comes back, so the result is rounded twice. A
// realistic shape of bug, and one only an exhaustive check finds.
extern "C" __attribute__((visibility("default"))) fp8e5m2 loge5m2(fp8e5m2 x) {
  return doubleToE4m3(
      std::log(static_cast<double>(static_cast<f16>(e5m2ToDouble(x)))));
}

// ------------------------------------------------------------------ bf16

// Correctly rounded. Evaluating in double and rounding once down to 8
// significand bits cannot double-round, because 53 >= 2*8 + 2.
#define CORRECT_BF16(name, expr)                                              \
  extern "C" __attribute__((visibility("default"))) bf16 name(bf16 x) {      \
    return static_cast<bf16>(expr);                                          \
  }

CORRECT_BF16(sqrtbf16, std::sqrt(static_cast<double>(x)))
CORRECT_BF16(expbf16, std::exp(static_cast<double>(x)))
CORRECT_BF16(logbf16, std::log(static_cast<double>(x)))
CORRECT_BF16(sinbf16, std::sin(static_cast<double>(x)))
CORRECT_BF16(erfbf16, std::erf(static_cast<double>(x)))
CORRECT_BF16(cbrtbf16, std::cbrt(static_cast<double>(x)))

// Truncating instead of round-to-nearest: wrong by up to 1 ULP on a large
// fraction of the input space.
extern "C" __attribute__((visibility("default"))) bf16 lgammabf16(bf16 x) {
  return roundTowardZero(std::lgamma(static_cast<double>(x)));
}

// Rounds away from zero on inexact results, which is wrong by 1 ULP almost
// everywhere it is not already exact. Named with an underscore suffix to avoid
// colliding with libm's float tgammaf at link time.
extern "C" __attribute__((visibility("default"))) bf16 tgamma_bf16(bf16 x) {
  return roundAwayFromZero(std::tgamma(static_cast<double>(x)));
}

// Computes the square root through powf in *float*, so the result is rounded
// twice. This is what happens in real implementations that reuse a generic pow
// path instead of having a dedicated sqrt. "sqrtdf" is a recognised alias for
// sqrt, so ulpscope resolves a reference for it.
extern "C" __attribute__((visibility("default"))) bf16 sqrtdfbf16(bf16 x) {
  const float f = static_cast<float>(x);
  return static_cast<bf16>(std::pow(f, 0.5f));
}

// Exact, and exercises the bit-manipulation path rather than the arithmetic one.
extern "C" __attribute__((visibility("default"))) bf16 fabsbf16(bf16 x) {
  return fromBits<bf16>(static_cast<uint16_t>(bitsOf(x) & 0x7FFFu));
}

// ------------------------------------------------------------------- f16

extern "C" __attribute__((visibility("default"))) f16 sqrtf16(f16 x) {
  return static_cast<f16>(std::sqrt(static_cast<double>(x)));
}
extern "C" __attribute__((visibility("default"))) f16 expf16(f16 x) {
  return static_cast<f16>(std::exp(static_cast<double>(x)));
}
extern "C" __attribute__((visibility("default"))) f16 sinf16(f16 x) {
  return static_cast<f16>(std::sin(static_cast<double>(x)));
}
extern "C" __attribute__((visibility("default"))) f16 lgammaf16(f16 x) {
  // Same defect as lgammabf16, in the other 16-bit format.
  const double d = std::lgamma(static_cast<double>(x));
  const f16 r = static_cast<f16>(d);
  if (static_cast<double>(r) == d)
    return r;
  const bool neg = std::signbit(d);
  uint16_t mag = static_cast<uint16_t>(bitsOf(r) & 0x7FFFu);
  if (mag >= 0x7F80u)
    return r;
  ++mag;
  return fromBits<f16>(static_cast<uint16_t>(neg ? (mag | 0x8000u) : mag));
}

// ------------------------------------------------------- double-ABI wrappers
//
// These cover f64 as well as the other formats. A `double f(double)` returning
// a double is exact by construction, which is precisely what makes it useful:
// scanning them proves the f64 decode/encode path and the 128-bit ULP
// arithmetic are right, which nothing else in this file does.

// Correctly rounded: libm's sqrt in double is correctly rounded, and returning
// it unchanged needs no further rounding.
extern "C" __attribute__((visibility("default"))) double sqrt_double(double x) {
  return std::sqrt(x);
}

extern "C" __attribute__((visibility("default"))) double log_double(double x) {
  return std::log(x);
}

// Computes in float and widens, so an f64 result is rounded twice. Fine at
// bf16, a genuine defect at f64. The name does not reduce to a known function,
// so ulpscope needs its reference set by hand - which is what the "reference"
// field is for.
extern "C" __attribute__((visibility("default"))) double log2_viaf32(double x) {
  return static_cast<double>(std::log2(static_cast<float>(x)));
}

// -------------------------------------------------- two-argument functions
//
// fmin, fmax, fdim and copysign are only interesting where the two operands
// interact: equal, adjacent, or straddling a signed zero. ulpscope's
// "neighbouring" argument mode walks exactly those, which a fixed second
// argument can never reach.
//
// The "_wrong" variants are also wrong on *every* input, so they are found
// immediately; the point is that the correct fmax below is only 3 ULP off, and
// only because of the -0/+0 boundary the standard leaves open.

extern "C" __attribute__((visibility("default"))) double fmax_double(double x,
                                                                     double y) {
  return x > y ? x : y;
}

extern "C" __attribute__((visibility("default"))) float fmaxf(float x, float y) {
  // Treats -0 and +0 as equal, which the standard does not.
  return x >= y ? x : y;
}

extern "C" __attribute__((visibility("default"))) float fminf(float x, float y) {
  return x < y ? x : y;
}

// Should give +0 when x <= y, and gives -0 instead.
extern "C" __attribute__((visibility("default"))) float fdimf(float x, float y) {
  return x > y ? x - y : -0.0f;
}

// -------------------------------------------- double wrappers (other convs)

// Exercises CallConv::Double: the same maths, with the bf16 narrowing done
// explicitly inside the function.
extern "C" __attribute__((visibility("default"))) double sqrtbf16_d(double x) {
  const double in = static_cast<double>(static_cast<bf16>(x));
  return static_cast<double>(static_cast<bf16>(std::sqrt(in)));
}
extern "C" __attribute__((visibility("default"))) double lgammabf16_d(double x) {
  const double in = static_cast<double>(static_cast<bf16>(x));
  return static_cast<double>(roundTowardZero(std::lgamma(in)));
}

// Exercises CallConv::Float: an honest f32 implementation. Note the spelling is
// "sqrt_f32" rather than "sqrtf32": glibc already exports the latter as its
// _Float32 entry point, so it would collide at link time.
extern "C" __attribute__((visibility("default"))) float sqrt_f32(float x) {
  return std::sqrt(x);
}
extern "C" __attribute__((visibility("default"))) float lgamma_f32(float x) {
  return std::lgamma(x);
}

// Exercises a two-argument function.
extern "C" __attribute__((visibility("default"))) double powbf16_d(double x,
                                                                  double y) {
  const double a = static_cast<double>(static_cast<bf16>(x));
  const double b = static_cast<double>(static_cast<bf16>(y));
  return static_cast<double>(static_cast<bf16>(std::pow(a, b)));
}