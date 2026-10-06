#pragma once
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <string>
#include <vector>

namespace khz::desktop {
inline constexpr COLORREF DarkBackground = RGB(22, 25, 31), DarkSurface = RGB(34, 39, 48),
    DarkHover = RGB(47, 56, 69), DarkText = RGB(230, 235, 243), DarkMuted = RGB(139, 151, 169),
    DarkAccent = RGB(47, 112, 202), DarkBorder = RGB(66, 78, 95);
inline HBRUSH DarkBrush(bool surface = false) {
    struct Brushes {
        HBRUSH background = CreateSolidBrush(DarkBackground), surface = CreateSolidBrush(DarkSurface);
        ~Brushes() { DeleteObject(background); DeleteObject(surface); }
    };
    static Brushes brushes;
    return surface ? brushes.surface : brushes.background;
}
inline void DarkFill(HDC dc, const RECT &rect, COLORREF color) {
    auto brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}
inline void DarkFrame(HDC dc, const RECT &rect, COLORREF color) {
    auto brush = CreateSolidBrush(color);
    FrameRect(dc, &rect, brush);
    DeleteObject(brush);
}
inline void DarkTitleBar(HWND window) {
    BOOL enabled = TRUE;
    // Windows 11 / current Windows 10. Older Windows builds ignore this attribute.
    DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &enabled, sizeof(enabled));
}
inline void DarkLabel(HDC dc, HWND window, std::wstring text, RECT rect, UINT flags, bool enabled = true) {
    auto font = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
    auto previous = font ? SelectObject(dc, font) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, enabled ? DarkText : DarkMuted);
    DrawTextW(dc, text.c_str(), int(text.size()), &rect, flags);
    if (previous) SelectObject(dc, previous);
}
inline std::wstring DarkControlText(HWND window) {
    std::wstring text(GetWindowTextLengthW(window) + 1, L'\0');
    text.resize(GetWindowTextW(window, text.data(), int(text.size())));
    return text;
}
inline void PaintDarkControl(HWND window, HDC dc) {
    RECT rect;
    GetClientRect(window, &rect);
    wchar_t name[64];
    GetClassNameW(window, name, 64);
    const auto type = std::wstring(name);
    const int pad = MulDiv(8, GetDpiForWindow(window), 96);
    if (type == L"Button") {
        auto style = GetWindowLongPtrW(window, GWL_STYLE);
        auto state = SendMessageW(window, BM_GETSTATE, 0, 0);
        bool check = (style & BS_TYPEMASK) == BS_AUTOCHECKBOX;
        bool enabled = IsWindowEnabled(window), hot = GetPropW(window, L"KHZDarkHot") != nullptr;
        bool primary = (style & BS_TYPEMASK) == BS_DEFPUSHBUTTON;
        DarkFill(dc, rect, check ? DarkBackground :
            enabled && primary ? DarkAccent : hot && enabled ? DarkHover : DarkSurface);
        if (check) {
            int size = MulDiv(16, GetDpiForWindow(window), 96);
            RECT box{rect.left, (rect.bottom - size) / 2, rect.left + size, (rect.bottom + size) / 2};
            bool checked = SendMessageW(window, BM_GETCHECK, 0, 0) == BST_CHECKED;
            DarkFill(dc, box, checked ? DarkAccent : DarkSurface);
            DarkFrame(dc, box, hot && enabled ? DarkAccent : DarkBorder);
            if (checked) {
                auto pen = CreatePen(PS_SOLID, MulDiv(2, GetDpiForWindow(window), 96), DarkText);
                auto old = SelectObject(dc, pen);
                MoveToEx(dc, box.left + size / 4, box.top + size / 2, nullptr);
                LineTo(dc, box.left + size * 9 / 20, box.top + size * 3 / 4);
                LineTo(dc, box.left + size * 4 / 5, box.top + size / 4);
                SelectObject(dc, old); DeleteObject(pen);
            }
            rect.left += size + pad;
        } else {
            DarkFrame(dc, rect, state & BST_PUSHED ? DarkText : DarkBorder);
            InflateRect(&rect, -pad, 0);
        }
        DarkLabel(dc, window, DarkControlText(window), rect,
                  DT_SINGLELINE | DT_VCENTER | (check ? DT_LEFT : DT_CENTER) | DT_END_ELLIPSIS, enabled);
        if (state & BST_FOCUS) {
            InflateRect(&rect, -2, -3);
            DrawFocusRect(dc, &rect);
        }
    } else if (type == L"ComboBox") {
        bool enabled = IsWindowEnabled(window);
        DarkFill(dc, rect, DarkSurface);
        DarkFrame(dc, rect, GetFocus() == window ? DarkAccent : DarkBorder);
        RECT label = rect; label.left += pad; label.right -= pad * 3;
        DarkLabel(dc, window, DarkControlText(window), label,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX, enabled);
        int x = rect.right - pad * 2, y = (rect.top + rect.bottom) / 2;
        POINT points[]{{x, y - 2}, {x + pad, y - 2}, {x + pad / 2, y + 3}};
        auto brush = CreateSolidBrush(enabled ? DarkText : DarkMuted);
        auto oldBrush = SelectObject(dc, brush), oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        Polygon(dc, points, 3);
        SelectObject(dc, oldPen); SelectObject(dc, oldBrush); DeleteObject(brush);
    } else {
        DarkFill(dc, rect, DarkBackground);
        bool tabs = type == WC_TABCONTROLW;
        int count = tabs ? TabCtrl_GetItemCount(window) : Header_GetItemCount(window);
        for (int i = 0; i < count; ++i) {
            RECT item;
            wchar_t text[256]{};
            if (tabs) {
                TabCtrl_GetItemRect(window, i, &item);
                TCITEMW info{}; info.mask = TCIF_TEXT; info.pszText = text; info.cchTextMax = 256;
                TabCtrl_GetItem(window, i, &info);
            } else {
                Header_GetItemRect(window, i, &item);
                HDITEMW info{}; info.mask = HDI_TEXT; info.pszText = text; info.cchTextMax = 256;
                Header_GetItem(window, i, &info);
            }
            bool selected = tabs && TabCtrl_GetCurSel(window) == i;
            DarkFill(dc, item, selected ? DarkHover : DarkSurface);
            if (selected) {
                RECT line = item; line.top = line.bottom - MulDiv(3, GetDpiForWindow(window), 96);
                DarkFill(dc, line, DarkAccent);
            }
            item.left += pad; item.right -= pad;
            DarkLabel(dc, window, text, item, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX |
                      (tabs ? DT_CENTER : DT_LEFT));
            if (selected && GetFocus() == window) { InflateRect(&item, -2, -4); DrawFocusRect(dc, &item); }
        }
    }
}
inline LRESULT CALLBACK DarkControlProc(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        auto dc = BeginPaint(window, &paint);
        PaintDarkControl(window, dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_PRINTCLIENT: PaintDarkControl(window, reinterpret_cast<HDC>(w)); return 0;
    case WM_MOUSEMOVE: {
        if (!GetPropW(window, L"KHZDarkHot")) {
            SetPropW(window, L"KHZDarkHot", reinterpret_cast<HANDLE>(1));
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0}; TrackMouseEvent(&track);
            InvalidateRect(window, nullptr, FALSE);
        }
        break;
    }
    case WM_MOUSELEAVE: RemovePropW(window, L"KHZDarkHot"); InvalidateRect(window, nullptr, FALSE); break;
    case WM_NCDESTROY:
        RemovePropW(window, L"KHZDarkHot");
        RemoveWindowSubclass(window, DarkControlProc, 1);
        break;
    }
    return DefSubclassProc(window, message, w, l);
}
inline void DarkControl(HWND window) {
    wchar_t name[64]; GetClassNameW(window, name, 64);
    auto type = std::wstring(name);
    SetWindowTheme(window, L"DarkMode_Explorer", nullptr);
    if (type == L"Button" || type == L"ComboBox" || type == WC_TABCONTROLW || type == WC_HEADERW)
        SetWindowSubclass(window, DarkControlProc, 1, 0);
    if (type == WC_LISTVIEWW) {
        ListView_SetBkColor(window, DarkSurface);
        ListView_SetTextBkColor(window, DarkSurface);
        ListView_SetTextColor(window, DarkText);
        DarkControl(ListView_GetHeader(window));
    }
    if (type == PROGRESS_CLASSW) {
        SetWindowTheme(window, L"", L"");
        SendMessageW(window, PBM_SETBKCOLOR, 0, DarkSurface);
        SendMessageW(window, PBM_SETBARCOLOR, 0, DarkAccent);
    }
}
inline bool DarkMessage(HWND window, UINT message, WPARAM w, LPARAM l, LRESULT &result) {
    if (message >= WM_CTLCOLORMSGBOX && message <= WM_CTLCOLORSTATIC) {
        wchar_t name[64]; GetClassNameW(reinterpret_cast<HWND>(l), name, 64);
        bool surface = message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX ||
                       std::wstring(name) == L"Edit";
        auto dc = reinterpret_cast<HDC>(w);
        SetBkColor(dc, surface ? DarkSurface : DarkBackground);
        SetTextColor(dc, IsWindowEnabled(reinterpret_cast<HWND>(l)) ? DarkText : DarkMuted);
        result = reinterpret_cast<LRESULT>(DarkBrush(surface));
        return true;
    }
    if (message == WM_DRAWITEM) {
        auto &item = *reinterpret_cast<DRAWITEMSTRUCT *>(l);
        if (item.CtlType == ODT_MENU) {
            DarkFill(item.hDC, item.rcItem, item.itemState & ODS_SELECTED ? DarkHover : DarkSurface);
            RECT rect = item.rcItem; rect.left += MulDiv(12, GetDpiForWindow(window), 96);
            DarkLabel(item.hDC, window, reinterpret_cast<const wchar_t *>(item.itemData), rect,
                      DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX, !(item.itemState & ODS_DISABLED));
            result = TRUE; return true;
        }
        if (item.CtlType != ODT_COMBOBOX) return false;
        DarkFill(item.hDC, item.rcItem, item.itemState & ODS_SELECTED ? DarkAccent : DarkSurface);
        if (item.itemID != UINT(-1)) {
            int size = int(SendMessageW(item.hwndItem, CB_GETLBTEXTLEN, item.itemID, 0));
            if (size != CB_ERR) {
                std::wstring text(size + 1, L'\0');
                SendMessageW(item.hwndItem, CB_GETLBTEXT, item.itemID, reinterpret_cast<LPARAM>(text.data()));
                RECT rect = item.rcItem; rect.left += MulDiv(6, GetDpiForWindow(window), 96);
                DarkLabel(item.hDC, item.hwndItem, text, rect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX,
                          !(item.itemState & ODS_DISABLED));
            }
        }
        if (item.itemState & ODS_FOCUS) DrawFocusRect(item.hDC, &item.rcItem);
        result = TRUE; return true;
    }
    if (message == WM_MEASUREITEM) {
        auto &item = *reinterpret_cast<MEASUREITEMSTRUCT *>(l);
        if (item.CtlType == ODT_MENU) {
            item.itemHeight = MulDiv(30, GetDpiForWindow(window), 96);
            item.itemWidth = MulDiv(215, GetDpiForWindow(window), 96); result = TRUE; return true;
        }
        if (item.CtlType == ODT_COMBOBOX) {
            item.itemHeight = MulDiv(23, GetDpiForWindow(window), 96); result = TRUE; return true;
        }
    }
    return false;
}

