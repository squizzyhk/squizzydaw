// midi-alsa (internal): class id of the ALSA sequencer IMidiDriver (category "MidiDriver").
#pragma once

#include "sq/sdk.hpp"

namespace sq::alsaseq {
inline constexpr sq_guid kAlsaMidiDriverClassId = sq::guid("{F1142D1E-3EBA-41C6-8802-6BE263B2A885}");
}  // namespace sq::alsaseq
