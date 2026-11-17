/*
 * rackboy_apu_shim.c
 *
 * Implementation side of the rackboy-local `GB_gameboy_t` stand-in.
 * See `rackboy_apu_shim.h` for the full role description and the
 * beads-tracked scope boundaries (rackboy-amu → rackboy-8d4 →
 * rackboy-w7q).
 *
 * Every deviation from pristine upstream SameBoy semantics is marked
 * with a `// RACKBOY LOCAL MOD` comment.
 */

#include "rackboy_apu_shim.h"

/* ------------------------------------------------------------------------ */
/* `GB_get_clock_rate` — DMG clock-rate stub                                */
/* ------------------------------------------------------------------------ */

// RACKBOY LOCAL MOD: always report the DMG clock rate (2^22 Hz,
// 4,194,304 Hz). Real SameBoy branches on `gb->model` and
// `gb->cgb_double_speed` — we pin DMG here per the v1 architecture
// decision in `docs/prompts/rackboy-lsdj-quad-voice-module.md` and
// swap in a model-aware implementation when rackboy-w7q wires up the
// real APU driver.
uint32_t GB_get_clock_rate(GB_gameboy_t *gb)
{
    (void)gb;
    return 4194304u;
}

/* ------------------------------------------------------------------------ */
/* `GB_is_cgb` — DMG-only stub                                              */
/* ------------------------------------------------------------------------ */

// RACKBOY LOCAL MOD: rackboy v1 runs DMG-only (see project prompt §
// "Architecture Decisions"). Returning `false` unconditionally
// disables every `GB_is_cgb(gb)`-guarded branch in `apu.c` — CGB
// infrared-register bias, CGB-double-speed counter plumbing, CGB-0
// edge cases — leaving the DMG code path active. rackboy-w7q replaces
// this with the standard `GB_MODEL_CGB_0 <= model && model <=
// GB_MODEL_CGB_E` check when CGB support lands.
bool GB_is_cgb(const GB_gameboy_t *gb)
{
    (void)gb;
    return false;
}
