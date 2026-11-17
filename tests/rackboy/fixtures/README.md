# Rackboy Test Fixtures

This directory holds the binary fixtures rackboy's `.sav` parser,
table interpreter, golden-audio regression, and round-trip spike
consume at test time. Every file here is treated as **binding
source-of-truth** by some downstream test or bead — never delete
or rewrite without a commit that also updates every consumer.

## Files

| File                  | Size     | Purpose                                           | Owner bead      |
|-----------------------|----------|---------------------------------------------------|-----------------|
| `rackboy_v1.sav`      | 131072 B | Populated `.sav` fixture (4 instruments, 1 slot)  | `rackboy-pj3`   |
| `expected.json`       | ~4 KB    | Expected parse of `rackboy_v1.sav`                | `rackboy-pj3`   |
| `README.md`           | —        | This file.                                        | —               |

## `rackboy_v1.sav` — populated fixture

This is a 128 KB LSDJ `.sav` authored by the
[`rackboy-fixture-gen`][fixture-gen] binary against the empty-template
base shipped by `gb-lsdj-instrument-extractor` at commit
[`2f4c454`][d-002]. The file contains a single project (slot 0, name
`RACKBOY`) with four pre-allocated instruments:

| Slot | Type byte | Type   | Authoring intent            |
|------|-----------|--------|-----------------------------|
| 0    | `0x00`    | Pulse  | Pulse1 (duty 10, env 0xF0)  |
| 1    | `0x00`    | Pulse  | Pulse2 (duty 01, env 0xE0)  |
| 2    | `0x01`    | Wave   | Wave (volume 100%)          |
| 3    | `0x03`    | Noise  | Noise (shape 0x55)          |

Every in-scope LSDJ instrument type byte from `docs/COMPATIBILITY.md`
is exercised (Pulse `0x00`, Wave `0x01`, Noise `0x03`). Kit
instruments (`0x02`) are out of scope per `docs/DECISIONS.md` D-003
and are not exercised by this fixture.

The fixture's block allocation matches the empty-template stub that
preceded it: 11 owned blocks (0–10) for slot 0, 180 unused blocks.
`rackboy-fixture-gen` uses `lsdj-sav`'s `Project::write_instrument` +
`Sav::store_project` pipeline, which recompresses the 32 KiB
decompressed song back into the existing block allocation — it does
not grow the allocation.

[fixture-gen]: https://github.com/mattsp1290/gb-lsdj-instrument-extractor/tree/main/crates/rackboy-fixture-gen
[d-002]: ../../docs/DECISIONS.md#d-002--pin-gb-lsdj-instrument-extractor-to-commit-2f4c454

### What this fixture exercises

- Container size + format verification (128 KB file, `'j'`/`'k'` magic
  bytes at `0x813E`, active-slot byte at `0x8140 == 0`)
- Header name-table parsing for slot 0 (`"RACKBOY\0"`)
- Header version-byte happy path (byte at `0x8100` is `0x1B`, matching
  the decompressed-song byte at `0x7FFF == 0x1B`)
- Block-allocation-map walk (11 owned blocks, 180 unused, contiguous)
- Full compressed block-chain decompression, including:
  - `0xC0` RLE opcode runs
  - `0xE0 0xFF` terminator
  - `0xE0 0xF0 <count>` default-wave expansion
- Decompressed-song format-version byte check at `0x7FFF == 0x1B`
- Instrument alloc map at `[0x2040, 0x2080)` — bytes 0..3 are `0x01`
  (allocated), bytes 4..64 are `0x00` (unallocated)
- Table alloc map at `[0x2020, 0x2040)` — all zeros (no tables)
- Per-instrument type classification for all four allocated slots
  via the type byte at offset 0 of each 16-byte record at
  `0x3080 + slot * 16`

### What this fixture does NOT yet exercise

- **Song-chain channel assignment.** The fixture has no rows in the
  song/chain/phrase layer; `rackboy-fixture-gen` writes instruments
  directly into the pre-allocated blocks without touching the pattern
  area. Rackboy parsers that derive Pulse1-vs-Pulse2 from which
  channel plays a note referencing the instrument will have no song
  evidence to work from; they must fall back to the
  `pulse_split_convention_note` in `expected.json` (slot 0 =
  Pulse1 use, slot 1 = Pulse2 use) or report both as generic Pulse.
- **LSDJ table commands** (`rackboy_table/` interpreter's
  domain). No tables are allocated.
- **Kit instruments.** Out of scope per D-003.
- **CH3 wave-frame lookup.** The Wave instrument references
  `wave_index 0`, but the decompressed song only holds the
  default-wave expansion produced by the `0xE0 0xF0` opcode during
  decompression. A test that needs a hand-crafted waveform will
  need to extend this fixture (or a sibling fixture) to write a
  non-default wave frame at slot 0.
- **Populated sidecar (`.json`) name override.** This fixture has
  no sidecar.

### Why this fixture-authoring path?

The original rackboy-n61 stub fixture was a byte-identical copy of
the extractor's `empty_template.sav`. `rackboy-pj3` needs a
populated bank to unblock SPIKE-4's per-instrument classification
layer and the golden-audio regression. The plan documented in
[`../../docs/DECISIONS.md` D-006][d-006] evaluated two paths:

