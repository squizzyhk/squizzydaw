// audiodriver-pulse: IAudioDriver over the PulseAudio simple API (internal).
#pragma once

#include "sq/interfaces/audiodevice.h"

namespace sq::pulse {

// Class id of the PulseAudio IAudioDriver (category "AudioDriver", driver info name "Pulse").
inline constexpr sq_guid kPulseDriverClassId = sq::guid("{10AEBF82-F73D-4072-8710-4325D313567D}");

sq_result createDriver(const sq_guid* riid, void** out);

}  // namespace sq::pulse
