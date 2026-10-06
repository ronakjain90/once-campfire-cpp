# Template foundation (architecture section 7). Library: campfire_views. Tests: test_views.
# tools/ctc.py turns each src/views/**/*.ct file into C++ at build time.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(CTC "${PROJECT_SOURCE_DIR}/tools/ctc.py")
set(CT_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen")

# campfire_views_templates(<out var> <root dir> <out dir> <header name> [ctc options...])
# The directory test/ below the root is skipped.
# Compiles every .ct file under <root dir>. Puts the generated sources in <out var>, and the
# generated header (<out dir>/<header name>) last in the list.
function(campfire_views_templates var root outdir header)
  file(GLOB_RECURSE templates CONFIGURE_DEPENDS RELATIVE "${root}" "${root}/*.ct")
  list(FILTER templates EXCLUDE REGEX "^test/")
  set(outputs "")
  set(inputs "")
  foreach(ct IN LISTS templates)
    list(APPEND outputs "${outdir}/${ct}.cpp")
    list(APPEND inputs "${root}/${ct}")
  endforeach()
  add_custom_command(
    OUTPUT ${outputs} "${outdir}/${header}"
    COMMAND "${Python3_EXECUTABLE}" "${CTC}" --root "${root}" --out "${outdir}" --header-name "${header}" --exclude test/ ${ARGN}
    DEPENDS ${inputs} "${CTC}"
    COMMENT "ctc: ${root}"
    VERBATIM)
  set(${var} ${outputs} "${outdir}/${header}" PARENT_SCOPE)
endfunction()

campfire_views_templates(CAMPFIRE_VIEWS_GENERATED "${CMAKE_CURRENT_SOURCE_DIR}" "${CT_GEN_DIR}"
                         views/templates.gen.hpp --require-source-comment)

campfire_library(views
  SOURCES
    fragment_cache.cpp
    helpers/application.cpp
    helpers/assets.cpp
    helpers/forms.cpp
    helpers/links.cpp
    helpers/tag.cpp
    helpers/turbo.cpp
    ${CAMPFIRE_VIEWS_GENERATED}
  PUBLIC_DEPS campfire_core campfire_compat campfire_routes
  WERROR)
target_include_directories(campfire_views PUBLIC "${CT_GEN_DIR}")

# ---- tests ----------------------------------------------------------------------------------------
# test/ct: templates that the tests render, with ERB twins in test/erb (expected output made by
# test/gen_expected.rb). test/neg: templates that must not compile (SafeHtml negative test).
set(CT_TEST_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen_test")
campfire_views_templates(CAMPFIRE_VIEWS_TEST_GENERATED "${CMAKE_CURRENT_SOURCE_DIR}/test/ct" "${CT_TEST_GEN_DIR}"
                         views/test_templates.gen.hpp)

campfire_test(views
  SOURCES
    test/ctc_test.cpp
    test/helpers_test.cpp
    test/layout_test.cpp
    ${CAMPFIRE_VIEWS_TEST_GENERATED}
  WERROR)
target_include_directories(test_views PRIVATE "${CT_TEST_GEN_DIR}")
target_compile_definitions(test_views PRIVATE CAMPFIRE_VIEWS_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test")

set(CT_NEG_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen_neg")
campfire_views_templates(CAMPFIRE_VIEWS_NEG_GENERATED "${CMAKE_CURRENT_SOURCE_DIR}/test/neg" "${CT_NEG_GEN_DIR}"
                         views/neg_templates.gen.hpp)
foreach(neg IN ITEMS safe_text safe_int)
  add_library(views_neg_${neg} OBJECT EXCLUDE_FROM_ALL "${CT_NEG_GEN_DIR}/${neg}.ct.cpp" "${CT_NEG_GEN_DIR}/views/neg_templates.gen.hpp")
  target_link_libraries(views_neg_${neg} PRIVATE campfire_views)
  target_include_directories(views_neg_${neg} PRIVATE "${CT_NEG_GEN_DIR}")
  add_test(NAME views_negative_${neg}
           COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target views_neg_${neg})
  # The build must fail, and the compiler must say why.
  set_tests_properties(views_negative_${neg} PROPERTIES
    PASS_REGULAR_EXPRESSION "needs a SafeHtml value"
    FAIL_REGULAR_EXPRESSION "THIS_STRING_NEVER_APPEARS")
endforeach()
