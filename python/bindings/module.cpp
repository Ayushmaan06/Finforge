#include <pybind11/pybind11.h>

#include "finforge/version.hpp"

namespace py = pybind11;

PYBIND11_MODULE(_engine, m) {
    m.doc() = "FinForge C++ engine (thin bindings; logic lives in engine/).";
    m.def("engine_version", &finforge::engine_version,
          "Engine version string: <semver>+<git sha>.<build type>.");
}
