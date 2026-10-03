#include <khz/khz_asset_paths.h>
#include <khz/khz_content.h>
#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); std::exit(1); } } while (0)
namespace {
void file(const std::filesystem::path& root, const char* relative, const char* bytes) {
    std::filesystem::path path;
    CHECK(khz::content::ConfinedPath(root, relative, &path, true));
    CHECK(khz::content::AtomicWrite(path, bytes, std::strlen(bytes)));
}
}
int main() {
    const auto project = std::filesystem::path(__FILE__).parent_path().parent_path();
    const auto results = project / "results";
    std::filesystem::create_directories(results);
    std::filesystem::path fixture;
    // Windows reuses PIDs. Acquire a fresh directory atomically so a previous
    // retained home fastfile cannot change this run's initial precedence case.
    for (unsigned attempt = 0; attempt != 100; ++attempt) {
        const auto candidate = results / ("asset-paths-" + std::to_string(GetCurrentProcessId()) + "-" +
            std::to_string(GetTickCount64()) + "-" + std::to_string(attempt));
        std::error_code error;
        if (std::filesystem::create_directory(candidate, error)) { fixture = candidate; break; }
        CHECK(!error || error == std::errc::file_exists);
    }
    CHECK(!fixture.empty());
    const auto home = fixture / "home", base = fixture / "base";
    std::filesystem::create_directories(home); std::filesystem::create_directories(base);
    khz::assets::Fastfile selected;
    file(base, "mods/fixture/mod.ff", "base");
    CHECK(khz::assets::ModFastfile(home, base, "mods/fixture", "mod", &selected));
    CHECK(selected.physical == base / "mods/fixture/mod.ff");
    file(home, "mods/fixture/mod.ff", "verified home");
    CHECK(khz::assets::ModFastfile(home, base, "mods/fixture", "mod", &selected));
    CHECK(selected.physical == home / "mods/fixture/mod.ff");
    CHECK(selected.relative == "mods/fixture/mod.ff");
    CHECK(!khz::assets::ModFastfile(home, base, "../fixture", "mod", &selected));
    CHECK(!khz::assets::ModFastfile(home, base, "main", "mod", &selected));
    CHECK(!khz::assets::ModFastfile(home, base, "mods/fixture", "common_mp", &selected));
    file(home, "usermaps/mp_fixture/mp_fixture.ff", "map");
    file(base, "usermaps/mp_fixture/mp_fixture_load.ff", "load");
    CHECK(khz::assets::MapFastfile(home, base, "mp_fixture", "mp_fixture", &selected));
    CHECK(selected.relative == "usermaps/mp_fixture/mp_fixture.ff");
    CHECK(khz::assets::MapFastfile(home, base, "mp_fixture", "mp_fixture_load", &selected));
    CHECK(selected.physical == base / "usermaps/mp_fixture/mp_fixture_load.ff");
    CHECK(!khz::assets::MapFastfile(home, base, "mp_other", "mp_fixture", &selected));
    CHECK(!khz::assets::MapFastfile(home, base, "mp_fixture", "common_mp", &selected));
    CHECK(!khz::assets::MapFastfile(home, base, "../mp_fixture", "../mp_fixture", &selected));
    CHECK(khz::assets::CustomReference("usermaps/mp_fixture/mp_fixture", "", "mp_fixture"));
    CHECK(khz::assets::CustomReference("usermaps/mp_fixture/mp_fixture_load", "", "mp_fixture"));
    CHECK(!khz::assets::CustomReference("usermaps/mp_other/mp_other", "", "mp_fixture"));
    CHECK(!khz::assets::CustomReference("usermaps/mp_fixture/mp_fixture_extra", "", "mp_fixture"));
    CHECK(khz::assets::CustomReference("mods/fixture/mod", "mods/fixture", "mp_fixture"));
    CHECK(!khz::assets::CustomReference("mods/fixture2/mod", "mods/fixture", "mp_fixture"));
    CHECK(!khz::assets::CustomReference("mods/fixture/common_mp", "mods/fixture", "mp_fixture"));
    CHECK(khz::assets::ReferencedFastfile(home, base, "mods/fixture", "mp_fixture", "mods/fixture/mod", &selected));
    CHECK(selected.physical == home / "mods/fixture/mod.ff");
    // A directory cannot satisfy a downloadable fastfile, even with its name.
    std::filesystem::create_directories(home / "mods/directory/mod.ff");
    CHECK(!khz::assets::ModFastfile(home, base, "mods/directory", "mod", &selected));
    std::puts("Native mod/usermap fastfile paths respect home precedence, active map and confinement.");
}
