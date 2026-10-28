// Squizzy Studio app shell: owns the core (settings, class registry, host context, module loader),
// wires the in-process engine units (engine/doc DocumentManager, engine/edit actions + commands,
// engine/audio AudioEngine) into the host as services, opens the audio device chosen in Devices.*,
// and routes every command (menus, keys, remote, scripts) through ONE command system.
//
// The same class drives SquizzyStudio.exe and the headless end-to-end tests (tests/e2e).
// MAIN THREAD ONLY.
//
// Commands ("Category.Name"; args/result are IAttributes, both nullable):
//   File.New                                     new song, becomes active, engine plays it
//   File.Open        path                        open a .song archive
//   File.Save        path?                       save (path required for a new document)
//   File.Close
//   File.ExportAudio path tail?:double start?:double end?:double   mixdown via engine render +
//                                                codec chosen by the file extension (wav, flac, ...)
//   File.ExportStems dir ext?:string
//   File.ExportMidi  path                        SMF type 1 of every MIDI clip (codec-midifile)
//   File.ExportDawProject path / File.ImportDawProject path (new document)
//   Transport.Play / Stop / Record (toggles) / Rewind / ToggleLoop / ToggleMetronome
//   Edit.Undo / Edit.Redo                        + every engine/edit command (Edit.*, Navigate.*)
//   Track.AddAudio name? channels? / Track.AddInstrument name?    -> result "node"
//   Track.ToggleArm track? / Track.SetInput track? channel:int
//   Track.InsertPlugin track? plugin:"<format>:<id>" params?:"<id>=<value>;..."
//   Plugins.Rescan full?:bool                    (async; IPluginRegistry::isScanning)
//   Browser.IndexFolder folder / Browser.Search text? tags? kind? limit?  -> result "count", "first"
//   Script.Evaluate source / Script.LoadPackage path
//   Audio.ListDevices driver?:"wasapi|asio|alsa|pulse|null"  -> result "count", "devices" (id<TAB>name lines);
//                                                the message carries the ASIO status reason
//   Audio.SelectDevice driver device?            writes Devices.Audio.Driver/Device and reopens
//   UI.SetTheme name                             writes UI.Theme (the resolver follows the setting)
//   Surface.List                                 -> "count", "surfaces" (id<TAB>name<TAB>connected)
//   Surface.LoadMap path connection?             .device / .surface.xml -> result "surface"
//   Surface.LoadRemotePages surface skin? context?   default skin theme://current/remote/mixer.skin.xml
//   Surface.SelectPage surface page:int
//   Osc.Listen port?:int / Osc.Stop              inbound "/sq/<Category>/<Name> [value]" runs that
//                                                command on the main thread (Transport, Navigate,
//                                                Edit.Undo/Redo only); bind address defaults to
//                                                127.0.0.1 (Services.hardware-remote.OscBindAddress)
//   Osc.Send host port address value?
//   Sync.Link enabled?:bool                      ITempoSync (link-ableton; optional)
//   Account.Status / Account.SignIn / Account.SignOut   ICloudAccount (cloud-client; optional)
//   Video.Probe path                             IVideoEngine -> "width", "height", "duration_ns", "backend"
//   <any script command>                         from the scripting catalog (object://scripting)
// `track?` = args "track" (guid) or "track_index" (int); default: the last track.
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "sq/app/module_loader.hpp"
#include "sq/interfaces/audioengine.h"
#include "sq/interfaces/document.h"
#include "sq/interfaces/pluginhost.h"
#include "sq/interfaces/scripting.h"

namespace sq::engine::audio {
class AudioEngine;
}
namespace sq::ui {
class ThemeResolver;
}

namespace sq::app {

struct ShellOptions {
  std::string servicesDir;     // "" = <exe dir>/services
  bool loadServices = true;
  // Overrides Devices.Audio.Driver: "" (settings) | "none" | "null" | "offline" | "wasapi" | "asio" | "alsa" | "pulse" (Linux).
  std::string audioDriver;
  bool gui = false;            // "auto" driver choice: GUI opens WASAPI, batch modes open nothing
};

struct CommandInfo {
  std::string category, name, title;
  std::string source;  // "app" | "edit" | "script" | "class"
};

class Shell {
 public:
  Shell();
  ~Shell();
  Shell(const Shell&) = delete;
  Shell& operator=(const Shell&) = delete;

  Settings settings;
  ClassRegistry registry;
  HostContext host{settings, registry};
  ModuleLoader loader{host, registry};

  // Core defaults (Engine.*, Devices.*, Media.*, Platform.*, UI.*). start() calls it too.
  static void registerDefaults(Settings& s);

  sq_result start(const ShellOptions& opt);
  void stop();  // idempotent; the destructor calls it
  bool hasEngine() const;  // false in builds where engine/doc|edit|audio were filtered out
  const std::string& servicesDir() const { return servicesDir_; }

  // Main-loop pump: engine idle (retired graphs, bounce edits), script timers, autosave, control-
  // surface pump (ISurfacePump), queued OSC commands.
  void idle();

  // Services (borrowed; null when unavailable).
  IDocumentManager* documents() const;
  sq::Ptr<IDocument> activeDocument() const;
  IAudioEngine* engine() const;
  engine::audio::AudioEngine* audio() const;  // C++ view of the engine (tests, GUI meters)
  sq::Ptr<IScriptEngine> scripts() const;
  sq::Ptr<IPluginRegistry> plugins() const;
  // Theme resolver (stock themes embedded in the EXE, else <exe>/themes); registered with the host as
  // IThemeResolver + IRemotePageProvider (ui.h). Null before start() / after stop().
  sq::ui::ThemeResolver* themes() const;

  // Opens the device selected by Devices.Audio.Driver / Devices.Audio.Device (or `driver`).
  sq_result openAudioDevice(const std::string& driver = {});
  std::string deviceDescription() const;

  // ---- commands ----
  std::vector<CommandInfo> commands();
  sq_result execute(std::string_view category, std::string_view name, IAttributes* args = nullptr, IAttributes* result = nullptr);
  // "Category.Name" form.
  sq_result execute(std::string_view dotted, IAttributes* args = nullptr, IAttributes* result = nullptr);
  // SQ_OK enabled, SQ_COMMAND_DISABLED, SQ_E_NOTFOUND unknown.
  sq_result isEnabled(std::string_view category, std::string_view name, IAttributes* args = nullptr);
  const std::string& lastMessage() const { return message_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::string servicesDir_;
  std::string message_;
  bool started_ = false;
};

}  // namespace sq::app
