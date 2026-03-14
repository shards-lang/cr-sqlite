cmake_minimum_required(VERSION 3.16)
project(crsqlite C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Git commit SHA for crsql_sha() function
execute_process(
  COMMAND git rev-parse HEAD
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE CRSQLITE_COMMIT_SHA
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)
if(NOT CRSQLITE_COMMIT_SHA)
  set(CRSQLITE_COMMIT_SHA "unknown")
endif()

# ---------- Sources ----------

set(SQLITE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/sqlite)

set(EXT_SOURCES
  src/crsqlite.c
  src/changes-vtab.c
  src/changes-vtab-impl.c
  src/ext-data.c
  src/util.c
  src/pack-columns.c
  src/tableinfo.c
  src/bootstrap.c
  src/db-version.c
  src/triggers.c
  src/crr.c
  src/local-writes.c
  src/automigrate.c
  src/unpack-columns-vtab.c
  src/cl-set-vtab.c
  src/fractindex.c
)

set(COMMON_INCLUDES
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${SQLITE_DIR}
)

set(COMMON_DEFINES
  CRSQLITE_COMMIT_SHA="${CRSQLITE_COMMIT_SHA}"
)

# ---------- Loadable extension (shared library) ----------

add_library(crsqlite MODULE ${EXT_SOURCES})
target_include_directories(crsqlite PRIVATE ${COMMON_INCLUDES})
target_compile_definitions(crsqlite PRIVATE ${COMMON_DEFINES})
target_compile_options(crsqlite PRIVATE -Wall)
if(APPLE)
  set_target_properties(crsqlite PROPERTIES SUFFIX ".dylib")
endif()
set_target_properties(crsqlite PROPERTIES
  PREFIX ""
  OUTPUT_NAME "crsqlite"
)

# ---------- Static library ----------

add_library(crsqlite_static STATIC ${EXT_SOURCES})
target_include_directories(crsqlite_static PRIVATE ${COMMON_INCLUDES})
target_compile_definitions(crsqlite_static PRIVATE ${COMMON_DEFINES})
target_compile_options(crsqlite_static PRIVATE -Wall)

# ---------- Generate sqlite3-extra.c (sqlite3.c + core_init.c) ----------

set(SQLITE3_EXTRA_C ${CMAKE_CURRENT_BINARY_DIR}/sqlite3-extra.c)
add_custom_command(
  OUTPUT ${SQLITE3_EXTRA_C}
  COMMAND ${CMAKE_COMMAND} -E cat
    ${SQLITE_DIR}/sqlite3.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core_init.c
    > ${SQLITE3_EXTRA_C}
  DEPENDS ${SQLITE_DIR}/sqlite3.c src/core_init.c
  COMMENT "Generating sqlite3-extra.c"
)

# ---------- SQLite CLI with cr-sqlite built in ----------

add_executable(sqlite3_cli
  ${SQLITE3_EXTRA_C}
  ${SQLITE_DIR}/shell.c
  ${EXT_SOURCES}
)
target_include_directories(sqlite3_cli PRIVATE ${COMMON_INCLUDES})
target_compile_definitions(sqlite3_cli PRIVATE
  ${COMMON_DEFINES}
  SQLITE_THREADSAFE=0
  SQLITE_OMIT_LOAD_EXTENSION=1
  SQLITE_EXTRA_INIT=core_init
  SQLITE_ENABLE_BYTECODE_VTAB
)
target_compile_options(sqlite3_cli PRIVATE -Wall)
set_target_properties(sqlite3_cli PROPERTIES OUTPUT_NAME "sqlite3")

if(UNIX AND NOT APPLE)
  target_link_libraries(sqlite3_cli m pthread dl)
elseif(APPLE)
  target_link_libraries(sqlite3_cli m)
endif()

# ---------- Tests ----------

file(GLOB TEST_SOURCES src/*.test.c)
add_executable(crsqlite_test
  ${SQLITE3_EXTRA_C}
  src/tests.c
  ${TEST_SOURCES}
  ${EXT_SOURCES}
)
target_include_directories(crsqlite_test PRIVATE ${COMMON_INCLUDES})
target_compile_definitions(crsqlite_test PRIVATE
  ${COMMON_DEFINES}
  SQLITE_THREADSAFE=0
  SQLITE_OMIT_LOAD_EXTENSION=1
  SQLITE_EXTRA_INIT=core_init
  UNIT_TEST=1
)
target_compile_options(crsqlite_test PRIVATE -Wall)
set_target_properties(crsqlite_test PROPERTIES OUTPUT_NAME "crsqlite_test")

if(UNIX AND NOT APPLE)
  target_link_libraries(crsqlite_test m pthread dl)
elseif(APPLE)
  target_link_libraries(crsqlite_test m)
endif()

enable_testing()
add_test(NAME unit_tests COMMAND crsqlite_test)

# ---------- Install ----------

install(TARGETS crsqlite
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
)
install(TARGETS crsqlite_static
  ARCHIVE DESTINATION lib
)
