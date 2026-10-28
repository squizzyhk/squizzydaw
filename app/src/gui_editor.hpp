// Squizzy Studio GUI - clip editor internals shared between gui_editor*.cpp.
#pragma once

#include <memory>

#include "gui_context.hpp"

namespace sq::app::gui {

// Inspector shown by the clip editor for audio clips (name, gain, fades, source, offset). It edits
// through kSetProperty (gainDb / fadeIn / fadeOut / sourceOffset / name / muted).
class AudioInspector {
 public:
  virtual ~AudioInspector() = default;
  virtual void reset() = 0;  // another clip was opened
  virtual void paint(Canvas& c, const Rect& area, const ClipView& clip, Color trackColor) = 0;
  // Hover / Down / Move / Up / DoubleClick inside the editor. True if it changed something visible.
  virtual bool mouse(const WidgetMouse& e, const ClipView& clip) = 0;
};
std::unique_ptr<AudioInspector> makeAudioInspector(GuiContext& ctx);  // gui_editor_audio.cpp

}  // namespace sq::app::gui
