#include "core/scan.h"

#include "core/mpfr_util.h"
#include "core/mpfrref.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <thread>

namespace ulpscope {

const char *scanModeName(ScanMode m) {
  switch (m) {
  case ScanMode::Exhaustive: return "Exhaustive";
  case ScanMode::Range: return "Range";
  case ScanMode::Random: return "Random sample";
  }
  return "?";
}

const char *argModeName(ArgMode m) {
  switch (m) {
  case ArgMode::Fixed: return "fixed";
  case ArgMode::Randomised: return "randomised";
  case ArgMode::Neighbouring: return "neighbouring";
  }
  return "?";
}

// Draws a uniform sample from [0, n). splitmix64, so a given seed always gives
// the same sequence.
static uint64_t nextRandom(uint64_t &state, uint64_t n) {
  state += 0x9E3779B97F4A7C15ull;
  uint64_t z = state;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  z ^= z >> 31;
  return n > 0 ? z % n : 0;
}

uint64_t ScanConfig::pointCount() const {
  switch (mode) {
  case ScanMode::Exhaustive:
    return fmt.inputCount();
  case ScanMode::Range:
    return hi > lo ? hi - lo : 0;
  case ScanMode::Random:
    return sampleCount;
  }
  return 0;
}

std::string ScanConfig::describeRange() const {
  std::string base;
  switch (mode) {
  case ScanMode::Exhaustive: {
    const uint64_t n = fmt.inputCount();
    if (n <= (1ull << 24))
      base = std::to_string(n) + " inputs (all of " + fmt.name + ")";
    else
      base = std::to_string(n) + " inputs - too many to scan exhaustively";
    break;
  }
  case ScanMode::Range: {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "[0x%llx, 0x%llx)",
                  static_cast<unsigned long long>(lo),
                  static_cast<unsigned long long>(hi));
    base = buf;
    break;
  }
  case ScanMode::Random:
    base = std::to_string(sampleCount) + " random inputs";
    break;
  }
  if (!filter.isEverything())
    base += ", filtered to " + filter.describe();
  return base;
}

uint64_t ulpDistance(const FloatFormat &fmt, uint64_t actualBits,
                     uint64_t expectedBits) {
  // Wide arithmetic: for a 64-bit format the ordered keys need 65 bits, and
  // truncating them to uint64 would turn a large error into a small one.
  const unsigned __int128 a = fmt.orderedKeyWide(actualBits);
  const unsigned __int128 b = fmt.orderedKeyWide(expectedBits);
  const unsigned __int128 d = a > b ? a - b : b - a;
  return static_cast<uint64_t>(d);
}

bool InputFilter::isEverything() const {
  return includeZero && includeNormal && includeSubnormal && includeInfinity &&
         includeNaN && !restrictExponent;
}

std::string InputFilter::describe() const {
  if (isEverything())
    return "all inputs";
  // Built by hand rather than with QString: this lives in the UI-free core.
  std::string s;
  auto add = [&s](const char *what) {
    if (!s.empty())
      s += ",";
    s += what;
  };
  if (includeNormal)
    add("normal");
  if (includeSubnormal)
    add("denormal");
  if (includeZero)
    add("zero");
  if (includeInfinity)
    add("inf");
  if (includeNaN)
    add("nan");
  if (restrictExponent) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), ", exp %d..%d", minExponent, maxExponent);
    s += buf;
  }
  return s;
}

bool InputFilter::admits(const FloatFormat &fmt, uint64_t bits) const {
  const Class c = fmt.classify(bits);
  switch (c) {
  case Class::Zero:
    return includeZero;
  case Class::Infinity:
    return includeInfinity;
  case Class::NaN:
    return includeNaN;
  case Class::Subnormal:
    if (!includeSubnormal)
      return false;
    // A subnormal sits below the minimum normal exponent, so map it to the
    // bottom of the range rather than special-casing it.
    return !restrictExponent ||
           admitsExponent(1 - fmt.bias - fmt.mantBits);
  case Class::Normal: {
    if (!includeNormal)
      return false;
    if (!restrictExponent)
      return true;
    const int e = static_cast<int>((bits >> fmt.mantBits) & fmt.expMask()) -
                  fmt.bias;
    return admitsExponent(e);
  }
  }
  return false;
}

