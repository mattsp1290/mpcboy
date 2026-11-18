// src/plugin/mac_editor.h - the Game Boy editor window of the macOS VST2 build (UI WP4).
//
// mac-vst build only (src/plugin/mac_editor.mm, Objective-C++ with ARC). The plain C++
// interface lets plugin_entry_mac.cpp own the editor without Cocoa headers.
//
// Main thread only. The editor draws the shared layout (src/ui/ui_layout.h) and art
// (ui/art, in the bundle at Contents/Resources/ui) one point per design unit: 1280 x 704
// (the 628-unit page plus the 76-unit tab bar). All interaction goes through the platform-free
// model in src/ui/ui_model.h and the plugin's editor edit API (MpcBoyPlugin::editorBeginEdit /
// editorSet / editorEndEdit). See .agents/plans/mpcboy-gb-ui/00-design.md "Mac editor".
#pragma once

#include <string>

class MpcBoyPlugin;

namespace mpcboy {

class MacEditor
{
public:
    explicit MacEditor(MpcBoyPlugin& plugin);
    ~MacEditor();   // closes
    MacEditor(const MacEditor&) = delete;
    MacEditor& operator=(const MacEditor&) = delete;

    // effEditOpen: `parentView` is the host's NSView*. Adds the editor view (idempotent: a
    // second open with the same parent does nothing; a different parent moves the view).
    // Loads the art on first use. Returns false for a null parent.
    bool open(void* parentView);
    // effEditClose / effClose / destructor: ends any open edit, stops the timer, removes the
    // view. Safe when nothing is open.
    void close();
    bool isOpen() const;

    // Art folder override (tests and tools); empty = mpc::pluginFolder() + "/ui/".
    void setArtDirectory(const std::string& dir) { artDir_ = dir; }

private:
    struct Impl;
    MpcBoyPlugin& plugin_;
    std::string artDir_;
    Impl* impl_ = nullptr;
};

// Offscreen render of the editor for tools and tests (no window): draws page `page` (0-based)
// at `scale` device pixels per point into a bitmap and writes it as a PNG. Returns false on
// any failure. Uses the same drawing code as the editor view. `tile` > 0 draws it the way the view
// does after partial invalidation instead: tile x tile design-unit pieces, each clipped to its
// rect and drawn with that rect as the dirty rect; the result must equal the untiled render
// (a check of the dirty-rect culling, tools/render_editor.mm MPCBOY_RENDER_TILE).
bool renderEditorPng(MpcBoyPlugin& plugin, int page, double scale, const std::string& artDir,
                     const std::string& pngPath, int tile = 0);

}  // namespace mpcboy
