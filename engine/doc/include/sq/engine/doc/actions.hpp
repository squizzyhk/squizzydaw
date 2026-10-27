// Squizzy Studio engine/doc - engine-specific action classes (beyond the frozen sq::doc::k* set).
// Executed through IEditor::execute like the built-ins and registered under category "Action".
//
//   kSetAutomation  track:guid param:string points:blob(double[2*n]: pos, value, ...)  (replaces lane;
//                   empty blob removes the lane)
//   kMoveTrack      node:guid index:int parent:guid?     (flat display index; moves child tracks along)
//   kTransposeClip  clip:guid semitones:int               (MIDI notes)
#pragma once

#include "sq/guid.h"

namespace sq::edoc {
inline constexpr sq_guid kSetAutomation = sq::guid("{14D5E2BA-6EC7-4616-A188-F9A29E14D634}");
inline constexpr sq_guid kMoveTrack = sq::guid("{397F45FD-1A14-4502-BD15-1A177904CDFB}");
inline constexpr sq_guid kTransposeClip = sq::guid("{9FE30552-7670-4353-A657-2BE8A1881FDA}");
}  // namespace sq::edoc
