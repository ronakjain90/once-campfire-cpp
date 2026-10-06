# Models: queries and callbacks for the tables that the app framework needs (A0).
# Later area tasks add their own model files here.
campfire_library(models
  SOURCES
    account.cpp
    account_admin.cpp
    attachments.cpp
    ban.cpp
    first_run.cpp
    hooks.cpp
    membership.cpp
    push_subscription.cpp
    room.cpp
    session.cpp
    user.cpp
    user_admin.cpp
    user_rooms.cpp
  PUBLIC_DEPS campfire_core campfire_db campfire_req campfire_compat campfire_storage
  WERROR)

campfire_test(models
  SOURCES
    tests/models_test.cpp
  WERROR)
target_compile_definitions(test_models PRIVATE CAMPFIRE_SPEC_DIR="${PROJECT_SOURCE_DIR}/spec")
