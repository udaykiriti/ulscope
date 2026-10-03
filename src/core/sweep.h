#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/scan.h"

namespace ulpscope {

// Runs one function to completion, recording progress as it goes.
struct FunctionSummary {
  std::string symbol;
  std::string baseName;
  std::string format;
  std::string callConv;
  bool ok = false;
  bool cancelled = false;
  // True when the format was too large to enumerate and a sample was used, so
  // the UI can say the numbers are not exhaustive.
  bool sampled = false;
  std::string error;
  uint64_t total = 0;
  uint64_t mismatchCount = 0;
  uint64_t maxUlp = 0;
  double rmsUlp = 0.0;
  // One-sample-per-class counts of *where* the errors are, so two functions
  // can be compared without re-running either.
  uint64_t mismatchesPerClass[5] = {0, 0, 0, 0, 0};
};

// A sweep over every function in a library that ulpscope can both call and
// reference. This is the question you actually want answered when picking up an
// unfamiliar math library: which functions are not correctly rounded, and how
// far off are they.
class LibrarySweep {
public:
  using ProgressFn = std::function<void(const FunctionSummary &done,
                                        int index, int total)>;

  // cfg supplies the format, ABI, filter and tolerance; only `symbol` is
  // replaced per function. Results are appended in a stable order.
  static std::vector<FunctionSummary> run(
      const std::string &libraryPath, const std::vector<std::string> &symbols,
      const ScanConfig &cfg, const ProgressFn &progress = nullptr,
      const std::atomic<bool> *cancel = nullptr);
};

} // namespace ulpscope