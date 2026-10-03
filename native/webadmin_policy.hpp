#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

namespace khz::web {
inline constexpr unsigned MaxConnections = 16, MaxHeader = 16384, MaxBody = 4096, MaxReply = 131072;
using Form = std::vector<std::pair<std::string, std::string>>;
inline bool Equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (unsigned i = 0; i != a.size(); ++i) {
        auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : c; };
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}
inline std::string_view Trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}
inline bool Decode(std::string_view text, std::string* result, unsigned maximum) {
    if (!result) return false; result->clear();
    auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    for (unsigned i = 0; i != text.size(); ++i) {
        unsigned char value = static_cast<unsigned char>(text[i]);
        if (value == '%') {
            if (i + 2 >= text.size() || hex(text[i + 1]) < 0 || hex(text[i + 2]) < 0) return false;
            value = static_cast<unsigned char>(hex(text[i + 1]) * 16 + hex(text[i + 2])); i += 2;
        } else if (value == '+') value = ' ';
        if (value < 32 || value == 127 || result->size() == maximum) return false;
        result->push_back(char(value));
    }
    return true;
}
inline bool ParseForm(std::string_view text, Form* form) {
    if (!form || text.size() > MaxBody) return false; form->clear();
    while (!text.empty()) {
        const auto end = text.find('&'); const auto field = text.substr(0, end); const auto equal = field.find('=');
        if (equal == std::string_view::npos || form->size() == 32) return false;
        std::string name, value;
        if (!Decode(field.substr(0, equal), &name, 32) || name.empty() || !Decode(field.substr(equal + 1), &value, 1023)) return false;
        for (const auto& entry : *form) if (entry.first == name) return false;
        form->emplace_back(std::move(name), std::move(value));
        if (end == std::string_view::npos) break;
        text.remove_prefix(end + 1); if (text.empty()) return false;
    }
    return true;
}
inline const std::string& Value(const Form& form, std::string_view name) {
    for (const auto& field : form) if (field.first == name) return field.second;
    static const std::string empty; return empty;
}
inline bool Decimal(std::string_view text, std::uint64_t maximum, std::uint64_t* result) {
    if (!result || text.empty()) return false; std::uint64_t value = 0;
    for (const unsigned char c : text) {
        if (c < '0' || c > '9' || value > maximum / 10 ||
            (value == maximum / 10 && unsigned(c - '0') > maximum % 10)) return false;
        value = value * 10 + c - '0';
    }
    *result = value; return true;
}
struct Parsed { bool post = false; std::string target, host, origin, cookie; Form form, query; };
enum class ParseResult { Incomplete, Valid, Invalid, TooLarge };
inline ParseResult Parse(std::string_view text, Parsed* parsed) {
    if (!parsed) return ParseResult::Invalid;
    const auto headerEnd = text.find("\r\n\r\n");
    if (headerEnd == std::string_view::npos) return text.size() > MaxHeader ? ParseResult::TooLarge : ParseResult::Incomplete;
    if (headerEnd + 4 > MaxHeader || text.size() > MaxHeader + MaxBody) return ParseResult::TooLarge;
    Parsed candidate; std::string_view lines = text.substr(0, headerEnd);
    const auto firstEnd = lines.find("\r\n");
    const auto first = lines.substr(0, firstEnd); const auto methodEnd = first.find(' '), versionStart = first.rfind(' ');
    if (methodEnd == std::string_view::npos || methodEnd == versionStart) return ParseResult::Invalid;
    const auto method = first.substr(0, methodEnd), version = first.substr(versionStart + 1);
    if ((method != "GET" && method != "POST") || (version != "HTTP/1.1" && version != "HTTP/1.0")) return ParseResult::Invalid;
    candidate.post = method == "POST"; candidate.target = first.substr(methodEnd + 1, versionStart - methodEnd - 1);
    if (candidate.target.empty() || candidate.target.front() != '/' || candidate.target.size() > 1023) return ParseResult::Invalid;
    for (unsigned char c : candidate.target) if (c <= 32 || c == 127 || c == '#') return ParseResult::Invalid;
    if (firstEnd == std::string_view::npos) lines = {};
    else lines.remove_prefix(firstEnd + 2);
    bool lengthSet = false, hostSet = false, originSet = false, cookieSet = false, typeSet = false;
    std::uint64_t length = 0;
    while (!lines.empty()) {
        const auto next = lines.find("\r\n"); const auto line = lines.substr(0, next); const auto colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0 || line.front() == ' ' || line.front() == '\t') return ParseResult::Invalid;
        const auto key = line.substr(0, colon), value = Trim(line.substr(colon + 1));
        for (unsigned char c : key) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || std::string_view("!#$%&'*+-.^_`|~").find(char(c)) != std::string_view::npos)) return ParseResult::Invalid;
        for (unsigned char c : value) if ((c < 32 && c != '\t') || c == 127) return ParseResult::Invalid;
        if (Equal(key, "Transfer-Encoding")) return ParseResult::Invalid;
        if (Equal(key, "Content-Length")) { if (lengthSet || !Decimal(value, MaxBody, &length)) return ParseResult::Invalid; lengthSet = true; }
        if (Equal(key, "Host")) { if (hostSet || value.empty() || value.size() > 127) return ParseResult::Invalid; hostSet = true; candidate.host = value; }
        if (Equal(key, "Origin")) { if (originSet || value.size() > 255) return ParseResult::Invalid; originSet = true; candidate.origin = value; }
        if (Equal(key, "Cookie")) { if (cookieSet || value.size() > 1023) return ParseResult::Invalid; cookieSet = true; candidate.cookie = value; }
        if (Equal(key, "Content-Type")) { if (typeSet || (candidate.post && !Equal(value, "application/x-www-form-urlencoded"))) return ParseResult::Invalid; typeSet = true; }
        if (next == std::string_view::npos) break; lines.remove_prefix(next + 2);
    }
    if ((version == "HTTP/1.1" && !hostSet) || (candidate.post && (!lengthSet || !typeSet)) || (!candidate.post && length)) return ParseResult::Invalid;
    const auto body = text.substr(headerEnd + 4); if (body.size() < length) return ParseResult::Incomplete;
    if (body.size() != length || !ParseForm(body, &candidate.form)) return ParseResult::Invalid;
    const auto question = candidate.target.find('?');
    if (question != std::string::npos) {
        if (!ParseForm(std::string_view(candidate.target).substr(question + 1), &candidate.query)) return ParseResult::Invalid;
        candidate.target.resize(question);
    }
    *parsed = std::move(candidate); return ParseResult::Valid;
}
inline std::string Escape(std::string_view text) {
    std::string result;
    for (const char c : text) switch (c) {
    case '&': result += "&amp;"; break; case '<': result += "&lt;"; break;
    case '>': result += "&gt;"; break; case '"': result += "&quot;"; break;
    case '\'': result += "&#39;"; break; default: result += c; break;
    }
    return result;
}
inline bool HexToken(std::string_view text) {
    if (text.size() != 64) return false;
    for (char c : text) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
inline bool PrivateAddress(std::string_view text) {
    unsigned parts[4]{};
    for (unsigned i = 0; i != 4; ++i) {
        const auto dot = text.find('.'); const auto part = text.substr(0, dot);
        std::uint64_t value = 0;
        if ((part.size() > 1 && part.front() == '0') || !Decimal(part, 255, &value)) return false;
        parts[i] = unsigned(value);
        if (i == 3) { if (dot != std::string_view::npos) return false; }
        else { if (dot == std::string_view::npos) return false; text.remove_prefix(dot + 1); }
    }
    return parts[0] == 127 || parts[0] == 10 || (parts[0] == 192 && parts[1] == 168) ||
        (parts[0] == 172 && parts[1] >= 16 && parts[1] <= 31);
}
inline bool SameOrigin(const Parsed& parsed, std::string_view address, unsigned port) {
    if (!PrivateAddress(address) || port < 1024 || port > 65535) return false;
    const auto authority = std::string(address) + ":" + std::to_string(port);
    return Equal(parsed.host, authority) && (!parsed.post || parsed.origin == "http://" + authority);
}
inline bool Cookie(std::string_view cookies, std::string* token) {
    if (!token) return false; token->clear();
    while (!cookies.empty()) {
        const auto end = cookies.find(';'); const auto part = Trim(cookies.substr(0, end));
        if (part.starts_with("SessionId=")) {
            if (!token->empty() || !HexToken(part.substr(10))) return false; *token = part.substr(10);
        }
        if (end == std::string_view::npos) break; cookies.remove_prefix(end + 1);
    }
    return true;
}
inline bool StaticPath(std::string_view text) {
    if (text.empty() || text.size() > 240) return false;
    unsigned segments = 0;
    while (!text.empty()) {
        const auto slash = text.find('/'); const auto part = text.substr(0, slash);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || ++segments > 8) return false;
        for (unsigned char c : part) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
        const auto stem = part.substr(0, part.find('.'));
        if (Equal(stem, "con") || Equal(stem, "prn") || Equal(stem, "aux") || Equal(stem, "nul") ||
            (stem.size() == 4 && (Equal(stem.substr(0, 3), "com") || Equal(stem.substr(0, 3), "lpt")) && stem[3] >= '0' && stem[3] <= '9')) return false;
        if (slash == std::string_view::npos) break; text.remove_prefix(slash + 1); if (text.empty()) return false;
    }
    return true;
}
}
