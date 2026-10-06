#pragma once
#include "host_transport.hpp"
#include "desktop_theme.hpp"
#include <commctrl.h>
#include <shobjidl.h>
#include <filesystem>
#include <string>
#include <vector>

namespace khz::desktop {
inline std::wstring Wide(std::string_view text) {
    if (text.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring result(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), result.data(), n);
    return result;
}
inline std::string Utf8(std::wstring_view text) {
    if (text.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr,
                                nullptr);
    std::string result(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), result.data(), n, nullptr,
                        nullptr);
    return result;
}
inline std::filesystem::path ImagePath() {
    wchar_t image[32768];
    auto n = GetModuleFileNameW(nullptr, image, std::size(image));
    return n && n < std::size(image) ? std::filesystem::path(std::wstring(image, n))
                                     : std::filesystem::path{};
}
inline std::wstring Text(HWND control) {
    int length = GetWindowTextLengthW(control);
    std::wstring text(length + 1, L'\0');
    GetWindowTextW(control, text.data(), length + 1);
    text.resize(length);
    return text;
}
inline void SetText(HWND control, std::string_view text) {
    auto wide = Wide(text);
    SetWindowTextW(control, wide.c_str());
}
inline HWND Control(HWND parent, const wchar_t *type, const wchar_t *text, int id, int x, int y,
                    int w, int h, DWORD style, HFONT font, unsigned dpi = 96) {
    auto scale = [&](int n) { return MulDiv(n, dpi, 96); };
    if (std::wstring_view(type) == L"COMBOBOX") style |= CBS_OWNERDRAWFIXED | CBS_HASSTRINGS;
    HWND result = CreateWindowExW(type == std::wstring_view(L"EDIT") ? WS_EX_CLIENTEDGE : 0, type,
                                  text, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style, scale(x), scale(y), scale(w),
                                  scale(h), parent, reinterpret_cast<HMENU>(INT_PTR(id)),
                                  GetModuleHandleW(nullptr), nullptr);
    SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    DarkControl(result);
    return result;
}
inline std::filesystem::path Pick(HWND parent, bool folder, bool save = false) {
    IFileDialog *dialog = nullptr;
    HRESULT hr = CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr))
        return {};
    DWORD flags = 0;
    dialog->GetOptions(&flags);
    dialog->SetOptions(flags | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : 0));
    if (!folder) {
        COMDLG_FILTERSPEC filter{L"Server configuration", L"*.cfg"};
        dialog->SetFileTypes(1, &filter);
        dialog->SetDefaultExtension(L"cfg");
    }
    std::filesystem::path path;
    if (SUCCEEDED(dialog->Show(parent))) {
        IShellItem *item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR value = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &value))) {
                path = value;
                CoTaskMemFree(value);
            }
            item->Release();
        }
    }
    dialog->Release();
    return path;
}
inline void Copy(HWND owner, const std::wstring &text) {
    if (!OpenClipboard(owner))
        return;
    auto memory = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (memory) {
        auto data = GlobalLock(memory);
        if (data) {
            memcpy(data, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(memory);
            EmptyClipboard();
            if (!SetClipboardData(CF_UNICODETEXT, memory))
                GlobalFree(memory);
        } else
            GlobalFree(memory);
    }
    CloseClipboard();
}
inline std::string QuotedText(std::string text) {
    for (auto &ch : text)
        if (ch == '"' || ch == '\\' || ch == ';' || static_cast<unsigned char>(ch) < 32)
            ch = ' ';
    return "\"" + text + "\"";
}
inline bool Launch(const std::filesystem::path &image, std::wstring arguments,
                   const std::filesystem::path &cwd) {
    auto command = hosting::Quote(image.wstring()) + L" " + arguments;
    STARTUPINFOW start{};
    start.cb = sizeof(start);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(image.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        cwd.c_str(), &start, &process))
        return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}
inline void ScaleDpi(HWND window, unsigned oldDpi, unsigned newDpi, HFONT &font, HFONT &titleFont,
                     const RECT &suggested) {
    struct Child {
        HWND window;
        RECT rect;
        bool title;
    };
    struct Layout {
        std::vector<Child> children;
        HFONT title;
    } layout{{}, titleFont};
    EnumChildWindows(
        window,
        [](HWND child, LPARAM state) -> BOOL {
            auto &layout = *reinterpret_cast<Layout *>(state);
            RECT rect;
            GetWindowRect(child, &rect);
            MapWindowPoints(nullptr, GetParent(child), reinterpret_cast<POINT *>(&rect), 2);
            layout.children.push_back(
                {child, rect,
                 reinterpret_cast<HFONT>(SendMessageW(child, WM_GETFONT, 0, 0)) == layout.title});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&layout));
    auto makeFont = [newDpi](int pixels, int weight) {
        return CreateFontW(-MulDiv(pixels, newDpi, 96), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET,
                           0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    };
    auto newFont = makeFont(15, FW_NORMAL), newTitle = makeFont(24, FW_SEMIBOLD);
    for (const auto &child : layout.children) {
        auto scale = [&](int value) { return MulDiv(value, newDpi, oldDpi); };
        SetWindowPos(child.window, nullptr, scale(child.rect.left), scale(child.rect.top),
                     scale(child.rect.right - child.rect.left),
                     scale(child.rect.bottom - child.rect.top), SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(child.window, WM_SETFONT,
                     reinterpret_cast<WPARAM>(child.title ? newTitle : newFont), TRUE);
        wchar_t name[64];
        GetClassNameW(child.window, name, 64);
        if (std::wstring_view(name) == L"ComboBox") {
            SendMessageW(child.window, CB_SETITEMHEIGHT, 0, MulDiv(23, newDpi, 96));
            SendMessageW(child.window, CB_SETITEMHEIGHT, WPARAM(-1), MulDiv(23, newDpi, 96));
        }
        if (std::wstring_view(name) == WC_LISTVIEWW)
            for (int column = 0; column < Header_GetItemCount(ListView_GetHeader(child.window));
                 ++column)
                ListView_SetColumnWidth(child.window, column,
                                        scale(ListView_GetColumnWidth(child.window, column)));
    }
    DeleteObject(font);
    DeleteObject(titleFont);
    font = newFont;
    titleFont = newTitle;
    SetWindowPos(window, nullptr, suggested.left, suggested.top, suggested.right - suggested.left,
                 suggested.bottom - suggested.top, SWP_NOZORDER | SWP_NOACTIVATE);
}
} // namespace khz::desktop
