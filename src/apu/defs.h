/*
 * rackboy_apu/defs.h
 *
 * Rackboy-local stand-in for SameBoy's `Core/defs.h`. We only vendor
 * `Core/apu.{c,h}` from upstream SameBoy (see
 * `vendor/sameboy/SOURCE.md`), so every other Core header that `apu.h`
 * or `apu.c` transitively needs has to be re-provided outside the
 * pristine vendor tree. `apu.h` `#include`s `"defs.h"` directly; this
 * file is the minimal rackboy-local replacement the vendored `apu.h`
 * finds via the Makefile's `-Irackboy_apu` include path.
 *
 * Every rackboy-local deviation from SameBoy's real `defs.h` interface
 * is marked with a `// RACKBOY LOCAL MOD` comment.
 *
 * See also: `rackboy_apu/rackboy_apu_shim.h`, `rackboy_apu/gb.h`,
 * beads rackboy-amu, `docs/DECISIONS.md` D-005 / D-008.
 */

#ifndef RACKBOY_APU_DEFS_H
#define RACKBOY_APU_DEFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Forward declaration of the Game Boy state container                      */
/* ------------------------------------------------------------------------ */

// RACKBOY LOCAL MOD: SameBoy's upstream `defs.h` forward-declares
// `GB_gameboy_t` as `typedef struct GB_gameboy_s GB_gameboy_t;` so
// `apu.h` function signatures can name it before the full struct
// definition is available (that arrives via `gb.h`). The rackboy shim
// uses the same approach so the struct definition can live in
// `rackboy_apu/rackboy_apu_shim.h` without re-including `apu.h`
// cyclically.
typedef struct GB_gameboy_s GB_gameboy_t;

/* ------------------------------------------------------------------------ */
/* `GB_ENUM` — SameBoy's "declare an enum but use a narrow storage type"    */
/* ------------------------------------------------------------------------ */

// RACKBOY LOCAL MOD: SameBoy's upstream `GB_ENUM(type, { values })`
// macro expands in two different ways depending on whether the TU is a
// Core-internal C build (where it declares a real enum) or a frontend
// C++ build (where it uses the narrow integer storage type).
//
// Inside the rackboy shim we keep only the narrow-storage form, and
// declare any inline enum constants SameBoy places inside struct
// bodies as ordinary file-scope named enums BEFORE `apu.h` is pulled
// in (see `rackboy_apu_shim.h`). The advantage is that the macro
// expansion inside a struct body is pure C99 (`uint8_t field;`), no
// GNU-style bare enum declaration, so `-std=c99 -Wpedantic` stays
// clean. The disadvantage is that we hand-maintain the
// `GB_SKIP_DIV_EVENT_*` (and any future `GB_ENUM` constants) in the
// shim header — `tools/check_vendor_drift.sh` protects against
// silent upstream drift by hashing the vendor tree.
#define GB_ENUM(type, ...) type

/* ------------------------------------------------------------------------ */
/* `internal` — SameBoy's internal-linkage visibility marker                */
/* ------------------------------------------------------------------------ */

// RACKBOY LOCAL MOD: upstream SameBoy defines `internal` as
// `__attribute__((visibility("internal")))` so Core-internal symbols
// don't leak into libretro / SDL frontends. Inside the rackboy
// scaffold we build `apu.c` as a single statically-linked translation
// unit, so we don't need hidden visibility — leave `internal` as a
// no-op qualifier that is syntactically valid anywhere SameBoy uses it
// (`internal void foo(…);`, `internal uint8_t bar(…);`).
#ifndef internal
#define internal /* no-op — see rackboy_apu/defs.h */
#endif

#ifdef __cplusplus
}
#endif

#endif /* RACKBOY_APU_DEFS_H */
