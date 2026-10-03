// Correctly rounded conversion into a target float format.
//
// Rounding is done explicitly here rather than by setting MPFR's emin/emax and
// hoping for IEEE subnormal behaviour. That hope is misplaced: MPFR's exponent
// range clamps the exponent, but it does *not* coarsen the significand grid for
// subnormal values the way IEEE does. Measured with f16 (bias 15, 10 stored
// mantissa bits), rounding 5.9212671107461366e-05 with prec=11 gave
// 993.5 * 2^-24 for *every* value of emin, where IEEE wants 993 * 2^-24: the
// IEEE subnormal grid is one step of 2^(1-bias-mantBits), coarser than 11
// significant bits at that magnitude.
//
// So the two cases are handled separately, using the parts of MPFR that are
// unambiguous:
//
//   subnormal  divide by the grid (an exact power-of-two shift), then round the
//              quotient to an integer. mpfr_get_si with MPFR_RNDN is
//              round-to-nearest-ties-to-even, which is exactly the IEEE rule.
//   normal     round to 1 + mantBits significant bits at a *wide* exponent
//              range, so MPFR does not clamp, and let the encoder handle
//              overflow.
//
// Neither step sets emin/emax, which also removes the need to care about those
// being thread-global in MPFR.
#pragma once

#include <mpfr.h>

#include <cfloat>
#include <cmath>

#include "core/format.h"

namespace ulpscope {

class MpfrRounder {
public:
  explicit MpfrRounder(const FloatFormat &fmt);

  ~MpfrRounder() {
    mpfr_clear(narrow_);
    mpfr_clear(work_);
  }

  MpfrRounder(const MpfrRounder &) = delete;
  MpfrRounder &operator=(const MpfrRounder &) = delete;

  const FloatFormat &fmt() const { return fmt_; }

  // Correctly rounded conversion of a real value into fmt. The result is
  // exactly representable in fmt, and is returned as a double.
  double round(double v);

  // The same, for a value already held at high precision. Use this for
  // reference results: going via mpfr_get_d() first would round twice.
  double roundFromMpfr(mpfr_srcptr x);

  // round(), then packed into fmt's bit pattern.
  uint64_t bits(double v) { return pack(round(v)); }

  // Packs a double that is already exactly representable in fmt.
  uint64_t pack(double exactInFmt);

  // Scratch space at high precision with a wide exponent range, for callers
  // that need to evaluate a function before rounding.
  mpfr_ptr work() { return work_; }

private:
  // Shared tail of both rounding paths.
  double assemble(bool neg, bool subnormal, long long frac);

  mpfr_t narrow_; // 1 + mantBits significant bits, wide exponent range
  mpfr_t work_;   // high precision, wide exponent range
  FloatFormat fmt_;
};

// One-shot convenience wrappers. Prefer a reusable MpfrRounder in hot loops.
double roundToFormat(double v, const FloatFormat &fmt);
uint64_t encodeRounded(double v, const FloatFormat &fmt);

} // namespace ulpscope