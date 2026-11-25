#pragma once

// Always-on checks for invariants whose violation means a bug in the engine (contracts.md ORD-1,
// MAT-7). They stay enabled in Release: they are cheap, and a silently corrupted book is worse
// than a crash.

namespace finforge::detail {

[[noreturn]] void check_failed(const char* expr, const char* msg, const char* file, int line);

}  // namespace finforge::detail

#define FINFORGE_CHECK(cond, msg)                                               \
    do {                                                                        \
        if (!(cond)) [[unlikely]] {                                             \
            ::finforge::detail::check_failed(#cond, (msg), __FILE__, __LINE__); \
        }                                                                       \
    } while (false)
