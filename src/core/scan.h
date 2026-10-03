// The scan: run a function over a set of inputs, compare every result against
// the correctly rounded MPFR reference, and record the ULP error.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/dynlib.h"
#include "core/format.h"

namespace ulpscope {

enum class ScanMode {
  Exhaustive, // every bit pattern (only sane for the narrow formats)
  Range,      // [lo, hi) bit patterns
  Random,     // a seeded random sample of bit patterns
};

const char *scanModeName(ScanMode m);

// How the arguments after the first are chosen. The first argument is always
// what varies; these decide what the others do.
enum class ArgMode {
  Fixed,      // whatever the caller set, held constant
  Randomised, // drawn from the same seed, uniformly over the format
  // Taken from the neighbourhood of the first argument. This is what makes
  // fmin, fmax, fdim and copysign testable at all: their defects live at
  // equality, at orderings, and at the signed-zero boundaries, and uniform
  // random pairs essentially never land there.
  Neighbouring,
};

const char *argModeName(ArgMode m);

// Which classes of input to visit. Used to narrow a scan to the part of the
// domain a bug is suspected in, since an exhaustive run is cheap enough that
// the interesting question is usually "what does this break on?".
struct InputFilter {
  bool includeZero = true;
  bool includeNormal = true;
  bool includeSubnormal = true;
  bool includeInfinity = true;
  bool includeNaN = true;

  // Restrict normals and subnormals to this unbiased exponent range. Inclusive
  // on both ends; use the format's own limits for "everything".
  bool restrictExponent = false;
  int minExponent = -1000;
  int maxExponent = 1000;

  bool admits(const FloatFormat &fmt, uint64_t bits) const;
  bool admitsExponent(int e) const {
    return e >= minExponent && e <= maxExponent;
  }
  bool isEverything() const;
  std::string describe() const;
};

struct ScanConfig {
  FloatFormat fmt = FloatFormat::bf16();
  std::string symbol;
  CallConv conv = CallConv::Double;
  int nargs = 1;
  // Values for the second and third arguments, used when argMode is Fixed.
  double fixedArgs[2] = {2.0, 0.0};
  ArgMode argMode = ArgMode::Fixed;
  // Offsets, in input steps, cycled through for the trailing arguments when
  // argMode is Neighbouring. Zero first, so equal operands are always covered.
  std::vector<int> neighbourOffsets{0, 1, -1, 2, -2};

  ScanMode mode = ScanMode::Exhaustive;
  uint64_t lo = 0;
  uint64_t hi = 0; // exclusive
  uint64_t sampleCount = 1u << 20;
  uint64_t seed = 0x9E3779B97F4A7C15ull;

  // Working precision for the reference.
  int workPrecBits = 200; // MPFR working precision for the reference
  int threads = 0;        // 0 => hardware_concurrency

  // Base name to evaluate instead of the one inferred from `symbol`. Empty
  // means infer it. This is the escape hatch for a symbol whose name does not
  // cleanly reduce to a known function - "log2_viaf32" is log2, but no amount
  // of suffix stripping gets there. Without this such a symbol can only be
  // checked by hand, and an unnoticed failure here looks like a function that
  // is wrong on every input.
  std::string referenceBase;

  InputFilter filter;

  // Largest error still counted as a pass. 0 means correctly rounded, which is
  // what most of these formats can actually achieve. Some functions genuinely
  // cannot be correctly rounded at every input, so a tolerance is often the
  // honest criterion.
  uint64_t toleranceUlp = 0;

  // True when the whole input space can be covered.
  bool isExhaustive() const { return mode == ScanMode::Exhaustive; }

  // An exhaustive scan materialises one Sample per input. Past this many inputs
  // that is gigabytes, so Scanner::run refuses it rather than attempting it and
  // letting the allocator decide. The UI asks the user to sample instead.
  static constexpr uint64_t kMaxExhaustiveInputs = 1ull << 24;
  bool wouldExhaustMemory() const {
    return mode == ScanMode::Exhaustive &&
           fmt.inputCount() > kMaxExhaustiveInputs;
  }
  uint64_t pointCount() const;
  // Human-readable summary of what will be scanned, for the status line.
  std::string describeRange() const;
};

struct Sample {
  uint64_t inputBits = 0;
  double input = 0.0;   // exact value of inputBits in `fmt`
  double raw = 0.0;     // what the function under test returned, before rounding
  double actual = 0.0;  // rounded into `fmt`
  double expected = 0.0;
  uint64_t actualBits = 0;
  uint64_t expectedBits = 0;
  uint64_t ulpError = 0;
  // Error exceeds the configured tolerance, regardless of whether it is zero.
  bool mismatch = false;
  // Set when one side is NaN and the other is not, so the UI can explain why
  // the ULP number is meaningless.
  bool nanDisagreement = false;
};

struct ScanResult {
  ScanConfig config;
  std::vector<Sample> samples;

  uint64_t total = 0;
  uint64_t mismatchCount = 0;
  uint64_t maxUlp = 0;
  uint64_t maxUlpAtBits = 0;
  double rmsUlp = 0.0; // sqrt(mean(ulp^2)) over finite ULP errors
  double meanUlp = 0.0;

  // Counts per input class, so a scan can report where errors actually live.
  uint64_t perClass[5] = {0, 0, 0, 0, 0}; // indexed by Class
  uint64_t mismatchesPerClass[5] = {0, 0, 0, 0, 0};
  // How many inputs exceeded 1, 2, 4, 16 and 1024 ULP.
  uint64_t overUlp[5] = {0, 0, 0, 0, 0};

  bool cancelled = false;
  bool ok = false;
  // False when the requested input set could not be produced in full - a
  // restrictive filter plus Random mode, for example. Silently scanning fewer
  // inputs than asked for and reporting success would be a quiet lie.
  bool inputSetComplete = true;
  uint64_t requestedInputs = 0;
  std::string error;

  // Samples with mismatch == true, worst first. Built by finalise().
  std::vector<const Sample *> worst;

  void finalise();
};

// ULP distance between two already-rounded values in `fmt`. NaN-aware: two
// NaNs are equal, a NaN against a number is flagged separately by the caller.
uint64_t ulpDistance(const FloatFormat &fmt, uint64_t actualBits,
                     uint64_t expectedBits);

class Scanner {
public:
  // Called from the worker thread with the number of points completed.
  using ProgressFn = std::function<void(uint64_t done, uint64_t total)>;

  // Runs synchronously. Safe to call from any thread.
  static ScanResult run(const ScanConfig &cfg, const DynamicLibrary &lib,
                        void *fn, const ProgressFn &progress = nullptr,
                        const std::atomic<bool> *cancel = nullptr);
};

// Samples the bit patterns a scan will visit, in order. Exposed so the plot and
// table can reason about the x axis without duplicating the mode logic.
//
// `complete` reports whether the full requested set was produced. Callers that
// care should check it rather than assuming: with a restrictive filter and
// Random mode the admitted set can be far smaller than requested.
std::vector<uint64_t> enumerateInputs(const ScanConfig &cfg,
                                      bool *complete = nullptr);

// Counts the inputs a scan would visit, stopping once it passes `cap` and
// returning `cap`. Cheaper and safer than enumerating them, which matters when
// the answer is only "is there anything here?".
uint64_t countAdmittedInputs(const ScanConfig &cfg, uint64_t cap);

} // namespace ulpscope