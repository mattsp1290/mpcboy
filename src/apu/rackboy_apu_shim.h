/*
 * rackboy_apu_shim.h
 *
 * Rackboy-local stand-in for SameBoy's `Core/gb.h`. The vendored
 * `Core/apu.c` includes `"gb.h"` unconditionally, and upstream that
 * header pulls in the entire SameBoy Core: CPU, memory, timing, input,
 * save state, debugger, SGB, … — hundreds of files we intentionally
 * do NOT vendor (see `rackboy_apu/vendor/sameboy/SOURCE.md`). This
 * shim provides just the subset of that surface which `apu.c` and
 * `apu.h` actually reach.
 *
 * Role in the rackboy scaffold
 * ----------------------------
 * `rackboy_apu/gb.h` is a one-line indirection that `#include`s this
 * file. When `apu.c` (compiled with `-Irackboy_apu`) asks for `gb.h`
 * the preprocessor lands here, and every rackboy-owned declaration
 * lives in one place. The file reservation registered on beads task
 * `rackboy-amu` is `rackboy_apu/rackboy_apu_shim.{h,c}` — that is the
 * authoritative home for the shim's content.
 *
 * Scope of THIS task (beads: rackboy-amu)
 * ---------------------------------------
 * - Provide a minimal `GB_gameboy_t` stand-in whose sub-state fields
 *   (`apu`, `apu_output`) use the real upstream `GB_apu_t` /
 *   `GB_apu_output_t` types from `vendor/sameboy/apu.h`, plus the
 *   top-level fields `apu.c` actually reaches (`div_counter`,
 *   `cgb_double_speed`, `io_registers[0x100]`, `model`, and — added
 *   during the shim implementation after an audit of `apu.c` — the
 *   additional fields `halted`, `stopped`, `during_div_write`, `sgb`,
 *   `address_bus`, and `pc`). The prompt's original field list
 *   (`apu`, `apu_output`, `div_counter`, `cgb_double_speed`,
 *   `io_registers[0x100]`, `model`) undersized the real dependency
 *   surface; the extra fields are required for `apu.c` to actually
 *   compile and are documented line-by-line below with rationale.
 * - Stub `GB_get_clock_rate()` at the DMG rate (4,194,304 Hz).
 * - Stub `GB_is_cgb()` at `false` (rackboy v1 runs DMG-only).
 * - Provide the handful of Core-level enums (GB_model_t, GB_IO_*) and
 *   helper macros (`nounroll`, `unlikely`, `LE32`, `nodefault`, …)
 *   SameBoy's `Core/defs.h` and `Core/gb.h` normally supply. Every
 *   rackboy-local deviation from the upstream interface is marked
 *   with a `// RACKBOY LOCAL MOD` comment so the diff against SameBoy
 *   stays auditable.
 *
 * Productionization (wiring the APU driver, per-channel tap patches,
 * model-aware clock rate, real CGB/AGB branches) is tracked by:
 *   - rackboy-8d4 — productionize per-channel tap patches
 *   - rackboy-w7q — rackboy_apu init / register write / run driver
 *
 * See also: `docs/prompts/rackboy-lsdj-quad-voice-module.md` §
 * "Architecture Decisions"; `docs/DECISIONS.md` D-005 / D-008.
 *
 * License: this file is rackboy-originated; SameBoy upstream is Expat
 * (MIT). See `rackboy_apu/vendor/sameboy/LICENSE`.
 */

#ifndef RACKBOY_APU_SHIM_H
#define RACKBOY_APU_SHIM_H

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ------------------------------------------------------------------------ */
/* SameBoy's `defs.h` — rackboy-local replacement                           */
/* ------------------------------------------------------------------------ */

// RACKBOY LOCAL MOD: `apu.h` transitively `#include`s `"defs.h"`, so we
// need the rackboy defs.h visible before we include the vendored apu.h
// below. Including it here as well (even though it will also be pulled
// in from apu.h) keeps the "everything rackboy touches lives in shim.h"
// invariant clear at a glance.
#include "defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* `M_PI`                                                                   */
/* ------------------------------------------------------------------------ */

// RACKBOY LOCAL MOD: `apu.c`'s band-limited-synthesis initializer uses
// `M_PI` from `<math.h>`. ISO C99 does not require `<math.h>` to
// provide `M_PI`, and `-std=c99` without feature-test macros (GNU /
// POSIX / BSD) can leave it undefined. Declare a fallback so the
// vendored translation unit compiles regardless of the host libc
// configuration. Most hosts (glibc, musl, Apple libc) already define
// `M_PI` — the `#ifndef` guard keeps us silent in those cases.
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------------ */
/* Compiler-hint macros                                                     */
/* ------------------------------------------------------------------------ */

