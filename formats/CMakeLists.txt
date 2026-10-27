# formats/: parent dir. Owned by the orchestrator; each formats/<x>/ is owned by one agent.
# Children are auto-discovered (formats/<x>/CMakeLists.txt), filtered by SQ_ONLY / SQ_SKIP.
# Conventions (docs/CONTRACTS.md 1.1): library target sqformats_<x> with alias sq::formats_<x>,
# headers in formats/<x>/include/sq/formats/<x>/*.hpp, tests named formats_<x>_<what>.
sq_add_child_components()
