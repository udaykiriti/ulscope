#include "core/dynlib.h"

#include "core/mpfrref.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <elf.h>

#include <dlfcn.h>

namespace ulpscope {

const char *callConvName(CallConv c) {
  switch (c) {
  case CallConv::Double: return "double f(double)";
  case CallConv::Native16: return "16-bit native (bf16/_Float16)";
  case CallConv::Float: return "float f(float)";
  case CallConv::Integer8: return "uint8_t f(uint8_t)";
  case CallConv::Unknown: return "unknown";
  }
  return "?";
}

const char *callConvDescription(CallConv c) {
  switch (c) {
  case CallConv::Double:
    return "Argument and result are passed as double. Use for wrappers and "
           "anything that promotes its inputs.";
  case CallConv::Native16:
    return "Argument and result are a real 16-bit float (__bf16 or _Float16). "
           "Result is masked to 16 bits. Matches llvm-libc's own signatures.";
  case CallConv::Float:
    return "Argument and result are float. Use for an f32 implementation.";
  case CallConv::Integer8:
    return "Argument and result are a plain 8-bit value (fp8 e4m3/e5m2), "
           "passed and returned as integers.";
  case CallConv::Unknown:
    return "No calling convention selected.";
  }
  return "";
}

// ------------------------------------------------------------- ELF symbols

std::vector<std::string> listElfSymbols(const std::string &path,
                                        std::string *error) {
  std::vector<std::string> out;
  FILE *f = std::fopen(path.c_str(), "rb");
  if (!f) {
    if (error)
      *error = "cannot open " + path;
    return out;
  }

  auto fail = [&](const char *msg) -> std::vector<std::string> {
    if (error)
      *error = std::string(msg) + " in " + path;
    std::fclose(f);
    return {};
  };

  Elf64_Ehdr eh;
  if (std::fread(&eh, sizeof(eh), 1, f) != 1)
    return fail("short read (ELF header)");

  if (std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0)
    return fail("not an ELF file");
  if (eh.e_ident[EI_CLASS] != ELFCLASS64)
    return fail("only 64-bit ELF is supported");
  if (eh.e_shoff == 0 || eh.e_shnum == 0)
    return fail("no section headers (stripped or static?)");

  if (std::fseek(f, static_cast<long>(eh.e_shoff), SEEK_SET) != 0)
    return fail("cannot seek to section headers");

  std::vector<Elf64_Shdr> shdrs(eh.e_shnum);
  if (std::fread(shdrs.data(), sizeof(Elf64_Shdr), eh.e_shnum, f) !=
      static_cast<size_t>(eh.e_shnum))
    return fail("short read (section headers)");

  for (const Elf64_Shdr &s : shdrs) {
    if (s.sh_type != SHT_DYNSYM || s.sh_link >= shdrs.size() || s.sh_entsize == 0)
      continue;
    const Elf64_Shdr &strtab = shdrs[s.sh_link];

    if (std::fseek(f, static_cast<long>(s.sh_offset), SEEK_SET) != 0)
      continue;
    if (std::fseek(f, static_cast<long>(strtab.sh_offset), SEEK_SET) != 0)
      continue;

    // Pull the whole string table in one go; it is small and this keeps the
    // loop simple.
    std::string strings(strtab.sh_size, '\0');
    if (strtab.sh_size &&
        std::fread(strings.data(), 1, strtab.sh_size, f) != strtab.sh_size)
      continue;

    if (std::fseek(f, static_cast<long>(s.sh_offset), SEEK_SET) != 0)
      continue;

    const size_t count = s.sh_size / s.sh_entsize;
    for (size_t i = 0; i < count; ++i) {
      Elf64_Sym sym;
      if (std::fread(&sym, sizeof(sym), 1, f) != 1)
        break;
      if (sym.st_name == 0 || sym.st_name >= strings.size())
        continue;
      if (ELF64_ST_TYPE(sym.st_info) != STT_FUNC)
        continue;
      if (sym.st_shndx == SHN_UNDEF)
        continue;
      out.emplace_back(strings.c_str() + sym.st_name);
    }
  }

  std::fclose(f);

  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

// ------------------------------------------------------------------ library

DynamicLibrary::~DynamicLibrary() { unload(); }

bool DynamicLibrary::load(const std::string &path, std::string *error) {
  unload();
  // RTLD_LOCAL so we do not squat on names in the global namespace, and
  // RTLD_NOW so an unsatisfiable relocation is reported at load time rather
  // than halfway through a scan.
  handle_ = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!handle_) {
    if (error)
      *error = ::dlerror() ? ::dlerror() : "dlopen failed";
    return false;
  }
  path_ = path;
  symbols_ = listElfSymbols(path, error);
  return true;
}

void DynamicLibrary::unload() {
  if (handle_)
    ::dlclose(handle_);
  handle_ = nullptr;
  path_.clear();
  symbols_.clear();
}

bool DynamicLibrary::resolve(const std::string &symbol, void **out) const {
  if (!handle_)
    return false;
  ::dlerror(); // clear any stale error
  void *p = ::dlsym(handle_, symbol.c_str());
  if (!p) {
    ::dlerror();
    return false;
  }
  *out = p;
  return true;
}

int DynamicLibrary::inferArgCount(const std::string &symbol) {
  int args = -1;
  if (MpfrRef::knownBase(baseName(symbol), &args))
    return args;
  return -1;
}

CallConv guessCallConv(const std::string &symbol) {
  // Check the long, unambiguous spellings first. Anything ending in a bare "f"
  // is ambiguous between "float" and "sqrt"/"fmin"-style names, so only trust
  // it when the stripped stem is a known function.
  auto endsWith = [&symbol](const char *suf) {
    const size_t n = std::char_traits<char>::length(suf);
    return symbol.size() > n && symbol.compare(symbol.size() - n, n, suf) == 0;
  };

  if (endsWith("bf16"))
    return CallConv::Native16;
  if (endsWith("f16"))
    return CallConv::Native16;
  if (endsWith("e4m3") || endsWith("e5m2"))
    return CallConv::Integer8;
  if (endsWith("f32") || endsWith("f64"))
    return CallConv::Float;
  if (endsWith("_f32") || endsWith("_f64"))
    return CallConv::Float;
  // A "_d" or "_double" wrapper takes and returns double. Underscored on
  // purpose: a bare "d" suffix would also match "sqrtdf", which is not a
  // double-ABI function.
  if (endsWith("_double") || endsWith("_d"))
    return CallConv::Double;

  // A trailing "f" means float only if what remains is a real function name.
  if (symbol.size() > 1 && symbol.back() == 'f') {
    const std::string stem = symbol.substr(0, symbol.size() - 1);
    if (!MpfrRef::resolveBase(stem).empty() &&
        MpfrRef::resolveBase(stem) == MpfrRef::resolveBase(symbol))
      return CallConv::Float;
  }

  return CallConv::Unknown;
}

namespace {

using Fn1d = double (*)(double);
using Fn2d = double (*)(double, double);
using Fn3d = double (*)(double, double, double);
using Fn1f = float (*)(float);
using Fn2f = float (*)(float, float);
using Fn3f = float (*)(float, float, float);
using Fn1i = uint8_t (*)(uint8_t);
using Fn2i = uint8_t (*)(uint8_t, uint8_t);
using Fn3i = uint8_t (*)(uint8_t, uint8_t, uint8_t);

// dlsym hands back a void*, which is not the same type as a function pointer.
// Go through memcpy so this stays well defined rather than relying on a cast
// that -Wpedantic rightly complains about.
template <typename Fn> Fn asFn(void *p) {
  Fn fn;
  std::memcpy(&fn, &p, sizeof(fn));
  return fn;
}

uint16_t narrowTo16(double v) {
  // Take the low 16 bits of the returned float's encoding. The callee wrote a
  // 16-bit value into xmm0's low half; the upper half is undefined, which is
  // why we reinterpret the bits rather than convert the value.
  float f = static_cast<float>(v);
  uint32_t bits;
  std::memcpy(&bits, &f, sizeof(bits));
  return static_cast<uint16_t>(bits & 0xFFFFu);
}

// Build the float that a 16-bit-native callee expects to find in xmm0.
//
// The argument must be the 16-bit encoding placed in the *low* half of the
// register, not the value converted to float. Passing 1.0f instead of the
// pattern 0x3F80 makes the callee read zero, which is a silent and very
// confusing failure, so this is done in one obviously named place.
float nativeArg16(uint64_t bits16) {
  const uint32_t u = static_cast<uint32_t>(bits16 & 0xFFFFu);
  float f;
  std::memcpy(&f, &u, sizeof(f));
  return f;
}

} // namespace

bool DynamicLibrary::call(void *fn, CallConv conv, const double *in,
                          const uint64_t *inBits, int nargs, double *out,
                          std::string *error) const {
  if (!fn || !in || !out) {
    if (error)
      *error = "invalid call";
    return false;
  }
  if (nargs < 1 || nargs > 3) {
    if (error)
      *error = "only 1 to 3 arguments are supported";
    return false;
  }
  if (conv == CallConv::Native16 && !inBits) {
    if (error)
      *error = "the 16-bit calling convention needs input bit patterns";
    return false;
  }
  if (conv == CallConv::Integer8 && !inBits) {
    if (error)
      *error = "the 8-bit calling convention needs input bit patterns";
    return false;
  }
  if (conv == CallConv::Unknown) {
    // Not a real convention, so it must be rejected rather than falling out of
    // the switch below: that would leave *out untouched and report success,
    // handing back an uninitialised value as though it were a result.
    if (error)
      *error = "no calling convention selected";
    return false;
  }
  const double x = in[0];
  const double y = nargs > 1 ? in[1] : 0.0;
  const double z = nargs > 2 ? in[2] : 0.0;

  switch (conv) {
  case CallConv::Double:
    switch (nargs) {
    case 1: *out = asFn<Fn1d>(fn)(x); break;
    case 2: *out = asFn<Fn2d>(fn)(x, y); break;
    default: *out = asFn<Fn3d>(fn)(x, y, z); break;
    }
    break;

  case CallConv::Float:
    switch (nargs) {
    case 1: *out = asFn<Fn1f>(fn)(static_cast<float>(x)); break;
    case 2:
      *out = asFn<Fn2f>(fn)(static_cast<float>(x), static_cast<float>(y));
      break;
    default:
      *out = asFn<Fn3f>(fn)(static_cast<float>(x), static_cast<float>(y),
                            static_cast<float>(z));
      break;
    }
    break;

  case CallConv::Integer8:
    // No native hardware type, so this travels through the integer registers:
    // the argument in %dil, the result in %al.
    switch (nargs) {
    case 1:
      *out = asFn<Fn1i>(fn)(static_cast<uint8_t>(inBits[0] & 0xFFu));
      break;
    case 2:
      *out = asFn<Fn2i>(fn)(static_cast<uint8_t>(inBits[0] & 0xFFu),
                            static_cast<uint8_t>(inBits[1] & 0xFFu));
      break;
    default:
      *out = asFn<Fn3i>(fn)(static_cast<uint8_t>(inBits[0] & 0xFFu),
                            static_cast<uint8_t>(inBits[1] & 0xFFu),
                            static_cast<uint8_t>(inBits[2] & 0xFFu));
      break;
    }
    break;

  case CallConv::Native16:
    // On x86-64 SysV a __bf16 or _Float16 parameter occupies the low 16 bits of
    // an SSE register, and the float signature puts its argument in exactly
    // that register, so the float call shapes give the callee the register
    // contents it expects. The value we hand over is the encoding, not a
    // number; see nativeArg16.
    switch (nargs) {
    case 1:
      *out = narrowTo16(asFn<Fn1f>(fn)(nativeArg16(inBits[0])));
      break;
    case 2:
      *out = narrowTo16(
          asFn<Fn2f>(fn)(nativeArg16(inBits[0]), nativeArg16(inBits[1])));
      break;
    default:
      *out = narrowTo16(asFn<Fn3f>(fn)(nativeArg16(inBits[0]),
                                       nativeArg16(inBits[1]),
                                       nativeArg16(inBits[2])));
      break;
    }
    break;

  default:
    // Unreachable, since CallConv::Unknown is rejected above. Kept as a hard
    // error rather than a silent no-op so that adding a convention without
    // adding a case fails loudly instead of returning an untouched result.
    if (error)
      *error = "unsupported calling convention";
    return false;
  }

  if (error)
    error->clear();
  return true;
}

} // namespace ulpscope