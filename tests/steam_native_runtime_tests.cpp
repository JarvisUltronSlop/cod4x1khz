#include <steamsdk/steam/steam_api.h>
#include <cstdio>
#include <cstdlib>

#define CHECK(v) do { if (!(v)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #v); std::abort(); } } while (0)
int main() {
    SteamErrMsg error{};
    CHECK(SteamAPI_InitEx(&error) == k_ESteamAPIInitResult_FailedGeneric);
    CHECK(error[0] && SteamAPI_GetHSteamUser() == 0);
    CHECK(SteamClient() == nullptr && SteamUser() == nullptr && SteamFriends() == nullptr);
    SteamAPI_RunCallbacks(); SteamAPI_Shutdown();
    CHECK(!SteamAPI_Init());
    std::printf("Native Steam loader: missing runtime reports initialization failure, preserves null interfaces and allows shutdown/retry\n");
}
