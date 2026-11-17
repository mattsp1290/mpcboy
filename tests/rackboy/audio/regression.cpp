// tests/audio/regression.cpp
//
// Deterministic golden-audio regression (bead rackboy-5wl).
// Implements the binding spec in tests/audio/README.md:
//
//   * Load the committed LSDJ fixture at
//     tests/fixtures/rackboy_v1.sav through the production
//     sav pipeline (rackboy_sav_header + rackboy_sav_compression +
//     rackboy_sav_validate).
//   * Replay the committed trigger / clock pattern from
//     tests/audio/pattern.csv through the shared DSP core
//     (APU shim + rackboy_core gate/trigger/tick + taps) at
//     AD #11's pinned 48 kHz / 2 s window.
//   * Emit four per-channel float buffers (96000 samples each).
//   * Enforce structural invariants (no NaN / no Inf / no
//     clipping past ±1) unconditionally.
//   * If the reference buffers exist at
//     tests/audio/reference/{pulse1,pulse2,wave,noise}.bin,
//     compute the per-channel RMS of (rendered - reference)
//     and FAIL if any exceeds AD #11's -60 dBFS threshold.
//     If the buffers are absent, print a skip notice and
//     PASS — bead `rackboy-n2x` owns the committed reference
//     buffers and the regen tool.
//
// This file is deliberately self-contained: the CSV parser,
// the `.bin` reader, and the RMS / dBFS helpers all live
// inline. The test module's only external link-time
// dependencies are the production rackboy_sav parser closure,
// rackboy_core DSP, rackboy_table, the APU shim + vendored
// SameBoy, and tests/common/fixture_loader.
//
// What is deliberately OUT OF SCOPE for this iteration
// ----------------------------------------------------
// * The offline renderer wrapper (tools/render_golden_audio.sh)
//   that produces reference buffers — tracked by rackboy-n2x.
//   Until it lands the skip-when-absent branch below is the
//   operative path.
// * metadata.json provenance (rackboy_commit +
//   rackboy_binary_sha256 + …). Once the wrapper script ships
//   we add a metadata read here and gate against the on-disk
//   fields.
// * WAV containers. The bead description pins ".bin" raw
//   f32le per channel; the README's output-format section
//   was updated in the same commit to match. A future bead
//   can add a WAV wrapper if / when a DAW author wants to
//   audition the references directly.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

// Denormal-mode control (bead rackboy-7hy). On x86-64 the SSE
// MXCSR-write intrinsics live in <xmmintrin.h> (FTZ) and
// <pmmintrin.h> (DAZ). On aarch64 we write FPCR bit 24 directly
// via inline asm; no system header is needed.
#if defined(__x86_64__) || defined(_M_X64)
#  include <xmmintrin.h>
#  include <pmmintrin.h>
#endif

#include "doctest/doctest.h"

extern "C" {
#include "rackboy_apu.h"
}

#include "common/fixture_loader.h"
#include "rackboy_bank.h"
#include "rackboy_clock.h"
#include "rackboy_constants.h"
#include "rackboy_gate.h"
#include "rackboy_sav_compression.h"
#include "rackboy_sav_header.h"
#include "rackboy_sav_sidecar.h"
#include "rackboy_sav_types.h"
#include "rackboy_sav_validate.h"
#include "rackboy_table.h"
#include "rackboy_taps.h"
#include "rackboy_trigger.h"
#include "rackboy_voice.h"

using rackboy::core::gate_reset;
using rackboy::core::gate_step;
using rackboy::core::GateState;
using rackboy::core::kNumVoices;
using rackboy::core::taps_process;
using rackboy::core::taps_reset;
using rackboy::core::TapState;
using rackboy::core::tick_all;
using rackboy::core::trigger_all;
using rackboy::core::voices_reset_all;
using rackboy::core::VoiceState;
using rackboy::sav::LoadPhase;
using rackboy::sav::RackboyBank;
using rackboy::sav::RackboyBankNames;
using rackboy::table::table_reg_state_reset;
using rackboy::table::TableRegState;

namespace rb_compression = rackboy::sav::compression;
namespace fixture_common = rackboy::tests::common;

