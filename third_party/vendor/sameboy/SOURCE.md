# Vendored SameBoy Core Sources

## Upstream

- **Project:** [SameBoy](https://github.com/LIJI32/SameBoy) by Lior Halphon
- **Clone URL:** `https://github.com/LIJI32/SameBoy.git`
- **Upstream commit:** `208ba4afabffab9edde416f2dbb8ae459e34adb8`
- **Upstream tag/version:** none (untagged commit on `master`)
- **Upstream path:** `Core/apu.c`, `Core/apu.h`
- **License:** Expat (MIT) — see `LICENSE` in this directory
- **Imported on:** 2026-04-13
- **Content hash:** `23120b65283dc5dd791e5ffa8bb09c06ca822b77953b61a412701750c9ebb996`
  (sha256 over the sorted-manifest of `apu.c`, `apu.h`, `LICENSE` — see
  `tools/check_vendor_drift.sh` `hash_vendor_tree()` for the exact recipe)

## Vendored files

| File        | Upstream path    |
|-------------|------------------|
| `apu.c`     | `Core/apu.c`     |
| `apu.h`     | `Core/apu.h`     |
| `LICENSE`   | `LICENSE` (root) |

## Modifications

**None inside this directory.** The files in `rackboy_apu/vendor/sameboy/`
are a clean, byte-identical copy of the upstream commit above, and
`tools/check_vendor_drift.sh` enforces that invariant (local-clone mode
diffs against an upstream checkout; `--hash-only` mode re-hashes the vendor
tree against the `Content hash` field above).

Local modifications, if any are ever needed, live **outside** this
directory as unified diff files under
[`rackboy_apu/patches/`](../../patches/). At build time the patches are
applied to a build-side shadow copy of this tree (typically
`build/rackboy_apu/sameboy/`); the shadow tree is what the compiler reads,
and this directory stays read-only. See [`docs/DECISIONS.md` D-005][d-005]
for the authoritative workflow decision and
[`../../patches/README.md`](../../patches/README.md) for the operational
reference (file naming, patch format, apply and regenerate procedures,
patch index).

**Status today (2026-04-14):** no patches have landed. SPIKE-1 was
originally scoped as the consumer of the patch-overlay workflow — the
per-channel APU tap was expected to land as
`rackboy_apu/patches/001-per-channel-tap.patch`. That patch did
**not** land because upstream SameBoy at the vendored commit already
exposes `GB_set_channel_muted(gb, channel, muted)` as a public API
at `Core/apu.h:220`, which is sufficient to satisfy SPIKE-1's
bit-exact-zero acceptance when the scene is rendered four times with
three channels muted per run. See [`docs/DECISIONS.md` D-008][d-008]
for the decision record and
[`docs/spikes/tap-feasibility.md`](../../../docs/spikes/tap-feasibility.md)
for the execution report. D-005's overlay workflow remains valid as
infrastructure-in-waiting; the `rackboy_apu/patches/` directory stays
empty until a future spike identifies an upstream gap that cannot be
worked around via upstream APIs.

[d-005]: ../../../docs/DECISIONS.md#d-005--vendored-sameboy-local-patches-live-under-rackboy_apupatches
[d-008]: ../../../docs/DECISIONS.md#d-008--spike-1-satisfied-via-sameboy-upstream-gb_set_channel_muted

## Dependencies not vendored

`apu.c` `#include`s `"gb.h"` (and transitively other SameBoy `Core/` headers).
These are **intentionally not vendored** — the rackboy shim will provide a
minimal `gb.h` / stubbed `GB_gameboy_t` stand-in in a later iteration
(tracked by Beads task `rackboy-amu`). The shim and any build wiring must
live **outside** `rackboy_apu/vendor/sameboy/` so this directory remains a
pristine copy of upstream.

## Updating

To refresh this vendor drop:

1. Check out the desired SameBoy commit locally
   (`git clone https://github.com/LIJI32/SameBoy.git` or `git fetch` if
   already cloned).
2. Copy `Core/apu.c`, `Core/apu.h`, and top-level `LICENSE` into this directory.
3. Update the **Upstream commit** and **Imported on** fields above.
4. Verify byte-identity against the upstream checkout with `diff -q`:

   ```
   diff -q rackboy_apu/vendor/sameboy/apu.c   <SAMEBOY>/Core/apu.c
   diff -q rackboy_apu/vendor/sameboy/apu.h   <SAMEBOY>/Core/apu.h
   diff -q rackboy_apu/vendor/sameboy/LICENSE <SAMEBOY>/LICENSE
   ```

   All three must report no differences. Any drift means an auto-formatter
   or editor touched the vendor tree — back it out before committing.
5. Do **not** record intentional deviations by editing files in this
   directory. Per D-005 local modifications live as unified diffs under
   `rackboy_apu/patches/` and are applied to a build-side shadow tree. If a
   vendor refresh causes an existing patch to fail to apply, regenerate the
   patch following the procedure in `../../patches/README.md` — this
   directory itself must remain a pristine copy of upstream at every
   commit.
