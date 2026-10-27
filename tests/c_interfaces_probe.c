/* Compiled as C: every area interface header must be valid C for its POD/constant part. */
#include "sq/interfaces/common.h"
#include "sq/interfaces/audiodevice.h"
#include "sq/interfaces/midi.h"
#include "sq/interfaces/codec.h"
#include "sq/interfaces/document.h"
#include "sq/interfaces/audioengine.h"
#include "sq/interfaces/pluginhost.h"
#include "sq/interfaces/index.h"
#include "sq/interfaces/scripting.h"
#include "sq/interfaces/dsp.h"
#include "sq/interfaces/ui.h"
#include "sq/interfaces/remote.h"
#include "sq/interfaces/video.h"
#include "sq/interfaces/surface.h"
#include "sq/interfaces/asio.h"

size_t sq_c_iface_sizes(size_t* out, size_t cap);

/* Sizes as seen by a C compiler; the C++ test compares them with its own view. */
size_t sq_c_iface_sizes(size_t* out, size_t cap) {
  const size_t s[] = {
    sizeof(sq_audio_device_info), sizeof(sq_audio_device_config), sizeof(sq_audio_io),
    sizeof(sq_midi_port_info), sizeof(sq_midi_timed_event), sizeof(sq_midi_file_event),
    sizeof(sq_audio_file_format), sizeof(sq_audio_file_marker), sizeof(sq_codec_format_info),
    sizeof(sq_track_info), sizeof(sq_clip_info), sizeof(sq_clip_event), sizeof(sq_doc_marker),
    sizeof(sq_engine_status), sizeof(sq_transport_state), sizeof(sq_render_request),
    sizeof(sq_plugin_desc), sizeof(sq_plugin_process), sizeof(sq_param_info), sizeof(sq_param_change),
    sizeof(sq_index_item), sizeof(sq_index_row), sizeof(sq_index_query), sizeof(sq_command),
    sizeof(sq_vector_ops), sizeof(sq_stretch_config), sizeof(sq_analysis_event), sizeof(sq_tensor),
    sizeof(sq_theme_resource), sizeof(sq_remote_control), sizeof(sq_osc_arg), sizeof(sq_tempo_sync_state),
    sizeof(sq_account_state), sizeof(sq_discovery_record), sizeof(sq_video_info), sizeof(sq_video_frame),
    sizeof(sq_video_backend_info)};
  size_t n = sizeof s / sizeof s[0], i;
  for (i = 0; i < n && i < cap; ++i) out[i] = s[i];
  return n;
}
