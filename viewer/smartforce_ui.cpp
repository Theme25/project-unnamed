#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include "smartforce_ui.h"
#include "smartforce_config.h"
#include "../src/cli.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <io.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

namespace smartui {
namespace {
constexpr int ID_FIELDS = 5000;
enum { ID_TABS = 4000, ID_RUN, ID_STOP, ID_SYNTAX, ID_LOG, ID_FILE, ID_ROUTE, ID_OUTPUT, ID_STATUS, ID_HINT };
constexpr int ID_FORMS = 4020;
constexpr UINT WM_FORM_SCROLL = WM_APP + 73, WM_FORM_WHEEL = WM_APP + 74;
constexpr size_t LIVE_BYTES = 256 * 1024;

std::wstring Text(HWND h) {
    std::wstring s(GetWindowTextLengthW(h) + 1, L'\0');
    s.resize(GetWindowTextW(h, s.data(), static_cast<int>(s.size())));
    return s;
}
std::wstring ExePath() {
    std::wstring s(32768, L'\0');
    s.resize(GetModuleFileNameW(nullptr, s.data(), static_cast<DWORD>(s.size())));
    return s;
}
std::wstring ParentPath(const std::wstring& path) { return path.substr(0, path.find_last_of(L"\\/")); }
std::wstring WinError(DWORD e = GetLastError()) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, e, 0, (LPWSTR)&buf, 0, nullptr);
    std::wstring s = buf ? buf : L"Unknown Windows error";
    if (buf) LocalFree(buf);
    return s + L" (" + std::to_wstring(e) + L")";
}
bool Narrow(const std::wstring& s, std::string& out) {
    if (s.empty()) { out.clear(); return true; }
    BOOL replaced = FALSE;
    const bool utf8 = GetACP() == CP_UTF8;
    const DWORD flags = utf8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
    const int n = WideCharToMultiByte(CP_ACP, flags, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, utf8 ? nullptr : &replaced);
    if (!n || replaced) return false;
    out.resize(n);
    return WideCharToMultiByte(CP_ACP, flags, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, utf8 ? nullptr : &replaced) && !replaced;
}
std::wstring Widen(const std::string& s) {
    const int n = MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    if (n) MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}
std::string DisplayLine(std::string line) {
    // Translate engine diagnostics only. Patterns, filenames and result rows stay verbatim.
    auto replace = [&](const char* from, const char* to) {
        const size_t n = std::strlen(from); size_t at = 0;
        while ((at = line.find(from, at)) != std::string::npos) { line.replace(at, n, to); at += std::strlen(to); }
    };
    const auto starts = [&](const char* s) { return line.rfind(s, 0) == 0; };
    if (starts("--save:")) line.replace(0, 7, "Saving results:");
    else if (starts("--start-percent")) {
        replace("--start-percent", "Start from (%)"); replace("--stochastic", "Random search order"); replace("--seed", "Seed");
    }
    else if (starts("--maxmem") || starts("--sortby") || starts("--list --stochastic") || starts("out of memory:") ||
             starts("stopped early:") || starts("note: only the best rows") || line.find("threads need about") != std::string::npos) {
        replace("--maxmem", "Memory budget"); replace("--threads", "CPU threads"); replace("--sortby", "Sort results by");
        replace("--list --stochastic", "Listing strings in random order"); replace("--top 0", "Output rows = 0"); replace("--save", "Save all results");
    } else if (starts("report: level")) replace("(see --names)", "(use Show object names)");
    else if (starts("stopped by Ctrl+C")) replace("stopped by Ctrl+C", "Stopped");
    else if (starts("played ") || starts("visited ") || starts("position ")) { replace("stopped: Ctrl+C", "stopped: Stop requested"); replace("stopped: --samples", "stopped: sample limit"); }
    else if (line.find("strings match,") != std::string::npos) replace("stopped by Ctrl+C", "Stop requested");
    else if (starts("... ")) replace("--top 0 shows all", "set Output rows to 0 to show all");
    else if (starts("stochastic: ")) line.replace(0, 11, "Random order:");
    return line;
}
struct Handle {
    HANDLE h = nullptr;
    ~Handle() { Close(); }
    void Close() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); h = nullptr; }
};
struct StopWait { HANDLE event, done, parent; };
DWORD WINAPI WaitForStop(void* p) {
    const auto& w = *static_cast<StopWait*>(p);
    HANDLE handles[] = {w.done, w.event, w.parent};
    const DWORD r = WaitForMultipleObjects(w.parent ? 3 : 2, handles, FALSE, INFINITE);
    if (r == WAIT_OBJECT_0 + 1 || r == WAIT_OBJECT_0 + 2) RequestSmartforceStop();
    return 0;
}
struct Control { HWND label = nullptr, input = nullptr; const Setting* setting = nullptr; };

