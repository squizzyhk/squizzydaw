// audiodriver-alsa: runtime binding of libasound.so.2 (see alsa_api.hpp).
#include "alsa_api.hpp"

#include <dlfcn.h>

#include <cstring>
#include <mutex>

namespace sq::alsa {
namespace {

struct Loaded {
  Api api{};
  bool ok = false;
  std::string why;
};

template <class F>
bool bind(void* lib, const char* name, F& fn, std::string& missing) {
  void* p = dlsym(lib, name);
  if (!p) {
    if (missing.empty()) missing = name;
    return false;
  }
  fn = reinterpret_cast<F>(p);
  return true;
}

Loaded& loadOnce() {
  static Loaded L;
  static std::once_flag once;
  std::call_once(once, [] {
    // libasound is never unloaded: it keeps process-wide configuration caches and may have
    // handed out state to other modules of this process.
    void* lib = nullptr;
    for (const char* name : {"libasound.so.2", "libasound.so"}) {
      lib = dlopen(name, RTLD_NOW | RTLD_LOCAL);
      if (lib) break;
    }
    if (!lib) {
      const char* e = dlerror();
      L.why = std::string("libasound.so.2 not found (") + (e ? e : "dlopen failed") + "); install the ALSA runtime library";
      return;
    }
    std::string missing;
    Api& a = L.api;
    bool ok = true;
    ok &= bind(lib, "snd_strerror", a.strerror, missing);
    ok &= bind(lib, "snd_lib_error_set_handler", a.lib_error_set_handler, missing);
    ok &= bind(lib, "snd_device_name_hint", a.device_name_hint, missing);
    ok &= bind(lib, "snd_device_name_get_hint", a.device_name_get_hint, missing);
    ok &= bind(lib, "snd_device_name_free_hint", a.device_name_free_hint, missing);
    ok &= bind(lib, "snd_pcm_open", a.pcm_open, missing);
    ok &= bind(lib, "snd_pcm_close", a.pcm_close, missing);
    ok &= bind(lib, "snd_pcm_type", a.pcm_type, missing);
    ok &= bind(lib, "snd_pcm_nonblock", a.pcm_nonblock, missing);
    ok &= bind(lib, "snd_pcm_hw_params_malloc", a.hw_params_malloc, missing);
    ok &= bind(lib, "snd_pcm_hw_params_free", a.hw_params_free, missing);
    ok &= bind(lib, "snd_pcm_hw_params_any", a.hw_params_any, missing);
    ok &= bind(lib, "snd_pcm_hw_params_set_access", a.hw_params_set_access, missing);
    ok &= bind(lib, "snd_pcm_hw_params_test_format", a.hw_params_test_format, missing);
    ok &= bind(lib, "snd_pcm_hw_params_set_format", a.hw_params_set_format, missing);
    ok &= bind(lib, "snd_pcm_hw_params_get_channels_min", a.hw_params_get_channels_min, missing);
    ok &= bind(lib, "snd_pcm_hw_params_get_channels_max", a.hw_params_get_channels_max, missing);
    ok &= bind(lib, "snd_pcm_hw_params_set_channels", a.hw_params_set_channels, missing);
    ok &= bind(lib, "snd_pcm_hw_params_set_rate_resample", a.hw_params_set_rate_resample, missing);
    ok &= bind(lib, "snd_pcm_hw_params_test_rate", a.hw_params_test_rate, missing);
    ok &= bind(lib, "snd_pcm_hw_params_set_rate_near", a.hw_params_set_rate_near, missing);
    ok &= bind(lib, "snd_pcm_hw_params_get_rate_min", a.hw_params_get_rate_min, missing);
    ok &= bind(lib, "snd_pcm_hw_params_get_rate_max", a.hw_params_get_rate_max, missing);
    ok &= bind(lib, "snd_pcm_hw_params_set_period_size_near", a.hw_params_set_period_size_near, missing);
    ok &= bind(lib, "snd_pcm_hw_params_set_buffer_size_near", a.hw_params_set_buffer_size_near, missing);
    ok &= bind(lib, "snd_pcm_hw_params_get_period_size", a.hw_params_get_period_size, missing);
    ok &= bind(lib, "snd_pcm_hw_params_get_buffer_size", a.hw_params_get_buffer_size, missing);
    ok &= bind(lib, "snd_pcm_hw_params_get_period_size_min", a.hw_params_get_period_size_min, missing);
    ok &= bind(lib, "snd_pcm_hw_params_get_period_size_max", a.hw_params_get_period_size_max, missing);
    ok &= bind(lib, "snd_pcm_hw_params", a.hw_params, missing);
    ok &= bind(lib, "snd_pcm_sw_params_malloc", a.sw_params_malloc, missing);
    ok &= bind(lib, "snd_pcm_sw_params_free", a.sw_params_free, missing);
    ok &= bind(lib, "snd_pcm_sw_params_current", a.sw_params_current, missing);
    ok &= bind(lib, "snd_pcm_sw_params_set_start_threshold", a.sw_params_set_start_threshold, missing);
    ok &= bind(lib, "snd_pcm_sw_params_set_avail_min", a.sw_params_set_avail_min, missing);
    ok &= bind(lib, "snd_pcm_sw_params", a.sw_params, missing);
    ok &= bind(lib, "snd_pcm_prepare", a.pcm_prepare, missing);
    ok &= bind(lib, "snd_pcm_start", a.pcm_start, missing);
    ok &= bind(lib, "snd_pcm_drop", a.pcm_drop, missing);
    ok &= bind(lib, "snd_pcm_link", a.pcm_link, missing);
    ok &= bind(lib, "snd_pcm_unlink", a.pcm_unlink, missing);
    ok &= bind(lib, "snd_pcm_state", a.pcm_state, missing);
    ok &= bind(lib, "snd_pcm_wait", a.pcm_wait, missing);
    ok &= bind(lib, "snd_pcm_recover", a.pcm_recover, missing);
    ok &= bind(lib, "snd_pcm_writei", a.pcm_writei, missing);
    ok &= bind(lib, "snd_pcm_readi", a.pcm_readi, missing);
    ok &= bind(lib, "snd_pcm_avail_update", a.pcm_avail_update, missing);
    if (!ok) {
      L.why = "libasound.so.2 lacks symbol " + missing + " (too old?)";
      return;
    }
    L.ok = true;
  });
  return L;
}

}  // namespace

const Api* load(std::string* why) {
  Loaded& L = loadOnce();
  if (why) *why = L.why;
  return L.ok ? &L.api : nullptr;
}

std::string errorText(int err) {
  const Api* a = load();
  if (a) {
    const char* s = a->strerror(err);
    if (s) return s;
  }
  return std::strerror(err < 0 ? -err : err);
}

}  // namespace sq::alsa
