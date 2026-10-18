// SPDX-License-Identifier: MIT
#include "chatview/log_path.h"

namespace curia::chatview {

std::filesystem::path logFileIn(const std::filesystem::path& userDir) {
    return userDir / "logs" / "debug.log";
}

LogPathChoice chooseLogPath(const std::string& flagPath,
                            const std::vector<std::filesystem::path>& userDirs,
                            const ExistsFn& exists) {
    LogPathChoice choice;
    if (!flagPath.empty()) {
        choice.path = std::filesystem::path(flagPath);
        choice.fromFlag = true;
        return choice;
    }
    if (!exists) {
        return choice;
    }
    for (const std::filesystem::path& dir : userDirs) {
        const std::filesystem::path file = logFileIn(dir);
        if (exists(file)) {
            choice.path = file;
            return choice;
        }
    }
    return choice;
}

}  // namespace curia::chatview
