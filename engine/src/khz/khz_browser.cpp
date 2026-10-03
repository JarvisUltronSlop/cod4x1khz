#include "khz_browser.h"
#include "khz_content.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Winsock2.h>
#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace khz::browser {
namespace {
bool number(std::string_view text, unsigned maximum, unsigned* value) {
    if (text.empty() || text.size() > 10) return false;
    unsigned parsed = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || parsed > maximum) return false;
    *value = parsed; return true;
}
bool printable(std::string_view text, unsigned maximum) {
    if (text.size() >= maximum) return false;
    for (unsigned char c : text) if (c < 32 || c == 127 || c == '\\') return false;
    return true;
}
struct Socket {
    SOCKET value = INVALID_SOCKET; bool startup = false;
    Socket() { WSADATA data; startup = WSAStartup(MAKEWORD(2, 2), &data) == 0; if (startup) value = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); }
    ~Socket() { if (value != INVALID_SOCKET) closesocket(value); if (startup) WSACleanup(); }
};
sockaddr_in address(const Endpoint& endpoint) {
    sockaddr_in result{}; result.sin_family = AF_INET; result.sin_port = htons(endpoint.port);
    std::memcpy(&result.sin_addr, endpoint.ip.data(), 4); return result;
}
Endpoint endpoint(const sockaddr_in& value) { Endpoint result; result.port = ntohs(value.sin_port); std::memcpy(result.ip.data(), &value.sin_addr, 4); return result; }
bool receive(SOCKET socket, std::uint64_t deadline, const Endpoint& expected, char* buffer, int capacity, int* length) {
    while (GetTickCount64() < deadline) {
        const auto now = GetTickCount64();
        if (now >= deadline) return false;
        const auto remaining = deadline - now;
        timeval timeout{long(remaining / 1000), long((remaining % 1000) * 1000)};
        fd_set set; FD_ZERO(&set); FD_SET(socket, &set);
        const int selected = select(0, &set, nullptr, nullptr, &timeout);
        if (selected <= 0) return false;
        sockaddr_in from{}; int fromLength = sizeof(from);
        const int bytes = recvfrom(socket, buffer, capacity, 0, reinterpret_cast<sockaddr*>(&from), &fromLength);
        if (bytes <= 0 || from.sin_family != AF_INET || !(endpoint(from) == expected)) continue;
        *length = bytes; return true;
    }
    return false;
}
}

