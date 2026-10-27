# codec-midifile: Standard MIDI File (type 0/1) reader + writer implementing sq/interfaces/midi.h
# (IMidiFileReader / IMidiFileWriter, category "MidiFile").
sq_add_service(NAME codec-midifile VERSION 0.1.0
  SOURCES midifile.cpp smf.cpp smf.hpp
  DESCRIPTION "Squizzy Studio Standard MIDI File codec")

# Header-only meta-event / tempo-map helpers for consumers (no link dependency on the service).
add_library(sq_codec_midifile_headers INTERFACE)
target_include_directories(sq_codec_midifile_headers INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
add_library(sq::codec_midifile_headers ALIAS sq_codec_midifile_headers)

sq_add_test(NAME codec_midifile SOURCES tests/test_midifile.cpp
  LIBS sq::appcore sq::codec_midifile_headers DEPENDS svc_codec-midifile)
