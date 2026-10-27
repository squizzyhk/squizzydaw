// Squizzy Studio - deliberately misbehaving CLAP test plugins (original code, test-only).
// Built twice from this file:
//   SQ_MISBEHAVE_CRASH: clap_entry.init dereferences a null pointer (access violation / SIGSEGV).
//   SQ_MISBEHAVE_HANG : clap_entry.init never returns.
// They prove the out-of-process scanner survives, quarantines (crash) or times out (hang).
// Never load these in the host process.
#include <clap/clap.h>

#include <cstring>
#include <thread>
#include <chrono>

namespace {

#if defined(SQ_MISBEHAVE_CRASH)
bool eInit(const char*) {
  volatile int* p = nullptr;
  *p = 42;  // deliberate crash
  return *p == 42;
}
#elif defined(SQ_MISBEHAVE_HANG)
bool eInit(const char*) {
  for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}
#else
#error "define SQ_MISBEHAVE_CRASH or SQ_MISBEHAVE_HANG"
#endif

void eDeinit() {}
const void* eGetFactory(const char*) { return nullptr; }

}  // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, eInit, eDeinit, eGetFactory};