// Counts the inputs a scan would visit, giving up once it passes `cap` and
// returning `cap`. Callers use this to ask "is there anything here?" without
// committing to a vector of every input, which is exactly what an unbounded
// count on a wide format would do.
uint64_t countAdmittedInputs(const ScanConfig &cfg, uint64_t cap) {
  const uint64_t n = cfg.fmt.inputCount();
  uint64_t seen = 0;
  for (uint64_t i = 0; i < n && seen < cap; ++i) {
    if (cfg.filter.isEverything() || cfg.filter.admits(cfg.fmt, i))
      ++seen;
  }
  return seen;
}

std::vector<uint64_t> enumerateInputs(const ScanConfig &cfg, bool *complete) {
  std::vector<uint64_t> out;
  const uint64_t n = cfg.fmt.inputCount();
  const bool unfiltered = cfg.filter.isEverything();

  // Counting the admitted inputs is only affordable for the narrow formats,
  // but there it is cheap, and it is the difference between "you asked for
  // 1000 and got 2" being reported or hidden.
  const bool countable = n <= (1u << 24);
  uint64_t admitted = 0;
  if (countable) {
    for (uint64_t i = 0; i < n; ++i)
      if (unfiltered || cfg.filter.admits(cfg.fmt, i))
        ++admitted;
  }

  switch (cfg.mode) {
  case ScanMode::Exhaustive:
    // Do not reserve the full input space up front: with a filter active most
    // of it is skipped, and for a wide format the reservation alone could be
    // larger than memory (f32 is 4 billion entries).
    if (unfiltered && n <= (1u << 20))
      out.reserve(n);
    for (uint64_t i = 0; i < n; ++i)
      if (unfiltered || cfg.filter.admits(cfg.fmt, i))
        out.push_back(i);
    break;

  case ScanMode::Range: {
    const uint64_t hi = std::min(cfg.hi, n);
    for (uint64_t i = cfg.lo; i < hi; ++i)
      if (unfiltered || cfg.filter.admits(cfg.fmt, i))
        out.push_back(i);
    break;
  }

  case ScanMode::Random:
    if (countable && admitted <= cfg.sampleCount) {
      // The filter admits no more than was asked for, so drawing a sample
      // would only throw some of them away. Scan the whole admitted set
      // instead: it is both cheaper and more complete.
      for (uint64_t i = 0; i < n; ++i)
        if (unfiltered || cfg.filter.admits(cfg.fmt, i))
          out.push_back(i);
      break;
    }

    // Rejection sampling, so the draw stays uniform over the *admitted* inputs
    // rather than over all of them. The cap bounds the work when the admitted
    // set is much smaller than requested; hitting it is reported rather than
    // quietly returning a short scan.
    out.reserve(cfg.sampleCount);
    uint64_t s = cfg.seed;
    const uint64_t cap = cfg.sampleCount * 64 + 1024;
    for (uint64_t tries = 0; out.size() < cfg.sampleCount && tries < cap;
         ++tries) {
      const uint64_t bits = nextRandom(s, n);
      if (unfiltered || cfg.filter.admits(cfg.fmt, bits))
        out.push_back(bits);
    }
    break;
  }

  if (complete)
    *complete = out.size() >= cfg.pointCount() ||
                (countable && out.size() >= admitted);
  return out;
}

