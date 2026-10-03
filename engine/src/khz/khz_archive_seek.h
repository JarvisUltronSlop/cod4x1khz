#pragma once
#include <qcommon/unzip.h>
#include <cstdint>

// Native FS origins: current=0, end=1, begin=2. No allocation, null output,
// signed-to-unsigned decompressor errors, or out-of-file seeks.
inline int KHZ_ArchiveSeek(unzFile archive, unsigned long directoryPosition,
    std::int64_t length, int offset, int origin) {
    if (!archive || origin < 0 || origin > 2) return -1;
    const auto position = static_cast<std::int64_t>(unztell(archive));
    const auto target = (origin == 0 ? position : origin == 1 ? length : 0) + offset;
    if (position < 0 || length < 0 || target < 0 || target > length) return -1;
    if (target < position || (origin == 2 && target == 0)) {
        if (unzSetCurrentFileInfoPosition(archive, directoryPosition) != UNZ_OK || unzOpenCurrentFile(archive) != UNZ_OK) return -1;
    }
    auto remaining = target - unztell(archive);
    unsigned char discard[16384];
    while (remaining > 0) {
        const auto amount = static_cast<unsigned>(remaining < sizeof(discard) ? remaining : sizeof(discard));
        const int read = unzReadCurrentFile(archive, discard, amount);
        if (read <= 0 || static_cast<unsigned>(read) > amount) return -1;
        remaining -= read;
    }
    return unztell(archive) == target ? 0 : -1;
}