// RACKBOY LOCAL MOD: SameBoy's `Core/defs.h` defines a handful of
// loop-unroll / branch-prediction hint macros that `apu.c` relies on.
// Upstream implementations use GCC/Clang-specific `_Pragma` and
// `__builtin_*` intrinsics. We mirror the same names with equivalent
// semantics; where the hint has no portable equivalent we expand to a
// no-op so the code still compiles.

#if defined(__GNUC__) || defined(__clang__)
#ifndef likely
#define likely(x)    __builtin_expect(!!(x), 1)
#endif
#ifndef unlikely
#define unlikely(x)  __builtin_expect(!!(x), 0)
#endif
#ifndef unreachable
#define unreachable() __builtin_unreachable()
#endif
#ifndef noinline
#define noinline     __attribute__((noinline))
#endif
#else
#ifndef likely
#define likely(x)    (!!(x))
#endif
#ifndef unlikely
#define unlikely(x)  (!!(x))
#endif
#ifndef unreachable
#define unreachable() ((void)0)
#endif
#ifndef noinline
#define noinline     /* no-op */
#endif
#endif

// RACKBOY LOCAL MOD: `nounroll` / `unrolled` are loop-level pragmas in
// upstream SameBoy (`_Pragma("nounroll")` / `_Pragma("unroll")`). They
// are performance hints, not load-bearing for correctness — expanding
// them to empty is safe for v1. A future iteration can replace these
// with compiler-specific implementations once the rackboy_apu driver
// starts measuring APU callback budgets.
#ifndef nounroll
#define nounroll    /* no-op — see rackboy_apu_shim.h */
#endif
#ifndef unrolled
#define unrolled    /* no-op — see rackboy_apu_shim.h */
#endif

// RACKBOY LOCAL MOD: `nodefault;` appears as a bare statement at the
// end of SameBoy `switch` blocks to mark "any default here is a bug".
// Upstream it expands to `default: unreachable()`. We mirror that —
// using a statement-expression-safe expansion so `nodefault;` inside a
// `switch` body remains valid C99.
#define nodefault   default: unreachable()

// RACKBOY LOCAL MOD: `inline_const(type, init)` is SameBoy's shorthand
// for a compound literal. Upstream expansion is effectively
// `((type)init)`, which lets apu.c write `inline_const(uint8_t[], {7,
// 7, 5, 1})[duty]` and index it like a local array. We use the same
// expansion.
#define inline_const(type, ...) ((type)__VA_ARGS__)

// RACKBOY LOCAL MOD: `GB_ASSERT_NOT_RUNNING_OTHER_THREAD(gb)` is a
// SameBoy thread-safety assertion that panics if the Core is re-entered
// from another thread. Rackboy's APU driver (rackboy-w7q) is single-
// threaded within a VCV process callback and does not yet have a
// thread-identity check to assert against, so we stub this to a no-op.
//
// `apu.c` invokes this macro as a bare line with NO trailing semicolon
// (see `apu.c:~2167` and `apu.c:~2185`), so the expansion must either
// be a complete C statement OR empty. An empty expansion is the
// simplest — it leaves the invocation site as whitespace, which
// harmlessly vanishes whether it sits inside a compound statement or
// between other statements. Productionization can swap in a real
// assertion once a threading model lands.
#define GB_ASSERT_NOT_RUNNING_OTHER_THREAD(gb) /* no-op — see rackboy_apu_shim.h */

/* ------------------------------------------------------------------------ */
/* Endian helpers                                                           */
/* ------------------------------------------------------------------------ */

// RACKBOY LOCAL MOD: `apu.c`'s WAV-recording path uses `LE16` / `LE32`
// / `LE64` / `BE32` / `BE64` to serialize header fields in a defined
// byte order. SameBoy's upstream macros pick the swap direction based
// on host endianness. All current rackboy hosts (x86_64 macOS / Linux,
// ARM64 Daisy) are little-endian, so `LE*` are identity and `BE*`
// byte-swap via GCC/Clang intrinsics. The `GB_BIG_ENDIAN` guard mirrors
// apu.c's own `#ifdef` — leaving it undefined keeps the little-endian
// fast path.
#ifdef GB_BIG_ENDIAN
#error "rackboy_apu shim assumes a little-endian host. Update LE*/BE* macros before enabling GB_BIG_ENDIAN."
#endif