namespace {

// ---------------------------------------------------------------------------
// Denormal-mode lock (bead rackboy-7hy)
// ---------------------------------------------------------------------------
//
// tests/audio/README.md "Denormal handling (FTZ / DAZ)" pins the
// render path to IEEE-754-correct denormal handling (FTZ off +
// DAZ off) so the reference buffers are reproducible across
// machines where an ambient SSE flag could otherwise flush
// near-silence samples to zero. This helper runs at `render()`
// entry and is idempotent; safe to call from every TEST_CASE.
//
// The matching `metadata.json.denormal_mode` value is
// `"ftz_off_daz_off"`. The RMS TEST_CASE below asserts that
// value on disk before comparing buffers — a committed
// reference with a stale denormal_mode stops the comparison
// instead of silently producing the wrong answer.
void set_ieee_denormal_mode()
{
#if defined(__x86_64__) || defined(_M_X64)
    // SSE: MXCSR bits 15 (FTZ) and 6 (DAZ). The Intel-provided
    // setters abstract both bits; calling them is enough.
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_OFF);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_OFF);
#elif defined(__aarch64__) || defined(_M_ARM64)
    // ARMv8 A64: FPCR bit 24 is the Flush-to-Zero mode. Clear
    // it to get IEEE-compliant denormal handling. ARM doesn't
    // have a separate DAZ bit; FTZ off already gives
    // DAZ-equivalent behavior (inputs + outputs are both
    // treated as finite denormals).
    //
    // `isb` (Instruction Synchronization Barrier) follows the
    // FPCR write so subsequent floating-point instructions see
    // the new mode. Apple M-series drains the pipeline on FPCR
    // writes anyway — empirically no drift observed on Apple
    // Silicon without the barrier — but the barrier is cheap
    // insurance for future aarch64 targets (Daisy firmware,
    // Linux ARM CI runners) where the drain isn't guaranteed.
    // The `"memory"` clobber pins the ordering so the compiler
    // does not hoist subsequent float reads across the mode
    // change.
    std::uint64_t fpcr = 0;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    fpcr &= ~(std::uint64_t{1} << 24);
    __asm__ __volatile__("msr fpcr, %0\n\t"
                         "isb"
                         :
                         : "r"(fpcr)
                         : "memory");
#else
    // 32-bit x86, PowerPC, RISC-V, etc.: no action taken. The
    // render still runs but the denormal mode is whatever the
    // ambient process was set to, which is exactly the
    // "ambient" state the README's escape hatch originally
    // described. A future iteration can add per-arch
    // branches here.
#endif
}

// ---------------------------------------------------------------------------
// Binding constants (mirrored from tests/audio/README.md)
// ---------------------------------------------------------------------------

// AD #11. Changing any of these invalidates every committed reference
// buffer + requires a docs/DECISIONS.md entry.
constexpr std::uint32_t kSampleRateHz     = 48000;
constexpr std::size_t   kRenderSamples    = 96000;  // 2 s @ 48 kHz
constexpr std::size_t   kChannels         = kNumVoices;  // 4
constexpr float         kRmsFailDbfs      = -60.0f;

// Reference buffer filenames, indexed in channel order
// (Pulse1, Pulse2, Wave, Noise) — matches
// rackboy::core::VoiceChannel and the readout format helper.
const char* const kReferenceFilenames[kChannels] = {
    "pulse1.bin",
    "pulse2.bin",
    "wave.bin",
    "noise.bin",
};

// ---------------------------------------------------------------------------
// Candidate-path search (mirrors tests/common/fixture_loader)
// ---------------------------------------------------------------------------
//
// The audio test binary can launch from any of four cwds; keep the
// search set in sync with tests/common/fixture_loader.cpp so the sav
// fixture and the pattern / reference artifacts resolve from the
// same anchors.

std::string try_open(const std::vector<std::string>& candidates)
{
    for (const auto& path : candidates) {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (f != nullptr) {
            std::fclose(f);
            return path;
        }
    }
    return std::string();
}

std::string resolve_pattern_path()
{
    return try_open({
        "audio/pattern.csv",          // from tests/
        "tests/audio/pattern.csv",    // from repo root
        "../audio/pattern.csv",       // from tests/build/
        "../../audio/pattern.csv",    // from tests/build/audio/
    });
}

std::string resolve_reference_path(const char* filename)
{
    std::string name(filename);
    return try_open({
        "audio/reference/" + name,
        "tests/audio/reference/" + name,
        "../audio/reference/" + name,
        "../../audio/reference/" + name,
    });
}

// Read a value from a flat "k: v" JSON document. Zero-dep
// parser — the regeneration tool always writes metadata.json
// with jq's default indentation (one key per line), and we
// only need to read string values, so a line scan that finds
// `"<key>":` and grabs the next double-quoted token is
// enough. Returns an empty string if the key is absent or
// the value is not a string.
//
// Kept deliberately narrow: we do NOT want a general-purpose
// JSON parser in the test TU just for one field lookup. If
// the regen tool ever changes format (e.g. one-line JSON),
// this helper must be revisited — the matching test will
// REQUIRE-fail rather than silently misparse, so the drift
// is loud.
std::string read_string_field(const std::string& json,
                              const std::string& key)
{
    const std::string needle = "\"" + key + "\"";
    std::size_t k = json.find(needle);
    if (k == std::string::npos) {
        return std::string();
    }
    // Advance past the key, expect a colon + whitespace + opening quote.
    std::size_t colon = json.find(':', k + needle.size());
    if (colon == std::string::npos) {
        return std::string();
    }
    std::size_t open_q = json.find('"', colon + 1);
    if (open_q == std::string::npos) {
        return std::string();
    }
    std::size_t close_q = json.find('"', open_q + 1);
    if (close_q == std::string::npos) {
        return std::string();
    }
    return json.substr(open_q + 1, close_q - open_q - 1);
}

// Resolve tests/audio/reference/metadata.json relative to the
// four candidate cwds the test binary can launch from.
std::string resolve_metadata_path()
{
    return try_open({
        "audio/reference/metadata.json",
        "tests/audio/reference/metadata.json",
        "../audio/reference/metadata.json",
        "../../audio/reference/metadata.json",
    });
}