void ScanResult::finalise() {
  total = samples.size();
  mismatchCount = 0;
  maxUlp = 0;
  maxUlpAtBits = 0;
  worst.clear();
  worst.reserve(1024);

  for (uint64_t &v : perClass)
    v = 0;
  for (uint64_t &v : mismatchesPerClass)
    v = 0;
  for (uint64_t &v : overUlp)
    v = 0;

  // Thresholds reported in the summary, in ULP.
  static const uint64_t kOver[5] = {1, 2, 4, 16, 1024};

  long double sum = 0.0L;
  long double sumsq = 0.0L;
  uint64_t finite = 0;

  for (const Sample &s : samples) {
    const int cls = static_cast<int>(config.fmt.classify(s.inputBits));
    if (cls >= 0 && cls < 5)
      ++perClass[cls];

    if (s.mismatch) {
      ++mismatchCount;
      if (cls >= 0 && cls < 5)
        ++mismatchesPerClass[cls];
      if (worst.size() < 100000)
        worst.push_back(&s);
    }

    if (!s.nanDisagreement) {
      sum += s.ulpError;
      sumsq += static_cast<long double>(s.ulpError) * s.ulpError;
      ++finite;
      if (s.ulpError > maxUlp) {
        maxUlp = s.ulpError;
        maxUlpAtBits = s.inputBits;
      }
      for (int i = 0; i < 5; ++i)
        if (s.ulpError > kOver[i])
          ++overUlp[i];
    }
  }

  if (finite) {
    meanUlp = static_cast<double>(sum / static_cast<long double>(finite));
    rmsUlp =
        static_cast<double>(std::sqrt(sumsq / static_cast<long double>(finite)));
  }

  std::sort(worst.begin(), worst.end(),
            [](const Sample *a, const Sample *b) {
              // A NaN disagreement outranks any finite distance, since no
              // numeric ordering exists for it.
              if (a->nanDisagreement != b->nanDisagreement)
                return a->nanDisagreement;
              return a->ulpError > b->ulpError;
            });
}

