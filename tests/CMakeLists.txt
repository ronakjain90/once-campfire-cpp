# Shared wiring of the tests: one doctest main for all test executables.
add_library(campfire_doctest_main STATIC doctest_main.cpp)
target_link_libraries(campfire_doctest_main PUBLIC campfire_doctest PRIVATE campfire_options)