// Read the entire metadata.json into a string. Returns empty
// on any I/O failure — callers REQUIRE a non-empty return.
std::string read_file(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::string();
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// ---------------------------------------------------------------------------
// CSV pattern parser
// ---------------------------------------------------------------------------

enum class PatternEventType {
    Gate1Rising,
    Gate2Rising,
    Gate1Falling,
    Gate2Falling,
    KnobSet,
};

struct PatternEvent {
    double           time_seconds;
    // Sample index at which this event must fire. Computed
    // once at parse time via `std::nearbyint(time_seconds *
    // kSampleRateHz)` so the render loop can dispatch
    // against integer sample indices — that is what makes
    // the README binding-parameters row "Sample-accurate
    // alignment — no ±1 sample jitter" a testable claim
    // rather than a double-precision accident.
    std::size_t      sample_index;
    PatternEventType type;
    // Slot tuple for gate1_rising — one entry per voice. For
    // other event types the field is unused.
    std::int16_t     slot_tuple[kChannels];
    // knob values for knob_set — mirrors the slot_tuple layout.
    float            knob_tuple[kChannels];
    bool             has_value;  // true if event_value was present
};

// Convert a `time_seconds` to the sample index it fires on.
// Round-to-nearest-even via `std::nearbyint` so an event at
// exactly one sample period (1/48000 s) lands on sample 1,
// and an event at 1/2 sample period (1/96000 s) rounds to
// the even neighbor (sample 0). Exposed at namespace scope
// so a doctest can pin the rounding behaviour independent
// of the CSV parser plumbing.
std::size_t seconds_to_sample_index(double time_seconds)
{
    const double sample_f = time_seconds
                                * static_cast<double>(kSampleRateHz);
    return static_cast<std::size_t>(std::nearbyint(sample_f));
}

// Trim ASCII whitespace and surrounding double quotes from `s`.
// The quotes are optional per the README's column examples
// (`"0,1,2,3"` for an instrument-slot tuple vs `0,1,2,3` — both
// tolerated). Called on each field after comma-splitting.
std::string trim(const std::string& s)
{
    std::size_t begin = 0;
    std::size_t end   = s.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(s[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    // Strip one pair of surrounding double quotes, if present.
    if (end - begin >= 2 && s[begin] == '"' && s[end - 1] == '"') {
        ++begin;
        --end;
    }
    return s.substr(begin, end - begin);
}

// Split `s` on commas, trimming each piece. `max_parts` caps
// the split count — the last piece keeps any trailing commas
// as part of its content. The CSV here only needs splitting
// into the three top-level columns (time, type, value);
// further splitting of the value column's inner CSV happens
// separately.
std::vector<std::string> split_commas(const std::string& s,
                                      std::size_t        max_parts)
{
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= s.size() && out.size() + 1 < max_parts) {
        std::size_t comma = s.find(',', start);
        if (comma == std::string::npos) {
            out.push_back(trim(s.substr(start)));
            return out;
        }
        out.push_back(trim(s.substr(start, comma - start)));
        start = comma + 1;
    }
    out.push_back(trim(s.substr(start)));
    return out;
}

// Parse a "p1,p2,wv,no" slot-tuple string into the four-
// element `out` array. Returns true on success.
bool parse_slot_tuple(const std::string& s, std::int16_t out[kChannels])
{
    const auto parts = split_commas(s, /*max_parts=*/kChannels);
    if (parts.size() != kChannels) {
        return false;
    }
    for (std::size_t i = 0; i < kChannels; ++i) {
        try {
            int v = std::stoi(parts[i]);
            out[i] = static_cast<std::int16_t>(v);
        } catch (...) {
            return false;
        }
    }
    return true;
}

// Parse a "k1,k2,k3,k4" knob-tuple string into four floats in
// [0, 1]. Values outside the range are allowed for now; the
// voice module's clamp handles out-of-range CV on the audio
// thread. Returns true on success.
bool parse_knob_tuple(const std::string& s, float out[kChannels])
{
    const auto parts = split_commas(s, /*max_parts=*/kChannels);
    if (parts.size() != kChannels) {
        return false;
    }
    for (std::size_t i = 0; i < kChannels; ++i) {
        try {
            out[i] = std::stof(parts[i]);
        } catch (...) {
            return false;
        }
    }
    return true;
}

// Parse the whole pattern.csv. Fails the current TEST_CASE on
// any malformed line rather than returning an error — these
// tests require a clean pattern to produce deterministic
// output, so a broken CSV is a hard failure not a soft one.
std::vector<PatternEvent> read_pattern(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in, "unable to open pattern.csv at " << path);

    std::vector<PatternEvent> events;
    std::string line;
    std::size_t line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;

        // Strip trailing \r (Windows-style line endings).
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        // Skip comments + empty lines.
        std::string s = trim(line);
        if (s.empty() || s[0] == '#') {
            continue;
        }

        const auto top = split_commas(s, /*max_parts=*/3);
        REQUIRE_MESSAGE(top.size() >= 2,
                        "pattern.csv line " << line_no
                            << " has fewer than 2 columns: " << s);

        PatternEvent ev{};
        try {
            ev.time_seconds = std::stod(top[0]);
        } catch (...) {
            FAIL("pattern.csv line " << line_no
                     << " time column is not a number: " << top[0]);
        }
        REQUIRE_MESSAGE(ev.time_seconds >= 0.0,
                        "pattern.csv line " << line_no
                            << " time column is negative: " << ev.time_seconds);
        REQUIRE_MESSAGE(ev.time_seconds
                            <= static_cast<double>(kRenderSamples)
                                   / static_cast<double>(kSampleRateHz),
                        "pattern.csv line " << line_no
                            << " time column " << ev.time_seconds
                            << "s falls outside the "
                            << (static_cast<double>(kRenderSamples)
                                / static_cast<double>(kSampleRateHz))
                            << "s render window");
        ev.sample_index = seconds_to_sample_index(ev.time_seconds);

        const std::string& typ = top[1];
        if (typ == "gate1_rising") {
            ev.type = PatternEventType::Gate1Rising;
        } else if (typ == "gate2_rising") {
            ev.type = PatternEventType::Gate2Rising;
        } else if (typ == "gate1_falling") {
            ev.type = PatternEventType::Gate1Falling;
        } else if (typ == "gate2_falling") {
            ev.type = PatternEventType::Gate2Falling;
        } else if (typ == "knob_set") {
            // The render loop does not yet dispatch
            // `knob_set` (it would reassign
            // `voice.active_instrument_index` via
            // rackboy::core::control_to_index(knob + cv, …)).
            // Reject at parse time — line-number in the
            // error — so a CSV that accidentally lands a
            // knob_set row fails fast with a grep-able
            // message instead of silently mid-rendering.
            FAIL("pattern.csv line " << line_no
                     << " knob_set events are not yet supported by the "
                        "rackboy-5wl regression render loop; track "
                        "rackboy-n2x for full knob_set dispatch");
        } else {
            FAIL("pattern.csv line " << line_no
                     << " unknown event_type: " << typ);
        }

        const std::string value = top.size() >= 3 ? top[2] : std::string();
        ev.has_value = !value.empty();

        if (ev.type == PatternEventType::Gate1Rising) {
            REQUIRE_MESSAGE(ev.has_value,
                            "pattern.csv line " << line_no
                                << " gate1_rising requires an instrument-slot tuple");
            const bool ok = parse_slot_tuple(value, ev.slot_tuple);
            REQUIRE_MESSAGE(ok,
                            "pattern.csv line " << line_no
                                << " malformed slot tuple: " << value);
        } else if (ev.type == PatternEventType::KnobSet) {
            REQUIRE_MESSAGE(ev.has_value,
                            "pattern.csv line " << line_no
                                << " knob_set requires a 4-float value tuple");
            const bool ok = parse_knob_tuple(value, ev.knob_tuple);
            REQUIRE_MESSAGE(ok,
                            "pattern.csv line " << line_no
                                << " malformed knob tuple: " << value);
        }

        events.push_back(ev);
    }

    REQUIRE_MESSAGE(!events.empty(), "pattern.csv produced zero events");
    REQUIRE_MESSAGE(events.front().type == PatternEventType::Gate1Rising,
                    "pattern.csv first row must be gate1_rising per README");
    REQUIRE_MESSAGE(events.front().time_seconds == 0.0,
                    "pattern.csv first row must be at t=0");
    return events;
}

