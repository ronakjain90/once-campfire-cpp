# SQLite 3.53.2 amalgamation: the version that rusqlite 0.40 bundles (libsqlite3-sys 0.38.2).
# The options are the ones in the build.rs of that crate for the "bundled" feature, with two changes.
# These changes do not change the file format:
# - No SQLITE_ENABLE_MEMORY_MANAGEMENT. With it, all connections share one page cache and one mutex.
# - SQLITE_DEFAULT_MEMSTATUS=0. With the default, each allocation locks one global mutex.
add_library(campfire_sqlite3 STATIC sqlite3.c)
target_include_directories(campfire_sqlite3 SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(campfire_sqlite3 PRIVATE
  SQLITE_CORE SQLITE_DEFAULT_FOREIGN_KEYS=1 SQLITE_ENABLE_API_ARMOR SQLITE_ENABLE_COLUMN_METADATA
  SQLITE_ENABLE_DBSTAT_VTAB SQLITE_ENABLE_FTS3 SQLITE_ENABLE_FTS3_PARENTHESIS SQLITE_ENABLE_FTS5
  SQLITE_ENABLE_JSON1 SQLITE_ENABLE_LOAD_EXTENSION=1 SQLITE_DEFAULT_MEMSTATUS=0
  SQLITE_ENABLE_RTREE SQLITE_ENABLE_STAT4 SQLITE_SOUNDEX SQLITE_THREADSAFE=1 SQLITE_USE_URI
  HAVE_USLEEP=1 HAVE_ISNAN _POSIX_THREAD_SAFE_FUNCTIONS)
# Third-party code: no project warnings.
target_compile_options(campfire_sqlite3 PRIVATE -w)
target_link_libraries(campfire_sqlite3 PUBLIC ${CMAKE_DL_LIBS} m Threads::Threads)
