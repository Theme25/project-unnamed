// Integration test of real native controls in a hidden, isolated viewer instance.
// Run in a disposable test directory containing a copy of rbview.exe.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commctrl.h>

#include <cstdio>
#include <string>
#include "smartforce_config.h"

static DWORD childPid;
static HWND window, guide;
static int checks = 0;
static bool Check(bool ok, const char* label) {
    ++checks;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label); std::fflush(stdout);
    return ok;
}
static BOOL CALLBACK FindWindow(HWND h, LPARAM) {
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    wchar_t cls[64]; GetClassNameW(h, cls, 64);
    if (pid == childPid && !wcscmp(cls, L"RbViewer")) window = h;
    if (pid == childPid && !wcscmp(cls, L"RbSmartforceGuide")) guide = h;
    return TRUE;
}
static std::wstring Text(HWND h) {
    const int n = static_cast<int>(SendMessageW(h, WM_GETTEXTLENGTH, 0, 0));
    std::wstring s(n + 1, L'\0');
    s.resize(SendMessageW(h, WM_GETTEXT, s.size(), (LPARAM)s.data())); return s;
}
static HWND Field(HWND panel, const wchar_t* key) {
    const auto& fields = smartui::Settings();
    for (size_t i = 0; i < fields.size(); ++i) if (!wcscmp(fields[i].key, key)) {
        HWND owner = fields[i].page < 0 ? panel : GetDlgItem(panel, 4020 + fields[i].page);
        return GetDlgItem(owner, 5000 + static_cast<int>(i));
    }
    return nullptr;
}
static void Set(HWND panel, const wchar_t* key, const wchar_t* value) { SendMessageW(Field(panel, key), WM_SETTEXT, 0, (LPARAM)value); }
static void Choose(HWND panel, const wchar_t* key, int index) {
    HWND h = Field(panel, key); SendMessageW(h, CB_SETCURSEL, index, 0);
    SendMessageW(panel, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(h), CBN_SELCHANGE), (LPARAM)h);
}
static bool WaitDone(HWND panel, DWORD timeout) {
    const ULONGLONG deadline = GetTickCount64() + timeout;
    do {
        if (IsWindowEnabled(GetDlgItem(panel, 4001))) return true;
        Sleep(30);
    } while (GetTickCount64() < deadline && IsWindow(window));
    return false;
}
static bool Inside(HWND child, HWND parent) {
    RECT c{}, p{}; GetWindowRect(child, &c); GetWindowRect(parent, &p);
    return c.left >= p.left && c.top >= p.top && c.right <= p.right && c.bottom <= p.bottom && c.right > c.left && c.bottom > c.top;
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    SetProcessDPIAware();

    FILE* routeFile = std::fopen("ui-route.txt", "w");
    if (!routeFile) return 3;
    std::fputs("L17 C0 G0 I0 d7n2\n", routeFile); std::fclose(routeFile);
    std::wstring command = smartui::CommandLine({argv[1], L"ui-route.txt", L"--smartforce"});
    STARTUPINFOW si{}; si.cb = sizeof si; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(argv[1], command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return 3;
    childPid = pi.dwProcessId; CloseHandle(pi.hThread);
    WaitForInputIdle(pi.hProcess, 10000);
    bool ok = true;
    for (int i = 0; i < 300 && !window; ++i) { EnumWindows(FindWindow, 0); Sleep(20); }
    ok &= Check(window != nullptr, "viewer starts without a console");
    HWND panel = FindWindowExW(window, nullptr, L"RbSmartforce", nullptr);
    for (int i = 0; i < 300 && !panel; ++i) { Sleep(20); panel = FindWindowExW(window, nullptr, L"RbSmartforce", nullptr); }
    if (!Check(panel != nullptr, "Smartforce panel exists")) { PostMessageW(window, WM_CLOSE, 0, 0); return 1; }
    // The main HWND and panel HWND exist during WM_CREATE, before their children and settings.
    // Startup's --smartforce selects this page only after all initialization has completed.
    for (int i = 0; i < 500 && (!(GetWindowLongW(panel, GWL_STYLE) & WS_VISIBLE) || !GetDlgItem(panel, 4008)); ++i) Sleep(20);
    ok &= Check((GetWindowLongW(panel, GWL_STYLE) & WS_VISIBLE) != 0, "--smartforce selects the full-width tab");
    for (const auto& s : smartui::Settings()) ok &= Check(Field(panel, s.key) != nullptr, "search setting has a native control");
    ok &= Check(GetParent(Field(panel, L"update")) == GetDlgItem(panel, 4020), "update interval is on regular Settings");
    SendMessageW(GetDlgItem(panel, 4003), BM_CLICK, 0, 0); EnumWindows(FindWindow, 0);
    ok &= Check(guide && Text(GetDlgItem(guide, 4100)).find(L"SMARTFORCE STRING SYNTAX AND CONDITIONS") == 0, "String syntax opens the bundled guide without a separate file");
    ok &= Check(guide && !GetDlgItem(guide, 4101) && !GetDlgItem(guide, 4102), "guide has no Edit or Reload buttons");
    SendMessageW(guide, WM_COMMAND, IDCANCEL, 0); guide = nullptr;
    CreateDirectoryW(L"docs", nullptr);
    FILE* authoredGuide = std::fopen("docs/smartforce-string-guide.txt", "wb");
    if (authoredGuide) { std::fputs("My own guide\nUTF-8: \xc5\xbc\xc3\xb3\xc5\x82\xc4\x87\n", authoredGuide); std::fclose(authoredGuide); }
    SendMessageW(GetDlgItem(panel, 4003), BM_CLICK, 0, 0); EnumWindows(FindWindow, 0);
    ok &= Check(Text(GetDlgItem(guide, 4100)) == L"My own guide\r\nUTF-8: \u017c\u00f3\u0142\u0107\r\n", "opening uses the author's UTF-8 file and normalizes line breaks");
    SendMessageW(guide, WM_CLOSE, 0, 0); guide = nullptr;
    SendMessageW(GetDlgItem(panel, 4003), BM_CLICK, 0, 0); EnumWindows(FindWindow, 0);
    ok &= Check(guide && Text(GetDlgItem(guide, 4100)).find(L"My own guide") == 0, "reopening preserves the authored guide");
    SendMessageW(guide, WM_CLOSE, 0, 0); guide = nullptr;
    SendMessageW(GetDlgItem(panel, 4006), BM_CLICK, 0, 0);
    ok &= Check(Text(Field(panel, L"inputs")) == L"d7n2" && Text(Field(panel, L"level")) == L"17", "Use current route copies the loaded prefix and setup");
    Set(panel, L"level", L"17"); Set(panel, L"checkpoint", L"0"); Set(panel, L"threads", L"12");
    Set(panel, L"inputs", L"<d12+j1..3,20..70%>"); Set(panel, L"report", L"alive@12"); Set(panel, L"maxmem", L"0.1");
    Set(panel, L"savename", L"ui-results.txt");
    SendMessageW(Field(panel, L"save"), BM_CLICK, 0, 0);
    ok &= Check(IsWindowEnabled(Field(panel, L"savename")), "save checkbox enables filename");
    ok &= Check(!IsWindowEnabled(Field(panel, L"time")), "deterministic search disables time limit");
    SendMessageW(Field(panel, L"stochastic"), BM_CLICK, 0, 0);
    ok &= Check(IsWindowEnabled(Field(panel, L"time")) && IsWindowEnabled(Field(panel, L"seed")) && IsWindowEnabled(Field(panel, L"start-percent")) && IsWindowEnabled(Field(panel, L"update")), "random search enables time, update and seed");
    SendMessageW(Field(panel, L"stochastic"), BM_CLICK, 0, 0);
    SendMessageW(GetDlgItem(panel, 4001), BM_CLICK, 0, 0);
    ok &= Check(!IsWindowEnabled(Field(panel, L"inputs")) && !IsWindowEnabled(GetDlgItem(panel, 4001)), "running search locks its configuration");
    ok &= Check(WaitDone(panel, 15000), "search completes without blocking viewer");
    ok &= Check(Text(GetDlgItem(panel, 4007)).find(L"jump 1") != std::wstring::npos, "jump extrema reach live output");
    ok &= Check(Text(GetDlgItem(panel, 4008)).find(L"Finished") != std::wstring::npos, "completed status appears");
    ok &= Check(GetFileAttributesW(L"ui-results.txt") != INVALID_FILE_ATTRIBUTES, "GUI writes results");
    HWND tabs = GetDlgItem(panel, 4000);
    HDC dc = GetDC(window); const double dpi = GetDeviceCaps(dc, LOGPIXELSX) / 96.0; ReleaseDC(window, dc);
    const LPARAM click = MAKELPARAM(static_cast<int>(130 * dpi), static_cast<int>(12 * dpi));
    SendMessageW(tabs, WM_LBUTTONDOWN, MK_LBUTTON, click); SendMessageW(tabs, WM_LBUTTONUP, 0, click);
    ok &= Check(SendMessageW(tabs, TCM_GETCURSEL, 0, 0) == 1, "Advanced settings tab opens");
    SetWindowPos(window, nullptr, 0, 0, static_cast<int>(1280 * dpi), static_cast<int>(660 * dpi), SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOZORDER);
    HWND advanced = GetDlgItem(panel, 4021), basic = GetDlgItem(panel, 4020);
    bool regularFits = true;
    for (const auto& s : smartui::Settings()) if (s.page == 0) regularFits &= Inside(Field(panel, s.key), basic);
    ok &= Check(regularFits, "all regular settings fit the minimum window without scrolling");
    RECT tabRect{}, formRect{}, overlap{}; GetWindowRect(tabs, &tabRect); GetWindowRect(advanced, &formRect);
    ok &= Check(!IntersectRect(&overlap, &tabRect, &formRect), "tab background does not overlap settings controls");
    ok &= Check((GetWindowLongW(advanced, GWL_STYLE) & WS_CLIPCHILDREN) != 0, "settings page clips its own controls");
    // Both pages can fit at the window's enforced minimum size. Shrink the
    // child panel directly to exercise scrolling independently of field count.
    RECT panelRect{}; GetClientRect(panel, &panelRect);
    SetWindowPos(panel, nullptr, 0, 0, panelRect.right, static_cast<int>(300 * dpi), SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOZORDER);
    SCROLLINFO scroll{}; scroll.cbSize = sizeof scroll; scroll.fMask = SIF_ALL; GetScrollInfo(advanced, SB_VERT, &scroll);
    ok &= Check(scroll.nMax >= static_cast<int>(scroll.nPage), "short settings panel exposes a vertical scrollbar");
    SendMessageW(advanced, WM_VSCROLL, SB_BOTTOM, 0);
    ok &= Check(Inside(Field(panel, L"directory"), advanced), "scrollbar reaches the final setting");
    for (const auto& s : smartui::Settings()) if (s.page == 1) {
        HWND field = Field(panel, s.key);
        const int notification = s.kind == smartui::Kind::Check ? BN_SETFOCUS : s.kind == smartui::Kind::Choice ? CBN_SETFOCUS : EN_SETFOCUS;
        SendMessageW(advanced, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(field), notification), (LPARAM)field);
        ok &= Check(Inside(field, advanced), "focusing a setting scrolls it fully into view");
    }
    SendMessageW(advanced, WM_VSCROLL, SB_BOTTOM, 0);
    SendMessageW(Field(panel, L"directory"), WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0);
    scroll.fMask = SIF_ALL; GetScrollInfo(advanced, SB_VERT, &scroll);
    ok &= Check(scroll.nPos < scroll.nMax - static_cast<int>(scroll.nPage) + 1, "mouse wheel over a field scrolls its page");
    bool switches = true;
    for (int i = 0; i < 12; ++i) {
        const int selected = i % 2;
        const LPARAM at = MAKELPARAM(static_cast<int>((selected ? 130 : 25) * dpi), static_cast<int>(12 * dpi));
        SendMessageW(tabs, WM_LBUTTONDOWN, MK_LBUTTON, at); SendMessageW(tabs, WM_LBUTTONUP, 0, at);
        SetWindowPos(window, nullptr, 0, 0, static_cast<int>((i % 3 ? 1280 : 1500) * dpi), static_cast<int>((i % 3 ? 660 : 820) * dpi), SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOZORDER);
        switches &= !!(GetWindowLongW(basic, GWL_STYLE) & WS_VISIBLE) == (selected == 0);
        switches &= !!(GetWindowLongW(advanced, GWL_STYLE) & WS_VISIBLE) == (selected == 1);
        switches &= Inside(selected ? advanced : basic, panel);
    }
    ok &= Check(switches, "repeated tab switches and resizing leave exactly one settings page shown");
    Set(panel, L"inputs", L"d1..100n1..100a1..100e1..100");
    Set(panel, L"savename", L"ui-stopped.txt"); Choose(panel, L"mode", 1); Set(panel, L"top", L"1");
    SendMessageW(GetDlgItem(panel, 4001), BM_CLICK, 0, 0);
    Sleep(180); SendMessageW(GetDlgItem(panel, 4002), BM_CLICK, 0, 0);
    ok &= Check(WaitDone(panel, 15000), "Stop button waits for saving");
    ok &= Check(Text(GetDlgItem(panel, 4008)).find(L"Stopped") != std::wstring::npos, "stopped status appears");
    ok &= Check(Text(GetDlgItem(panel, 4007)).find(L"Ctrl+C") == std::wstring::npos, "live stop message uses the app's controls");
    ok &= Check(GetFileAttributesW(L"ui-stopped.txt") != INVALID_FILE_ATTRIBUTES, "Stop preserves result file");
    Set(panel, L"savename", L"ui-close.txt");
    SendMessageW(GetDlgItem(panel, 4001), BM_CLICK, 0, 0); Sleep(180);
    PostMessageW(window, WM_CLOSE, 0, 0);
    ok &= Check(WaitForSingleObject(pi.hProcess, 15000) == WAIT_OBJECT_0, "closing viewer waits for graceful worker stop");
    ok &= Check(GetFileAttributesW(L"ui-close.txt") != INVALID_FILE_ATTRIBUTES, "closing viewer preserves result file");
    CloseHandle(pi.hProcess);
    window = nullptr;
    command = smartui::CommandLine({argv[1], L"--smartforce"});
    if (CreateProcessW(argv[1], command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        childPid = pi.dwProcessId; CloseHandle(pi.hThread);
        panel = nullptr;
        for (int i = 0; i < 500; ++i) {
            EnumWindows(FindWindow, 0);
            if (window) panel = FindWindowExW(window, nullptr, L"RbSmartforce", nullptr);
            if (panel && (GetWindowLongW(panel, GWL_STYLE) & WS_VISIBLE) && GetDlgItem(panel, 4008)) break;
            Sleep(20);
        }
        ok &= Check(panel && Text(Field(panel, L"inputs")) == L"d1..100n1..100a1..100e1..100" &&
                    Text(Field(panel, L"savename")) == L"ui-close.txt" && Text(Field(panel, L"threads")) == L"12" &&
                    Text(Field(panel, L"maxmem")) == L"0.1" && SendMessageW(Field(panel, L"mode"), CB_GETCURSEL, 0, 0) == 1,
                    "Settings and Advanced settings survive restart");
        PostMessageW(window, WM_CLOSE, 0, 0);
        ok &= Check(WaitForSingleObject(pi.hProcess, 10000) == WAIT_OBJECT_0, "reopened viewer closes cleanly");
        CloseHandle(pi.hProcess);
    } else ok &= Check(false, "reopen viewer");
    std::printf("%s (%d checks)\n", ok ? "ALL PASSED" : "FAILED", checks);
    return ok ? 0 : 1;
}
