/*
 * rackboy_apu/gb.h
 *
 * Rackboy-local stand-in for SameBoy's `Core/gb.h`. The vendored
 * `Core/apu.c` `#include`s `"gb.h"` unconditionally, and only that
 * header is expected to provide the full `GB_gameboy_t` definition
 * plus every Core-level macro, enum, and helper-function prototype the
 * APU reaches through. Upstream `Core/gb.h` is ~1500 lines of cross-
 * system glue (CPU, memory, input, save state, debugger, SGB, …) —
 * this shim provides just the subset `apu.c` actually touches.
 *
 * The `#include "rackboy_apu_shim.h"` below is the single indirection
 * point: every rackboy-owned declaration lives in the shim header so
 * future build-integration work can re-point this wrapper without
 * rewriting the shim itself. Beads-tracked productionization of
 * per-channel tap patches (rackboy-8d4) and the APU driver
 * (rackboy-w7q) layer on top of this file.
 *
 * Every rackboy-local deviation from pristine SameBoy is marked with a
 * `// RACKBOY LOCAL MOD` comment inside `rackboy_apu_shim.h`.
 */

#ifndef RACKBOY_APU_GB_H
#define RACKBOY_APU_GB_H

// RACKBOY LOCAL MOD: the entire rackboy-local gb.h surface lives in
// `rackboy_apu_shim.h` so the file reservation from beads rackboy-amu
// (`rackboy_apu/rackboy_apu_shim.{h,c}`) remains the authoritative
// home for every rackboy-owned declaration. This wrapper exists only
// so `apu.c`'s unconditional `#include "gb.h"` resolves via the
// Makefile's `-Irackboy_apu` search path.
#include "rackboy_apu_shim.h"

#endif /* RACKBOY_APU_GB_H */
