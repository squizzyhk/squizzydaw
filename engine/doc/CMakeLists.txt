# engine/doc: document model, XmlArchive, transactions/actions/undo, side effects, autosave.
# Implements sdk/include/sq/interfaces/document.h. Static library sq::engine_doc (in-process; the
# host calls sq::edoc::registerDocumentServices()).
sq_add_library(NAME sqengine_doc
  SOURCES
    src/model.cpp
    src/document.cpp
    src/nodes.cpp
    src/actions.cpp
    src/sideeffects.cpp
    src/archive.cpp
    src/manager.cpp
  INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
  LIBS tp::miniz)
add_library(sq::engine_doc ALIAS sqengine_doc)

sq_add_test(NAME engine_doc_model SOURCES tests/test_doc_model.cpp LIBS sq::engine_doc)
sq_add_test(NAME engine_doc_archive SOURCES tests/test_doc_archive.cpp LIBS sq::engine_doc)
sq_add_test(NAME engine_doc_undo_stress SOURCES tests/test_doc_undo_stress.cpp LIBS sq::engine_doc TIMEOUT 600)
sq_add_test(NAME engine_doc_host SOURCES tests/test_doc_host.cpp LIBS sq::engine_doc sq::appcore)
sq_add_test(NAME engine_doc_verify SOURCES tests/test_doc_verify.cpp LIBS sq::engine_doc sq::appcore)
