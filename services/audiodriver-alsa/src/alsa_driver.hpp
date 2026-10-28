// audiodriver-alsa: IAudioDriver over ALSA PCM (internal).
#pragma once

#include "sq/interfaces/audiodevice.h"

namespace sq::alsa {

// Class id of the ALSA IAudioDriver (category "AudioDriver", driver info name "ALSA").
inline constexpr sq_guid kAlsaDriverClassId = sq::guid("{6542CA4E-3454-49ED-AEAF-4C52D15988B1}");

sq_result createDriver(const sq_guid* riid, void** out);

}  // namespace sq::alsa
