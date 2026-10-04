#include "gfx_vulkan/vk_context.h"
#include <cstdio>
#include <fstream>
#include <exception>
#include <cstring>

int main(int argc, char** argv) {
    try {
        gfx::vk::Config config;
        const char* output = nullptr;
        for (int i = 1; i < argc; ++i) {
            if (!std::strcmp(argv[i], "--validation")) config.validation = true;
            else if (!std::strcmp(argv[i], "--json") && i + 1 < argc) output = argv[++i];
            else throw std::runtime_error("Usage: khz_vulkan_probe [--validation] [--json output]");
        }
        gfx::vk::Context context(config);
        const auto report = context.json();
        if (output) { std::ofstream file(output); file << report << '\n'; if (!file) throw std::runtime_error("Cannot write probe result"); }
        std::puts(report.c_str());
        return context.validationErrors ? 1 : 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "Vulkan preflight failed: %s\n", error.what()); return 1; }
}
