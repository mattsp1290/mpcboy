// plugin_entry_mac.cpp - the macOS VST2 entry point (mac-vst build only): MpcBoyPlugin plus the
// Game Boy editor window. Replaces plugin_entry.cpp's MPC_PLUGIN_ENTRY there; the MPC and
// native builds keep that one unchanged (no editor, effFlagsHasEditor clear).
//
// It follows MPC_PLUGIN_ENTRY_BODY (VST version check, createEffect) and then sets
// effFlagsHasEditor and installs a dispatcher that handles the editor opcodes and forwards the
// rest to the SDK's (see .agents/plans/mpcboy-gb-ui/00-design.md "Dispatcher wrap"):
//   effEditGetRect  *(ERect**)ptr = a member ERect {0, 0, 704, 1280}; the same before and after open
//   effEditOpen     ptr = the host's NSView*; adds the editor view (idempotent)
//   effEditClose    removes it; safe when nothing is open
//   effEditIdle     nothing (the editor has its own 30 Hz timer)
//   effClose        tears the editor down first, then forwards (the SDK deletes the plugin and
//                   frees the AEffect; nothing is touched afterwards)
// Both VSTPluginMain and main_macho are exported (cmake/mac_exports.txt).
#include "MpcBoyPlugin.h"
#include "mac_editor.h"
#include "ui_layout.h"

namespace {

class MpcBoyMacPlugin : public MpcBoyPlugin
{
public:
    MpcBoyMacPlugin() : editor_(*this) {}
    ~MpcBoyMacPlugin() override { editor_.close(); }

    ERect rect_{0, 0, static_cast<VstInt16>(mpcboy::ui::kEditorHeight), static_cast<VstInt16>(mpcboy::ui::kEditorWidth)};
    mpcboy::MacEditor editor_;
};

MpcBoyMacPlugin* macPlugin(AEffect* e)
{
    if (e == nullptr || e->object == nullptr) return nullptr;
    return static_cast<MpcBoyMacPlugin*>(static_cast<mpc::Plugin*>(e->object));
}

VstIntPtr macDispatcher(AEffect* e, VstInt32 opcode, VstInt32 index, VstIntPtr value, void* ptr, float opt)
{
    MpcBoyMacPlugin* p = macPlugin(e);
    if (p == nullptr) return 0;
    switch (opcode)
    {
        case effEditGetRect:
            if (ptr == nullptr) return 0;
            *static_cast<ERect**>(ptr) = &p->rect_;
            return 1;
        case effEditOpen:
            return p->editor_.open(ptr) ? 1 : 0;
        case effEditClose:
            p->editor_.close();
            return 1;
        case effEditIdle:
            return 0;
        case effClose:
            p->editor_.close();
            return mpc::detail::dispatcher(e, opcode, index, value, ptr, opt);   // deletes p, frees e
        default:
            return mpc::detail::dispatcher(e, opcode, index, value, ptr, opt);
    }
}

}  // namespace

extern "C" MPC_EXPORT AEffect* VSTPluginMain(audioMasterCallback host)
{
    if (host != nullptr && host(nullptr, audioMasterVersion, 0, 0, nullptr, 0.0f) == 0) return nullptr;   // VST1 only
    AEffect* e = mpc::detail::createEffect(new MpcBoyMacPlugin(), host);
    if (e == nullptr) return nullptr;
    e->flags |= effFlagsHasEditor;
    e->dispatcher = &macDispatcher;
    return e;
}

extern "C" MPC_EXPORT AEffect* main_macho(audioMasterCallback host) { return VSTPluginMain(host); }
