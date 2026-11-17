# Vendored doctest

## Upstream

- **Project:** [doctest](https://github.com/doctest/doctest) by Viktor Kirilov
- **Clone URL:** `https://github.com/doctest/doctest.git`
- **Upstream commit:** `ae7a13539fb71f270b87eb2e874fbac80bc8dda2`
- **Upstream tag/version:** `v2.4.11` (`DOCTEST_VERSION_STR = "2.4.11"`)
- **Upstream path:** `doctest/doctest.h`, `LICENSE.txt`
- **License:** MIT — see `LICENSE.txt` in this directory
- **Imported on:** 2026-04-13
- **Content hash:** `8d0fe66bf47cc2a73c061274fd5689b6cde7287d912bbbbc7560eabf49485099`
  (sha256 over the sorted-manifest of `doctest.h`, `LICENSE.txt` — see
  `tools/check_vendor_drift.sh` `hash_vendor_tree()` for the exact recipe)

## Vendored files

| File          | Upstream path        |
|---------------|----------------------|
| `doctest.h`   | `doctest/doctest.h`  |
| `LICENSE.txt` | `LICENSE.txt` (root) |

## Modifications

**None.** This is a clean import. doctest is a single-header library; the
rackboy test runner simply `#include`s it from
`tests/third_party/doctest/doctest.h` and defines
`DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` in exactly one translation unit per
test binary.

## Dependencies not vendored

None. doctest is strictly self-contained — it has zero external
dependencies beyond a C++11 toolchain.

## Updating

To refresh this vendor drop:

1. Pick a new release tag from <https://github.com/doctest/doctest/releases>.
2. Replace `doctest.h` and `LICENSE.txt` in this directory with the
   upstream files from that tag:

   ```
   curl -sSL https://raw.githubusercontent.com/doctest/doctest/<tag>/doctest/doctest.h   -o tests/third_party/doctest/doctest.h
   curl -sSL https://raw.githubusercontent.com/doctest/doctest/<tag>/LICENSE.txt         -o tests/third_party/doctest/LICENSE.txt
   ```

3. Update the **Upstream commit**, **Upstream tag/version**, and
   **Imported on** fields above.
4. Verify byte-identity against the upstream checkout with `diff -q`
   (parallels the SameBoy vendor refresh step — see
   `../../../rackboy_apu/vendor/sameboy/SOURCE.md`):

   ```
   # assuming a local clone at <DOCTEST>
   diff -q tests/third_party/doctest/doctest.h   <DOCTEST>/doctest/doctest.h
   diff -q tests/third_party/doctest/LICENSE.txt <DOCTEST>/LICENSE.txt
   ```

   Both must report no differences. Any drift means a transfer was
   munged — back it out before committing.
5. Verify that the `DOCTEST_VERSION_MAJOR / _MINOR / _PATCH` triple near
   the top of the new `doctest.h` matches the tag you picked — this is
   the upstream's built-in provenance marker and a second, independent
   check beyond `diff -q`.
6. Record any intentional deviations in a new `## Modifications`
   subsection — a clean import should remain the default.
