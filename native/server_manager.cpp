#include "desktop_ui.hpp"
#include "host_controller.hpp"
#include "server_profile.hpp"
#include <shellapi.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cwctype>

using namespace khz::hosting;
using namespace khz::desktop;
namespace {
enum Id {
    Profiles = 100,
    NewProfile,
    Save,
    Start,
    Stop,
    Join,
    CopyAddress,
    ProfileName,
    Network,
    Adapter,
    Port,
    Hostname,
    MaxClients,
    Password,
    Advertised,
    AutoNetwork,
    Discover,
    Firewall,
    Maps = 200,
    Mode,
    AddMap,
    RemoveMap,
    UpMap,
    DownMap,
    Rotation,
    Shuffle,
    Mixed,
    ModeCheck = 230,
    Bots = 300,
    Difficulty,
    ApplyBots,
    RuleList,
    RuleValue,
    ApplyRule,
    Players = 400,
    Reason,
    Minutes,
    Power,
    Kick,
    TempBan,
    PermBan,
    AddAdmin,
    Identity,
    Unban,
    RemoveAdmin,
    ListAdmins,
    ListBans,
    Chat,
    SendChat,
    Extra = 500,
    Generated,
    RefreshConfig,
    ImportConfig,
    ExportConfig,
    PluginName,
    LoadPlugin,
    UnloadPlugin,
    ListPlugins,
    Command,
    RunCommand,
    Output,
    NextMap,
    RestartMap,
    TrayOpen = 600,
    TrayStop,
    TrayExit
};
constexpr UINT TrayMessage = WM_APP + 1;
struct App {
    HWND window{}, tab{}, address{}, status{}, pages[5]{}, controls[650]{};
    HFONT font{}, titleFont{};
    unsigned dpi = 96;
    HBRUSH background = CreateSolidBrush(RGB(247, 249, 252));
    std::filesystem::path root;
    Profile profile, runningProfile, networkProfile;
    std::vector<Profile> profiles, cleanupProfiles;
    std::vector<std::string> maps;
    std::vector<std::pair<std::string, std::string>> adapters;
    std::vector<Setting> settings = Settings();
    std::vector<Player> displayedPlayers;
    std::uint64_t displayedSession = 0;
    Controller controller;
    NOTIFYICONDATAW tray{};
    bool restartPending = false, exitPending = false, networkMapped = false,
         automaticPending = false, firewallPending = false;
    ULONGLONG lastStatus = 0, lastTickTime = 0;
    std::uint64_t lastTick = 0, lastSession = 0;
    double hz = 0;
    HANDLE networkProcess = nullptr;
    ULONGLONG networkDeadline = 0;
    std::filesystem::path networkResult;
    std::wstring networkAction;
    bool cleanupPending = false;
    std::string publicIp, networkNote = "Internet connectivity has not been verified";
    void Error(const std::string &text) {
        MessageBoxW(window, Wide(text).c_str(), L"cod4x1khz Server Manager",
                    MB_OK | MB_ICONWARNING);
    }
    HWND C(HWND parent, const wchar_t *type, const wchar_t *text, int id, int x, int y, int w,
           int h, DWORD style = 0) {
        auto c = Control(parent, type, text, id, x, y, w, h, style, font, dpi);
        if (id > 0 && id < 650)
            controls[id] = c;
        return c;
    }
    void Label(HWND parent, const wchar_t *text, int x, int y, int w = 180) {
        C(parent, L"STATIC", text, 0, x, y, w, 23);
    }
    void Combo(HWND page, int id, int x, int y, int w, const std::vector<std::wstring> &values) {
        auto c =
            C(page, L"COMBOBOX", L"", id, x, y, w, 220, WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL);
        for (auto &v : values)
            SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(v.c_str()));
    }
    void Edit(HWND page, int id, int x, int y, int w, const wchar_t *text = L"", DWORD style = 0) {
        C(page, L"EDIT", text, id, x, y, w, 27, WS_TABSTOP | ES_AUTOHSCROLL | style);
    }
    void Button(HWND page, int id, const wchar_t *text, int x, int y, int w = 110) {
        C(page, L"BUTTON", text, id, x, y, w, 29, WS_TABSTOP);
    }
    void Check(HWND page, int id, const wchar_t *text, int x, int y, int w = 220) {
        C(page, L"BUTTON", text, id, x, y, w, 24, WS_TABSTOP | BS_AUTOCHECKBOX);
    }
    bool Checked(int id) { return SendMessageW(controls[id], BM_GETCHECK, 0, 0) == BST_CHECKED; }
    void CheckValue(int id, bool value) {
        SendMessageW(controls[id], BM_SETCHECK, value ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    unsigned Selection(int id) {
        auto n = SendMessageW(controls[id], CB_GETCURSEL, 0, 0);
        return n == CB_ERR ? 0 : unsigned(n);
    }
    void Select(int id, unsigned n) { SendMessageW(controls[id], CB_SETCURSEL, n, 0); }
    std::string Value(int id) { return Utf8(Text(controls[id])); }
    unsigned Number(int id, unsigned fallback = 0) {
        try {
            auto text = Value(id);
            size_t end = 0;
            auto n = std::stoul(text, &end);
            return end == text.size() ? unsigned(n) : fallback;
        } catch (...) {
            return fallback;
        }
    }
    void Table(HWND page, int id, int x, int y, int w, int h,
               const std::vector<std::pair<std::wstring, int>> &columns) {
        auto c = C(page, WC_LISTVIEWW, L"", id, x, y, w, h,
                   WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS);
        ListView_SetExtendedListViewStyle(c, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        int i = 0;
        for (auto &col : columns) {
            LVCOLUMNW item{};
            item.mask = LVCF_TEXT | LVCF_WIDTH;
            item.pszText = const_cast<wchar_t *>(col.first.c_str());
            item.cx = MulDiv(col.second, dpi, 96);
            ListView_InsertColumn(c, i++, &item);
        }
    }
    void Row(int id, int n, const std::vector<std::wstring> &columns) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = n;
        item.pszText = const_cast<wchar_t *>(columns[0].c_str());
        ListView_InsertItem(controls[id], &item);
        for (size_t i = 1; i < columns.size(); ++i)
            ListView_SetItemText(controls[id], n, int(i),
                                 const_cast<wchar_t *>(columns[i].c_str()));
    }
    int SelectedRow(int id) { return ListView_GetNextItem(controls[id], -1, LVNI_SELECTED); }
    void Log(const std::string &text) {
        auto current = Text(controls[Output]);
        if (current.size() > 60000)
            current.erase(0, 20000);
        current += Wide(text);
        current += L"\r\n";
        SetWindowTextW(controls[Output], current.c_str());
        SendMessageW(controls[Output], EM_SETSEL, current.size(), current.size());
        SendMessageW(controls[Output], EM_SCROLLCARET, 0, 0);
    }
    void LoadProfiles() {
        auto directory = root / "runtime/servers";
        if (std::filesystem::is_directory(directory))
            for (auto &entry : std::filesystem::directory_iterator(directory)) {
                Profile candidate;
                std::string error;
                if (entry.is_directory() &&
                    LoadProfile(entry.path() / "profile.ini", candidate, error)) {
                    profiles.push_back(candidate);
                    if (std::filesystem::exists(entry.path() / "router-map.txt"))
                        cleanupProfiles.push_back(candidate);
                }
            }
        if (profiles.empty()) {
            char id[33];
            NewToken(id);
            profile.id = id;
            profiles.push_back(profile);
        } else
            profile = profiles.front();
    }
    void RefreshProfiles() {
        SendMessageW(controls[Profiles], CB_RESETCONTENT, 0, 0);
        for (auto &p : profiles) {
            auto text = Wide(p.name);
            SendMessageW(controls[Profiles], CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(text.c_str()));
        }
        for (unsigned i = 0; i < profiles.size(); ++i)
            if (profiles[i].id == profile.id)
                Select(Profiles, i);
    }
    void FindAdapters() {
        ULONG size = 16384;
        std::vector<unsigned char> data(size);
        auto *list = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(data.data());
        auto result = GetAdaptersAddresses(
            AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, list, &size);
        if (result == ERROR_BUFFER_OVERFLOW) {
            data.resize(size);
            list = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(data.data());
            result = GetAdaptersAddresses(AF_INET, 0, nullptr, list, &size);
        }
        if (result != NO_ERROR)
            return;
        for (auto *a = list; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
                continue;
            for (auto *u = a->FirstUnicastAddress; u; u = u->Next) {
                if (u->Address.lpSockaddr->sa_family != AF_INET)
                    continue;
                char ip[64];
                auto *addr = reinterpret_cast<sockaddr_in *>(u->Address.lpSockaddr);
                InetNtopA(AF_INET, &addr->sin_addr, ip, sizeof(ip));
                if (std::string(ip).starts_with("169.254."))
                    continue;
                adapters.emplace_back(Utf8(a->FriendlyName), ip);
            }
        }
    }
    void Create() {
        root = ImagePath().parent_path();
        while (!std::filesystem::is_directory(root / "runtime") && root != root.parent_path())
            root = root.parent_path();
        if (!std::filesystem::is_directory(root / "runtime"))
            throw std::runtime_error("Cannot locate the installed runtime folder");
        maps = InstalledMaps(root);
        FindAdapters();
        LoadProfiles();
        dpi = GetDpiForWindow(window);
        font = CreateFontW(-MulDiv(15, dpi, 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                           CLEARTYPE_QUALITY, 0, L"Segoe UI");
        titleFont = CreateFontW(-MulDiv(24, dpi, 96), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                                DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        auto title = C(window, L"STATIC", L"cod4x1khz  /  Server Manager", 0, 22, 16, 590, 36);
        SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(titleFont), TRUE);
        Button(window, TrayExit, L"Exit", 774, 20, 120);
        Label(window, L"Profile", 24, 65, 65);
        Combo(window, Profiles, 90, 61, 280, {});
        Button(window, NewProfile, L"New", 382, 61, 65);
        Button(window, Save, L"Save", 458, 61, 155);
        Button(window, Start, L"Start Server", 642, 61, 120);
        Button(window, Stop, L"Stop Server", 774, 61, 120);
        tab = C(window, WC_TABCONTROLW, L"", 0, 22, 105, 872, 492, WS_TABSTOP);
        const wchar_t *names[]{L"Hosting", L"Maps & Modes", L"Rules & Bots", L"Players & Admin",
                               L"Advanced & Logs"};
        for (int i = 0; i < 5; ++i) {
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = const_cast<wchar_t *>(names[i]);
            TabCtrl_InsertItem(tab, i, &item);
            pages[i] = CreateWindowExW(
                WS_EX_CONTROLPARENT, L"KHZManagerPage", L"", WS_CHILD | WS_VISIBLE,
                MulDiv(35, dpi, 96), MulDiv(141, dpi, 96), MulDiv(840, dpi, 96),
                MulDiv(444, dpi, 96), window, nullptr, GetModuleHandleW(nullptr), nullptr);
        }
        auto page = pages[0];
        Label(page, L"Profile name", 0, 4);
        Edit(page, ProfileName, 190, 0, 310);
        Label(page, L"Server name", 0, 46);
        Edit(page, Hostname, 190, 42, 460);
        Label(page, L"Hosting mode", 0, 88);
        Combo(page, Network, 190, 84, 210, {L"Localhost", L"Local network", L"Internet"});
        Label(page, L"Network adapter", 0, 130);
        std::vector<std::wstring> adapterNames;
        for (auto &[name, ip] : adapters)
            adapterNames.push_back(Wide(name + " — " + ip));
        Combo(page, Adapter, 190, 126, 460, adapterNames);
        Label(page, L"UDP port", 0, 172);
        Edit(page, Port, 190, 168, 95, L"28960", ES_NUMBER);
        Label(page, L"Players (1–16)", 322, 172, 125);
        Edit(page, MaxClients, 456, 168, 80, L"16", ES_NUMBER);
        Label(page, L"Join password", 0, 214);
        Edit(page, Password, 190, 210, 310, L"", ES_PASSWORD);
        Label(page, L"Public IPv4 override", 0, 256);
        Edit(page, Advertised, 190, 252, 210);
        Check(page, AutoNetwork, L"Offer automatic router / firewall setup", 190, 296, 425);
        Button(page, Discover, L"Refresh public IP", 190, 335, 175);
        Button(page, Firewall, L"Configure firewall", 380, 335, 175);
        C(page, L"STATIC",
          L"Internet: forward this UDP port to the selected adapter.\r\nPublic IP discovery and "
          L"router mapping do not prove outside connectivity.\r\nCompatible players connect "
          L"directly using cod4x1khz.",
          0, 0, 382, 800, 62);
        page = pages[1];
        std::vector<std::wstring> mapNames;
        for (auto &m : maps)
            mapNames.push_back(Wide(m));
        Combo(page, Maps, 0, 0, 290, mapNames);
        std::vector<std::wstring> modeNames;
        for (auto &m : Modes())
            modeNames.push_back(Wide(m.second));
        Combo(page, Mode, 305, 0, 235, modeNames);
        Button(page, AddMap, L"Add", 558, 0, 85);
        Table(page, Rotation, 0, 44, 820, 245, {{L"Map", 345}, {L"Game mode", 410}});
        Button(page, RemoveMap, L"Remove", 0, 305);
        Button(page, UpMap, L"Move up", 125, 305);
        Button(page, DownMap, L"Move down", 250, 305);
        Button(page, NextMap, L"Next map", 535, 305);
        Button(page, RestartMap, L"Restart map", 660, 305);
        Check(page, Shuffle, L"Shuffle maps each cycle", 0, 350);
        Check(page, Mixed, L"Random modes from selected pool", 280, 350, 380);
        for (unsigned i = 0; i < Modes().size(); ++i)
            Check(page, ModeCheck + i, Wide(Modes()[i].second).c_str(), int(i % 3) * 270,
                  390 + int(i / 3) * 26, 265);
        page = pages[2];
        Label(page, L"Bot count", 0, 4, 80);
        std::vector<std::wstring> counts;
        for (int i = 0; i < 16; ++i)
            counts.push_back(std::to_wstring(i));
        Combo(page, Bots, 85, 0, 70, counts);
        Label(page, L"Difficulty", 183, 4, 80);
        Combo(page, Difficulty, 265, 0, 160, {L"Recruit", L"Regular", L"Hardened", L"Veteran"});
        Button(page, ApplyBots, L"Apply bots now", 450, 0, 155);
        Table(page, RuleList, 0, 44, 820, 325, {{L"Setting", 495}, {L"Value", 285}});
        Label(page, L"Selected value", 0, 393, 130);
        Edit(page, RuleValue, 145, 387, 435);
        Button(page, ApplyRule, L"Update rule", 605, 387, 140);
        page = pages[3];
        Table(page, Players, 0, 0, 820, 200,
              {{L"Slot", 45}, {L"Player", 300}, {L"Type", 100}, {L"Ping", 75}, {L"Power", 70}});
        Label(page, L"Reason", 0, 215, 60);
        Edit(page, Reason, 65, 210, 330, L"Server administration");
        Label(page, L"Minutes", 412, 215, 70);
        Edit(page, Minutes, 482, 210, 60, L"30", ES_NUMBER);
        Label(page, L"Power", 560, 215, 65);
        Edit(page, Power, 625, 210, 65, L"10", ES_NUMBER);
        Button(page, Kick, L"Kick", 0, 250, 90);
        Button(page, TempBan, L"Temporary ban", 105, 250, 155);
        Button(page, PermBan, L"Permanent ban", 275, 250, 155);
        Button(page, AddAdmin, L"Set admin power", 445, 250, 165);
        Label(page, L"Local-key identity", 0, 295, 130);
        Edit(page, Identity, 145, 290, 555);
        Button(page, Unban, L"Unban", 0, 326, 90);
        Button(page, RemoveAdmin, L"Remove admin", 105, 326, 155);
        Button(page, ListAdmins, L"List admins", 275, 326, 135);
        Button(page, ListBans, L"List bans", 425, 326, 125);
        Edit(page, Chat, 0, 375, 620);
        Button(page, SendChat, L"Send server chat", 640, 375, 170);
        page = pages[4];
        Label(page, L"Extra configuration (preserved separately)", 0, 0, 450);
        C(page, L"EDIT", L"", Extra, 0, 25, 398, 125,
          WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_WANTRETURN);
        Label(page, L"Generated configuration", 420, 0, 350);
        C(page, L"EDIT", L"", Generated, 420, 25, 398, 125,
          ES_MULTILINE | ES_READONLY | WS_VSCROLL);
        Button(page, RefreshConfig, L"Preview config", 0, 160, 140);
        Button(page, ImportConfig, L"Import .cfg", 156, 160, 125);
        Button(page, ExportConfig, L"Export .cfg", 297, 160, 125);
        Edit(page, PluginName, 0, 201, 225, L"plugin_name");
        Button(page, LoadPlugin, L"Load plugin", 240, 201, 130);
        Button(page, UnloadPlugin, L"Unload plugin", 385, 201, 145);
        Button(page, ListPlugins, L"List plugins", 545, 201, 130);
        Edit(page, Command, 0, 242, 625);
        Button(page, RunCommand, L"Run command", 645, 242, 165);
        C(page, L"EDIT", L"", Output, 0, 282, 820, 155, ES_MULTILINE | ES_READONLY | WS_VSCROLL);
        status = C(window, L"STATIC", L"Stopped", 0, 25, 612, 600, 24);
        address = C(window, L"STATIC", L"127.0.0.1:28960", 0, 25, 642, 470, 25);
        Button(window, CopyAddress, L"Copy address", 585, 638, 145);
        Button(window, Join, L"Join server", 745, 638, 145);
        RefreshProfiles();
        Fill();
        ShowPage(0);
        controller.onReply = [this](const Packet &reply) {
            if (reply.message == HostMessage::CommandResult)
                Log(reply.output);
            if (reply.message == HostMessage::StatusResult) {
                RefreshPlayers(reply);
                if (reply.session != lastSession || reply.tick < lastTick) {
                    lastTickTime = 0;
                    hz = 0;
                }
                if (lastTickTime && reply.fixture > lastTickTime)
                    hz = double(reply.tick - lastTick) * 1000.0 / (reply.fixture - lastTickTime);
                lastTick = reply.tick;
                lastTickTime = reply.fixture;
                lastSession = reply.session;
            }
        };
        controller.initialReadyCheck = [this](const Packet &reply) {
            for (const auto &row : runningProfile.rotation)
                if (row.map == reply.map) {
                    if (!runningProfile.mixed)
                        return row.mode == reply.gametype;
                    std::istringstream pool(runningProfile.modePool);
                    std::string mode;
                    while (pool >> mode)
                        if (mode == reply.gametype)
                            return true;
                }
            return false;
        };
        tray.cbSize = sizeof(tray);
        tray.hWnd = window;
        tray.uID = 1;
        tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        tray.uCallbackMessage = TrayMessage;
        tray.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
        wcscpy_s(tray.szTip, L"cod4x1khz Server Manager");
        Shell_NotifyIconW(NIM_ADD, &tray);
        SetTimer(window, 1, 25, nullptr);
    }
    void ShowPage(int page) {
        for (int i = 0; i < 5; ++i)
            ShowWindow(pages[i], i == page ? SW_SHOW : SW_HIDE);
    }
    void RefreshRotation() {
        ListView_DeleteAllItems(controls[Rotation]);
        for (unsigned i = 0; i < profile.rotation.size(); ++i) {
            auto &row = profile.rotation[i];
            std::string mode = row.mode;
            for (auto &m : Modes())
                if (m.first == mode)
                    mode = m.second;
            Row(Rotation, i, {Wide(row.map), Wide(mode)});
        }
    }
    void RefreshRules() {
        int selected = SelectedRow(RuleList);
        ListView_DeleteAllItems(controls[RuleList]);
        for (unsigned i = 0; i < settings.size(); ++i) {
            auto &rule = settings[i];
            auto v = profile.rules[rule.key];
            if ((rule.key.find("password") != v.npos || rule.key.find("Password") != v.npos) &&
                !v.empty())
                v = "••••••••";
            Row(RuleList, i, {Wide(rule.label), Wide(v)});
        }
        if (selected >= 0)
            ListView_SetItemState(controls[RuleList], selected, LVIS_SELECTED, LVIS_SELECTED);
    }
    void Fill() {
        SetText(controls[ProfileName], profile.name);
        SetText(controls[Hostname], profile.hostname);
        SetText(controls[Port], std::to_string(profile.port));
        SetText(controls[MaxClients], std::to_string(profile.maxClients));
        SetText(controls[Password], profile.password);
        SetText(controls[Advertised], profile.advertised);
        SetText(controls[Extra], profile.extra);
        Select(Network, unsigned(profile.network));
        Select(Bots, profile.bots);
        Select(Difficulty, profile.difficulty);
        Select(Maps, 0);
        Select(Mode, 0);
        Select(Adapter, 0);
        for (unsigned i = 0; i < adapters.size(); ++i)
            if (adapters[i].second == profile.bind)
                Select(Adapter, i);
        CheckValue(Shuffle, profile.shuffle);
        CheckValue(Mixed, profile.mixed);
        CheckValue(AutoNetwork, profile.automaticNetwork);
        for (unsigned i = 0; i < Modes().size(); ++i)
            CheckValue(ModeCheck + i,
                       profile.modePool.find(Modes()[i].first) != profile.modePool.npos);
        RefreshRotation();
        RefreshRules();
        SetText(controls[Generated], Configuration(profile));
        NetworkControls();
    }
    void NetworkControls() {
        auto mode = Selection(Network);
        EnableWindow(controls[Adapter], mode != 0);
        EnableWindow(controls[Advertised], mode == 2);
        EnableWindow(controls[AutoNetwork], mode != 0);
        EnableWindow(controls[Discover], mode == 2);
        EnableWindow(controls[Firewall], mode != 0);
    }
    bool Read() {
        profile.name = Value(ProfileName);
        profile.hostname = Value(Hostname);
        profile.port = Number(Port);
        profile.maxClients = Number(MaxClients);
        profile.password = Value(Password);
        profile.network = NetworkMode(Selection(Network));
        profile.bind =
            profile.network == NetworkMode::Localhost
                ? "127.0.0.1"
                : (adapters.empty()
                       ? ""
                       : adapters[std::min(size_t(Selection(Adapter)), adapters.size() - 1)]
                             .second);
        profile.advertised = Value(Advertised);
        profile.bots = Selection(Bots);
        profile.difficulty = Selection(Difficulty);
        profile.shuffle = Checked(Shuffle);
        profile.mixed = Checked(Mixed);
        profile.automaticNetwork = Checked(AutoNetwork);
        profile.extra = Value(Extra);
        profile.modePool.clear();
        for (unsigned i = 0; i < Modes().size(); ++i)
            if (Checked(ModeCheck + i))
                profile.modePool += Modes()[i].first + " ";
        std::string error;
        if (!Validate(profile, error)) {
            Error(error);
            return false;
        }
        return true;
    }
    bool SaveCurrent() {
        if (!Read())
            return false;
        std::string error;
        if (!SaveProfile(root, profile, error)) {
            Error(error);
            return false;
        }
        for (auto &p : profiles)
            if (p.id == profile.id)
                p = profile;
        RefreshProfiles();
        SetText(controls[Generated], Configuration(profile));
        return true;
    }
    void StartCurrent() {
        if (!SaveCurrent())
            return;
        for (auto &row : profile.rotation)
            if (std::find(maps.begin(), maps.end(), row.map) == maps.end()) {
                Error("Map is not installed: " + row.map);
                return;
            }
        runningProfile = profile;
        if (!controller.Start(root, StartArguments(profile), ConfigurationPacket(profile)))
            Error(controller.error);
        else {
            automaticPending =
                profile.automaticNetwork && profile.network != NetworkMode::Localhost;
            Log("Starting " + profile.name + " on " + profile.bind + ":" +
                std::to_string(profile.port));
        }
    }
    void StopCurrent() {
        automaticPending = firewallPending = false;
        controller.Stop();
        cleanupPending =
            networkMapped || (networkProcess && networkAction == L"map") ||
            std::filesystem::exists(ProfileHome(root, runningProfile) / "router-map.txt");
    }
    void SendCommand(const std::string &command) {
        if (command.size() >= sizeof(Packet::command)) {
            Error("Command is too long");
            return;
        }
        Packet request;
        request.message = HostMessage::Command;
        strcpy_s(request.command, command.c_str());
        if (!controller.Request(request))
            Log("Server is not ready or its command queue is full");
    }
    void RefreshPlayers(const Packet &reply) {
        int old = SelectedRow(Players);
        std::uint64_t generation =
            reply.session == displayedSession && old >= 0 && size_t(old) < displayedPlayers.size()
                ? displayedPlayers[old].generation
                : 0;
        int oldSlot =
            old >= 0 && size_t(old) < displayedPlayers.size() ? displayedPlayers[old].slot : -1;
        displayedSession = reply.session;
        displayedPlayers.assign(reply.players, reply.players + reply.playerCount);
        ListView_DeleteAllItems(controls[Players]);
        for (unsigned i = 0; i < displayedPlayers.size(); ++i) {
            auto &p = displayedPlayers[i];
            Row(Players, i,
                {std::to_wstring(p.slot), Wide(p.name), p.bot ? L"Bot" : L"Player",
                 std::to_wstring(p.ping), std::to_wstring(p.power)});
            if (p.generation == generation && p.slot == oldSlot)
                ListView_SetItemState(controls[Players], i, LVIS_SELECTED, LVIS_SELECTED);
        }
    }
    void Act(PlayerAction action) {
        int selected = SelectedRow(Players);
        if (selected < 0 || size_t(selected) >= displayedPlayers.size()) {
            Error("Select a connected player first");
            return;
        }
        auto &p = displayedPlayers[selected];
        if (p.bot && action != PlayerAction::Kick) {
            Error(
                "Bots can be kicked but cannot receive identity bans or administrator privileges");
            return;
        }
        Packet request;
        request.message = HostMessage::PlayerAction;
        request.targetSlot = p.slot;
        request.admission = p.generation;
        request.session = displayedSession;
        request.action = action;
        request.minutes = Number(Minutes);
        request.power = Number(Power);
        auto text = Value(Reason);
        strncpy_s(request.command, text.c_str(), _TRUNCATE);
        if (!controller.Request(request))
            Log("Server is not ready for administration");
    }
    void BeginNetwork(const wchar_t *action, bool elevated = false,
                      const Profile *recovery = nullptr) {
        if (networkProcess) {
            Log("Network setup is already running");
            return;
        }
        if (recovery)
            networkProfile = *recovery;
        else if (controller.Pid() || std::wstring_view(action) == L"unmap")
            networkProfile = runningProfile;
        else {
            if (!SaveCurrent())
                return;
            networkProfile = profile;
        }
        const auto image = root / "runtime/tools/x64/cod4x1khz_network.exe";
        auto home = ProfileHome(root, networkProfile);
        std::filesystem::create_directories(home);
        std::ofstream snapshot(home / "network-options.ini", std::ios::binary);
        snapshot << "version=1\nid=" << networkProfile.id
                 << "\nname=Network options\nnetwork=" << unsigned(networkProfile.network)
                 << "\nbind=" << networkProfile.bind << "\nport=" << networkProfile.port
                 << "\nbots=0\nmap=mp_killhouse dm\n";
        snapshot.close();
        if (!snapshot) {
            Log("Cannot save network setup options");
            return;
        }
        networkAction = action;
        // Attempt cleanup once per stop/recovery. A failed attempt retains its journal for
        // the next launch, rather than trapping Exit in an endless retry loop.
        if (networkAction == L"unmap")
            networkMapped = false;
        networkResult = home / "network-result.txt";
        DeleteFileW(networkResult.c_str());
        std::wstring arguments = L"--profile " + Wide(networkProfile.id) + L" --action " + action;
        if (elevated) {
            SHELLEXECUTEINFOW start{};
            start.cbSize = sizeof(start);
            start.fMask = SEE_MASK_NOCLOSEPROCESS;
            start.hwnd = window;
            start.lpVerb = L"runas";
            start.lpFile = image.c_str();
            start.lpParameters = arguments.c_str();
            start.nShow = SW_HIDE;
            if (ShellExecuteExW(&start))
                networkProcess = start.hProcess;
            else
                Log("Firewall setup declined or failed; configure the selected UDP port manually");
        } else {
            auto command = Quote(image.wstring()) + L" " + arguments;
            STARTUPINFOW start{};
            start.cb = sizeof(start);
            PROCESS_INFORMATION process{};
            if (CreateProcessW(image.c_str(), command.data(), nullptr, nullptr, FALSE,
                               CREATE_NO_WINDOW, nullptr, root.c_str(), &start, &process)) {
                networkProcess = process.hProcess;
                CloseHandle(process.hThread);
            } else
                Log("Network helper is missing or could not start");
        }
        networkDeadline = GetTickCount64() + 20000;
    }
    void Poll() {
        controller.Poll();
        auto now = GetTickCount64();
        if (controller.phase == Controller::Phase::Running && now - lastStatus >= 500) {
            Packet query;
            query.message = HostMessage::Status;
            controller.Request(query);
            lastStatus = now;
        }
        if (controller.phase == Controller::Phase::Failed) {
            automaticPending = firewallPending = false;
            if (networkMapped)
                cleanupPending = true;
        }
        if (networkProcess &&
            (WaitForSingleObject(networkProcess, 0) == WAIT_OBJECT_0 || now >= networkDeadline)) {
            if (now >= networkDeadline) {
                TerminateProcess(networkProcess, 1);
                networkNote = "Network setup timed out; manual forwarding may be needed";
                Log(networkNote);
            }
            CloseHandle(networkProcess);
            networkProcess = nullptr;
            std::ifstream result(networkResult);
            std::string line;
            while (std::getline(result, line)) {
                if (line.starts_with("public=") &&
                    (networkAction == L"map" || networkAction == L"discover"))
                    publicIp = line.substr(7);
                else if (line.starts_with("mapped=") &&
                         (networkAction == L"map" || networkAction == L"unmap"))
                    networkMapped = line.substr(7) == "1";
                else if (line.starts_with("note=")) {
                    networkNote = line.substr(5);
                    Log(networkNote);
                }
            }
        }
        if (cleanupPending && !networkProcess) {
            cleanupPending = false;
            BeginNetwork(L"unmap");
        }
        if (!networkProcess && !cleanupPending && !cleanupProfiles.empty() &&
            (controller.phase == Controller::Phase::Idle ||
             controller.phase == Controller::Phase::Failed)) {
            auto recovery = cleanupProfiles.back();
            cleanupProfiles.pop_back();
            Log("Cleaning up a recorded router mapping for " + recovery.name);
            BeginNetwork(L"unmap", false, &recovery);
        }
        if (automaticPending && controller.phase == Controller::Phase::Running && !networkProcess) {
            automaticPending = false;
            firewallPending = true;
            BeginNetwork(runningProfile.network == NetworkMode::Internet ? L"map" : L"discover");
        } else if (firewallPending && !networkProcess) {
            firewallPending = false;
            BeginNetwork(L"firewall", true);
        }
        if (restartPending && !networkProcess && !cleanupPending && cleanupProfiles.empty() &&
            (controller.phase == Controller::Phase::Idle ||
             controller.phase == Controller::Phase::Failed)) {
            restartPending = false;
            StartCurrent();
        }
        if (exitPending && !networkProcess && !cleanupPending && cleanupProfiles.empty() &&
            (controller.phase == Controller::Phase::Idle ||
             controller.phase == Controller::Phase::Failed)) {
            DestroyWindow(window);
            return;
        }
        const wchar_t *phaseNames[]{L"Stopped", L"Starting…", L"Loading map…",
                                    L"Running", L"Stopping…", L"Failed"};
        std::wstring text = phaseNames[unsigned(controller.phase)];
        if (controller.phase == Controller::Phase::Running) {
            text += L"  |  " + Wide(controller.status.map) + L"  |  " +
                    std::to_wstring(controller.status.playerCount) + L" players";
            if (hz > 0) {
                wchar_t rate[64];
                swprintf_s(rate, L"  |  %.1f Hz", hz);
                text += rate;
            }
        }
        if (controller.phase == Controller::Phase::Failed)
            text += L"  |  " + Wide(controller.error);
        SetWindowTextW(status, text.c_str());
        bool active = controller.phase != Controller::Phase::Idle &&
                      controller.phase != Controller::Phase::Failed;
        const auto mode = active ? runningProfile.network : NetworkMode(Selection(Network));
        const auto port = active ? runningProfile.port : Number(Port);
        std::string ip;
        if (mode == NetworkMode::Internet) {
            const auto advertised = active ? runningProfile.advertised : Value(Advertised);
            ip = advertised.empty() ? publicIp : (PublicIPv4(advertised) ? advertised : "");
        } else if (active) {
            ip = runningProfile.bind;
        } else if (mode == NetworkMode::Localhost) {
            ip = "127.0.0.1";
        } else if (Selection(Adapter) < adapters.size()) {
            ip = adapters[Selection(Adapter)].second;
        }
        const bool validAddress = !ip.empty() && port >= 1024 && port <= 65535;
        SetText(address, validAddress ? ip + ":" + std::to_string(port)
                                      : (mode == NetworkMode::Internet
                                             ? "Public IPv4 unavailable — use Refresh public IP"
                                             : "Address unavailable — check adapter and port"));
        EnableWindow(controls[CopyAddress], validAddress);
        EnableWindow(controls[Join], controller.phase == Controller::Phase::Running);
        EnableWindow(controls[Start],
                     !active && !networkProcess && !cleanupPending && cleanupProfiles.empty());
        EnableWindow(controls[Stop], active);
        EnableWindow(controls[Profiles], !active && !networkProcess);
        EnableWindow(controls[NewProfile], !active && !networkProcess);
        SetWindowTextW(controls[Save], active ? L"Save & Restart" : L"Save");
    }
    void CommandEvent(int id) {
        if (id == Network) {
            NetworkControls();
            return;
        }
        if (id == Profiles) {
            if (Selection(Profiles) < profiles.size()) {
                profile = profiles[Selection(Profiles)];
                publicIp.clear();
                Fill();
            }
            return;
        }
        if (id == NewProfile) {
            profile = Profile{};
            char token[33];
            NewToken(token);
            profile.id = token;
            profile.name = "New server";
            profiles.push_back(profile);
            RefreshProfiles();
            Fill();
            return;
        }
        if (id == Save) {
            if (SaveCurrent() && controller.Pid()) {
                restartPending = true;
                StopCurrent();
            }
            return;
        }
        if (id == Start) {
            StartCurrent();
            return;
        }
        if (id == Stop || id == TrayStop) {
            restartPending = false;
            StopCurrent();
            return;
        }
        if (id == TrayOpen) {
            ShowWindow(window, SW_RESTORE);
            SetForegroundWindow(window);
            return;
        }
        if (id == TrayExit) {
            exitPending = true;
            restartPending = false;
            StopCurrent();
            return;
        }
        if (id == CopyAddress) {
            Copy(window, Text(address));
            return;
        }
        if (id == Join) {
            if (controller.phase != Controller::Phase::Running) {
                Error("Start the server first");
                return;
            }
            auto target = runningProfile.bind + ":" + std::to_string(runningProfile.port);
            if (!Launch(root / "cod4x1khz.exe",
                        L"+connect " + Wide(target) + L" +set password " +
                            Wide(QuotedText(runningProfile.password)),
                        root))
                Error("Installed game executable is missing");
            return;
        }
        if (id == Discover) {
            BeginNetwork(L"discover");
            return;
        }
        if (id == Firewall) {
            BeginNetwork(L"firewall", true);
            return;
        }
        if (id == AddMap) {
            if (!maps.empty() && profile.rotation.size() < 64) {
                profile.rotation.push_back(
                    {maps[std::min(size_t(Selection(Maps)), maps.size() - 1)],
                     Modes()[Selection(Mode)].first});
                RefreshRotation();
            }
            return;
        }
        if (id == RemoveMap || id == UpMap || id == DownMap) {
            int row = SelectedRow(Rotation);
            if (row < 0)
                return;
            if (id == RemoveMap)
                profile.rotation.erase(profile.rotation.begin() + row);
            else if (id == UpMap && row > 0)
                std::swap(profile.rotation[row], profile.rotation[row - 1]);
            else if (id == DownMap && size_t(row + 1) < profile.rotation.size())
                std::swap(profile.rotation[row], profile.rotation[row + 1]);
            RefreshRotation();
            return;
        }
        if (id == ApplyRule) {
            int row = SelectedRow(RuleList);
            if (row >= 0 && size_t(row) < settings.size()) {
                profile.rules[settings[row].key] = Value(RuleValue);
                RefreshRules();
            }
            return;
        }
        if (id == ApplyBots) {
            Packet request;
            request.message = HostMessage::Bots;
            request.bots = Selection(Bots);
            request.difficulty = Selection(Difficulty);
            if (request.bots >= profile.maxClients) {
                Error("Keep at least one slot available for a player");
                return;
            }
            profile.bots = request.bots;
            profile.difficulty = request.difficulty;
            if (!controller.Request(request))
                Log("Bot settings will apply when the server starts");
            return;
        }
        if (id == Kick) {
            Act(PlayerAction::Kick);
            return;
        }
        if (id == TempBan) {
            Act(PlayerAction::TemporaryBan);
            return;
        }
        if (id == PermBan) {
            Act(PlayerAction::PermanentBan);
            return;
        }
        if (id == AddAdmin) {
            Act(PlayerAction::AddAdmin);
            return;
        }
        if (id == Unban || id == RemoveAdmin) {
            auto identity = Value(Identity);
            if (identity.size() != 64 ||
                identity.find_first_not_of("0123456789abcdefABCDEF") != identity.npos) {
                Error("Enter the full 64-character local-key identity");
                return;
            }
            SendCommand((id == Unban ? "unban " : "AdminRemoveAdmin ") + identity);
            return;
        }
        if (id == ListAdmins) {
            SendCommand("AdminListAdmins");
            ShowPage(4);
            TabCtrl_SetCurSel(tab, 4);
            return;
        }
        if (id == ListBans) {
            SendCommand("khz_manager_bans");
            ShowPage(4);
            TabCtrl_SetCurSel(tab, 4);
            return;
        }
        if (id == SendChat) {
            SendCommand("consay " + QuotedText(Value(Chat)));
            return;
        }
        if (id == NextMap) {
            SendCommand("map_rotate");
            return;
        }
        if (id == RestartMap) {
            Packet request;
            request.message = HostMessage::Restart;
            controller.Request(request);
            return;
        }
        if (id == RunCommand) {
            SendCommand(Value(Command));
            return;
        }
        if (id == LoadPlugin || id == UnloadPlugin) {
            auto name = Value(PluginName);
            if (!Identifier(name)) {
                Error("Enter an installed native plugin name without a path or extension");
                return;
            }
            SendCommand((id == LoadPlugin ? "loadPlugin " : "unloadPlugin ") + name);
            return;
        }
        if (id == ListPlugins) {
            SendCommand("plugins");
            return;
        }
        if (id == RefreshConfig) {
            if (Read())
                SetText(controls[Generated], Configuration(profile));
            return;
        }
        if (id == ImportConfig) {
            auto path = Pick(window, false);
            if (!path.empty()) {
                if (std::filesystem::file_size(path) > 65536) {
                    Error("Imported configuration exceeds 64 KiB");
                    return;
                }
                std::ifstream file(path, std::ios::binary);
                std::string text((std::istreambuf_iterator<char>(file)), {}), error;
                if (ImportConfiguration(profile, text, error))
                    Fill();
                else
                    Error(error);
            }
            return;
        }
        if (id == ExportConfig) {
            if (!Read())
                return;
            auto path = Pick(window, false, true);
            if (!path.empty()) {
                std::ofstream file(path, std::ios::binary);
                file << Configuration(profile);
                if (!file)
                    Error("Cannot export config");
            }
            return;
        }
    }
} app;
LRESULT CALLBACK PageProc(HWND window, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_COMMAND || msg == WM_NOTIFY || msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLOREDIT)
        return SendMessageW(GetParent(window), msg, w, l);
    return DefWindowProcW(window, msg, w, l);
}
LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    try {
        switch (message) {
        case WM_CREATE:
            app.window = window;
            app.Create();
            return 0;
        case WM_COMMAND:
            if (HIWORD(w) == BN_CLICKED || HIWORD(w) == CBN_SELCHANGE || HIWORD(w) == 0)
                app.CommandEvent(LOWORD(w));
            return 0;
        case WM_NOTIFY: {
            auto *info = reinterpret_cast<NMHDR *>(l);
            if (info->hwndFrom == app.tab && info->code == TCN_SELCHANGE)
                app.ShowPage(TabCtrl_GetCurSel(app.tab));
            else if (info->idFrom == RuleList && info->code == LVN_ITEMCHANGED) {
                int row = app.SelectedRow(RuleList);
                if (row >= 0 && size_t(row) < app.settings.size())
                    SetText(app.controls[RuleValue], app.profile.rules[app.settings[row].key]);
            } else if (info->idFrom == Players && info->code == LVN_ITEMCHANGED) {
                int row = app.SelectedRow(Players);
                if (row >= 0 && size_t(row) < app.displayedPlayers.size())
                    SetText(app.controls[Identity], app.displayedPlayers[row].identity);
            }
            return 0;
        }
        case WM_TIMER:
            app.Poll();
            return 0;
        case WM_DPICHANGED:
            ScaleDpi(window, app.dpi, HIWORD(w), app.font, app.titleFont,
                     *reinterpret_cast<RECT *>(l));
            app.dpi = HIWORD(w);
            return 0;
        case WM_CLOSE:
            ShowWindow(window, SW_HIDE);
            return 0;
        case TrayMessage:
            if (l == WM_LBUTTONDBLCLK)
                app.CommandEvent(TrayOpen);
            else if (l == WM_RBUTTONUP) {
                POINT point;
                GetCursorPos(&point);
                auto menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, TrayOpen, L"Open Server Manager");
                AppendMenuW(menu, MF_STRING, TrayStop, L"Stop server");
                AppendMenuW(menu, MF_STRING, TrayExit, L"Exit and stop server");
                SetForegroundWindow(window);
                auto selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x,
                                               point.y, 0, window, nullptr);
                DestroyMenu(menu);
                if (selected)
                    app.CommandEvent(selected);
            }
            return 0;
        case WM_CTLCOLORSTATIC:
            SetBkColor(reinterpret_cast<HDC>(w), RGB(247, 249, 252));
            SetTextColor(reinterpret_cast<HDC>(w), RGB(35, 58, 91));
            return reinterpret_cast<LRESULT>(app.background);
        case WM_DESTROY:
            KillTimer(window, 1);
            Shell_NotifyIconW(NIM_DELETE, &app.tray);
            app.controller.Close();
            if (app.networkProcess) {
                TerminateProcess(app.networkProcess, 1);
                CloseHandle(app.networkProcess);
            }
            DeleteObject(app.font);
            DeleteObject(app.titleFont);
            PostQuitMessage(0);
            return 0;
        }
    } catch (const std::exception &error) {
        app.Error(error.what());
    }
    return DefWindowProcW(window, message, w, l);
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WSADATA sockets;
    WSAStartup(MAKEWORD(2, 2), &sockets);
    auto image = ImagePath();
    auto identity = image.wstring();
    std::transform(identity.begin(), identity.end(), identity.begin(), std::towlower);
    auto mutexName =
        L"Local\\cod4x1khz-manager-" + std::to_wstring(std::hash<std::wstring>{}(identity));
    HANDLE mutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
    if (!mutex) {
        MessageBoxW(nullptr, L"Cannot acquire Server Manager ownership.", L"cod4x1khz", MB_OK);
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        struct Search {
            const std::filesystem::path &image;
            HWND found = nullptr;
        } search{image};
        EnumWindows(
            [](HWND candidate, LPARAM context) -> BOOL {
                auto &search = *reinterpret_cast<Search *>(context);
                wchar_t name[64];
                if (!GetClassNameW(candidate, name, 64) ||
                    std::wstring_view(name) != L"KHZServerManager")
                    return TRUE;
                DWORD pid = 0;
                GetWindowThreadProcessId(candidate, &pid);
                HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                if (!process)
                    return TRUE;
                wchar_t actual[32768];
                DWORD length = 32768;
                bool match = QueryFullProcessImageNameW(process, 0, actual, &length) &&
                             !_wcsicmp(actual, search.image.c_str());
                CloseHandle(process);
                if (match) {
                    search.found = candidate;
                    return FALSE;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&search));
        if (search.found) {
            ShowWindow(search.found, SW_RESTORE);
            SetForegroundWindow(search.found);
        } else
            MessageBoxW(nullptr,
                        L"Server Manager is already starting. Open it from the system tray.",
                        L"cod4x1khz", MB_OK);
        CloseHandle(mutex);
        WSACleanup();
        CoUninitialize();
        return 0;
    }
    INITCOMMONCONTROLSEX controls{sizeof(controls),
                                  ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    WNDCLASSW page{};
    page.hInstance = instance;
    page.lpszClassName = L"KHZManagerPage";
    page.lpfnWndProc = PageProc;
    page.hbrBackground = app.background;
    RegisterClassW(&page);
    WNDCLASSW cls{};
    cls.hInstance = instance;
    cls.lpszClassName = L"KHZServerManager";
    cls.lpfnWndProc = Proc;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    cls.hbrBackground = app.background;
    RegisterClassW(&cls);
    unsigned dpi = GetDpiForSystem();
    auto window = CreateWindowExW(
        WS_EX_CONTROLPARENT, cls.lpszClassName, L"cod4x1khz Server Manager",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
        MulDiv(935, dpi, 96), MulDiv(732, dpi, 96), nullptr, nullptr, instance, nullptr);
    ShowWindow(window, show);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    CloseHandle(mutex);
    WSACleanup();
    CoUninitialize();
    return int(message.wParam);
}