ScanResult Scanner::run(const ScanConfig &cfg, const DynamicLibrary &lib, void *fn,
                        const ProgressFn &progress,
                        const std::atomic<bool> *cancel) {
  ScanResult result;
  result.config = cfg;

  if (!fn) {
    result.error = "function not resolved";
    return result;
  }
  if (!cfg.fmt.valid()) {
    result.error = "unsupported format: " + std::string(cfg.fmt.name);
    return result;
  }
  // Refuse before allocating anything. One Sample per input over 2^32 inputs is
  // tens of gigabytes, and over 2^64 the loop would run until the machine died.
  // The caller is told to sample instead rather than being allowed to find out
  // the hard way.
  if (cfg.wouldExhaustMemory()) {
    result.error = std::string(cfg.fmt.name) + " has " +
                   std::to_string(cfg.fmt.inputCount()) +
                   " inputs; an exhaustive scan would need far more memory than "
                   "is available. Use a random sample or a range.";
    return result;
  }
  if (cfg.workPrecBits < (1 + cfg.fmt.mantBits) + 64) {
    result.error = "working precision too low for a reliable reference";
    return result;
  }

  const std::vector<uint64_t> inputs =
      enumerateInputs(cfg, &result.inputSetComplete);
  result.requestedInputs = cfg.pointCount();
  result.samples.resize(inputs.size());

  // Fail loudly rather than comparing everything against a NaN reference,
  // which would report every input as a mismatch and look like a catastrophic
  // numerical bug rather than a missing reference.
  const std::string base = cfg.referenceBase.empty()
                               ? baseName(cfg.symbol)
                               : baseName(cfg.referenceBase);
  if (MpfrRef::lookup(base) == nullptr) {
    result.error = "no MPFR reference for \"" +
                   (cfg.referenceBase.empty() ? baseName(cfg.symbol)
                                             : cfg.referenceBase) +
                   "\"";
    return result;
  }

  const unsigned hw = std::thread::hardware_concurrency();
  unsigned nthreads = cfg.threads > 0 ? static_cast<unsigned>(cfg.threads)
                                      : (hw ? hw : 1u);
  nthreads = std::max(1u, std::min<unsigned>(nthreads,
                                             static_cast<unsigned>(
                                                 std::max<size_t>(1, inputs.size()))));

  std::atomic<uint64_t> done{0};
  std::atomic<bool> failed{false};
  std::string firstError;
  std::mutex errMutex;

  auto worker = [&](size_t begin, size_t end) {
    // Each thread owns its rounder and its MPFR state, and its own copy of the
    // random stream, so the draw for a given input is the same however many
    // threads are running.
    const FloatFormat &fmt = cfg.fmt;
    MpfrRounder rounder(cfg.fmt);
    uint64_t rng = cfg.seed;

    for (size_t i = begin; i < end; ++i) {
      if (cancel && cancel->load(std::memory_order_relaxed)) {
        result.cancelled = true;
        return;
      }

      const uint64_t bits = inputs[i];
      Sample &s = result.samples[i];
      s.inputBits = bits;
      s.input = cfg.fmt.decode(bits);

      // First argument varies; the rest depend on argMode.
      double ins[3] = {s.input, cfg.fixedArgs[0], cfg.fixedArgs[1]};
      uint64_t insBits[3];
      insBits[0] = bits;

      switch (cfg.argMode) {
      case ArgMode::Fixed:
        break;

      case ArgMode::Randomised: {
        for (int a = 1; a < cfg.nargs && a < 3; ++a)
          insBits[a] = nextRandom(rng, fmt.inputCount());
        break;
      }

      case ArgMode::Neighbouring: {
        // Step the trailing arguments a short distance from the first. Offset 0
        // gives equal operands, +1/-1 give adjacent values, and the negatives
        // exercise the crossing from -0 to +0.
        const size_t k = cfg.neighbourOffsets.empty() ? 1
                                                       : cfg.neighbourOffsets.size();
        for (int a = 1; a < cfg.nargs && a < 3; ++a) {
          const int off = cfg.neighbourOffsets[(i + static_cast<size_t>(a)) % k];
          const uint64_t limit = fmt.inputCount();
          const int64_t moved = static_cast<int64_t>(bits) + off;
          insBits[a] = moved < 0
                           ? 0u
                           : static_cast<uint64_t>(moved) % limit;
        }
        break;
      }
      }

      for (int a = 1; a < cfg.nargs && a < 3; ++a)
        ins[a] = cfg.fmt.decode(insBits[a]);

      double raw = 0.0;
      std::string err;
      if (!lib.call(fn, cfg.conv, ins, insBits, cfg.nargs, &raw, &err)) {
        std::lock_guard<std::mutex> lock(errMutex);
        if (!failed.exchange(true))
          firstError = err;
        return;
      }

      if (cfg.conv == CallConv::Native16 || cfg.conv == CallConv::Integer8) {
        // These conventions return a *bit pattern*, not a number. Decoding it
        // with the target format is exact, so there is nothing to round.
        const uint64_t rb =
            static_cast<uint64_t>(raw) & cfg.fmt.mask(cfg.fmt.totalBits);
        s.raw = raw; // kept verbatim; only meaningful as a bit pattern
        s.actual = cfg.fmt.decode(rb);
        s.actualBits = rb;
      } else {
        s.raw = raw;
        // Both sides go through the same rounding, so a comparison is always
        // between two values the format can actually hold.
        s.actual = rounder.round(raw);
        s.actualBits = rounder.pack(s.actual);
      }

      s.expected = evaluateReference(base, ins, cfg.nargs, cfg.workPrecBits,
                                     rounder);
      s.expectedBits = rounder.pack(s.expected);

      const bool an = std::isnan(s.actual);
      const bool en = std::isnan(s.expected);
      if (an && en) {
        s.nanDisagreement = false;
        s.ulpError = 0;
        s.mismatch = false;
      } else if (an != en) {
        s.nanDisagreement = true;
        s.ulpError = UINT64_MAX;
        s.mismatch = true;
      } else {
        s.nanDisagreement = false;
        s.ulpError = ulpDistance(cfg.fmt, s.actualBits, s.expectedBits);
        s.mismatch = s.ulpError > cfg.toleranceUlp;
      }

      const uint64_t n = done.fetch_add(1, std::memory_order_relaxed) + 1;
      if (progress && (n % 4096 == 0))
        progress(n, inputs.size());
    }
  };

  const size_t chunk = (inputs.size() + nthreads - 1) / nthreads;
  std::vector<std::thread> pool;
  pool.reserve(nthreads);
  for (unsigned t = 0; t < nthreads; ++t) {
    const size_t begin = std::min(inputs.size(), chunk * t);
    const size_t end = std::min(inputs.size(), begin + chunk);
    if (begin < end)
      pool.emplace_back(worker, begin, end);
  }
  for (std::thread &t : pool)
    t.join();

  if (failed.load()) {
    result.error = firstError.empty() ? "call failed" : firstError;
    return result;
  }
  if (result.cancelled)
    return result;

  result.finalise();
  result.ok = true;
  if (progress)
    progress(inputs.size(), inputs.size());
  return result;
}

} // namespace ulpscope