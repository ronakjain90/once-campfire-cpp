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
  "${CMAKE_CURRENT_SOURCE_DIR}/data/up.html.gz")
set(campfire_data_cpp "${CMAKE_CURRENT_BINARY_DIR}/data.gen.cpp")
add_custom_command(
  OUTPUT "${campfire_data_cpp}"
  COMMAND python3 "${PROJECT_SOURCE_DIR}/tools/embed.py" "${campfire_data_cpp}" ${campfire_data_files}
  DEPENDS "${PROJECT_SOURCE_DIR}/tools/embed.py" ${campfire_data_files}
  COMMENT "Embedding the data files")

campfire_library(app
  SOURCES health.cpp not_found.cpp rails.cpp "${campfire_routes_cpp}" "${campfire_data_cpp}"
  PUBLIC_DEPS campfire_net
  WERROR)

campfire_executable(campfire SOURCES main.cpp DEPS campfire_app)
set_target_properties(campfire PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