// App-owned modal notices share the same fixed dark palette as the main windows.
inline int DarkMessageBox(HWND owner, const wchar_t *text, const wchar_t *title, UINT flags) {
    struct Notice { const wchar_t *text; const wchar_t *title; UINT flags; HFONT font{}; } notice{text, title, flags};
    struct Template { DLGTEMPLATE dialog; WORD menu{}, windowClass{}, title{}; } data{};
    data.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER;
    data.dialog.cx = 290; data.dialog.cy = 105;
    return int(DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &data.dialog, owner,
        [](HWND window, UINT message, WPARAM w, LPARAM l) -> INT_PTR {
            auto *notice = reinterpret_cast<Notice *>(GetWindowLongPtrW(window, DWLP_USER));
            if (message == WM_INITDIALOG) {
                notice = reinterpret_cast<Notice *>(l); SetWindowLongPtrW(window, DWLP_USER, l);
                SetWindowTextW(window, notice->title); DarkTitleBar(window);
                unsigned dpi = GetDpiForWindow(window);
                notice->font = CreateFontW(-MulDiv(15, dpi, 96), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                    DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
                auto dc = GetDC(window); auto old = SelectObject(dc, notice->font);
                RECT label{0, 0, MulDiv(470, dpi, 96), 0};
                DrawTextW(dc, notice->text, -1, &label, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
                SelectObject(dc, old); ReleaseDC(window, dc);
                auto scale = [dpi](int v) { return MulDiv(v, dpi, 96); };
                RECT rect{0, 0, scale(514), label.bottom + scale(100)};
                AdjustWindowRectEx(&rect, GetWindowLongW(window, GWL_STYLE), FALSE, GetWindowLongW(window, GWL_EXSTYLE));
                SetWindowPos(window, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top, SWP_NOMOVE | SWP_NOZORDER);
                auto add = [&](const wchar_t *type, const wchar_t *caption, int id, RECT r, DWORD style) {
                    auto child = CreateWindowExW(0, type, caption, WS_CHILD | WS_VISIBLE | style,
                        r.left, r.top, r.right - r.left, r.bottom - r.top, window,
                        reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
                    SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(notice->font), TRUE); DarkControl(child);
                    return child;
                };
                add(L"STATIC", notice->text, 0, {scale(22), scale(22), scale(492), scale(22) + label.bottom}, SS_NOPREFIX);
                bool cancel = (notice->flags & MB_TYPEMASK) == MB_OKCANCEL;
                bool yesNo = (notice->flags & MB_TYPEMASK) == MB_YESNO;
                int y = label.bottom + scale(49), right = scale(492), width = scale(100);
                if (cancel || yesNo) add(L"BUTTON", cancel ? L"Cancel" : L"No", cancel ? IDCANCEL : IDNO,
                    {right - width, y, right, y + scale(30)}, WS_TABSTOP);
                if (cancel || yesNo) right -= width + scale(12);
                auto primary = add(L"BUTTON", yesNo ? L"Yes" : L"OK", yesNo ? IDYES : IDOK,
                    {right - width, y, right, y + scale(30)}, WS_TABSTOP | BS_DEFPUSHBUTTON);
                SetFocus(primary); return FALSE;
            }
            LRESULT result;
            if (DarkMessage(window, message, w, l, result)) return result;
            if (message == WM_ERASEBKGND) { RECT r; GetClientRect(window, &r); FillRect(reinterpret_cast<HDC>(w), &r, DarkBrush()); return TRUE; }
            if (message == WM_COMMAND && (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL || LOWORD(w) == IDYES || LOWORD(w) == IDNO)) {
                EndDialog(window, LOWORD(w)); return TRUE;
            }
            if (message == WM_CLOSE) { EndDialog(window, IDCANCEL); return TRUE; }
            if (message == WM_DESTROY && notice) DeleteObject(notice->font);
            return FALSE;
        }, reinterpret_cast<LPARAM>(&notice)));
}
} // namespace khz::desktop
