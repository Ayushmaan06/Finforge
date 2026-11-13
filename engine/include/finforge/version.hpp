#pragma once

#include <string>

namespace finforge {

// Engine version string, "<semver>+<git sha>.<build type>" (PLAN.md D6).
// Recorded in every run manifest.
std::string engine_version();

}  // namespace finforge
