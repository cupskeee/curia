// SPDX-License-Identifier: MIT
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <system_error>

#include "platform/env.h"
#include "platform/game_window.h"
#include "platform/platform.h"

namespace curia::platform {

namespace {

// A /proc entry that names a process (all digits).
bool isProcessDir(const std::string& name) {
    return !name.empty() &&
           std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; });
}

class LinuxPlatform final : public Platform {
public:
    std::string_view name() const override { return "linux"; }

    long long processId() const override { return static_cast<long long>(getpid()); }

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

    // Reads /proc/<pid>/comm of every process: "ck3" for the native game, "ck3.exe" under Proton
    // (the kernel keeps the first 15 bytes of the name, which both fit). No permission is needed;
    // a process that ends during the scan is skipped. std::nullopt when /proc cannot be read, also
    // part way through (an error ends the iteration, so it is checked after the loop as well).
    std::optional<bool> isGameRunning() const override {
        std::error_code ec;
        std::filesystem::directory_iterator it("/proc", ec);
        if (ec) {
            return std::nullopt;
        }
        for (const std::filesystem::directory_iterator end{}; it != end; it.increment(ec)) {
            const std::string entry = it->path().filename().string();
            if (!isProcessDir(entry)) {
                continue;
            }
            std::ifstream comm(it->path() / "comm");
            std::string name;
            if (comm && std::getline(comm, name) && isGameProcessName(name)) {
                return true;
            }
        }
        if (ec) {
            return std::nullopt;  // the scan stopped part way: cannot tell
        }
        return false;
    }

    // Not in M4: X11 needs _NET_ACTIVE_WINDOW and _NET_WM_PID, and Wayland offers no way at all.
    std::optional<bool> gameIsFrontmost() const override { return std::nullopt; }
    std::optional<ScreenRect> findGameWindow() const override { return std::nullopt; }

    // Every fifth tick: the scan opens one file per process.
    int gameRunningPollTicks() const override { return 5; }
};

}  // namespace

std::unique_ptr<Platform> create() {
    return std::make_unique<LinuxPlatform>();
}

}  // namespace curia::platform
