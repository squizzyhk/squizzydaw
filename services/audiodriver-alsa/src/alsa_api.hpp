// audiodriver-alsa: the part of the ALSA PCM API this driver uses, loaded from libasound.so.2 at
// runtime (dlopen/dlsym). The build needs no ALSA development headers: the opaque handle types,
// the constants and the prototypes below are declared here from the stable libasound ABI.
#pragma once

#include <cstddef>
#include <string>

namespace sq::alsa {

// Opaque libasound objects.
struct snd_pcm_t;
struct snd_pcm_hw_params_t;
struct snd_pcm_sw_params_t;
using snd_pcm_uframes_t = unsigned long;
using snd_pcm_sframes_t = long;

// snd_pcm_stream_t
constexpr int kStreamPlayback = 0;
constexpr int kStreamCapture = 1;
// snd_pcm_open modes
constexpr int kModeNonblock = 0x1;
// snd_pcm_access_t
constexpr int kAccessRwInterleaved = 3;
// snd_pcm_format_t (little-endian variants)
constexpr int kFormatS16Le = 2;
constexpr int kFormatS24Le = 6;  // 24 bits in the low 3 bytes of a 32-bit container
constexpr int kFormatS32Le = 10;
constexpr int kFormatFloatLe = 14;
constexpr int kFormatS24_3Le = 32;  // packed 3 bytes
// snd_pcm_type_t values we care about
constexpr int kPcmTypeHw = 0;
constexpr int kPcmTypeNull = 4;
// snd_pcm_state_t
constexpr int kStateXrun = 4;

using ErrorHandler = void (*)(const char* file, int line, const char* function, int err, const char* fmt, ...);

// Function table. Every pointer is resolved when load() succeeds.
struct Api {
  const char* (*strerror)(int);
  int (*lib_error_set_handler)(ErrorHandler);
  int (*device_name_hint)(int card, const char* iface, void*** hints);
  char* (*device_name_get_hint)(const void* hint, const char* id);
  int (*device_name_free_hint)(void** hints);

  int (*pcm_open)(snd_pcm_t** pcm, const char* name, int stream, int mode);
  int (*pcm_close)(snd_pcm_t* pcm);
  int (*pcm_type)(snd_pcm_t* pcm);
  int (*pcm_nonblock)(snd_pcm_t* pcm, int nonblock);

  int (*hw_params_malloc)(snd_pcm_hw_params_t** p);
  void (*hw_params_free)(snd_pcm_hw_params_t* p);
  int (*hw_params_any)(snd_pcm_t* pcm, snd_pcm_hw_params_t* p);
  int (*hw_params_set_access)(snd_pcm_t* pcm, snd_pcm_hw_params_t* p, int access);
  int (*hw_params_test_format)(snd_pcm_t* pcm, snd_pcm_hw_params_t* p, int format);
  int (*hw_params_set_format)(snd_pcm_t* pcm, snd_pcm_hw_params_t* p, int format);
  int (*hw_params_get_channels_min)(const snd_pcm_hw_params_t* p, unsigned* val);
  int (*hw_params_get_channels_max)(const snd_pcm_hw_params_t* p, unsigned* val);
  int (*hw_params_set_channels)(snd_pcm_t* pcm, snd_pcm_hw_params_t* p, unsigned val);
  int (*hw_params_set_rate_resample)(snd_pcm_t* pcm, snd_pcm_hw_params_t* p, unsigned val);
  int (*hw_params_test_rate)(snd_pcm_t* pcm, snd_pcm_hw_params_t* p, unsigned val, int dir);
  int (*hw_params_set_rate_near)(snd_pcm_t* pcm, snd_pcm_hw_params_t* p, unsigned* val, int* dir);
  int (*hw_params_get_rate_min)(const snd_pcm_hw_params_t* p, unsigned* val, int* dir);
  int (*hw_params_get_rate_max)(const snd_pcm_hw_params_t* p, unsigned* val, int* dir);
  int (*hw_params_set_period_size_near)(snd_pcm_t* pcm, snd_pcm_hw_params_t* p, snd_pcm_uframes_t* val, int* dir);
  int (*hw_params_set_buffer_size_near)(snd_pcm_t* pcm, snd_pcm_hw_params_t* p, snd_pcm_uframes_t* val);
  int (*hw_params_get_period_size)(const snd_pcm_hw_params_t* p, snd_pcm_uframes_t* val, int* dir);
  int (*hw_params_get_buffer_size)(const snd_pcm_hw_params_t* p, snd_pcm_uframes_t* val);
  int (*hw_params_get_period_size_min)(const snd_pcm_hw_params_t* p, snd_pcm_uframes_t* val, int* dir);
  int (*hw_params_get_period_size_max)(const snd_pcm_hw_params_t* p, snd_pcm_uframes_t* val, int* dir);
  int (*hw_params)(snd_pcm_t* pcm, snd_pcm_hw_params_t* p);

