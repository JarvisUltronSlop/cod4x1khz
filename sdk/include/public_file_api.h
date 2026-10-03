/* Rebuilt source-compatible public FS names. File work must run in an explicit
 * host job or pinned plugin thread; no engine FS pointer/handle is shared.
 * Paths remain beneath the project home/base roots. Streams are invalidated
 * by an FS/map barrier. Full-file buffers remain valid until FreeFile, even
 * after cancellation; final unload reclaims them after all DLL pins finish.
 */
#ifndef COD4X1KHZ_PUBLIC_FILE_API_H
#define COD4X1KHZ_PUBLIC_FILE_API_H
PAPI long __cdecl Plugin_FS_SV_FOpenFileRead(const char* filename, fileHandle_t* handle);
PAPI fileHandle_t __cdecl Plugin_FS_SV_FOpenFileWrite(const char* filename);
PAPI fileHandle_t __cdecl Plugin_FS_SV_FOpenFileAppend(const char* filename);
PAPI long __cdecl Plugin_FS_FOpenFileRead(const char* filename, fileHandle_t* handle);
PAPI fileHandle_t __cdecl Plugin_FS_FOpenFileWrite(const char* filename);
PAPI fileHandle_t __cdecl Plugin_FS_FOpenFileAppend(const char* filename);
PAPI int __cdecl Plugin_FS_Read(void* destination, int bytes, fileHandle_t handle);
PAPI int __cdecl Plugin_FS_Write(const void* source, int bytes, fileHandle_t handle);
PAPI int __cdecl Plugin_FS_ReadLine(void* destination, int capacity, fileHandle_t handle);
PAPI qboolean __cdecl Plugin_FS_FCloseFile(fileHandle_t handle);
PAPI int __cdecl Plugin_FS_ReadFile(const char* qpath, void** destination);
PAPI void __cdecl Plugin_FS_FreeFile(void* buffer);
PAPI int __cdecl Plugin_FS_WriteFile(const char* qpath, const void* source, int bytes);
PAPI int __cdecl Plugin_FS_SV_HomeWriteFile(const char* qpath, const void* source, int bytes);
PAPI int __cdecl Plugin_FS_SV_BaseWriteFile(const char* qpath, const void* source, int bytes);
PAPI int __cdecl Plugin_FS_SV_WriteFile(const char* qpath, const void* source, int bytes);
PAPI void __cdecl Plugin_FS_SV_HomeCopyFile(char* source, char* destination);
/* Extension: obtain a private owner-validated opaque ZIP cookie before the
 * pinned Plugin_unzSetPassword operation. It is not an engine or decoder pointer.
 * Password <=63 bytes, classic PKZIP encryption only; WinZip AES/Zip64 and
 * multi-volume archives remain outside the pinned minizip format support.
 */
PAPI void* __cdecl Plugin_FS_GetZipHandle(fileHandle_t handle);
PAPI int __cdecl Plugin_unzSetPassword(void* zip, const char* password);
#endif
