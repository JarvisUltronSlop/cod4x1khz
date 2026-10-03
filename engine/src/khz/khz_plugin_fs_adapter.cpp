#include <universal/q_shared.h>
#include "khz_plugin_fs.h"
#include "khz_paths.h"
#if defined(COD4X1KHZ) && defined(KISAK_MP)
#include <universal/com_files.h>
#include <qcommon/threads.h>
#include <server_mp/server_mp.h>
#include <stringed/stringed_hooks.h>
#include <Windows.h>
#include <cstring>

namespace {
namespace io = khz::plugin_fs;
namespace fs = std::filesystem;
bool relativePath(const fs::path& root, const fs::path& candidate, std::string* output) {
    auto base = fs::absolute(root).lexically_normal(), path = fs::absolute(candidate).lexically_normal();
    auto i = base.begin(), j = path.begin();
    for (; i != base.end(); ++i, ++j) {
        if (j == path.end()) return false;
        const auto a = i->wstring(), b = j->wstring();
        if (CompareStringOrdinal(a.c_str(), int(a.size()), b.c_str(), int(b.size()), TRUE) != CSTR_EQUAL) return false;
    }
    fs::path remainder; for (; j != path.end(); ++j) remainder /= *j;
    *output = remainder.generic_string(); return !output->empty();
}
bool descriptorPath(const io::Catalog& catalog, const fs::path& physical, io::Search* search) {
    if (relativePath(catalog.home, physical, &search->relative)) { search->root = io::Root::Home; return true; }
    if (relativePath(catalog.base, physical, &search->relative)) { search->root = io::Root::Base; return true; }
    return false;
}
}
bool KHZ_PluginFsCaptureCatalog() {
    if (!Sys_IsMainThread() || com_inServerFrame || !FS_Initialized() || !fs_homepath || !fs_basepath || !fs_restrict || !fs_ignoreLocalized) return false;
    try {
        io::Catalog catalog;
        catalog.project = fs::path(KHZ_ProjectRoot()); catalog.home = fs::path(fs_homepath->current.string);
        catalog.base = fs::path(fs_basepath->current.string); catalog.game = *fs_gamedir ? fs_gamedir : "main";
        const auto language = SEH_GetCurrentLanguage();
        for (const auto* search = fs_searchpaths; search; search = search->next) {
            if (catalog.search.size() == io::SearchCapacity) return false;
            io::Search copy;
            copy.enabled = !search->bLocalized || (!fs_ignoreLocalized->current.enabled && search->language == language);
            if (search->iwd) {
                copy.kind = io::SearchKind::Archive; copy.checksum = std::uint32_t(search->iwd->checksum);
                if (!descriptorPath(catalog, fs::path(search->iwd->iwdFilename), &copy)) return false;
                copy.archiveAllowed = !fs_numServerIwds || search->bLocalized || search->ignorePureCheck;
                for (int i = 0; !copy.archiveAllowed && i < fs_numServerIwds && i < 1024; ++i)
                    copy.archiveAllowed = fs_serverIwds[i] == search->iwd->checksum;
            } else if (search->dir) {
                copy.kind = io::SearchKind::Directory;
                if (!descriptorPath(catalog, fs::path(search->dir->path) / fs::path(search->dir->gamedir), &copy)) return false;
                copy.looseAllowed = (!search->ignore && !fs_restrict->current.enabled && !fs_numServerIwds) || search->bLocalized || search->ignorePureCheck;
                copy.referenceLoose = !search->bLocalized && !search->ignorePureCheck;
            } else return false;
            catalog.search.push_back(std::move(copy));
        }
        return io::Publish(catalog);
    } catch (...) { return false; }
}
void KHZ_PluginFsServiceReferences() {
    if (!Sys_IsMainThread() || com_inServerFrame) return;
    io::Reference reference;
    unsigned serviced = 0;
    while (serviced++ < io::ReferenceCapacity && io::PollReference(&reference)) {
        if (reference.epoch != io::Epoch() || !FS_Initialized()) continue;
        if (reference.loose) {
            // Native loose reads set a nonzero pure sentinel. This dedicated
            // sequence avoids consuming gameplay/presentation random seeds.
            extern int fs_fakeChkSum;
            static unsigned serial = 0;
            fs_fakeChkSum = int((++serial & 0x7fffffff) ? serial & 0x7fffffff : 1);
            continue;
        }
        try {
            const auto physical = fs::path(reference.root == io::Root::Home ? fs_homepath->current.string : fs_basepath->current.string) / fs::path(reference.relative);
            for (auto* search = fs_searchpaths; search; search = search->next) {
                if (!search->iwd || std::uint32_t(search->iwd->checksum) != reference.checksum) continue;
                std::string relative;
                if (relativePath(physical.parent_path(), fs::path(search->iwd->iwdFilename), &relative) &&
                    !_stricmp(relative.c_str(), physical.filename().string().c_str())) {
                    search->iwd->referenced = 1; break;
                }
            }
        } catch (...) {}
    }
}
#else
bool KHZ_PluginFsCaptureCatalog() { return false; }
void KHZ_PluginFsServiceReferences() {}
#endif