  int (*sw_params_malloc)(snd_pcm_sw_params_t** p);
  void (*sw_params_free)(snd_pcm_sw_params_t* p);
  int (*sw_params_current)(snd_pcm_t* pcm, snd_pcm_sw_params_t* p);
  int (*sw_params_set_start_threshold)(snd_pcm_t* pcm, snd_pcm_sw_params_t* p, snd_pcm_uframes_t val);
  int (*sw_params_set_avail_min)(snd_pcm_t* pcm, snd_pcm_sw_params_t* p, snd_pcm_uframes_t val);
  int (*sw_params)(snd_pcm_t* pcm, snd_pcm_sw_params_t* p);

  int (*pcm_prepare)(snd_pcm_t* pcm);
  int (*pcm_start)(snd_pcm_t* pcm);
  int (*pcm_drop)(snd_pcm_t* pcm);
  int (*pcm_link)(snd_pcm_t* a, snd_pcm_t* b);
  int (*pcm_unlink)(snd_pcm_t* pcm);
  int (*pcm_state)(snd_pcm_t* pcm);
  int (*pcm_wait)(snd_pcm_t* pcm, int timeoutMs);
  int (*pcm_recover)(snd_pcm_t* pcm, int err, int silent);
  snd_pcm_sframes_t (*pcm_writei)(snd_pcm_t* pcm, const void* buffer, snd_pcm_uframes_t frames);
  snd_pcm_sframes_t (*pcm_readi)(snd_pcm_t* pcm, void* buffer, snd_pcm_uframes_t frames);
  snd_pcm_sframes_t (*pcm_avail_update)(snd_pcm_t* pcm);
};

// Loads libasound once per process (thread-safe). Returns nullptr when it cannot be loaded or a
// symbol is missing; `why` then explains (also on later calls).
const Api* load(std::string* why = nullptr);

// "ALSA error text" for a negative errno-style code (works without libasound too).
std::string errorText(int err);

// RAII holders for the parameter structs.
class HwParams {
 public:
  explicit HwParams(const Api* a) : a_(a) {
    if (a_->hw_params_malloc(&p_) < 0) p_ = nullptr;
  }
  ~HwParams() {
    if (p_) a_->hw_params_free(p_);
  }
  HwParams(const HwParams&) = delete;
  HwParams& operator=(const HwParams&) = delete;
  snd_pcm_hw_params_t* get() const { return p_; }

 private:
  const Api* a_;
  snd_pcm_hw_params_t* p_ = nullptr;
};

class SwParams {
 public:
  explicit SwParams(const Api* a) : a_(a) {
    if (a_->sw_params_malloc(&p_) < 0) p_ = nullptr;
  }
  ~SwParams() {
    if (p_) a_->sw_params_free(p_);
  }
  SwParams(const SwParams&) = delete;
  SwParams& operator=(const SwParams&) = delete;
  snd_pcm_sw_params_t* get() const { return p_; }

 private:
  const Api* a_;
  snd_pcm_sw_params_t* p_ = nullptr;
};

}  // namespace sq::alsa
