#include "core/mpfrref.h"

#include <algorithm>
#include <cmath>

namespace ulpscope {

std::string baseName(const std::string &symbol) {
  std::string s = symbol;
  while (!s.empty() && s.front() == '_')
    s.erase(s.begin());

  // Explicit type suffixes. Bare "d" and "l" are deliberately absent: they look
  // right for long-double spellings but they eat real function names, turning
  // "sqrtdf" into "sqrtd". The underscore forms "_d" and "_l" are safe and are
  // included, so "powbf16_d" still resolves.
  //
  // Order matters and is not cosmetic: "sqrtbf16" also ends with "f16", so
  // "bf16" has to be tried before "f16" or the peel leaves "sqrtb". More
  // specific suffixes always come first.
  //
  // fp8 suffixes are here so "sqrte4m3" resolves to "sqrt". Without them the
  // peel would stop at "sqrte" and the function would look unknown.
  static const char *typeSuffixes[] = {
      "bfloat16", "float128", "float64", "float32", "float16", "bfloat",
      "e4m3fn",   "e5m2fnz",  "e4m3fnuz", "e5m2",  "e4m3",   "e8m0",
      "bf16",     "f128",     "f32",     "f64",    "f16",     "f80",
      "double",   "float",    "_bfloat",  "_bf16",  "_f128",   "_f32",
      "_f64",     "_f16",     "_f80",     "_e4m3",  "_e5m2",  "_d",
      "_l",
  };

  // Peel repeatedly so compound spellings resolve: "powbf16_d" -> "powbf16_" ->
  // "powbf16" -> "pow". Separators left dangling by a peel are trimmed so the
  // next round can continue.
  auto peel = [&s](const char *const *table, size_t n) {
    for (;;) {
      bool stripped = false;
      for (size_t i = 0; i < n; ++i) {
        const std::string t = table[i];
        if (s.size() > t.size() &&
            s.compare(s.size() - t.size(), t.size(), t) == 0) {
          s.resize(s.size() - t.size());
          stripped = true;
          break;
        }
      }
      while (!s.empty() && s.back() == '_')
        s.pop_back();
      if (!stripped)
        break;
    }
  };

  peel(typeSuffixes, std::size(typeSuffixes));

  // If what is left already names a function, stop here. Without this check the
  // bare-"f" rule below would reduce "sqrtdf" to "sqrtd" and lose the match.
  if (MpfrRef::lookup(s))
    return s;

  // Only now treat a trailing "f" as the float suffix, and re-check after each
  // one so "sinbf" resolves rather than stalling on the inner "bf".
  static const char *bareSuffixes[] = {"f", "bf"};
  peel(bareSuffixes, std::size(bareSuffixes));
  return s;
}

namespace {

// ------------------------------------------------- unary/binary adapt wrappers

// Lift MPFR's one-, two- and three-operand functions into a single uniform
// shape so they can all live in one table.
#define UNARY(name, fn)                                                       \
  int name(mpfr_ptr r, mpfr_srcptr a, mpfr_srcptr, mpfr_srcptr,               \
          mpfr_rnd_t rnd) {                                                  \
    return fn(r, a, rnd);                                                    \
  }

#define BINARY(name, fn)                                                      \
  int name(mpfr_ptr r, mpfr_srcptr a, mpfr_srcptr b, mpfr_srcptr,             \
           mpfr_rnd_t rnd) {                                                 \
    return fn(r, a, b, rnd);                                                 \
  }

#define TERNARY(name, fn)                                                     \
  int name(mpfr_ptr r, mpfr_srcptr a, mpfr_srcptr b, mpfr_srcptr c,           \
           mpfr_rnd_t rnd) {                                                 \
    return fn(r, a, b, c, rnd);                                              \
  }

TERNARY(t_fma, mpfr_fma)

UNARY(u_sqrt, mpfr_sqrt)
UNARY(u_cbrt, mpfr_cbrt)
UNARY(u_exp, mpfr_exp)
UNARY(u_exp2, mpfr_exp2)
UNARY(u_exp10, mpfr_exp10)
UNARY(u_expm1, mpfr_expm1)
UNARY(u_log, mpfr_log)
UNARY(u_log2, mpfr_log2)
UNARY(u_log10, mpfr_log10)
UNARY(u_log1p, mpfr_log1p)
UNARY(u_sin, mpfr_sin)
UNARY(u_cos, mpfr_cos)
UNARY(u_tan, mpfr_tan)
UNARY(u_asin, mpfr_asin)
UNARY(u_acos, mpfr_acos)
UNARY(u_atan, mpfr_atan)
UNARY(u_sinh, mpfr_sinh)
UNARY(u_cosh, mpfr_cosh)
UNARY(u_tanh, mpfr_tanh)
UNARY(u_asinh, mpfr_asinh)
UNARY(u_acosh, mpfr_acosh)
UNARY(u_atanh, mpfr_atanh)
UNARY(u_rint, mpfr_rint)
UNARY(u_erf, mpfr_erf)
UNARY(u_erfc, mpfr_erfc)
UNARY(u_j0, mpfr_j0)
UNARY(u_j1, mpfr_j1)
UNARY(u_y0, mpfr_y0)
UNARY(u_y1, mpfr_y1)
UNARY(u_digamma, mpfr_digamma)
UNARY(u_zeta, mpfr_zeta)

BINARY(b_pow, mpfr_pow)
BINARY(b_atan2, mpfr_atan2)
BINARY(b_fmod, mpfr_fmod)
BINARY(b_remainder, mpfr_remainder)
BINARY(b_hypot, mpfr_hypot)
BINARY(b_copysign, mpfr_copysign)

#undef UNARY
#undef BINARY
#undef TERNARY

// ------------------------------------------------------ hand-written entries

// mpfr_floor/ceil/round/trunc are two-argument macros with a hard-wired
// rounding mode, so they cannot go through the UNARY adapter. These are the C
// semantics: round() is half away from zero, not half to even.
int refFloor(mpfr_ptr r, mpfr_srcptr a, mpfr_srcptr, mpfr_srcptr, mpfr_rnd_t) {
  return mpfr_floor(r, a);
}
int refCeil(mpfr_ptr r, mpfr_srcptr a, mpfr_srcptr, mpfr_srcptr, mpfr_rnd_t) {
  return mpfr_ceil(r, a);
}
int refRound(mpfr_ptr r, mpfr_srcptr a, mpfr_srcptr, mpfr_srcptr, mpfr_rnd_t) {
  return mpfr_round(r, a);
}
int refTrunc(mpfr_ptr r, mpfr_srcptr a, mpfr_srcptr, mpfr_srcptr, mpfr_rnd_t) {
  return mpfr_trunc(r, a);
}

// mpfr_roundeven takes no rounding mode: it is round-half-to-even by
// definition.
int refRoundeven(mpfr_ptr r, mpfr_srcptr a, mpfr_srcptr, mpfr_srcptr, mpfr_rnd_t) {
  return mpfr_roundeven(r, a);
}

// lgamma and tgamma report the sign of Gamma separately, because Gamma can be
// negative where the result itself is perfectly well defined. Both MPFR
// routines already store the value we want in rop; the sign output is simply
// discarded here.
//
// Forcing a NaN whenever Gamma goes negative would disagree with every real
// libc: lgamma(-0.5) is log(2*sqrt(pi)) = 1.2655, a perfectly finite answer,
// not a NaN.
int refLgamma(mpfr_ptr rop, mpfr_srcptr op, mpfr_srcptr, mpfr_srcptr,
              mpfr_rnd_t rnd) {
  int sign = 0;
  mpfr_lgamma(rop, &sign, op, rnd);
  return sign;
}

int refTgamma(mpfr_ptr rop, mpfr_srcptr op, mpfr_srcptr, mpfr_srcptr,
              mpfr_rnd_t rnd) {
  // mpfr_gamma is already correctly signed, including the +/-inf at the poles.
  return mpfr_gamma(rop, op, rnd);
}

// This MPFR version has no mpfr_abs.
int refFabs(mpfr_ptr rop, mpfr_srcptr op, mpfr_srcptr, mpfr_srcptr,
        mpfr_rnd_t rnd) {
  return mpfr_abs(rop, op, rnd);
}

// nearbyint and the *rint family differ from rint only in whether they raise
// inexact, which this tool does not model. The value is identical.
int refRint(mpfr_ptr rop, mpfr_srcptr op, mpfr_srcptr, mpfr_srcptr,
        mpfr_rnd_t rnd) {
  return mpfr_rint(rop, op, rnd);
}

// logb/ilogb: floor(log2(x)) as an exactly representable value. mpfr_get_exp
// returns e with x = m * 2^e and 0.5 <= m < 1, hence floor(log2 x) = e - 1.
int refLogb(mpfr_ptr rop, mpfr_srcptr op, mpfr_srcptr, mpfr_srcptr,
        mpfr_rnd_t rnd) {
  if (mpfr_nan_p(op)) {
    mpfr_set_nan(rop);
    return 0;
  }
  if (mpfr_zero_p(op)) {
    mpfr_set_inf(rop, -1); // logb(0) is -inf
    return 0;
  }
  if (mpfr_inf_p(op)) {
    mpfr_set(rop, op, rnd); // logb(inf) is inf
    return 0;
  }
  mpfr_set_si(rop, static_cast<long>(mpfr_get_exp(op) - 1), rnd);
  return 0;
}

// fmin/fmax/fdim are exact operations, so implementing them with comparisons is
// not an approximation.
int refFmin(mpfr_ptr rop, mpfr_srcptr a, mpfr_srcptr b, mpfr_srcptr, mpfr_rnd_t rnd) {
  if (mpfr_nan_p(a)) {
    if (mpfr_nan_p(b))
      mpfr_set_nan(rop);
    else
      mpfr_set(rop, b, rnd);
    return 0;
  }
  if (mpfr_nan_p(b)) {
    mpfr_set(rop, a, rnd);
    return 0;
  }
  mpfr_set(rop, mpfr_cmp(a, b) <= 0 ? a : b, rnd);
  return 0;
}

int refFmax(mpfr_ptr rop, mpfr_srcptr a, mpfr_srcptr b, mpfr_srcptr, mpfr_rnd_t rnd) {
  if (mpfr_nan_p(a)) {
    if (mpfr_nan_p(b))
      mpfr_set_nan(rop);
    else
      mpfr_set(rop, b, rnd);
    return 0;
  }
  if (mpfr_nan_p(b)) {
    mpfr_set(rop, a, rnd);
    return 0;
  }
  mpfr_set(rop, mpfr_cmp(a, b) >= 0 ? a : b, rnd);
  return 0;
}

int refFdim(mpfr_ptr rop, mpfr_srcptr a, mpfr_srcptr b, mpfr_srcptr, mpfr_rnd_t rnd) {
  if (mpfr_nan_p(a) || mpfr_nan_p(b)) {
    mpfr_set_nan(rop);
    return 0;
  }
  if (mpfr_cmp(a, b) <= 0) {
    mpfr_set_zero(rop, 1); // fdim(x, y) is +0 when x <= y
    return 0;
  }
  return mpfr_sub(rop, a, b, rnd);
}

const MpfrRef::Entry kTable[] = {
    // 1-argument
    {"sqrt", 1, u_sqrt},
    {"cbrt", 1, u_cbrt},
    {"exp", 1, u_exp},
    {"exp2", 1, u_exp2},
    {"exp10", 1, u_exp10},
    {"expm1", 1, u_expm1},
    {"log", 1, u_log},
    {"log2", 1, u_log2},
    {"log10", 1, u_log10},
    {"log1p", 1, u_log1p},
    {"logb", 1, refLogb},
    {"ilogb", 1, refLogb},
    {"sin", 1, u_sin},
    {"cos", 1, u_cos},
    {"tan", 1, u_tan},
    {"asin", 1, u_asin},
    {"acos", 1, u_acos},
    {"atan", 1, u_atan},
    {"sinh", 1, u_sinh},
    {"cosh", 1, u_cosh},
    {"tanh", 1, u_tanh},
    {"asinh", 1, u_asinh},
    {"acosh", 1, u_acosh},
    {"atanh", 1, u_atanh},
    {"fabs", 1, refFabs},
    {"floor", 1, refFloor},
    {"ceil", 1, refCeil},
    {"round", 1, refRound},
    {"trunc", 1, refTrunc},
    {"rint", 1, u_rint},
    {"nearbyint", 1, refRint},
    {"roundeven", 1, refRoundeven},
    {"lrint", 1, refRint},
    {"llrint", 1, refRint},
    {"lround", 1, refRint},
    {"llround", 1, refRint},
    {"lgamma", 1, refLgamma},
    {"tgamma", 1, refTgamma},
    {"digamma", 1, u_digamma},
    {"zeta", 1, u_zeta},
    {"erf", 1, u_erf},
    {"erfc", 1, u_erfc},
    {"j0", 1, u_j0},
    {"j1", 1, u_j1},
    {"y0", 1, u_y0},
    {"y1", 1, u_y1},

    // 2-argument
    {"pow", 2, b_pow},
    {"atan2", 2, b_atan2},
    {"fmod", 2, b_fmod},
    {"remainder", 2, b_remainder},
    {"hypot", 2, b_hypot},
    {"fmin", 2, refFmin},
    {"fmax", 2, refFmax},
    {"fdim", 2, refFdim},
    {"copysign", 2, b_copysign},

    // 3-argument
    {"fma", 3, t_fma},
};

// Spellings that survive suffix stripping but still name one of the functions
// above. Explicit beats heuristic: a symbol we cannot vouch for should be
// reported as unknown rather than compared against the wrong function.
struct Alias {
  const char *alias;
  const MpfrRef::Entry *target;
};

const Alias kAliases[] = {
    {"sqrtdf", &kTable[0]}, // sqrt
    {"fsqrt", &kTable[0]},  // sqrt
    {"sqrte", &kTable[0]},  // sqrt, left behind by an unstripped fp8 suffix
};

} // namespace

const MpfrRef::Entry *MpfrRef::lookup(const std::string &base) {
  for (const auto &e : kTable)
    if (base == e.base)
      return &e;
  for (const auto &a : kAliases)
    if (base == a.alias)
      return a.target;
  return nullptr;
}

std::string MpfrRef::resolveBase(const std::string &symbol) {
  const std::string b = baseName(symbol);
  return lookup(b) ? b : std::string();
}

std::vector<std::string> MpfrRef::baseNames() {
  std::vector<std::string> v;
  v.reserve(std::size(kTable));
  for (const auto &e : kTable)
    v.emplace_back(e.base);
  std::sort(v.begin(), v.end());
  return v;
}

bool MpfrRef::knownBase(const std::string &base, int *argsOut) {
  const Entry *e = lookup(base);
  if (!e)
    return false;
  if (argsOut)
    *argsOut = e->args;
  return true;
}

double evaluateReference(const std::string &base, const double *in, int nargs,
                         int workPrec, MpfrRounder &rounder) {
  const std::string b = baseName(base);
  const MpfrRef::Entry *e = MpfrRef::lookup(b);
  if (!e)
    return rounder.round(NAN);

  // Operands at full working precision. These are exact, because every input
  // ulpscope feeds in came out of a float format with at most 24 significand
  // bits, far below `workPrec`.
  mpfr_t a[3];
  mpfr_t result;
  mpfr_init2(result, workPrec);
  for (int i = 0; i < 3; ++i)
    mpfr_init2(a[i], workPrec);

  const int n = std::min(nargs, e->args);
  for (int i = 0; i < n; ++i)
    mpfr_set_d(a[i], in[i], MPFR_RNDN);
  for (int i = n; i < e->args; ++i)
    mpfr_set_d(a[i], 0.0, MPFR_RNDN);

  e->impl(result, a[0], a[1], a[2], MPFR_RNDN);
  // Round the high-precision result directly into the target format. Going via
  // mpfr_get_d() first would round twice.
  const double out = rounder.roundFromMpfr(result);

  for (int i = 0; i < 3; ++i)
    mpfr_clear(a[i]);
  mpfr_clear(result);
  return out;
}

} // namespace ulpscope