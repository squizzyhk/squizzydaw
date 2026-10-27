// Squizzy Studio UI - UI Automation bridge (Windows) for the backend-neutral accessibility tree
// (accessibility.hpp). The Win32 window owns one and answers WM_GETOBJECT with it, so screen
// readers see every widget with its name, role (UIA control type), help text, bounds, enabled
// state and the RangeValue / Toggle / Invoke patterns.
//
// Providers never hold AccessibleNode pointers: each one remembers the node's accessibleId() and
// looks it up from the current root on every call, so a rebuilt or dropped tree yields
// UIA_E_ELEMENTNOTAVAILABLE instead of a dangling pointer. The bridge lives on the UI thread.
// UIA may call providers on any thread (an in-process client calls straight through), so with a
// window every provider method runs its work on the UI thread: directly when already there, else
// synchronously through a message-only window the bridge owns (SendMessageTimeout, 10 s). The
// UI thread must keep pumping messages; once the window is gone calls report "not available".
// On other platforms the bridge compiles to a no-op (rootProvider() returns nullptr).
#pragma once

#include <cstdint>
#include <memory>

#include "sq/ui/accessibility.hpp"

namespace sq::ui {

class UiaBridge {
 public:
  // hwnd: HWND hosting the tree (nullptr = no window, e.g. tests; bounds stay in client pixels).
  explicit UiaBridge(void* hwnd);
  ~UiaBridge();  // disconnects: providers handed out earlier report "element not available"
  UiaBridge(const UiaBridge&) = delete;
  UiaBridge& operator=(const UiaBridge&) = delete;

  void setRoot(AccessibleNode* root);  // borrowed; nullptr before the tree dies
  AccessibleNode* root() const;

  // Add-ref'd IRawElementProviderSimple* (as void*) for the root, nullptr without a root or
  // off Windows. Release it with the COM Release().
  void* rootProvider();
  // Same for any node of the current tree (nullptr if the id is not in it).
  void* providerFor(uint32_t accessibleId);

  // WM_GETOBJECT: returns the LRESULT to give back, or 0 with handled=false.
  intptr_t handleGetObject(uintptr_t wparam, intptr_t lparam, bool& handled);
  // Raises structure/property change events for assistive tech (cheap when nobody listens).
  void notifyChanged(AccessibleNode* node);
  // WM_DESTROY: tells UIA the window's providers are gone.
  void windowDestroyed();

  struct State;  // internal, shared with the providers

 private:
  std::shared_ptr<State> state_;
};

}  // namespace sq::ui