// Prefer the guide file beside the app; keep an embedded copy for standalone executables.
struct Guide {
    HWND hwnd = nullptr, text = nullptr, close = nullptr;
    HFONT font = nullptr;
    double dpi = 1;
    std::wstring path = ParentPath(ExePath()) + L"\\docs\\smartforce-string-guide.txt";
    int S(int n) const { return static_cast<int>(n * dpi + 0.5); }
    static std::string Embedded() {
        HRSRC res = FindResourceW(nullptr, L"SMARTFORCE_GUIDE", RT_RCDATA);
        if (!res) return {};
        const auto data = static_cast<const char*>(LockResource(LoadResource(nullptr, res)));
        return data ? std::string(data, SizeofResource(nullptr, res)) : std::string();
    }
    void Error(const std::wstring& message) { MessageBoxW(hwnd, message.c_str(), L"String syntax", MB_OK | MB_ICONERROR); }
    void Load() {
        std::string bytes;
        Handle file;
        file.h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file.h == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) { Error(L"Cannot read the guide: " + WinError(error)); return; }
            bytes = Embedded();
        } else {
            LARGE_INTEGER size{};
            if (!GetFileSizeEx(file.h, &size) || size.QuadPart > 4 * 1024 * 1024) { Error(L"The guide could not be read or is larger than 4 MB."); return; }
            bytes.resize(static_cast<size_t>(size.QuadPart));
            DWORD got = 0;
            if (!ReadFile(file.h, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr)) { Error(L"Cannot read the guide: " + WinError()); return; }
            bytes.resize(got);
        }
        if (bytes.compare(0, 3, "\xef\xbb\xbf") == 0) bytes.erase(0, 3);
        const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        if (!n && !bytes.empty()) { Error(L"The guide is not valid UTF-8 text."); return; }
        std::wstring contents(n, L'\0'), lines;
        if (n) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), contents.data(), n);
        for (size_t i = 0; i < contents.size(); ++i) {
            if (contents[i] == L'\r') { lines += L"\r\n"; if (i + 1 < contents.size() && contents[i + 1] == L'\n') ++i; }
            else if (contents[i] == L'\n') lines += L"\r\n";
            else lines += contents[i];
        }
        SetWindowTextW(text, lines.c_str());
    }
    void Init() {
        auto make = [&](const wchar_t* cls, const wchar_t* caption, DWORD style, int id, DWORD ex = 0) {
            HWND h = CreateWindowExW(ex, cls, caption, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, 0, 0, 1, 1,
                                    hwnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
            SendMessageW(h, WM_SETFONT, (WPARAM)font, FALSE); return h;
        };
        text = make(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 4100, WS_EX_CLIENTEDGE);
        SendMessageW(text, EM_SETLIMITTEXT, 4 * 1024 * 1024, 0);
        close = make(L"BUTTON", L"Close", BS_PUSHBUTTON, IDCANCEL);
        Load();
    }
    void Layout() {
        RECT r{}; GetClientRect(hwnd, &r);
        MoveWindow(text, S(10), S(10), std::max(1L, r.right - S(20)), std::max(1L, r.bottom - S(57)), TRUE);
        MoveWindow(close, r.right - S(100), r.bottom - S(37), S(90), S(27), TRUE);
    }
};
LRESULT CALLBACK GuideProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    auto g = reinterpret_cast<Guide*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        g = static_cast<Guide*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        g->hwnd = h; SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)g);
    }
    if (!g) return DefWindowProcW(h, m, wp, lp);
    switch (m) {
        case WM_CREATE: g->Init(); return 0;
        case WM_SIZE: g->Layout(); return 0;
        case WM_GETMINMAXINFO: {
            auto info = reinterpret_cast<MINMAXINFO*>(lp); info->ptMinTrackSize = {g->S(420), g->S(260)}; return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == IDCANCEL) DestroyWindow(h);
            return 0;
        case WM_CLOSE: DestroyWindow(h); return 0;
        case WM_NCDESTROY: SetWindowLongPtrW(h, GWLP_USERDATA, 0); delete g; return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

LRESULT CALLBACK FormProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_COMMAND || m == WM_NOTIFY) return SendMessageW(GetParent(h), m, wp, lp);
    if (m == WM_VSCROLL) return SendMessageW(GetParent(h), WM_FORM_SCROLL, wp, (LPARAM)h);
    if (m == WM_MOUSEWHEEL) return SendMessageW(GetParent(h), WM_FORM_WHEEL, wp, (LPARAM)h);
    return DefWindowProcW(h, m, wp, lp);
}
LRESULT CALLBACK FormInputProc(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    if (m == WM_MOUSEWHEEL && !SendMessageW(h, CB_GETDROPPEDSTATE, 0, 0))
        return SendMessageW(GetParent(h), m, wp, lp);
    if (m == WM_NCDESTROY) RemoveWindowSubclass(h, FormInputProc, id);
    return DefSubclassProc(h, m, wp, lp);
}

struct Panel {
    HWND hwnd = nullptr, parent = nullptr, tabs = nullptr, output = nullptr, status = nullptr, hint = nullptr, tips = nullptr;
    HWND run = nullptr, stop = nullptr, syntax = nullptr, log = nullptr, file = nullptr, route = nullptr;
    HWND forms[2] = {}, guide = nullptr;
    HFONT font = nullptr, mono = nullptr;
    double dpi = 1;
    std::wstring ini, logPath;
    Values values;
    std::vector<Control> controls;
    Handle process, stopEvent, reader;
    bool stopping = false;
    int page = 0;
    int wheelRemainder = 0;
    std::string tail;

