// Squizzy Studio engine/doc - private helpers shared by the implementation files.
#pragma once

#include <string>

#include "sq/cpp/attributes.hpp"
#include "sq/engine/doc/document.hpp"

namespace sq::edoc::detail {

// Property views (getProperties) and SetProperty per record kind. set* return SQ_E_NOTFOUND for an
// unknown key, SQ_E_ACCESS for read-only keys, SQ_E_TYPE / SQ_E_INVALIDARG for bad values.
void docProps(const DocProps& p, IAttributes* dst);
void trackProps(const TrackRec& t, IAttributes* dst);
void clipProps(const ClipRec& c, IAttributes* dst);
void markerProps(const MarkerRec& m, IAttributes* dst);
sq_result setDocProp(DocProps& p, const std::string& key, IAttributes* src, const char* valueKey);
sq_result setTrackProp(TrackRec& t, const std::string& key, IAttributes* src, const char* valueKey);
sq_result setClipProp(ClipRec& c, const std::string& key, IAttributes* src, const char* valueKey);
sq_result setMarkerProp(MarkerRec& m, const std::string& key, IAttributes* src, const char* valueKey);

void putUser(const PropertyBag& bag, IAttributes* dst);
sq_result readValue(IAttributes* src, const char* key, Value& out);

// Registers the built-ins into the native action table (called once, lazily).
void registerBuiltinActions();

// Clip editing primitives shared by actions and side effects.
// Trims `c` so it covers [newStart, newEnd) keeping its content aligned to the timeline.
void trimClip(ClipRec& c, double newStart, double newEnd, const TempoMapRec& tempo);
// Splits at `at` (strictly inside). `left` is trimmed in place, returns the right part (new id).
ClipRec splitClip(ClipRec& left, double at, const TempoMapRec& tempo);

std::string fmtDouble(double v);

// True if following output routing from `from` reaches `target` (or loops). Cycle-safe.
bool routesTo(const Store& s, const sq_guid& from, const sq_guid& target);

}  // namespace sq::edoc::detail
