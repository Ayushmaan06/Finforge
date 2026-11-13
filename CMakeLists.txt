cmake_minimum_required(VERSION 3.28)

project(finforge VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(FINFORGE_BUILD_TESTS "Build the C++ tests" ON)
option(FINFORGE_BUILD_PYTHON "Build the pybind11 module finforge._engine" OFF)
option(FINFORGE_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

if(FINFORGE_SANITIZE)
  if(FINFORGE_BUILD_PYTHON)
    message(FATAL_ERROR "FINFORGE_SANITIZE and FINFORGE_BUILD_PYTHON cannot be combined: "
                        "an ASan-instrumented extension cannot be loaded by a normal python.")
  endif()
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer
                      -fno-sanitize-recover=all)
  add_link_options(-fsanitize=address,undefined)
endif()

# Git SHA is part of the engine version (PLAN.md D6).
find_package(Git QUIET)
set(FINFORGE_GIT_SHA "unknown")
if(GIT_FOUND)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse --short=12 HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE _sha OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _sha_rc ERROR_QUIET)
  if(_sha_rc EQUAL 0 AND _sha)
    set(FINFORGE_GIT_SHA "${_sha}")
  endif()
endif()

include(FetchContent)

if(FINFORGE_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(engine)

if(FINFORGE_BUILD_PYTHON)
  add_subdirectory(python/bindings)
endif()