#define LE16(x) ((uint16_t)(x))
#define LE32(x) ((uint32_t)(x))
#define LE64(x) ((uint64_t)(x))

#if defined(__GNUC__) || defined(__clang__)
#define BE16(x) ((uint16_t)__builtin_bswap16((uint16_t)(x)))
#define BE32(x) ((uint32_t)__builtin_bswap32((uint32_t)(x)))
#define BE64(x) ((uint64_t)__builtin_bswap64((uint64_t)(x)))
#else
#define BE16(x) ((uint16_t)(((uint16_t)(x) << 8) | ((uint16_t)(x) >> 8)))
#define BE32(x) ((uint32_t)(((BE16((uint32_t)(x) & 0xFFFFu)) << 16) | BE16((uint32_t)(x) >> 16)))
#define BE64(x) ((uint64_t)(((uint64_t)BE32((uint64_t)(x) & 0xFFFFFFFFu) << 32) | BE32((uint64_t)(x) >> 32)))
#endif

/* ------------------------------------------------------------------------ */
/* MIN / MAX                                                                */
/* ------------------------------------------------------------------------ */

// RACKBOY LOCAL MOD: SameBoy's upstream `defs.h` defines `MIN` / `MAX`
// as the classic two-evaluation comparison macros. `apu.c` uses them
// with simple scalar arguments (no side effects), so the standard
// definitions are safe. Guard against existing definitions from other
// headers to stay polite if the shim is transitively included from a
// TU that already defines them.
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

/* ------------------------------------------------------------------------ */
/* `GB_model_t` enum                                                        */
/* ------------------------------------------------------------------------ */

// RACKBOY LOCAL MOD: SameBoy defines `GB_model_t` in `Core/model.h`
// (included transitively via `Core/gb.h`). We reproduce the subset
// `apu.c` compares against, using the **exact upstream numeric values**
// at the vendored commit (`208ba4a`) so the shim is a faithful
// stand-in. Upstream uses bit-flag encoding: bits 6 (PAL), 5 (GBP),
// 7 (NO_SFC) are combined with a base class via OR. We include only
// the base constants `apu.c` tests against — the bit-flag variants
// (SGB_PAL, SGB_NO_SFC, GBP_A, etc.) are not referenced by `apu.c`
// and are omitted for minimality.
//
// Verified against SameBoy `Core/model.h` at upstream commit
// `208ba4afabffab9edde416f2dbb8ae459e34adb8`.
typedef enum {
    GB_MODEL_DMG_B    = 0x002,
    GB_MODEL_SGB_NTSC = 0x004,
    GB_MODEL_MGB      = 0x100,
    GB_MODEL_SGB2     = 0x101,
    GB_MODEL_CGB_0    = 0x200,
    GB_MODEL_CGB_A    = 0x201,
    GB_MODEL_CGB_B    = 0x202,
    GB_MODEL_CGB_C    = 0x203,
    GB_MODEL_CGB_D    = 0x204,
    GB_MODEL_CGB_E    = 0x205,
    GB_MODEL_AGB_A    = 0x207,
} GB_model_t;

/* ------------------------------------------------------------------------ */
/* GB I/O register offsets (within the 0xFF00-0xFFFF I/O space)            */
/* ------------------------------------------------------------------------ */

// RACKBOY LOCAL MOD: SameBoy defines these in `Core/memory.h`. The
// numeric values are the canonical Game Boy hardware register offsets
// documented in Pan Docs, GBDev Wiki, and every GB emulator — this is
// public-ABI information, not reverse engineering of SameBoy. `apu.c`
// indexes `gb->io_registers[]` with these constants so the byte at
// offset `GB_IO_NR10` is the value of the NR10 register (at CPU
// address 0xFF10).
enum {
    GB_IO_NR10      = 0x10, /* Channel 1 sweep */
    GB_IO_NR11      = 0x11, /* Channel 1 length / duty */
    GB_IO_NR12      = 0x12, /* Channel 1 volume envelope */
    GB_IO_NR13      = 0x13, /* Channel 1 period low */
    GB_IO_NR14      = 0x14, /* Channel 1 period high / trigger */

