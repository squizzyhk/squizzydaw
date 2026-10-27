# engine/edit: arrange edit tools as engine/doc Actions + SideEffects, plus keyboard-addressable
# commands (ICommandHandler). Static library sq::engine_edit (in-process; the host calls
# sq::edit::registerEditServices(host) after engine/doc's registerDocumentServices).
sq_require_components(engine/doc)

sq_add_library(NAME sqengine_edit
  SOURCES
    src/common.cpp
    src/actions_clip.cpp
    src/actions_audio.cpp
    src/actions_time.cpp
    src/sideeffects.cpp
    src/commands.cpp
  INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PUBLIC_LIBS sq::engine_doc)
add_library(sq::engine_edit ALIAS sqengine_edit)

sq_add_test(NAME engine_edit_actions SOURCES tests/test_edit_actions.cpp LIBS sq::engine_edit sq::appcore)
sq_add_test(NAME engine_edit_sideeffects SOURCES tests/test_edit_sideeffects.cpp LIBS sq::engine_edit sq::appcore)
sq_add_test(NAME engine_edit_commands SOURCES tests/test_edit_commands.cpp LIBS sq::engine_edit sq::appcore)
sq_add_test(NAME engine_edit_workflow SOURCES tests/test_edit_workflow.cpp LIBS sq::engine_edit sq::appcore)
