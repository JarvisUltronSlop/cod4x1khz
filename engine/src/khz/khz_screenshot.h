#pragma once
#include <cstddef>
#include <cstdint>
struct client_t;
enum class KHZ_ScreenshotRequesterKind : unsigned { Console, RemoteConsole, PlayerAdmin, Plugin, WebAdmin };
struct KHZ_ScreenshotRequester {
    KHZ_ScreenshotRequesterKind kind=KHZ_ScreenshotRequesterKind::Console;
    int slot=-1,power=0;
    std::uint8_t fingerprint[32]{};
};
// Main control/service owner only. The administration adapter checks power45
// before requesting; attribution is copied, never retained through a pointer.
void KHZ_ScreenshotServerInit();
bool KHZ_ScreenshotServerRequest(client_t* client,const char* basename,const KHZ_ScreenshotRequester& requester);
bool KHZ_ScreenshotServerClientCommand(client_t* client,const char* text);
void KHZ_ScreenshotServerService();
void KHZ_ScreenshotServerErrorCleanup();
void KHZ_ScreenshotServerShutdown();
const char* KHZ_ScreenshotServerStatsJSON();
void KHZ_ScreenshotClientInit();
// Packet owner validates/copies bounded metadata only. Capture and transfer
// publication occur on main service; encoding is a private service worker.
bool KHZ_ScreenshotClientServerCommand(const char* text);
void KHZ_ScreenshotClientService();
void KHZ_ScreenshotClientDisconnect();
void KHZ_ScreenshotClientShutdown();
const char* KHZ_ScreenshotClientStatsJSON();
// Root renderer adapter, main only. Packed top-down native BGR24 game pixels;
// synchronized backbuffer access. Neither desktop capture nor frame mutation.
bool KHZ_CaptureScreenshotPixels(std::uint8_t* bgr,unsigned capacity,unsigned* width,unsigned* height);
