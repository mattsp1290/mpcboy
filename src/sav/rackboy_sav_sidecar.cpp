// rackboy_sav/rackboy_sav_sidecar.cpp
//
// Implementation of the `<savname>.json` sidecar parser
// declared in rackboy_sav_sidecar.h. Bead rackboy-69j.
//
// The JSON subset is strict (see the header for the full
// schema), which keeps the parser around 200 lines with no
// STL, no heap, and no recursion. The state machine is:
//
//   1. skip whitespace, expect `{`
//   2. repeat:
//        skip whitespace
//        if next is `}`: stop
//        expect `"` string (key)
//        classify key as CHANNEL-SLOT
//        skip whitespace, expect `:`
//        skip whitespace, expect `"` string (value)
//        store value at channel[slot].text (null-terminate)
//        skip whitespace
//        if next is `,`: consume and loop
//        if next is `}`: stop
//        else: InvalidJson
//   3. skip trailing whitespace, expect EOF
//
// An empty object `{}` is a legal sidecar (every slot
// empty) and returns Ok with a fully-reset
// RackboyBankNames.

#include "rackboy_sav_sidecar.h"

#include <cstddef>

#include "rackboy_sav_types.h"

namespace rackboy {
namespace sav {

namespace {

// ---------------------------------------------------------------------------
// Whitespace + byte classification
// ---------------------------------------------------------------------------

inline bool is_ws(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// Printable ASCII byte allowed inside a string value.
// Deliberately excludes DEL (0x7F) and every control byte
// (0x00..0x1F) per the strict-subset contract. The `"` and
// `\` bytes are the caller's responsibility to handle — see
// the parse_string loop.
inline bool is_printable(char c) noexcept
{
    const unsigned char u = static_cast<unsigned char>(c);
    return u >= 0x20 && u <= 0x7E;
}

// Parser cursor. Kept small and passed by reference so every
// helper is a leaf function and the state machine stays flat.
struct Cursor {
    const char* json;
    std::size_t len;
    std::size_t pos;
};

inline bool at_eof(const Cursor& c) noexcept
{
    return c.pos >= c.len;
}

inline char peek(const Cursor& c) noexcept
{
    // Caller must confirm !at_eof first. This helper never
    // reads past the buffer — the `pos < len` assertion is
    // the contract.
    return c.json[c.pos];
}

inline void advance(Cursor& c) noexcept
{
    ++c.pos;
}

inline void skip_ws(Cursor& c) noexcept
{
    while (!at_eof(c) && is_ws(peek(c))) {
        advance(c);
    }
}

// ---------------------------------------------------------------------------
// String parsing (keys and values share the same shape)
// ---------------------------------------------------------------------------
//
// Parses a JSON string from the current position. On Ok
// return, `*out_body` points into the original `json`
// buffer at the first character of the string body, and
// `*out_len` is the body length in bytes (not counting the
// surrounding quotes). The cursor is left just past the
// closing `"`. Returns `InvalidJson` if:
//
//   * the cursor isn't pointing at `"`,
//   * the buffer ends before the closing `"`,
//   * a `\` escape byte appears anywhere in the body
//     (strict subset, no escapes),
//   * a non-printable byte appears in the body.
//
// Note: `*out_body` aliases the caller-provided `json`
// buffer; it is stable for the lifetime of that buffer.
SidecarError parse_string(Cursor&       c,
                          const char**  out_body,
                          std::size_t*  out_len) noexcept
{
    if (at_eof(c) || peek(c) != '"') {
        return SidecarError::InvalidJson;
    }
    advance(c);  // consume opening quote

    const std::size_t body_start = c.pos;

    while (true) {
        if (at_eof(c)) {
            return SidecarError::InvalidJson;
        }
        const char ch = peek(c);
        if (ch == '"') {
            break;
        }
        if (ch == '\\') {
            // Strict subset: no escapes at all.
            return SidecarError::InvalidJson;
        }
        if (!is_printable(ch)) {
            return SidecarError::InvalidJson;
        }
        advance(c);
    }

    *out_body = c.json + body_start;
    *out_len  = c.pos - body_start;
    advance(c);  // consume closing quote
    return SidecarError::Ok;
}

// ---------------------------------------------------------------------------
// Key classification
// ---------------------------------------------------------------------------
//
// A key must match `PU1-NN`, `PU2-NN`, `WV-NN`, or
// `NO-NN` where NN is exactly two decimal digits that form
// a slot index in [0, kMaxInstrumentsPerChannel).
//
// On success sets `*out_channel` to 0/1/2/3 for
// pulse1/pulse2/wave/noise, and `*out_slot` to the slot
// index. On classification failure returns `UnknownChannel`
// or `SlotOutOfRange`.
//
// Channel IDs (only used inside this translation unit):
enum : int {
    kChanPulse1 = 0,
    kChanPulse2 = 1,
    kChanWave   = 2,
    kChanNoise  = 3,
};

inline bool is_dec_digit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

// Sidecar keys spell the slot index with exactly two
// decimal digits (`PU1-00` ... `PU1-63`). If a future bead
// bumps `kMaxInstrumentsPerChannel` past 100 the two-digit
// format becomes ambiguous — slot 100 would collide with
// slot 10 as `PU1-10` — so the key format AND `classify_key`
// would have to be updated in lockstep. Fire a build error
// at the source of the coupling rather than silently
// dropping the addressable slot range.
static_assert(kMaxInstrumentsPerChannel <= 100,
              "Sidecar keys use exactly two decimal "
              "digits (PU1-NN). If kMaxInstrumentsPerChannel "
              "grows past 100, the key format and "
              "classify_key must be updated in lockstep.");

SidecarError classify_key(const char*  body,
                          std::size_t  len,
                          int*         out_channel,
                          std::size_t* out_slot) noexcept
{
    // All four legal channel prefixes leave exactly two
    // slot digits after a `-`, so the total key length is
    // `prefix_len + 1 + 2`:
    //
    //   PU1-NN -> 6
    //   PU2-NN -> 6
    //   WV-NN  -> 5
    //   NO-NN  -> 5
    //
    // Reject anything else before touching individual bytes
    // so a one-byte key can't trip a read past the buffer.
    if (len != 5 && len != 6) {
        return SidecarError::UnknownChannel;
    }

    std::size_t prefix_len = 0;
    int channel = -1;

    if (len == 6 && body[0] == 'P' && body[1] == 'U' &&
        (body[2] == '1' || body[2] == '2')) {
        channel    = (body[2] == '1') ? kChanPulse1 : kChanPulse2;
        prefix_len = 3;
    } else if (len == 5 && body[0] == 'W' && body[1] == 'V') {
        channel    = kChanWave;
        prefix_len = 2;
    } else if (len == 5 && body[0] == 'N' && body[1] == 'O') {
        channel    = kChanNoise;
        prefix_len = 2;
    } else {
        return SidecarError::UnknownChannel;
    }

    if (body[prefix_len] != '-') {
        return SidecarError::UnknownChannel;
    }

    const char d0 = body[prefix_len + 1];
    const char d1 = body[prefix_len + 2];
    if (!is_dec_digit(d0) || !is_dec_digit(d1)) {
        return SidecarError::UnknownChannel;
    }

    const std::size_t slot =
        static_cast<std::size_t>(d0 - '0') * 10u +
        static_cast<std::size_t>(d1 - '0');

    if (slot >= kMaxInstrumentsPerChannel) {
        return SidecarError::SlotOutOfRange;
    }

    *out_channel = channel;
    *out_slot    = slot;
    return SidecarError::Ok;
}

// Store a decoded name into the matching slot of the names
// struct. Copies `len` bytes verbatim (we already verified
// they are printable ASCII in parse_string) and
// null-terminates. Returns `NameTooLong` if the body does
// not fit in `kMaxInstrumentNameBytes - 1` bytes.
SidecarError store_name(RackboyBankNames& out,
                        int               channel,
                        std::size_t       slot,
                        const char*       body,
                        std::size_t       len) noexcept
{
    if (len + 1 > kMaxInstrumentNameBytes) {
        return SidecarError::NameTooLong;
    }

    RackboyInstrumentName* target = nullptr;
    switch (channel) {
        case kChanPulse1: target = &out.pulse1[slot]; break;
        case kChanPulse2: target = &out.pulse2[slot]; break;
        case kChanWave:   target = &out.wave  [slot]; break;
        case kChanNoise:  target = &out.noise [slot]; break;
    }
    // Unreachable: classify_key pins `channel` to one of
    // the four kChan* constants before we get here. If
    // this branch ever fires it is an internal invariant
    // bug (NOT a malformed sidecar file), but we still
    // soft-fail rather than invoke UB via a null write —
    // the parser runs on the file-IO thread off the audio
    // path and silent termination of a runaway caller is
    // preferable to a crash.
    if (target == nullptr) {
        return SidecarError::InvalidJson;
    }

    // Copy verbatim, then terminate AND zero the tail
    // bytes so a duplicate-key overwrite with a shorter
    // second value produces a byte-for-byte deterministic
    // POD layout: every byte past the new null terminator
    // is `\0`, matching a freshly `names_reset`'d slot.
    // C-string readers were already correct before this
    // second loop (they stop at the first `\0`), but a
    // raw-buffer hash / memcmp / SPSC-bound fingerprint
    // over the POD must not leak tail bytes from an
    // earlier same-slot entry. No memcpy / memset: we
    // stay standalone without <cstring> to keep the
    // include footprint minimal and predictable on Daisy.
    for (std::size_t i = 0; i < len; ++i) {
        target->text[i] = body[i];
    }
    for (std::size_t i = len; i < kMaxInstrumentNameBytes; ++i) {
        target->text[i] = '\0';
    }
    return SidecarError::Ok;
}

}  // namespace

SidecarError parse_sidecar(const char*       json,
                           std::size_t       json_len,
                           RackboyBankNames* out_names) noexcept
{
    // (1) Parameter-level guard: out_names first so a null
    //     names pointer cannot have its memory touched.
    if (out_names == nullptr) {
        return SidecarError::NullOutNames;
    }

    // (2) Reset the destination unconditionally. Every code
    //     path below this line leaves *out_names in a known
    //     state: freshly reset on any error return, fully
    //     populated on Ok.
    names_reset(*out_names);

    // (3) Null-json guard. Placed AFTER names_reset so a
    //     caller who poisoned out_names still gets a clean
    //     slate on the null-json error path.
    if (json == nullptr) {
        return SidecarError::NullJson;
    }

    Cursor c{json, json_len, 0};

    // (4) Expect a top-level `{`.
    skip_ws(c);
    if (at_eof(c) || peek(c) != '{') {
        names_reset(*out_names);
        return SidecarError::InvalidJson;
    }
    advance(c);

    // (5) Empty-object short-circuit: `{}` is a legal (but
    //     useless) sidecar. Returning Ok here keeps the
    //     "caller can always load an empty sidecar without
    //     special-casing the empty-file path" contract.
    skip_ws(c);
    if (!at_eof(c) && peek(c) == '}') {
        advance(c);
        skip_ws(c);
        if (!at_eof(c)) {
            names_reset(*out_names);
            return SidecarError::InvalidJson;
        }
        return SidecarError::Ok;
    }

    // (6) Parse zero-or-more `"KEY": "VALUE"` pairs.
    while (true) {
        // Key.
        const char* key_body = nullptr;
        std::size_t key_len  = 0;
        SidecarError err =
            parse_string(c, &key_body, &key_len);
        if (err != SidecarError::Ok) {
            names_reset(*out_names);
            return err;
        }

        int         channel = -1;
        std::size_t slot    = 0;
        err = classify_key(key_body, key_len, &channel, &slot);
        if (err != SidecarError::Ok) {
            names_reset(*out_names);
            return err;
        }

        // Colon between key and value.
        skip_ws(c);
        if (at_eof(c) || peek(c) != ':') {
            names_reset(*out_names);
            return SidecarError::InvalidJson;
        }
        advance(c);

        // Value.
        skip_ws(c);
        const char* val_body = nullptr;
        std::size_t val_len  = 0;
        err = parse_string(c, &val_body, &val_len);
        if (err != SidecarError::Ok) {
            names_reset(*out_names);
            return err;
        }

        err = store_name(*out_names, channel, slot,
                         val_body, val_len);
        if (err != SidecarError::Ok) {
            names_reset(*out_names);
            return err;
        }

        // Separator: `,` -> more pairs, `}` -> done,
        // anything else -> InvalidJson.
        skip_ws(c);
        if (at_eof(c)) {
            names_reset(*out_names);
            return SidecarError::InvalidJson;
        }
        const char sep = peek(c);
        if (sep == ',') {
            advance(c);
            skip_ws(c);
            // A trailing comma before `}` is a strict-
            // subset violation; parse_string at the top of
            // the next loop iteration will fail cleanly
            // with InvalidJson if that happens.
            continue;
        }
        if (sep == '}') {
            advance(c);
            break;
        }
        names_reset(*out_names);
        return SidecarError::InvalidJson;
    }

    // (7) After the closing `}`, only trailing whitespace
    //     is allowed. Garbage bytes after the object are
    //     a parse error — catches a truncation bug where
    //     the caller accidentally concatenates two
    //     sidecars.
    skip_ws(c);
    if (!at_eof(c)) {
        names_reset(*out_names);
        return SidecarError::InvalidJson;
    }

    return SidecarError::Ok;
}

const char* sidecar_error_string(SidecarError e) noexcept
{
    // No default case on purpose — -Wswitch-enum catches a
    // future enumerator addition at compile time, matching
    // the rackboy_sav_{header,instruments,tables,waves}
    // convention.
    switch (e) {
        case SidecarError::Ok:
            return "ok";
        case SidecarError::NullJson:
            return "null sidecar buffer";
        case SidecarError::NullOutNames:
            return "null output names struct";
        case SidecarError::InvalidJson:
            return "invalid sidecar JSON";
        case SidecarError::UnknownChannel:
            return "unknown channel prefix in sidecar key";
        case SidecarError::SlotOutOfRange:
            return "sidecar key slot index out of range";
        case SidecarError::NameTooLong:
            return "sidecar value name too long";
    }
    // Only reachable if a caller cast an arbitrary int to
    // SidecarError (e.g. a log-replay tool deserializing a
    // corrupt log line). The enum-value contract in the
    // header pins enumerators 0..6; anything else is a
    // deserialization bug. We still return a string instead
    // of asserting because logging tools would rather print
    // "unknown" than crash.
    return "unknown SidecarError value";
}

}  // namespace sav
}  // namespace rackboy
