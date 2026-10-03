# ulpscope

**Exhaustive ULP verification for bf16, f16 and fp8.**

`bfloat16` and `float16` have only 65,536 distinct inputs. That means you can
check *every single one* against a correctly rounded MPFR reference, instantly,
and know precisely which inputs your implementation gets wrong. That is what
this does.

It exists for the moment after you write or port a low-precision math function
and want to know exactly where it stops being correctly rounded, rather than
guessing from a handful of spot checks.

---

## Quick start

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build --output-on-failure   # 210 checks
./build/ulpscope
```

A demo library with deliberately varied quality is loaded on startup, so there
is something to look at immediately. Try `sqrtbf16` (clean), then
`lgammabf16` (1 ULP on most inputs), then `sqrtdfbf16` (double-rounded).

Requires Qt 6 (Core, Gui, Widgets), CMake, and MPFR with development headers.

---

## What it does

### Inspect

**Bit inspector.** Type a value as a decimal, a C99 hex float, or a hex bit
pattern, or just click the bits. See the sign, exponent and mantissa split, the
exact decimal and hex float, the class (normal, denormal, zero, infinity, NaN),
the spacing at that value, and the neighbours on either side.

Covers six formats: `e4m3`, `e5m2`, `f16`, `bf16`, `f32`, `f64`.

### Scan

**Whole-input scan.** Point it at a `.so`, pick a function, and it evaluates
that function over every input and compares each result against MPFR. For the
16-bit formats that is all 65,536 of them, in well under a second.

**Library sweep.** Check every exported function that ulpscope can both call
and reference, then get them ranked by error. When you pick up an unfamiliar
math library, this is the question you actually want answered: which functions
are wrong, and by how much.

**Input filters.** Restrict a scan to a class (zero, normal, denormal,
infinity, NaN) or to an exponent range. An exhaustive bf16 sweep is cheap
enough that the useful question is usually "what does this break *on*?" rather
than "is anything broken?". If a filter admits fewer inputs than you asked for,
ulpscope says so instead of reporting a short scan as a success.

**Argument modes.** For two- and three-argument functions, choose whether the
trailing arguments are *fixed*, *randomised*, or *neighbouring* the first
argument. Neighbouring is what makes `fmin`, `fmax`, `fdim` and `copysign`
testable at all: their defects live at equality, at orderings, and at the
signed-zero boundary, and no fixed second argument ever reaches any of those.

**Reference override.** For a symbol whose name does not reduce to a known
function, say what it implements. `log2_viaf32` is `log2`, but no amount of
suffix stripping gets there. Without an override such a symbol can only be
checked by hand, and an unnoticed failure there looks like a function that is
wrong on every input. With one, a missing reference is a clear error rather
than a 100 percent mismatch rate.

**Tolerance.** Declare 1 ULP acceptable when a function genuinely cannot be
correctly rounded everywhere. Zero, the default, demands correct rounding.

### Analyse

**ULP plot.** Input on x, error in ULP on y. Click a point to see the input,
your output, the expected output and the bits. Colour by result or by input
class. The wheel zooms, a left drag pans, and right-click or double-click
resets. The y axis rescales to what is visible, so zooming into a quiet region
is not flattened by an outlier somewhere else.

**Worst-case table.** Sorted by error, filterable to mismatches only, with
per-class error counts and a summary that names the class actually holding the
failures.

### Export

Failing inputs as a standalone C test table, as an llvm-libc style lit test, as
CSV, and the whole library sweep as CSV. The case limit is adjustable.

---

## Pointing it at your own library

ulpscope never links against your math library. It reaches it through `dlsym`,
so there is no FFI layer and no rebuild: build your functions as a shared
object with exported symbols, load it, pick one.

The **ABI** selector says how to call it.

| ABI | Signature | Use for |
| --- | --- | --- |
| `16-bit native` | `bf16 f(bf16)` | llvm-libc's own `__bf16` and `_Float16` signatures |
| `uint8_t f(uint8_t)` | `uint8_t f(uint8_t)` | fp8 e4m3 and e5m2, which have no native type |
| `double` | `double f(double)` | wrappers, or anything that promotes its argument |
| `float` | `float f(float)` | an honest f32 implementation |

The ABI and the format are both inferred from the symbol name. `sqrte4m3`
implies e4m3 and the integer ABI; `exp_f32` implies f32 and float. Choosing
either by hand overrides that.

A wrong ABI does not fail loudly, it returns garbage. That is why the library
sweep infers the format and ABI per function rather than applying one setting
to everything.

Argument count is inferred the same way: `pow` takes two, `lgammabf16` takes
one, and both can be overridden.

### Name resolution

Symbols are matched by stripping type suffixes, so `sqrtf`, `sqrtbf16`,
`sqrt_bf16`, `sqrt_f32` and `log2f128` all resolve. `sqrtdf` and `fsqrt` are
recognised aliases for `sqrt`. A name that matches nothing is reported, never
silently compared against the wrong function.

---

## Reference coverage

56 functions, which is everything llvm-libc's list needs:

```
sqrt cbrt exp exp2 exp10 expm1 log log2 log10 log1p logb ilogb
sin cos tan asin acos atan sinh cosh tanh asinh acosh atanh
fabs floor ceil round trunc rint nearbyint roundeven lrint llrint lround llround
lgamma tgamma digamma zeta erf erfc j0 j1 y0 y1
pow atan2 fmod remainder hypot fmin fmax fdim copysign fma
```

Adding one is a row in the table in `src/core/mpfrref.cpp`. So is adding a
format: a row in `formatSpecs()` in `src/core/format.cpp`. Nothing else needs
to change.

---

## Wide formats

f32 has 4 billion inputs and f64 has more, so exhaustive mode is not offered
for them. Use **Random sample**, which is seeded and therefore reproducible, or
**Range**. The generator is splitmix64 with a fixed seed, so the same
configuration always selects the same inputs.

`Scanner::run` refuses an exhaustive scan above 2^24 inputs rather than
attempting it. See the note on memory below for why that guard exists.

---

## Windows and layout

Every panel is a dock widget: scan options, input filter, ULP plot, results,
library sweep, bit inspector. Drag any of them to an edge, float them into
their own window, hide them, or tab them together. A float can be spread
across two monitors.

The arrangement is saved on exit and restored on the next run. **View, Reset
layout** puts everything back, and **View, Float all panels** undocks
everything at once.

The scan options are laid out in a grid rather than one long toolbar row,
because in a row the labels end up pressed against their controls and the
strip overflows.

---

## The demo library

`demo/bf16_demo.cpp` stands in for an implementation under test. It
deliberately varies in quality so the plot shows something on first run.

| Function | Behaviour | Result |
| --- | --- | --- |
| `sqrtbf16`, `expbf16`, `logbf16`, `sinbf16`, `erfbf16`, `cbrtbf16` | correctly rounded | 0 mismatches |
| `fabsbf16` | exact bit manipulation | 0 mismatches |
| `sqrte4m3` | correctly rounded fp8 | 0 mismatches |
| `sqrt_double`, `log_double` | correctly rounded f64 | 0 mismatches |
| `lgammabf16` | rounds toward zero | about 1 ULP on most inputs |
| `tgamma_bf16` | rounds away from zero | about 1 ULP on most inputs |
| `sqrtdfbf16` | `sqrt` via `powf` in `float`, so double-rounded | scattered errors |
| `loge5m2` | widens through f16 first, so double-rounded | fp8 rounding errors |
| `log2_viaf32` | computes in f32 and widens | f64 errors; needs a reference override |
| `fmaxf`, `fdimf` | wrong at the signed-zero boundary | found only by neighbouring args |

`lgammabf16_d`, `sqrtbf16_d` and `powbf16_d` are double-ABI wrappers,
`sqrt_f32` and `lgamma_f32` are float-ABI, `sqrtf16`, `expf16`, `sinf16` and
`lgammaf16` are 16-bit f16, and `fmax_double`, `fminf`, `fmaxf` and `fdimf`
are two-argument. Every ABI, every format and all three argument modes have
something to exercise.

### A note on fmax and signed zero

C leaves `fmax(+0, -0)` and `fmin(+0, -0)` unspecified, so a correctly rounded
reference and a conforming implementation may legitimately disagree on those
two inputs. With **neighbouring** arguments, `fmax_double`, which is otherwise
correct, reports exactly three such cases out of 65,536. Those are not a defect,
and they are the reason a tolerance setting exists.

---

## Sharp edges

Things that cost real time to discover, all commented at the point they matter
in the source.

### MPFR cannot round to IEEE subnormals through emin and emax

MPFR clamps the exponent range, but it does not coarsen the significand grid
for subnormal values the way IEEE does. Measured on f16, rounding
`5.9212671107461366e-05` at precision 11 gave `993.5 * 2^-24` for *every*
value of `emin`, where IEEE requires `993 * 2^-24`: the IEEE subnormal grid is
one step of `2^(1-bias-mantBits)`, which is coarser than 11 significant bits
at that magnitude.

`MpfrRounder` therefore never sets `emin` or `emax`. It handles the two cases
separately: subnormals by an exact power-of-two shift followed by
`mpfr_get_si(..., MPFR_RNDN)`, which is round-to-nearest-ties-to-even and so is
exactly the IEEE rule, and normals by rounding to `1 + mantBits` significant
bits at a wide exponent range. See `src/core/mpfr_util.h`.

### Never round a reference through a double

`mpfr_get_d` at 200 bits followed by a second rounding to bf16 is double
rounding, and can disagree with the single correct rounding.
`MpfrRounder::roundFromMpfr` rounds straight into the target format.

### Test rounding against an oracle, not against itself

Checking `encode(decode(b)) == b` for every input passes even when the encoder
is wrong, because it never sees a value the format cannot already hold. That is
exactly why the subnormal bug above survived an exhaustive 65,536 input test.
`testAgainstCompilerRounding` compares against the compiler's own conversions
from `double` to `__bf16` and `_Float16`, which are an independent,
correctly-rounded implementation.

### An unguarded loop over the input space will take the machine down with it

One `Sample` is about 72 bytes, so an exhaustive f32 scan asks for around
300 GB, and an exhaustive f64 scan effectively never terminates.
`Scanner::run` refuses any exhaustive scan above 2^24 inputs, so no caller,
whether UI, script, or something added later, can trigger it.

To ask "does this filter match anything?", use `countAdmittedInputs(cfg, cap)`.
Never enumerate: the enumeration is the thing that kills you. This one was
learned the hard way, when an inverted confirmation dialog meant that answering
"don't sample" was the option that ran the impossible scan.

### A wrong calling convention does not fail, it returns garbage

Calling an fp8 or double-ABI function through the bf16 convention produces
plausible numbers and a 100 percent mismatch rate rather than an error.

### fp8 has no native hardware type

`_Float16` and `__bf16` travel in the low 16 bits of an SSE register. fp8
travels in `%dil` and returns in `%al`. Passing `1.0f` where a `__bf16`
argument is expected makes the callee read zero, with no error anywhere.

### bf16 and f16 are both 16 bits wide

Anything keyed on bit width, such as a combo box's item data or a cache, will
conflate them. Key on the format index.

### lgamma is log absolute gamma, not NaN where gamma is negative

`lgamma(-0.5)` is `1.2655...`, a perfectly finite answer. MPFR reports the sign
of gamma separately and it must be discarded, or the reference disagrees with
every real libc.

### Benchmark what you draw, and count the pixels

A micro-benchmark of the scatter plot found `drawPoints` far faster than
65,536 individual `drawEllipse` calls, and `drawPoints` with `NoPen` set draws
nothing at all. The "1 ms" was a no-op and the plot was blank. `QPainterPath`
was both slower than the original and rasterised away most of the points, so a
first attempt at batching made things worse. `bench/plotbench.cpp` therefore
reports painted pixels alongside timing, so that a fast no-op cannot read as a
good result. End to end, repaint went from 72 ms to 5.9 ms for 65,536 points,
and from 1.15 s to 80 ms for a million.

### Truncating a float is not a rounding error

Round-to-nearest and truncation only disagree when the discarded bits are
exactly one half, which essentially never happens for a transcendental result.
An early version of the demo "rounded toward zero" that way, and ulpscope
correctly reported zero mismatches for it.

### Dock areas arrange around the central widget

With no central widget, or one that can be squeezed to nothing, Qt hands the
entire middle row to the right-hand dock. A right-side panel then renders as a
full-width band and the plot gets squashed. The empty central widget and its
minimum width in `MainWindow` are load-bearing, not decoration.

### resizeDocks is a request, and it is discarded before the first layout

Calling it while building the UI does nothing, because the dock areas have no
sizes yet. The default arrangement is applied from a deferred call in
`showEvent` for that reason.

---

## Project layout

```
src/core/     numeric core, with no GUI dependencies
  format.*      IEEE-754 formats: decode, encode, ULP arithmetic
  mpfr_util.h   correctly rounded conversion into a target format
  mpfrref.*     the reference function registry
  dynlib.*      dlopen, ELF symbol enumeration, calling conventions
  scan.*        the scan engine, input filters, ULP comparison
  sweep.*       checking every function in a library
  exporter.*    test vector and CSV generation