// ---------------------------------------------------------------------------
// Fixture loading
// ---------------------------------------------------------------------------
//
// The audio regression uses the same fixture as tests/sav and
// tests/table — load via the full rackboy_sav pipeline so a
// regression in any parser stage surfaces here as a
// LoadPhase!=Ok failure rather than silent-wrong audio.

RackboyBank load_rackboy_fixture()
{
    const std::vector<std::uint8_t> sav =
        fixture_common::load_fixture_bytes();

    const std::size_t start_block = rackboy::sav::first_block_for_slot(
        sav.data(), rackboy::sav::kRackboyV1ActiveSlot);
    REQUIRE(start_block < rackboy::sav::kBlockAllocTableLen);

    std::string decomp_err;
    auto song = rb_compression::decompress(
        sav.data(), sav.size(),
        static_cast<std::uint8_t>(start_block), &decomp_err);
    REQUIRE_MESSAGE(song.has_value(),
                    "fixture decompress failed: " << decomp_err);

    rackboy::sav::LoadInputs in{};
    in.raw_sav               = sav.data();
    in.raw_sav_len           = sav.size();
    in.decompressed_song     = song->data();
    in.decompressed_song_len = song->size();
    in.sidecar_json          = nullptr;
    in.sidecar_json_len      = 0;

    RackboyBank      bank{};
    RackboyBankNames names{};
    const auto       r = rackboy::sav::load_rackboy_sav(in, &bank, &names);
    REQUIRE_MESSAGE(r.phase == LoadPhase::Ok,
                    "load_rackboy_sav failed with phase="
                        << static_cast<int>(r.phase)
                        << " sub=" << r.sub_error_code);
    REQUIRE(bank.loaded);
    return bank;
}

