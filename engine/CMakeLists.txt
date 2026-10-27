# engine/: parent dir. Owned by the orchestrator; each engine/<x>/ is owned by one agent.
# Children are auto-discovered (engine/<x>/CMakeLists.txt), filtered by SQ_ONLY / SQ_SKIP.
# Conventions (docs/CONTRACTS.md 1.1): library target sqengine_<x> with alias sq::engine_<x>,
# headers in engine/<x>/include/sq/engine/<x>/*.hpp, tests named engine_<x>_<what>.
# A child that links a sibling calls sq_require_components(engine/<y>) first.
sq_add_child_components()
