#include <universal/q_shared.h>
#include <universal/com_memory.h>
#include <universal/com_files.h>
#include <qcommon/qcommon.h>
#include <qcommon/files.h>
#include <qcommon/cmd.h>
#include <qcommon/unzip.h>
#include <sound/snd_public.h>
#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_set>
#include <stdexcept>

#define CHECK(v) do { if (!(v)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #v); std::abort(); } } while (0)
int fs_numServerIwds;
int fs_serverIwds[1024];
const char* fs_serverIwdNames[1024];
extern int fs_fakeChkSum;
int fs_fakeChkSum;
static std::vector<std::string> arguments;
static std::unordered_set<const char*> strings;
static unsigned stops;

void MyAssertHandler(const char*, int, int, const char*, ...) { throw std::runtime_error("Filesystem assertion"); }
void QDECL Com_Error(errorParm_t, const char*, ...) { throw std::runtime_error("Filesystem error"); }
void Com_DPrintf(int, const char*, ...) {}
void Com_Printf(int, const char*, ...) { throw std::runtime_error("Unexpected filesystem path"); }
void I_strncat(char*, int, const char*) { throw std::runtime_error("Unexpected filesystem path"); }
void I_strncpyz(char*, const char*, int) { throw std::runtime_error("Unexpected filesystem path"); }
int I_strnicmp(const char*, const char*, int) { throw std::runtime_error("Unexpected filesystem path"); }
int I_strncmp(const char*, const char*, int) { throw std::runtime_error("Unexpected filesystem path"); }
char* I_strlwr(char*) { throw std::runtime_error("Unexpected filesystem path"); }
char* QDECL va(const char*, ...) { throw std::runtime_error("Unexpected filesystem path"); }
int FS_FilenameCompare(const char*, const char*) { throw std::runtime_error("Unexpected filesystem path"); }
int FS_SV_FileExists(char*) { throw std::runtime_error("Unexpected filesystem path"); }
int DB_FileSize(const char*, int) { throw std::runtime_error("Unexpected filesystem path"); }
const dvar_s* fs_gameDirVar = nullptr;
searchpath_s* fs_searchpaths;
void Com_Memcpy(void* dest, const void* source, size_t bytes) { std::memcpy(dest, source, bytes); }
void SND_StopSounds(snd_stopsounds_arg_t which) { CHECK(which == SND_STOP_STREAMED); ++stops; }
int I_stricmp(const char* a, const char* b) { CHECK(strings.contains(a)); CHECK(strings.contains(b)); return _stricmp(a, b); }
const char* CopyString(const char* source) {
    auto* copy = static_cast<char*>(VirtualAlloc(nullptr, std::strlen(source) + 1, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    CHECK(copy);
    if constexpr (sizeof(void*) == 8) CHECK(reinterpret_cast<uintptr_t>(copy) > UINT32_MAX);
    std::strcpy(copy, source); CHECK(strings.insert(copy).second); return copy;
}
void FreeString(const char* text) { CHECK(strings.erase(text) == 1); CHECK(VirtualFree(const_cast<char*>(text), 0, MEM_RELEASE)); }
void Cmd_TokenizeString(char* text) { arguments.clear(); std::istringstream input(text); std::string word; while (input >> word) arguments.push_back(word); }
void Cmd_EndTokenizedString() { arguments.clear(); }
int Cmd_Argc() { return static_cast<int>(arguments.size()); }
const char* Cmd_Argv(int index) { return arguments.at(index).c_str(); }

static void load(std::string sums, std::string names) { FS_PureServerSetLoadedIwds(sums.data(), names.data()); }
int main() {
    static_assert(offsetof(unz_s, tmpFile) == (sizeof(void*) == 8 ? 136 : 128));
    fs_fakeChkSum = 42;
    load("101 202 303 404", "iw_00 iw_01 iw_02 iw_03");
    CHECK(fs_numServerIwds == 4 && strings.size() == 4 && stops == 1 && fs_fakeChkSum == 0);
    const char* original[4]; std::memcpy(original, fs_serverIwdNames, sizeof(original));
    for (int n = 0; n < 4; ++n) CHECK(fs_serverIwds[n] == (n + 1) * 101 && strings.contains(fs_serverIwdNames[n]));
    // A successive map with the same pure list must retain all native pointers.
    load("303 101 404 202", "IW_02 iw_00 iw_03 iw_01");
    CHECK(stops == 1 && strings.size() == 4 && !std::memcmp(original, fs_serverIwdNames, sizeof(original)));
    // Replacing a list must retire every old string and keep the new list intact.
    load("505 606", "mod_a mod_b");
    CHECK(stops == 2 && fs_numServerIwds == 2 && strings.size() == 2);
    CHECK(!std::strcmp(fs_serverIwdNames[0], "mod_a") && !std::strcmp(fs_serverIwdNames[1], "mod_b"));
    CHECK(fs_serverIwdNames[2] == nullptr && fs_serverIwdNames[3] == nullptr);
    load("", ""); CHECK(stops == 3 && fs_numServerIwds == 0 && strings.empty());
    load("", ""); CHECK(stops == 3 && strings.empty());
    std::printf("Production pure-file list: %zu-bit pointers, repeated maps, replacement and shutdown passed\n", sizeof(void*) * 8);
}
