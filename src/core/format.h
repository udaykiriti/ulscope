// IEEE-754 binary interchange format descriptions.
//
// Formats are described by data, not by code. Adding one is a row in
// formatSpecs(); everything downstream - the inspector's bit grid, the scan's
// MPFR rounding, the export tables - is derived from that row.
#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace ulpscope {

enum class Class {
  Zero,
  Normal,
  Subnormal,
  Infinity,
  NaN,
};

const char *className(Class c);

// One row of the format table. Kept as plain data so the registry can be built
// at runtime and extended without touching any logic.
struct FormatSpec {
  const char *name;
  int totalBits;
  int expBits;
  int mantBits; // *stored* fraction bits; the implicit bit is not included
  int bias;
  bool hasInfNan;
};

struct FloatFormat {
  const char *name; // "bf16"
  int totalBits;    // 16
  int expBits;      // 8
  int mantBits;     // 7
  int bias;         // 127
  bool hasInfNan;   // false only for formats like x87 80-bit

  // Named accessors for the formats people actually point this tool at.
  static FloatFormat bf16() { return {"bf16", 16, 8, 7, 127, true}; }
  static FloatFormat f16() { return {"f16", 16, 5, 10, 15, true}; }
  static FloatFormat f32() { return {"f32", 32, 8, 23, 127, true}; }
  static FloatFormat float64() { return {"f64", 64, 11, 52, 1023, true}; }
  static FloatFormat e4m3() { return {"e4m3", 8, 4, 3, 7, true}; }
  static FloatFormat e5m2() { return {"e5m2", 8, 5, 2, 15, true}; }

  // Every format the tool knows about, built from formatSpecs().
  static const std::vector<FloatFormat> &all();
  static const FloatFormat *byName(const std::string &name);
  // Row table behind all(), for callers that want to extend it.
  static const std::vector<FormatSpec> &specs();

  // Index into all() for the format a symbol name implies ("sqrtbf16" -> bf16),
  // or -1 when the name says nothing. The two 16-bit formats are told apart by
  // name and never by width, because they share a width but not an encoding.
  static int indexImpliedByName(const std::string &symbol);
  static int indexByName(const char *name);

  // A format is usable by this tool if it has an implicit leading bit, fits in
  // a uint64, and does not need an explicit integer bit.
  bool valid() const {
    return totalBits >= 4 && totalBits <= 64 && mantBits >= 1 && mantBits <= 63 &&
           expBits >= 1 && totalBits == expBits + mantBits + 1;
  }

  uint64_t mask(int width) const {
    return width >= 64 ? ~0ULL : ((1ULL << width) - 1);
  }

  uint64_t fracMask() const { return mask(mantBits); }
  uint64_t signMask() const { return 1ULL << (totalBits - 1); }
  // All-ones *exponent field value*, i.e. the field mask. Not positioned in the
  // bit pattern; use infinity() when you need an actual encoding.
  uint64_t expMask() const {
    return hasInfNan ? mask(expBits) : ~0ULL;
  }
  // The exponent field, shifted into place. This is what bit patterns use.
  uint64_t expField() const { return expMask() << mantBits; }
  uint64_t biasedZero() const { return 0; }
  // Quiet NaN, no sign bit.
  uint64_t canonicalNaN() const {
    return expField() | (1ULL << (mantBits - 1));
  }
  uint64_t infinity() const { return expField(); }
  uint64_t negativeInfinity() const { return expField() | signMask(); }

  // Precision of a normal value: 1 implicit bit plus the stored ones.
  int precision() const { return 1 + mantBits; }

  // Unbiased exponent of the smallest normal value.
  int minExponent() const { return 1 - bias; }
  // Unbiased exponent of the largest finite value.
  int maxExponent() const { return static_cast<int>(expMask()) - 1 - bias; }
  // Spacing at the bottom of the subnormal range, 2^(1 - bias - mantBits).
  double subnormalGrid() const {
    return std::ldexp(1.0, 1 - bias - mantBits);
  }

  // These are the IEEE quantities. They are deliberately *not* handed to MPFR
  // as emin/emax: MPFR clamps the exponent but does not coarsen the
  // significand grid for subnormal values the way IEEE does, so the subnormal
  // range is rounded explicitly. See MpfrRounder.

  // Real value of a bit pattern. Exact.
  double decode(uint64_t bits) const;

  Class classify(uint64_t bits) const;
  Class classifyReal(double v) const;

  // Spacing of the format near `bits`, as a real number. 0 if not finite.
  double ulpOf(uint64_t bits) const;

  // Maps a bit pattern into a monotonically increasing integer space so that
  // subtraction is ULP distance. 64-bit formats need the wide variant.
  unsigned __int128 orderedKeyWide(uint64_t bits) const;
  uint64_t orderedKey(uint64_t bits) const {
    return static_cast<uint64_t>(orderedKeyWide(bits));
  }

  // Number of distinct bit patterns, saturating at UINT64_MAX for f32+.
  uint64_t inputCount() const {
    return totalBits >= 64 ? ~0ULL : (1ULL << totalBits);
  }

  double minNormal() const;
  double maxFinite() const;

  std::string describe() const;
};

// Correctly rounded (round-to-nearest, ties-to-even) conversion from a real
// value to `fmt`'s bit pattern, handling subnormals, overflow to infinity and
// NaN canonicalisation. Routed through MPFR so there is exactly one rounding
// implementation in the program.
uint64_t encode(double v, const FloatFormat &fmt);

// Same, but returns the exact representable value as a double.
double encodeToDouble(double v, const FloatFormat &fmt);

Class classifyDouble(double v);

} // namespace ulpscope