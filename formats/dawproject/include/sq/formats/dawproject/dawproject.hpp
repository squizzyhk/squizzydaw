// Squizzy Studio formats - DAWproject export/import (the open interchange format: a ZIP holding
// project.xml + metadata.xml + media). Own implementation of a documented SUBSET:
//
//   project.xml  <Project version="1.0">
//                  <Application name="Squizzy Studio" version="..."/>
//                  <Transport><Tempo unit="bpm" value=".."/><TimeSignature numerator denominator/></Transport>
//                  <Structure>
//                    <Track id name contentType="audio|notes" loaded="true" color="#RRGGBB">
//                      <Channel id role="regular" audioChannels solo="false">
//                        <Volume unit="decibel" value/> <Pan unit="normalized" value(0..1)/> <Mute value/>
//                      </Channel></Track> ...
//                  </Structure>
//                  <Arrangement id><Lanes timeUnit="beats">
//                      <Lanes track="trackId"><Clips>
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
// Not covered (documented gaps): automation, devices/plugins, sends, tempo/signature changes after
// the first point (only the first tempo and signature are exported), clip fades, comping lanes,
// video. Import tolerates unknown elements.
//
// Main thread only (uses document.h, which is main-thread only).
#pragma once

#include <string>

#include "sq/interfaces/document.h"

namespace sq::formats::dawproject {

struct ExportReport {
  int tracks = 0, clips = 0, notes = 0, media = 0, markers = 0;
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
