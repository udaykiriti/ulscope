# ulpscope

A low-precision float explorer and ULP error plotter.

`bfloat16` and `float16` have only 65,536 distinct inputs, so you can check
*every* one of them exhaustively, instantly, against a correctly rounded MPFR
reference. ulpscope does that, plots the error, and hands you the failing
inputs as C++ test vectors.

It is built for the case where you have just written (or ported) a low-precision
math function and want to know, exactly, where it stops being correctly
rounded.

## What it does

- **Bit inspector** — type a value in decimal, C99 hex float, or hex bit
  pattern, or just click the bits. See the sign/exponent/mantissa split, the
  exact decimal and hex float, the class (normal, denormal, infinity, NaN), the
  spacing at that value, and the neighbours. Formats: **e4m3, e5m2, f16, bf16,
  f32, f64**.
- **Scan** — point it at a `.so`, pick a function, and it evaluates that
  function over every input (or a seeded random sample) and compares each
  result against MPFR.
- **Library sweep** — check *every* exported function that ulpscope can both
  call and reference, and get them ranked by error. This is the question you
  actually want answered when picking up an unfamiliar math library.
- **ULP plot** — input on x, error in ULP on y. Click a point to see the input,
  your output, the expected output and the bits. Colour by result or by input
  class.
- **Worst-case table** — sorted by error, filterable to mismatches only, with
  per-class error counts.
- **Input filters** — restrict a scan to a class (zero / normal / denormal /
  inf / NaN) and to an exponent range. An exhaustive bf16 sweep takes a second,
  so the useful question is usually "what does this break *on*?". If a filter
  admits fewer inputs than you asked for, ulpscope says so rather than reporting
  a short scan as a success.
- **Argument modes** — for two- and three-argument functions, choose whether the
  trailing arguments are **fixed**, **randomised**, or **neighbouring** the
  first argument. Neighbouring is what makes `fmin`/`fmax`/`fdim` testable at
  all: their defects live at equality, at orderings, and at the signed-zero
  boundary, and no fixed second argument reaches any of those.
- **Reference override** — for a symbol whose name does not reduce to a known
  function, say what it implements (`log2_viaf32` is `log2`). Without it such a
  symbol can only be checked by hand, and an unnoticed failure there looks like
  a function that is wrong on every input. With it, a missing reference is a
  clear error rather than a 100% mismatch rate.
- **Tolerance** — declare 1 ULP acceptable when a function genuinely cannot be
  correctly rounded everywhere. 0 means correctly rounded.
- **Export** — failing inputs as a C test table, as an llvm-libc style lit
  test, CSV, and the whole sweep as CSV. The case limit is adjustable.

The plot zooms with the wheel, pans with a left drag, and resets on right-click
or double-click. The y axis rescales to what is visible, so zooming into a quiet
region is not flattened by an outlier elsewhere.

Formats and reference functions are both **data**: adding a format is a row in
`formatSpecs()` in `src/core/format.cpp`, and adding a function is a row in
the table in `src/core/mpfrref.cpp`. Nothing else needs to change.

## Windows and layout

Every panel — scan options, input filter, ULP plot, results, library sweep, bit
inspector — is a dock widget. They can be dragged to any edge, floated into
their own window (so a scan can be spread across two monitors), hidden, or
tabbed together. The arrangement is saved on exit and restored on the next run;
**View → Reset layout** puts everything back.

The scan options are a grid rather than one long toolbar row, because in a row
the labels end up pressed against their controls and the strip overflows.

Two things about this were not obvious:

- **Dock areas arrange around the central widget.** With no central widget, or
  one that can be squeezed to nothing, Qt hands the entire middle row to the
  right-hand dock — a right-side panel renders as a full-width band and the plot
  gets squashed. The empty central widget and its minimum width are load-bearing,
  not decoration.
- **`resizeDocks` is a request, and it is discarded if the dock areas have no
  sizes yet.** Calling it while building the UI does nothing; the default
  arrangement has to be applied after the first layout pass, hence the deferred
  call in `showEvent`.

## Building

