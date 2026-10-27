// Squizzy Studio UI - backend-neutral accessibility tree. Widgets implement AccessibleNode; the
// platform bridge (UI Automation on Windows, see window_win32 / uia_provider) exposes it to
// screen readers. All calls happen on the UI (main) thread.
#pragma once

#include <cstdint>
#include <string>

#include "sq/ui/ui.hpp"

namespace sq::ui {

enum class AccessibleRole { Window, Group, Text, Button, ToggleButton, Slider, Image, Meter, Custom };

class AccessibleNode {
 public:
  virtual ~AccessibleNode() = default;
  // Stable id, unique within one tree (> 0). Used as the UIA runtime id.
  virtual uint32_t accessibleId() const = 0;
  virtual AccessibleRole accessibleRole() const = 0;
  virtual std::string accessibleName() const = 0;
  virtual std::string accessibleHelp() const { return {}; }
  // Bounds in client pixels of the hosting window.
  virtual Rect accessibleBounds() const = 0;
  virtual size_t accessibleChildCount() const = 0;
  virtual AccessibleNode* accessibleChild(size_t i) const = 0;
  virtual AccessibleNode* accessibleParent() const = 0;
  virtual bool accessibleEnabled() const { return true; }
  // Range values (knobs, faders, meters).
  virtual bool hasRangeValue() const { return false; }
  virtual double rangeValue() const { return 0; }
  virtual double rangeMin() const { return 0; }
  virtual double rangeMax() const { return 1; }
  virtual bool rangeReadOnly() const { return true; }
  virtual bool setRangeValue(double v) { (void)v; return false; }
  // Toggles (mute/solo buttons).
  virtual bool isToggle() const { return false; }
  virtual bool toggled() const { return false; }
  virtual bool toggle() { return false; }
  // Plain buttons.
  virtual bool invoke() { return false; }
};

// Depth-first lookup by id (nullptr if absent).
AccessibleNode* findAccessible(AccessibleNode* root, uint32_t id);

}  // namespace sq::ui
