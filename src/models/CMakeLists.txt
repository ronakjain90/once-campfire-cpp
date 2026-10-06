# Models: queries and callbacks for the tables that the app framework needs (A0).
# Later area tasks add their own model files here.
campfire_library(models
  SOURCES
    account.cpp
    ban.cpp
    session.cpp
    user.cpp
  PUBLIC_DEPS campfire_core campfire_db campfire_req
  WERROR)

campfire_test(models
  SOURCES
    tests/models_test.cpp
  WERROR)
target_compile_definitions(test_models PRIVATE CAMPFIRE_SPEC_DIR="${PROJECT_SOURCE_DIR}/spec")