    int S(int n) const { return static_cast<int>(n * dpi + 0.5); }
    HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0, HWND owner = nullptr) {
        HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1, owner ? owner : hwnd,
                                (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(h, WM_SETFONT, (WPARAM)font, FALSE);
        return h;
    }
    void Tip(HWND h, const wchar_t* text) {
        if (!h) return;
        TOOLINFOW ti{};
        ti.cbSize = sizeof ti; ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = hwnd; ti.uId = (UINT_PTR)h; ti.lpszText = const_cast<wchar_t*>(text);
        SendMessageW(tips, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
    HWND Input(const wchar_t* key) const {
        for (const auto& c : controls) if (!wcscmp(c.setting->key, key)) return c.input;
        return nullptr;
    }
    void Read() {
        for (const auto& c : controls) {
            const auto& s = *c.setting;
            if (s.kind == Kind::Check) values[s.key] = SendMessageW(c.input, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0";
            else if (s.kind == Kind::Choice) {
                const LRESULT i = SendMessageW(c.input, CB_GETCURSEL, 0, 0);
                values[s.key] = i >= 0 && static_cast<size_t>(i) < s.choices.size() ? s.choices[i].value : s.initial;
            } else values[s.key] = Text(c.input);
        }
    }
    void Fill() {
        for (const auto& c : controls) {
            const auto& s = *c.setting;
            if (s.kind == Kind::Check) SendMessageW(c.input, BM_SETCHECK, values[s.key] == L"1" ? BST_CHECKED : BST_UNCHECKED, 0);
            else if (s.kind == Kind::Choice) {
                int selected = 0;
                for (size_t i = 0; i < s.choices.size(); ++i) if (values[s.key] == s.choices[i].value) selected = static_cast<int>(i);
                SendMessageW(c.input, CB_SETCURSEL, selected, 0);
            } else SetWindowTextW(c.input, values[s.key].c_str());
        }
        UpdateEnabled();
    }
    void UpdateEnabled() {
        for (const auto& c : controls) {
            const bool enabled = !process.h && Enabled(*c.setting, values);
            EnableWindow(c.input, enabled);
            if (c.label) EnableWindow(c.label, enabled);
        }
        EnableWindow(run, !process.h); EnableWindow(stop, process.h && !stopping);
        EnableWindow(file, !process.h && values[L"mode"] != L"names"); EnableWindow(route, !process.h);
        EnableWindow(log, !logPath.empty());
    }
    void Save() {
        Read();
        for (const auto& s : Settings()) {
            // INI readers strip surrounding quotes. Store a prefix to preserve literal patterns/paths.
            const std::wstring encoded = L"v:" + values[s.key];
            WritePrivateProfileStringW(L"smartforce", s.key, encoded.c_str(), ini.c_str());
        }
    }
    void Init() {
        values = Defaults(std::thread::hardware_concurrency());
        values[L"directory"] = ParentPath(ExePath());
        RouteContext ctx;
        SendMessageW(parent, WM_CURRENT_ROUTE, 0, (LPARAM)&ctx);
        values[L"level"] = std::to_wstring(ctx.route.level);
        values[L"checkpoint"] = std::to_wstring(ctx.route.checkpoint);
        values[L"gless"] = ctx.route.gless ? L"1" : L"0";
        values[L"mathspikes"] = std::to_wstring(ctx.display.mathSpikes);
        values[L"window"] = std::to_wstring(ctx.display.windowW) + L"x" + std::to_wstring(ctx.display.windowH);
        values[L"scale"] = ctx.display.showAll ? L"showall" : L"noscale";
        values[L"player"] = Widen(rb::PlayerName(ctx.player));
        for (const auto& s : Settings()) {
            std::wstring buf(65536, L'\0');
            buf.resize(GetPrivateProfileStringW(L"smartforce", s.key, L"", buf.data(), static_cast<DWORD>(buf.size()), ini.c_str()));
            if (buf.rfind(L"v:", 0) == 0) values[s.key] = buf.substr(2);
        }
        tabs = Make(WC_TABCONTROLW, L"", WS_TABSTOP | WS_CLIPSIBLINGS, ID_TABS);
        for (const wchar_t* name : {L"Settings", L"Advanced settings"}) {
            TCITEMW ti{}; ti.mask = TCIF_TEXT; ti.pszText = const_cast<wchar_t*>(name);
            TabCtrl_InsertItem(tabs, TabCtrl_GetItemCount(tabs), &ti);
        }
        for (int i = 0; i < 2; ++i)
            forms[i] = Make(L"RbSmartforceForm", L"", WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_VSCROLL,
                            ID_FORMS + i, WS_EX_CONTROLPARENT);
        tips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                              CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(tips, TTM_SETMAXTIPWIDTH, 0, S(520));
        SendMessageW(tips, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);
        controls.reserve(Settings().size());
        for (const auto& s : Settings()) {
            Control c; c.setting = &s;
            HWND owner = s.page < 0 ? hwnd : forms[s.page];
            if (s.kind != Kind::Check) c.label = Make(L"STATIC", s.label, SS_LEFT, 0, 0, owner);
            const int id = ID_FIELDS + static_cast<int>(controls.size());
            if (s.kind == Kind::Check) c.input = Make(L"BUTTON", s.label, BS_AUTOCHECKBOX | BS_NOTIFY | WS_TABSTOP, id, 0, owner);
            else if (s.kind == Kind::Choice) {
                c.input = Make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, id, 0, owner);
                for (const auto& choice : s.choices) SendMessageW(c.input, CB_ADDSTRING, 0, (LPARAM)choice.label);
            } else {
                c.input = Make(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, id, WS_EX_CLIENTEDGE, owner);
                SendMessageW(c.input, EM_SETLIMITTEXT, 1000000, 0);
            }
            if (s.page >= 0) SetWindowSubclass(c.input, FormInputProc, 1, 0);
            Tip(c.input, s.help); Tip(c.label, s.help);
            controls.push_back(c);
        }
        file = Make(L"BUTTON", L"Insert file...", BS_PUSHBUTTON | WS_TABSTOP, ID_FILE);
        route = Make(L"BUTTON", L"Use current route", BS_PUSHBUTTON | WS_TABSTOP, ID_ROUTE);
        run = Make(L"BUTTON", L"Start", BS_PUSHBUTTON | WS_TABSTOP, ID_RUN);
        stop = Make(L"BUTTON", L"Stop and finish saving", BS_PUSHBUTTON | WS_TABSTOP, ID_STOP);
        syntax = Make(L"BUTTON", L"String syntax", BS_PUSHBUTTON | WS_TABSTOP, ID_SYNTAX);
        log = Make(L"BUTTON", L"Open full log", BS_PUSHBUTTON | WS_TABSTOP, ID_LOG);
        output = Make(L"EDIT", L"Results will appear here.\r\n\r\nEnter an input pattern and the conditions you want to find,\r\nthen press Start.\r\n\r\nString syntax opens the guide.\r\nHover or focus a setting for help.",
                      ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP, ID_OUTPUT, WS_EX_CLIENTEDGE);
        mono = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FIXED_PITCH, L"Consolas");
        if (mono) SendMessageW(output, WM_SETFONT, (WPARAM)mono, FALSE);
        SendMessageW(output, EM_SETLIMITTEXT, LIVE_BYTES * 2, 0);
        status = Make(L"STATIC", L"Ready. Search settings are independent of the route controls above.", SS_LEFTNOWORDWRAP, ID_STATUS);
        hint = Make(L"STATIC", L"Hover or focus a setting for help. Open String syntax for the guide.", SS_LEFT, ID_HINT);
        Tip(route, L"Copy the loaded route and its level, checkpoint, glitchless and display rules. Searching requires a route without invincibility or manual warps.");
        Tip(stop, L"Finish the current work and save results before stopping.");
        Tip(syntax, L"Open the input-string and condition guide.");
        Fill();
        Layout();
        SetTimer(hwnd, 1, 250, nullptr);
    }
    void Layout() {
        RECT rc{}; GetClientRect(hwnd, &rc);
        const int w = rc.right, h = rc.bottom, gap = S(8), row = S(27), step = S(31);
        const int left = w * 59 / 100, bodyY = S(102), bottom = h - S(24);
        auto put = [](HWND c, int x, int y, int width, int height) { MoveWindow(c, x, y, std::max(1, width), std::max(1, height), TRUE); };
        for (int i = 0; i < 2; ++i) {
            const auto& c = controls[i];
            put(c.label, 0, i * step + S(5), S(148), row - S(5));
            put(c.input, S(150), i * step, w - S(150) - S(152) - gap, row);
        }
        put(file, w - S(152), 0, S(152), row);
        put(route, w - S(152), step, S(152), row);
        int x = 0;
        for (auto item : {std::pair<HWND, int>{run, 80}, {stop, 176}, {syntax, 120}, {log, 112}}) {
            put(item.first, x, S(66), S(item.second), row); x += S(item.second) + gap;
        }
        // Only the tab header occupies this rectangle. Each page owns and clips its controls;
        // a full-height tab sibling would prevent the parent from repainting hidden controls.
        put(tabs, 0, bodyY, left, S(30));
        const int formY = bodyY + S(30), hintY = std::max(formY + S(32), bottom - S(40));
        for (int i = 0; i < 2; ++i) {
            ShowWindow(forms[i], i == page ? SW_SHOWNA : SW_HIDE);
            put(forms[i], 0, formY, left, hintY - formY - gap);
            LayoutForm(i);
        }
        put(hint, gap, hintY, left - 2 * gap, bottom - hintY);
        put(output, left + gap, bodyY, w - left - gap, bottom - bodyY);
        put(status, 0, h - S(20), w, S(20));
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }
    void LayoutForm(int index) {
        const int gap = S(8), step = S(28), row = S(25);
        int count = 0;
        for (const auto& c : controls) if (c.setting->page == index) ++count;
        RECT rc{}; GetClientRect(forms[index], &rc);
        SCROLLINFO si{}; si.cbSize = sizeof si; si.fMask = SIF_RANGE | SIF_PAGE;
        si.nMax = 2 * gap + ((count + 1) / 2) * step - 1; si.nPage = std::max(1L, rc.bottom);
        SetScrollInfo(forms[index], SB_VERT, &si, TRUE);
        si.fMask = SIF_POS; GetScrollInfo(forms[index], SB_VERT, &si);
        GetClientRect(forms[index], &rc);  // The scrollbar can change the available width.
        const int colW = (rc.right - 3 * gap) / 2;
        count = 0;
        for (const auto& c : controls) {
            if (c.setting->page != index) continue;
            const int x = gap + (count % 2) * (colW + gap), y = gap + (count / 2) * step - si.nPos;
            const int labelW = std::min(S(164), colW * 55 / 100);
            if (c.label) MoveWindow(c.label, x, y + S(5), labelW, row - S(5), TRUE);
            const bool check = c.setting->kind == Kind::Check;
            MoveWindow(c.input, x + (check ? 0 : labelW), y, std::max(1, colW - (check ? 0 : labelW)),
                       c.setting->kind == Kind::Choice ? S(260) : row, TRUE);
            ++count;
        }
    }
    void Scroll(HWND form, int request, int delta = 0) {
        SCROLLINFO si{}; si.cbSize = sizeof si; si.fMask = SIF_ALL; GetScrollInfo(form, SB_VERT, &si);
        int next = si.nPos;
        switch (request) {
            case SB_LINEUP: next -= S(31); break;
            case SB_LINEDOWN: next += S(31); break;
            case SB_PAGEUP: next -= si.nPage; break;
            case SB_PAGEDOWN: next += si.nPage; break;
            case SB_TOP: next = 0; break;
            case SB_BOTTOM: next = si.nMax; break;
            case SB_THUMBTRACK: case SB_THUMBPOSITION: next = si.nTrackPos; break;
            default: next += delta; break;
        }
        si.fMask = SIF_POS; si.nPos = next; SetScrollInfo(form, SB_VERT, &si, TRUE);
        LayoutForm(form == forms[0] ? 0 : 1);
        RedrawWindow(form, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }
    void Reveal(const Control& c) {
        if (c.setting->page < 0) return;
        HWND form = forms[c.setting->page];
        RECT r{}, client{}; GetWindowRect(c.input, &r); MapWindowPoints(nullptr, form, (POINT*)&r, 2); GetClientRect(form, &client);
        const int dy = r.top < S(4) ? r.top - S(4) : r.bottom > client.bottom - S(4) ? r.bottom - client.bottom + S(4) : 0;
        if (dy) Scroll(form, -1, dy);
    }
    void Error(const std::wstring& text) { MessageBoxW(hwnd, text.c_str(), L"Smartforce", MB_OK | MB_ICONERROR); }
    bool Args(std::vector<std::wstring>& args) {
        Read();
        std::wstring error, key;
        if (Arguments(values, args, error, key)) {
            // Normalize before changing the worker's base folder.
            std::wstring absolute(32768, L'\0');
            const DWORD n = GetFullPathNameW(values[L"directory"].c_str(), static_cast<DWORD>(absolute.size()), absolute.data(), nullptr);
            if (!n || n >= absolute.size()) { Error(L"Invalid Search folder path."); return false; }
            absolute.resize(n); values[L"directory"] = absolute;
            SetWindowTextW(Input(L"directory"), absolute.c_str());
            return true;
        }
        for (const auto& c : controls) if (c.setting->key == key) {
            if (c.setting->page >= 0) { page = c.setting->page; TabCtrl_SetCurSel(tabs, page); Layout(); }
            Error(error); SetFocus(c.input); return false;
        }
        Error(error); return false;
    }
    void Start() {
        if (process.h) return;
        std::vector<std::wstring> args;
        if (!Args(args)) return;
        const DWORD attr = GetFileAttributesW(values[L"directory"].c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) { Error(L"The Search folder does not exist."); return; }
        for (const auto& a : args) {
            std::string narrow;
            if (!Narrow(a, narrow)) { Error(L"A pattern, path or condition contains characters this Windows text encoding cannot read. Try a folder or filename using simpler characters."); return; }
        }
        const std::wstring unique = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        const std::wstring eventName = L"Local\\rbview-smartforce-" + unique;
        stopEvent.h = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
        if (!stopEvent.h) { Error(WinError()); return; }
        std::wstring directory = values[L"directory"] + L"\\smartforce-logs";
        if (!CreateDirectoryW(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            Error(L"Cannot create the full-log directory: " + WinError()); stopEvent.Close(); return;
        }
        SYSTEMTIME now; GetLocalTime(&now);
        wchar_t stamp[64]; swprintf(stamp, 64, L"%04d-%02d-%02d-%02d%02d%02d", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
        // Resolve before setting the worker's cwd, including a user-entered relative working directory.
        std::wstring path = directory + L"\\L" + values[L"level"] + L"-" + stamp + L"-" + unique + L".log";
        std::wstring absolute(32768, L'\0');
        const DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(absolute.size()), absolute.data(), nullptr);
        if (!length || length >= absolute.size()) { Error(L"Invalid log path."); stopEvent.Close(); return; }
        absolute.resize(length);
        Handle probe;
        probe.h = CreateFileW(absolute.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (probe.h == INVALID_HANDLE_VALUE) { Error(L"Cannot create the full log: " + WinError()); stopEvent.Close(); return; }
        probe.Close();
        const std::wstring exe = ExePath();
        std::vector<std::wstring> worker = {exe, L"--smartforce-worker", eventName, absolute, std::to_wstring(GetCurrentProcessId())};
        worker.insert(worker.end(), args.begin(), args.end());
        std::wstring command = CommandLine(worker);
        if (command.size() >= 32767) { Error(L"This pattern is too long to start directly. Save a long fixed prefix in a file and reference it with {FILE}."); stopEvent.Close(); return; }
        STARTUPINFOW si{}; si.cb = sizeof si; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                            values[L"directory"].c_str(), &si, &pi)) {
            Error(L"Cannot start Smartforce: " + WinError()); stopEvent.Close(); return;
        }
        CloseHandle(pi.hThread); process.h = pi.hProcess;
        reader.Close(); logPath = absolute; stopping = false; tail.clear();
        SetWindowTextW(output, L"Starting Smartforce...\r\n");
        SetWindowTextW(status, (L"Running. Full log: " + logPath).c_str());
        Save(); UpdateEnabled();
    }
    void RequestStop() {
        if (!process.h || stopping) return;
        SetEvent(stopEvent.h); stopping = true;
        SetWindowTextW(status, L"Stopping: waiting for current work and result saving to finish...");
        UpdateEnabled();
    }
    void Tail() {
        if (!reader.h && !logPath.empty()) {
            reader.h = CreateFileW(logPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (reader.h == INVALID_HANDLE_VALUE) { reader.h = nullptr; return; }
        }
        if (!reader.h) return;
        bool changed = false;
        char buf[32768]; DWORD got;
        // Bound each UI tick even with --top 0. Seek to the tail when a burst is very large.
        LARGE_INTEGER size{}, position{}; SetFilePointerEx(reader.h, {}, &position, FILE_CURRENT);
        if (GetFileSizeEx(reader.h, &size) && size.QuadPart - position.QuadPart > static_cast<LONGLONG>(LIVE_BYTES * 2)) {
            LARGE_INTEGER offset{}; offset.QuadPart = size.QuadPart - LIVE_BYTES;
            SetFilePointerEx(reader.h, offset, nullptr, FILE_BEGIN); tail.clear();
        }
        for (int i = 0; i < 16 && ReadFile(reader.h, buf, sizeof buf, &got, nullptr) && got; ++i) {
            tail.append(buf, got); changed = true;
        }
        if (!changed) return;
        if (tail.size() > LIVE_BYTES) {
            const size_t cut = tail.find('\n', tail.size() - LIVE_BYTES);
            tail.erase(0, cut == std::string::npos ? tail.size() - LIVE_BYTES : cut + 1);
        }
        std::string display, line;
        display.reserve(tail.size() * 2);
        for (size_t i = 0; i < tail.size(); ++i) {
            if (tail[i] == '\r' || tail[i] == '\n') {
                display += DisplayLine(line) + "\r\n"; line.clear();
                if (tail[i] == '\r' && i + 1 < tail.size() && tail[i + 1] == '\n') ++i;
            } else line += tail[i];
        }
        display += DisplayLine(line);
        DWORD start = 0, end = 0;
        SendMessageW(output, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
        const bool follow = GetFocus() != output;  // selecting/copying output must not move the selection
        const int firstLine = static_cast<int>(SendMessageW(output, EM_GETFIRSTVISIBLELINE, 0, 0));
        SetWindowTextW(output, Widen(display).c_str());
        if (follow) { SendMessageW(output, EM_SETSEL, -1, -1); SendMessageW(output, EM_SCROLLCARET, 0, 0); }
        else { SendMessageW(output, EM_SETSEL, start, end); SendMessageW(output, EM_LINESCROLL, 0, firstLine); }
    }
    void Poll() {
        if (!process.h) return;
        Tail();
        if (WaitForSingleObject(process.h, 0) != WAIT_OBJECT_0) return;
        Tail();
        DWORD code = 3; GetExitCodeProcess(process.h, &code);
        process.Close(); stopEvent.Close(); reader.Close();
        std::wstring text = code == 0 ? (stopping ? (values[L"save"] == L"1" ? L"Stopped; result saving finished." : L"Stopped; results are in the output.") : L"Finished.") :
                            code == 1 ? (stopping ? L"Stopped; no matches found before stopping." : L"Finished; no matches.") :
                            L"Search failed (exit " + std::to_wstring(code) + L"). See the output or full log.";
        SetWindowTextW(status, text.c_str()); stopping = false; UpdateEnabled();
        PostMessageW(parent, WM_SEARCH_FINISHED, code, 0);
    }
    void OpenGuide() {
        if (!IsWindow(guide)) {
            auto g = new Guide; g->font = font; g->dpi = dpi;
            RECT r{}; GetWindowRect(GetAncestor(hwnd, GA_ROOT), &r);
            guide = CreateWindowExW(WS_EX_CONTROLPARENT, L"RbSmartforceGuide", L"String syntax", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                    r.left + S(30), r.top + S(30), S(760), S(600), GetAncestor(hwnd, GA_ROOT), nullptr, GetModuleHandleW(nullptr), g);
        }
        if (IsWindowVisible(hwnd)) { ShowWindow(guide, SW_SHOWNORMAL); SetForegroundWindow(guide); SetFocus(GetDlgItem(guide, 4100)); }
    }
    void InsertFile() {
        wchar_t path[32768] = L"";
        OPENFILENAMEW of{}; of.lStructSize = sizeof of; of.hwndOwner = hwnd;
        of.lpstrFilter = L"Route / Smartforce files\0*.*\0"; of.lpstrFile = path; of.nMaxFile = 32768;
        of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&of)) return;
        // Absolute paths work regardless of the configured cwd. The pattern parser owns their interpretation.
        const std::wstring pattern = L"{" + std::wstring(path) + L"}";
        SendMessageW(Input(L"inputs"), EM_REPLACESEL, TRUE, (LPARAM)pattern.c_str());
        SetFocus(Input(L"inputs"));
    }
    void UseRoute() {
        RouteContext ctx;
        SendMessageW(parent, WM_CURRENT_ROUTE, 0, (LPARAM)&ctx);
        if (ctx.route.Tool()) { Error(L"Searching does not support invincibility or manual warps. Load a route without these changes before using it as a starting string."); return; }
        Read();
        values[L"inputs"] = ctx.route.in.empty() ? L"" : Widen(rb::EncodeInputsRLE(ctx.route.in));
        values[L"level"] = std::to_wstring(ctx.route.level); values[L"checkpoint"] = std::to_wstring(ctx.route.checkpoint);
        values[L"gless"] = ctx.route.gless ? L"1" : L"0";
        values[L"mathspikes"] = std::to_wstring(ctx.display.mathSpikes);
        values[L"window"] = std::to_wstring(ctx.display.windowW) + L"x" + std::to_wstring(ctx.display.windowH);
        values[L"scale"] = ctx.display.showAll ? L"showall" : L"noscale"; values[L"trig"] = L"intel";
        values[L"player"] = Widen(rb::PlayerName(ctx.player));
        Fill(); SetFocus(Input(L"inputs")); SendMessageW(Input(L"inputs"), EM_SETSEL, -1, -1);
    }
    void Command(int id, int code) {
        if (id >= ID_FIELDS && id < ID_FIELDS + static_cast<int>(controls.size())) {
            const auto& c = controls[id - ID_FIELDS];
            if (code == EN_SETFOCUS || code == CBN_SETFOCUS || code == BN_SETFOCUS) { SetWindowTextW(hint, c.setting->help); Reveal(c); }
            if (code == BN_CLICKED || code == CBN_SELCHANGE) { Read(); UpdateEnabled(); }
            return;
        }
        if (code != BN_CLICKED) return;
        if (id == ID_RUN) Start();
        else if (id == ID_STOP) RequestStop();
        else if (id == ID_SYNTAX) OpenGuide();
        else if (id == ID_FILE && !process.h) InsertFile();
        else if (id == ID_ROUTE && !process.h) UseRoute();
        else if (id == ID_LOG && !logPath.empty()) {
            const auto r = (INT_PTR)ShellExecuteW(hwnd, L"open", L"notepad.exe", QuoteWindows(logPath).c_str(), nullptr, SW_SHOWNORMAL);
            if (r <= 32) Error(L"Cannot open the log. Its path is:\r\n" + logPath);
        }
    }
    ~Panel() {
        if (IsWindow(guide)) DestroyWindow(guide);
        if (process.h) SetEvent(stopEvent.h);  // abnormal teardown still asks the independent worker to save
        if (mono) DeleteObject(mono);
    }
};

LRESULT CALLBACK PanelProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    Panel* p = reinterpret_cast<Panel*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        p = static_cast<Panel*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        p->hwnd = h; SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)p);
    }
    if (!p) return DefWindowProcW(h, m, wp, lp);
    switch (m) {
        case WM_CREATE: p->Init(); return 0;
        case WM_SIZE: p->Layout(); return 0;
        case WM_COMMAND: p->Command(LOWORD(wp), HIWORD(wp)); return 0;
        case WM_FORM_SCROLL: p->Scroll((HWND)lp, LOWORD(wp)); return 0;
        case WM_FORM_WHEEL: {
            p->wheelRemainder += GET_WHEEL_DELTA_WPARAM(wp);
            UINT lines = 3; SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
            SCROLLINFO si{}; si.cbSize = sizeof si; si.fMask = SIF_PAGE; GetScrollInfo((HWND)lp, SB_VERT, &si);
            const int distance = lines == WHEEL_PAGESCROLL ? static_cast<int>(si.nPage) : p->S(31) * static_cast<int>(lines);
            p->Scroll((HWND)lp, -1, -p->wheelRemainder / WHEEL_DELTA * distance);
            p->wheelRemainder %= WHEEL_DELTA;
            return 0;
        }
        case WM_NOTIFY:
            if (reinterpret_cast<NMHDR*>(lp)->hwndFrom == p->tabs && reinterpret_cast<NMHDR*>(lp)->code == TCN_SELCHANGE) {
                p->page = TabCtrl_GetCurSel(p->tabs); p->Layout();
            }
            return 0;
        case WM_TIMER: p->Poll(); return 0;
        case WM_DESTROY: KillTimer(h, 1); p->Save(); return 0;
        case WM_NCDESTROY: SetWindowLongPtrW(h, GWLP_USERDATA, 0); delete p; return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}
