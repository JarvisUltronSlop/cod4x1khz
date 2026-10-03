#include "khz_content_protocol.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <limits>

namespace khz::content::wire {
namespace {
bool tokens(std::string_view text, std::array<std::string_view, 14>* output, unsigned expected) {
    if (text.empty() || text.size() > MaximumText || text.front() == ' ' || text.back() == ' ') return false;
    for (unsigned char c : text) if (c <= 32 && c != ' ') return false;
    unsigned count = 0;
    while (!text.empty()) {
        if (count >= expected) return false;
        const auto end = text.find(' ');
        const auto token = text.substr(0,end);
        if (token.empty()) return false;
        (*output)[count++] = token;
        if (end == text.npos) break;
        text.remove_prefix(end+1);
    }
    return count == expected;
}
template<class T> bool number(std::string_view text, T* output, bool nonzero = false) {
    if (text.empty() || text.size() > 20 || (text.size() > 1 && text.front() == '0')) return false;
    T value{}; const auto parsed = std::from_chars(text.data(),text.data()+text.size(),value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data()+text.size() || (nonzero && !value)) return false;
    *output = value; return true;
}
template<std::size_t N> bool copy(std::string_view text, char (&output)[N]) {
    if (text.empty() || text.size() >= N) return false;
    std::memcpy(output,text.data(),text.size()); output[text.size()] = 0; return true;
}
bool valid(const Query& query) {
    return query.id && query.session && query.generation
        && std::memchr(query.relative,0,sizeof(query.relative)) && SafeAssetPath(query.relative)
        && std::string_view(query.relative).find_first_of(" \t\r\n\"") == std::string_view::npos;
}
bool parseIdentity(const std::array<std::string_view,14>& fields, Query* query) {
    return fields[1] == "1" && number(fields[2],&query->id,true)
        && number(fields[3],&query->session,true) && number(fields[4],&query->generation,true);
}
bool decode(std::string_view text, Digest* digest) {
    if (text.size() != 64) return false;
    for (unsigned i=0;i<32;++i) {
        unsigned value = 0;
        for (unsigned j=0;j<2;++j) {
            const char c = text[2*i+j];
            if (c >= '0' && c <= '9') value = value*16+c-'0';
            else if (c >= 'a' && c <= 'f') value = value*16+c-'a'+10;
            else return false;
        }
        (*digest)[i] = std::uint8_t(value);
    }
    return true;
}
std::string encode(const Digest& digest) {
    std::string result(64,'0');
    for (unsigned i=0;i<32;++i) {
        result[2*i] = "0123456789abcdef"[digest[i]>>4];
        result[2*i+1] = "0123456789abcdef"[digest[i]&15];
    }
    return result;
}
bool metadataValid(const Metadata& metadata) {
    if (!valid(metadata.query) || !metadata.bytes || metadata.bytes > MaxFileBytes
        || std::all_of(metadata.sha256.begin(),metadata.sha256.end(),[](unsigned char c){return !c;})
        || !std::memchr(metadata.url,0,sizeof(metadata.url))) return false;
    const std::string_view url(metadata.url), relative(metadata.query.relative);
    if (url.empty() || url.find_first_of(" \t\r\n\"") != url.npos || (!url.starts_with("https://") && !url.starts_with("http://"))) return false;
    const bool iwd = relative.ends_with(".iwd");
    return metadata.checkPure == iwd && (iwd || (!metadata.checksum && !metadata.pureChecksum));
}
std::string identity(const Query& query) {
    return "1 " + std::to_string(query.id) + " " + std::to_string(query.session) + " " + std::to_string(query.generation);
}
}
bool ParseQuery(std::string_view text, Query* output) {
    if (!output) return false;
    Query query; std::array<std::string_view,14> fields{};
    if (!tokens(text,&fields,6) || fields[0] != "khz_content" || !parseIdentity(fields,&query)
        || !copy(fields[5],query.relative) || !valid(query)) return false;
    *output = query; return true;
}
bool FormatQuery(const Query& query, std::string* output) {
    if (!output || !valid(query)) return false;
    *output = "khz_content " + identity(query) + " " + query.relative; return output->size() <= MaximumText;
}
bool ParseMetadata(std::string_view text, Metadata* output) {
    if (!output) return false;
    Metadata metadata; std::array<std::string_view,14> fields{};
    unsigned pure = 0;
    if (!tokens(text,&fields,13) || fields[0] != "khz_content_meta" || !parseIdentity(fields,&metadata.query)
        || !number(fields[5],&metadata.bytes,true) || !decode(fields[6],&metadata.sha256)
        || !number(fields[7],&metadata.checksum) || !number(fields[8],&metadata.pureChecksum)
        || !number(fields[9],&metadata.checksumFeed) || !number(fields[10],&pure) || pure > 1
        || !copy(fields[11],metadata.query.relative) || !copy(fields[12],metadata.url)) return false;
    metadata.checkPure = pure != 0;
    if (!metadataValid(metadata)) return false;
    *output = metadata; return true;
}
bool FormatMetadata(const Metadata& metadata, std::string* output) {
    if (!output || !metadataValid(metadata)) return false;
    *output = "khz_content_meta " + identity(metadata.query) + " " + std::to_string(metadata.bytes) + " "
        + encode(metadata.sha256) + " " + std::to_string(metadata.checksum) + " " + std::to_string(metadata.pureChecksum)
        + " " + std::to_string(metadata.checksumFeed) + " " + (metadata.checkPure ? "1 " : "0 ")
        + metadata.query.relative + " " + metadata.url;
    return output->size() <= MaximumText;
}
bool ParseRejection(std::string_view text, Rejection* output) {
    if (!output) return false;
    Rejection rejection; std::array<std::string_view,14> fields{}; unsigned error = 0;
    if (!tokens(text,&fields,6) || fields[0] != "khz_content_error" || !parseIdentity(fields,&rejection.query)
        || !number(fields[5],&error,true) || error > unsigned(Error::Storage)) return false;
    rejection.error = Error(error); *output = rejection; return true;
}
bool FormatRejection(const Rejection& rejection, std::string* output) {
    const auto& query = rejection.query; const unsigned error = unsigned(rejection.error);
    if (!output || !query.id || !query.session || !query.generation || !error || error > unsigned(Error::Storage)) return false;
    *output = "khz_content_error " + identity(query) + " " + std::to_string(error); return true;
}
bool MakeRequest(const Metadata& metadata, const Query& pending, std::string_view origin,
    std::uint32_t checksumFeed, Request* output) {
    const auto& query = metadata.query;
    Request request;
    if (!output || !metadataValid(metadata) || !valid(pending) || query.id != pending.id
        || query.session != pending.session || query.generation != pending.generation
        || std::strcmp(query.relative,pending.relative) || metadata.checksumFeed != checksumFeed
        || origin.empty() || origin.size() >= sizeof(request.origin) || !ApprovedUrl(metadata.url,origin)) return false;
    request.id = query.id; request.session = query.session; request.generation = query.generation;
    request.expectedBytes = metadata.bytes; request.sha256 = metadata.sha256;
    request.checkPure = metadata.checkPure; request.checksum = metadata.checksum;
    request.pureChecksum = metadata.pureChecksum; request.checksumFeed = metadata.checksumFeed;
    std::strcpy(request.relative,query.relative); std::strcpy(request.url,metadata.url);
    std::memcpy(request.origin,origin.data(),origin.size()); request.origin[origin.size()] = 0;
    *output = request; return true;
}
}
