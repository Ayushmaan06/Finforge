configure_file(src/build_info.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/finforge/build_info.hpp @ONLY)

add_library(finforge_engine STATIC
  src/events.cpp
  src/instrument.cpp
  src/journal.cpp
  src/matching_engine.cpp
  src/order.cpp
  src/order_book.cpp
  src/rng.cpp
  src/sha256.cpp
  src/simulator.cpp
  src/synthetic.cpp
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
# Full book invariant check after every operation (O(resting orders)); Debug builds only.
target_compile_definitions(finforge_engine PRIVATE $<$<CONFIG:Debug>:FINFORGE_EXPENSIVE_CHECKS>)

add_executable(finforge_sim apps/finforge_sim.cpp)
set_target_properties(finforge_sim PROPERTIES OUTPUT_NAME finforge-sim)
target_link_libraries(finforge_sim PRIVATE finforge::engine)
target_compile_options(finforge_sim PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(FINFORGE_BUILD_TESTS)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
    URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  add_executable(finforge_engine_tests
    tests/differential_test.cpp
    tests/event_queue_test.cpp
    tests/instrument_test.cpp
    tests/matching_engine_test.cpp
    tests/order_test.cpp
    tests/rng_test.cpp
    tests/sha256_test.cpp
    tests/simulator_test.cpp
    tests/types_test.cpp
    tests/version_test.cpp)
  target_link_libraries(finforge_engine_tests PRIVATE finforge::engine GTest::gtest_main)
  target_compile_options(finforge_engine_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  include(GoogleTest)
  # Death tests fork; discovery must not time out on slow sanitizer builds.
  gtest_discover_tests(finforge_engine_tests DISCOVERY_TIMEOUT 60)

  # Demo 2 golden file (PLAN Phase 1 exit criterion).
  add_test(NAME finforge_sim.golden
    COMMAND ${CMAKE_COMMAND}
      -DSIM=$<TARGET_FILE:finforge_sim>
      -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/apps/demo/orders.csv
      -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/apps/demo/expected.txt
      -DACTUAL=${CMAKE_CURRENT_BINARY_DIR}/finforge_sim_actual.txt
      -P ${CMAKE_CURRENT_SOURCE_DIR}/apps/golden_test.cmake)
endif()

if(FINFORGE_BUILD_BENCH)
  FetchContent_Declare(googlebenchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.10.0.tar.gz
    URL_HASH SHA256=acc5c7c0fbed855c4aaa2d7c3c36fb82b0c15c28f4e151e3f679c2fc0aa404af
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googlebenchmark)

  add_executable(finforge_bench bench/book_bench.cpp)
  target_link_libraries(finforge_bench PRIVATE finforge::engine benchmark::benchmark)
  target_compile_options(finforge_bench PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
