// rackboy_core/rackboy_bank.cpp
//
// Implementation of the BankSlot SPSC handoff declared in
// rackboy_bank.h. Bead rackboy-2i8.
//
// The whole file is intentionally small: the SPSC contract lives in
// the header comment and the two memory-order choices below; the
// actual code is a memcpy (by value assignment of a trivially
// copyable struct) plus one atomic store.

#include "rackboy_bank.h"

#include <atomic>

#include "rackboy_sav_types.h"

namespace rackboy {
namespace core {

BankSlot::BankSlot() noexcept
    : buffers_{}, active_(nullptr)
{
    // Value-initialize both buffers to all-zero bytes. `bank_reset`
    // in init() will then walk them and set the documented
    // power-on-silent state; the zero-init here is just belt-and-
    // braces so a BankSlot that somehow ends up used before init()
    // at least reads as bytes 0x00 instead of uninitialized memory.
    //
    // active_ starts as nullptr so a caller that skipped init() and
    // dereferences `current()` crashes loudly instead of returning
    // garbage. init() fixes this up by publishing &buffers_[0].
}

void BankSlot::init() noexcept
{
    // Reset both payload buffers to the documented power-on silent
    // state. `bank_reset` zeroes every pool slot, clears all *_count
    // fields, marks every table/wave slot unoccupied, and sets
    // `loaded = false` — exactly what the audio thread expects when
    // no .sav has been loaded yet.
    ::rackboy::sav::bank_reset(buffers_[0]);
    ::rackboy::sav::bank_reset(buffers_[1]);

    // Publish buffer 0 as the current bank. Release ordering pairs
    // with the acquire-load in `current()`, so any reader that sees
    // this pointer also sees the bank_reset writes above. A plain
    // `store(p, memory_order_release)` is the strongest ordering we
    // need — we don't care about cross-thread ordering of *earlier*
    // writes on unrelated memory.
    active_.store(&buffers_[0], std::memory_order_release);
}

const Bank* BankSlot::current() const noexcept
{
    // Acquire load pairs with the release store in `init()` /
    // `swap_bank()`, so the reader sees every byte of the bank the
    // pointer addresses. This is the entire audio-thread hot path:
    // one atomic pointer load, no branches, no locks, no alloc.
    return active_.load(std::memory_order_acquire);
}

void BankSlot::swap_bank(const Bank& staged) noexcept
{
    // Relaxed load is enough to pick the inactive buffer: under the
    // SPSC contract there is exactly one writer, so this load can
    // only observe a value this same thread previously wrote (or
    // init()'s release store — which this thread's earlier init()
    // call sequenced before this). We do not need to synchronize
    // with the audio thread *here* — synchronization happens at the
    // final release store below.
    Bank* const current_active = active_.load(std::memory_order_relaxed);

    // Pick the "other" buffer. `current_active` is always either
    // &buffers_[0] or &buffers_[1] once init() has run, so this
    // ternary is exhaustive.
    Bank* const target =
        (current_active == &buffers_[0]) ? &buffers_[1] : &buffers_[0];

    // Copy the staged bank into the inactive buffer. `Bank` is
    // trivially copyable (static_asserts in rackboy_sav_types.h and
    // in rackboy_bank.h), so the compiler is free to lower this to a
    // memcpy. Doing the copy via plain assignment (rather than an
    // explicit std::memcpy call) keeps the code readable and lets
    // the compiler track field-level aliasing if it wants to.
    //
    // The inactive buffer is, by the SPSC contract, not being read
    // by the audio thread — it held the *previous* previous bank,
    // which the audio thread already moved past on its previous
    // process() call.
    *target = staged;

    // Release store publishes the new bank. Every byte written by
    // the assignment above happens-before any subsequent
    // acquire-load in `current()`, so the audio thread either sees
    // the *old* pointer (and reads the old bank in full) or the
    // *new* pointer (and reads the new bank in full) — never a mix.
    active_.store(target, std::memory_order_release);
}

int BankSlot::active_index_for_test() const noexcept
{
    // Test-only helper: return 0 or 1 depending on which internal
    // buffer is currently published, or -1 in the pre-init state
    // where `active_` is still null. Explicitly handling null keeps a
    // probe-before-init test from silently seeing "1" (which would
    // be the naive `nullptr != &buffers_[0]` ternary result and looks
    // like "buffer 1 is live" when really nothing is live yet). Uses
    // an acquire load for symmetry with `current()`, even though
    // doctest itself runs single-threaded and relaxed would also be
    // correct.
    const Bank* const active = active_.load(std::memory_order_acquire);
    if (active == nullptr) {
        return -1;
    }
    return (active == &buffers_[0]) ? 0 : 1;
}

}  // namespace core
}  // namespace rackboy
