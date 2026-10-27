# tools/: parent dir. Owned by the orchestrator; each tools/<x>/ is owned by one agent.
# Children are auto-discovered (tools/<x>/CMakeLists.txt), filtered by SQ_ONLY / SQ_SKIP.
# Conventions (docs/CONTRACTS.md 1.1): executables via sq_add_tool(NAME sq-<x> ...) -> bin/sq-<x>.exe,
# tests named tools_<x>_<what>.
sq_add_child_components()
