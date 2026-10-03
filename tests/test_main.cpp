// Headless smoke test: load the demo library, scan, check the numbers look
// sane. Built as part of the normal build and run by `ctest`.
#include <cstdio>
#include <cmath>
#include <cstring>

#include "core/dynlib.h"
#include "core/exporter.h"
#include "core/format.h"
#include "core/mpfr_util.h"
#include "core/mpfrref.h"
#include "core/scan.h"
#include "core/sweep.h"

using namespace ulpscope;

static int failures = 0;
static int checks = 0;

static void check(bool ok, const char *what) {
  ++checks;
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

#ifndef ULPSCOPE_DEMO_SO
#define ULPSCOPE_DEMO_SO "ulpscope_demo"
#endif

static void testFormats() {
  const FloatFormat bf = FloatFormat::bf16();
  check(bf.decode(0x3F80) == 1.0, "bf16 1.0");
  check(encodeRounded(1.0, bf) == 0x3F80, "bf16 encode 1.0");
  check(bf.infinity() == 0x7F80, "bf16 infinity");
  check(bf.negativeInfinity() == 0xFF80, "bf16 negative infinity");
  check(bf.canonicalNaN() == 0x7FC0, "bf16 canonical NaN has no sign bit");
  check(bf.orderedKey(0x8000) == bf.orderedKey(0x0000), "bf16 -0 equals +0");

  const FloatFormat f16 = FloatFormat::f16();
  check(f16.decode(0x3C00) == 1.0, "f16 1.0");
  check(f16.infinity() == 0x7C00, "f16 infinity");
  check(f16.decode(0x0001) == std::ldexp(1.0, -24), "f16 smallest denormal");

  const FloatFormat f32 = FloatFormat::f32();
  check(f32.infinity() == 0x7F800000u, "f32 infinity");
  check(f32.inputCount() == (1ull << 32), "f32 input count");
  check(f32.decode(1u) == std::ldexp(1.0, -149), "f32 smallest denormal");

  // Rounding corners that a hand-rolled encoder usually gets wrong.
  check(encodeRounded(0x1.FEp127, bf) == 0x7F7F, "bf16 max finite encodes");
  check(encodeRounded(0x1.FFp127, bf) == 0x7F80,
        "bf16 overflow tie rounds to infinity");
  check(encodeRounded(1e40, bf) == 0x7F80, "bf16 overflow");
  check(encodeRounded(0x1p-133, bf) == 0x0001, "bf16 smallest denormal");
  check(encodeRounded(0x1p-134, bf) == 0x0000,
        "bf16 half a denormal ties to even, giving zero");
  check(encodeRounded(0x1.8p-133, bf) == 0x0002,
        "bf16 1.5 denormals tie to even, giving two");
}

// The whole point of the tool is that the 16-bit formats are small enough to
// check completely, so the encoder and decoder are verified exhaustively.
static void testExhaustiveRoundTrip() {
  for (const FloatFormat &fmt : FloatFormat::all()) {
    if (fmt.totalBits > 16)
      continue; // f32 has 4 billion inputs; sampled separately

    const uint64_t n = fmt.inputCount();
    uint64_t bad = 0;
    for (uint64_t b = 0; b < n; ++b) {
      const double v = fmt.decode(b);
      if (std::isnan(v))
        continue; // NaN payloads are not preserved by design
      if (encodeRounded(v, fmt) != b)
        ++bad;
    }
    check(bad == 0, "exhaustive decode/encode round trip");

    // ULP spacing must equal the actual gap to the next representable value.
    uint64_t badUlp = 0;
    for (uint64_t b = 1; b < n; ++b) {
      if (fmt.classify(b) != Class::Normal || fmt.classify(b + 1) != Class::Normal)
        continue;
      if ((b & fmt.signMask()) != ((b + 1) & fmt.signMask()))
        continue; // do not walk across the sign boundary
      if (fmt.ulpOf(b) != std::fabs(fmt.decode(b + 1) - fmt.decode(b)))
        ++badUlp;
    }
    check(badUlp == 0, "ulpOf matches the gap to the next value");
  }

  // f32 cannot be swept, so sample it.
  const FloatFormat f32 = FloatFormat::f32();
  uint64_t bad = 0;
  uint32_t x = 12345;
  for (long i = 0; i < 200000; ++i) {
    x = x * 1664525u + 1013904223u;
    const double v = f32.decode(x);
    if (std::isnan(v))
      continue;
    if (encodeRounded(v, f32) != x)
      ++bad;
  }
  check(bad == 0, "sampled f32 round trip");

  // -0 and +0 are one point, and one denormal step either side of it is 1 ULP.
  const FloatFormat bf = FloatFormat::bf16();
  check(bf.orderedKey(0x8000) - bf.orderedKey(0x8001) == 1,
        "one ULP from -0 to -denormal");
  check(bf.orderedKey(0x0001) - bf.orderedKey(0x8001) == 2,
        "two ULP from -denormal to +denormal across zero");
}

// The registry is data, so adding a format must not disturb anything else.
static void testRegistry() {
  const auto &all = FloatFormat::all();
  check(!all.empty(), "registry is populated");
  for (const FloatFormat &f : all)
    check(f.valid(), "every registered format is self-consistent");

  // Names must be unique, or byName() would silently shadow one format.
  for (size_t i = 0; i < all.size(); ++i)
    for (size_t j = i + 1; j < all.size(); ++j)
      check(std::string(all[i].name) != std::string(all[j].name),
            "format names are unique");

  // Spot-check the values a hand-written table could plausibly get wrong.
  const FloatFormat *f64 = FloatFormat::byName("f64");
  check(f64 && f64->mantBits == 52 && f64->bias == 1023, "f64 layout");
  check(f64 && f64->decode(0x3FF0000000000000ull) == 1.0, "f64 1.0");
  check(f64 && f64->infinity() == 0x7FF0000000000000ull, "f64 infinity");
  check(f64 && f64->decode(1ull) == std::ldexp(1.0, -1074), "f64 min denormal");

  const FloatFormat *e5m2 = FloatFormat::byName("e5m2");
  check(e5m2 && e5m2->inputCount() == 256, "e5m2 has 256 inputs");
  check(e5m2 && e5m2->maxFinite() == 57344.0, "e5m2 max finite");

  // e4m3 as llvm-libc spells it is the S.8M3 layout: no infinities, and the
  // largest finite magnitude is (2 - 2^-3) * 2^8 = 240.
  const FloatFormat *e4m3 = FloatFormat::byName("e4m3");
  check(e4m3 && e4m3->inputCount() == 256, "e4m3 has 256 inputs");
  check(e4m3 && e4m3->maxFinite() == 240.0, "e4m3 max finite");
  check(e4m3 && e4m3->bias == 7, "e4m3 bias");

  check(FloatFormat::byName("nonesuch") == nullptr, "unknown format name");
}

static void testInputFilter() {
  const FloatFormat bf = FloatFormat::bf16();

  InputFilter all;
  check(all.isEverything(), "default filter admits everything");
  check(all.admits(bf, 0x3F80), "default filter admits a normal");
  check(all.admits(bf, 0x0001), "default filter admits a denormal");
  check(all.admits(bf, 0x0000), "default filter admits zero");
  check(all.admits(bf, 0x7FC0), "default filter admits NaN");

  InputFilter normalsOnly;
  normalsOnly.includeSubnormal = false;
  normalsOnly.includeZero = false;
  normalsOnly.includeInfinity = false;
  normalsOnly.includeNaN = false;
  check(!normalsOnly.isEverything(), "a narrowed filter is not everything");
  check(normalsOnly.admits(bf, 0x3F80), "normal-only admits a normal");
  check(!normalsOnly.admits(bf, 0x0001), "normal-only rejects a denormal");
  check(!normalsOnly.admits(bf, 0x0000), "normal-only rejects zero");
  check(!normalsOnly.admits(bf, 0x7F80), "normal-only rejects infinity");

  // Exponent restriction: bf16 normals span -126..127.
  InputFilter nearOne;
  nearOne.includeSubnormal = false;
  nearOne.includeZero = false;
  nearOne.includeInfinity = false;
  nearOne.includeNaN = false;
  nearOne.restrictExponent = true;
  nearOne.minExponent = 0;
  nearOne.maxExponent = 0;
  check(nearOne.admits(bf, 0x3F80), "exp 0 admits 1.0");
  check(!nearOne.admits(bf, 0x4000), "exp 0 rejects 2.0");
  check(!nearOne.admits(bf, 0x0001), "exp filter rejects denormals too");

  // The filter must actually shape an enumerated input set.
  ScanConfig cfg;
  cfg.fmt = bf;
  cfg.mode = ScanMode::Exhaustive;
  check(enumerateInputs(cfg).size() == 65536, "unfiltered sweep is complete");

  cfg.filter = normalsOnly;
  const uint64_t n = enumerateInputs(cfg).size();
  check(n > 0 && n < 65536, "normal-only sweep is narrower");
  // 65536 total - 254 denormals - 2 zeros - 2 infinities - 254 NaNs.
  check(n == 65024, "normal-only sweep count");
}

static void testTolerance() {
  DynamicLibrary lib;
  std::string err;
  if (!lib.load(ULPSCOPE_DEMO_SO, &err))
    return;
  void *fn = nullptr;
  if (!lib.resolve("lgammabf16", &fn))
    return;

  // lgammabf16 is wrong by at most 1 ULP, so a tolerance of 1 should clear it
  // while the default of 0 should not. This is the check that makes a tolerance
  // setting meaningful rather than cosmetic.
  ScanConfig strict;
  strict.fmt = FloatFormat::bf16();
  strict.symbol = "lgammabf16";
  strict.conv = CallConv::Native16;
  const ScanResult tight = Scanner::run(strict, lib, fn);
  check(tight.ok, "strict scan ran");
  check(tight.maxUlp == 1, "the demo defect is 1 ULP");
  check(tight.mismatchCount > 0, "correctly rounded is too strict for it");

  ScanConfig loose = strict;
  loose.toleranceUlp = 1;
  const ScanResult tolerant = Scanner::run(loose, lib, fn);
  check(tolerant.ok, "tolerant scan ran");
  check(tolerant.mismatchCount == 0, "a 1 ULP tolerance admits the defect");
  check(tolerant.total == tight.total, "tolerance does not change coverage");
}

// fp8 is the reason the registry is data: llvm-libc carries these types, and
// each has only 256 inputs so the whole domain is trivially checkable.
static void testNarrowFormats() {
  for (const FloatFormat &fmt : FloatFormat::all()) {
    if (fmt.totalBits != 8)
      continue;

    check(fmt.inputCount() == 256, "an 8-bit format has 256 inputs");

    // Exhaustive: decode/encode must be a perfect identity for every value.
    uint64_t bad = 0;
    for (uint64_t b = 0; b < 256; ++b) {
      const double v = fmt.decode(b);
      if (std::isnan(v))
        continue;
      if (encodeRounded(v, fmt) != b)
        ++bad;
    }
    check(bad == 0, "exhaustive fp8 round trip");

    // The exponent range must bracket the format's own limits exactly.
    check(fmt.decode(fmt.infinity()) ==
              std::numeric_limits<double>::infinity(),
          "infinity decodes to infinity");
    check(encodeRounded(fmt.maxFinite(), fmt) ==
              fmt.expField() | fmt.fracMask(),
          "max finite round trips");
    check(encodeRounded(fmt.maxFinite() * 2, fmt) == fmt.infinity(),
          "2x max finite overflows");

    // A scan of the whole format must work end to end, using the fp8 symbols
    // the demo actually exports.
    DynamicLibrary lib;
    std::string err;
    if (!lib.load(ULPSCOPE_DEMO_SO, &err))
      continue;

    const bool isE4m3 = std::string(fmt.name) == "e4m3";
    const char *symbol = isE4m3 ? "sqrte4m3" : "loge5m2";
    void *fn = nullptr;
    if (!lib.resolve(symbol, &fn))
      continue;

    ScanConfig cfg;
    cfg.fmt = fmt;
    cfg.symbol = symbol;
    cfg.conv = CallConv::Integer8;
    const ScanResult r = Scanner::run(cfg, lib, fn);
    check(r.ok, "fp8 scan runs");
    check(r.total == 256, "fp8 scan covers every input");

    if (isE4m3) {
      check(r.mismatchCount == 0, "sqrte4m3 is correct at every input");
      check(r.maxUlp == 0, "sqrte4m3 has no error");
    } else {
      check(r.mismatchCount > 0, "double-rounded loge5m2 is caught");
    }
  }
}

// The ULP distance must not be computed at 64 bits, or a large error in a
// 64-bit format would wrap into a small one.
static void testWideUlpDistance() {
  const FloatFormat f64 = FloatFormat::float64();

  // Largest negative finite and largest positive finite are far apart in the
  // real line, and the distance must be positive and enormous, not wrapped.
  const uint64_t negMax = f64.negativeInfinity() - 1;
  const uint64_t posMax = f64.infinity() - 1;
  const uint64_t d = ulpDistance(f64, negMax, posMax);
  check(d > 0, "f64 cross-zero ULP distance does not wrap to zero");
  check(d > (1ull << 62), "f64 cross-zero distance is enormous");

  // Adjacent values one ULP apart.
  check(ulpDistance(f64, 0x3FF0000000000000ull, 0x3FF0000000000001ull) == 1,
        "adjacent f64 values are 1 ULP apart");
  check(ulpDistance(f64, 0x3FF0000000000000ull, 0x3FF0000000000000ull) == 0,
        "equal f64 values are 0 ULP apart");
  check(ulpDistance(f64, 0x0000000000000000ull, 0x8000000000000000ull) == 0,
        "f64 signed zeros are the same point");
}

static void testSweep(const char *so) {
  const std::vector<std::string> symbols{
      "sqrtbf16", "lgammabf16", "fabsbf16", "expbf16"};
  ScanConfig cfg;
  cfg.fmt = FloatFormat::bf16();
  cfg.conv = CallConv::Native16;

  const auto results = LibrarySweep::run(so, symbols, cfg);
  check(results.size() == symbols.size(), "sweep reports every function");

  for (const FunctionSummary &s : results) {
    check(s.ok, "each swept function ran");
    check(!s.baseName.empty(), "each swept function resolved a reference");
  }

  // The known-good and known-bad functions must be distinguished.
  const FunctionSummary *sqrtRow = nullptr;
  const FunctionSummary *lgammaRow = nullptr;
  for (const FunctionSummary &s : results) {
    if (s.symbol == "sqrtbf16")
      sqrtRow = &s;
    if (s.symbol == "lgammabf16")
      lgammaRow = &s;
  }
  check(sqrtRow && sqrtRow->mismatchCount == 0, "sweep finds sqrt correct");
  check(lgammaRow && lgammaRow->mismatchCount > 0, "sweep finds lgammabf16 wrong");
  check(lgammaRow && lgammaRow->maxUlp == 1, "sweep records the worst ULP");

  // Per-class counts must add up to the total.
  for (const FunctionSummary &s : results) {
    uint64_t sum = 0;
    for (uint64_t v : s.mismatchesPerClass)
      sum += v;
    check(sum == s.mismatchCount, "per-class mismatch counts add up");
  }

  // A symbol that is not there must be reported, not silently dropped.
  const auto missing =
      LibrarySweep::run(so, {std::string("definitely_not_here")}, cfg);
  check(missing.size() == 1 && !missing[0].ok, "missing symbol is reported");
  check(missing[0].error == "symbol not found", "missing symbol says why");
}

// Auto-detection has to get the common cases right, and must decline rather
// than guess when the name is ambiguous: a wrong ABI produces garbage silently.
static void testConventionAndFormatGuessing() {
  struct Case {
    const char *symbol;
    CallConv conv;
  };
  const Case cases[] = {
      {"sqrtbf16", CallConv::Native16},
      {"lgammabf16", CallConv::Native16},
      {"exp_f16", CallConv::Native16},
      {"sinf16", CallConv::Native16},
      {"sqrte4m3", CallConv::Integer8},
      {"loge5m2", CallConv::Integer8},
      {"sqrt_f32", CallConv::Float},
      {"sinf", CallConv::Float},
      {"tgammaf", CallConv::Float},
      // Ambiguous or type-free: the tool must not invent a convention.
      {"sqrt", CallConv::Unknown},
      {"lgammabf16_d", CallConv::Double},
      {"powbf16_d", CallConv::Double},
      {"sqrtbf16_d", CallConv::Double},
      // A bare "d" must not be treated as a double suffix: "sqrtdf" is a
      // legacy spelling of sqrt, not a double-ABI function.
      {"sqrtdf", CallConv::Unknown},
      {"not_a_function", CallConv::Unknown},
  };
  for (const Case &c : cases)
    check(guessCallConv(c.symbol) == c.conv,
          (std::string("call convention for ") + c.symbol).c_str());
}

// Cross-check the encoder against the compiler's own conversion, which is an
// independent oracle. This is the test that catches subnormal rounding errors.
//
// It exists because of a real bug: rounding through MPFR's emin/emax gave
// 993.5 * 2^-24 for an f16 value where IEEE wants 993 * 2^-24, for *every*
// emin, because MPFR clamps the exponent but does not coarsen the significand
// grid the way IEEE does. A test that only checked decode/encode identity
// passed throughout, because it never looks at a value that is not already
// representable.
static void testAgainstCompilerRounding() {
  const FloatFormat bf = FloatFormat::bf16();
  const FloatFormat f16 = FloatFormat::f16();
  MpfrRounder rbf(bf), rf16(f16);

  uint32_t x = 987654321u;
  auto next = [&x] {
    x = x * 1664525u + 1013904223u;
    return x;
  };

  int bfBad = 0, f16Bad = 0;

  // Broad sweep of ordinary magnitudes.
  for (int i = 0; i < 200000; ++i) {
    const uint32_t bits = next();
    double v = std::ldexp(1.0 + (bits & 0xFFFFF) / 1048576.0,
                          static_cast<int>(bits % 60) - 30);
    if (next() & 1)
      v = -v;

    const uint64_t mine = encodeRounded(v, bf);
    const __bf16 theirs = static_cast<__bf16>(v);
    uint16_t tb;
    std::memcpy(&tb, &theirs, sizeof(tb));
    if (mine != tb) {
      if (bfBad < 4)
        std::printf("  bf16 %.17g: mine %04x, compiler %04x\n", v, mine, tb);
      ++bfBad;
    }
  }

  // The subnormal ranges specifically, where the old code was wrong. Values
  // just below the minimum normal, which is exactly where the grids differ.
  for (int i = 0; i < 200000; ++i) {
    const double scale = f16.subnormalGrid();
    // A random number of subnormal steps.
    const double v = (next() % 1024) * scale * ((next() & 1) ? -1.0 : 1.0);

    const uint64_t mine = encodeRounded(v, f16);
    const _Float16 theirs = static_cast<_Float16>(v);
    uint16_t tb;
    std::memcpy(&tb, &theirs, sizeof(tb));
    if (mine != tb) {
      if (f16Bad < 4)
        std::printf("  f16 denormal %.17g: mine %04x, compiler %04x\n", v, mine,
                    tb);
      ++f16Bad;
    }
  }

  check(bfBad == 0, "bf16 rounding agrees with the compiler on normals");
  check(f16Bad == 0, "f16 rounding agrees with the compiler on subnormals");

  // The exact case that was wrong, pinned so it cannot regress silently.
  check(encodeRounded(5.9212671107461366e-05, f16) == 0x03e1,
        "f16 subnormal rounds down, not up");
  check(encodeRounded(0x1p-25, f16) == 0x0000, "f16 half a subnormal ties to 0");
  check(encodeRounded(0x1.8p-24, f16) == 0x0002,
        "f16 1.5 subnormals tie to the even one");
  check(encodeRounded(-5.9212671107461366e-05, f16) == 0x83e1,
        "negative f16 subnormal keeps its sign");
  check(encodeRounded(-0x1p-133, bf) == 0x8001,
        "negative bf16 subnormal keeps its sign");
  check(encodeRounded(-0x1p-134, bf) == 0x8000,
        "negative half-subnormal ties to -0");
}

// A scan that quietly covers fewer inputs than asked for is worse than one
// that fails, so the shortfall has to be reported.
static void testInputSetShortfall() {
  const FloatFormat bf = FloatFormat::bf16();

  // A filter that admits almost nothing.
  ScanConfig cfg;
  cfg.fmt = bf;
  cfg.mode = ScanMode::Random;
  cfg.sampleCount = 1000;
  cfg.filter.includeNormal = false;
  cfg.filter.includeSubnormal = false;
  cfg.filter.includeInfinity = false;
  cfg.filter.includeNaN = false;
  cfg.filter.restrictExponent = true;
  cfg.filter.minExponent = -2000;
  cfg.filter.maxExponent = -2000;

  bool complete = false;
  const auto inputs = enumerateInputs(cfg, &complete);
  check(inputs.size() < cfg.sampleCount,
        "an impossible filter admits fewer inputs than requested");
  check(complete, "covering everything the filter admits counts as complete");
  for (uint64_t b : inputs)
    check(cfg.filter.admits(bf, b), "every enumerated input satisfies the filter");

  // Range mode must actually restrict.
  ScanConfig ranged;
  ranged.fmt = bf;
  ranged.mode = ScanMode::Range;
  ranged.lo = 0x3F00;
  ranged.hi = 0x3F80;
  check(enumerateInputs(ranged).size() == 0x80, "range mode honours lo and hi");

  // An unfiltered random scan must deliver exactly what was asked for.
  ScanConfig plain;
  plain.fmt = bf;
  plain.mode = ScanMode::Random;
  plain.sampleCount = 5000;
  complete = false;
  check(enumerateInputs(plain, &complete).size() == 5000,
        "random mode delivers the requested count");
  check(complete, "a full random scan is complete");
}

// A missing reference must be an error, not a scan that flags every input.
static void testMissingReferenceFails() {
  DynamicLibrary lib;
  std::string err;
  if (!lib.load(ULPSCOPE_DEMO_SO, &err))
    return;
  void *fn = nullptr;
  if (!lib.resolve("sqrtbf16", &fn))
    return;

  ScanConfig cfg;
  cfg.fmt = FloatFormat::bf16();
  cfg.symbol = "a_name_that_is_not_a_function";
  cfg.conv = CallConv::Native16;
  cfg.mode = ScanMode::Random;
  cfg.sampleCount = 1000;

  const ScanResult r = Scanner::run(cfg, lib, fn);
  check(!r.ok, "a scan with no reference fails rather than reporting mismatches");
  check(r.error.find("no MPFR reference") != std::string::npos,
        "the error says what is missing");
  check(r.mismatchCount == 0, "a failed scan reports no mismatches");
}

// Two-argument functions are only testable if the operands actually interact.
static void testNeighbouringArguments() {
  DynamicLibrary lib;
  std::string err;
  if (!lib.load(ULPSCOPE_DEMO_SO, &err))
    return;

  // fmax_double is correct except at the -0/+0 boundary the standard leaves
  // open. A fixed second argument never reaches that; a neighbouring one does.
  void *fn = nullptr;
  if (!lib.resolve("fmax_double", &fn))
    return;

  ScanConfig fixed;
  fixed.fmt = FloatFormat::bf16();
  fixed.symbol = "fmax_double";
  fixed.conv = CallConv::Double;
  fixed.nargs = 2;
  fixed.argMode = ArgMode::Fixed;
  fixed.fixedArgs[0] = 2.0;
  fixed.mode = ScanMode::Random;
  fixed.sampleCount = 20000;
  const ScanResult withFixed = Scanner::run(fixed, lib, fn);
  check(withFixed.ok, "fixed-argument scan ran");
  check(withFixed.mismatchCount == 0,
        "a correct fmax looks correct with a fixed second argument");

  ScanConfig near = fixed;
  near.argMode = ArgMode::Neighbouring;
  near.mode = ScanMode::Exhaustive;
  const ScanResult withNeighbours = Scanner::run(near, lib, fn);
  check(withNeighbours.ok, "neighbouring-argument scan ran");
  check(withNeighbours.total == 65536, "neighbouring mode is exhaustive");
  check(withNeighbours.mismatchCount > withFixed.mismatchCount,
        "neighbouring arguments reach cases a fixed argument cannot");

  // The deliberately wrong variants must be caught either way.
  for (const char *sym : {"fmaxf", "fdimf"}) {
    void *f = nullptr;
    if (!lib.resolve(sym, &f))
      continue;
    ScanConfig c = near;
    c.symbol = sym;
    c.conv = CallConv::Float;
    const ScanResult r = Scanner::run(c, lib, f);
    check(r.ok, "two-argument defect scan ran");
    check(r.mismatchCount > 0, "a wrong fmax or fdim is caught");
  }
}

// f64 was reachable but never exercised end to end.
static void testF64Scan() {
  DynamicLibrary lib;
  std::string err;
  if (!lib.load(ULPSCOPE_DEMO_SO, &err))
    return;
  const FloatFormat f64 = FloatFormat::float64();

  for (const char *sym : {"sqrt_double", "log_double"}) {
    void *fn = nullptr;
    if (!lib.resolve(sym, &fn))
      continue;
    ScanConfig cfg;
    cfg.fmt = f64;
    cfg.symbol = sym;
    cfg.conv = CallConv::Double;
    cfg.mode = ScanMode::Random;
    cfg.sampleCount = 20000;
    const ScanResult r = Scanner::run(cfg, lib, fn);
    check(r.ok, "f64 scan runs");
    check(r.total == 20000, "f64 scan covers the requested sample");
    check(r.mismatchCount == 0,
          "a correctly rounded double function has no f64 errors");
    check(r.maxUlp == 0, "f64 max error is zero when nothing is wrong");
  }

  // A symbol whose name does not reduce to a function, given a reference by
  // hand. Without the override it is uncheckable.
  void *fn = nullptr;
  if (lib.resolve("log2_viaf32", &fn)) {
    ScanConfig cfg;
    cfg.fmt = f64;
    cfg.symbol = "log2_viaf32";
    cfg.conv = CallConv::Double;
    cfg.mode = ScanMode::Random;
    cfg.sampleCount = 20000;
    cfg.referenceBase = "log2";
    const ScanResult r = Scanner::run(cfg, lib, fn);
    check(r.ok, "f64 scan with an explicit reference runs");
    check(r.mismatchCount > 0,
          "computing log2 through float is caught at f64 precision");
  }
}

// An exhaustive scan must be refused rather than attempted: one Sample per
// input over 2^32 inputs is tens of gigabytes, and an unguarded loop has taken
// this process out with SIGKILL before.
static void testExhaustiveIsGuarded() {
  const FloatFormat f32 = FloatFormat::f32();
  const FloatFormat f64 = FloatFormat::float64();
  const FloatFormat bf = FloatFormat::bf16();

  ScanConfig cfg;
  cfg.fmt = f32;
  cfg.symbol = "sqrtbf16";
  cfg.conv = CallConv::Native16;
  cfg.mode = ScanMode::Exhaustive;

  check(cfg.wouldExhaustMemory(), "an exhaustive f32 scan is refused");
  check(!([&] {
          ScanConfig c = cfg;
          c.fmt = bf;
          return c.wouldExhaustMemory();
        }()),
        "an exhaustive bf16 scan is allowed");

  // The counting helper must answer without materialising anything, which is
  // the whole point of using it for the "does this filter match anything"
  // check.
  ScanConfig narrow = cfg;
  narrow.fmt = bf;
  narrow.filter.includeNormal = false;
  narrow.filter.includeSubnormal = false;
  narrow.filter.includeInfinity = false;
  narrow.filter.includeNaN = false;
  narrow.filter.includeZero = false; // zeros are included by default
  check(countAdmittedInputs(narrow, 1) == 0,
        "countAdmittedInputs reports an empty filter");
  check(countAdmittedInputs(narrow, 10) == 0, "and stays at zero");

  ScanConfig some = narrow;
  some.filter.includeNormal = true;
  some.filter.includeZero = true;
  some.filter.restrictExponent = true;
  some.filter.minExponent = 0;
  some.filter.maxExponent = 0;
  check(countAdmittedInputs(some, 1) == 1,
        "countAdmittedInputs stops at the cap");

  // The core must refuse before allocating, so the call returns rather than
  // exhausting memory.
  DynamicLibrary lib;
  std::string err;
  if (!lib.load(ULPSCOPE_DEMO_SO, &err))
    return;
  void *fn = nullptr;
  if (!lib.resolve("sqrtbf16", &fn))
    return;
  const ScanResult r = Scanner::run(cfg, lib, fn);
  check(!r.ok, "an oversized exhaustive scan fails instead of running");
  check(r.error.find("random sample") != std::string::npos,
        "the refusal says what to do instead");
  check(r.samples.empty(), "and nothing was allocated");
}

static void testReference() {
  const FloatFormat bf = FloatFormat::bf16();
  MpfrRounder r(bf);

  const double one = 1.0;
  check(evaluateReference("sqrtbf16", &one, 1, 200, r) == 1.0, "sqrt(1) == 1");

  const double four = 4.0;
  check(evaluateReference("sqrtbf16", &four, 1, 200, r) == 2.0, "sqrt(4) == 2");

  const double zero = 0.0;
  check(std::isinf(evaluateReference("lgammabf16", &zero, 1, 200, r)),
        "lgamma(0) is +inf");

  // Gamma has a pole at every non-positive integer, so lgamma(-1) is +inf.
  const double neg = -1.0;
  check(std::isinf(evaluateReference("lgammabf16", &neg, 1, 200, r)) &&
            evaluateReference("lgammabf16", &neg, 1, 200, r) > 0,
        "lgamma(-1) is +inf");

  // lgamma is log|Gamma|, so it stays finite where Gamma is negative.
  // This is where a NaN-forcing wrapper would silently diverge from libc.
  const double neg_half = -0.5;
  // evaluateReference returns the result already rounded to bf16, so compare
  // against the bf16 value: log|Gamma(-0.5)| = log(2*sqrt(pi)) = 1.2655121...
  // rounds to 1.265625.
  const double lh = evaluateReference("lgammabf16", &neg_half, 1, 200, r);
  check(lh == 1.265625, "lgamma(-0.5) is finite and bf16-rounded");

  // tgamma keeps the sign of Gamma, unlike lgamma.
  // Gamma(-0.5) = -2*sqrt(pi) = -3.5449077..., which rounds to -3.546875.
  const double tg = evaluateReference("tgamma_bf16", &neg_half, 1, 200, r);
  check(tg == -3.546875, "tgamma(-0.5) is negative and keeps Gamma's sign");

  const double ln2 = 0.69314718055994530942;
  check(evaluateReference("expbf16", &ln2, 1, 200, r) == 2.0, "exp(ln 2) == 2");

  check(baseName("sqrtf") == "sqrt", "suffix strip f");
  check(baseName("lgammabf16") == "lgamma", "suffix strip bf16");
  check(baseName("log2f128") == "log2", "suffix strip f128");
  check(baseName("__sqrtdf") == "sqrtdf", "no false strip on df");
  check(baseName("powbf16_d") == "pow", "compound suffix stripping");
  check(MpfrRef::resolveBase("lgammabf16") == "lgamma", "resolveBase lgamma");
}

static void testLibrary(const char *soPath) {
  DynamicLibrary lib;
  std::string err;
  if (!lib.load(soPath, &err)) {
    std::printf("FAIL: cannot load %s: %s\n", soPath, err.c_str());
    ++failures;
    return;
  }
  check(lib.symbols().size() > 5, "demo exports a reasonable number of symbols");

  bool found = false;
  for (const auto &s : lib.symbols())
    if (s == "sqrtbf16")
      found = true;
  check(found, "sqrtbf16 is in the dynamic symbol table");
  check(DynamicLibrary::inferArgCount("lgammabf16") == 1, "arg count for lgamma");
  check(DynamicLibrary::inferArgCount("powbf16_d") == 2, "arg count for pow");
  check(DynamicLibrary::inferArgCount("not_a_function") == -1,
        "unknown symbol has no arg count");
}

static void testScan(const char *soPath) {
  DynamicLibrary lib;
  std::string err;
  if (!lib.load(soPath, &err))
    return;
  void *fn = nullptr;
  if (!lib.resolve("sqrtbf16", &fn)) {
    std::printf("FAIL: sqrtbf16 not resolved\n");
    ++failures;
    return;
  }

  ScanConfig cfg;
  cfg.fmt = FloatFormat::bf16();
  cfg.symbol = "sqrtbf16";
  cfg.conv = CallConv::Native16; // bf16 travels through the SSE registers
  cfg.nargs = 1;
  cfg.mode = ScanMode::Exhaustive;

  const ScanResult r = Scanner::run(cfg, lib, fn);
  if (!r.ok) {
    std::printf("FAIL: sqrtbf16 scan failed: %s\n", r.error.c_str());
    ++failures;
    return;
  }
  std::printf("  sqrtbf16 (native16): %llu/%llu mismatches, max %llu ULP\n",
              static_cast<unsigned long long>(r.mismatchCount),
              static_cast<unsigned long long>(r.total),
              static_cast<unsigned long long>(r.maxUlp));
  check(r.total == 65536, "exhaustive bf16 scan covers 65536 inputs");
  check(r.mismatchCount == 0, "correctly rounded sqrtbf16 has zero mismatches");

  // A deliberately truncating implementation must be caught.
  void *lg = nullptr;
  if (lib.resolve("lgammabf16", &lg)) {
    ScanConfig c2 = cfg;
    c2.symbol = "lgammabf16";
    const ScanResult r2 = Scanner::run(c2, lib, lg);
    check(r2.ok, "lgammabf16 scan ran");
    check(r2.mismatchCount > 0,
          "truncating lgammabf16 is reported as mismatching");
    std::printf("  lgammabf16 (truncating): %llu/%llu mismatches, max %llu ULP\n",
                static_cast<unsigned long long>(r2.mismatchCount),
                static_cast<unsigned long long>(r2.total),
                static_cast<unsigned long long>(r2.maxUlp));
    if (!r2.worst.empty())
      check(r2.worst.front()->mismatch, "worst list only holds mismatches");
  }

  // The double-convention wrapper of the same bad function must also be caught.
  void *lgd = nullptr;
  if (lib.resolve("lgammabf16_d", &lgd)) {
    ScanConfig c3 = cfg;
    c3.symbol = "lgammabf16_d";
    c3.conv = CallConv::Double;
    const ScanResult r3 = Scanner::run(c3, lib, lgd);
    check(r3.ok, "lgammabf16_d scan ran");
    check(r3.mismatchCount > 0, "double-convention wrapper is also flagged");
    std::printf("  lgammabf16_d (double conv): %llu/%llu mismatches, max %llu ULP\n",
                static_cast<unsigned long long>(r3.mismatchCount),
                static_cast<unsigned long long>(r3.total),
                static_cast<unsigned long long>(r3.maxUlp));
  }

  // Export must produce something non-trivial.
  ScanConfig c4 = cfg;
  c4.symbol = "lgammabf16";
  void *lg2 = nullptr;
  if (lib.resolve("lgammabf16", &lg2)) {
    const ScanResult r4 = Scanner::run(c4, lib, lg2);
    const ExportOptions eo;
    check(exportTable(r4, eo).find("lgammabf16") != std::string::npos,
          "export mentions the function");
    check(exportCsv(r4, 10).find("input_bits") == 0, "csv has a header");
  }
}

int main(int argc, char **argv) {
  const char *so = argc > 1 ? argv[1] : ULPSCOPE_DEMO_SO;
  std::printf("ulpscope tests (demo library: %s)\n", so);

  testFormats();
  testRegistry();
  testExhaustiveRoundTrip();
  testInputFilter();
  testNarrowFormats();
  testWideUlpDistance();
  testConventionAndFormatGuessing();
  testAgainstCompilerRounding();
  testReference();
  testTolerance();
  testInputSetShortfall();
  testMissingReferenceFails();
  testNeighbouringArguments();
  testF64Scan();
  testExhaustiveIsGuarded();
  testLibrary(so);
  testSweep(so);
  testScan(so);

  std::printf("%d/%d checks passed\n", checks - failures, checks);
  return failures ? 1 : 0;
}