bool ParseEndpoint(std::string_view text, Endpoint* result) {
    if (!result || text.empty() || text.size() > 21) return false;
    Endpoint value;
    for (unsigned i = 0; i < 4; ++i) {
        const auto split = text.find(i == 3 ? ':' : '.');
        if (split == text.npos) return false;
        const auto part = text.substr(0, split); unsigned octet = 0;
        if ((part.size() > 1 && part[0] == '0') || !number(part, 255, &octet)) return false;
        value.ip[i] = std::uint8_t(octet); text.remove_prefix(split + 1);
    }
    unsigned port = 0;
    if (!number(text, 65535, &port) || !port || !value.ip[0] || value.ip[0] >= 224) return false;
    value.port = std::uint16_t(port); *result = value; return true;
}
bool FormatEndpoint(const Endpoint& value, char* text, std::size_t capacity) {
    if (!text || !capacity || !value.port) return false;
    const int length = std::snprintf(text, capacity, "%u.%u.%u.%u:%u", value.ip[0], value.ip[1], value.ip[2], value.ip[3], value.port);
    return length >= 0 && std::size_t(length) < capacity;
}
bool PrivateEndpoint(const Endpoint& value) {
    const auto& ip = value.ip;
    return value.port && (ip[0] == 127 || ip[0] == 10 || (ip[0] == 172 && ip[1] >= 16 && ip[1] <= 31) || (ip[0] == 192 && ip[1] == 168));
}
bool ParseInfo(std::string_view packet, const Endpoint& source, const Endpoint& expected, std::string_view challenge, Server* output) {
    if (!output || !(source == expected) || !expected.port || challenge.empty() || challenge.size() > 32 || packet.size() > 4096) return false;
    constexpr char prefix[] = "\xff\xff\xff\xffinfoResponse\n";
    if (!packet.starts_with(std::string_view(prefix, sizeof(prefix) - 1))) return false;
    packet.remove_prefix(sizeof(prefix) - 1);
    if (!packet.empty() && packet.back() == '\n') packet.remove_suffix(1);
    if (packet.find('\n') != packet.npos || packet.find('\r') != packet.npos || packet.find('\0') != packet.npos || packet.empty() || packet[0] != '\\') return false;
    packet.remove_prefix(1);
    std::array<std::string_view, 64> names{}; unsigned count = 0;
    Server result; result.address = expected;
    bool checkedChallenge = false, checkedProtocol = false;
    while (!packet.empty()) {
        const auto split = packet.find('\\');
        if (split == packet.npos || !split || count == names.size()) return false;
        const auto key = packet.substr(0, split); packet.remove_prefix(split + 1);
        const auto next = packet.find('\\'); const auto value = packet.substr(0, next);
        for (unsigned i = 0; i < count; ++i) if (names[i] == key) return false;
        names[count++] = key;
        if (!printable(key, 64) || !printable(value, 1024)) return false;
        if (key == "challenge") { if (value != challenge) return false; checkedChallenge = true; }
        else if (key == "protocol") { if (!number(value, 65535, &result.protocol)) return false; checkedProtocol = true; }
        else if (key == "hostname" || key == "sv_hostname") { if (!printable(value, sizeof(result.hostname))) return false; std::memset(result.hostname, 0, sizeof(result.hostname)); std::memcpy(result.hostname, value.data(), value.size()); }
        else if (key == "mapname") { if (!printable(value, sizeof(result.map))) return false; std::memcpy(result.map, value.data(), value.size()); }
        else if (key == "gametype") { if (!printable(value, sizeof(result.gameType))) return false; std::memcpy(result.gameType, value.data(), value.size()); }
        else if (key == "fs_game" || key == "mod") { if (!printable(value, sizeof(result.mod))) return false; std::memset(result.mod, 0, sizeof(result.mod)); std::memcpy(result.mod, value.data(), value.size()); }
        else if (key == "clients") { if (!number(value, 64, &result.clients)) return false; }
        else if (key == "sv_maxclients") { if (!number(value, 64, &result.maximum)) return false; }
        else if (key == "pure" || key == "sv_pure") { unsigned flag; if (!number(value, 1, &flag)) return false; result.pure = flag != 0; }
        else if (key == "pswrd" || key == "password") { unsigned flag; if (!number(value, 1, &flag)) return false; result.password = flag != 0; }
        if (next == packet.npos) break;
        packet.remove_prefix(next + 1);
    }
    if (!checkedChallenge || !checkedProtocol || !result.maximum || result.clients > result.maximum) return false;
    result.compatible = result.protocol == 10002;
    *output = result; return true;
}
bool ParseMaster(const void* data, std::size_t length, const Endpoint& source, const Endpoint& master,
    std::array<Endpoint, MaxServers>* values, unsigned* count, bool* terminal) {
    constexpr char prefix[] = "\xff\xff\xff\xffgetserversResponse";
    if (!data || !values || !count || !terminal || *count > MaxServers || !(source == master) || !master.port
        || length < sizeof(prefix) - 1 || length > MaxMasterPacket || std::memcmp(data, prefix, sizeof(prefix) - 1)) return false;
    auto result = *values; unsigned size = *count; bool done = false;
    const auto* bytes = static_cast<const unsigned char*>(data); std::size_t at = sizeof(prefix) - 1;
    while (at < length) {
        if (bytes[at++] != '\\') return false;
        if (length - at >= 3 && !std::memcmp(bytes + at, "EOT", 3)) {
            at += 3; done = true;
            while (at < length && bytes[at] == 0) ++at;
            if (at != length) return false;
            break;
        }
        if (length - at < 6) return false;
        Endpoint value; std::memcpy(value.ip.data(), bytes + at, 4); value.port = std::uint16_t(unsigned(bytes[at + 4]) << 8 | bytes[at + 5]); at += 6;
        if (!value.port || !value.ip[0] || value.ip[0] >= 224) return false;
        bool duplicate = false; for (unsigned i = 0; i < size; ++i) if (result[i] == value) { duplicate = true; break; }
        if (!duplicate) { if (size == MaxServers) return false; result[size++] = value; }
    }
    *values = result; *count = size; *terminal = done; return true;
}
bool Query(const Endpoint& target, Server* result, unsigned timeout) {
    if (!result || !target.port || !timeout || timeout > 5000) return false;
    Socket socket; if (socket.value == INVALID_SOCKET) return false;
    std::array<unsigned char, 8> random{};
    if (BCryptGenRandom(nullptr, random.data(), DWORD(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return false;
    char challenge[17]{};
    for (unsigned i = 0; i < random.size(); ++i) std::snprintf(challenge + 2 * i, 3, "%02x", random[i]);
    const std::string request = std::string("\xff\xff\xff\xffgetinfo ") + challenge;
    const auto to = address(target);
    const std::uint64_t start = GetTickCount64(), deadline = start + timeout;
    if (sendto(socket.value, request.data(), int(request.size()), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) != request.size()) return false;
    std::array<char, 4096> buffer{}; int length = 0;
    while (receive(socket.value, deadline, target, buffer.data(), int(buffer.size()), &length)) {
        if (ParseInfo(std::string_view(buffer.data(), length), target, target, challenge, result)) { result->pingMs = unsigned(GetTickCount64() - start); return true; }
    }
    return false;
}
bool QueryMaster(const Endpoint& master, std::array<Endpoint, MaxServers>* values, unsigned* count, unsigned timeout) {
    if (!values || !count || !master.port || !timeout || timeout > 5000) return false;
    Socket socket; if (socket.value == INVALID_SOCKET) return false;
    constexpr char request[] = "\xff\xff\xff\xffgetservers 10002 full empty";
    const auto to = address(master);
    if (sendto(socket.value, request, sizeof(request) - 1, 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) != sizeof(request) - 1) return false;
    std::array<Endpoint, MaxServers> staged{}; unsigned size = 0;
    const auto deadline = GetTickCount64() + timeout;
    std::array<char, MaxMasterPacket> buffer{}; int length = 0; bool terminal = false;
    while (receive(socket.value, deadline, master, buffer.data(), int(buffer.size()), &length)) {
        if (ParseMaster(buffer.data(), length, master, master, &staged, &size, &terminal) && terminal) { *values = staged; *count = size; return true; }
    }
    return false;
}
bool Discover(std::array<Server, MaxLocalServers>* output, unsigned* count, unsigned timeout,
    std::uint16_t firstPort, unsigned portCount) {
    if (!output || !count || !timeout || timeout > 5000 || !firstPort || !portCount || portCount > 8
        || unsigned(firstPort)+portCount-1 > 65535) return false;
    Socket socket; if (socket.value == INVALID_SOCKET) return false;
    const BOOL broadcast = TRUE;
    if (setsockopt(socket.value,SOL_SOCKET,SO_BROADCAST,reinterpret_cast<const char*>(&broadcast),sizeof(broadcast))) return false;
    std::array<unsigned char,8> random{};
    if (BCryptGenRandom(nullptr,random.data(),DWORD(random.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return false;
    char challenge[17]{};
    for (unsigned i=0;i<random.size();++i) std::snprintf(challenge+2*i,3,"%02x",random[i]);
    const std::string request = std::string("\xff\xff\xff\xffgetinfo ")+challenge;
    const auto start = GetTickCount64(), deadline = start+timeout;
    for (unsigned i=0;i<portCount;++i) {
        const Endpoint destinations[]{{{255,255,255,255},std::uint16_t(firstPort+i)},{{127,0,0,1},std::uint16_t(firstPort+i)}};
        for (const auto& destination : destinations) {
            const auto to = address(destination);
            sendto(socket.value,request.data(),int(request.size()),0,reinterpret_cast<const sockaddr*>(&to),sizeof(to));
        }
    }
    std::array<Server,MaxLocalServers> servers{}; unsigned size = 0;
    std::array<char,4096> buffer{};
    while (GetTickCount64() < deadline) {
        const auto now = GetTickCount64(); if (now >= deadline) break;
        const auto remaining = deadline-now;
        timeval wait{long(remaining/1000),long((remaining%1000)*1000)};
        fd_set set; FD_ZERO(&set); FD_SET(socket.value,&set);
        if (select(0,&set,nullptr,nullptr,&wait) <= 0) break;
        sockaddr_in from{}; int fromLength = sizeof(from);
        const int bytes = recvfrom(socket.value,buffer.data(),int(buffer.size()),0,reinterpret_cast<sockaddr*>(&from),&fromLength);
        if (bytes <= 0 || from.sin_family != AF_INET) continue;
        const auto source = endpoint(from);
        if (!PrivateEndpoint(source) || source.port < firstPort || source.port >= unsigned(firstPort)+portCount) continue;
        Server server;
        if (!ParseInfo(std::string_view(buffer.data(),bytes),source,source,challenge,&server) || !server.compatible) continue;
        unsigned index = 0; while (index<size && !(servers[index].address == source)) ++index;
        if (index == MaxLocalServers) continue;
        server.pingMs = unsigned(GetTickCount64()-start);
        servers[index] = server; if (index==size) ++size;
    }
    *output = servers; *count = size; return true;
}
bool Favorites::Add(const Endpoint& endpoint, std::string_view label) {
    if (!endpoint.port || !printable(label, 96)) return false;
    unsigned index = 0; while (index < count_ && !(values_[index].address == endpoint)) ++index;
    if (index == MaxFavorites) return false;
    if (index == count_) ++count_;
    values_[index] = {}; values_[index].address = endpoint; std::memcpy(values_[index].label, label.data(), label.size());
    return true;
}
bool Favorites::Remove(const Endpoint& endpoint) {
    for (unsigned i = 0; i < count_; ++i) if (values_[i].address == endpoint) {
        for (unsigned j = i + 1; j < count_; ++j) values_[j - 1] = values_[j];
        values_[--count_] = {}; return true;
    }
    return false;
}
bool Favorites::Save(const std::filesystem::path& file) const {
    std::string text = "KHZ_FAVORITES_1\n";
    for (unsigned i = 0; i < count_; ++i) {
        char endpoint[32]{};
        if (!FormatEndpoint(values_[i].address, endpoint, sizeof(endpoint)) || !printable(values_[i].label, 96)) return false;
        text += endpoint; text += '\t'; text += values_[i].label; text += '\n';
    }
    return content::AtomicWrite(file, text.data(), text.size());
}
bool Favorites::Load(const std::filesystem::path& file) {
    const DWORD attributes = GetFileAttributesW(file.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    std::error_code error;
    if (std::filesystem::file_size(file, error) > 32768 || error) return false;
    std::ifstream input(file, std::ios::binary);
    std::string line; if (!std::getline(input, line) || line != "KHZ_FAVORITES_1") return false;
    Favorites staged;
    while (std::getline(input, line)) {
        if (line.empty() || line.size() > 128) return false;
        const auto split = line.find('\t'); Endpoint endpoint;
        if (split == line.npos || !ParseEndpoint(std::string_view(line).substr(0, split), &endpoint)
            || !staged.Add(endpoint, std::string_view(line).substr(split + 1))) return false;
    }
    if (!input.eof()) return false;
    *this = staged; return true;
}
}
