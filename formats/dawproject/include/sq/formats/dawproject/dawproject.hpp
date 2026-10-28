// Squizzy Studio formats - DAWproject export/import (the open interchange format: a ZIP holding
// project.xml + metadata.xml + media). Own implementation of a documented SUBSET:
//
//   project.xml  <Project version="1.0">
//                  <Application name="Squizzy Studio" version="..."/>
//                  <Transport><Tempo unit="bpm" value=".."/><TimeSignature numerator denominator/></Transport>
//                  <Structure>
//                    <Track id name contentType="audio|notes|tracks" loaded="true" color="#RRGGBB">
//                      <Channel id role="regular|submix|effect|master" destination="<channel id>" audioChannels solo>
//                        <Devices>   instrument (deviceRole="instrument") then inserts (deviceRole="audioFX"):
//                          <BuiltinDevice|Vst3Plugin|ClapPlugin|Vst2Plugin|AuPlugin id deviceID deviceName
//                             deviceVendor sqFormat sqPath>  (sqFormat/sqPath: Squizzy extension attributes)
//                            <Enabled value/>  (false = bypassed insert)
//                            <Parameters><RealParameter id parameterID value/>...</Parameters>
//                        </Devices>
//                        <Volume id unit="decibel" value/> <Pan id unit="normalized" value(0..1)/> <Mute id value/>
//                        <Sends><Send id destination="<bus channel id>" type="pre|post"><Volume id unit="decibel" value/></Send></Sends>
//                      </Channel>
//                      <Track .../>  (children of a folder track: contentType="tracks", no channel)
//                    </Track> ...
//                  </Structure>
//                  <Arrangement id><Lanes timeUnit="beats">
//                      <Lanes track="trackId">
//                        <Points unit><Target parameter="<parameter id>"/><RealPoint|BoolPoint time value/>...</Points>
//                        <Clips>
//                        <Clip time duration playStart name>
//                          audio:  <Audio channels sampleRate duration algorithm="raw"><File path="audio/x.wav"/></Audio>
//                                  (playStart = source offset in SECONDS, attribute contentTimeUnit="seconds")
//                          notes:  <Notes><Note time duration channel key vel(0..1) rel/></Notes>
//                        </Clip></Clips></Lanes>
//                    </Lanes>
//                    <Markers><Marker time name/></Markers>
//                  </Arrangement>
//                </Project>
//   metadata.xml <MetaData><Title/><Application/></MetaData>
//   audio/*      copies of every referenced media file (unique names)
//
// Every track kind is exported: audio / instrument tracks, buses (role submix), FX tracks (role
// effect), the master (role master), folders (nested tracks). Routing is the channel destination
// (the master channel = the default route), sends keep their level and pre/post, the instrument
// and inserts are devices with their parameter values, and the automation lanes (volume, pan,
// mute, send levels, device parameters) are Points targeting those parameters.
// Not covered (documented gaps): plugin state chunks, tempo/signature changes after the first
// point (only the first tempo and signature are exported), clip fades, comping lanes, video.
// Import tolerates unknown elements; devices it cannot map are reported in `warnings`.
//
// Main thread only (uses document.h, which is main-thread only).
#pragma once

#include <string>

#include "sq/interfaces/document.h"

namespace sq::formats::dawproject {

struct ExportReport {
  int tracks = 0, clips = 0, notes = 0, media = 0, markers = 0;  // tracks: every track but the master
  int buses = 0, devices = 0, sends = 0, automation = 0;          // buses incl. FX tracks; automation lanes
  bool master = false;
  std::string warnings;  // newline separated
};

// Writes `doc` to `path` (.dawproject). Media files are copied into the archive.
sq_result exportProject(IDocument* doc, const std::string& path, ExportReport* report = nullptr, std::string* error = nullptr);

// Reads `path` into `doc` (expected to be empty, e.g. fresh from createDocument): tracks, clips,
// notes, markers, tempo and signature are added through doc's IEditor in ONE transaction (one
// undo step). Media is extracted to `mediaDir` (created if needed).
sq_result importProject(const std::string& path, IDocument* doc, const std::string& mediaDir, ExportReport* report = nullptr,
                        std::string* error = nullptr);

}  // namespace sq::formats::dawproject
