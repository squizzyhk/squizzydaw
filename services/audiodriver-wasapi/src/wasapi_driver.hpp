// Squizzy Studio - WASAPI audio driver service (internal).
#pragma once

#include "sq/interfaces/audiodevice.h"

namespace sq::wasapi {
// Class factory entry for the IAudioDriver implementation (a null driver on non-Windows builds).
sq_result createDriver(const sq_guid* riid, void** out);
}  // namespace sq::wasapi
