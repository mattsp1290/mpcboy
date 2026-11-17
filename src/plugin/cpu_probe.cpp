// src/plugin/cpu_probe.cpp - see cpu_probe.h. Empty in a release build.

#include "cpu_probe.h"

#ifdef MPCBOY_CPU_PROBE

#include <time.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "param_bridge.h"

namespace mpcboy {

namespace {
long long nowNs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}
}  // namespace

void CpuProbe::start()
{
    host_.setProgram(kProgramStack4);
    worker_ = std::thread([this] { workerLoop(); });
}

CpuProbe::~CpuProbe()
{
    stop_.store(true, std::memory_order_release);
    if (worker_.joinable()) worker_.join();
}

int CpuProbe::injectNote(EngineMidiEvent* midi)
{
    if (noteHeld_) return 0;
    midi[0].offset = 0; midi[0].status = 0x90; midi[0].data1 = 60; midi[0].data2 = 100;
    noteHeld_ = true;
    return 1;
}

long long CpuProbe::begin() const { return nowNs(); }

void CpuProbe::end(long long t0)
{
    const long long dt = nowNs() - t0;
    const uint32_t h = head_.load(std::memory_order_relaxed);
    ring_[h % kRing].store(static_cast<uint32_t>(dt > 0xFFFFFFFFLL ? 0xFFFFFFFFLL : dt), std::memory_order_relaxed);
    head_.store(h + 1, std::memory_order_release);
}

bool CpuProbe::format(int index, std::string& out) const
{
    if (index != P_DbgUs) return false;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "m%.0f p99 %.0f",
                  static_cast<double>(meanX10_.load(std::memory_order_relaxed)) / 10.0,
                  static_cast<double>(p99X10_.load(std::memory_order_relaxed)) / 10.0);
    out = buf;
    return true;
}

void CpuProbe::appendParam(std::vector<mpc::Param>& p)
{
    mpc::Param dbg = mpc::floatParam("dbg_us", "CPU us", 0.0f, 100000.0f, 0.0f, "us", 1);
    dbg.automatable = false;   // read-only: the text comes from format()
    p.push_back(dbg);
}

// Once per second, mean/p99 over the last <= 20,000 blocks (~58 s at 128
// frames). Calls requestDisplayUpdate() from here, never from process().
void CpuProbe::workerLoop()
{
    std::vector<uint32_t> snap;
    snap.reserve(kRing);
    while (!stop_.load(std::memory_order_acquire))
    {
        for (int i = 0; i < 20 && !stop_.load(std::memory_order_acquire); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const uint32_t head = head_.load(std::memory_order_acquire);
        const uint32_t n = std::min<uint32_t>(head, 20000);
        if (n < 16) continue;
        snap.clear();
        double sum = 0.0;
        for (uint32_t k = 0; k < n; ++k)
        {
            const uint32_t v = ring_[(head - 1 - k) % kRing].load(std::memory_order_relaxed);
            snap.push_back(v);
            sum += v;
        }
        std::sort(snap.begin(), snap.end());
        const double p99 = snap[static_cast<size_t>(0.99 * static_cast<double>(n - 1))];
        meanX10_.store(static_cast<uint32_t>(sum / n / 100.0), std::memory_order_relaxed);   // ns -> us * 10
        p99X10_.store(static_cast<uint32_t>(p99 / 100.0), std::memory_order_relaxed);
        host_.requestDisplayUpdate();
    }
}

}  // namespace mpcboy

#endif  // MPCBOY_CPU_PROBE
