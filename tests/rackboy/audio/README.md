# Rackboy Golden-Audio Regeneration Procedure

This directory hosts the **deterministic golden-audio regression** for
rackboy — the regression that verifies every voice's rendered output
stays bit-reproducible across refactors of the APU shim, table
interpreter, voice scheduler, and resampler.

This file is the **binding recipe** for regenerating the reference
buffers under `tests/audio/reference/`. The regression test itself
(tracked by Beads `rackboy-5wl`) reads those buffers and compares
against a fresh render; if this recipe drifts from what the test
does, the regression is worthless. **Update this file and the test in
the same commit** whenever either changes.

> **Current state (iteration 81):** the regression test
> (`tests/audio/regression.cpp`), the trigger / clock
> pattern (`tests/audio/pattern.csv`), the regeneration
> tool (`tools/regen_reference/regen.sh`), and the four
> committed `.bin` reference buffers + `metadata.json`
> all live in-tree. The RMS-vs-reference CHECK runs on
> every `make -C tests test-audio` invocation; the
> skip-when-absent branch is dormant unless the
> reference directory is emptied. Regeneration is a
> single `tools/regen_reference/regen.sh` call from a
> clean worktree.

## Binding parameters

Every row below is **frozen** per the project's Architecture Decisions
(see `docs/prompts/rackboy-lsdj-quad-voice-module.md#architecture-decisions-binding-for-the-task-graph`,
decision #11). Changing any of these values invalidates every committed
reference buffer and requires:

1. A new entry in `docs/DECISIONS.md` that records the change and its
   rationale.
2. Regeneration of **every** reference buffer under
   `tests/audio/reference/`.
3. An update to this file replacing the old values.

| Parameter                  | Value                                          | Source of truth |
|----------------------------|------------------------------------------------|-----------------|
| Sample rate                | **48000 Hz**                                   | AD #11          |
| Render duration            | **2.0 s** (96 000 samples per channel)         | AD #11          |
| Bit depth / sample format  | **32-bit float** (PCM, little-endian, mono per channel) | [D-013](../../docs/DECISIONS.md#d-013--golden-audio-reference-buffers-stored-as-raw-f32le-bin-not-wav) |
| Channel count per buffer   | **4** (Pulse1, Pulse2, Wave, Noise — one `.bin` per channel) | AD #11 output mapping + [D-013](../../docs/DECISIONS.md#d-013--golden-audio-reference-buffers-stored-as-raw-f32le-bin-not-wav) |
| Base note                  | **C5 / MIDI 72**                               | AD #5           |
| RMS acceptance epsilon     | **< −60 dBFS** per channel over the full 2 s window | AD #11          |
| Sample-accurate alignment  | **Yes** (no ±1 sample jitter tolerated)        | AD #11          |

AD numbers refer to the Architecture Decisions list in the project
prompt. These are not re-litigated here — `docs/DECISIONS.md` is where
future changes go.

## Renderer build + determinism requirements

The binding parameters above are necessary but not sufficient for
reproducibility. The rackboy offline renderer that produces reference
buffers MUST also satisfy every item in this section. The wrapper
script under "Regeneration procedure" below is responsible for
enforcing these at regeneration time; the regression test
(`rackboy-5wl`) is responsible for re-asserting them at verify time.

### Forbidden compiler flags

The golden-audio renderer build **must not** pass any of the
following (or their GCC / Clang equivalents):

- `-ffast-math`
- `-funsafe-math-optimizations`
- `-ffinite-math-only`
- `-fassociative-math`
- `-freciprocal-math`
- `-fno-signed-zeros`
- `-fno-trapping-math`

Any of these rewrites float associativity, re-orders evaluation, or
swaps IEEE-compliant transcendentals for faster approximations, all
of which destroy bit-level reproducibility. The regeneration wrapper
script MUST `grep` the recorded `rackboy_build_flags` for these
tokens and exit non-zero if any are present.

### Denormal handling (FTZ / DAZ)

x86-64 CPUs have a per-thread "flush denormals to zero" / "denormals
are zero" mode that can be toggled by SSE MXCSR writes (via
`-ffast-math`, via `_MM_SET_DENORMAL_MODE`, or via third-party code
in the process). A renderer whose output depends on denormal values
near silence will produce different buffers under FTZ-on vs FTZ-off.

Rackboy locks the renderer to **Option 1** (bead `rackboy-7hy`):
`set_ieee_denormal_mode()` in `tests/audio/regression.cpp` runs at
every `render()` entry and explicitly clears FTZ + DAZ on both
x86-64 (via `_MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_OFF)` +
`_MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_OFF)`) and aarch64
(via an `msr fpcr` write clearing bit 24). This is the default and
is the only mode the committed reference buffers target.

As a belt-and-suspenders complement to the runtime lock, we **also**
record the mode in `metadata.json.denormal_mode = "ftz_off_daz_off"`
and have the RMS `TEST_CASE` REQUIRE that value before comparing
buffers. A stale metadata value (or an ambient SSE/FPCR write that
escaped `set_ieee_denormal_mode`) surfaces as a hard test failure
pointing at `tools/regen_reference/regen.sh` as the fix, instead of
silently producing incorrect RMS.

Other values in the four-state vocabulary — `ftz_on_daz_on`,
`ftz_on_daz_off`, `ftz_off_daz_on` — are reserved for the unlikely
case that a future spike finds `rackboy_apu` numerically unstable
under IEEE-correct denormal handling. A fifth value `"ambient"`
existed between iterations 81 and 82 as an explicit "mode
untracked" marker; the RMS gate now treats that as a fail
condition, mapping the escape hatch through a follow-up bead
rather than a silent default.

### SIMD and libm

The golden-audio render path must be **bit-exact cross-platform
between macOS and Linux** on x86-64 and aarch64. To achieve this:

- **Disable SIMD in the render path** for the golden-audio build
  (e.g. `-fno-tree-vectorize` on GCC / Clang, or per-function
  attributes on the rackboy_apu core). Hand-vectorized code paths
  are not allowed in the golden-audio render. The non-golden
  builds (VCV plugin, Daisy firmware) can re-enable SIMD freely
  since their output is not regression-gated.
- **Pin the libm implementation or prove bit-exactness.** Different
  libm versions (macOS vs glibc vs musl) produce last-ULP differences
  in `sin()`, `cos()`, `log()`, etc. If rackboy_apu only uses
  fixed-point / rational arithmetic internally (no transcendentals),
  this is a non-issue and the doc should note that. If transcendentals
  are used, the wrapper script must record the libm identity in
  `metadata.json.libm_version`.
- **Record the compiler identity** (`cc --version` or equivalent)
  in `metadata.json.rackboy_build_flags` — not just the flags, but
  the toolchain version that produced the binary.

### Vendor submodule state

Rackboy depends on **two vendored trees** that directly affect the
rendered output:

- `rackboy_apu/vendor/sameboy/` — the APU core itself. Any drift
  here produces different audio.
- `tests/third_party/doctest/` — does not affect audio, but is
  recorded for completeness.

The wrapper script MUST compute `git submodule status` equivalents
for the vendored trees (for plain directories, the SHA-256 of the
directory manifest — a sorted list of path+SHA pairs for every
tracked file) and write them into `metadata.json.rackboy_vendor_shas`.
A single rackboy commit SHA is necessary but not sufficient when the
vendor tree can be locally modified (it can't under clean-import
discipline, but defense-in-depth matters here).

### UBSan-clean requirement

The golden-audio renderer build MUST pass a clean `-fsanitize=undefined`
run across the regeneration invocation. Undefined behavior (signed
overflow, null deref, misaligned access, etc.) is a common source of
"mysteriously different audio on different compilers." The UBSan-
instrumented render is gated by `scripts/ci_local.sh` stage 4 (bead
`rackboy-z5f`), which rebuilds the audio regression with
`-fsanitize=undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer`
layered onto `tests/Makefile`'s additive `EXTRA_CXXFLAGS` / `EXTRA_CFLAGS` /
`EXTRA_LDFLAGS` hooks, into an isolated `tests/build-ubsan/` tree
(`BUILD_DIR=build-ubsan` override). The additive-hook layering is what
keeps stage 4 from duplicating the Makefile's baseline flags — a future
edit to CXXFLAGS/CFLAGS defaults carries through automatically. The
isolation is load-bearing in both directions: tests/Makefile caches
per-module `.o` files for the C sources (vendored SameBoy `apu.c`,
`rackboy_apu.c`, `rackboy_apu_shim.c`) across invocations, so if stage
4 shared storage with stage 3, a later release-build stage 3 would
link UBSan-runtime-referencing objects and fail with "undefined
symbols `___ubsan_handle_*`". Stage 4 also `rm -rf`s the isolated dir
before each run so a stale object from an earlier toolchain cannot
silently weaken sanitizer coverage for a subset of translation units.

The committed reference buffers are still produced from a release
(non-UBSan) build regardless. The committed reference buffers are still
produced from a release (non-UBSan) build regardless.

## Required input artifacts

All four must exist on disk **before** the regeneration is run. Each
is committed to the repo and byte-pinned; do not regenerate these
without an explicit DECISIONS.md entry.

### 1. Fixture `.sav` file

- **Path:** `tests/fixtures/rackboy_v1.sav` (committed)
- **Owner:** Beads `rackboy-pj3` (supersedes `rackboy-n61`)
- **Current state (2026-04-14):** populated 128 KB LSDJ `.sav`
  authored via the `rackboy-fixture-gen` binary in
  `gb-lsdj-instrument-extractor` at commit `2f4c454`. Contains
  one project (slot 0, name `RACKBOY`) with 4 pre-allocated
  instruments: Pulse (slot 0, authored for Pulse1), Pulse (slot
  1, authored for Pulse2), Wave (slot 2), Noise (slot 3). See
  [`tests/fixtures/README.md`](../fixtures/README.md) and
  [`docs/DECISIONS.md` D-006][d-006] for the authorship
  procedure.
- **Contract:** a `.sav` that exercises every in-scope LSDJ
  instrument type byte (Pulse `0x00`, Wave `0x01`, Noise `0x03`)
  plus the container + decompression path. Table commands are
  still deferred to a later follow-up fixture; instruments that
  reference the deferred `chord` command MUST NOT be present
  (see `docs/DECISIONS.md` D-001).

[d-006]: ../../docs/DECISIONS.md#d-006--populated-rackboy_v1sav-authored-via-rackboy-fixture-gen
- **Verification:** `tests/fixtures/expected.json` (committed,
  byte-for-byte) is what the parser must produce when it loads
  `rackboy_v1.sav`. Any parser change that alters this file requires a new
  `rackboy-43y` spike run.

### 2. Fixture JSON sidecar

- **Path:** `tests/fixtures/rackboy_v1.json` (committed, optional per AD #8)
- **Contract:** the `<savname>.json` instrument-name sidecar used by
  the module's UI. Present in this directory so the fixture bank
  renders identically regardless of whether the user has a sidecar.
- **Not strictly required for the regression** — the golden audio is
  a function of the `.sav` alone — but committing it here keeps the
  fixture self-contained.

### 3. Trigger / clock pattern

- **Path:** `tests/audio/pattern.csv` (committed)
- **Format:** UTF-8 CSV, one event per row, columns:
  `time_seconds, event_type, event_value`. `event_type` is one of:
  - `gate1_rising` — Gate 1 rising edge (trigger). `event_value` is
    the four-channel instrument-slot tuple, e.g. `"0,1,2,3"` meaning
    Pulse1→instrument 0, Pulse2→instrument 1, Wave→instrument 2,
    Noise→instrument 3. Knob+CV state is held for the full 2-second
    window unless a later `knob_set` row overrides it.
  - `gate2_rising` — Gate 2 rising edge (clock). No `event_value`.
  - `knob_set` — change the CV/knob sum mid-render. `event_value` is
    a 4-float tuple in `[0.0, 1.0]`.
  - `gate1_falling`, `gate2_falling` — explicit falling edges. In
    rackboy the gate semantics are edge-sensitive, so these are
    typically not used but are tolerated for completeness.
- **First-row rule:** the first row MUST be `gate1_rising` at
  `time_seconds = 0.0` with an explicit instrument-slot tuple. This
  ensures the render starts from a deterministic state regardless of
  the renderer's initial knob readings.
- **Termination:** events at `time_seconds >= 2.0` are ignored by the
  regression.

### 4. Rackboy binary committed to the test pin

The regeneration MUST be run from a **clean build at a specific git
commit**. The commit SHA is recorded at the top of each reference
buffer's sidecar `.json` (see "Output format" below). Running the
regeneration from a dirty worktree or a different commit is NOT
acceptable — the reference buffers would drift silently against the
source.

## Output format

Each regeneration run produces **four** raw `.bin` files and
**one** sidecar metadata file:

```
tests/audio/reference/
├── pulse1.bin      raw f32le, 48 kHz, 96 000 samples (384 000 B)
├── pulse2.bin      raw f32le, 48 kHz, 96 000 samples (384 000 B)
├── wave.bin        raw f32le, 48 kHz, 96 000 samples (384 000 B)
├── noise.bin       raw f32le, 48 kHz, 96 000 samples (384 000 B)
└── metadata.json   render provenance
```

The `.bin` files are **raw IEEE-754 float32 little-endian PCM
with no container header**. This matches the bead description
for `rackboy-5wl` and keeps the regression test at zero-
dependency (no libsndfile, no WAV header parsing). A future
bead may ship a thin WAV-wrapped mirror under
`tests/audio/reference/wav/` if an audition workflow (e.g.
dragging into a DAW) ever justifies the extra artifacts —
the raw `.bin` stays authoritative.

`metadata.json` schema (every field required unless marked optional):

```json
{
  "rackboy_commit": "40-char git SHA at time of regeneration",
  "rackboy_binary_sha256": "sha256 of the built renderer binary on disk",
  "rackboy_build_flags": "exact CXXFLAGS + CFLAGS + LDFLAGS passed to the binary",
  "compiler_identity": "output of `$(CXX) --version | head -1`",
  "libm_version": "identifier string for the libm used (optional; only required if the renderer calls transcendentals)",
  "denormal_mode": "one of: ftz_off_daz_off | ftz_on_daz_on | ftz_on_daz_off | ftz_off_daz_on",
  "rackboy_vendor_shas": {
    "rackboy_apu/vendor/sameboy": "sha256 of sorted <path>:<sha1> manifest for the vendored SameBoy tree"
  },
  "sample_rate_hz": 48000,
  "duration_samples": 96000,
  "sample_format": "f32le",
  "fixture_sav_sha256": "sha256 of tests/fixtures/rackboy_v1.sav",
  "pattern_csv_sha256": "sha256 of tests/audio/pattern.csv",
  "regenerated_on": "ISO-8601 timestamp",
  "regenerated_by": "short free-form note (human or agent name)"
}
```

The regression test (`rackboy-5wl`) MUST fail fast if any of these
fields disagree with the current on-disk artifacts — that is the
signal that either the fixture / pattern drifted, the renderer
binary was rebuilt with different flags, the vendor tree was
touched, or the reference buffer is stale. The `rackboy_binary_sha256`
+ `rackboy_vendor_shas` pair is the tightest check; the git commit
alone is necessary but not sufficient because a clean rebuild from
the same commit with different toolchain/flags can produce a
different binary.

## Regeneration procedure

### Preconditions

1. Working tree clean on `main` or a dedicated regeneration branch.
2. `git rev-parse HEAD` captures a committed state (no uncommitted
   changes).
3. The rackboy offline renderer has built successfully — see the
   `rackboy_apu` host-side test harness, Beads `rackboy-997`
   (rackboy_apu: host-side APU test harness binary). This is the
   binary that drives the render.
4. `tests/fixtures/rackboy_v1.sav`, `tests/fixtures/rackboy_v1.json` (if
   present), and `tests/audio/pattern.csv` are all byte-identical to
   their committed versions (`git status tests/fixtures tests/audio`
   should show nothing modified).

### Canonical invocation

The wrapper script is `tools/regen_reference/regen.sh` (bead
`rackboy-n2x`). Run it from the repo root with a clean
worktree:

```sh
tools/regen_reference/regen.sh                 # default: tests/audio/reference/
tools/regen_reference/regen.sh --no-clean      # skip make clean (dev iteration only)
tools/regen_reference/regen.sh --out-dir /tmp  # scratch output to /tmp
```

The historical "tools/render_golden_audio.sh" pathname from
earlier iterations of this document was never implemented;
the landed shell wrapper lives under `tools/regen_reference/`
and shares the same responsibilities. See
`tools/regen_reference/README.md` for the design rationale
(the renderer is the audio test binary itself — single-
sourcing eliminates drift between the regen path and the
RMS-verification path).

The wrapper script is deliberately thin — its job is to:

1. Validate preconditions (clean tree, inputs present, output
   directory empty or overwrite-confirmed).
2. Compute `sha256` of the two input files.
3. Invoke the rackboy offline renderer with the bindings above.
4. Write `metadata.json` populated with the SHA, the flags, and the
   current commit.
5. Exit nonzero on any validation or render failure.

### Acceptance check after regeneration

Run the regression test against the freshly-regenerated reference
buffers — a re-render MUST round-trip through the check cleanly:

```sh
make -C tests test-audio    # (or the CI target once rackboy-bzi lands)
```

If the re-render disagrees with itself, the renderer or the fixture
is non-deterministic and the regression cannot be committed. Investigate
the RNG seed source in the table interpreter (see
`docs/LSDJ_REFERENCE.md` entry for the `Z` command), SIMD flags, and
any `-ffast-math`-like floating-point optimizations before concluding
the regression is ready to commit.

### Commit procedure

Once the regression round-trips cleanly, commit the new reference
buffers + `metadata.json` in a **single commit** alongside any
rackboy source change that motivated the regeneration. The commit
message MUST include:

1. The git SHA recorded in `metadata.json` (usually the commit SHA
   one ancestor back, since the commit itself hasn't been made yet).
2. A one-sentence explanation of why the regeneration was needed
   (e.g., "rackboy_apu CH3 wave RAM handler change shifts every CH3
   sample by 1 ULP"). Vague reasons like "updated golden audio" are
   not acceptable — if you cannot articulate the cause, the
   regeneration is probably wrong.
3. A link to the relevant `docs/DECISIONS.md` entry, if the
   regeneration was triggered by a binding-parameter change.

## Determinism checklist

Before committing reference buffers, verify each item:

- [ ] Working tree clean at the SHA recorded in `metadata.json`.
- [ ] `.sav`, `pattern.csv`, renderer binary, and vendor-tree
      manifest hashes all match `metadata.json`.
- [ ] Renderer built with none of the forbidden flags
      (`-ffast-math`, `-funsafe-math-optimizations`, etc. — see
      "Renderer build + determinism requirements" above). The
      wrapper script enforces this but the checklist is a
      belt-and-suspenders reminder.
- [ ] Renderer was built UBSan-clean (`-fsanitize=undefined` run
      against the golden fixture exits zero).
- [ ] Re-run of the renderer against the same inputs produces a
      byte-identical `.bin` set (diff every `.bin` — float 32 PCM is
      bit-stable with a correct-rounding renderer).
- [ ] Regression test (`rackboy-5wl`) passes green from a clean
      `make -C tests test-audio` invocation **once `rackboy-bzi`
      lands that target in `tests/Makefile`**. Until then, the
      regression is invoked via its module directly (`make -C tests
      test-audio` will print a skip notice).
- [ ] All four `.bin` files are exactly 384 000 bytes long (96 000
      float32 samples each).
- [ ] No non-silent samples appear on channels that should be silent
      for a given fixture instrument (SPIKE-1 acceptance, AD #12).

## When this file or the regression changes

This file and `rackboy-5wl`'s test code are coupled. Do not drift them
apart. The commit that changes one MUST also update the other (or
explicitly note in the commit why no update is needed, and reference
the next iteration that will reconcile them).

Any change to the **binding parameters** table above requires a
`docs/DECISIONS.md` entry FIRST, then the procedure update and the
reference regeneration in the same follow-up commit.
