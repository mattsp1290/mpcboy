#include "import_triggers.h"

#include <chrono>

namespace mpcboy {

namespace {

constexpr long long kTriggerDebounceNs = 250LL * 1000 * 1000;

long long steadyNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

ImportTriggers::ImportTriggers() { now_.store(&steadyNs, std::memory_order_relaxed); }

void ImportTriggers::setClock(NowFn fn) { now_.store(fn ? fn : &steadyNs, std::memory_order_release); }

void ImportTriggers::onValue(int k, float plain, bool onAudioThread)
{
    Slot& s = slots_[k];
    if (plain < 0.5f)
    {
        s.high.store(false, std::memory_order_release);
        return;
    }
    if (s.high.exchange(true, std::memory_order_acq_rel)) return;   // already high: not an edge
    s.reset.store(true, std::memory_order_release);
    if (!onAudioThread) return;

    const long long now = now_.load(std::memory_order_acquire)();
    const long long last = s.lastNs.load(std::memory_order_acquire);
    if (last != 0 && now - last < kTriggerDebounceNs) return;
    s.lastNs.store(now, std::memory_order_release);
    s.pending.store(true, std::memory_order_release);   // acted on by process()
}

void ImportTriggers::cancelPending()
{
    for (Slot& s : slots_) s.pending.store(false, std::memory_order_release);
}

}  // namespace mpcboy
