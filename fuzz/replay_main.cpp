// Stands in for libFuzzer where there is none: runs a target's entry point over
// every file named on the command line, and every file in a directory named
// there. Options, which libFuzzer would take, are ignored.
//
// A crash reproduces as a crash here too, so an input found by fuzzing on one
// platform can be debugged on another.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size);

namespace {

void run(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
    LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t *>(bytes.data()),
                           bytes.size());
}

} // namespace

int main(int argc, char **argv) {
    std::vector<std::filesystem::path> inputs;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument.starts_with('-')) {
            continue;
        }
        const std::filesystem::path path(argument);
        if (std::filesystem::is_directory(path)) {
            for (const auto &entry : std::filesystem::directory_iterator(path)) {
                if (entry.is_regular_file()) {
                    inputs.push_back(entry.path());
                }
            }
        } else {
            inputs.push_back(path);
        }
    }
    std::sort(inputs.begin(), inputs.end());

    for (const std::filesystem::path &input : inputs) {
        run(input);
    }
    std::cout << "ran " << inputs.size() << " inputs\n";
    // Nothing to run is a mistake in the invocation, not a pass.
    return inputs.empty() ? 1 : 0;
}