src/ui/       Qt 6 widgets
demo/         the stand-in library
bench/        plot repaint benchmark
tests/        the ctest suite
```

`src/core` depends only on `QtCore` and `QtGui`, so the numeric core can be
reused from a command-line harness or a test without a display.

---

## Testing

`ctest` runs 210 checks. The parts worth knowing about:

- **Exhaustive round trips** for e4m3, e5m2, f16 and bf16: all 256 or 65,536
  inputs decode and re-encode identically, plus a sampled f32 check.
- **Cross-check against the compiler's own conversion**, which is the test
  that would have caught the subnormal bug.
- **`ulpOf` equals the gap to the next representable value**, for every normal
  value in every format.
- **Input-set completeness**: a restrictive filter plus random mode must report
  that fewer inputs exist than were requested, rather than returning a short
  scan as if it were complete.
- **A missing reference is an error**, not a scan reporting every input as
  wrong.
- **Neighbouring arguments reach what fixed arguments cannot**: a correct
  `fmax` looks clean with a fixed second argument and still shows its
  signed-zero cases when the arguments are neighbours.
- **Sweep and tolerance**: a 1 ULP tolerance must admit a 1 ULP defect and
  still reject a strict scan, and per-class mismatch counts must sum to the
  total.
- **Oversized exhaustive scans are refused**, before anything is allocated.
- **f64 end to end**, sampled, including a symbol that needs a reference
  override.
- **Calling-convention inference**, including that it declines to guess when a
  name is ambiguous.

Two helpers for headless machines:

```sh
./build/ulpscope --selftest shot.png   # runs scans, writes screenshots
cmake --build build --target ulpscope_bench   # repaint time and painted pixels
```