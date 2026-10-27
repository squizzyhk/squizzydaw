# sqtest: minimal in-house test framework. Target: sq::test (links main()).
add_library(sqtest_main STATIC sqtest_main.cpp sqtest.hpp)
add_library(sq::test ALIAS sqtest_main)
sq_target_defaults(sqtest_main)
target_include_directories(sqtest_main PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
