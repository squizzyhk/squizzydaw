// midi-alsa: runtime binding of the ALSA sequencer functions (see seq_api.hpp).
#include "seq_api.hpp"

#include <dlfcn.h>

#include <mutex>

namespace sq::alsaseq {
namespace {

template <class F>
void bind(void* lib, const char* name, F& fn, std::string& missing) {
  void* p = dlsym(lib, name);
  if (!p && missing.empty()) missing = name;
  fn = reinterpret_cast<F>(p);
}

}  // namespace

const SeqApi* loadSeq(std::string* why) {
  static SeqApi api{};
  static bool ok = false;
  static std::string reason;
  static std::once_flag once;
  std::call_once(once, [] {
    void* lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);  // never unloaded
    if (!lib) lib = dlopen("libasound.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
      const char* e = dlerror();
      reason = std::string("libasound.so.2 not found (") + (e ? e : "dlopen failed") + ")";
      return;
    }
    std::string m;
    SeqApi& a = api;
    bind(lib, "snd_strerror", a.strerror, m);
    bind(lib, "snd_lib_error_set_handler", a.lib_error_set_handler, m);
    bind(lib, "snd_seq_open", a.open, m);
    bind(lib, "snd_seq_close", a.close, m);
    bind(lib, "snd_seq_set_client_name", a.set_client_name, m);
    bind(lib, "snd_seq_client_id", a.client_id, m);
    bind(lib, "snd_seq_create_simple_port", a.create_simple_port, m);
    bind(lib, "snd_seq_delete_simple_port", a.delete_simple_port, m);
    bind(lib, "snd_seq_connect_from", a.connect_from, m);
    bind(lib, "snd_seq_disconnect_from", a.disconnect_from, m);
    bind(lib, "snd_seq_poll_descriptors_count", a.poll_descriptors_count, m);
    bind(lib, "snd_seq_poll_descriptors", a.poll_descriptors, m);
    bind(lib, "snd_seq_event_input", a.event_input, m);
    bind(lib, "snd_seq_event_input_pending", a.event_input_pending, m);
    bind(lib, "snd_seq_event_output_direct", a.event_output_direct, m);
    bind(lib, "snd_seq_client_info_malloc", a.client_info_malloc, m);
    bind(lib, "snd_seq_client_info_free", a.client_info_free, m);
    bind(lib, "snd_seq_client_info_set_client", a.client_info_set_client, m);
    bind(lib, "snd_seq_client_info_get_client", a.client_info_get_client, m);
    bind(lib, "snd_seq_client_info_get_name", a.client_info_get_name, m);
    bind(lib, "snd_seq_query_next_client", a.query_next_client, m);
    bind(lib, "snd_seq_port_info_malloc", a.port_info_malloc, m);
    bind(lib, "snd_seq_port_info_free", a.port_info_free, m);
    bind(lib, "snd_seq_port_info_set_client", a.port_info_set_client, m);
    bind(lib, "snd_seq_port_info_set_port", a.port_info_set_port, m);
    bind(lib, "snd_seq_port_info_get_port", a.port_info_get_port, m);
    bind(lib, "snd_seq_port_info_get_capability", a.port_info_get_capability, m);
    bind(lib, "snd_seq_port_info_get_type", a.port_info_get_type, m);
    bind(lib, "snd_seq_port_info_get_name", a.port_info_get_name, m);
    bind(lib, "snd_seq_query_next_port", a.query_next_port, m);
    if (!m.empty()) {
      reason = "libasound.so.2 lacks symbol " + m;
      return;
    }
    ok = true;
  });
  if (why) *why = reason;
  return ok ? &api : nullptr;
}

}  // namespace sq::alsaseq
