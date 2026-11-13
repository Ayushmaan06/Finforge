configure_file(src/build_info.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/finforge/build_info.hpp @ONLY)

add_library(finforge_engine STATIC
  src/version.cpp
)
add_library(finforge::engine ALIAS finforge_engine)

target_include_directories(finforge_engine
  PUBLIC include
  PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_features(finforge_engine PUBLIC cxx_std_20)
# Linked into the Python extension, so it must be position independent.
set_target_properties(finforge_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(finforge_engine PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(FINFORGE_BUILD_TESTS)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
    URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  add_executable(finforge_engine_tests tests/version_test.cpp)
  target_link_libraries(finforge_engine_tests PRIVATE finforge::engine GTest::gtest_main)
  target_compile_options(finforge_engine_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  include(GoogleTest)
  gtest_discover_tests(finforge_engine_tests)
endif()