    GB_IO_NR20      = 0x15, /* unused, reads as 0xFF */
    GB_IO_NR21      = 0x16, /* Channel 2 length / duty */
    GB_IO_NR22      = 0x17, /* Channel 2 volume envelope */
    GB_IO_NR23      = 0x18, /* Channel 2 period low */
    GB_IO_NR24      = 0x19, /* Channel 2 period high / trigger */

    GB_IO_NR30      = 0x1A, /* Channel 3 DAC enable */
    GB_IO_NR31      = 0x1B, /* Channel 3 length */
    GB_IO_NR32      = 0x1C, /* Channel 3 output level */
    GB_IO_NR33      = 0x1D, /* Channel 3 period low */
    GB_IO_NR34      = 0x1E, /* Channel 3 period high / trigger */

    GB_IO_NR40      = 0x1F, /* unused, reads as 0xFF */
    GB_IO_NR41      = 0x20, /* Channel 4 length */
    GB_IO_NR42      = 0x21, /* Channel 4 volume envelope */
    GB_IO_NR43      = 0x22, /* Channel 4 polynomial counter */
    GB_IO_NR44      = 0x23, /* Channel 4 trigger */

    GB_IO_NR50      = 0x24, /* Master volume / VIN pan */
    GB_IO_NR51      = 0x25, /* Sound panning */
    GB_IO_NR52      = 0x26, /* Sound on/off */

    GB_IO_WAV_START = 0x30, /* Wave pattern RAM first byte */
    GB_IO_WAV_END   = 0x3F, /* Wave pattern RAM last byte */

    GB_IO_LCDC      = 0x40, /* LCD control */
    GB_IO_STAT      = 0x41, /* LCD status */

    GB_IO_RP        = 0x56  /* Infrared port (CGB) */
};

// RACKBOY LOCAL MOD: LCDC bit 7 — "LCD enable". `apu.c` checks this
// to know whether to apply the LCD-enabled phase of the APU
// interference calculation.
#ifndef GB_LCDC_ENABLE
#define GB_LCDC_ENABLE 0x80
#endif

/* ------------------------------------------------------------------------ */
/* `GB_SKIP_DIV_EVENT_*` constants                                          */
/* ------------------------------------------------------------------------ */

// RACKBOY LOCAL MOD: upstream `apu.h` declares these inline via
// `GB_ENUM(uint8_t, { … }) skip_div_event;` inside `GB_apu_t`. Our
// local `GB_ENUM` expansion drops the enum declaration and keeps only
// the storage type, so we declare the constants at file scope here —
// before including `apu.h` below — so they are visible wherever
// `apu.c` references them.
enum {
    GB_SKIP_DIV_EVENT_INACTIVE = 0,
    GB_SKIP_DIV_EVENT_SKIPPED  = 1,
    GB_SKIP_DIV_EVENT_SKIP     = 2,
};

/* ------------------------------------------------------------------------ */
/* Minimal SGB sub-state                                                    */
/* ------------------------------------------------------------------------ */

// RACKBOY LOCAL MOD: `apu.c` reads `gb->sgb->intro_animation` inside a
// guarded branch (`if (gb->sgb && gb->sgb->intro_animation < …)`).
// Upstream `GB_sgb_t` is hundreds of fields of SGB-specific state — we
// reproduce just the one field apu.c touches and leave the rest
// omitted. Rackboy v1 runs DMG-only, so `gb->sgb` is always NULL and
// the branch is dead code; the struct body exists only so the
// compiler has a complete type for the dereference.
typedef struct GB_sgb_s {
    int16_t intro_animation;
} GB_sgb_t;

// RACKBOY LOCAL MOD: SameBoy's `Core/sgb.h` defines this as `200` —
// the SGB intro frame count. The exact value is only load-bearing when
// the SGB intro is actually playing, which rackboy v1 never does (see
// `GB_sgb_t` comment above). We match the upstream value so the shim
// is a faithful stand-in. Verified at upstream commit `208ba4a`.
#ifndef GB_SGB_INTRO_ANIMATION_LENGTH
#define GB_SGB_INTRO_ANIMATION_LENGTH 200
#endif

/* ------------------------------------------------------------------------ */
/* Expose SameBoy Core-internal declarations                                */
/* ------------------------------------------------------------------------ */

// RACKBOY LOCAL MOD: `apu.c` references `GB_BAND_LIMITED_ONE`, the
// internal helper function prototypes `GB_apu_init`, `GB_apu_run`,
// and the `GB_sample_t` union form. All of those live behind
// `#ifdef GB_INTERNAL` in `vendor/sameboy/apu.h`. The rackboy_apu
// translation unit compiles `apu.c` with GB_INTERNAL defined; do the
// same here before pulling in `apu.h` so the types it declares match
// what `apu.c` will see.
#ifndef GB_INTERNAL
#define GB_INTERNAL 1
#endif