Panel* GetPanel(HWND h) { return reinterpret_cast<Panel*>(GetWindowLongPtrW(h, GWLP_USERDATA)); }
}  // namespace

bool WorkerMainIfRequested(int& exitCode) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc < 2 || wcscmp(argv[1], L"--smartforce-worker")) { if (argv) LocalFree(argv); return false; }
    exitCode = 3;
    if (argc < 6) { LocalFree(argv); return true; }
    // GUI-subsystem processes have no console streams. Reopen explicitly and make both descriptors
    // share one file position; shell redirection and inherited handles are not involved.
    if (!_wfreopen(argv[3], L"wb", stdout) || !_wfreopen(argv[3], L"ab", stderr) || _dup2(_fileno(stdout), _fileno(stderr))) {
        LocalFree(argv); return true;
    }
    setvbuf(stdout, nullptr, _IONBF, 0); setvbuf(stderr, nullptr, _IONBF, 0);
    Handle event, done, parent, thread;
    event.h = OpenEventW(SYNCHRONIZE, FALSE, argv[2]);
    done.h = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    parent.h = OpenProcess(SYNCHRONIZE, FALSE, wcstoul(argv[4], nullptr, 10));
    std::vector<std::string> strings = {"rbview-smartforce"};
    bool valid = event.h && done.h;
    for (int i = 5; i < argc && valid; ++i) {
        std::string s;
        valid = Narrow(argv[i], s); strings.push_back(std::move(s));
    }
    LocalFree(argv);
    if (!valid) { std::fprintf(stderr, "Cannot initialize the Smartforce worker or encode its arguments.\n"); return true; }
    std::vector<char*> narrow;
    for (auto& s : strings) narrow.push_back(s.data());
    narrow.push_back(nullptr);
    StopWait wait{event.h, done.h, parent.h};
    thread.h = CreateThread(nullptr, 0, WaitForStop, &wait, 0, nullptr);
    if (!thread.h) { std::fprintf(stderr, "Cannot create the search stop handler.\n"); return true; }
    if (!parent.h) RequestSmartforceStop();
    exitCode = RbsimMain(static_cast<int>(strings.size()), narrow.data());
    SetEvent(done.h); WaitForSingleObject(thread.h, INFINITE);
    std::fflush(stdout); std::fflush(stderr);
    return true;
}

