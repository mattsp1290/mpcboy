// rackboy_core/rackboy_bank.h
//
// Audio-thread-owned bank slot + single-producer / single-consumer
// handoff for the LSDJ .sav-derived instrument bank.
//
// This is the plumbing that lets an off-audio-thread parser hand a
// freshly-populated `rackboy::sav::RackboyBank` to the audio thread
// without ever allocating, locking, or touching memory the audio
// thread might be reading. Bead rackboy-2i8.
//
// Ownership model
// ---------------
// The `BankSlot` pre-allocates TWO full-size `RackboyBank` payloads
// ("A" and "B") at module construction and keeps one of them
// designated as the audio-thread-visible bank via an atomic pointer.
//
//   * The audio thread reads the current bank via `current()`, which
//     performs a single acquire-load of the atomic pointer. Once it
//     has the pointer, the bank it points at is stable — the writer
//     side never mutates that payload until it becomes the inactive
//     slot again.
//
//   * The writer thread (a.k.a. the parser / UI thread) calls
//     `swap_bank(const RackboyBank& staged)` to publish a new bank.
//     `swap_bank` copies `staged` into the currently-inactive slot
//     and then release-stores the new pointer into the atomic. After
//     the store returns, the old slot is the "inactive" one and the
//     writer can reuse it on the next call.
//
// Why a BankSlot helper and not just a bare `std::atomic<RackboyBank*>`
// -------------------------------------------------------------------
// Three reasons:
//
//   1. **Storage.** Someone has to own the two payload buffers. On
//      Daisy the whole `BankSlot` goes in `DSY_SDRAM_BSS`; on VCV it
//      lives as a member of the module struct. Either way, wrapping
//      the pair of banks + the atomic pointer in one aggregate makes
//      the memory placement obvious at the call site and keeps the
//      two banks from drifting apart.
//
//   2. **SPSC invariant.** The "one producer, one consumer" rule is
//      what makes the pointer-swap safe without a full mutex. Having
//      a dedicated type documents and localizes that contract rather
//      than relying on every call site to remember it.
//
//   3. **No alloc in process().** The audio thread's hot path only
//      ever dereferences a pointer. It never constructs, destructs,
//      or copies a `RackboyBank`. The writer thread's copy into the
//      inactive slot is the only memcpy involved, and it happens
//      off-audio-thread.
//
// Concurrency model (informal proof sketch)
// -----------------------------------------
// Single-producer / single-consumer with a two-slot ping-pong:
//
//   Writer (non-audio):                Reader (audio):
//     1. stage fresh bank in the         a. p = active_.load(acquire)
//        slot that active_ does NOT      b. read *p (pool counts,
//        point at                            instruments, tables, waves)
//     2. active_.store(staged, release)  c. return output samples
//
// Step (1) only ever writes into the inactive slot, which the reader
// cannot observe: whatever pointer the reader acquired in (a) is the
// *other* buffer, and the writer never mutates the buffer the reader
// is currently holding. Step (2)'s release-store pairs with any
// subsequent (a) acquire-load, so all of (1)'s bytes are visible
// before the reader sees the new pointer. The reader's read in (b)
// happens-before the next writer call because the writer cannot
// re-enter until the audio thread has moved on to a new `current()`
// call — by assumption of the SPSC contract, there is only ONE
// writer, and it runs strictly sequentially.
//
// If we ever want multiple writers, or long-lived reader-side holds
// across a swap, this file is the wrong primitive — reach for an
// RCU / hazard-pointer / epoch-based approach instead. That is not
// the rackboy-2i8 scope.

#ifndef RACKBOY_CORE_RACKBOY_BANK_H_
#define RACKBOY_CORE_RACKBOY_BANK_H_

#include <atomic>
#include <cstddef>
#include <type_traits>

#include "rackboy_sav_types.h"

