// src/plugin/cpu_probe.h - the debug-only CPU probe (see docs/BENCH.md).
//
// With -DMPCBOY_CPU_PROBE=ON the plugin holds a Stack4 note by itself, times
// every process() call and shows mean/p99 in microseconds as the display text
// of the read-only parameter `dbg_us` (appended last). All of that lives in
// this class; the plugin calls it unconditionally. In a release build the
// class has empty inline bodies, so no #ifdef appears outside the probe files.
#pragma once

#include <string>
#include <vector>

#include "engine.h"
#include "mpc_plugin.h"

#ifdef MPCBOY_CPU_PROBE
#include <atomic>
#include <thread>
#endif

namespace mpcboy {

#ifdef MPCBOY_CPU_PROBE

class CpuProbe
{
public:
    explicit CpuProbe(mpc::Plugin& host) : host_(host) {}
    ~CpuProbe();
    CpuProbe(const CpuProbe&) = delete;
    CpuProbe& operator=(const CpuProbe&) = delete;

    // Selects the Stack4 program and starts the statistics thread. Call at the
    // end of the plugin constructor (after the programs were added).
    void start();
    // Forget the held note (the engine was reset).
    void onReset() { noteHeld_ = false; }
    // Writes the self-held note-on into `midi` once; returns the number of events written (0 or 1).
    int injectNote(EngineMidiEvent* midi);
    // process() timing: begin() returns a timestamp, end() records the duration.
    long long begin() const;
    void end(long long t0);
    // Display text of the dbg_us parameter.
    bool format(int index, std::string& out) const;
    // Appends the dbg_us parameter to the parameter table.
    static void appendParam(std::vector<mpc::Param>& p);

private:
    void workerLoop();
    static constexpr uint32_t kRing = 32768;   // ~95 s of 128-frame blocks

    mpc::Plugin& host_;
    std::atomic<uint32_t> ring_[kRing] = {};
    std::atomic<uint32_t> head_{0};
    std::atomic<uint32_t> meanX10_{0};
    std::atomic<uint32_t> p99X10_{0};
    std::atomic<bool> stop_{false};
    std::thread worker_;
    bool noteHeld_ = false;   // audio thread only
};

#else

class CpuProbe
{
public:
    explicit CpuProbe(mpc::Plugin&) {}
    void start() {}
    void onReset() {}
    int injectNote(EngineMidiEvent*) { return 0; }
    long long begin() const { return 0; }
    void end(long long) {}
    bool format(int, std::string&) const { return false; }
    static void appendParam(std::vector<mpc::Param>&) {}
};

#endif

}  // namespace mpcboy