#include "vendor/sameboy/apu.h"

/* ------------------------------------------------------------------------ */
/* `GB_gameboy_t` — the rackboy stand-in                                    */
/* ------------------------------------------------------------------------ */

// RACKBOY LOCAL MOD: minimal `GB_gameboy_t` stand-in. The fields below
// are the complete set `vendor/sameboy/apu.c` reaches through `gb->…`.
//
// The beads `rackboy-amu` description authorized six fields (`apu`,
// `apu_output`, `div_counter`, `cgb_double_speed`, `io_registers`,
// `model`). An audit of `apu.c` during this iteration found six
// additional top-level fields (`halted`, `stopped`, `during_div_write`,
// `sgb`, `address_bus`, `pc`) that `apu.c` unconditionally
// dereferences — omitting them leaves `apu.c` uncompilable and defeats
// the reason the shim exists at all, so the audited fields are
// included here and the gap between the prompt's field list and the
// real dependency surface is documented in the commit body. The
// rationale for each extra field is one line, inline below.
// Note: `GB_gameboy_t` is already typedef'd as `struct GB_gameboy_s`
// in `defs.h` (so `apu.h` can use the type name in its function
// signatures before this full struct definition is visible). Here we
// only define the struct body — re-declaring the typedef would be a
// C11-only redefinition and fire `-Wtypedef-redefinition` under
// `-std=c99`.
struct GB_gameboy_s {
    /* Authorized by rackboy-amu description */
    GB_apu_t        apu;
    GB_apu_output_t apu_output;
    uint16_t        div_counter;
    bool            cgb_double_speed;
    uint8_t         io_registers[0x100];
    GB_model_t      model;

    /* Extra fields found during apu.c audit (see file header) */

    // Interference calculation: apu.c adds a halt-state contribution
    // to the DAC interference term (`apu.c:~281`).
    bool            halted;

    // Wave-channel bugged-read guard: apu.c treats a `STOP` state as
    // blocking wave-RAM reads in DMG mode (`apu.c:~890`).
    bool            stopped;

    // DIV-write glitch tracker: apu.c uses this to detect the
    // single-cycle window where a DIV write races the APU sweep
    // calculator (`apu.c:~636`).
    bool            during_div_write;

    // SGB boot-intro guard: apu.c bails out of sample generation while
    // the SGB intro is playing (`apu.c:~359`). Rackboy v1 leaves this
    // NULL so the branch is always dead code.
    GB_sgb_t       *sgb;

    // CPU address bus latch: apu.c reads this to reproduce the
    // wave-RAM glitch where an in-progress read leaks the last
    // address-bus byte (`apu.c:~880`).
    uint16_t        address_bus;

    // Program counter: apu.c substitutes `gb->pc` for the address-bus
    // value in one specific DMG edge case (`apu.c:~1946`).
    uint16_t        pc;
};

_Static_assert(GB_IO_RP < 0x100,
    "GB_IO_RP must fit within io_registers[0x100]");

/* ------------------------------------------------------------------------ */
/* Function stubs                                                           */
/* ------------------------------------------------------------------------ */

// RACKBOY LOCAL MOD: SameBoy inspects `gb->model` plus
// `gb->cgb_double_speed` to return one of several clock rates (DMG
// 4.194304 MHz, CGB normal 4.194304 MHz, CGB double speed 8.388608
// MHz, SGB 4.295454 MHz…). Rackboy v1 always runs in DMG mode, so
// we hardcode the DMG rate (2^22 Hz). rackboy-w7q swaps this for a
// model-aware implementation once the rackboy APU driver actually
// consumes it.
uint32_t GB_get_clock_rate(GB_gameboy_t *gb);

// RACKBOY LOCAL MOD: SameBoy's `GB_is_cgb` returns true for any
// `GB_MODEL_CGB_*`. Rackboy v1 is DMG-only, so we return `false`
// unconditionally; rackboy-w7q swaps this for a `model >=
// GB_MODEL_CGB_0 && model <= GB_MODEL_CGB_E` check when CGB support
// actually matters.
bool GB_is_cgb(const GB_gameboy_t *gb);

#ifdef __cplusplus
}
#endif

#endif /* RACKBOY_APU_SHIM_H */
