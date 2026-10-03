#include "core/format.h"

#include "core/mpfr_util.h"

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <limits>

namespace ulpscope {

const char *className(Class c) {
  switch (c) {
  case Class::Zero: return "zero";
  case Class::Normal: return "normal";
  case Class::Subnormal: return "denormal";
  case Class::Infinity: return "infinity";
  case Class::NaN: return "nan";
  }
  return "?";
}

// The format table. Adding a format means adding a row and nothing else:
// decode, encode, the MPFR rounding parameters, the inspector's bit grid and
// the export tables are all derived from these numbers.
//
// Ordered roughly by width, narrowest first, since those are the formats this
// tool exists for.
const std::vector<FormatSpec> kSpecs = {
    {"e4m3", 8, 4, 3, 7, true},    // _Float8_e4m3fn, as llvm-libc spells it
    {"e5m2", 8, 5, 2, 15, true},   // _Float8_e5m2
    {"f16", 16, 5, 10, 15, true},
    {"bf16", 16, 8, 7, 127, true},
    {"f32", 32, 8, 23, 127, true},
    {"f64", 64, 11, 52, 1023, true},
};

const std::vector<FormatSpec> &FloatFormat::specs() { return kSpecs; }

const std::vector<FloatFormat> &FloatFormat::all() {
  static const std::vector<FloatFormat> v = [] {
    std::vector<FloatFormat> out;
    out.reserve(kSpecs.size());
    for (const FormatSpec &s : kSpecs) {
      FloatFormat f{s.name, s.totalBits, s.expBits, s.mantBits, s.bias,
                    s.hasInfNan};
      // A malformed row would silently produce wrong rounding for every
      // result, so refuse it loudly here instead.
      if (!f.valid())
        std::fprintf(stderr,
                     "ulpscope: ignoring malformed format spec '%s' "
                     "(%d bits = %d exp + %d mant + 1 sign)\n",
                     s.name, s.totalBits, s.expBits, s.mantBits);
      else
        out.push_back(f);
    }
    return out;
  }();
  return v;
}

const FloatFormat *FloatFormat::byName(const std::string &name) {
  const int i = indexByName(name.c_str());
  return i < 0 ? nullptr : &all()[i];
}

int FloatFormat::indexByName(const char *name) {
  for (size_t i = 0; i < kSpecs.size(); ++i)
    if (name == std::string(kSpecs[i].name))
      return static_cast<int>(i);
  return -1;
}

int FloatFormat::indexImpliedByName(const std::string &symbol) {
  struct Suffix {
    const char *text;
    const char *format;
  };
  // Most specific first. fp8 suffixes matter because a bare "e4m3" also ends in
  // nothing else, but "e4m3fn" must be tried before "fn"-less matching.
  static const Suffix kSuffixes[] = {
      {"bfloat16", "bf16"}, {"bf16", "bf16"},   {"f16", "f16"},
      {"e4m3fnuz", "e4m3"}, {"e4m3fn", "e4m3"}, {"e4m3", "e4m3"},
      {"e5m2fnz", "e5m2"},  {"e5m2", "e5m2"},   {"f32", "f32"},
      {"f64", "f64"},
  };
  for (const Suffix &s : kSuffixes) {
    const size_t n = std::char_traits<char>::length(s.text);
    if (symbol.size() > n && symbol.compare(symbol.size() - n, n, s.text) == 0)
      return indexByName(s.format);
  }
  return -1;
}

double FloatFormat::decode(uint64_t bits) const {
  const uint64_t frac = bits & fracMask();
  const uint64_t e = (bits >> mantBits) & expMask();
  const bool neg = (bits & signMask()) != 0;

  double v;
  if (e == 0) {
    // Zero or subnormal: no implicit bit.
    v = std::ldexp(static_cast<double>(frac), 1 - bias - mantBits);
  } else if (hasInfNan && e == expMask()) {
    v = frac ? std::numeric_limits<double>::quiet_NaN()
             : std::numeric_limits<double>::infinity();
  } else {
    const uint64_t significand = frac | (1ULL << mantBits);
    v = std::ldexp(static_cast<double>(significand),
                   static_cast<int>(e) - bias - mantBits);
  }
  return neg ? -v : v;
}

Class FloatFormat::classify(uint64_t bits) const {
  const uint64_t frac = bits & fracMask();
  const uint64_t e = (bits >> mantBits) & expMask();
  if (hasInfNan && e == expMask())
    return frac ? Class::NaN : Class::Infinity;
  if (e == 0)
    return frac ? Class::Subnormal : Class::Zero;
  return Class::Normal;
}

Class FloatFormat::classifyReal(double v) const {
  if (std::isnan(v))
    return Class::NaN;
  if (std::isinf(v))
    return Class::Infinity;
  if (v == 0.0)
    return Class::Zero;
  return std::fabs(v) < minNormal() ? Class::Subnormal : Class::Normal;
}

double FloatFormat::ulpOf(uint64_t bits) const {
  const Class c = classify(bits);
  if (c == Class::Infinity || c == Class::NaN)
    return 0.0;
  if (c == Class::Subnormal || c == Class::Zero)
    return std::ldexp(1.0, 1 - bias - mantBits);
  const int64_t e = static_cast<int64_t>((bits >> mantBits) & expMask()) - bias;
  return std::ldexp(1.0, static_cast<int>(e - mantBits));
}

unsigned __int128 FloatFormat::orderedKeyWide(uint64_t bits) const {
  // Map sign/magnitude into a single increasing integer line centred on zero,
  // so that subtraction is ULP distance and -0 and +0 collapse to one point.
  //
  // Computed at 128 bits: a 64-bit format needs a centre of 2^63 and adding a
  // magnitude of up to 2^63-1 overflows a uint64, which would silently invert
  // the ordering for f64.
  const unsigned __int128 centre = (unsigned __int128)1 << (totalBits - 1);
  const uint64_t sign = bits & signMask();
  const uint64_t magnitude = bits & ~signMask();
  return sign ? (centre - magnitude) : (centre + magnitude);
}

double FloatFormat::minNormal() const {
  return std::ldexp(1.0, 1 - bias);
}

double FloatFormat::maxFinite() const {
  const int64_t emax = static_cast<int64_t>(expMask()) - 1 - bias;
  return std::ldexp(2.0 - std::ldexp(1.0, -mantBits), static_cast<int>(emax));
}

std::string FloatFormat::describe() const {
  return std::string(name) + " (" + std::to_string(totalBits) + "-bit, " +
         std::to_string(expBits) + "-bit exponent, " +
         std::to_string(mantBits) + " stored mantissa bits)";
}

// ------------------------------------------------------------------ encoding

MpfrRounder::MpfrRounder(const FloatFormat &fmt) : fmt_(fmt) {
  // Deliberately no mpfr_set_emin/mpfr_set_emax anywhere: those are global
  // state in MPFR, and they would not give IEEE subnormal rounding anyway.
  // `narrow_` rounds normals to the right number of significant bits at a wide
  // exponent range; `work_` is high precision for the subnormal shift.
  mpfr_init2(narrow_, static_cast<mpfr_prec_t>(fmt_.precision()));
  mpfr_init2(work_, 256);
}

// Shared tail of both rounding paths. `narrow_`/`work_` always hold a
// magnitude; the sign is applied here and only here.
double MpfrRounder::assemble(bool neg, bool subnormal, long long frac) {
  if (subnormal) {
    // frac is the rounded number of steps of the subnormal grid. It can reach
    // 2^mantBits, which is exactly the smallest normal, and encoding that as a
    // raw fraction value carries correctly because the bit lands in the
    // exponent field.
    double v = static_cast<double>(frac) * fmt_.subnormalGrid();
    return neg ? -v : v;
  }
  const double v = mpfr_get_d(narrow_, MPFR_RNDN);
  return neg ? -v : v;
}

double MpfrRounder::round(double v) {
  if (std::isnan(v))
    return fmt_.decode(fmt_.canonicalNaN());
  if (std::isinf(v))
    return std::signbit(v) ? -fmt_.decode(fmt_.infinity())
                           : fmt_.decode(fmt_.infinity());
  if (v == 0.0)
    return std::signbit(v) ? -0.0 : 0.0;

  const bool neg = std::signbit(v);
  const double a = std::fabs(v);

  if (a < fmt_.minNormal()) {
    // Shift by a power of two, which is exact, then round to an integer.
    mpfr_set_d(work_, a, MPFR_RNDN);
    mpfr_mul_2si(work_, work_, -fmt_.minExponent() + fmt_.mantBits, MPFR_RNDN);
    const long long frac = mpfr_get_si(work_, MPFR_RNDN);
    return assemble(neg, /*subnormal=*/true, frac);
  }

  // Both paths store a *magnitude* in narrow_/work_ and let assemble apply the
  // sign, so the sign is applied exactly once.
  mpfr_set_d(narrow_, a, MPFR_RNDN);
  return assemble(neg, /*subnormal=*/false, 0);
}

double MpfrRounder::roundFromMpfr(mpfr_srcptr x) {
  if (mpfr_nan_p(x))
    return fmt_.decode(fmt_.canonicalNaN());
  if (mpfr_inf_p(x))
    return std::signbit(mpfr_get_d(x, MPFR_RNDN)) ? -fmt_.decode(fmt_.infinity())
                                                  : fmt_.decode(fmt_.infinity());
  if (mpfr_zero_p(x))
    return std::signbit(mpfr_get_d(x, MPFR_RNDN)) ? -0.0 : 0.0;

  const bool neg = mpfr_signbit(x) != 0;

  // Work on the magnitude from here on. Comparing the *signed* value against
  // minNormal would send every negative result down the subnormal path, which
  // is a silent and very confusing way to produce garbage.
  mpfr_abs(work_, x, MPFR_RNDN);

  if (mpfr_cmp_d(work_, fmt_.minNormal()) < 0) {
    // Shift by a power of two, which is exact, then round to an integer.
    mpfr_mul_2si(work_, work_, -fmt_.minExponent() + fmt_.mantBits, MPFR_RNDN);
    const long long frac = mpfr_get_si(work_, MPFR_RNDN);
    return assemble(neg, /*subnormal=*/true, frac);
  }

  mpfr_set(narrow_, work_, MPFR_RNDN);
  return assemble(neg, /*subnormal=*/false, 0);
}

uint64_t MpfrRounder::pack(double exactInFmt) {
  if (std::isnan(exactInFmt))
    return fmt_.canonicalNaN();
  if (std::isinf(exactInFmt))
    return std::signbit(exactInFmt) ? fmt_.negativeInfinity()
                                    : fmt_.infinity();

  const bool neg = std::signbit(exactInFmt);
  const double a = std::fabs(exactInFmt);

  uint64_t bits = 0;
  if (a == 0.0) {
    bits = 0;
  } else if (a >= fmt_.minNormal()) {
    int exp = 0;
    const double m = std::frexp(a, &exp); // a = m * 2^exp, m in [0.5, 1)
    const int biased = exp - 1 + fmt_.bias;
    if (biased >= static_cast<int>(fmt_.expMask()))
      return std::signbit(exactInFmt) ? fmt_.negativeInfinity()
                                      : fmt_.infinity();
    bits = (static_cast<uint64_t>(biased) << fmt_.mantBits);
    // m in [0.5, 1) -> fraction in [0, 2^mantBits)
    bits |= static_cast<uint64_t>(std::ldexp(m * 2.0 - 1.0, fmt_.mantBits) +
                                  0.5);
  } else {
    // Subnormal: value == frac * 2^(1 - bias - mantBits)
    const double scale = std::ldexp(1.0, 1 - fmt_.bias - fmt_.mantBits);
    bits = static_cast<uint64_t>(std::floor(a / scale + 0.5));
  }

  bits &= fmt_.mask(fmt_.totalBits);
  return neg ? (bits | fmt_.signMask()) : bits;
}

double roundToFormat(double v, const FloatFormat &fmt) {
  MpfrRounder r(fmt);
  return r.round(v);
}

uint64_t encodeRounded(double v, const FloatFormat &fmt) {
  MpfrRounder r(fmt);
  return r.bits(v);
}

double encodeToDouble(double v, const FloatFormat &fmt) {
  return roundToFormat(v, fmt);
}

uint64_t encode(double v, const FloatFormat &fmt) {
  return encodeRounded(v, fmt);
}

Class classifyDouble(double v) {
  if (std::isnan(v))
    return Class::NaN;
  if (std::isinf(v))
    return Class::Infinity;
  if (v == 0.0)
    return Class::Zero;
  return Class::Normal;
}

} // namespace ulpscope