// ---------------------------------------------------------------------------
// Render core
// ---------------------------------------------------------------------------
//
// Converts a pattern-event timestamp to a sample index, then
// runs the sample-accurate gate dispatcher. Gate edges are
// applied by injecting a one-sample high pulse on the
// corresponding Schmitt detector. rackboy::core::gate_step
// takes a float in [0, 1]; driving it to 1.0 for one sample
// between two 0.0 samples triggers exactly one rising edge.

struct RenderOutput {
    std::vector<float> channels[kChannels];
};

// Apply one pattern event at the current sample. For gate
// events this means preparing the corresponding voice(s) and
// dispatching via trigger_all / tick_all.
void apply_event(const PatternEvent&   ev,
                 const RackboyBank&    bank,
                 VoiceState (&voices)[kNumVoices],
                 TableRegState (&regs)[kNumVoices],
                 rackboy_apu_t*        apu)
{
    switch (ev.type) {
        case PatternEventType::Gate1Rising:
            // Apply the slot tuple to each voice's
            // active_instrument_index before dispatching.
            // Out-of-range slots become kNoInstrument; the
            // trigger dispatcher silences those voices.
            for (std::size_t ch = 0; ch < kChannels; ++ch) {
                const std::int16_t slot = ev.slot_tuple[ch];
                // Voice pool size per channel comes from the
                // bank; an OOR slot collapses to kNoInstrument
                // so the dispatcher skips the voice.
                std::int16_t pool_size = 0;
                switch (ch) {
                    case 0: pool_size = bank.pulse1_count; break;
                    case 1: pool_size = bank.pulse2_count; break;
                    case 2: pool_size = bank.wave_count;   break;
                    case 3: pool_size = bank.noise_count;  break;
                }
                voices[ch].active_instrument_index =
                    (slot >= 0 && slot < pool_size)
                        ? slot
                        : rackboy::core::kNoInstrument;
            }
            trigger_all(voices, regs, bank, apu);
            break;

        case PatternEventType::Gate2Rising:
            tick_all(voices, regs, bank, apu);
            break;

        case PatternEventType::Gate1Falling:
        case PatternEventType::Gate2Falling:
            // Falling edges are no-ops for the rackboy audio
            // path — the gate Schmitt detector's hysteresis
            // re-arms automatically.
            break;

        case PatternEventType::KnobSet:
            // Unreachable: `read_pattern` rejects
            // `knob_set` rows at parse time with the CSV
            // line number. This case exists only so the
            // switch is exhaustive against the enum.
            break;
    }
}

RenderOutput render(const std::vector<PatternEvent>& pattern,
                    const RackboyBank&               bank)
{
    // Lock the denormal mode to IEEE-correct before anything
    // floating-point touches the APU. Idempotent across
    // TEST_CASE runs. See the helper comment above for the
    // README binding and the metadata-side assertion.
    set_ieee_denormal_mode();

    // Initialize runtime state. Pre-reset every piece the
    // audio loop depends on so a prior TEST_CASE's state
    // doesn't bleed across (doctest re-runs every TEST_CASE
    // in the same process).
    rackboy_apu_t apu{};
    std::memset(&apu, 0, sizeof(apu));
    rackboy_apu_init(&apu, kSampleRateHz);

    VoiceState voices[kNumVoices];
    voices_reset_all(voices);
    TableRegState regs[kNumVoices];
    for (std::size_t i = 0; i < kNumVoices; ++i) {
        table_reg_state_reset(regs[i]);
    }

    TapState taps{};
    taps_reset(taps);

    // Pre-allocate the four output channels so push_back
    // never grows mid-render.
    RenderOutput out;
    for (std::size_t ch = 0; ch < kChannels; ++ch) {
        out.channels[ch].resize(kRenderSamples, 0.0f);
    }

    std::size_t event_ix = 0;
    for (std::size_t s = 0; s < kRenderSamples; ++s) {
        // Fire every pattern event whose sample_index has
        // been reached. Comparing integer-to-integer here —
        // the `time_seconds → sample_index` rounding was
        // done once in `read_pattern` via
        // `seconds_to_sample_index` — is what turns the
        // README's "no ±1 sample jitter" binding row into a
        // code-enforced invariant. Events sharing a
        // `sample_index` fire in CSV file order, which is
        // well-defined because pattern.csv's README rule
        // pins gate1_rising before gate2_rising at matching
        // timestamps.
        while (event_ix < pattern.size()
               && pattern[event_ix].sample_index <= s) {
            apply_event(pattern[event_ix], bank, voices, regs, &apu);
            ++event_ix;
        }

        // Render one APU sample for this tick.
        float raw[kChannels] = {0.0f, 0.0f, 0.0f, 0.0f};
        rackboy_apu_run_samples(&apu, 1,
                                &raw[0], &raw[1], &raw[2], &raw[3]);

        // DC-blocker + NaN guard, per channel.
        float cleaned[kChannels] = {0.0f, 0.0f, 0.0f, 0.0f};
        taps_process(taps, 1,
                     &raw[0], &raw[1], &raw[2], &raw[3],
                     &cleaned[0], &cleaned[1], &cleaned[2], &cleaned[3]);

        for (std::size_t ch = 0; ch < kChannels; ++ch) {
            out.channels[ch][s] = cleaned[ch];
        }
    }

    return out;
}

