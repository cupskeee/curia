// SPDX-License-Identifier: MIT
// Feeds files (or every file under a directory) to the fuzz entry point without libFuzzer, so that
// the seed corpus and the fixtures are replayed on every platform and compiler as an ordinary
// test. Exit code 0 when every input ran; a violated invariant aborts, as it does under the fuzzer.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace {

namespace fs = std::filesystem;

bool replayFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "fuzz_replay: cannot read %s\n", path.string().c_str());
        return false;
    }
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<fs::path> files;
    for (int i = 1; i < argc; ++i) {
        const fs::path arg(argv[i]);
        std::error_code ec;
        if (fs::is_directory(arg, ec)) {
            for (fs::recursive_directory_iterator it(arg, ec), end; !ec && it != end;
                 it.increment(ec)) {
                if (it->is_regular_file(ec)) {
                    files.push_back(it->path());
                }
            }
        } else {
            files.push_back(arg);
        }
        if (ec) {
            std::fprintf(stderr, "fuzz_replay: cannot list %s\n", argv[i]);
            return 1;
        }
    }
    std::sort(files.begin(), files.end());
    bool ok = !files.empty();
    for (const fs::path& file : files) {
        ok = replayFile(file) && ok;
    }
    std::printf("fuzz_replay: %zu inputs\n", files.size());
    return ok ? 0 : 1;
}
