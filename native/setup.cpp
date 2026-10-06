#include "desktop_ui.hpp"
#include "install_package.hpp"
#include <thread>
#include <atomic>
#include <memory>

using namespace khz::desktop;
namespace {
enum {
    Source = 101,
    BrowseSource,
    Destination,
    BrowseDestination,
    Next,
    Back,
    Cancel,
    LaunchGame,
    LaunchManager
};
constexpr UINT ProgressMessage = WM_APP + 1, FinishedMessage = WM_APP + 2;
struct Progress {
    unsigned percent;
    std::string text;
};
struct Wizard {
    HWND window{}, source{}, destination{}, description{}, progress{}, status{}, next{}, back{},
        cancel{};
    HFONT font{}, titleFont{};
    unsigned dpi = 96;
    int step = 0;
    bool busy = false, finished = false;
    std::atomic_bool cancellation = false;
    std::thread worker;
    khz::setup::Plan plan;
    std::string error;
    ~Wizard() {
        cancellation = true;
        if (worker.joinable())
            worker.join();
    }
    HWND C(const wchar_t *type, const wchar_t *text, int id, int x, int y, int w, int h,
           DWORD style = 0) {
        return Control(window, type, text, id, x, y, w, h, style, font, dpi);
    }
    void Create() {
        dpi = GetDpiForWindow(window);
        font = CreateFontW(-MulDiv(15, dpi, 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                           CLEARTYPE_QUALITY, 0, L"Segoe UI");
        titleFont = CreateFontW(-MulDiv(26, dpi, 96), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                                DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        auto title = C(L"STATIC", L"Install cod4x1khz", 0, 28, 24, 610, 42);
        SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(titleFont), TRUE);
        description = C(L"STATIC", L"", 0, 28, 84, 615, 76);
        source = C(L"EDIT", khz::setup::DetectSteam().c_str(), Source, 28, 178, 495, 29,
                   WS_TABSTOP | ES_AUTOHSCROLL);
        C(L"BUTTON", L"Browse…", BrowseSource, 535, 178, 105, 30, WS_TABSTOP);
        wchar_t local[32768];
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, std::size(local));
        auto output = n ? std::filesystem::path(local) / "cod4x1khz"
                        : std::filesystem::path(L"C:\\Games\\cod4x1khz");
        destination =
            C(L"EDIT", output.c_str(), Destination, 28, 247, 495, 29, WS_TABSTOP | ES_AUTOHSCROLL);
        C(L"BUTTON", L"Browse…", BrowseDestination, 535, 247, 105, 30, WS_TABSTOP);
        C(L"STATIC", L"Steam Call of Duty 4 (2007) folder", 0, 28, 151, 600, 22);
        C(L"STATIC", L"Standalone output folder", 0, 28, 220, 600, 22);
        progress = C(PROGRESS_CLASSW, L"", 0, 28, 299, 612, 22);
        status = C(L"STATIC", L"", 0, 28, 333, 612, 30);
        back = C(L"BUTTON", L"Back", Back, 285, 416, 105, 32, WS_TABSTOP);
        next = C(L"BUTTON", L"Next", Next, 405, 416, 115, 32, WS_TABSTOP | BS_DEFPUSHBUTTON);
        cancel = C(L"BUTTON", L"Cancel", Cancel, 535, 416, 105, 32, WS_TABSTOP);
        ShowStep();
    }
    void ShowStep() {
        const wchar_t *text =
            step == 0
                ? L"1 / 3 — Select your installed Steam copy of Call of Duty 4.\r\nThe installer "
                  L"copies game assets and leaves the original installation untouched."
            : step == 1 ? L"2 / 3 — Choose the standalone output folder.\r\nIt will contain the "
                          L"blue-icon game and the included Server Manager."
                        : L"3 / 3 — Ready to install.\r\nExisting cod4x1khz profiles, "
                          L"administration data and custom config are preserved.";
        SetWindowTextW(description, text);
        EnableWindow(source, step == 0);
        EnableWindow(GetDlgItem(window, BrowseSource), step == 0);
        EnableWindow(destination, step == 1);
        EnableWindow(GetDlgItem(window, BrowseDestination), step == 1);
        EnableWindow(back, step > 0);
        SetWindowTextW(next, step == 2 ? L"Install" : L"Next");
    }
    void Start() {
        error.clear();
        if (!khz::setup::Inspect(Text(source), Text(destination), plan, error)) {
            MessageBoxW(window, Wide(error).c_str(), L"Installation folder check",
                        MB_OK | MB_ICONWARNING);
            return;
        }
        busy = true;
        cancellation = false;
        EnableWindow(next, FALSE);
        EnableWindow(back, FALSE);
        EnableWindow(source, FALSE);
        EnableWindow(destination, FALSE);
        SetWindowTextW(description, plan.repair
                                        ? L"Repairing the installed project files…"
                                        : L"Installing the standalone game and Server Manager…");
        worker = std::thread([this] {
            bool ok = khz::setup::Install(
                plan, cancellation,
                [this](unsigned n, const std::string &text) {
                    auto update = new Progress{n, text};
                    if (!PostMessageW(window, ProgressMessage, 0, reinterpret_cast<LPARAM>(update)))
                        delete update;
                },
                error);
            PostMessageW(window, FinishedMessage, ok, 0);
        });
    }
    void Command(int id) {
        if (id == BrowseSource || id == BrowseDestination) {
            auto folder = Pick(window, true);
            if (!folder.empty())
                SetWindowTextW(id == BrowseSource ? source : destination, folder.c_str());
            return;
        }
        if (id == Back && !busy) {
            if (step > 0)
                --step;
            ShowStep();
            return;
        }
        if (id == Next && !busy) {
            if (finished) {
                DestroyWindow(window);
                return;
            }
            if (step < 2) {
                ++step;
                ShowStep();
            } else
                Start();
            return;
        }
        if (id == Cancel) {
            if (busy) {
                cancellation = true;
                SetWindowTextW(status, L"Cancelling safely after the current file…");
                EnableWindow(cancel, FALSE);
            } else
                DestroyWindow(window);
            return;
        }
        if (id == LaunchGame || id == LaunchManager) {
            Launch(plan.destination /
                       (id == LaunchGame ? L"cod4x1khz.exe" : L"cod4x1khz_server_manager.exe"),
                   L"", plan.destination);
        }
    }
} wizard;
LRESULT CALLBACK Proc(HWND window, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE:
        wizard.window = window;
        wizard.Create();
        return 0;
    case WM_DPICHANGED:
        ScaleDpi(window, wizard.dpi, HIWORD(w), wizard.font, wizard.titleFont,
                 *reinterpret_cast<RECT *>(l));
        wizard.dpi = HIWORD(w);
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkColor(reinterpret_cast<HDC>(w), RGB(255, 255, 255));
        SetTextColor(reinterpret_cast<HDC>(w), RGB(35, 58, 91));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    case WM_COMMAND:
        wizard.Command(LOWORD(w));
        return 0;
    case ProgressMessage: {
        std::unique_ptr<Progress> update(reinterpret_cast<Progress *>(l));
        SendMessageW(wizard.progress, PBM_SETPOS, update->percent, 0);
        SetText(wizard.status, update->text);
        return 0;
    }
    case FinishedMessage:
        if (wizard.worker.joinable())
            wizard.worker.join();
        wizard.busy = false;
        EnableWindow(wizard.cancel, TRUE);
        EnableWindow(wizard.next, TRUE);
        if (w) {
            wizard.finished = true;
            SetWindowTextW(wizard.description,
                           L"Installation complete.\r\nLaunch cod4x1khz.exe for normal play, or "
                           L"Server Manager to host a server.");
            SetWindowTextW(wizard.next, L"Finish");
            SetWindowTextW(wizard.cancel, L"Close");
            wizard.C(L"BUTTON", L"Launch game", LaunchGame, 28, 366, 170, 32, WS_TABSTOP);
            wizard.C(L"BUTTON", L"Open Server Manager", LaunchManager, 214, 366, 220, 32,
                     WS_TABSTOP);
        } else {
            SetText(wizard.status, wizard.error);
            wizard.step = 2;
            wizard.ShowStep();
        }
        return 0;
    case WM_CLOSE:
        wizard.Command(Cancel);
        return 0;
    case WM_DESTROY:
        DeleteObject(wizard.font);
        DeleteObject(wizard.titleFont);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, msg, w, l);
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&common);
    WNDCLASSW cls{};
    cls.hInstance = instance;
    cls.lpszClassName = L"KHZSetup";
    cls.lpfnWndProc = Proc;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&cls);
    auto dpi = GetDpiForSystem();
    auto window = CreateWindowExW(WS_EX_CONTROLPARENT, cls.lpszClassName, L"cod4x1khz Setup",
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                  CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(690, dpi, 96),
                                  MulDiv(515, dpi, 96), nullptr, nullptr, instance, nullptr);
    ShowWindow(window, show);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    CoUninitialize();
    return int(msg.wParam);
}
