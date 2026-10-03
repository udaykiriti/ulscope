// Reference implementations of the standard functions, evaluated in MPFR.
//
// The point of this file is that there is exactly one place that knows what
// the correctly rounded answer to, say, `lgammaf(3.5)` is. Everything else in
// ulpscope only compares against what comes out of here.
#pragma once

#include <mpfr.h>

#include <string>
#include <vector>

#include "core/format.h"
#include "core/mpfr_util.h"

namespace ulpscope {

// Strip type suffixes so one entry covers every width: "sqrtf", "sqrt",
// "sqrtbf16", "sqrt_bf16" and "sqrtf128" all resolve to base name "sqrt".
// Stripping is repeated, so compound spellings like "powbf16_d" also resolve.
std::string baseName(const std::string &symbol);

class MpfrRef {
public:
  // Uniform three-operand shape, with unused operands ignored, so unary,
  // binary and the genuinely ternary functions can all share one table.
  using Impl = int (*)(mpfr_ptr, mpfr_srcptr, mpfr_srcptr, mpfr_srcptr,
                       mpfr_rnd_t);

  struct Entry {
    const char *base;
    int args;
    Impl impl;
  };

  // nullptr if the function is not one we can provide a reference for.
  static const Entry *lookup(const std::string &base);

  // Every base name we can evaluate, for populating the UI.
  static std::vector<std::string> baseNames();

  // Resolves `symbol` (with suffixes) to a base name, or "" when unsupported.
  static std::string resolveBase(const std::string &symbol);

  static bool knownBase(const std::string &base, int *argsOut = nullptr);
};

// Computes base(in[0..nargs-1]) at `workPrec` bits of precision and returns the
// result already correctly rounded into `fmt`. Rounding happens here rather
// than in the caller so a double-rounded reference is impossible.
double evaluateReference(const std::string &base, const double *in, int nargs,
                         int workPrec, MpfrRounder &rounder);

} // namespace ulpscope