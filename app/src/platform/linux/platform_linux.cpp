// SPDX-License-Identifier: MIT
#include "platform/env.h"
#include "platform/platform.h"

namespace curia::platform {

namespace {

class LinuxPlatform final : public Platform {
public:
    std::string_view name() const override { return "linux"; }

    // TODO(M3): read Steam's libraryfolders.vdf for non-default library locations.
    std::vector<std::filesystem::path> candidateCk3UserDirs() const override {
        const std::string home = getEnv("HOME");
        if (home.empty()) {
            return {};
        }
        const std::filesystem::path user(home);
        const std::filesystem::path suffix =
            std::filesystem::path("Paradox Interactive") / "Crusader Kings III";
        std::vector<std::filesystem::path> dirs;
        const std::string dataHome = getEnv("XDG_DATA_HOME");
        if (!dataHome.empty()) {
            dirs.push_back(std::filesystem::path(dataHome) / suffix);
        }
        dirs.push_back(user / ".local" / "share" / suffix);
        dirs.push_back(user / ".paradoxinteractive" / "Crusader Kings III");
        // CK3 under Proton keeps its user folder inside the Steam prefix (app id 1158310).
        dirs.push_back(user / ".local" / "share" / "Steam" / "steamapps" / "compatdata" /
                       "1158310" / "pfx" / "drive_c" / "users" / "steamuser" / "Documents" /
                       suffix);
        return dirs;
    }
};

}  // namespace

std::unique_ptr<Platform> create() {
    return std::make_unique<LinuxPlatform>();
}

}  // namespace curia::platform
