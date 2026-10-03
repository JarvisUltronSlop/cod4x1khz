#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <webadmin_policy.hpp>

namespace khz::plugins::io {
inline constexpr unsigned MaxFormFields = 32, MaxFormText = 65536, MaxFormBuffer = 8 * 1024 * 1024, MaxHeaders = 16384;
struct FormField { char name[1024]{}, value[1024]{}; };
using Form = std::array<FormField, MaxFormFields>;
inline bool Equal(std::string_view a, std::string_view b) { return web::Equal(a, b); }
inline bool Decode(std::string_view encoded, char* output, unsigned capacity) {
    if (!output || !capacity) return false; unsigned used = 0;
    auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    for (unsigned i = 0; i != encoded.size(); ++i) {
        unsigned char value = static_cast<unsigned char>(encoded[i]);
        if (value == '%') {
            if (i + 2 >= encoded.size() || hex(encoded[i + 1]) < 0 || hex(encoded[i + 2]) < 0) return false;
            value = static_cast<unsigned char>(hex(encoded[i + 1]) * 16 + hex(encoded[i + 2])); i += 2;
        } else if (value == '+') value = ' ';
        // Form strings cannot carry NUL. Encoded line breaks are ordinary
        // values; HTTP framing is validated before parsing this body.
        if (!value || used + 1 >= capacity) return false; output[used++] = char(value);
    }
    output[used] = 0; return true;
}
inline std::string Encode(std::string_view input) {
    static constexpr char hex[] = "0123456789ABCDEF"; std::string encoded;
    for (unsigned char c : input) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') encoded += char(c);
        else if (c == ' ') encoded += '+';
        else { encoded += '%'; encoded += hex[c >> 4]; encoded += hex[c & 15]; }
    }
    return encoded;
}
inline bool AppendForm(char* output, unsigned capacity, std::string_view key, std::string_view value) {
    if (!output || !capacity || capacity > MaxFormBuffer || key.empty() || key.size() > 1023 || value.size() > MaxFormText / 3) return false;
    const auto* end = static_cast<const char*>(std::memchr(output, 0, capacity)); if (!end) return false;
    for (char c : key) if (!c) return false; for (char c : value) if (!c) return false;
    const auto pair = std::string(end != output ? "&" : "") + Encode(key) + "=" + Encode(value);
    const auto used = unsigned(end - output); if (pair.size() >= capacity - used) return false;
    std::memcpy(output + used, pair.data(), pair.size()); output[used + pair.size()] = 0; return true;
}
inline bool ParseForm(std::string_view body, Form* output) {
    if (!output) return false; *output = {};
    if (body.size() > MaxFormText) return false;
    Form candidate{}; unsigned count = 0;
    // Pinned responses terminate forms on unencoded CR/LF. Encode any line
    // break within a field; do not double decode it when looking up a value.
    body = body.substr(0, body.find_first_of("\r\n"));
    while (!body.empty()) {
        const auto ampersand = body.find('&'); const auto field = body.substr(0, ampersand); const auto equal = field.find('=');
        if (count == MaxFormFields || equal == std::string_view::npos || equal == 0) return false;
        auto& entry = candidate[count++];
        if (!Decode(field.substr(0, equal), entry.name, sizeof(entry.name)) || !Decode(field.substr(equal + 1), entry.value, sizeof(entry.value))) return false;
        if (ampersand == std::string_view::npos) break; body.remove_prefix(ampersand + 1);
    }
    *output = candidate; return true;
}
inline const char* Value(const Form& form, std::string_view name) {
    for (const auto& field : form) { if (!field.name[0]) break; if (Equal(field.name, name)) return field.value; }
    return "";
}
inline bool Headers(std::string_view headers) {
    if (headers.size() > MaxHeaders) return false;
    std::array<std::string_view, 64> seen{}; unsigned count = 0;
    while (!headers.empty()) {
        const auto next = headers.find("\r\n"); const auto line = headers.substr(0, next); const auto colon = line.find(':');
        if (line.empty() || colon == std::string_view::npos || !colon || count == seen.size()) return false;
        const auto key = line.substr(0, colon);
        for (unsigned char c : key) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || std::string_view("!#$%&'*+-.^_`|~").find(char(c)) != std::string_view::npos)) return false;
        for (unsigned char c : line.substr(colon + 1)) if ((c < 32 && c != '\t') || c == 127) return false;
        // WinHTTP owns framing/target. Explicit credentials remain available
        // to a configured plugin, but are never forwarded through redirects.
        for (const char* reserved : {"Host", "Content-Length", "Transfer-Encoding", "Connection", "Proxy-Connection"}) if (Equal(key, reserved)) return false;
        for (unsigned i = 0; i != count; ++i) if (Equal(seen[i], key)) return false;
        seen[count++] = key;
        if (next == std::string_view::npos) break; headers.remove_prefix(next + 2);
    }
    return true;
}
inline bool Method(std::string_view method) {
    if (method.empty() || method.size() > 16) return false;
    for (char c : method) if (c < 'A' || c > 'Z') return false; return true;
}
inline bool RelativePath(std::string_view path) {
    if (path.empty() || path.size() > 240) return false;
    unsigned count = 0;
    while (!path.empty()) {
        const auto slash = path.find('/'); const auto part = path.substr(0, slash);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ' || ++count > 16) return false;
        for (unsigned char c : part) if (c < 32 || c == 127 || c == ':' || c == '\\' || c == '<' || c == '>' || c == '"' || c == '|' || c == '*' || c == '?') return false;
        const auto stem = part.substr(0, part.find('.'));
        if (Equal(stem, "con") || Equal(stem, "prn") || Equal(stem, "aux") || Equal(stem, "nul") ||
            (stem.size() == 4 && (Equal(stem.substr(0, 3), "com") || Equal(stem.substr(0, 3), "lpt")) && stem[3] >= '0' && stem[3] <= '9')) return false;
        if (slash == std::string_view::npos) break; path.remove_prefix(slash + 1); if (path.empty()) return false;
    }
    return true;
}
}
