#include <khz/khz_content.h>
#include <khz/khz_browser.h>
#include <khz/khz_update.h>
#include <khz/khz_update_policy.h>
#include <khz/khz_update_bootstrap.h>
#include <khz/khz_content_protocol.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace {
unsigned cases = 0, failures = 0;
void check(bool value, const char* name) { ++cases; if (!value) { ++failures; std::printf("FAIL: %s\n", name); } }
std::string hex(const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data); std::string text(size*2, '0');
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t i = 0; i < size; ++i) { text[2*i] = digits[bytes[i]>>4]; text[2*i+1] = digits[bytes[i]&15]; } return text;
}
bool digest(std::string_view text, khz::content::Digest* value) {
    if (text.size() != 64) return false;
    for (unsigned i = 0; i < 32; ++i) {
        unsigned byte = 0; const auto result = std::from_chars(text.data()+2*i, text.data()+2*i+2, byte, 16);
        if (result.ec != std::errc{} || result.ptr != text.data()+2*i+2) return false; (*value)[i] = static_cast<unsigned char>(byte);
    }
    return true;
}
std::vector<unsigned char> image(unsigned variant, bool custom = true) {
    std::vector<unsigned char> bytes(1024, 0);
    auto u16 = [&](std::size_t at, std::uint16_t value) { std::memcpy(bytes.data()+at, &value, 2); };
    auto u32 = [&](std::size_t at, std::uint32_t value) { std::memcpy(bytes.data()+at, &value, 4); };
    bytes[0] = 'M'; bytes[1] = 'Z'; u32(60,128); std::memcpy(bytes.data()+128, "PE\0\0",4);
    u16(132,0x14c); u16(134,1); u16(148,224); u16(150,0x102);
    const unsigned optional = 152, section = optional+224;
    u16(optional,0x10b); u32(optional+16,4096); u32(optional+56,8192); u32(optional+60,512); u16(optional+68,3);
    std::memcpy(bytes.data()+section,".text",5); u32(section+8,512); u32(section+12,4096); u32(section+16,512); u32(section+20,512); u32(section+36,0x60000020);
    bytes[512] = 0xc3; bytes[513] = static_cast<unsigned char>(variant);
    if (custom) std::memcpy(bytes.data()+600, khz::update::BinaryIdentity(), std::strlen(khz::update::BinaryIdentity()));
    return bytes;
}
bool write(const std::filesystem::path& file, const std::vector<unsigned char>& bytes) { return khz::content::AtomicWrite(file, bytes.data(), bytes.size()); }
struct Signing {
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_KEY_HANDLE key = nullptr;
    ~Signing() { if (key) BCryptDestroyKey(key); if (algorithm) BCryptCloseAlgorithmProvider(algorithm,0); }
    bool Create(khz::update::PublicKey* output) {
        ULONG length = 0;
        return BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_RSA_ALGORITHM,nullptr,0)>=0
            && BCryptGenerateKeyPair(algorithm,&key,2048,0)>=0 && BCryptFinalizeKeyPair(key,0)>=0
            && BCryptExportKey(key,nullptr,BCRYPT_RSAPUBLIC_BLOB,output->data(),ULONG(output->size()),&length,0)>=0 && length==output->size();
    }
    bool Sign(khz::update::Manifest* manifest) {
        std::string text; khz::content::Digest hash{}; ULONG length=0;
        BCRYPT_PKCS1_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM};
        return khz::update::CanonicalBody(*manifest,&text) && khz::content::Sha256(text.data(),text.size(),&hash)
            && BCryptSignHash(key,&padding,hash.data(),ULONG(hash.size()),manifest->signature.data(),ULONG(manifest->signature.size()),&length,BCRYPT_PAD_PKCS1)>=0 && length==manifest->signature.size();
    }
};
std::string document(const khz::update::Manifest& manifest) {
    std::string text; khz::update::CanonicalBody(manifest,&text); text += "signature="+hex(manifest.signature.data(),256)+"\n"; return text;
}
bool wait(khz::content::Completion* completion, unsigned timeout=5000) {
    const auto deadline=GetTickCount64()+timeout;
    while (GetTickCount64()<deadline) { if(khz::content::Poll(completion)) return true; Sleep(2); } return false;
}
void basic(const std::filesystem::path& root) {
    using namespace khz;
    content::Digest hash{};
    check(content::Sha256("abc",3,&hash) && hex(hash.data(),32)=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA256 standard vector");
    check(content::Crc32("123456789",9)==0xcbf43926u,"stock CRC32 vector");
    check(content::SafeAssetPath("mods/fixture/fixture.iwd") && content::SafeAssetPath("usermaps/mp_fixture/mp_fixture.ff"),"mod and usermap paths");
    check(!content::SafeAssetPath("../escape.iwd") && !content::SafeAssetPath("mods/fixture/upstream.exe")
        && !content::SafeAssetPath("main/iw_00.iwd") && !content::SafeAssetPath("mods/CON/file.iwd"),"traversal, binary, stock asset and device path rejected");
    check(!content::SafeAssetPath("main/IW_00.iwd")&&!content::SafeAssetPath("main/iW_00.iwd")
        &&!content::SafeAssetPath("mods/cOn/file.iwd")&&!content::SafeAssetPath("mods/lPt9.fixture/file.iwd"),"Windows case aliases cannot replace stock assets or use reserved devices");
    check(content::SafeAssetPath("usermaps/mp_fixture/mp_fixture_load.ff")&&content::SafeAssetPath("usermaps/mp_fixture/mp_fixture.iwd")
        &&!content::SafeAssetPath("usermaps/mp_fixture/mp_other.ff")&&!content::SafeAssetPath("mods/fixture/nested/file.iwd"),"bounded allocation-free usermap suffix and component checks");
    check(!content::ApprovedUrl("https://example.com/a.iwd","") && !content::ApprovedUrl("http://example.com/a.iwd","http://example.com"),"unconfigured origin and public plaintext denied");
    check(content::ApprovedUrl("https://example.com/a.iwd","https://example.com")
        && !content::ApprovedUrl("https://evil.example/a.iwd","https://example.com")
        && !content::ApprovedUrl("https://user@example.com/a.iwd","https://example.com"),"exact configured origin and no embedded credentials");
    std::filesystem::path confined;
    check(content::ConfinedPath(root,"mods/fixture/file.iwd",&confined,true) && !content::ConfinedPath(root,"../escape",&confined,true),"project confinement");
    browser::Endpoint endpoint{};
    check(browser::ParseEndpoint("127.0.0.1:28961",&endpoint) && browser::PrivateEndpoint(endpoint),"canonical private endpoint");
    check(!browser::ParseEndpoint("127.00.0.1:28961",&endpoint) && !browser::ParseEndpoint("127.0.0.1:0",&endpoint),"ambiguous or zero endpoint denied");
    browser::ParseEndpoint("127.0.0.1:28961",&endpoint);
    browser::Server server;
    const std::string info="\xff\xff\xff\xffinfoResponse\n\\challenge\\abc\\protocol\\10002\\hostname\\Fixture\\clients\\1\\sv_maxclients\\4";
    check(browser::ParseInfo(info,endpoint,endpoint,"abc",&server) && server.compatible && server.clients==1,"info challenge/protocol parsing");
    auto source=endpoint; ++source.port;
    check(!browser::ParseInfo(info,source,endpoint,"abc",&server) && !browser::ParseInfo(info,endpoint,endpoint,"wrong",&server),"source port and challenge rejection");
    check(!browser::ParseInfo(info+"\\clients\\2",endpoint,endpoint,"abc",&server),"duplicate info fields rejected");
    browser::Favorites favorites, loaded;
    check(favorites.Add(endpoint,"Fixture") && favorites.Save(root/"favorites.txt") && loaded.Load(root/"favorites.txt") && loaded.Count()==1,"favorites atomic persistence");
    check(loaded.Add(endpoint,"Renamed") && loaded.Count()==1 && loaded.Remove(endpoint) && !loaded.Count(),"favorite update/remove uniqueness");
    check(!loaded.Add(endpoint,"bad\nlabel"),"favorite newline injection rejected");
}
void contentProtocol() {
    using namespace khz::content;
    wire::Query query; query.id=1; query.session=2; query.generation=3;
    std::strcpy(query.relative,"mods/fixture/package.iwd");
    std::string text; wire::Query parsed;
    check(wire::FormatQuery(query,&text)&&wire::ParseQuery(text,&parsed)&&!std::strcmp(query.relative,parsed.relative),"strict content query roundtrip");
    for(const auto* invalid:{"khz_content 1 01 2 3 mods/fixture/package.iwd","khz_content 1 1 0 3 mods/fixture/package.iwd",
        "khz_content 1 1 2 3 ../package.iwd","khz_content 1 1 2 3 mods/fixture/package.iwd extra",
        "khz_content 1 1 2 3 mods/fixture/package.iwd\n","khz_content 1 1 2 3  mods/fixture/package.iwd"})
        check(!wire::ParseQuery(invalid,&parsed),"noncanonical or unsafe content request rejected");
    wire::Metadata metadata; metadata.query=query; metadata.bytes=100;
    metadata.sha256.fill(3); metadata.checkPure=true; metadata.checksum=4; metadata.pureChecksum=5; metadata.checksumFeed=6;
    std::strcpy(metadata.url,"http://127.0.0.1:28999/package.iwd"); wire::Metadata decoded;
    check(wire::FormatMetadata(metadata,&text)&&wire::ParseMetadata(text,&decoded)&&decoded.sha256==metadata.sha256,"strict complete SHA/pure metadata roundtrip");
    Request request;
    check(wire::MakeRequest(decoded,query,"http://127.0.0.1:28999",6,&request)&&request.checkPure&&request.generation==3,"metadata bound to pending custom session/path/feed/origin");
    auto stale=query; ++stale.session;
    check(!wire::MakeRequest(decoded,stale,"http://127.0.0.1:28999",6,&request),"cross-session content response rejected");
    stale=query; ++stale.generation;
    check(!wire::MakeRequest(decoded,stale,"http://127.0.0.1:28999",6,&request),"old-map content response rejected");
    stale=query; std::strcpy(stale.relative,"mods/fixture/other.iwd");
    check(!wire::MakeRequest(decoded,stale,"http://127.0.0.1:28999",6,&request),"different requested content path rejected");
    check(!wire::MakeRequest(decoded,query,"http://127.0.0.1:29000",6,&request)
        &&!wire::MakeRequest(decoded,query,"http://127.0.0.1:28999",7,&request),"server cannot change trusted origin or checksum feed");
    auto invalid=metadata; invalid.checkPure=false;
    check(!wire::FormatMetadata(invalid,&text),"IWD requires stock pure verification contract");
    invalid=metadata; std::strcpy(invalid.query.relative,"mods/fixture/mod.ff"); invalid.checkPure=false;
    check(!wire::FormatMetadata(invalid,&text),"FF cannot carry inconsistent IWD checksums");
    invalid.checksum=invalid.pureChecksum=0;
    check(wire::FormatMetadata(invalid,&text)&&wire::ParseMetadata(text,&decoded),"FF SHA/size metadata keeps accepted feed without IWD checksums");
    wire::Rejection rejection; rejection.query=query; rejection.error=wire::Error::Busy; wire::Rejection error;
    check(wire::FormatRejection(rejection,&text)&&wire::ParseRejection(text,&error)&&error.error==wire::Error::Busy,"bounded content rejection roundtrip");
}
void updates(const std::filesystem::path& root, const std::string& origin, const std::filesystem::path& fixtureData) {
    using namespace khz;
    std::filesystem::create_directories(root/"incoming");
    update::Policy policy;
    std::snprintf(policy.origin,sizeof(policy.origin),"%s",origin.c_str());
    std::snprintf(policy.manifestUrl,sizeof(policy.manifestUrl),"%s/manifest.txt",origin.c_str());
    policy.privateHttpFixture=true;
    Signing signing;
    check(!update::Configured(policy),"update disabled without explicit trusted key");
    if (!signing.Create(&policy.trustedKey)) { check(false,"RSA fixture key generation"); return; }
    {
        update::Policy parsed,disabled;std::string encoded,reason;
        const std::string empty="KHZ_UPDATE_POLICY_1\nproject=cod4x1khz\norigin=\nmanifest_url=\ntrusted_key_cng_rsa2048=\nprivate_http_fixture=0\n";
        check(update::ParsePolicy(empty,false,&disabled,&reason)&&!update::Configured(disabled),"canonical empty policy disables every update endpoint");
        check(update::FormatPolicy(policy,&encoded)&&update::ParsePolicy(encoded,true,&parsed)
            &&parsed.trustedKey==policy.trustedKey&&!std::strcmp(parsed.origin,policy.origin),"canonical trusted fixture policy roundtrip");
        check(!update::ParsePolicy(encoded,false,&parsed),"private HTTP policy cannot enter production mode");
        std::string crlf;for(char c:empty){if(c=='\n')crlf+='\r';crlf+=c;}
        check(update::ParsePolicy(crlf,false,&parsed),"Windows CRLF policy is accepted without changing values");
        auto malformed=encoded;malformed.replace(malformed.find("project=cod4x1khz"),17,"project=upstream");
        check(!update::ParsePolicy(malformed,true,&parsed),"cross-project policy rejected");
        check(!update::ParsePolicy(empty+"origin=\n",false,&parsed)&&!update::ParsePolicy(empty.substr(0,empty.size()-1),false,&parsed),"duplicate fields and missing final newline rejected");
        malformed=encoded;malformed[malformed.find("trusted_key_cng_rsa2048=")+24]='Z';
        check(!update::ParsePolicy(malformed,true,&parsed),"noncanonical key hex rejected");
        malformed=empty;malformed.insert(0,"\xef\xbb\xbf");check(!update::ParsePolicy(malformed,false,&parsed),"policy BOM rejected");
        malformed=empty;malformed[4]=0;check(!update::ParsePolicy(malformed,false,&parsed),"embedded NUL policy rejected");
        check(!update::ParsePolicy(std::string(4097,'x'),false,&parsed),"bounded policy size");
        auto production=policy;production.privateHttpFixture=false;
        std::strcpy(production.origin,"https://updates.example");std::strcpy(production.manifestUrl,"https://updates.example/manifest.txt");
        check(update::FormatPolicy(production,&encoded)&&update::ParsePolicy(encoded,false,&parsed)&&update::Configured(parsed),"explicit origin-pinned HTTPS production policy");
        std::strcpy(production.manifestUrl,"https://other.example/manifest.txt");
        check(!update::FormatPolicy(production,&encoded),"production manifest cannot escape configured origin");
        const auto fresh=root/"policy-fixture";std::filesystem::create_directory(fresh);std::uint64_t version=999;
        check(update::ReadInstalledVersion(fresh,&version)&&version==0&&!std::filesystem::exists(fresh/"updates"),"fresh version read stays disabled and creates no update directory");
        std::filesystem::create_directories(fresh/"config");
        check(content::AtomicWrite(fresh/"config/update-policy.txt",empty.data(),empty.size())&&update::LoadPolicy(fresh,false,&parsed),"confined canonical policy loads");
        std::filesystem::create_directory(fresh/"updates");const std::string badVersion="01\n";
        check(content::AtomicWrite(fresh/"updates/version.txt",badVersion.data(),badVersion.size())&&!update::ReadInstalledVersion(fresh,&version),"noncanonical installed version rejected");
        const std::string goodVersion="42\n";
        check(content::AtomicWrite(fresh/"updates/version.txt",goodVersion.data(),goodVersion.size())&&update::ReadInstalledVersion(fresh,&version)&&version==42,"canonical installed version loads");
        check(update::QuoteArgument(L"a b")==L"\"a b\""&&update::QuoteArgument(L"a\\")==L"\"a\\\\\""
            &&update::QuoteArgument(L"a\"b")==L"\"a\\\"b\"","Windows restart argument quoting preserves spaces, trailing slash and quotes");
        HANDLE first=update::AcquireProjectMutex(fresh,0);
        check(first!=nullptr,"project update mutex acquired");if(first){ReleaseMutex(first);CloseHandle(first);}
        check(update::WaitForStopped(fresh,0,1000)&&!update::WaitForStopped(fresh,GetCurrentProcessId(),1000),"stopped-process gate refuses an invoking image outside installed paths");
    }
    update::Manifest manifest; manifest.version=1;
    std::array<std::filesystem::path,2> sources{};
    std::filesystem::create_directories(root/"runtime");
    std::array<content::Digest,4> oldHashes{};
    for(unsigned i=0;i<update::InstalledPaths.size();++i) {
        const auto old=image(i);
        check(write(root/update::InstalledPaths[i],old),"old root/runtime fixture PE creation");
        content::Sha256(old.data(),old.size(),&oldHashes[i]);
    }
    for(unsigned i=0;i<2;++i) {
        const auto next=image(i+10);
        sources[i]=root/"incoming"/(i?"server.bin":"client.bin"); check(write(sources[i],next),"new fixture PE creation");
        std::strcpy(manifest.files[i].name,update::PackagePaths[i]); manifest.files[i].bytes=next.size();
        content::Sha256(next.data(),next.size(),&manifest.files[i].sha256);
        std::snprintf(manifest.files[i].url,sizeof(manifest.files[i].url),"%s/%s",origin.c_str(),i ? "server.bin":"client.bin");
        if(!fixtureData.empty()) check(write(fixtureData/(i?"server.bin":"client.bin"),next),"HTTP signed binary fixture creation");
    }
    check(signing.Sign(&manifest),"RSA signed custom pair");
    auto text=document(manifest); update::Manifest verified;
    check(update::VerifyManifest(text,policy,0,&verified),"signed manifest verify");
    check(!update::VerifyManifest(text,policy,1,&verified),"same/downgrade release rejection");
    auto modified=text; modified[modified.find("cod4x1khz")]='X';
    check(!update::VerifyManifest(modified,policy,0,&verified),"crossproject and signature tamper rejection");
    modified=text; modified[modified.find("signature=")+10]^=1;
    check(!update::VerifyManifest(modified,policy,0,&verified),"invalid update signature rejection");
    modified=text; modified[modified.find("client_mirrors=")+14]='X';
    check(!update::VerifyManifest(modified,policy,0,&verified),"unauthorized mirror path rejection");
    auto wrongLayout=manifest; std::strcpy(wrongLayout.files[1].name,"cod4x1khz_server.exe");
    std::string wrongBody;
    check(!update::CanonicalBody(wrongLayout,&wrongBody),"root-level server package rejected");
    check(update::VerifyBinary(sources[0]),"custom x86 PE identity");
    check(write(root/"upstream.exe",image(99,false)) && !update::VerifyBinary(root/"upstream.exe"),"renamed upstream PE without custom identity denied");
    auto invalid=image(99); invalid[132]=0x64; invalid[133]=0x86;
    check(write(root/"x64.exe",invalid) && !update::VerifyBinary(root/"x64.exe"),"wrong architecture rejected");
    if(!fixtureData.empty()) {
        check(content::AtomicWrite(fixtureData/"manifest.txt",text.data(),text.size()),"signed HTTP manifest fixture publish");
        check(update::FetchManifest(policy,0,&verified),"configured signed manifest HTTP fixture");
    }
    const auto oldSetMatches=[&]() {
        for(unsigned i=0;i<update::InstalledPaths.size();++i) {
            content::Digest actual{};
            if(!content::HashFile(root/update::InstalledPaths[i],&actual)||actual!=oldHashes[i]) return false;
        }
        return true;
    };
    const auto newSetMatches=[&]() {
        for(unsigned i=0;i<update::InstalledPaths.size();++i) {
            content::Digest actual{};
            if(!content::HashFile(root/update::InstalledPaths[i],&actual)||actual!=manifest.files[update::InstalledSourceIndexes[i]].sha256) return false;
        }
        return true;
    };
    check(update::InspectRecordedState(root,policy)==update::RecordedState::None&&!std::filesystem::exists(root/"updates"),"read-only inspection preserves fresh root without a journal");
    for(unsigned step=1;step<=update::InstalledPaths.size();++step) {
        const auto result=update::Apply(root,manifest,policy,sources,0,step);
        check(!result.applied&&result.rolledBack,"failure after any replacement rolls back entire four-path set");
        check(oldSetMatches(),"each rollback restores all distinct old root/runtime bytes");
        check(update::Recover(root,policy).rolledBack,"each failed replacement has recoverable rolled-back journal");
        check(update::InspectRecordedState(root,policy)==update::RecordedState::Healthy,"healthy rolled-back journal permits later native startup without recovery helper");
    }
    auto result=update::Apply(root,manifest,policy,sources,0);
    check(result.applied && !result.rolledBack,"verified custom update commit");
    check(newSetMatches(),"client mirrors contain verified client bytes and server stays runtime");
    check(update::InspectRecordedState(root,policy)==update::RecordedState::Healthy,"healthy committed journal permits managed child startup without stopping parent");
    const std::string wrongVersion="2\n";
    check(content::AtomicWrite(root/"updates/version.txt",wrongVersion.data(),wrongVersion.size())
        &&update::InspectRecordedState(root,policy)==update::RecordedState::RecoveryRequired,"altered version requires stopped recovery");
    check(update::Recover(root,policy).applied,"startup committed transaction verification");
    const auto altered=image(99);
    check(write(root/update::InstalledPaths[2],altered)&&update::InspectRecordedState(root,policy)==update::RecordedState::RecoveryRequired,"altered mirror requires stopped recovery without replacing a running image");
    content::Digest inspectedHash{};
    check(content::HashFile(root/update::InstalledPaths[2],&inspectedHash)&&inspectedHash!=manifest.files[0].sha256,"read-only mismatch inspection leaves altered binary untouched");
    check(write(root/update::InstalledPaths[2],image(10)),"fixture restores verified mirror before explicit rollback");
    check(update::Rollback(root,policy).rolledBack,"explicit verified local rollback");
    check(oldSetMatches(),"explicit rollback restores all four original binaries");
    check(update::Recover(root,policy).rolledBack,"startup rolled-back transaction verification");
    check(update::InspectRecordedState(root,policy)==update::RecordedState::Healthy,"restored complete rolled-back state verified without writes");
    check(DeleteFileW((root/update::InstalledPaths[2]).c_str())&&DeleteFileW((root/update::InstalledPaths[3]).c_str()),"fixture starts without optional runtime mirrors");
    result=update::Apply(root,manifest,policy,sources,0,4);
    check(result.rolledBack&&!std::filesystem::exists(root/update::InstalledPaths[2])&&!std::filesystem::exists(root/update::InstalledPaths[3]),"failure restores optional mirrors to absent state");
    check(update::Recover(root,policy).rolledBack,"startup verifies absent mirrors after rollback");
    result=update::Apply(root,manifest,policy,sources,0);
    check(result.applied&&newSetMatches(),"successful update creates missing authorized mirrors from client bytes");
    check(update::Rollback(root,policy).rolledBack&&!std::filesystem::exists(root/update::InstalledPaths[2])&&!std::filesystem::exists(root/update::InstalledPaths[3]),"explicit rollback removes only newly created verified mirrors");
}
void network(const std::filesystem::path& root,const std::string& origin,const char* udpEndpoint) {
    using namespace khz;
    std::vector<std::uint8_t> document;
    check(content::ReadDocument(origin+"/fixture.txt",origin,&document),"bounded fixture metadata HTTP");
    check(!content::ReadDocument(origin+"/huge-document",origin,&document),"oversized HTTP document bounded");
    std::string metadata(reinterpret_cast<const char*>(document.data()),document.size());
    content::Request request; request.session=1; request.generation=7; request.checkPure=true;
    std::snprintf(request.origin,sizeof(request.origin),"%s",origin.c_str());
    std::strcpy(request.relative,"mods/fixture/fixture.iwd");
    std::size_t at=0;
    while(at<metadata.size()) {
        const auto end=metadata.find('\n',at), split=metadata.find('=',at); if(end==metadata.npos||split>=end) break;
        const auto key=metadata.substr(at,split-at); const auto value=std::string_view(metadata).substr(split+1,end-split-1);
        std::uint64_t number=0; std::from_chars(value.data(),value.data()+value.size(),number);
        if(key=="size") request.expectedBytes=number;
        else if(key=="sha256") check(digest(value,&request.sha256),"fixture SHA256 decode");
        else if(key=="checksum") request.checksum=std::uint32_t(number);
        else if(key=="pure") request.pureChecksum=std::uint32_t(number);
        else if(key=="feed") request.checksumFeed=std::uint32_t(number);
        at=end+1;
    }
    std::filesystem::create_directories(root/"assets"); std::filesystem::create_directories(root/"downloads");
    check(content::Start(root/"assets",root/"downloads"),"off-tick bounded download worker start");
    content::Completion completion;
    auto transfer=[&](const char* route,content::Status expected,unsigned attempts) {
        ++request.id; std::snprintf(request.url,sizeof(request.url),"%s/%s",origin.c_str(),route);
        const bool submitted=content::Submit(request);
        const bool received=submitted&&wait(&completion);
        check(received&&completion.status==expected&&completion.attempts==attempts,route);
        return received;
    };
    if(transfer("asset.iwd",content::Status::Staged,1)) {
        check(!content::Commit(completion,2,7),"staged content cannot cross session");
        check(content::Commit(completion,1,7),"verified pure IWD activation under explicit FS barrier");
        std::uint32_t checksum=0,pure=0;
        check(content::VerifyIwd(root/"assets/mods/fixture/fixture.iwd",request.checksumFeed,&checksum,&pure)
            && checksum==request.checksum&&pure==request.pureChecksum,"stock IWD and feed checksum parity");
    }
    transfer("retry.iwd",content::Status::Staged,3);
    transfer("wrong.iwd",content::Status::Hash,1);
    transfer("oversize.iwd",content::Status::Size,3);
    ++request.id; std::snprintf(request.url,sizeof(request.url),"%s/truncated.iwd",origin.c_str());
    check(content::Submit(request)&&wait(&completion)&&completion.status!=content::Status::Staged&&completion.attempts==3,"truncated transfer retries bounded");
    transfer("redirect",content::Status::Network,3);
    ++request.checksum; transfer("asset.iwd",content::Status::Pure,1); --request.checksum;
    ++request.id; std::snprintf(request.url,sizeof(request.url),"%s/slow.iwd",origin.c_str());
    check(content::Submit(request),"slow transfer submitted"); Sleep(80); content::Cancel(1,7);
    check(wait(&completion)&&completion.status==content::Status::Cancelled,"lifecycle cancellation");
    transfer("asset.iwd",content::Status::Staged,1); content::Cancel(1,7);
    check(!content::Commit(completion,1,7),"cancellation after staging blocks activation");
    check(content::Discard(completion),"cancelled staged content cleanup");
    ++request.id; std::snprintf(request.url,sizeof(request.url),"%s/slow.iwd",origin.c_str());
    const auto slowId=request.id;
    check(content::Submit(request),"queue saturation slow worker fixture");
    const auto deadline=GetTickCount64()+500;
    while(content::GetProgress().id!=slowId&&GetTickCount64()<deadline) Sleep(1);
    bool filled=true;
    for(unsigned i=0;i<content::QueueCapacity;++i) { ++request.id; filled &= content::Submit(request); }
    ++request.id;
    check(filled&&!content::Submit(request),"bounded download queue full rejection");
    content::Cancel(0,0);
    unsigned cancelled=0;
    for(unsigned i=0;i<content::QueueCapacity+1;++i) if(wait(&completion)&&completion.status==content::Status::Cancelled) ++cancelled;
    check(cancelled==content::QueueCapacity+1,"bounded queued/active cancellation drains every completion");
    content::Request binary;
    binary.id=++request.id; binary.session=9; binary.generation=1; const auto bytes=image(10); binary.expectedBytes=bytes.size();
    content::Sha256(bytes.data(),bytes.size(),&binary.sha256);
    std::snprintf(binary.origin,sizeof(binary.origin),"%s",origin.c_str()); std::snprintf(binary.url,sizeof(binary.url),"%s/client.bin",origin.c_str());
    std::filesystem::path staged;
    check(content::SubmitVerifiedBlob(binary)&&wait(&completion)&&completion.status==content::Status::Staged
        && content::StagedPath(completion,9,1,&staged)&&!content::Commit(completion,9,1),"verified update blobs stage separately and cannot enter asset FS");
    check(content::Discard(completion),"verified update staging cleanup");
    content::Stop();
    browser::Endpoint endpoint; browser::Server server;
    check(browser::ParseEndpoint(udpEndpoint,&endpoint)&&browser::Query(endpoint,&server)&&server.compatible,"UDP query filters wrong source/challenge then accepts exact reply");
    std::array<browser::Server,browser::MaxLocalServers> discovered{}; unsigned localCount=0;
    check(browser::Discover(&discovered,&localCount,150,endpoint.port,1)&&localCount==1
        &&discovered[0].address==endpoint&&discovered[0].compatible,"private discovery verifies random challenge/source port and deduplicates responses");
    std::array<browser::Endpoint,browser::MaxServers> values{}; unsigned count=0;
    check(browser::QueryMaster(endpoint,&values,&count)&&count==2,"configured master fragmented replies deduplicated");
    ++endpoint.port;
    check(!browser::Query(endpoint,&server,80),"absolute UDP timeout on unavailable endpoint");
}
}
int main(int argc,char** argv) {
    const bool fixtures=argc==5&&std::strcmp(argv[1],"--fixture")==0;
    if(argc!=1&&!fixtures) { std::fprintf(stderr,"Usage: khz_services_tests [--fixture HTTP_ORIGIN UDP_ENDPOINT DATA_DIR]\n"); return 2; }
    const auto root=std::filesystem::current_path()/L"service-test-results"/(std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root);
    basic(root);
    contentProtocol();
    updates(root,fixtures?argv[2]:"http://127.0.0.1:28999",fixtures?std::filesystem::path(argv[4]):std::filesystem::path{});
    if(fixtures) network(root,argv[2],argv[3]);
    std::printf("KHZ_SERVICES_TEST {\"passed\":%s,\"cases\":%u,\"failures\":%u,\"network_fixtures\":%s,\"scope\":\"isolated service modules; native game adapters untested\"}\n",failures?"false":"true",cases,failures,fixtures?"true":"false");
    return failures?1:0;
}
