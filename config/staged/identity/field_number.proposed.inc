// Only legacy/Steam3 numeric fields accept the pinned scanf-leading grammar.
// The existing strict number() helper and unrelated protocol parsers stay intact.
bool fieldNumber(std::string_view text, std::uint64_t maximum, std::uint64_t& value) {
    while (!text.empty()) {
        const char c = text.front();
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '\v' && c != '\f') break;
        text.remove_prefix(1);
    }
    if (!text.empty() && text.front() == '+') text.remove_prefix(1);
    return number(text, maximum, value);
}
