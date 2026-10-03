// Reviewed proposal: substitute StringToSteamId only after guarded source review.
// No official provider authentication follows from textual conversion.
std::uint64_t StringToSteamId(std::string_view text) {
    // Bound original input before removing spaces, avoiding pinned truncation.
    if (text.empty() || text.size() >= 128) return 0;
    text = trim(text); if (text.empty()) return 0;
    std::array<std::string_view, 4> part{}; unsigned count = 0;
    if (text.substr(0, 6) == "STEAM_") {
        if (!fields(text.substr(6), part, count) || count != 3) return 0;
        std::uint64_t instance, low, high;
        if (!fieldNumber(part[0], 1, instance) || !fieldNumber(part[1], 1, low) ||
            !fieldNumber(part[2], UINT32_MAX / 2, high)) return 0;
        return (1ULL << 56) | (1ULL << 52) | (instance << 32) | (high * 2 + low);
    }
    const bool bracketed = text.front() == '[';
    if (bracketed) {
        if (text.back() != ']') return 0;
        text.remove_prefix(1); text.remove_suffix(1);
    }
    if (text.find(':') != std::string_view::npos) {
        if (!fields(text, part, count) || (count != 3 && count != 4) || part[0].size() != 1) return 0;
        const unsigned type = accountType(part[0][0]);
        std::uint64_t universe, account, instance = (type == 1 || type == 3) ? 1 : 0;
        if (!type || !fieldNumber(part[1], 255, universe) ||
            !fieldNumber(part[2], UINT32_MAX, account) ||
            (count == 4 && !fieldNumber(part[3], 0xfffff, instance))) return 0;
        const auto id = (universe << 56) | (std::uint64_t(type) << 52) | (instance << 32) | account;
        return validId(id) ? id : 0;
    }
    // Pinned optional brackets describe Steam3 only, not decimal Steam64.
    if (bracketed) return 0;
    std::uint64_t id;
    return number(text, UINT64_MAX, id) && validId(id) ? id : 0;
}
