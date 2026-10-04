// SPDX-License-Identifier: MIT
#include "platform/env.h"
#include "platform/platform.h"

namespace curia::platform {

namespace {

class WindowsPlatform final : public Platform {
public:
    std::string_view name() const override { return "windows"; }

    // TODO(M3): use SHGetKnownFolderPath(FOLDERID_Documents) because Documents can be redirected.
    std::vector<std::filesystem::path> candidateCk3UserDirs() const override {
        const std::filesystem::path user = getEnvPath(L"USERPROFILE", "USERPROFILE");
        if (user.empty()) {
            return {};
        }
        const std::filesystem::path suffix =
            std::filesystem::path("Paradox Interactive") / "Crusader Kings III";
        return {user / "Documents" / suffix, user / "OneDrive" / "Documents" / suffix};
    }
};

}  // namespace

std::unique_ptr<Platform> create() {
    return std::make_unique<WindowsPlatform>();
}

}  // namespace curia::platform