1. **Extractor CLI against a real LSDJ ROM.** Running
   `gb-lsdj extract → plan-export → commit-export` against the
   LSDJ 9.4.0 stable ROM produces only 2 Pulse candidates (0 Wave,
   0 Noise). A fresh LSDJ boot emits ~13 APU writes during its
   boot tone and nothing else; without a pre-authored save loaded
   into the ROM, the capture window is empty. This path did not
   produce a fixture matching rackboy's acceptance criteria.
2. **`rackboy-fixture-gen` (the path taken).** A small Rust binary
   in the extractor workspace that uses `lsdj-sav`'s
   `Project::write_instrument` API directly to author the four
   instruments. Deterministic, reproducible from source, does not
   depend on an LSDJ song playing during capture, and lands as a
   committed artifact in the extractor repo.

[d-006]: ../../docs/DECISIONS.md#d-006--populated-rackboy_v1sav-authored-via-rackboy-fixture-gen

### Reproducing the fixture

```sh
cd /path/to/gb-lsdj-instrument-extractor
cargo run --release -p rackboy-fixture-gen -- /tmp/rackboy_v1.sav
shasum -a 256 /tmp/rackboy_v1.sav  # must equal fixture_sha256 in expected.json
cp /tmp/rackboy_v1.sav /path/to/rackboy/tests/fixtures/rackboy_v1.sav
```

The binary prints a verification summary to stderr as it reloads
the `.sav` it just wrote and confirms the 4-instrument mix.

## `expected.json` — parser acceptance reference

This file is the expected parse output for `rackboy_v1.sav`. The
SPIKE-4 round-trip check (Beads `rackboy-43y`) and the
`rackboy_sav/` doctest suite (`rackboy-byp`) both read this file and
assert their parse results against it.

Per `docs/DECISIONS.md` D-007, the acceptance criterion "byte-for-byte
identical to `expected.json`" is read **behaviorally**: the spike
parser asserts every fact in `expected.json` via typed doctest cases
rather than JSON-serializing its parse output and diffing. The
behavioral interpretation gives precise failure modes and avoids
locking the parser into a specific JSON-emit code path.

Schema: loosely-structured JSON with explicit named fields. Not
a formal schema today — when we grow a second fixture, extract the
shared fields into a `tests/fixtures/schema.json` and validate both
fixtures against it.

Top-level fields:

- `fixture_name`, `fixture_path`, `fixture_sha256`,
  `fixture_size_bytes` — identity + integrity
- `source` — how the fixture was authored (tool, commit, procedure)
- `scope_note` — what this fixture does and doesn't exercise
- `container` — top-level `.sav` file properties (magic, active
  project slot)
- `header_table` — per-slot header state
- `block_allocation` — slot-0 block chain layout
- `decompressed_song` — expected post-decompression state including
  instrument and table alloc maps
- `instruments` — per-type counts plus a `per_slot` array carrying
  each allocated slot's type byte and authoring intent
- `tables` — allocated table count (zero for this fixture)
- `parser_acceptance_checks` — explicit list of assertions
  rackboy's spike parser must exercise
- `load_time_warnings_expected` — warning strings the loader should
  emit against this fixture (empty for this fixture — no version
  byte mismatch to warn about)

Update discipline: every field in `expected.json` that the
`rackboy_sav/` parser tests against is binding. If rackboy's
behavior changes, update both the parser and this JSON in the
same commit.

## How to regenerate the hash

```sh
# macOS: ships shasum
shasum -a 256 tests/fixtures/rackboy_v1.sav

# Linux: typically ships sha256sum
sha256sum tests/fixtures/rackboy_v1.sav
```

Either command prints the sha256 of the fixture. It must equal
the `fixture_sha256` in `expected.json`. If it doesn't, the
fixture has drifted — investigate before committing. The same
portability shim exists in `tools/check_vendor_drift.sh` under
`_sha256` — reuse it if you script this check.

## Cross-references

- [`docs/COMPATIBILITY.md`](../../docs/COMPATIBILITY.md) — the
  authoritative `.sav` format spec this fixture conforms to
- [`docs/DECISIONS.md` D-002](../../docs/DECISIONS.md#d-002--pin-gb-lsdj-instrument-extractor-to-commit-2f4c454)
  — the extractor pin the fixture was authored against
- [`docs/DECISIONS.md` D-006](../../docs/DECISIONS.md#d-006--populated-rackboy_v1sav-authored-via-rackboy-fixture-gen)
  — the fixture authorship decision
- [`docs/DECISIONS.md` D-007](../../docs/DECISIONS.md#d-007--spike-4-behavioral-acceptance-and-compressionrs-port)
  — the SPIKE-4 acceptance interpretation
- [`docs/USING_EXTRACTOR.md`](../../docs/USING_EXTRACTOR.md) —
  CLI workflow for authoring a fixture from a real LSDJ ROM
  (alternative path, not used for this fixture — see D-006)
- [`tests/audio/README.md`](../audio/README.md) — consumes this
  fixture for the golden-audio regression procedure
- Beads: `rackboy-n61` (stub fixture, superseded by `rackboy-pj3`),
  `rackboy-pj3` (this fixture), `rackboy-43y` (SPIKE-4 round-trip),
  `rackboy-byp` (`rackboy_sav/` doctest suite), `rackboy-3hk` (data
  structures, downstream of spike)
