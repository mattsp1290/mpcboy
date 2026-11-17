# CPU benchmarks

Numbers below were measured, not estimated. Anything not measured says so.

## Budget (S8, 02-apu-and-voice.md WP3)

A 128-frame block at 44.1 kHz lasts 2,902 us. One core at 5 % is 145 us.
Device pass rule (G-CPU, one rule for S8 and WP10 row 9): **mean <= 145 us and
p99 <= 290 us per block**, measured in the plugin at a 128-frame buffer.
Native pass bar on the developer Mac (about 6x headroom): **p99 <= 25 us** for
APU single pass + resampler.

## Native bench (`mpcboy_bench`)

Host: Apple M4 Max (arm64), macOS (Darwin 25.6.0), Apple clang `-O2`
(`native` preset, Release, no `NDEBUG`). Command:

```text
cmake --preset native && cmake --build --preset native && ./build/native/mpcboy_bench
```

Workload: 200,000 blocks of 128 host frames after 4,000 warm-up blocks, one
`steady_clock` pair around each block. All four channels sounding (Pulse 1,
Pulse 2, Wave, Noise at volume 15, NR50 `0x77`, NR51 `0xFF`).

- **A (the pass bar):** `rackboy_apu_run_stereo` for `inputsNeeded(128)` frames
  (136..141 APU frames) + `PolyphaseResampler48To441::process`.
- **B0 (informational, WP3):** `Engine::render` with a held note, 4 channels, 4
  tables (return-to-start arpeggios) and 24 ticks/beat at 120 BPM injected by
  hand (`EngineBlock::tickOffsets`).
- **B (informational, WP6):** the same through the engine's own `TickClock`
  at 240 BPM, 96 ticks/beat (one tick every 114.8 host frames, about 1.1 per
  128-frame block), 4 tables on 4 channels.
- "APU only" and "resampler only" split A.

All times in microseconds per 128-frame block.

### Final runs (quiet of my own load; the host itself was busy, load average 44-54)

| run | APU only p99 | A mean | A p50 | **A p99** | A p99.9 | B mean | B p99 | gate |
|---|---|---|---|---|---|---|---|---|
| 1 | 7.17 | 10.89 | 10.42 | **14.42** | 32.00 | 11.08 | 16.46 | PASS |
| 2 | 7.29 | 10.04 | 9.71 | **14.50** | 24.71 | 10.88 | 14.33 | PASS |
| 3 | 6.33 | 9.96 | 9.58 | **13.75** | 24.17 | 10.92 | 13.71 | PASS |
| 4 | 7.21 | 9.64 | 9.46 | **11.75** | 20.58 | 10.75 | 13.62 | PASS |

Split, from two earlier runs at host load average 68-80 (means): APU single
pass 4.6 us, resampler 4.6 us, A 9.4-9.5 us, i.e. about 0.33 % of one core on
this CPU. `max` values (1 to 20 ms) are scheduler
preemption of the benchmark thread on a busy desktop, not work done by the
code; the device rule has no max bar for the same reason.

### WP6 re-run: TickClock in the render path (B = 4 tables, 240 BPM, 96/beat)

Same machine and command, after WP5/WP6 (clock, volume column, `P`, delayed
`K`, `W` retrigger, content store). Host load average 41-46 during these runs
(a busy desktop), so only the tails moved. Four consecutive runs, microseconds
per 128-frame block:

| run | A mean | A p99 | B0 mean | B0 p99 | **B mean** | **B p50** | **B p99** | B p99.9 | gate |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 11.01 | 13.04 | 12.38 | 26.42 | 16.15 | 12.25 | 28.79 | 667.42 | PASS |
| 2 | 17.29 | 24.46 | 12.26 | 16.62 | 14.45 | 11.46 | 25.25 | 95.50 | PASS |
| 3 | 11.67 | 24.83 | 17.65 | 24.71 | 12.12 | 11.38 | 15.38 | 32.25 | PASS |
| 4 | 9.74 | 12.12 | 10.42 | 14.29 | 11.35 | 10.83 | 17.25 | 27.29 | PASS |