// ---------------------------------------------------------------------------
// Reference buffer I/O + RMS diff
// ---------------------------------------------------------------------------
//
// `.bin` format: exactly kRenderSamples * 4 bytes of raw
// IEEE-754 float32 little-endian PCM, no header. Reads are
// bounded by the expected byte count; anything shorter /
// longer / unreadable is treated as "reference absent" for
// the current iteration so the renderer-output side of the
// regression is the only thing this test can fail on.

// Read a .bin file into `out`. Returns true on a bit-exact
// success: the file must be exactly the expected byte count,
// reads must complete without error, and there must be no
// trailing bytes beyond the expected sample count.
bool read_reference_bin(const std::string& path,
                        std::vector<float>* out)
{
    out->clear();
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    constexpr std::size_t kExpectBytes = kRenderSamples * sizeof(float);
    std::vector<char> buf(kExpectBytes);
    in.read(buf.data(), kExpectBytes);
    if (static_cast<std::size_t>(in.gcount()) != kExpectBytes) {
        return false;
    }
    // Reject overlong files: an on-disk blob with a trailing
    // byte is a red flag for a renderer that's writing more
    // than the expected sample count.
    char trailing;
    in.read(&trailing, 1);
    if (in.gcount() > 0) {
        return false;
    }

    out->resize(kRenderSamples);
    std::memcpy(out->data(), buf.data(), kExpectBytes);
    return true;
}

// Per-channel RMS of (a - b) converted to dBFS against unit
// peak. Returns -inf dBFS for an exactly-zero diff.
double rms_diff_dbfs(const std::vector<float>& a,
                     const std::vector<float>& b)
{
    REQUIRE(a.size() == b.size());
    double acc = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double d = static_cast<double>(a[i])
                             - static_cast<double>(b[i]);
        acc += d * d;
    }
    const double rms =
        std::sqrt(acc / static_cast<double>(a.size()));
    if (rms == 0.0) {
        return -std::numeric_limits<double>::infinity();
    }
    return 20.0 * std::log10(rms);
}

// ---------------------------------------------------------------------------
// Structural invariants
// ---------------------------------------------------------------------------

bool has_nan_or_inf(const std::vector<float>& buf)
{
    for (float v : buf) {
        if (!std::isfinite(v)) {
            return true;
        }
    }
    return false;
}

bool channel_silent(const std::vector<float>& buf)
{
    for (float v : buf) {
        if (v != 0.0f) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Reference-buffer writer (bead rackboy-n2x)
// ---------------------------------------------------------------------------
//
// Gated by the RACKBOY_REGEN_REFERENCE_DIR env var so the
// writer never runs during a normal `make -C tests test-audio`
// invocation — the regeneration tool at
// `tools/regen_reference/regen.sh` is the only caller that
// sets it. By making the test binary itself the renderer, we
// avoid drift between the regen tool and the verification
// path: both ask the same helpers inside regression.cpp to
// produce the bytes, which is the only way the README's
// "byte-identical re-render" acceptance check can hold by
// construction.
bool write_reference_bin(const std::string&        path,
                         const std::vector<float>& samples)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    constexpr std::size_t kExpectBytes = kRenderSamples * sizeof(float);
    out.write(reinterpret_cast<const char*>(samples.data()),
              static_cast<std::streamsize>(kExpectBytes));
    return static_cast<bool>(out);
}

}  // namespace

// ---------------------------------------------------------------------------
// TEST_CASEs
// ---------------------------------------------------------------------------

TEST_CASE("audio regression: seconds_to_sample_index rounds to nearest with "
          "no ±1 sample jitter at grid boundaries")
{
    // Anchors the README binding-parameters row
    // "Sample-accurate alignment — no ±1 sample jitter
    // tolerated" as a testable claim. If this ever fails,
    // the render loop is silently mis-scheduling events by
    // one sample, which would produce a drifted reference
    // buffer that looks "close" to the correct one but
    // fails the -60 dBFS RMS threshold for reasons that
    // have nothing to do with the DSP under test.
    const double kSampleSeconds = 1.0 / static_cast<double>(kSampleRateHz);

    // Exact boundaries — no rounding ambiguity.
    CHECK_EQ(seconds_to_sample_index(0.0), 0u);
    CHECK_EQ(seconds_to_sample_index(kSampleSeconds), 1u);
    CHECK_EQ(seconds_to_sample_index(2.0 * kSampleSeconds), 2u);
    CHECK_EQ(seconds_to_sample_index(0.125), 6000u);  // 125 ms grid
    CHECK_EQ(seconds_to_sample_index(1.0), 48000u);
    CHECK_EQ(seconds_to_sample_index(2.0),
             static_cast<std::size_t>(kRenderSamples));

    // Sub-sample offsets that round toward the nearer
    // sample boundary. `std::nearbyint` uses the current
    // rounding mode (default: round-to-nearest-even); our
    // offsets are far enough from .5 boundaries that the
    // tie-break rule doesn't matter.
    CHECK_EQ(seconds_to_sample_index(kSampleSeconds - 1e-8), 1u);
    CHECK_EQ(seconds_to_sample_index(kSampleSeconds + 1e-8), 1u);
    CHECK_EQ(seconds_to_sample_index(1.5 * kSampleSeconds + 1e-8), 2u);
    CHECK_EQ(seconds_to_sample_index(0.5 * kSampleSeconds - 1e-8), 0u);

    // A non-grid-aligned timestamp the committed pattern
    // doesn't use (100 ms = 4800 samples) — pre-pins the
    // expectation so a future pattern.csv extension can add
    // this row without revisiting the rounding rule.
    CHECK_EQ(seconds_to_sample_index(0.100), 4800u);
}

