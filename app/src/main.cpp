// SPDX-License-Identifier: MIT
#include <cstdio>
#include <string>

#include "app.h"
#include "core/build_info.h"
#include "core/cli.h"

int main(int argc, char** argv) {
    curia::CliOptions options;
    std::string error;
    if (!curia::parseCli(argc, argv, options, error)) {
        std::fprintf(stderr, "curia: %s\n%s", error.c_str(),
                     std::string(curia::cliUsage()).c_str());
        return 2;
    }
    if (options.showHelp) {
        std::printf("%s", std::string(curia::cliUsage()).c_str());
        return 0;
    }
    if (options.showVersion) {
        std::printf("%s\n", curia::buildInfoJson().c_str());
        return 0;
    }
    return curia::runApp(options);
}
