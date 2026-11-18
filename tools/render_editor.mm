// tools/render_editor.mm - renders the Mac editor offscreen, one PNG per page, so the look can
// be checked without a DAW (mac-vst build only; build/mac-vst/render_editor).
//
//   render_editor <art dir> <out dir> [scale ...]
//
// Writes <out dir>/<page>.png (scale 1) and <page>@<scale>x.png for each further scale, using
// the editor's own drawing code (mpcboy::renderEditorPng) on a real MpcBoyPlugin. Playback is
// simulated so the live LCDs show something: PU1 and WAV (switched on) play table 1 (MajArp) from a C-4
// note-on until the playhead reaches row 2; the TABLE page edits table 1 with the cursor on
// row 2. Exit 0 when every page rendered with all art found and the playhead got there.
#import <Foundation/Foundation.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "MpcBoyPlugin.h"
#include "mac_editor.h"
#include "ui_layout.h"

int main(int argc, char** argv)
{
    @autoreleasepool
    {
        if (argc < 3)
        {
            std::fprintf(stderr, "usage: %s <art dir> <out dir> [scale ...]\n", argv[0]);
            return 2;
        }
        const std::string art = argv[1], out = argv[2];
        std::vector<double> scales = {1.0};
        for (int i = 3; i < argc; ++i) scales.push_back(std::atof(argv[i]));
        [[NSFileManager defaultManager] createDirectoryAtPath:[NSString stringWithUTF8String:out.c_str()]
                                  withIntermediateDirectories:YES
                                                   attributes:nil
                                                        error:nil];
        MpcBoyPlugin plugin;
        plugin.prepare(44100.0, 512);
        plugin.set(mpcboy::P1_Tbl, 1.0f);   // MajArp
        plugin.set(mpcboy::WV_On, 1.0f);
        plugin.set(mpcboy::WV_Tbl, 1.0f);
        plugin.set(mpcboy::ED_Table, 1.0f);
        plugin.set(mpcboy::ED_Row, 2.0f);
        constexpr int kBlock = 64;
        std::vector<float> l(kBlock), r(kBlock);
        float* outs[2] = {l.data(), r.data()};
        long long pos = 0;
        bool reached = false;
        for (int b = 0; b < 2000 && !reached; ++b)
        {
            mpc::MidiEvent on = mpc::MidiEvent::noteOn(0, 60, 100, 0);
            mpc::ProcessContext ctx;
            ctx.outputs = outs;
            ctx.numOutputs = 2;
            ctx.numFrames = kBlock;
            ctx.midi = b == 8 ? &on : nullptr;   // a few blocks in: the content is live by then
            ctx.numMidiEvents = b == 8 ? 1 : 0;
            ctx.sampleRate = 44100.0;
            ctx.transport.valid = true;
            ctx.transport.bpm = 120.0;
            ctx.transport.tempoValid = true;
            ctx.transport.playing = true;
            ctx.transport.ppqValid = true;
            ctx.transport.ppqPos = static_cast<double>(pos) / 44100.0 * 2.0;
            plugin.process(ctx);
            pos += kBlock;
            const mpcboy::ChannelPlayback pb = plugin.playback().load(0);
            reached = b > 8 && pb.running && pb.row == 2;
        }
        if (!reached) std::fprintf(stderr, "render_editor: the PU1 playhead never reached row 2\n");

        // MPCBOY_RENDER_TILE=N: draw in N x N dirty-rect tiles (checks the view's partial redraw).
        const char* tileEnv = std::getenv("MPCBOY_RENDER_TILE");
        const int tile = tileEnv ? std::atoi(tileEnv) : 0;
        int failures = 0;
        for (int p = 0; p < mpcboy::ui::kNumPages; ++p)
        {
            std::string name = mpcboy::ui::kPages[p].name;
            for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (double s : scales)
            {
                char suffix[32] = "";
                if (s != 1.0) std::snprintf(suffix, sizeof(suffix), "@%gx", s);
                const std::string path = out + "/" + name + suffix + ".png";
                const bool ok = mpcboy::renderEditorPng(plugin, p, s, art, path, tile);
                std::printf("%s %s\n", ok ? "wrote" : "FAILED", path.c_str());
                if (!ok) ++failures;
            }
        }
        return failures == 0 && reached ? 0 : 1;
    }
}