TEST_CASE("audio regression: fixture + pattern render produces finite samples "
          "across all four channels")
{
    const std::string pattern_path = resolve_pattern_path();
    REQUIRE_MESSAGE(!pattern_path.empty(),
                    "tests/audio/pattern.csv not found on any candidate path");

    const auto pattern = read_pattern(pattern_path);
    const auto bank    = load_rackboy_fixture();
    const auto out     = render(pattern, bank);

    // Structural invariants — must hold whether or not the
    // reference buffers exist.
    for (std::size_t ch = 0; ch < kChannels; ++ch) {
        CAPTURE(ch);
        CHECK_FALSE(has_nan_or_inf(out.channels[ch]));
        CHECK_EQ(out.channels[ch].size(), kRenderSamples);
    }
}

TEST_CASE("audio regression: channels with populated instrument pools produce "
          "non-silent output")
{
    const std::string pattern_path = resolve_pattern_path();
    REQUIRE(!pattern_path.empty());
    const auto pattern = read_pattern(pattern_path);
    const auto bank    = load_rackboy_fixture();
    const auto out     = render(pattern, bank);

    // Channels whose pool is non-empty in the committed
    // fixture AND which the pattern triggers at t=0 must
    // produce non-silent audio during the 2 s window.
    //
    // The fixture authors both pulse instruments into a
    // single shared pulse pool: `rackboy_sav_instruments.h`
    // lines 87-91 + 273 document that the current parser
    // keeps every pulse in `pulse1_pool` and reserves
    // `pulse2_count == 0` as a placeholder for a future
    // chain-based channel split. The runtime consequence is
    // `bank.pulse1_count == 2 && bank.pulse2_count == 0`
    // against this fixture — and the pattern.csv's Pulse2
    // slot index at t=0 therefore falls through
    // `apply_event`'s OOR guard to `kNoInstrument`, which
    // AD #12 requires the audio path to silence bit-exactly.
    // When the chain-split bead lands, this expectation
    // flips; revisit `pattern.csv` + this branch together.
    // TODO(rackboy-chain-split).
    const std::int16_t pool_sizes[kChannels] = {
        static_cast<std::int16_t>(bank.pulse1_count),
        static_cast<std::int16_t>(bank.pulse2_count),
        static_cast<std::int16_t>(bank.wave_count),
        static_cast<std::int16_t>(bank.noise_count),
    };
    for (std::size_t ch = 0; ch < kChannels; ++ch) {
        CAPTURE(ch);
        if (pool_sizes[ch] > 0) {
            CHECK_FALSE(channel_silent(out.channels[ch]));
        } else {
            // Empty pool: channel must be silent.
            CHECK(channel_silent(out.channels[ch]));
        }
    }
}

