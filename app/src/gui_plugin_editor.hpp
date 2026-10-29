// Squizzy Studio GUI - native plugin editor windows. One top-level window per open editor, owned
// by the main window (stays above it, minimizes with it), hosting the plugin's own view through
// IPluginInstance::openEditor(hwnd, "win32"). The host follows the engine: when the graph is
// recompiled (new instance behind the same insert) the editor is re-opened on the new instance in
// the same window; when the insert vanishes the window closes. Closing a window writes the
// plugin's live state into the song (Shell::capturePluginStates) so it survives save / reload.
//
// Only Windows has an implementation; elsewhere open() explains that and returns false. Nothing
// here creates a window unless open() is called, so the headless GUI (--screenshot / --ui-script)
// never touches the platform. MAIN THREAD ONLY.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "gui_context.hpp"

namespace sq::app::gui {

class PluginEditorHost {
 public:
  PluginEditorHost(GuiContext& ctx, Shell& shell);
  ~PluginEditorHost();
  PluginEditorHost(const PluginEditorHost&) = delete;
  PluginEditorHost& operator=(const PluginEditorHost&) = delete;

  // Opens (or brings to front) the editor of the plugin behind (node, slot): node = track / bus
  // guid, null guid = the master track; slot = "user.inserts" line index, -1 = an instrument
  // track's instrument. nodeTitle is the node's display name ("Master", the track name); the
  // window title becomes "<nodeTitle> - <plugin name>". ownerHwnd = the main window's native
  // handle. Returns false (with the reason on the status line) when that insert is not a running
  // plugin, the plugin has no editor, or the window could not be created.
  bool open(const sq_guid& node, int32_t slot, const std::string& nodeTitle, void* ownerHwnd);
  // Closes every editor window without touching the song (document switch, shutdown).
  void closeAll();
  // Main-loop pump: follows graph recompiles / removed inserts (cheap unless the engine's compile
  // count or the document serial changed) and forgets windows the user closed.
  void idle();
  // IPluginHostCallback::onEditorResize relay (Shell::PluginHostHooks): resizes the window of
  // `inst` so its client area is w x h. false when no window hosts that instance.
  bool onEditorResize(IPluginInstance* inst, uint32_t w, uint32_t h);
  // Number of editor windows currently open.
  size_t count() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sq::app::gui
