// SPDX-License-Identifier: MIT
#include "platform/env.h"
#include "platform/platform.h"

namespace curia::platform {

namespace {

class MacPlatform final : public Platform {
public:
    std::string_view name() const override { return "macos"; }

    std::vector<std::filesystem::path> candidateCk3UserDirs() const override {
        const std::string home = getEnv("HOME");
        if (home.empty()) {
            return {};
        }
        return {std::filesystem::path(home) / "Documents" / "Paradox Interactive" /
                "Crusader Kings III"};
    }
};

}  // namespace

std::unique_ptr<Platform> create() {
    return std::make_unique<MacPlatform>();
}

}  // namespace curia::platform
