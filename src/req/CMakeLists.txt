# Request parts (architecture section 4): params, multipart, cookies, session, forgery, formats, bcrypt.
# Library: campfire_req. Tests: test_req.
campfire_library(req
  SOURCES
    param.cpp
    query.cpp
    format.cpp
  PUBLIC_DEPS campfire_core campfire_compat
  WERROR)