HWND Create(HWND parent, HFONT font, double dpi, const std::wstring& ini) {
    WNDCLASSEXW wc{}; wc.cbSize = sizeof wc; wc.lpfnWndProc = PanelProc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.lpszClassName = L"RbSmartforce";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = FormProc; wc.lpszClassName = L"RbSmartforceForm"; RegisterClassExW(&wc);
    wc.lpfnWndProc = GuideProc; wc.lpszClassName = L"RbSmartforceGuide"; RegisterClassExW(&wc);
    auto p = new Panel;
    p->parent = parent; p->font = font; p->dpi = dpi; p->ini = ini;
    return CreateWindowExW(WS_EX_CONTROLPARENT, L"RbSmartforce", L"Smartforce", WS_CHILD | WS_CLIPCHILDREN,
                           0, 0, 1, 1, parent, nullptr, wc.hInstance, p);
}
bool Running(HWND panel) { const auto p = GetPanel(panel); return p && p->process.h; }
void Stop(HWND panel) { if (auto p = GetPanel(panel)) p->RequestStop(); }
bool DialogMessage(HWND panel, MSG& message) {
    if (!panel) return false;
    const auto p = GetPanel(panel);
    HWND focus = GetFocus();
    if (p && IsWindow(p->guide) && (focus == p->guide || IsChild(p->guide, focus))) {
        if (!IsDialogMessageW(p->guide, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
        return true;
    }
    if (!IsWindowVisible(panel)) return false;
    if (focus != panel && !IsChild(panel, focus)) return false;
    if (!IsDialogMessageW(panel, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    return true;
}
}  // namespace smartui
