#include <pinc.h>
#include <cstdio>
#include <type_traits>

static_assert(sizeof(scr_entref_t) == 4);
static_assert(sizeof(khz_public_netadr_t) == 32);
static_assert(sizeof(pluginInfo_t) == 1232);
static_assert(COD4X1KHZ_PLUGIN_SDK_VERSION == (sizeof(void*) == 8 ? 3 : 2));
static_assert(std::is_same_v<decltype(&Plugin_Malloc), void*(__cdecl*)(size_t)>);
static_assert(std::is_same_v<decltype(&Plugin_ImportFunction), void*(__cdecl*)(const char*, const char*)>);

int main() {
    if (KHZ_PluginCompatibility[0] != COD4X1KHZ_PLUGIN_PROTOCOL ||
        KHZ_PluginCompatibility[1] != COD4X1KHZ_PLUGIN_SDK_VERSION) return 1;
    std::printf("Plugin SDK marker %u: %zu-bit pointer API, unchanged public address and entity reference layouts\n",
        KHZ_PluginCompatibility[1], sizeof(void*) * 8);
}