Requires Qt 6 (Core/Gui/Widgets), CMake, and MPFR with development headers.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/ulpscope
```

On startup ulpscope loads its own bundled demo library so there is something to
look at immediately.

## Pointing it at your own code

ulpscope never links against your math library; it reaches it through `dlsym`,
so no FFI layer and no rebuild is needed. Build your functions as a shared
object with exported symbols and select one.

The **ABI** menu says how to call it:

| ABI | Signature | Use for |
| --- | --- | --- |
| `16-bit native` | `bf16 f(bf16)` / `f16 f(f16)` | llvm-libc's own `__bf16` / `_Float16` signatures |
| `uint8_t f(uint8_t)` | `uint8_t f(uint8_t)` | fp8 e4m3 / e5m2, which have no native type |
| `double` | `double f(double)` | wrappers, anything that promotes its argument |
| `float` | `float f(float)` | an honest f32 implementation |

The **ABI** and **format** are inferred from the symbol name — `sqrte4m3` gets
e4m3 and the integer ABI, `exp_f32` gets f32 and float — and choosing either
manually overrides that. A wrong ABI does not fail loudly, so the library
sweep always infers per function rather than trusting one setting.

The 16-bit option passes the encoding in the low half of the SSE register,
which is where the compiler puts a `__bf16` argument, and reads the result back
from the same place.

Argument count is inferred from the function name (`pow` is two arguments,
`lgammabf16` is one) and can be overridden.

If ulpscope does not recognise a name, it says so rather than guessing. Symbols
are matched by stripping type suffixes, so `sqrtf`, `sqrtbf16`, `sqrt_bf16`,
`sqrt_f32` and `log2f128` all resolve to a known reference; `sqrtdf` and
`fsqrt` are recognised aliases for `sqrt`. A name that matches nothing is
reported rather than silently compared against the wrong function.

## Reference coverage

Around 60 functions, which is everything llvm-libc's list needs:
`sqrt cbrt exp exp2 exp10 expm1 log log2 log10 log1p logb ilogb
sin cos tan asin acos atan sinh cosh tanh asinh acosh atanh
fabs floor ceil round trunc rint nearbyint roundeven lrint llrint lround llround
lgamma tgamma digamma zeta erf erfc j0 j1 y0 y1
pow atan2 fmod remainder hypot fmin fmax fdim copysign fma`

Add one in `src/core/mpfrref.cpp` by adding a row to the table.

## f32

f32 has 4 billion inputs, so exhaustive mode is not offered for it. Use
**Random sample** (seeded, so a scan is reproducible) or **Range**. The random
generator is splitmix64 and the seed is fixed, so the same configuration always
selects the same inputs.

## The demo library

`demo/bf16_demo.cpp` stands in for "an implementation under test" and
deliberately varies in quality, so the plot shows something on first run:

| Function | Behaviour | Result |
| --- | --- | --- |
| `sqrtbf16`, `expbf16`, `logbf16`, `sinbf16`, `erfbf16`, `cbrtbf16` | correctly rounded | 0 mismatches |
| `fabsbf16` | exact bit manipulation | 0 mismatches |
| `lgammabf16` | rounds toward zero | ~1 ULP on most inputs |
| `tgamma_bf16` | rounds away from zero | ~1 ULP on most inputs |
| `sqrtdfbf16` | `sqrt` via `powf` in `float`, so double-rounded | scattered errors |
| `sqrte4m3` | correctly rounded fp8 | 0 mismatches |
| `loge5m2` | widens through f16 first, so double-rounded | fp8 rounding errors |
| `sqrt_double`, `log_double` | correctly rounded f64 | 0 mismatches |
| `log2_viaf32` | computes in f32 and widens | f64 errors; needs a reference override |
| `fmaxf`, `fdimf` | wrong at the signed-zero boundary | found only by neighbouring args |

`lgammabf16_d`, `sqrtbf16_d` and `powbf16_d` are `double`-ABI wrappers,
`sqrt_f32` / `lgamma_f32` are `float`-ABI, `sqrtf16` / `expf16` / `sinf16` /
`lgammaf16` are 16-bit f16, and `fmax_double` / `fminf` / `fmaxf` / `fdimf`
are two-argument, so every ABI, every format and the argument modes all have
something to exercise.

### A note on `fmax` and signed zero

The C standard leaves `fmax(+0, -0)` and `fmin(+0, -0)` unspecified, so
ulpscope's correctly rounded reference and a conforming implementation may
legitimately disagree on those two inputs. With **neighbouring** arguments,
`fmax_double` — which is otherwise correct — reports exactly 3 such cases out
of 65,536. Those are not a defect in the implementation, and they are why a
tolerance exists.

## Notes from building it

A few things that were not obvious going in, and are commented in the source:

- **MPFR cannot round to IEEE subnormals through `emin`/`emax`.** MPFR clamps
  the exponent range, but it does *not* coarsen the significand grid for
  subnormal values the way IEEE does. Measured on f16 (bias 15, 10 stored
  mantissa bits), rounding `5.9212671107461366e-05` at precision 11 gave
  `993.5 × 2⁻²⁴` for **every** value of `emin`, where IEEE requires
  `993 × 2⁻²⁴`: the IEEE subnormal grid is one step of `2^(1-bias-mantBits)`,
  which is coarser than 11 significant bits at that magnitude. So `MpfrRounder`
  never sets `emin`/`emax`; it handles the two cases separately — subnormals by
  an exact power-of-two shift followed by `mpfr_get_si(..., MPFR_RNDN)`, which
  *is* round-to-nearest-ties-to-even, and normals by rounding to
  `1 + mantBits` significant bits at a wide exponent range. See
  `src/core/mpfr_util.h`.
- **Never round a reference through a `double`.** `mpfr_get_d` at 200 bits
  followed by a second rounding to bf16 is double rounding and can disagree
  with the single correct rounding. `MpfrRounder::roundFromMpfr` rounds
  straight into the target format.
- **Test rounding against an oracle, not just against itself.** Checking
  `encode(decode(b)) == b` for every input passes even when the encoder is
  wrong, because it never sees a value the format cannot already hold. That is
  exactly why the subnormal bug above survived an exhaustive 65,536-input test.
  `testAgainstCompilerRounding` compares against the compiler's own
  `double → __bf16` / `double → _Float16` conversions, which are an
  independent correctly-rounded implementation.
- **`lgamma` is `log|Γ|`, not NaN where Γ is negative.** `lgamma(-0.5)` is
  `1.2655…`, a perfectly finite answer. MPFR reports the sign of Γ separately
  and it must be discarded, or the reference disagrees with every real libc.
- **Benchmark what you draw, and count the pixels.** A micro-benchmark of the
  scatter plot found `drawPoints` 150× faster than 65,536 individual
  `drawEllipse` calls — and `drawPoints` with `NoPen` set draws *nothing at
  all*, so the "1 ms" was a no-op and the plot was blank. `QPainterPath` was
  both slower than the original *and* rasterised away most of the points.
  `bench/plotbench.cpp` therefore reports painted pixels alongside timing, so a
  fast no-op cannot read as a good result. Measured cost per repaint is now
  5.9 ms for 65,536 points and 80 ms for a million.
- **An unguarded loop over the input space will take the machine down with
  it.** One `Sample` is ~72 bytes, so an exhaustive f32 scan asks for ~300 GB,
  and an f64 scan effectively never terminates. `Scanner::run` therefore
  refuses any exhaustive scan above 2²⁴ inputs, so no caller — UI, script, or
  something added later — can trigger it. Ask "does this filter match
  anything?" with `countAdmittedInputs(cfg, cap)`, never by enumerating: the
  enumeration is the thing that kills you. This one was learned the hard way;
  an inverted confirmation dialog meant answering "don't sample" was the option
  that ran the impossible scan.
- **A wrong calling convention does not fail, it returns garbage.** Calling an
  fp8 or `double`-ABI function through the bf16 convention produces plausible
  numbers and a 100% mismatch rate rather than an error, so the sweep infers
  the format and ABI per function instead of applying one setting to
  everything.
- **fp8 has no native hardware type.** `_Float16` and `__bf16` travel in the low
  16 bits of an SSE register; fp8 travels in `%dil` and returns in `%al`. Passing
  `1.0f` where a `__bf16` argument is expected makes the callee read zero, with
  no error anywhere.
- **bf16 and f16 are both 16 bits wide.** Anything keyed on bit width — a combo
  box's item data, a cache — will conflate them. Key on the format index.
- **Truncating a `float` is not a rounding error.** Round-to-nearest and
  truncation only disagree when the discarded bits are exactly one half, which
  essentially never happens for a transcendental result. An early version of
  the demo "rounded toward zero" that way, and ulpscope correctly reported zero
  mismatches for it.

## Layout

```
src/core/     no GUI dependencies
  format.*      IEEE-754 formats, decode/encode, ULP arithmetic
  mpfr_util.h   correctly rounded conversion into a target format
  mpfrref.*     the reference function registry
  dynlib.*      dlopen, ELF symbol enumeration, the calling conventions
  scan.*        the scan engine, input filters and ULP comparison
  sweep.*      check every function in a library
  exporter.*    test vector and CSV generation
