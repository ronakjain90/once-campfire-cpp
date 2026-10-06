# The app: routes, health, 404, main. T5 owns main.cpp and the route table format.
file(GLOB campfire_route_files CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/routes/*.inc")
set(campfire_routes_cpp "${CMAKE_CURRENT_BINARY_DIR}/routes.gen.cpp")
add_custom_command(
  OUTPUT "${campfire_routes_cpp}"
  COMMAND python3 "${PROJECT_SOURCE_DIR}/tools/routes_gen.py" --out "${campfire_routes_cpp}" ${campfire_route_files}
  DEPENDS "${PROJECT_SOURCE_DIR}/tools/routes_gen.py" ${campfire_route_files}
  COMMENT "Generating the route table")

set(campfire_data_files
  "${CMAKE_CURRENT_SOURCE_DIR}/data/404.html"
  "${CMAKE_CURRENT_SOURCE_DIR}/data/404.html.gz.chunked"
  "${CMAKE_CURRENT_SOURCE_DIR}/data/up.html"
  "${CMAKE_CURRENT_SOURCE_DIR}/data/up.html.gz"
  "${CMAKE_CURRENT_SOURCE_DIR}/data/422.html"
  "${CMAKE_CURRENT_SOURCE_DIR}/data/500.html"
  "${CMAKE_CURRENT_SOURCE_DIR}/data/502.html"
  "${CMAKE_CURRENT_SOURCE_DIR}/data/schema.sql")
set(campfire_data_cpp "${CMAKE_CURRENT_BINARY_DIR}/data.gen.cpp")
add_custom_command(
  OUTPUT "${campfire_data_cpp}"
  COMMAND python3 "${PROJECT_SOURCE_DIR}/tools/embed.py" "${campfire_data_cpp}" ${campfire_data_files}
  DEPENDS "${PROJECT_SOURCE_DIR}/tools/embed.py" ${campfire_data_files}
  COMMENT "Embedding the data files")

find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBDEFLATE REQUIRED IMPORTED_TARGET libdeflate)

campfire_library(app
  SOURCES
    allow_browser.cpp
    app.cpp
    compress.cpp
    concerns.cpp
    controllers/action_not_found.cpp
    controllers/common.cpp
    controllers/first_runs.cpp
    controllers/session_transfers.cpp
    controllers/rooms.cpp
    controllers/sessions.cpp
    controllers/users.cpp
    controllers/welcome.cpp
    dispatch.cpp
    errors.cpp
    finish.cpp
    fragment_cache.cpp
    health.cpp
    not_found.cpp
    page.cpp
    page_cache.cpp
    platform.cpp
    proxy.cpp
    rails.cpp
    rate_limit.cpp
    render_page.cpp
    rq.cpp
    rq_response.cpp
    session_cache.cpp
    user_agent.cpp
    worker_state.cpp
    "${campfire_routes_cpp}"
    "${campfire_data_cpp}"
  PUBLIC_DEPS campfire_net campfire_core campfire_compat campfire_db campfire_models campfire_req campfire_views
              campfire_assets campfire_routes
  DEPS OpenSSL::Crypto PkgConfig::LIBDEFLATE
  WERROR)

campfire_test(app
  SOURCES
    tests/app_test.cpp
    tests/cache_test.cpp
    tests/finish_test.cpp
    tests/signin_area_test.cpp
    tests/user_agent_test.cpp
    tests/rooms_test.cpp
  WERROR)
target_compile_definitions(test_app PRIVATE CAMPFIRE_SPEC_DIR="${PROJECT_SOURCE_DIR}/spec")

campfire_executable(campfire SOURCES main.cpp DEPS campfire_app)
set_target_properties(campfire PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