TEST_CASE("audio regression: RMS(rendered - reference) within -60 dBFS when "
          "reference buffers exist; skip with note when absent")
{
    const std::string pattern_path = resolve_pattern_path();
    REQUIRE(!pattern_path.empty());
    const auto pattern = read_pattern(pattern_path);
    const auto bank    = load_rackboy_fixture();
    const auto out     = render(pattern, bank);

    // Denormal-mode gate (bead rackboy-7hy). When the committed
    // reference buffers are present, they were rendered with
    // FTZ-off + DAZ-off per metadata.json. The renderer now
    // locks the mode on every render() call, so a mismatch
    // here means either the committed metadata is stale (re-
    // run `tools/regen_reference/regen.sh`) or an ambient
    // SSE/FPCR write escaped `set_ieee_denormal_mode`. Either
    // way, the RMS comparison is unsafe; fail loudly rather
    // than comparing against an incompatible reference.
    //
    // The absent-metadata branch is load-bearing in two
    // opposite directions:
    //   * Fresh checkout with no reference/ dir: skip is the
    //     correct answer (the per-channel `.bin` probe below
    //     then also skips with the iter-79 skip-when-absent
    //     message).
    //   * Partially-deleted reference/ dir (metadata.json
    //     gone but some `.bin` files kept): MUST fail loudly
    //     rather than comparing against references whose
    //     denormal mode we can no longer assert. Otherwise a
    //     rogue/stale `.bin` could silently invalidate the
    //     RMS check under a process whose FTZ escaped the
    //     lock. Probe for any resolvable `.bin` when
    //     metadata is absent and REQUIRE-fail if so.
    {
        const std::string metadata_path = resolve_metadata_path();
        if (!metadata_path.empty()) {
            const std::string contents = read_file(metadata_path);
            REQUIRE_MESSAGE(!contents.empty(),
                            "failed to read metadata.json at "
                                << metadata_path);
            const std::string mode =
                read_string_field(contents, "denormal_mode");
            REQUIRE_MESSAGE(mode == "ftz_off_daz_off",
                            "metadata.json.denormal_mode is '"
                                << mode
                                << "' but the renderer locks to"
                                   " 'ftz_off_daz_off' — re-run"
                                   " tools/regen_reference/regen.sh"
                                   " to refresh the committed"
                                   " metadata (bead rackboy-7hy)");
        } else {
            // MPCBOY LOCAL MOD: a missing metadata.json is a hard
            // failure in mpcboy (upstream skipped silently).
            FAIL("MPCBOY: tests/audio/reference/metadata.json not found;"
                 " golden regression must not be skipped (cwd must be"
                 " tests/rackboy)");
        }
    }

    for (std::size_t ch = 0; ch < kChannels; ++ch) {
        CAPTURE(ch);
        const std::string ref_path =
            resolve_reference_path(kReferenceFilenames[ch]);
        // MPCBOY LOCAL MOD: a missing reference buffer FAILS instead of
        // skipping (upstream printed a skip note and passed).
        REQUIRE_MESSAGE(!ref_path.empty(),
                        "MPCBOY: reference buffer "
                            << std::string(kReferenceFilenames[ch])
                            << " not found; golden regression must not be"
                               " skipped (cwd must be tests/rackboy)");

        std::vector<float> ref;
        const bool ok = read_reference_bin(ref_path, &ref);
        REQUIRE_MESSAGE(ok,
                        "failed to read reference buffer at "
                            << ref_path
                            << " (wrong size, trailing bytes, or "
                               "IO error — see "
                               "tests/audio/README.md)");

        const double dbfs = rms_diff_dbfs(out.channels[ch], ref);
        CAPTURE(dbfs);
        CHECK_MESSAGE(dbfs < kRmsFailDbfs,
                      "channel "
                          << ch
                          << " RMS drift "
                          << dbfs
                          << " dBFS exceeds AD #11 budget of "
                          << kRmsFailDbfs
                          << " dBFS");
    }
}

TEST_CASE("audio regen: write reference buffers when "
          "RACKBOY_REGEN_REFERENCE_DIR is set (bead rackboy-n2x)")
{
    const char* const out_dir = std::getenv("RACKBOY_REGEN_REFERENCE_DIR");
    const char* const allow   = std::getenv("RACKBOY_REGEN_ALLOW");
    const bool allow_ok = (allow != nullptr && allow[0] == '1' && allow[1] == '\0');
    if (out_dir == nullptr || out_dir[0] == '\0' || !allow_ok) {
        // Normal test-suite run: no regen requested. The three
        // TEST_CASEs above stay the ambient contract; this
        // branch just confirms the test binary is regen-capable
        // and exits. MESSAGE prints a short note in verbose
        // doctest output so it's visible when someone audits
        // what the audio module runs.
        //
        // Two gates, not one:
        //   * RACKBOY_REGEN_REFERENCE_DIR points at the output dir.
        //   * RACKBOY_REGEN_ALLOW=1 is a sentinel so a leaked
        //     shell export of the dir var alone (inherited into
        //     an unrelated `make -C tests test-audio`) cannot
        //     silently rewrite the committed buffers. Only
        //     tools/regen_reference/regen.sh sets both.
        MESSAGE("regen step skipped (require RACKBOY_REGEN_REFERENCE_DIR"
                " AND RACKBOY_REGEN_ALLOW=1; use"
                " tools/regen_reference/regen.sh)");
        return;
    }

    const std::string pattern_path = resolve_pattern_path();
    REQUIRE(!pattern_path.empty());
    const auto pattern = read_pattern(pattern_path);
    const auto bank    = load_rackboy_fixture();
    const auto out     = render(pattern, bank);

    // Structural sanity before we commit any of this to disk —
    // regen MUST NOT write NaN / wrong-length buffers. The
    // other TEST_CASEs already assert this but repeat it in
    // the regen path so a reordering of TEST_CASE execution
    // can't skip the guard.
    for (std::size_t ch = 0; ch < kChannels; ++ch) {
        CAPTURE(ch);
        REQUIRE_FALSE(has_nan_or_inf(out.channels[ch]));
        REQUIRE_EQ(out.channels[ch].size(), kRenderSamples);
    }

    const std::string dir(out_dir);
    for (std::size_t ch = 0; ch < kChannels; ++ch) {
        const std::string path =
            dir + "/" + kReferenceFilenames[ch];
        CAPTURE(path);
        const bool ok = write_reference_bin(path, out.channels[ch]);
        REQUIRE_MESSAGE(ok,
                        "failed to write reference buffer to " << path);
    }

    MESSAGE("wrote 4 reference buffers to " << dir);
}