src/ui/       Qt widgets
demo/         the stand-in library
tests/        ctest suite: exhaustive round trips for every narrow format, and
              a cross-check of the encoder against the compiler
```

`src/core` deliberately has no Qt dependency beyond `QtCore`/`QtGui`, so the
numeric core can be reused in a command-line harness or a test without a
display.

## Testing

`ctest` runs the suite. The parts worth knowing about:

- **Exhaustive round trips** for e4m3, e5m2, f16 and bf16 — all 65,536 (or 256)
  inputs decode and re-encode identically — plus a sampled f32 check.
- **Cross-check against the compiler's own conversion.** See the note above;
  this is the test that would have caught the subnormal bug.
- **`ulpOf` equals the gap to the next representable value**, for every normal
  value in every format.
- **Sweep and tolerance**: a 1 ULP tolerance must admit a 1 ULP defect and
  still reject a strict scan, and per-class mismatch counts must sum to the
  total.
- **Input-set completeness**: a restrictive filter plus Random mode must report
  that fewer inputs exist than were requested, rather than returning a short
  scan as if it were complete.
- **A missing reference is an error**, not a scan reporting every input as
  wrong.
- **Neighbouring arguments reach what fixed arguments cannot**: a correct
  `fmax` looks clean with a fixed second argument and still shows its
  signed-zero cases when the arguments are neighbours.
- **f64 end to end**, sampled, including a symbol that needs a reference
  override.
- **Calling-convention inference** is checked against the ambiguous cases as
  well as the obvious ones, including that it declines to guess.

`./build/ulpscope --selftest <path>.png` runs a scan, a sweep, and an fp8 scan
without a mouse, and writes screenshots — useful on a headless machine.

`cmake --build build --target ulpscope_bench` builds a plot benchmark that
reports repaint time *and* painted pixels.