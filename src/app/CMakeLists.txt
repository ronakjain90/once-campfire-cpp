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
pkg_check_modules(PCRE2 REQUIRED IMPORTED_TARGET libpcre2-8)

campfire_library(app
  SOURCES
    allow_browser.cpp
    app.cpp
    broadcasts.cpp
    active_storage.cpp
    compress.cpp
    concerns.cpp
    controllers/accounts_common.cpp
    controllers/accounts.cpp
    controllers/accounts_bots.cpp
    controllers/accounts_custom_styles.cpp
    controllers/accounts_join_codes.cpp
    controllers/accounts_logos.cpp
    controllers/accounts_users.cpp
    controllers/accounts_users_list.cpp
    controllers/action_not_found.cpp
    controllers/autocompletable_users.cpp
    controllers/common.cpp
    controllers/first_runs.cpp
    controllers/message_boosts.cpp
    controllers/message_bots.cpp
    controllers/messages.cpp
    controllers/pwa.cpp
    controllers/active_storage.cpp
    controllers/qr_codes.cpp
    controllers/unfurl_links.cpp
    opengraph/html.cpp
    opengraph/opengraph.cpp
    unfurl_http.cpp
    controllers/session_transfers.cpp
    controllers/directs.cpp
    controllers/involvements.cpp
    controllers/room_pages.cpp
    controllers/rooms.cpp
    controllers/sessions.cpp
    controllers/users.cpp
    controllers/users_avatars.cpp
    controllers/users_bans.cpp
    controllers/users_profiles.cpp
    controllers/users_push_subscriptions.cpp
    controllers/users_show.cpp
    controllers/users_test_notifications.cpp
    controllers/sidebars.cpp
    controllers/welcome.cpp
    dispatch.cpp
    file_server.cpp
    errors.cpp
    finish.cpp
    fragment_cache.cpp
    health.cpp
    message_actions.cpp
    message_presenter.cpp
    network_guard.cpp
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
    rqrcode.cpp
    session_cache.cpp
    user_agent.cpp
    worker_state.cpp
    "${campfire_routes_cpp}"
    "${campfire_data_cpp}"
  PUBLIC_DEPS campfire_net campfire_core campfire_compat campfire_db campfire_models campfire_req campfire_views
              campfire_assets campfire_routes campfire_richtext campfire_storage campfire_cable
  DEPS OpenSSL::Crypto PkgConfig::LIBDEFLATE PkgConfig::PCRE2
  WERROR)

campfire_test(app
  SOURCES
    tests/accounts_area_test.cpp
    tests/active_storage_test.cpp
    tests/app_test.cpp
    tests/cache_test.cpp
    tests/finish_test.cpp
    tests/messages_area_test.cpp
    tests/rqrcode_test.cpp
    tests/signin_area_test.cpp
    tests/user_agent_test.cpp
    tests/rooms_test.cpp
    tests/room_pages_test.cpp
    tests/unfurl_policy_test.cpp
    tests/unfurl_test.cpp
  WERROR)
target_compile_definitions(test_app PRIVATE CAMPFIRE_SPEC_DIR="${PROJECT_SOURCE_DIR}/spec")

# Fuzz targets (architecture section 14): built only with -DCAMPFIRE_FUZZ=ON.
option(CAMPFIRE_FUZZ "Build the libFuzzer targets" OFF)
if(CAMPFIRE_FUZZ)
  add_subdirectory(fuzz)
endif()

campfire_executable(campfire SOURCES main.cpp DEPS campfire_app)
set_target_properties(campfire PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
