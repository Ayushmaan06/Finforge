FetchContent_Declare(pybind11
  URL https://github.com/pybind/pybind11/archive/refs/tags/v2.13.6.tar.gz
  URL_HASH SHA256=e08cb87f4773da97fa7b5f035de8763abc656d87d5773e62f6da0587d1f0ec20
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(pybind11)

pybind11_add_module(_engine module.cpp backtest.cpp)
target_link_libraries(_engine PRIVATE finforge::engine)

install(TARGETS _engine LIBRARY DESTINATION finforge)
