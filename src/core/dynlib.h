// Loading a candidate implementation from a shared object and calling into it.
//
// Three calling conventions are supported, because "a .so built from your
// implementation" can reasonably mean any of them:
//
//   Double   double f(double [, double [, double]])
//            Anything that promotes its argument, including wrappers.
//   Native16 The real 16-bit type in and out: __bf16 or _Float16. On x86-64
//            SysV these arrive in the low 16 bits of an SSE register, which is
//            exactly where a float argument puts them, so we call through the
//            float signature and mask the result.
//   Float    float f(float [, float [, float]]) - e.g. an f32 implementation.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ulpscope {

enum class CallConv {
  Double,
  Native16,
  Float,
  // An 8-bit value with no native hardware type, so it travels as an integer:
  // passed in the low 8 bits of %dil, returned in %al. This is the convention a
  // freestanding libc uses for fp8, and it is what llvm-libc's fp8 entry points
  // expect.
  Integer8,
  // Not a convention: the answer is "cannot tell from the name".
  Unknown,
};

const char *callConvName(CallConv c);
const char *callConvDescription(CallConv c);

// Best guess at a calling convention from a symbol's name, so the common case
// needs no manual selection. Returns Unknown when the name says nothing useful,
// in which case the user should pick. A wrong guess is worse than no guess
// because it produces garbage silently, so this only fires on unambiguous
// spellings.
CallConv guessCallConv(const std::string &symbol);

// Exported dynamic symbols of an ELF shared object, sorted and de-duplicated.
// Read straight from the file's section headers rather than guessed from the
// symbol table, which the file format already describes.
std::vector<std::string> listElfSymbols(const std::string &path,
                                        std::string *error);

class DynamicLibrary {
public:
  DynamicLibrary() = default;
  ~DynamicLibrary();

  DynamicLibrary(const DynamicLibrary &) = delete;
  DynamicLibrary &operator=(const DynamicLibrary &) = delete;

  bool load(const std::string &path, std::string *error);
  void unload();

  bool isOpen() const { return handle_ != nullptr; }
  const std::string &path() const { return path_; }
  const std::vector<std::string> &symbols() const { return symbols_; }

  // False if the symbol is absent or if the call cannot be made.
  bool resolve(const std::string &symbol, void **out) const;

  // Calls a resolved symbol. Returns false and sets *error on a bad argument
  // count.
  //
  // `inBits` holds the input values already encoded as 16-bit patterns. It is
  // only consulted for CallConv::Native16, where the argument travels as raw
  // bits rather than as a number; pass nullptr for the other conventions.
  bool call(void *fn, CallConv conv, const double *in, const uint64_t *inBits,
            int nargs, double *out, std::string *error) const;

  // Number of arguments `symbol` most likely takes, inferred from its base
  // name. Returns -1 when the name is not a known function.
  static int inferArgCount(const std::string &symbol);

private:
  void *handle_ = nullptr;
  std::string path_;
  std::vector<std::string> symbols_;
};

} // namespace ulpscope