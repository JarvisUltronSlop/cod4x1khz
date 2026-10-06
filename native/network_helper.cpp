#include "desktop_ui.hpp"
#include "server_profile.hpp"
#include <netfw.h>
#include <natupnp.h>
#include <winhttp.h>
#include <ws2tcpip.h>
#include <fstream>
#include <sstream>

using namespace khz::hosting;
namespace {
template <class T> struct Com {
    T *p = nullptr;
    ~Com() {
        if (p)
            p->Release();
    }
    T **operator&() { return &p; }
    T *operator->() { return p; }
};
struct Bstr {
    BSTR value;
    explicit Bstr(std::wstring_view text)
        : value(SysAllocStringLen(text.data(), UINT(text.size()))) {}
    ~Bstr() { SysFreeString(value); }
    operator BSTR() const { return value; }
};
std::string publicIp() {
    HINTERNET session =
        WinHttpOpen(L"cod4x1khz/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!session)
        return {};
    WinHttpSetTimeouts(session, 3000, 3000, 3000, 5000);
    auto connection = WinHttpConnect(session, L"api.ipify.org", INTERNET_DEFAULT_HTTPS_PORT, 0);
    auto request = connection
                       ? WinHttpOpenRequest(connection, L"GET", L"/", nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                       : nullptr;
    std::string ip;
    if (request &&
        WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0,
                           0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0, bytes = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr,
                            &status, &bytes, nullptr);
        char data[64];
        DWORD read = 0;
        if (status == 200 && WinHttpReadData(request, data, 63, &read)) {
            ip.assign(data, read);
            if (!PublicIPv4(ip))
                ip.clear();
        }
    }
    if (request)
        WinHttpCloseHandle(request);
    if (connection)
        WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ip;
}
std::string firewall(const std::filesystem::path &root, const Profile &p) {
    Com<INetFwPolicy2> policy;
    if (FAILED(CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&policy.p))))
        return "Firewall policy is unavailable; configure UDP manually";
    Com<INetFwRules> rules;
    if (FAILED(policy->get_Rules(&rules.p)))
        return "Cannot access firewall rules";
    Com<INetFwRule> rule;
    if (FAILED(CoCreateInstance(__uuidof(NetFwRule), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&rule.p))))
        return "Cannot create firewall rule";
    auto image = root / "runtime/x64/cod4x1khz_server.exe";
    auto name = L"cod4x1khz " + khz::desktop::Wide(p.id) + L" UDP " + std::to_wstring(p.port);
    Bstr ruleName(name), application(image.wstring()), ports(std::to_wstring(p.port)),
        group(L"cod4x1khz"), remote(p.network == NetworkMode::Lan ? L"LocalSubnet" : L"*");
    HRESULT hr = rule->put_Name(ruleName);
    if (SUCCEEDED(hr))
        hr = rule->put_ApplicationName(application);
    if (SUCCEEDED(hr))
        hr = rule->put_Protocol(NET_FW_IP_PROTOCOL_UDP);
    if (SUCCEEDED(hr))
        hr = rule->put_LocalPorts(ports);
    if (SUCCEEDED(hr))
        hr = rule->put_RemoteAddresses(remote);
    if (SUCCEEDED(hr))
        hr = rule->put_Grouping(group);
    if (SUCCEEDED(hr))
        hr = rule->put_Direction(NET_FW_RULE_DIR_IN);
    if (SUCCEEDED(hr))
        hr = rule->put_Action(NET_FW_ACTION_ALLOW);
    if (SUCCEEDED(hr))
        hr = rule->put_Profiles(NET_FW_PROFILE2_ALL);
    if (SUCCEEDED(hr))
        hr = rule->put_Enabled(VARIANT_TRUE);
    if (SUCCEEDED(hr))
        hr = rules->Add(rule.p);
    return SUCCEEDED(hr)
               ? "Firewall rule configured for this dedicated EXE and UDP port"
               : "Firewall setup failed or was denied; allow this EXE and UDP port manually";
}
bool owned(IStaticPortMapping *mapping, const std::wstring &description,
           const std::string *bind = nullptr, unsigned port = 0) {
    BSTR value = nullptr;
    if (FAILED(mapping->get_Description(&value)))
        return false;
    bool match = value && description == value;
    SysFreeString(value);
    if (bind) {
        value = nullptr;
        if (FAILED(mapping->get_InternalClient(&value)))
            return false;
        match = match && value && khz::desktop::Wide(*bind) == value;
        SysFreeString(value);
        long actual = 0;
        mapping->get_InternalPort(&actual);
        match = match && actual == long(port);
    }
    return match;
}
std::string mapping(const std::filesystem::path &home, const Profile &p, bool remove, bool &mapped,
                    std::string &ip) {
    Com<IUPnPNAT> nat;
    if (FAILED(
            CoCreateInstance(CLSID_UPnPNAT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&nat.p))))
        return "UPnP unavailable; forward UDP manually";
    Com<IStaticPortMappingCollection> collection;
    if (FAILED(nat->get_StaticPortMappingCollection(&collection.p)) || !collection.p)
        return "Router does not expose UPnP; forward UDP manually (CGNAT may need ISP help)";
    const auto description = L"cod4x1khz-" + khz::desktop::Wide(p.id);
    Bstr protocol(L"UDP");
    auto ledger = home / "router-map.txt";
    unsigned oldPort = 0;
    {
        std::ifstream file(ledger);
        file >> oldPort;
    }
    if (oldPort >= 1024 && oldPort <= 65535) {
        Com<IStaticPortMapping> previous;
        if (SUCCEEDED(collection->get_Item(oldPort, protocol, &previous.p)) && previous.p &&
            owned(previous.p, description)) {
            if (remove || oldPort != p.port) {
                if (FAILED(collection->Remove(oldPort, protocol)))
                    return "Could not remove owned router mapping; retry Stop/network cleanup";
            }
        }
        if (remove || oldPort != p.port)
            DeleteFileW(ledger.c_str());
    }
    if (remove) {
        mapped = false;
        return "Owned router mapping removed; existing unrelated mappings were preserved";
    }
    Com<IStaticPortMapping> existing;
    if (SUCCEEDED(collection->get_Item(p.port, protocol, &existing.p)) && existing.p &&
        !owned(existing.p, description, &p.bind, p.port))
        return "UDP port already has another router mapping; choose another port or forward "
               "manually";
    std::ofstream intent(ledger, std::ios::trunc);
    intent << p.port << '\n';
    intent.close();
    if (!intent)
        return "Cannot record router mapping ownership";
    Com<IStaticPortMapping> created;
    Bstr client(khz::desktop::Wide(p.bind)), label(description);
    if (existing.p) {
        created.p = existing.p;
        created.p->AddRef();
    } else if (FAILED(collection->Add(p.port, protocol, p.port, client, VARIANT_TRUE, label,
                                      &created.p)))
        return "UPnP port mapping failed; forward this UDP port manually";
    BSTR external = nullptr;
    if (SUCCEEDED(created->get_ExternalIPAddress(&external)) && external) {
        auto candidate = khz::desktop::Utf8(external);
        if (PublicIPv4(candidate))
            ip = candidate;
        SysFreeString(external);
    }
    mapped = true;
    return "Owned UDP router mapping created; Internet reachability still requires an external "
           "client test";
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc != 5 || std::wstring_view(argv[1]) != L"--profile" ||
        std::wstring_view(argv[3]) != L"--action")
        return 2;
    auto id = khz::desktop::Utf8(argv[2]);
    if (id.size() != 32 || id.find_first_not_of("0123456789abcdef") != id.npos)
        return 2;
    auto action = std::wstring_view(argv[4]);
    if (action != L"discover" && action != L"map" && action != L"unmap" && action != L"firewall")
        return 2;
    auto root = khz::desktop::ImagePath().parent_path().parent_path().parent_path().parent_path();
    auto homePath = root / "runtime/servers" / id;
    Profile profile;
    std::string error;
    auto options = std::filesystem::exists(homePath / "network-options.ini")
                       ? homePath / "network-options.ini"
                       : homePath / "profile.ini";
    if (!LoadProfile(options, profile, error) || profile.id != id)
        return 3;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WSADATA sockets;
    WSAStartup(MAKEWORD(2, 2), &sockets);
    auto home = ProfileHome(root, profile);
    std::string ip, note;
    bool mapped = false;
    if (action == L"firewall")
        note = profile.network == NetworkMode::Localhost ? "Localhost requires no firewall rule"
                                                         : firewall(root, profile);
    else if (action == L"map" || action == L"unmap")
        note = action == L"map" && profile.network != NetworkMode::Internet
                   ? "Router mapping requires Internet hosting"
                   : mapping(home, profile, action == L"unmap", mapped, ip);
    if (action == L"discover" || action == L"map") {
        if (ip.empty())
            ip = publicIp();
        if (note.empty())
            note = ip.empty() ? "Public IPv4 unavailable; set an override or retry"
                              : "Public IPv4 discovered; external connectivity remains unverified";
    }
    std::ofstream output(home / "network-result.tmp", std::ios::binary);
    output << "public=" << ip << "\nmapped=" << (mapped ? 1 : 0) << "\nnote=" << note << '\n';
    output.close();
    MoveFileExW((home / "network-result.tmp").c_str(), (home / "network-result.txt").c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    WSACleanup();
    CoUninitialize();
    return output ? 0 : 4;
}
