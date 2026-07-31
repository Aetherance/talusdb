cmake_minimum_required(VERSION 3.20)

project(
  db
  VERSION 0.1.0
  DESCRIPTION "A LevelDB-inspired storage engine project"
  LANGUAGES CXX
)

include(CTest)
include(GoogleTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(DB_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(DB_BUILD_BENCHMARKS "Build TalusDB benchmarks" ON)

set(DB_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/include")
set(DB_SOURCE_DIR "${PROJECT_SOURCE_DIR}/src")
set(DB_TEST_DIR "${PROJECT_SOURCE_DIR}/tests")
set(DB_BENCHMARK_DIR "${PROJECT_SOURCE_DIR}/benchmarks")

function(db_enable_warnings target_name)
  if(MSVC)
    target_compile_options(
      "${target_name}"
      PRIVATE
        /W4
        /permissive-
        $<$<BOOL:${DB_WARNINGS_AS_ERRORS}>:/WX>
    )
  else()
    target_compile_options(
      "${target_name}"
      PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        $<$<BOOL:${DB_WARNINGS_AS_ERRORS}>:-Werror>
    )
  endif()
endfunction()

file(
  GLOB_RECURSE DB_LIBRARY_SOURCES
  CONFIGURE_DEPENDS
  "${DB_SOURCE_DIR}/*.cc"
  "${DB_SOURCE_DIR}/*.cpp"
  "${DB_SOURCE_DIR}/*.cxx"
)

list(FILTER DB_LIBRARY_SOURCES EXCLUDE REGEX ".*/.*_test\\.(cc|cpp|cxx)$")

file(
  GLOB_RECURSE DB_PUBLIC_HEADERS
  CONFIGURE_DEPENDS
  "${DB_INCLUDE_DIR}/*.h"
  "${DB_INCLUDE_DIR}/*.hh"
  "${DB_INCLUDE_DIR}/*.hpp"
)

if(DB_LIBRARY_SOURCES)
  add_library(db STATIC ${DB_LIBRARY_SOURCES})
  target_include_directories(
    db
    PUBLIC
      "${DB_INCLUDE_DIR}"
    PRIVATE
      "${DB_SOURCE_DIR}"
  )
  target_compile_features(db PUBLIC cxx_std_20)
  db_enable_warnings(db)

  if(DB_PUBLIC_HEADERS)
    target_sources(
      db
      PUBLIC
        FILE_SET public_headers
        TYPE HEADERS
        BASE_DIRS "${DB_INCLUDE_DIR}"
        FILES ${DB_PUBLIC_HEADERS}
    )
  endif()
else()
  message(STATUS "No library sources found under src/ yet.")
endif()

if(BUILD_TESTING AND TARGET db)
  find_package(GTest REQUIRED)

  file(
    GLOB DB_TEST_SOURCES
    CONFIGURE_DEPENDS
    "${DB_TEST_DIR}/*_test.cc"
    "${DB_TEST_DIR}/*_test.cpp"
    "${DB_TEST_DIR}/*_test.cxx"
  )

  foreach(test_source IN LISTS DB_TEST_SOURCES)
    get_filename_component(test_name "${test_source}" NAME_WE)
    add_executable("${test_name}" "${test_source}")
    target_link_libraries("${test_name}" PRIVATE db GTest::gtest GTest::gtest_main)
    target_include_directories(
      "${test_name}"
      PRIVATE
        "${DB_SOURCE_DIR}"
    )
    target_compile_features("${test_name}" PRIVATE cxx_std_20)
    db_enable_warnings("${test_name}")
    gtest_discover_tests("${test_name}")
  endforeach()
endif()

if(DB_BUILD_BENCHMARKS AND TARGET db)
  add_executable(db_bench "${DB_BENCHMARK_DIR}/db_bench.cc")
  target_link_libraries(db_bench PRIVATE db)
  target_compile_features(db_bench PRIVATE cxx_std_20)
  db_enable_warnings(db_bench)
endif()

find_program(DB_MAKE_BIN NAMES make gmake)
if(DB_MAKE_BIN)
  add_custom_target(
    fmt
    COMMAND "${DB_MAKE_BIN}" fmt
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Formatting project sources via make fmt"
    VERBATIM
  )
else()
  add_custom_target(
    fmt
    COMMAND
      "${CMAKE_COMMAND}" -E echo
      "make not found; run `make fmt` once GNU make is available."
    VERBATIM
  )
endif()
