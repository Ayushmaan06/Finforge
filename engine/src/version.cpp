#include "finforge/version.hpp"

#include <string>

#include "finforge/build_info.hpp"

namespace finforge {

std::string engine_version() {
    std::string build_type = build_info::kBuildType;
    if (build_type.empty()) build_type = "unspecified";
    return std::string(build_info::kVersion) + "+" + build_info::kGitSha + "." + build_type;
}

}  // namespace finforge
