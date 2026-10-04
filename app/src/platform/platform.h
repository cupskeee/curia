// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace curia::platform {

// Thin per-OS layer. Shared code never includes platform headers; each OS implements this interface
// in platform/{win,mac,linux} and CMake picks one. Overlay tweaks, hotkeys, secret storage and
// process spawning join this interface in later milestones (see docs/architecture.md section 5.3).
class Platform {
public:
    virtual ~Platform() = default;

    virtual std::string_view name() const = 0;

    // Places where CK3's user folder (the one containing logs/debug.log) may be, most likely first.
    virtual std::vector<std::filesystem::path> candidateCk3UserDirs() const = 0;
};

std::unique_ptr<Platform> create();

}  // namespace curia::platform
