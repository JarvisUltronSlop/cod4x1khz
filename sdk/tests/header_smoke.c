/* Compile this same unit with MSVC /TC and /TP /Gd. It verifies that SDK
 * callback declarations and the header-emitted external marker work in both
 * languages without requiring engine headers or loading any DLL. */
#include "pinc.h"
PCL int OnInit(void) { return 0; }
PCL void OnInfoRequest(pluginInfo_t* information) {
    information->handlerVersion.major = PLUGIN_HANDLER_VERSION_MAJOR;
    information->handlerVersion.minor = PLUGIN_HANDLER_VERSION_MINOR;
}
