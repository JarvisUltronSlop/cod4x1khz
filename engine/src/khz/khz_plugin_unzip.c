/* Adapted for cod4x1khz: private symbol names and the foundation zlib ABI.
 * Original minizip/Info-ZIP copyright and license notices are included intact
 * from the pinned CoD4x source below. Compile this translation unit as C.
 */
#include "../../deps/zlib/zlib.h"
/* The pinned newer zlib header uses ZLIB_H, whereas the foundation uses
 * _ZLIB_H. Suppress the former only after loading the actual foundation ABI. */
#ifndef ZLIB_H
#define ZLIB_H
#endif
#include "khz_plugin_zip_prefix.h"
#ifdef NOUNCRYPT
#undef NOUNCRYPT
#endif
#include "../../../upstream/cod4x_server/src/zlib/unzip.c"

/* All actual openings use unzOpen2 with verified HANDLE callbacks. Prevent
 * the unused convenience unzOpen entry point from opening arbitrary paths.
 */
static voidpf ZCALLBACK khz_zip_denied_open(voidpf opaque, const char* name, int mode) {
    (void)opaque; (void)name; (void)mode; return NULL;
}
static uLong ZCALLBACK khz_zip_denied_read(voidpf opaque, voidpf stream, void* data, uLong size) {
    (void)opaque; (void)stream; (void)data; (void)size; return 0;
}
static uLong ZCALLBACK khz_zip_denied_write(voidpf opaque, voidpf stream, const void* data, uLong size) {
    (void)opaque; (void)stream; (void)data; (void)size; return 0;
}
static long ZCALLBACK khz_zip_denied_tell(voidpf opaque, voidpf stream) { (void)opaque; (void)stream; return -1; }
static long ZCALLBACK khz_zip_denied_seek(voidpf opaque, voidpf stream, uLong position, int origin) {
    (void)opaque; (void)stream; (void)position; (void)origin; return -1;
}
static int ZCALLBACK khz_zip_denied_close(voidpf opaque, voidpf stream) { (void)opaque; (void)stream; return -1; }
static int ZCALLBACK khz_zip_denied_error(voidpf opaque, voidpf stream) { (void)opaque; (void)stream; return 1; }
void KHZ_Private_fill_fopen_filefunc(zlib_filefunc_def* functions) {
    memset(functions, 0, sizeof(*functions));
    functions->zopen_file = khz_zip_denied_open;
    functions->zread_file = khz_zip_denied_read;
    functions->zwrite_file = khz_zip_denied_write;
    functions->ztell_file = khz_zip_denied_tell;
    functions->zseek_file = khz_zip_denied_seek;
    functions->zclose_file = khz_zip_denied_close;
    functions->zerror_file = khz_zip_denied_error;
}
