# Jobs: the queues of the in-process job runner, and the Web Push and webhook pieces that need no database
# (architecture section 13). Library: campfire_jobs.
find_package(OpenSSL REQUIRED)

campfire_library(jobs
  SOURCES
    queue.cpp
    web_push.cpp
    webhook.cpp
  PUBLIC_DEPS campfire_core campfire_compat campfire_storage
  DEPS OpenSSL::Crypto
  WERROR)

campfire_test(jobs
  SOURCES
    tests/queue_test.cpp
    tests/web_push_test.cpp
    tests/webhook_test.cpp
  WERROR)
