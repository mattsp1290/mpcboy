// MpcBoyPlugin.cpp - WP1 placeholder. WP7 replaces this with the real plugin.
//
// SPDX-License-Identifier: MIT
//
// One parameter (Bend Range at index 0, per decision D9) and a quiet 440 Hz
// sine while a note is held, so the shared library links and makes sound.

#include "mpc_plugin.h"

#include <cmath>

class MpcBoyPlugin : public mpc::Plugin
{
public:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "mpcboy";
        i.vendor = "mpcboy";
        i.uniqueId = MPC_FOURCC('M', 'b', 'G', 'b');   // must match "uid" in plugin/plugin.json
        i.version = 100;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    MpcBoyPlugin() : mpc::Plugin(makeInfo())
    {
        addParam(mpc::intParam("bendrng", "Bend Range", 0, 24, 2, "st"));
    }

    void reset() override
    {
        phase_ = 0.0;
        held_ = 0;
    }

    void process(mpc::ProcessContext& ctx) override
    {
        int pos = 0;
        for (int e = 0; e < ctx.numMidiEvents; ++e)
        {
            const mpc::MidiEvent& ev = ctx.midi[e];
            if (ev.sampleOffset > pos) { render(ctx, pos, ev.sampleOffset); pos = ev.sampleOffset; }
            if (ev.isNoteOn()) { if (held_ == 0) phase_ = 0.0; ++held_; }
            else if (ev.isNoteOff()) { if (held_ > 0) --held_; }
            else if (ev.isAllNotesOff()) held_ = 0;
        }
        render(ctx, pos, ctx.numFrames);
    }

private:
    void render(mpc::ProcessContext& ctx, int from, int to)
    {
        if (held_ == 0) return;
        const double inc = 440.0 / ctx.sampleRate;
        float* L = ctx.outputs[0];
        float* R = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        for (int i = from; i < to; ++i)
        {
            const float s = 0.1f * (float)std::sin(6.283185307179586 * phase_);
            L[i] = s;
            if (R) R[i] = s;
            phase_ += inc;
            if (phase_ >= 1.0) phase_ -= 1.0;
        }
    }

    double phase_ = 0.0;
    int held_ = 0;
};

MPC_PLUGIN_ENTRY(MpcBoyPlugin)