namespace rackboy {
namespace core {

// Alias the sav-layer bank type into the core namespace so call sites
// in rackboy_core don't have to reach into the sav namespace just to
// name the payload. The alias is a pure type re-export; no new
// fields, no wrapping.
using Bank = ::rackboy::sav::RackboyBank;

// BankSlot — two-buffer SPSC handoff for a single audio-thread bank.
//
// Storage:
//
//   buffers_[0], buffers_[1]
//       The two `Bank` payloads. Both are pre-reset to the power-on
//       silent state in `init()`. Exactly one of them is the
//       audio-thread-visible "current" bank at any time.
//
//   active_
//       Atomic pointer to whichever buffer is current. Always points
//       at one of `&buffers_[0]` / `&buffers_[1]`, never null after
//       `init()` has run.
//
// Thread contract:
//   * `init()` must be called from the owning thread before any
//     audio-thread access. It is NOT safe to call concurrently with
//     `current()` or `swap_bank()`.
//   * `current()` is audio-thread safe. It performs a single
//     acquire-load and returns a pointer that stays valid until the
//     next `swap_bank()` *from the same writer* completes. Under the
//     SPSC contract the audio thread is expected to consume the
//     pointer within one process() call and not hold it across
//     swaps; this is how most audio graphs already work.
//   * `swap_bank()` is called from exactly one writer thread. Calling
//     it from more than one thread concurrently is undefined.
//
// POD contract:
//   `BankSlot` itself is NOT trivially copyable (it contains an
//   atomic), which is fine — the audio-thread-visible *payload* is
//   the `Bank` value, and that one retains the trivially-copyable /
//   standard-layout contract the sav-layer header already asserts.
//   BankSlot is constructed in place (e.g. as a module member or a
//   DSY_SDRAM_BSS global) and never memcpy'd.

// Storage guidance:
//   Daisy target:  place in DSY_SDRAM_BSS. The two Bank payloads plus
//                  the atomic pointer come to ~22 KiB — a rounding
//                  error against the Patch / Seed 64 MB SDRAM budget,
//                  and still comfortably under the STM32H750's 512 KiB
//                  internal SRAM if the module is ever relocated there.
//   VCV target:    plain module member; constructed once per module
//                  instance. The UI / parser thread owns the writer
//                  side; the Rack audio thread is the reader.
//
// A single `BankSlot` per module instance is the expected pattern. If
// multiple BankSlots ever end up adjacent in memory, revisit cache-line
// alignment (see the `alignas(64)` note on the struct below).
//
// TODO: add `alignas(64)` if we ever place multiple BankSlots
// adjacently — today's single-slot layout does not benefit, so we
// leave the default alignment to keep sizeof(BankSlot) minimal.
struct BankSlot {
    // Construct an uninitialized slot. Call `init()` before any
    // audio-thread access. The two-step initialization exists so
    // BankSlot can live at file scope / in DSY_SDRAM_BSS, where
    // non-trivial constructors with side effects are awkward. `init()`
    // is also where the two payload buffers get reset to silence.
    BankSlot() noexcept;

    // Non-copyable / non-movable: two slots would need two atomic
    // pointers with their own SPSC invariants, and a move would
    // silently invalidate the audio thread's current pointer.
    BankSlot(const BankSlot&)            = delete;
    BankSlot& operator=(const BankSlot&) = delete;
    BankSlot(BankSlot&&)                 = delete;
    BankSlot& operator=(BankSlot&&)      = delete;

    // Reset both payload buffers to the power-on silent state and
    // publish `buffers_[0]` as the current bank. Safe to call again
    // from the owning thread while the audio thread is NOT running;
    // not safe to call concurrently with `current()` or `swap_bank()`.
    void init() noexcept;

    // Audio-thread read path. Returns a pointer to the currently
    // published bank. The pointer is never null after `init()` and
    // remains valid for the duration of the reader's current
    // process() call (see the SPSC contract above).
    const Bank* current() const noexcept;

    // Writer-thread publish path. Copies `staged` into the currently
    // inactive buffer and then atomically publishes it as the new
    // current bank. Safe to call repeatedly; each call ping-pongs
    // between the two buffers. Must not be called concurrently from
    // multiple writer threads.
    //
    // `staged` is passed by const-ref rather than by value so the
    // writer can stage the fresh bank in its own scratch storage
    // (e.g. a parser-owned `Bank`) and hand it off without an
    // intermediate copy on the caller's stack. The memcpy into the
    // inactive slot is the only bulk copy involved.
    void swap_bank(const Bank& staged) noexcept;

    // Test-only introspection: which of the two internal buffers is
    // currently published. Returns 0 or 1 after `init()` has run.
    // Returns -1 in the pre-init state when `active_` is still null,
    // so a test that probes before `init()` gets an obviously wrong-
    // looking value instead of the silently misleading "1" that a
    // naive `nullptr != &buffers_[0]` ternary would return. The audio
    // thread does NOT need this helper — it only dereferences
    // `current()`. Exposed so the doctest suite can verify the
    // ping-pong invariant.
    int active_index_for_test() const noexcept;

private:
    // The two pre-allocated payload buffers. Laid out as a 2-element
    // array so `buffers_[0]` / `buffers_[1]` and the "active index"
    // test helper can share the same indexing scheme.
    Bank buffers_[2];

    // Atomic pointer to the current bank. Always equal to
    // `&buffers_[0]` or `&buffers_[1]` after `init()`.
    std::atomic<Bank*> active_;
};

// Compile-time contract: the payload we publish across the SPSC
// boundary is still the same trivially-copyable / standard-layout
// bank the sav-layer header promised. These are belt-and-braces — the
// sav header already carries the same assertions, but repeating them
// here pins the invariant at the exact translation unit that does the
// memcpy, so a future refactor that forgets to re-check the sav-layer
// constraints still fails loudly in rackboy_core.
static_assert(std::is_trivially_copyable<Bank>::value,
              "rackboy_core::Bank must stay trivially copyable for the "
              "SPSC swap_bank memcpy path.");
static_assert(std::is_standard_layout<Bank>::value,
              "rackboy_core::Bank must stay standard-layout so host "
              "tests and the Daisy target agree on the copied bytes.");

}  // namespace core
}  // namespace rackboy

#endif  // RACKBOY_CORE_RACKBOY_BANK_H_
