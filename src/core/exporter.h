// Turn a scan into something you can paste into an llvm-libc unit test.
#pragma once

#include <string>
#include <vector>

#include "core/scan.h"

namespace ulpscope {

struct ExportOptions {
  // Name of the macro the generated test uses to invoke the function under
  // test, e.g. "lgammabf16". Defaults to the scanned symbol.
  std::string functionName;
  // llvm-libc style test header paths, left empty for a freestanding snippet.
  std::string litTestName;
  bool includePassingCases = false;
  uint64_t maxCases = 1000;
  // Emit one EXPECT-style block per case as well as the table.
  bool litStyle = true;
  int indentSpaces = 2;
};

// A self-contained C test that walks the table and reports mismatches. This is
// the form that is easiest to paste into a unit test file.
std::string exportTable(const ScanResult &result, const ExportOptions &opts);

// The same cases as a lit-style sequence of EXPECT lines.
std::string exportLit(const ScanResult &result, const ExportOptions &opts);

// CSV of the worst cases, for spreadsheets.
std::string exportCsv(const ScanResult &result, uint64_t maxCases);

} // namespace ulpscope