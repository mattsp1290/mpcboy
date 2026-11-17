// tools/render_editor.mm - renders the Mac editor offscreen, one PNG per page, so the look can
// be checked without a DAW (mac-vst build only; build/mac-vst/render_editor).
//
//   render_editor <art dir> <out dir> [scale ...]
//
// Writes <out dir>/<page>.png (scale 1) and <page>@<scale>x.png for each further scale, using
// the editor's own drawing code (mpcboy::renderEditorPng) on a real MpcBoyPlugin at its
// defaults after a few process() blocks (the TABLE grid shows the factory content). Exit 0 when
// every page rendered with all art found.
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
        std::vector<float> l(512), r(512);
        float* outs[2] = {l.data(), r.data()};
        for (int b = 0; b < 4; ++b) plugin.internalProcess(nullptr, outs, 512);

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
                const bool ok = mpcboy::renderEditorPng(plugin, p, s, art, path);
                std::printf("%s %s\n", ok ? "wrote" : "FAILED", path.c_str());
                if (!ok) ++failures;
            }
        }
        return failures == 0 ? 0 : 1;
    }
}
