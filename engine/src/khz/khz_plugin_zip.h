#pragma once
#include "../../deps/zlib/zlib.h"
#include "khz_plugin_zip_prefix.h"
#include "../../../upstream/cod4x_server/src/zlib/unzip.h"
// This public pinned export has no declaration in the upstream unzip header.
#ifdef __cplusplus
extern "C" {
#endif
int KHZ_Private_unzSetPassword(unzFile file, const char* password);
#ifdef __cplusplus
}
#endif