B stays within about 1 us (p50) of B0 and A: ticking 4 tables at 96/beat and
240 BPM (about 8.7x the tick rate of B0) adds roughly 0.5-1 us per block, and
the clock bookkeeping itself is a few doubles per block. The mean of B in the
quietest run (run 4, 11.35 us) is under 8 % of the 145 us device budget on this
Mac. Runs 1-3 had p99 values up to 29 us that come from the contended host (the
same A workload shows 24 us p99 in run 2); the device is still unmeasured.

### Runs that did NOT meet the bar (reported as measured)

Three consecutive runs made while a competing AddressSanitizer process of mine
was spinning on the same machine and the host load average was 57-65:

| run | A mean | A p50 | **A p99** | gate |
|---|---|---|---|---|
| 1 | 12.02 | 10.42 | **26.54** | FAIL |
| 2 | 14.10 | 10.79 | **25.50** | FAIL |
| 3 | 16.96 | 10.62 | **25.54** | FAIL |

p50 is unchanged (about 10.5 us); only the tail moved, which is CPU contention,
not the workload. An even earlier pair (host load average 100-150) showed A p99
45.7 us and 26.9 us. The first version of the engine workload B also showed a
p50 of 145 us: that was a bug in the bench's table (it climbed +4 semitones per
loop until the pulse hit register value 2047, a 131 kHz oscillator that is
expensive to emulate), fixed by using arpeggios that return to their start. It
was not a code regression.

**Verdict:** the native pass bar (A p99 <= 25 us) is met when the benchmark is
not competing for CPU (11.75 to 14.50 us), and is marginal-to-failing under
heavy host contention (25.5 to 26.5 us at load average ~60). Re-run on a quiet
machine before relying on the exact figure. Headroom against the 145 us device
budget on this Mac is about 10x; the device itself is unmeasured (below).

## G-CPU device gate (needs the user's MPC Live III)

**Status: pending. No device result is claimed.** Mechanism (WP3 test 7):

1. Build the debug CPU spike plugin and package it:

   ```text
   cmake --preset arm64 -DMPCBOY_CPU_PROBE=ON -B build/arm64-probe && cmake --build build/arm64-probe
   cmake --preset arm32 -DMPCBOY_CPU_PROBE=ON -B build/arm32-probe && cmake --build build/arm32-probe
   cmake --preset native -DMPCBOY_CPU_PROBE=ON -B build/native-probe && cmake --build build/native-probe --target mpcboy_plugin
   MPCBOY_BUILD_SUFFIX=-probe scripts/make_min_folder.sh      # -> build/minfolder-probe/, SDK validator runs on it
   ```

   (The arm64 and arm32 probe builds were produced and the folder passed the
   SDK validator with 0 errors, 0 warnings. The native probe also runs under
   `host_sim ... test` with no failures.)
2. Copy `build/minfolder-probe/mpcboy - VST - mpcboy` into `<drive>/VST2 Plugins/`
   on the MPC, insert "mpcboy" on a track, set the buffer to 128 frames.
3. Since WP7 the probe is the real plugin with two additions: it selects the
   `Stack4` program (4 channels, 4 tables, 24 ticks/beat, project tempo) and
   holds a note by itself, and the parameter `dbg_us` is appended last. Every `process()` call is timed with
   `clock_gettime(CLOCK_MONOTONIC)` into a 32,768-entry ring; a worker thread
   computes mean and p99 over the last <= 20,000 blocks (about 58 s) once per
   second, stores them for `formatValue`, and calls `requestDisplayUpdate()`
   from the worker (never from `process()`). The read-only parameter `CPU us`
   (`dbg_us`) displays `m<mean> p99 <p99>` in microseconds.
4. Hold for 60 s. Pass: mean <= 145 us and p99 <= 290 us.
5. Record the readings here and in `docs/DEVICE_TESTS.md`. If it fails,
   profile and fix before WP5 (candidates in order: SameBoy per-sample feeding
   in `feed_samples`, then resampler taps 257 -> 129 with polyphase test 5
   re-run). If it cannot be run now it stays "pending" and is a release blocker
   at WP10 row 9.

| Device | Block | mean (us) | p99 (us) | Result |
|---|---|---|---|---|
| MPC Live III | 128 | - | - | pending (not run) |
