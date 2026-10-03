#include "core/sweep.h"

#include <algorithm>

#include "core/dynlib.h"
#include "core/mpfrref.h"

namespace ulpscope {

namespace {
// Above this many inputs, an exhaustive sweep is not worth running: bf16 and f16
// are 65536, fp8 is 256, and anything larger has to be sampled.
constexpr uint64_t kMaxExhaustive = 1u << 24;
// One sample size for every sampled format, so the mismatch percentages in the
// ranking are comparable between rows.
constexpr uint64_t kSampledInputs = 1u << 16;
} // namespace

std::vector<FunctionSummary> LibrarySweep::run(
    const std::string &libraryPath, const std::vector<std::string> &symbols,
    const ScanConfig &cfg, const ProgressFn &progress,
    const std::atomic<bool> *cancel) {

  std::vector<FunctionSummary> out;
  out.reserve(symbols.size());

  DynamicLibrary lib;
  std::string err;
  if (!lib.load(libraryPath, &err)) {
    FunctionSummary s;
    s.ok = false;
    s.error = err;
    out.push_back(s);
    return out;
  }

  int index = 0;
  for (const std::string &symbol : symbols) {
    ++index;
    if (cancel && cancel->load())
      break;

    FunctionSummary sum;
    sum.symbol = symbol;
    sum.baseName = MpfrRef::resolveBase(symbol);
    sum.format = cfg.fmt.name;
    sum.callConv = callConvName(cfg.conv);

    void *fn = nullptr;
    if (!lib.resolve(symbol, &fn)) {
      sum.error = "symbol not found";
      out.push_back(sum);
      if (progress)
        progress(sum, index, static_cast<int>(symbols.size()));
      continue;
    }

    ScanConfig c = cfg;
    c.symbol = symbol;

    // Sweep every symbol under *its own* format and ABI. Using the caller's
    // settings for all of them would call fp8 and double-ABI functions as if
    // they were bf16, which does not fail - it returns plausible garbage, and
    // those rows would then dominate the ranking with meaningless errors.
    const int fmtIndex = FloatFormat::indexImpliedByName(symbol);
    if (fmtIndex >= 0)
      c.fmt = FloatFormat::all()[fmtIndex];

    const CallConv guessed = guessCallConv(symbol);
    if (guessed != CallConv::Unknown)
      c.conv = guessed;

    sum.format = c.fmt.name;
    sum.callConv = callConvName(c.conv);

    // Re-derive the arity per function: a sweep spans both unary and binary
    // functions, and guessing wrong would call into the wrong ABI.
    const int inferred = DynamicLibrary::inferArgCount(symbol);
    if (inferred > 0)
      c.nargs = inferred;

    // An exhaustive sweep is only affordable on the narrow formats. f32 has
    // 4 billion inputs and f64 is worse, so fall back to a seeded sample rather
    // than trying to allocate the whole input space. Sampling per function
    // would make rows incomparable, so every large format uses the same count.
    if (c.mode == ScanMode::Exhaustive && c.fmt.inputCount() > kMaxExhaustive) {
      c.mode = ScanMode::Random;
      c.sampleCount = kSampledInputs;
      sum.sampled = true;
    }

    sum.format = c.fmt.name;
    sum.callConv = callConvName(c.conv);

    const ScanResult r = Scanner::run(c, lib, fn, nullptr, cancel);
    if (!r.ok) {
      sum.error = r.cancelled ? "cancelled" : r.error;
      sum.cancelled = r.cancelled;
    } else {
      sum.ok = true;
      sum.total = r.total;
      sum.mismatchCount = r.mismatchCount;
      sum.maxUlp = r.maxUlp;
      sum.rmsUlp = r.rmsUlp;
      for (int i = 0; i < 5; ++i)
        sum.mismatchesPerClass[i] = r.mismatchesPerClass[i];
    }

    out.push_back(sum);
    if (progress)
      progress(sum, index, static_cast<int>(symbols.size()));
  }

  return out;
}

} // namespace ulpscope