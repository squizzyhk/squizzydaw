# engine/audio: M2 audio engine core (process graph, PDC, render thread, fast lane, recording,
# offline render tasks, IAudioEngine). Owner: engine-audio agent.
# Consumes only interface headers (audiodevice.h, codec.h, document.h); no sibling targets.
sq_add_library(NAME sqengine_audio
  SOURCES
    src/processor.cpp
    src/nodes.cpp
    src/graph.cpp
    src/render_thread.cpp
    src/devices.cpp
    src/recorder.cpp
    src/render.cpp
    src/doc_builder.cpp
    src/engine.cpp
    src/plugin_fx.cpp
    # built-in instruments + effects (builtin.hpp)
    src/builtin/catalog.cpp
    src/builtin/synth.cpp
    src/builtin/drums.cpp
    src/builtin/keys.cpp
    src/builtin/bass.cpp
    src/builtin/sampler.cpp
    src/builtin/eq.cpp
    src/builtin/dynamics.cpp
    src/builtin/reverb.cpp
    src/builtin/delay.cpp
    src/builtin/modulation.cpp
    src/builtin/distortion.cpp
    src/builtin/filter.cpp
    src/builtin/utility.cpp
  INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include")
add_library(sq::engine_audio ALIAS sqengine_audio)
if(WIN32)
  target_link_libraries(sqengine_audio PUBLIC avrt)
endif()
find_package(Threads REQUIRED)
target_link_libraries(sqengine_audio PUBLIC Threads::Threads)

set(_ea_libs sq::engine_audio sq::appcore)
sq_add_test(NAME engine_audio_graph SOURCES tests/test_graph.cpp LIBS ${_ea_libs})
sq_add_test(NAME engine_audio_render SOURCES tests/test_render.cpp LIBS ${_ea_libs})
sq_add_test(NAME engine_audio_pdc SOURCES tests/test_pdc.cpp LIBS ${_ea_libs})
sq_add_test(NAME engine_audio_record SOURCES tests/test_record.cpp LIBS ${_ea_libs} TIMEOUT 300)
sq_add_test(NAME engine_audio_rtsafety SOURCES tests/test_rtsafety.cpp LIBS ${_ea_libs})
sq_add_test(NAME engine_audio_service SOURCES tests/test_service.cpp LIBS ${_ea_libs})
sq_add_test(NAME engine_audio_verify SOURCES tests/test_verify.cpp LIBS ${_ea_libs})
sq_add_test(NAME engine_audio_plugin_fx SOURCES tests/test_plugin_fx.cpp LIBS ${_ea_libs})
sq_add_test(NAME engine_audio_builtin SOURCES tests/test_builtin.cpp LIBS ${_ea_libs})
sq_add_test(NAME engine_audio_builtin_dsp SOURCES tests/test_builtin_dsp.cpp LIBS ${_ea_libs})
sq_add_test(NAME engine_audio_builtin_rtsafety SOURCES tests/test_builtin_rtsafety.cpp LIBS ${_ea_libs})
