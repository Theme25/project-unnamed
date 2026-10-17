// viewer/rbview.cpp - "Red Ball TAS": the Practice Hack's TAS mode (same keys, same behaviour) on the bit-exact
// simulator, in a Windows program with a proper UI: savestates without limits, a ghost route, the jump brute-forcer,
// rbsim's optimize/beam searches, and a route viewer (seek, events, death-warp ghost).
// The TAS logic itself is in viewer/tas.{h,cpp} (no Windows code, tested by viewer/tas_test.cpp).
//
// Build: make viewer  (cross-compiled with MinGW-w64; static, one .exe)
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../src/redball.h"
#include "tas.h"

int CmdOptimize(int argc, char** argv);
int CmdBeam(int argc, char** argv);

using namespace rb;
namespace gp = Gdiplus;

// ------------------------------------------------------------------ level names (from the SWF's title texts)
static const wchar_t* kLevelNames[18] = {L"",
                                         L"Move and jump",
                                         L"Funny ball",
                                         L"Lifts and thorns",
                                         L"Axes",
                                         L"Jump!",
                                         L"Springboards",
                                         L"Box bridge",
                                         L"Car",
                                         L"Ninja on the bridge",
                                         L"Red ball on the ball",
                                         L"Train",
                                         L"Sleep slope",
                                         L"Pakman",
                                         L"Catapult",
                                         L"Shop burglary",
                                         L"Short cut",
                                         L"The King"};

// ------------------------------------------------------------------ ids and state
enum : int {
    ID_TIMER = 1,
    ID_LEVEL = 100, ID_CHECKPOINT, ID_GLESS, ID_INVINCIBLE, ID_SPIKES, ID_WINDOW, ID_SCALE, ID_KEYS, ID_HELP,
    ID_INPUTS, ID_PASTE, ID_COPY, ID_OPEN, ID_SAVE, ID_LOAD,
    ID_FIRST, ID_BACK, ID_PLAY, ID_FWD, ID_LAST, ID_SPEED, ID_REALTIME, ID_FOLLOW, ID_TRAIL, ID_HUD, ID_SLIDER,
    IDC_CMD, IDC_CMDRUN, ID_FEEDBACK, ID_MODE,
    ID_CANVAS, ID_STATUS, ID_TABS,
    ID_EVENTS,
    ID_SLOTS, ID_SLOTSAVE, ID_SLOTLOAD, ID_SLOTIMPORT, ID_SLOTEXPORT,
    ID_NAMED, ID_NSAVE, ID_NLOAD, ID_NRENAME, ID_NDELETE, ID_NGHOST, ID_NCOPY,
    ID_BACKUPS, ID_BLOAD,
    ID_GCUR, ID_GPASTE, ID_GOPEN, ID_GCLEAR, ID_GINFO,
    ID_SMODE, ID_STIME, ID_STHREADS, ID_SMEM, ID_SMAXF, ID_SSTART, ID_SSTOP, ID_SLOAD, ID_SLOG, ID_SSTATUS,
    ID_BINFO, ID_BLIST, ID_BUP, ID_BDOWN, ID_BNEXT, ID_BAPPLY, ID_BLEAVE,
    ID_OFAST, ID_OPAUSEEND, ID_OKEEPEND, ID_OMARKS, ID_OPOLYS, ID_ODEC, ID_OKEYS, ID_WFILL, ID_WAPPLY, ID_WCLEAR,
};
enum : UINT { WM_SEARCH_LINE = WM_APP + 1, WM_SEARCH_DONE };

static HWND g_wnd, g_canvas, g_status, g_tabs;
static HWND g_lblLevel, g_level, g_lblCp, g_cp, g_gless, g_invincible, g_lblSpikes, g_spikes, g_lblWindow, g_window, g_scale, g_keys, g_help;
static HWND g_lblInputs, g_inputs, g_paste, g_copy, g_open, g_save, g_load;
static HWND g_first, g_back, g_play, g_fwd, g_last, g_lblSpeed, g_speed, g_realtime, g_follow, g_trail, g_hudBox, g_slider;
static HWND g_cmd, g_cmdRun, g_feedback, g_mode;
static HWND g_events;
static HWND g_lblSlots, g_slots, g_slotSave, g_slotLoad, g_slotImport, g_slotExport, g_lblNamed, g_named, g_nSave, g_nLoad, g_nRename, g_nDelete,
    g_nGhost, g_nCopy, g_lblBackups, g_backups, g_bLoad;
static HWND g_gInfo, g_gCur, g_gPaste, g_gOpen, g_gClear;
static HWND g_sMode, g_lblSTime, g_sTime, g_lblSThreads, g_sThreads, g_lblSMem, g_sMem, g_lblSMaxF, g_sMaxF, g_sStart, g_sStop, g_sLoad, g_sStatus, g_sLog;
static HWND g_bInfo, g_bList, g_bUp, g_bDown, g_bNext, g_bApply, g_bLeave;
static HWND g_speedLbl, g_grpPlay, g_fast, g_lblEnd, g_pauseEnd, g_keepEnd, g_lblFlag, g_grpView, g_marksBox, g_polysBox, g_lblDec, g_dec,
    g_grpWarp, g_lblWarp, g_wLbl[5], g_wEdit[5], g_wFill, g_wApply, g_wClear, g_grpKeys, g_lblKeys, g_oKeys;
static const int NPAGES = 6;
static std::vector<HWND> g_pages[NPAGES];
static HFONT g_font;
static double g_dpi = 1.0;

static tas::Session g_s;
static tas::Timeline g_ghost;
static bool g_ghostOn = false;
static std::wstring g_ghostName;

static bool g_hud = true, g_marks = false, g_polys = false, g_follow_on = true, g_trail_on = true;
static int g_decimals = 4;
static int g_remap[256];  // key -> key (Practice Hack "/remap"), 0 = none
static bool g_heldL, g_heldU, g_heldR, g_tapL, g_tapU, g_tapR;
static int g_ioWait = 0;  // 1: a digit imports the clipboard (I), -1: a digit exports (O)
static double g_userZoom = 1.0;
static double g_viewX = 0, g_viewY = 0;
static bool g_dragging = false;
static POINT g_dragFrom;
static double g_dragViewX, g_dragViewY;
static LARGE_INTEGER g_qpf, g_lastTick;
static std::wstring g_iniPath, g_statesPath;
static std::string g_shownRoute;
static size_t g_shownEvents = (size_t)-1;
static bool g_modal = false;

// search
static std::thread g_searchThread;
static std::atomic<bool> g_searchRunning{false};
static std::string g_searchBest;
static tas::Route g_searchBase;

static int S(int v) { return (int)std::lround(v * g_dpi); }
static std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }
static std::string Narrow(const std::wstring& s) {
    std::string o;
    for (wchar_t c : s) o += (c < 128) ? (char)c : '?';
    return o;
}
static std::wstring GetText(HWND h) {
    const int n = GetWindowTextLengthW(h);
    std::wstring s((size_t)n + 1, L'\0');
    GetWindowTextW(h, &s[0], n + 1);
    s.resize((size_t)n);
    return s;
}
static void SetText(HWND h, const std::wstring& t) {
    if (GetText(h) != t) SetWindowTextW(h, t.c_str());
}
static bool Checked(HWND h) { return SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED; }
static void SetCheck(HWND h, bool on) { SendMessageW(h, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }

static void RefreshAll();
static void RefreshStates();

// ------------------------------------------------------------------ clipboard
static std::wstring ClipboardText() {
    std::wstring s;
    if (!OpenClipboard(g_wnd)) return s;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* t = (const wchar_t*)GlobalLock(h)) {
            s = t;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return s;
}
static void SetClipboardText(const std::wstring& s) {
    if (!OpenClipboard(g_wnd)) return;
    EmptyClipboard();
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
    if (g) {
        std::memcpy(GlobalLock(g), s.c_str(), (s.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(g);
        SetClipboardData(CF_UNICODETEXT, g);
    }
    CloseClipboard();
}
// A pasted route: "L4: d12e1..." (the part after the last ':'), or a full rbview route line ("L3 C0 G0 I0 ...").
static bool RouteFromText(std::wstring t, tas::Route& out, std::string& err) {
    while (!t.empty() && std::iswspace(t.back())) t.pop_back();
    while (!t.empty() && std::iswspace(t.front())) t.erase(t.begin());
    const std::string s = Narrow(t);
    if (s.size() > 1 && s[0] == 'L' && std::isdigit((unsigned char)s[1]) && s.find(' ') != std::string::npos) return tas::RouteFromLine(s, out, err);
    std::string body = s;
    const size_t c = body.rfind(':');
    if (c != std::string::npos) body = body.substr(c + 1);
    std::vector<uint8_t> in;
    if (!tas::ParseRLE(body, in, err)) return false;
    out = g_s.route();
    out.in = in;
    out.warps.clear();
    return true;
}

// ------------------------------------------------------------------ spike rule and window (GetDisplayConfig)
static const int kSpikeModes[4] = {1, 2, 0, -1};
static const wchar_t* kSpikeNames[4] = {L"Standardized (mathspikes 1)", L"Level math (mathspikes 2)",
                                        L"Flash shape test (mathspikes 0, TE)", L"Off (no spike deaths)"};
static const wchar_t* kWindowPresets[] = {L"550x400", L"640x480", L"800x600", L"829x603", L"935x925", L"1024x768", L"1280x720",
                                          L"1366x768", L"1600x900", L"1920x1080"};
static bool ParseWindowSize(const std::wstring& t, int& w, int& h) {
    int a = 0, b = 0;
    wchar_t x = 0;
    if (std::swscanf(t.c_str(), L"%d %lc %d", &a, &x, &b) != 3 || (x != L'x' && x != L'X' && x != L'*') || a < 1 || b < 1 || a > 16384 ||
        b > 16384)
        return false;
    w = a, h = b;
    return true;
}
static void SetDisplayControls() {
    const DisplayConfig& dc = GetDisplayConfig();
    int sel = 0;
    for (int i = 0; i < 4; ++i)
        if (kSpikeModes[i] == dc.mathSpikes) sel = i;
    SendMessageW(g_spikes, CB_SETCURSEL, (WPARAM)sel, 0);
    wchar_t b[32];
    std::swprintf(b, 32, L"%dx%d", dc.windowW, dc.windowH);
    if (GetFocus() != GetWindow(g_window, GW_CHILD)) SetText(g_window, b);
    SendMessageW(g_scale, CB_SETCURSEL, dc.showAll ? 0 : 1, 0);
    const bool flash = dc.mathSpikes == 0, free = !g_searchRunning;
    EnableWindow(g_spikes, free);
    EnableWindow(g_window, flash && free);
    EnableWindow(g_scale, flash && free);
}
static void ReadDisplayControls() {
    if (g_searchRunning) return;
    DisplayConfig& dc = GetDisplayConfig();
    const int sel = (int)SendMessageW(g_spikes, CB_GETCURSEL, 0, 0);
    dc.mathSpikes = kSpikeModes[sel >= 0 && sel < 4 ? sel : 0];
    int w, h;
    if (ParseWindowSize(GetText(g_window), w, h)) dc.windowW = w, dc.windowH = h;
    dc.showAll = SendMessageW(g_scale, CB_GETCURSEL, 0, 0) != 1;
}

// ------------------------------------------------------------------ settings
static void FillCheckpoints(int level, int select) {
    SendMessageW(g_cp, CB_RESETCONTENT, 0, 0);
    LevelTemplate t(level);
    const int n = t.CheckpointCount();
    for (int i = 0; i < n; ++i) {
        wchar_t b[32];
        std::swprintf(b, 32, i == 0 ? L"Start (0)" : L"Checkpoint %d", i);
        SendMessageW(g_cp, CB_ADDSTRING, 0, (LPARAM)b);
    }
    SendMessageW(g_cp, CB_SETCURSEL, (WPARAM)std::max(0, std::min(select, n - 1)), 0);
}

static void SaveSettings() {
    if (g_iniPath.empty()) return;
    const wchar_t* ini = g_iniPath.c_str();
    WritePrivateProfileStringW(L"route", L"line", Widen(tas::RouteToLine(g_s.route())).c_str(), ini);
    const DisplayConfig& dc = GetDisplayConfig();
    wchar_t d[64];
    std::swprintf(d, 64, L"%d", dc.mathSpikes);
    WritePrivateProfileStringW(L"display", L"mathspikes", d, ini);
    std::swprintf(d, 64, L"%dx%d", dc.windowW, dc.windowH);
    WritePrivateProfileStringW(L"display", L"window", d, ini);
    WritePrivateProfileStringW(L"display", L"scale", dc.showAll ? L"showAll" : L"noScale", ini);
    auto wi = [&](const wchar_t* k, int v) {
        wchar_t b[32];
        std::swprintf(b, 32, L"%d", v);
        WritePrivateProfileStringW(L"view", k, b, ini);
    };
    wi(L"hud", g_hud), wi(L"marks", g_marks), wi(L"polygons", g_polys), wi(L"follow", g_follow_on), wi(L"trail", g_trail_on);
    wi(L"decimals", g_decimals), wi(L"realtime", g_s.realtimeLoad), wi(L"stopatend", g_s.stopAtEnd);
    std::wstring rm;
    for (int k = 0; k < 256; ++k)
        if (g_remap[k]) rm += std::to_wstring(k) + L"=" + std::to_wstring(g_remap[k]) + L" ";
    WritePrivateProfileStringW(L"keys", L"remap", rm.c_str(), ini);
    g_s.SaveStatesFile(Narrow(g_statesPath));
}

// ------------------------------------------------------------------ route actions
static void FocusCanvas() { SetFocus(g_canvas); }

static void SyncSetupControls() {
    const tas::Route& r = g_s.route();
    if ((int)SendMessageW(g_level, CB_GETCURSEL, 0, 0) != r.level - 1) {
        SendMessageW(g_level, CB_SETCURSEL, (WPARAM)r.level - 1, 0);
        FillCheckpoints(r.level, r.checkpoint);
    } else if ((int)SendMessageW(g_cp, CB_GETCURSEL, 0, 0) != r.checkpoint) {
        SendMessageW(g_cp, CB_SETCURSEL, (WPARAM)r.checkpoint, 0);
    }
    SetCheck(g_gless, r.gless);
    SetCheck(g_invincible, r.invincible);
}

// Load a route from the controls (level / start / flags / text box): shown from frame 0, paused.
static bool LoadFromControls(bool showErrors) {
    std::string err;
    std::vector<uint8_t> in;
    if (!tas::ParseRLE(Narrow(GetText(g_inputs)), in, err)) {
        if (showErrors) MessageBoxW(g_wnd, Widen(err).c_str(), L"Route", MB_ICONWARNING);
        return false;
    }
    ReadDisplayControls();
    tas::Route r = g_s.route();
    const int level = (int)SendMessageW(g_level, CB_GETCURSEL, 0, 0) + 1;
    const int cp = std::max(0, (int)SendMessageW(g_cp, CB_GETCURSEL, 0, 0));
    if (in != r.in || level != r.level || cp != r.checkpoint) r.warps.clear();
    r.level = level, r.checkpoint = cp, r.in = in;
    r.gless = Checked(g_gless), r.invincible = Checked(g_invincible);
    if (r != g_s.route()) g_s.Backup();
    g_s.Load(r, 0);
    g_s.paused = true;
    g_userZoom = 1.0;
    g_follow_on = true;
    RefreshAll();
    return true;
}

// Re-simulate the current route with the current settings, keeping the frame.
static void Resimulate() {
    ReadDisplayControls();
    tas::Route r = g_s.route();
    r.gless = Checked(g_gless), r.invincible = Checked(g_invincible);
    const int p = g_s.pos;
    g_s.Load(r, p);
    RefreshAll();
}

static void ChangeLevel(int level) {  // Ctrl+A / Ctrl+D: a new level, empty route (the mod's SetLevel)
    if (level < 1) level = NUM_LEVELS;
    if (level > NUM_LEVELS) level = 1;
    g_s.Backup();
    tas::Route r = g_s.route();
    r.level = level, r.checkpoint = 0, r.in.clear(), r.warps.clear();
    g_s.Load(r, 0);
    g_s.brute = tas::BruteEnv();
    SyncSetupControls();
    RefreshAll();
}

// As the mod's state load (instant / real time), or as a viewer load (frame 0, paused).
static void LoadRouteObject(const tas::Route& r, bool asLoad) {
    if (asLoad) {
        g_s.LoadRoute(r);
    } else {
        g_s.Backup();
        g_s.Load(r, 0);
        g_s.paused = true;
    }
    SyncSetupControls();
    RefreshAll();
}

// ------------------------------------------------------------------ files
// Reads a stats log from the mod ("# inputs<TAB>RLE", "LEVEL <id> <checkpoint>", "#ENV ..."), an rbview route file
// (a route line "L3 C0 G0 I0 RLE ..."), or a text file whose first line is the input string. Display settings from
// an #ENV line are applied.
static bool ReadRouteFile(const wchar_t* file, tas::Route& out) {
    FILE* f = _wfopen(file, L"rb");
    if (!f) {
        MessageBoxW(g_wnd, L"Cannot open that file.", L"Open", MB_ICONWARNING);
        return false;
    }
    std::string text;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0 && text.size() < (8u << 20)) text.append(buf, n);
    fclose(f);
    std::string route;
    int level = -1, cp = 0;
    bool isLog = false, camMoved = false, dpNonzero = false, prevR = false;
    std::string lastCam;
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(p, e - p);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        p = e + 1;
        if (line.rfind("# inputs\t", 0) == 0) {
            route = line.substr(9);
            isLog = true;
        } else if (line.rfind("#ENV\t", 0) == 0) {
            std::vector<std::string> kv;
            size_t q = 5;
            while (q <= line.size()) {
                size_t t = line.find('\t', q);
                if (t == std::string::npos) t = line.size();
                kv.push_back(line.substr(q, t - q));
                q = t + 1;
            }
            DisplayConfig& dc = GetDisplayConfig();
            if (!g_searchRunning)
                for (size_t i = 0; i + 1 < kv.size(); i += 2) {
                    if (kv[i] == "mathspikes") {
                        const int ms = std::atoi(kv[i + 1].c_str());
                        if (ms >= 0 && ms <= 2) dc.mathSpikes = ms;
                    }
                    int ww, wh;
                    if (kv[i] == "window" && ParseWindowSize(Widen(kv[i + 1]), ww, wh)) dc.windowW = ww, dc.windowH = wh;
                    if (kv[i] == "scaleMode") dc.showAll = kv[i + 1] != "noScale";
                }
        } else if (line.rfind("LEVEL ", 0) == 0) {
            if (!prevR) {
                std::sscanf(line.c_str() + 6, "%d %d", &level, &cp);
                camMoved = dpNonzero = false;
                lastCam.clear();
            }
            prevR = false;
        } else if (line == "R") {
            prevR = true;
        } else if (isLog && !line.empty() && std::isdigit((unsigned char)line[0])) {
            std::vector<std::string> fl;
            size_t q = 0;
            while (q <= line.size()) {
                size_t t = line.find('\t', q);
                if (t == std::string::npos) t = line.size();
                fl.push_back(line.substr(q, t - q));
                q = t + 1;
            }
            if (fl.size() >= 29 && (fl.size() - 21) % 7 == 1) {  // v2 trailing block: isGless detection as rbsim verify
                const size_t c = fl.size() - 6;
                const std::string cam = fl[c] + fl[c + 1];
                if (!lastCam.empty() && cam != lastCam) camMoved = true;
                lastCam = cam;
                for (size_t k = c + 2; k < c + 4; ++k)
                    if (fl[k] != "0000000000000000" && fl[k] != "8000000000000000" && fl[k].rfind("7ff", 0) != 0) dpNonzero = true;
            }
        } else if (!isLog && route.empty() && !line.empty() && line[0] != '#') {
            std::string err;
            if (line.size() > 1 && line[0] == 'L' && std::isdigit((unsigned char)line[1]) && tas::RouteFromLine(line, out, err)) return true;
            route = line;
            break;
        }
    }
    std::string err;
    std::vector<uint8_t> in;
    if (!tas::ParseRLE(route, in, err)) {
        MessageBoxW(g_wnd, Widen(err).c_str(), L"Open", MB_ICONWARNING);
        return false;
    }
    out = g_s.route();
    out.in = in;
    out.warps.clear();
    if (level >= 1 && level <= NUM_LEVELS) out.level = level, out.checkpoint = cp;
    if (isLog) out.gless = camMoved && !dpNonzero, out.invincible = false;
    {
        LevelTemplate t(out.level);
        if (out.checkpoint >= t.CheckpointCount()) out.checkpoint = 0;
    }
    return true;
}

static void LoadRouteFile(const wchar_t* file) {
    tas::Route r;
    if (ReadRouteFile(file, r)) LoadRouteObject(r, false);
}

static bool PickFile(bool save, std::wstring& out) {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW of = {};
    of.lStructSize = sizeof of;
    of.hwndOwner = g_wnd;
    of.lpstrFilter = L"Routes and logs (*.txt;*.tsv)\0*.txt;*.tsv\0All files\0*.*\0";
    of.lpstrFile = file;
    of.nMaxFile = MAX_PATH;
    of.lpstrDefExt = L"txt";
    of.Flags = save ? OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST : OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!(save ? GetSaveFileNameW(&of) : GetOpenFileNameW(&of))) return false;
    out = file;
    return true;
}

static void SaveRouteFile() {
    std::wstring file;
    if (!PickFile(true, file)) return;
    FILE* f = _wfopen(file.c_str(), L"wb");
    if (!f) {
        MessageBoxW(g_wnd, L"Cannot write that file.", L"Save", MB_ICONWARNING);
        return;
    }
    const tas::Route& r = g_s.route();
    std::fprintf(f, "# rbview route: level %d, checkpoint %d%s%s. Input string: %s\n", r.level, r.checkpoint, r.gless ? ", glitchless" : "",
                 r.Tool() ? ", uses invincibility or warps (not a real-game route)" : "", r.in.empty() ? "-" : EncodeInputsRLE(r.in).c_str());
    std::fprintf(f, "%s\n", tas::RouteToLine(r).c_str());
    fclose(f);
}

// ------------------------------------------------------------------ small modal windows (input box, key settings)
static LRESULT CALLBACK ModalProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_CLOSE) {
        SetWindowLongPtrW(h, GWLP_USERDATA, 2);
        return 0;
    }
    if (m == WM_COMMAND && HIWORD(wp) == BN_CLICKED) {
        if (LOWORD(wp) == IDOK) SetWindowLongPtrW(h, GWLP_USERDATA, 1);
        else if (LOWORD(wp) == IDCANCEL) SetWindowLongPtrW(h, GWLP_USERDATA, 2);
        else SetWindowLongPtrW(h, GWLP_USERDATA, 100 + LOWORD(wp));
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}
static HWND ModalChild(HWND p, const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int hh, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(hh), p, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}
static HWND ModalWindow(const wchar_t* title, int w, int h) {
    RECT rc;
    GetWindowRect(g_wnd, &rc);
    RECT want = {0, 0, S(w), S(h)};
    AdjustWindowRect(&want, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE);
    const int ww = want.right - want.left, wh = want.bottom - want.top;
    return CreateWindowExW(WS_EX_DLGMODALFRAME, L"RbModal", title, WS_POPUP | WS_CAPTION | WS_SYSMENU, (rc.left + rc.right - ww) / 2,
                           (rc.top + rc.bottom - wh) / 2, ww, wh, g_wnd, nullptr, GetModuleHandleW(nullptr), nullptr);
}
// Runs the window until a button sets its result: 1 (OK / Enter), 2 (cancel / Esc) or 100 + control id.
static int ModalLoop(HWND d, HWND focus) {
    g_modal = true;
    EnableWindow(g_wnd, FALSE);
    ShowWindow(d, SW_SHOW);
    if (focus) SetFocus(focus);
    SetWindowLongPtrW(d, GWLP_USERDATA, 0);
    MSG msg;
    int res = 0;
    while ((res = (int)GetWindowLongPtrW(d, GWLP_USERDATA)) == 0 && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if ((msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) || (msg.message == WM_CHAR && msg.wParam == L'\r')) {
            SetWindowLongPtrW(d, GWLP_USERDATA, 1);
            continue;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            SetWindowLongPtrW(d, GWLP_USERDATA, 2);
            continue;
        }
        if (!IsDialogMessageW(d, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return res;
}
static void EndModal(HWND d) {
    EnableWindow(g_wnd, TRUE);
    DestroyWindow(d);
    SetForegroundWindow(g_wnd);
    RedrawWindow(g_wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    g_modal = false;
    FocusCanvas();
}

static bool InputBox(const wchar_t* title, const wchar_t* prompt, std::wstring& value) {
    HWND d = ModalWindow(title, 380, 120);
    ModalChild(d, L"STATIC", prompt, 0, 0, 12, 10, 356, 20);
    HWND e = ModalChild(d, L"EDIT", value.c_str(), ES_AUTOHSCROLL | WS_TABSTOP, 10, 12, 34, 356, 24, WS_EX_CLIENTEDGE);
    ModalChild(d, L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK, 196, 78, 80, 28);
    ModalChild(d, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL, 286, 78, 80, 28);
    SendMessageW(e, EM_SETSEL, 0, -1);
    int r;
    while ((r = ModalLoop(d, e)) > 2) {
    }
    if (r == 1) value = GetText(e);
    EndModal(d);
    return r == 1;
}

static const wchar_t* kKeyHelp =
    L"Practice Hack keys (rbview uses the same ones):\r\n"
    L"A/Left  W/Up  D/Right   move (a key tapped between two frames counts too)\r\n"
    L"T or P   pause / run          Space   one frame (while running: pause)\r\n"
    L", and .   game speed down / up (0, 1/32 ... 64)\r\n"
    L"0-9   save state          Shift+0-9   load state\r\n"
    L"I then 0-9   import the clipboard into a state (Shift+0-9: and play it)\r\n"
    L"O then 0-9   copy a state to the clipboard          Shift+O   backups\r\n"
    L"Backspace   remove 10 frames (Shift: 31)          Shift+Space   remove 1 frame\r\n"
    L"Esc   drop the rest of a playback     R   restart the level     F   checkpoint restart\r\n"
    L"U   real-time state loads          Shift+U   fast playback\r\n"
    L"G   invincibility     Shift+G   glitchless     C   clear the message\r\n"
    L"H   HUD     Shift+H   graphics marks (spike hitboxes, jump probes)     J   physics outlines + control points\r\n"
    L"B   jump take-off     Shift+B   landing: the jump brute-forcer (W/S, Space, Enter, Esc)\r\n"
    L"Ctrl+A / Ctrl+D (Ctrl+Left/Right)   previous / next level\r\n"
    L"/   command line: /config mathspikes N, /config decimals N, /set -vx -vy -vr -px -py, /remap A LEFT, /remap reset\r\n"
    L"Z / Shift+Z   zoom in / out at the mouse          =   reset the zoom\r\n"
    L"\r\n"
    L"rbview only: Home/End first/last frame, PageUp/PageDown one frame back/on (Shift: 10) without changing the route,\r\n"
    L"F1 this help. Keys do nothing while you type in a text box (route, command line, ...): click the level view first.";

static void KeysDialog() {
    HWND d = ModalWindow(L"Keys", 700, 480);
    ModalChild(d, L"EDIT", kKeyHelp, ES_MULTILINE | ES_READONLY | WS_VSCROLL, 0, 10, 10, 680, 260, WS_EX_CLIENTEDGE);
    ModalChild(d, L"STATIC", L"Remapped keys (the mod's /remap: pressing FROM acts as TO):", 0, 0, 10, 278, 600, 20);
    HWND list = ModalChild(d, L"LISTBOX", L"", WS_VSCROLL | LBS_NOINTEGRALHEIGHT, 50, 10, 300, 300, 130, WS_EX_CLIENTEDGE);
    ModalChild(d, L"STATIC", L"From", 0, 0, 324, 304, 50, 20);
    HWND from = ModalChild(d, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 51, 380, 300, 120, 24, WS_EX_CLIENTEDGE);
    ModalChild(d, L"STATIC", L"To", 0, 0, 324, 334, 50, 20);
    HWND to = ModalChild(d, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 52, 380, 330, 120, 24, WS_EX_CLIENTEDGE);
    ModalChild(d, L"BUTTON", L"Add", BS_PUSHBUTTON | WS_TABSTOP, 53, 510, 300, 120, 28);
    ModalChild(d, L"BUTTON", L"Remove selected", BS_PUSHBUTTON | WS_TABSTOP, 54, 510, 334, 120, 28);
    ModalChild(d, L"BUTTON", L"Reset all", BS_PUSHBUTTON | WS_TABSTOP, 55, 510, 368, 120, 28);
    ModalChild(d, L"STATIC", L"Key names: A-Z, 0-9, LEFT, UP, SPACE, SHIFT, COMMA, PERIOD, ... or a key code.", 0, 0, 324, 366, 180, 60);
    ModalChild(d, L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL, 600, 440, 90, 30);
    auto fill = [&]() {
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        for (int k = 0; k < 256; ++k)
            if (g_remap[k]) {
                const std::wstring s = Widen(tas::VkName(k)) + L"  ->  " + Widen(tas::VkName(g_remap[k]));
                const int i = (int)SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)s.c_str());
                SendMessageW(list, LB_SETITEMDATA, (WPARAM)i, k);
            }
    };
    fill();
    for (;;) {
        const int r = ModalLoop(d, nullptr);
        if (r == 2) break;
        if (r == 100 + 53 || r == 1) {  // Add (Enter adds too)
            if (!GetWindowTextLengthW(from) && !GetWindowTextLengthW(to)) {
                if (r == 1) break;
                continue;
            }
            const int a = tas::KeyNameToVk(Narrow(GetText(from))), b = tas::KeyNameToVk(Narrow(GetText(to)));
            if (a > 0 && a < 256 && b > 0 && b < 256) {
                g_remap[a] = b;
                SetWindowTextW(from, L"");
                SetWindowTextW(to, L"");
                fill();
            } else {
                MessageBoxW(d, L"Unknown key name.", L"Keys", MB_ICONWARNING);
            }
        } else if (r == 100 + 54) {
            const int i = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
            if (i >= 0) g_remap[(int)SendMessageW(list, LB_GETITEMDATA, (WPARAM)i, 0)] = 0;
            fill();
        } else if (r == 100 + 55) {
            std::fill(g_remap, g_remap + 256, 0);
            fill();
        }
    }
    EndModal(d);
    SaveSettings();
}

// ------------------------------------------------------------------ drawing
static gp::Color Rgb(int r, int g, int b, int a = 255) { return gp::Color((BYTE)a, (BYTE)r, (BYTE)g, (BYTE)b); }

static bool IsKillBody(int level, const std::string& n) {
    if (n.find("kill") != std::string::npos || n.find("axe") == 0 || n == "roundBlock" || n == "boomCrank" || n == "kingStar1") return true;
    if (level == 7 && n == "star") return true;
    if (level == 8 && n.rfind("killer", 0) == 0) return true;
    return false;
}

struct View {
    double z, cx, cy;
    int w, h;
    gp::PointF P(double x, double y) const { return gp::PointF((gp::REAL)((x - cx) * z + w / 2.0), (gp::REAL)((y - cy) * z + h / 2.0)); }
};

static void DrawBox(gp::Graphics& g, const View& v, const DisplayObj& o, const gp::Color& line, const gp::Color* fill, float width) {
    const gp::PointF a = v.P(o.x0 / 20.0, o.y0 / 20.0), b = v.P(o.x1 / 20.0, o.y1 / 20.0);
    const gp::RectF r(a.X, a.Y, b.X - a.X, b.Y - a.Y);
    if (fill) {
        gp::SolidBrush br(*fill);
        g.FillRectangle(&br, r);
    }
    gp::Pen pen(line, width);
    g.DrawRectangle(&pen, r);
}

static void DrawLabel(gp::Graphics& g, const gp::PointF& p, const wchar_t* text, const gp::Color& c, float size, bool bold = true) {
    gp::FontFamily ff(L"Segoe UI");
    gp::Font font(ff.IsAvailable() ? &ff : gp::FontFamily::GenericSansSerif(), size, bold ? gp::FontStyleBold : gp::FontStyleRegular, gp::UnitPixel);
    gp::SolidBrush br(c);
    g.DrawString(text, -1, &font, p, &br);
}

static void DrawBall(gp::Graphics& g, const gp::PointF& c, float r, double angDeg, const gp::Color& fill, const gp::Color& edge, float lw) {
    gp::SolidBrush br(fill);
    gp::Pen pen(edge, lw);
    g.FillEllipse(&br, c.X - r, c.Y - r, 2 * r, 2 * r);
    g.DrawEllipse(&pen, c.X - r, c.Y - r, 2 * r, 2 * r);
    const double ang = angDeg * AS3_PI / 180;
    gp::SolidBrush eye(Rgb(255, 255, 255, fill.GetA()));
    const float er = r * 0.28f;
    const gp::PointF e(c.X + (gp::REAL)(std::cos(ang) * r * 0.45), c.Y + (gp::REAL)(std::sin(ang) * r * 0.45));
    g.FillEllipse(&eye, e.X - er, e.Y - er, 2 * er, 2 * er);
}

static std::wstring Fixed(double v) {
    wchar_t b[64];
    std::swprintf(b, 64, L"%.*f", g_decimals, v);
    return b;
}

static void Paint(HDC hdc, int w, int h) {
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    {
        gp::Graphics g(mem);
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(gp::TextRenderingHintAntiAliasGridFit);
        gp::LinearGradientBrush sky(gp::Point(0, 0), gp::Point(0, h), Rgb(170, 214, 245), Rgb(232, 244, 252));
        g.FillRectangle(&sky, 0, 0, w, h);
        if (g_s.cur) {
            const Sim& s = *g_s.cur;
            const World& wd = s.world;
            const int level = g_s.route().level;
            View v;
            v.w = w, v.h = h;
            v.z = std::min(w / 550.0, h / 400.0) * g_userZoom;
            if (g_follow_on) {
                v.cx = -s.camX + 275, v.cy = -s.camY + 200;
                g_viewX = v.cx, g_viewY = v.cy;
            } else {
                v.cx = g_viewX, v.cy = g_viewY;
            }
            const float lw = (float)std::max(1.0, v.z);
            // bodies (not the ball)
            for (int32_t b = 0; b < wd.numBodies; ++b) {
                const Body& bd = wd.bodies[b];
                if (!bd.inWorld || b == s.playerBody) continue;
                gp::Color fill = bd.IsStatic() ? Rgb(64, 78, 96) : Rgb(96, 140, 190);
                if (IsKillBody(level, s.BodyName(b))) fill = Rgb(176, 40, 40);
                gp::SolidBrush br(fill);
                gp::Pen pen(Rgb(25, 30, 40, 200), lw * 0.8f);
                for (int32_t sh = bd.shapeList; sh != -1; sh = wd.shapes[sh].next) {
                    const Geom& ge = s.tpl->geoms[wd.shapes[sh].geom];
                    if (ge.type == e_circleShape) {
                        const Vec2 c = b2MulX(bd.xf, ge.localPosition);
                        const gp::PointF pc = v.P(c.x * PHYS_SCALE, c.y * PHYS_SCALE);
                        const float r = (float)(ge.radius * PHYS_SCALE * v.z);
                        g.FillEllipse(&br, pc.X - r, pc.Y - r, 2 * r, 2 * r);
                        g.DrawEllipse(&pen, pc.X - r, pc.Y - r, 2 * r, 2 * r);
                        const Vec2 e = b2MulX(bd.xf, Vec2(ge.localPosition.x + ge.radius, ge.localPosition.y));
                        gp::Pen sp(Rgb(255, 255, 255, 140), lw);
                        g.DrawLine(&sp, pc, v.P(e.x * PHYS_SCALE, e.y * PHYS_SCALE));
                    } else {
                        gp::PointF pts[MAX_POLY_VERTS];
                        for (int32_t k = 0; k < ge.vertexCount; ++k) {
                            const Vec2 p = b2MulX(bd.xf, ge.vertices[k]);
                            pts[k] = v.P(p.x * PHYS_SCALE, p.y * PHYS_SCALE);
                        }
                        g.FillPolygon(&br, pts, ge.vertexCount);
                        g.DrawPolygon(&pen, pts, ge.vertexCount);
                    }
                }
            }
            // spikes, and (Shift+H) where each one really kills this frame
            const int ms = GetDisplayConfig().mathSpikes;
            for (int32_t i = 0; i < s.tpl->spikeCount; ++i) {
                const SpikeObj& sp = s.tpl->spikes[i];
                const double lx[3] = {0, 3, 6}, ly[3] = {0, -9.65, 0};
                gp::PointF pts[3];
                for (int k = 0; k < 3; ++k) pts[k] = v.P(sp.a * lx[k] + sp.c * ly[k] + sp.tx, sp.b * lx[k] + sp.d * ly[k] + sp.ty);
                gp::SolidBrush br(g_marks && ms >= 0 ? Rgb(200, 30, 30, 90) : Rgb(200, 30, 30));
                g.FillPolygon(&br, pts, 3);
                if (g_marks && ms >= 0) {
                    // The effective hitbox of this frame: where a control point of the ball kills. Standardized / math:
                    // the point is shifted by -dp (the "cover"), i.e. the triangle by +dp, and must also lie inside the
                    // spike's box. Flash's shape test: the outline of the last redraw, inside the current box (padded by
                    // 2 subpixels right and bottom). Drawn as one shape: the shifted outline cut to the box.
                    if (ms == 0) {
                        // Flash's shape test is done on whole window pixels: draw every window pixel that kills, as the
                        // area of control-point positions that probe it (exact, rbsim self-test 24).
                        const gp::PointF a0 = v.P(sp.bx0 / 20.0 - 12, sp.by0 / 20.0 - 12), a1 = v.P(sp.bx1 / 20.0 + 12, sp.by1 / 20.0 + 12);
                        if (a1.X < 0 || a0.X > w || a1.Y < 0 || a0.Y > h) continue;
                        std::vector<KillPixel> kp;
                        SpikeKillPixels(sp, s.camX, s.camY, s.spDrawn[i] ? s.spDrawnX[i] / 20.0 : NAN, s.spDrawn[i] ? s.spDrawnY[i] / 20.0 : NAN, kp);
                        gp::SolidBrush cb(Rgb(255, 120, 0, 220));
                        gp::Pen grid(Rgb(150, 50, 0, 160), 1);
                        for (const KillPixel& p : kp) {
                            const gp::PointF q0 = v.P(p.x0, p.y0), q1 = v.P(p.x1, p.y1);
                            g.FillRectangle(&cb, q0.X, q0.Y, q1.X - q0.X, q1.Y - q0.Y);
                            if (q1.X - q0.X >= 6) g.DrawRectangle(&grid, q0.X, q0.Y, q1.X - q0.X, q1.Y - q0.Y);  // one window pixel
                        }
                        continue;
                    }
                    const double ox = s.dpX, oy = s.dpY, padX = 0, padY = 0;
                    gp::PointF cov[3];
                    for (int k = 0; k < 3; ++k) cov[k] = gp::PointF(pts[k].X + (gp::REAL)(ox * v.z), pts[k].Y + (gp::REAL)(oy * v.z));
                    const gp::PointF a = v.P(sp.bx0 / 20.0, sp.by0 / 20.0), b = v.P(sp.bx1 / 20.0 + padX, sp.by1 / 20.0 + padY);
                    gp::GraphicsPath tri;
                    tri.AddPolygon(cov, 3);
                    gp::GraphicsPath box;
                    box.AddRectangle(gp::RectF(a.X, a.Y, b.X - a.X, b.Y - a.Y));
                    gp::Region hit(&tri);
                    hit.Intersect(&box);
                    gp::SolidBrush cb(Rgb(255, 120, 0, 220));
                    g.FillRegion(&cb, &hit);
                    // outline of the hitbox (drawn clipped to it, so only its own edges show)
                    gp::Region oldClip;
                    g.GetClip(&oldClip);
                    g.SetClip(&box, gp::CombineModeIntersect);
                    gp::Pen op(Rgb(150, 50, 0), std::max(1.0f, lw * 0.8f));
                    g.DrawPolygon(&op, cov, 3);
                    g.SetClip(&oldClip);
                    g.SetClip(&tri, gp::CombineModeIntersect);
                    g.DrawRectangle(&op, a.X, a.Y, b.X - a.X, b.Y - a.Y);
                    g.SetClip(&oldClip);
                }
            }
            // checkpoints
            for (int i = 0; i < 5; ++i) {
                if (!s.tpl->cps[i]) continue;
                const gp::Color fc = Rgb(40, 200, 220, 110);
                DrawBox(g, v, *s.tpl->cps[i], Rgb(20, 140, 160), s.cpFrame[i] != 1 ? &fc : nullptr, lw * 1.5f);
                wchar_t b[8];
                std::swprintf(b, 8, L"%d", i);
                DrawLabel(g, v.P(s.tpl->cps[i]->x0 / 20.0, s.tpl->cps[i]->y0 / 20.0 - 14), b, Rgb(20, 120, 140), (float)std::max(10.0, 11 * v.z));
            }
            // switches and triggers
            for (const LevelDisplay& ld : kDisplayTable) {
                if (ld.id != level) continue;
                for (int32_t i = 0; i < ld.count; ++i) {
                    const DisplayObj& o = ld.items[i];
                    const std::string n = o.name;
                    gp::Color c;
                    int idx = -1;
                    if (n.rfind("blueCheck", 0) == 0) c = Rgb(40, 90, 230);
                    else if (n.rfind("greenCheck", 0) == 0) c = Rgb(30, 170, 60);
                    else if (n.rfind("redCheck", 0) == 0) c = Rgb(220, 40, 40);
                    else if (n == "wrongWay") c = Rgb(240, 140, 20);
                    else continue;
                    if (level == 9 || level == 16) idx = n.back() - '1';
                    else if (level == 5) idx = n == "blueCheck" ? 0 : 1;
                    else if (level == 7) idx = n == "redCheck" ? 0 : 1;
                    else if (level == 13 || level == 14 || level == 15) idx = 0;
                    const bool pressed = idx >= 0 && idx < 4 && s.switchFrame[idx] != 1;
                    const gp::Color fc(120, c.GetR(), c.GetG(), c.GetB());
                    DrawBox(g, v, o, c, pressed ? &fc : nullptr, lw * 1.5f);
                    if (n == "wrongWay") DrawLabel(g, v.P(o.x0 / 20.0, o.y0 / 20.0 - 14), L"wrong way", c, (float)std::max(10.0, 11 * v.z));
                }
            }
            if (level == 15) {
                if (const DisplayObj* kl = s.tpl->Display("killLine")) {
                    DisplayObj o = *kl;
                    const double dx = s.lvInt[1] - s.tpl->Place("killLine").tx;
                    o.x0 += dx, o.x1 += dx;
                    const gp::Color fc = Rgb(200, 30, 30, 160);
                    DrawBox(g, v, o, Rgb(160, 20, 20), &fc, lw);
                }
            }
            if (s.tpl->aim) {
                const DisplayObj goal = const_cast<Sim&>(s).GoalTarget();
                const gp::Color fc = Rgb(255, 210, 40, s.isTimeStop ? 200 : 70);
                DrawBox(g, v, goal, Rgb(200, 150, 0), &fc, lw * 2);
                DrawLabel(g, v.P(goal.x0 / 20.0, goal.y0 / 20.0 - 16), L"FLAG", Rgb(160, 110, 0), (float)std::max(11.0, 12 * v.z));
            }
            // path: played part, and the rest of the route (still to play back) fainter
            const std::vector<tas::FrameInfo>& info = g_s.tl.info();
            const std::vector<uint8_t>& in = g_s.route().in;
            if (g_trail_on && !info.empty()) {
                std::vector<gp::PointF> pts;
                gp::Pen pen(Rgb(220, 30, 30, 120), (float)std::max(1.0, 1.5 * v.z));
                for (int i = 0; i <= g_s.pos && i < (int)info.size(); ++i) {
                    if (i > 0 && in[(size_t)i - 1] == IN_RESTART) {
                        if (pts.size() > 1) g.DrawLines(&pen, pts.data(), (INT)pts.size());
                        pts.clear();
                    }
                    pts.push_back(v.P(info[(size_t)i].bx, info[(size_t)i].by));
                }
                if (pts.size() > 1) g.DrawLines(&pen, pts.data(), (INT)pts.size());
                if (g_s.pos < (int)in.size()) {
                    std::vector<gp::PointF> rest;
                    gp::Pen pr(Rgb(220, 30, 30, 50), (float)std::max(1.0, 1.2 * v.z));
                    for (int i = g_s.pos; i < (int)info.size(); ++i) {
                        if (i > g_s.pos && in[(size_t)i - 1] == IN_RESTART) break;
                        rest.push_back(v.P(info[(size_t)i].bx, info[(size_t)i].by));
                    }
                    if (rest.size() > 1) g.DrawLines(&pr, rest.data(), (INT)rest.size());
                }
            }
            // physics outlines (J): every shape as the engine sees it, the ball's 16 control points
            if (g_polys) {
                gp::Pen pen(Rgb(0, 200, 80, 220), 1);
                for (int32_t b = 0; b < wd.numBodies; ++b) {
                    const Body& bd = wd.bodies[b];
                    if (!bd.inWorld) continue;
                    for (int32_t sh = bd.shapeList; sh != -1; sh = wd.shapes[sh].next) {
                        const Geom& ge = s.tpl->geoms[wd.shapes[sh].geom];
                        if (ge.type == e_circleShape) {
                            const Vec2 c = b2MulX(bd.xf, ge.localPosition);
                            const gp::PointF pc = v.P(c.x * PHYS_SCALE, c.y * PHYS_SCALE);
                            const float r = (float)(ge.radius * PHYS_SCALE * v.z);
                            g.DrawEllipse(&pen, pc.X - r, pc.Y - r, 2 * r, 2 * r);
                        } else {
                            gp::PointF pts[MAX_POLY_VERTS];
                            for (int32_t k = 0; k < ge.vertexCount; ++k) {
                                const Vec2 p = b2MulX(bd.xf, ge.vertices[k]);
                                pts[k] = v.P(p.x * PHYS_SCALE, p.y * PHYS_SCALE);
                            }
                            g.DrawPolygon(&pen, pts, ge.vertexCount);
                        }
                    }
                }
                if (s.playerAlive) {
                    gp::SolidBrush cp(Rgb(255, 0, 255));
                    const double bx = s.spriteX[s.playerBody], by = s.spriteY[s.playerBody], rot = s.spriteRotW[s.playerBody] * AS3_PI / 180;
                    const double r = PLAYER_SPRITE_WIDTH_TWIPS / 20.0 / 2;
                    for (int k = 0; k < 16; ++k) {
                        const double a = 2 * AS3_PI / 16 * k + rot;
                        const gp::PointF p = v.P(bx + r * std::cos(a), by + r * std::sin(a));
                        g.FillRectangle(&cp, p.X - 1.5f, p.Y - 1.5f, 3.0f, 3.0f);
                    }
                }
            }
            // ghost route
            if (g_ghostOn && g_ghost.route().level == level && !g_ghost.info().empty()) {
                const tas::FrameInfo& gf = g_ghost.info()[(size_t)std::min(g_s.pos, (int)g_ghost.info().size() - 1)];
                const gp::PointF c = v.P(gf.bx, gf.by);
                const float r = (float)(10.5 * v.z);
                DrawBall(g, c, r, gf.brot, Rgb(40, 110, 240, gf.alive ? 110 : 50), Rgb(20, 60, 160, 160), lw);
                DrawLabel(g, gp::PointF(c.X + r + 2, c.Y - r - 4), L"ghost", Rgb(20, 60, 160, 200), (float)std::max(10.0, 10 * v.z));
            }
            // brute-force target height
            if (g_s.brute.active) {
                const gp::PointF a = v.P(0, g_s.brute.TargetY() * PHYS_SCALE);
                gp::Pen pen(Rgb(120, 0, 200), 1.5f);
                pen.SetDashStyle(gp::DashStyleDash);
                g.DrawLine(&pen, gp::PointF(0, a.Y), gp::PointF((gp::REAL)w, a.Y));
                DrawLabel(g, gp::PointF(8, a.Y - 18), L"brute force: target height", Rgb(120, 0, 200), 12);
            }
            // the ball (its sprite)
            {
                const double bx = s.spriteX[s.playerBody], by = s.spriteY[s.playerBody];
                const gp::PointF c = v.P(bx, by);
                const float r = (float)(10.5 * v.z);
                if (s.playerAlive) {
                    DrawBall(g, c, r, s.spriteRotW[s.playerBody], Rgb(230, 30, 30), Rgb(110, 0, 0), lw * 1.2f);
                    if (g_marks) {
                        // the mod's ball box mark
                        const double a = s.spriteRotW[s.playerBody] * AS3_PI / 180, half = 10.5 * (std::fabs(std::cos(a)) + std::fabs(std::sin(a)));
                        gp::SolidBrush bb(Rgb(32, 0, 192, 60));
                        g.FillRectangle(&bb, c.X - (gp::REAL)(half * v.z), c.Y - (gp::REAL)(half * v.z), (gp::REAL)(2 * half * v.z),
                                        (gp::REAL)(2 * half * v.z));
                        // the 16 control points (what the spike hitboxes kill)
                        // (whole twips, exactly as the spike test computes them)
                        gp::SolidBrush cpb(Rgb(255, 0, 255));
                        double cp[32];
                        BallControlPoints(bx, by, s.spriteRotW[s.playerBody], GetDisplayConfig().mathSpikes, s.camX, s.camY, cp);
                        const float cr = std::min(4.0f, std::max(1.5f, (float)(0.6 * v.z)));
                        for (int k = 0; k < 16; ++k) {
                            const gp::PointF p = v.P(cp[2 * k], cp[2 * k + 1]);
                            g.FillRectangle(&cpb, p.X - cr, p.Y - cr, 2 * cr, 2 * cr);
                        }
                        // jump probes: the three ground tests of the last Update (Level.Update, at the ball's position after
                        // the step = the shown position). Any hit = grounded: up jumps, left/right accelerate fully.
                        const Body& pb = wd.bodies[s.playerBody];
                        const double px = pb.xf.position.x, py = pb.xf.position.y;
                        const double qx[3] = {px - 0.2, px, px + 0.2}, qy[3] = {py + 0.38, py + 0.4, py + 0.38};
                        const bool hitp[3] = {s.probeL, s.probeC, s.probeR};
                        const float pr = std::min(7.0f, std::max(2.5f, (float)(2.2 * v.z)));
                        for (int k = 0; k < 3; ++k) {
                            const gp::PointF p = v.P(qx[k] * PHYS_SCALE, qy[k] * PHYS_SCALE);
                            gp::SolidBrush pbr(hitp[k] ? Rgb(0, 200, 60) : Rgb(255, 255, 255));
                            gp::Pen pp(Rgb(0, 0, 0), 1);
                            g.FillEllipse(&pbr, p.X - pr, p.Y - pr, 2 * pr, 2 * pr);
                            g.DrawEllipse(&pp, p.X - pr, p.Y - pr, 2 * pr, 2 * pr);
                        }
                    }
                } else {
                    // after a death the frozen ball is tested against targets shifted by the camera: the death-warp ghost
                    const gp::PointF gc = v.P(bx - s.camX, by - s.camY);
                    gp::Pen gpen(Rgb(140, 60, 200), lw * 1.5f);
                    gpen.SetDashStyle(gp::DashStyleDash);
                    g.DrawEllipse(&gpen, gc.X - r, gc.Y - r, 2 * r, 2 * r);
                    DrawLabel(g, gp::PointF(gc.X + r + 3, gc.Y - r), L"warp ghost", Rgb(140, 60, 200), (float)std::max(10.0, 10 * v.z));
                    gp::Pen pen(Rgb(90, 90, 90), lw * 1.5f);
                    g.DrawEllipse(&pen, c.X - r, c.Y - r, 2 * r, 2 * r);
                    gp::Pen x(Rgb(200, 0, 0), lw * 2);
                    g.DrawLine(&x, c.X - r * 0.7f, c.Y - r * 0.7f, c.X + r * 0.7f, c.Y + r * 0.7f);
                    g.DrawLine(&x, c.X - r * 0.7f, c.Y + r * 0.7f, c.X + r * 0.7f, c.Y - r * 0.7f);
                }
            }
            if (g_follow_on && g_userZoom < 0.999) {
                gp::Pen pen(Rgb(0, 0, 0, 90), 1);
                const gp::PointF a = v.P(v.cx - 275, v.cy - 200), b = v.P(v.cx + 275, v.cy + 200);
                g.DrawRectangle(&pen, a.X, a.Y, b.X - a.X, b.Y - a.Y);
            }
            // HUD (H): the mod's texts
            if (g_hud) {
                const Body& b = wd.bodies[s.playerBody];
                const int shownFrame = s.isTimeStop ? s.winFrame : s.frameCount;
                std::vector<std::wstring> lines;
                wchar_t t[256];
                std::swprintf(t, 256, L"Time: %.3fs   (frame %d)", shownFrame / 31.0, shownFrame);
                lines.push_back(t);
                std::swprintf(t, 256, L"GameSpeed: %.3fx", g_s.paused ? 0.0 : g_s.speed);
                lines.push_back(t);
                lines.push_back(L"Speed: " + Fixed(std::sqrt(b.linearVelocity.x * b.linearVelocity.x + b.linearVelocity.y * b.linearVelocity.y)));
                lines.push_back(L"Vx: " + Fixed(b.linearVelocity.x) + L"     x: " + Fixed(b.xf.position.x));
                lines.push_back(L"Vy: " + Fixed(b.linearVelocity.y) + L"     y: " + Fixed(b.xf.position.y));
                lines.push_back(L"Vrot: " + Fixed(10.5 / 30 * b.angularVelocity) + L"     angle: " + Fixed(std::fmod(b.sweep.a * 180 / AS3_PI, 360)));
                std::swprintf(t, 256, L"Contact Count: %d entities", s.world.listener.count);
                lines.push_back(t);
                std::wstring cl = L"Contact List: ";
                for (int32_t i = 0; i < s.world.listener.count; ++i) cl += (i ? L", " : L"") + Widen(s.BodyName(s.world.listener.bodies[i]));
                lines.push_back(cl);
                // distance to the flag: the gap between the ball's box and the flag's box (hitTestObject wins at 0)
                if (s.tpl->aim && s.playerAlive) {
                    const DisplayObj goal = const_cast<Sim&>(s).GoalTarget();
                    const Rect bb = BallBounds(s.spriteX[s.playerBody], s.spriteY[s.playerBody], s.spriteRotW[s.playerBody]);
                    const double gx = std::max({0.0, goal.x0 - bb.x1, bb.x0 - goal.x1}) / 20 / PHYS_SCALE;
                    const double gy = std::max({0.0, goal.y0 - bb.y1, bb.y0 - goal.y1}) / 20 / PHYS_SCALE;
                    lines.push_back(L"To goal: " + Fixed(std::sqrt(gx * gx + gy * gy)) + L" m     (dx " + Fixed(gx) + L", dy " + Fixed(gy) + L")");
                }
                std::wstring flags;
                if (g_s.route().invincible) flags += L"INVINCIBLE  ";
                if (!g_s.route().warps.empty()) flags += L"WARPED  ";
                if (g_s.route().gless) flags += L"GLITCHLESS  ";
                // the HUD grows with the canvas (1x up to about 800x580, then with the smaller side)
                const float hs = (float)std::max(1.0, std::min(3.0, std::min(w / 800.0, h / 580.0)));
                const float fs = 13 * hs, lh = 17 * hs;
                gp::FontFamily ff(L"Segoe UI");
                gp::Font mf(ff.IsAvailable() ? &ff : gp::FontFamily::GenericSansSerif(), fs, gp::FontStyleRegular, gp::UnitPixel);
                float tw = 0;
                for (const std::wstring& l : lines) {
                    gp::RectF r;
                    g.MeasureString(l.c_str(), -1, &mf, gp::PointF(0, 0), &r);
                    tw = std::max(tw, r.Width);
                }
                gp::SolidBrush back(Rgb(255, 255, 255, 150));
                g.FillRectangle(&back, 4.0f, 4.0f, std::max(380 * hs, tw + 12), lines.size() * lh + 8 + (flags.empty() ? 0 : lh + 1));
                for (size_t i = 0; i < lines.size(); ++i) DrawLabel(g, gp::PointF(8, 6 + lh * i), lines[i].c_str(), Rgb(20, 20, 30), fs, false);
                if (!flags.empty()) DrawLabel(g, gp::PointF(8, 6 + lh * lines.size()), flags.c_str(), Rgb(190, 0, 120), fs);
                // A W D (filled = pressed this frame)
                const uint8_t code = g_s.InPlayback() ? g_s.route().in[(size_t)g_s.pos] : tas::KeysToCode(g_heldL || g_tapL, g_heldU || g_tapU, g_heldR || g_tapR);
                const bool k[3] = {(code & 4) != 0 && code != IN_RESTART, (code & 2) != 0 && code != IN_RESTART, (code & 1) != 0 && code != IN_RESTART};
                const wchar_t* nm[3] = {L"A", L"W", L"D"};
                const float kw = 26 * hs, kh = 24 * hs, gap = 30 * hs;
                const float kx[3] = {(float)w - 14 - 2 * gap - kw, (float)w - 14 - gap - kw, (float)w - 14 - kw}, ky[3] = {8 + 28 * hs, 8, 8 + 28 * hs};
                for (int i = 0; i < 3; ++i) {
                    gp::SolidBrush kb(k[i] ? Rgb(230, 30, 30, 220) : Rgb(255, 255, 255, 150));
                    g.FillRectangle(&kb, kx[i], ky[i], kw, kh);
                    gp::Pen kp(Rgb(40, 40, 40), 1);
                    g.DrawRectangle(&kp, kx[i], ky[i], kw, kh);
                    DrawLabel(g, gp::PointF(kx[i] + 7 * hs, ky[i] + 3 * hs), nm[i], k[i] ? Rgb(255, 255, 255) : Rgb(40, 40, 40), fs);
                }
            }
            // big status line
            if (s.isTimeStop || !s.playerAlive) {
                wchar_t b[256];
                if (s.isTimeStop)
                    std::swprintf(b, 256, L"FLAG  frame %d  (%.3f s)%ls", s.winFrame, s.winFrame / 31.0,
                                  s.deathFrame >= 0 ? (g_s.tl.warpValid() ? L"  death warp" : L"  death warp, too late for a real run") : L"");
                else
                    std::swprintf(b, 256, L"DEAD  (frame %d)", s.deathFrame);
                gp::SolidBrush back(Rgb(255, 255, 255, 170));
                g.FillRectangle(&back, (INT)(w / 2 - 230), (INT)(h - 40), 460, 32);
                DrawLabel(g, gp::PointF((gp::REAL)(w / 2 - 220), (gp::REAL)(h - 38)), b, s.isTimeStop ? Rgb(170, 110, 0) : Rgb(190, 0, 0), 20);
            }
        }
    }
    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

// ------------------------------------------------------------------ refreshing the controls
static const wchar_t* InputName(uint8_t c) {
    static const wchar_t* n[9] = {L"none", L"right", L"up", L"up+right", L"left", L"left+right", L"left+up", L"left+up+right", L"R (restart)"};
    return c < 9 ? n[c] : L"?";
}

static std::wstring RouteSummary(const tas::Route& r) {
    wchar_t b[96];
    std::swprintf(b, 96, L"L%d%ls  %d fr  ", r.level, r.checkpoint ? (L" cp" + std::to_wstring(r.checkpoint)).c_str() : L"", (int)r.in.size());
    std::wstring s = b;
    if (r.gless) s += L"[gless] ";
    if (r.Tool()) s += L"[tool] ";
    std::string rle = r.in.empty() ? "-" : EncodeInputsRLE(r.in);
    if (rle.size() > 60) rle = rle.substr(0, 57) + "...";
    return s + Widen(rle);
}

static void RefreshMode() {
    std::wstring m;
    if (g_s.brute.active) m = L"BRUTE FORCE";
    else if (g_s.paused || g_s.speed == 0) m = g_s.InPlayback() ? L"PAUSED (playback)" : L"PAUSED";
    else m = g_s.InPlayback() ? L"PLAYBACK" : L"RECORDING";
    if (g_ioWait == 1) m = L"IMPORT: press 0-9";
    if (g_ioWait == -1) m = L"EXPORT: press 0-9";
    if (GetText(g_mode) != m) {
        SetWindowTextW(g_mode, m.c_str());
        InvalidateRect(g_mode, nullptr, TRUE);
    }
    SetText(g_play, g_s.paused ? L"Play (T)" : L"Pause (T)");
    static const double speeds[] = {0, 1.0 / 32, 1.0 / 16, 1.0 / 8, 0.25, 0.5, 1, 2, 4, 8, 16, 32, 64};
    static const wchar_t* names[] = {L"0x", L"1/32x", L"1/16x", L"1/8x", L"1/4x", L"1/2x", L"1x", L"2x", L"4x", L"8x", L"16x", L"32x", L"64x"};
    for (int i = 0; i < 13; ++i)
        if (speeds[i] == g_s.speed) {
            // never move the slider under the mouse or the keyboard focus
            if (GetCapture() != g_speed && GetFocus() != g_speed && SendMessageW(g_speed, TBM_GETPOS, 0, 0) != i)
                SendMessageW(g_speed, TBM_SETPOS, TRUE, i);
            SetText(g_speedLbl, names[i]);
        }
    // the Options tab mirrors the keys (U, Shift+U, H, Shift+H, J, ...)
    SetCheck(g_realtime, g_s.realtimeLoad);
    SetCheck(g_fast, g_s.fastPlayback);
    SetCheck(g_pauseEnd, g_s.stopAtEnd);
    SetCheck(g_keepEnd, !g_s.stopAtEnd);
    SetCheck(g_hudBox, g_hud);
    SetCheck(g_marksBox, g_marks);
    SetCheck(g_polysBox, g_polys);
    SetCheck(g_trail, g_trail_on);
    SetCheck(g_follow, g_follow_on);
    if (GetCapture() != g_dec && SendMessageW(g_dec, TBM_GETPOS, 0, 0) != g_decimals) SendMessageW(g_dec, TBM_SETPOS, TRUE, g_decimals);
    SetText(g_lblDec, L"HUD decimals: " + std::to_wstring(g_decimals));
}

static void RefreshStatus() {
    if (!g_s.cur) return;
    const Sim& s = *g_s.cur;
    const Body& b = s.world.bodies[s.playerBody];
    wchar_t buf[512];
    std::swprintf(buf, 512, L"  frame %d / %d   next: %ls   |   ball x %.2f  y %.2f px   vx %.3f  vy %.3f   |   %ls%ls", g_s.pos, g_s.tl.size(),
                  g_s.InPlayback() ? InputName(g_s.route().in[(size_t)g_s.pos]) : L"(recording)", s.spriteX[s.playerBody], s.spriteY[s.playerBody],
                  b.linearVelocity.x, b.linearVelocity.y, s.isTimeStop ? L"flag" : s.playerAlive ? L"alive" : L"dead",
                  s.displayUncertain ? L"   |   a hit test was within a rounding unit: check in Flash" : L"");
    SetText(g_status, buf);
    SendMessageW(g_slider, TBM_SETRANGEMAX, TRUE, (LPARAM)g_s.tl.size());
    SendMessageW(g_slider, TBM_SETPOS, TRUE, g_s.pos);
    SetText(g_feedback, Widen(g_s.feedback));
    RefreshMode();
}

static void RefreshRouteText(bool force) {
    const std::string rle = g_s.route().in.empty() ? "" : EncodeInputsRLE(g_s.route().in);
    if (rle == g_shownRoute && !force) return;
    if (GetFocus() == g_inputs) return;  // never overwrite what is being typed
    g_shownRoute = rle;
    SetText(g_inputs, Widen(rle));
}

static void RefreshEvents(bool force) {
    const std::vector<tas::Event>& ev = g_s.tl.events();
    if (!force && ev.size() == g_shownEvents) return;
    g_shownEvents = ev.size();
    SendMessageW(g_events, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g_events, LB_RESETCONTENT, 0, 0);
    if (ev.empty()) SendMessageW(g_events, LB_ADDSTRING, 0, (LPARAM)L"(no deaths, checkpoints or flag yet)");
    for (const tas::Event& e : ev) {
        const std::wstring line = L"[" + std::to_wstring(e.index) + L"]  " + Widen(e.text);
        SendMessageW(g_events, LB_ADDSTRING, 0, (LPARAM)line.c_str());
    }
    SendMessageW(g_events, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_events, nullptr, TRUE);
}

static void RefreshBrute() {
    const tas::BruteEnv& e = g_s.brute;
    wchar_t b[400];
    if (!e.started)
        std::swprintf(b, 400, L"The Practice Hack's jump brute-forcer.\r\nPress B on the take-off frame (jump held), then play the jump and press "
                              L"Shift+B on the landing frame.");
    else if (!e.active)
        std::swprintf(b, 400, L"Take-off saved after frame %d (next frame: y %.4f, vy %.4f).\r\nPlay the jump, then Shift+B on the landing frame.",
                      e.jumpstart, e.ypos0, e.vy0);
    else
        std::swprintf(b, 400, L"%d frames after the take-off, %d height steps (target y %.5f m), %d ways to get there.\r\nW/S (Up/Down) change the "
                              L"height, Space picks, Enter loads, Esc leaves. Each line is simulated.",
                      e.x, e.currYOffsets, e.TargetY(), (int)e.cands.size());
    SetText(g_bInfo, b);
    SendMessageW(g_bList, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < e.cands.size(); ++i) {
        const tas::BruteCandidate& c = e.cands[i];
        const std::string rle = EncodeInputsRLE(std::vector<uint8_t>(c.in.begin() + std::min((size_t)e.jumpstart, c.in.size()), c.in.end()));
        wchar_t t[600];
        std::swprintf(t, 600, L"%ls end vy %.3f  (simulated: y %.5f, vy %.3f%ls)   jump: %hs", (int)i == e.choice ? L">" : L"  ", c.predictedVy, c.endY,
                      c.endVy, c.alive ? L"" : L", dead", rle.c_str());
        SendMessageW(g_bList, LB_ADDSTRING, 0, (LPARAM)t);
    }
    if (e.choice < (int)e.cands.size()) SendMessageW(g_bList, LB_SETCURSEL, (WPARAM)e.choice, 0);
}

static void RefreshGhostInfo() {
    if (!g_ghostOn) {
        SetText(g_gInfo, L"No ghost. A ghost is a second route drawn as a blue ball on the same frame, e.g. the record you want to beat.");
        return;
    }
    const tas::Route& r = g_ghost.route();
    std::wstring t = L"Ghost: " + g_ghostName + L"\r\nLevel " + std::to_wstring(r.level) + L", " + std::to_wstring(r.in.size()) + L" frames";
    const int gw = g_ghost.winFrame(), cw = g_s.tl.winFrame();
    if (gw >= 0) t += L", flag on frame " + std::to_wstring(gw);
    if (gw >= 0 && cw >= 0) t += L"\r\nCurrent route: flag on frame " + std::to_wstring(cw) + L" (" + (cw <= gw ? L"" : L"+") + std::to_wstring(cw - gw) + L" frames)";
    if (r.level != g_s.route().level) t += L"\r\n(another level: not drawn)";
    SetText(g_gInfo, t);
}

static void RefreshStates() {
    int sel = (int)SendMessageW(g_slots, LB_GETCURSEL, 0, 0);
    SendMessageW(g_slots, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < 10; ++i) {
        const std::wstring s = std::to_wstring(i) + L":  " + (g_s.slotUsed[i] ? RouteSummary(g_s.slots[i]) : L"(empty)");
        SendMessageW(g_slots, LB_ADDSTRING, 0, (LPARAM)s.c_str());
    }
    SendMessageW(g_slots, LB_SETCURSEL, (WPARAM)std::max(0, sel), 0);
    sel = (int)SendMessageW(g_named, LB_GETCURSEL, 0, 0);
    SendMessageW(g_named, LB_RESETCONTENT, 0, 0);
    for (const tas::NamedState& n : g_s.named) {
        const std::wstring s = Widen(n.name) + L"  -  " + RouteSummary(n.route);
        SendMessageW(g_named, LB_ADDSTRING, 0, (LPARAM)s.c_str());
    }
    if (sel >= 0 && sel < (int)g_s.named.size()) SendMessageW(g_named, LB_SETCURSEL, (WPARAM)sel, 0);
    SendMessageW(g_backups, LB_RESETCONTENT, 0, 0);
    for (const tas::Route& r : g_s.backups) SendMessageW(g_backups, LB_ADDSTRING, 0, (LPARAM)RouteSummary(r).c_str());
}

static void RefreshAll() {
    RefreshRouteText(true);
    RefreshEvents(true);
    RefreshStatus();
    RefreshBrute();
    RefreshGhostInfo();
    SetDisplayControls();
    InvalidateRect(g_canvas, nullptr, FALSE);
    SaveSettings();
}

// ------------------------------------------------------------------ search (rbsim optimize / beam, in-process)
static void SearchSink(const char* text, bool) {
    std::string s(text);
    size_t a = 0;
    while (a < s.size()) {
        size_t b = s.find('\n', a);
        if (b == std::string::npos) b = s.size();
        if (b > a) PostMessageW(g_wnd, WM_SEARCH_LINE, 0, (LPARAM) new std::string(s.substr(a, b - a)));
        a = b + 1;
    }
}

static void StartSearch() {
    if (g_searchRunning) return;
    const tas::Route& r = g_s.route();
    if (r.Tool()) {
        MessageBoxW(g_wnd, L"The search works on real-game routes: turn invincibility off and remove warps first.", L"Search", MB_ICONINFORMATION);
        return;
    }
    const bool beam = SendMessageW(g_sMode, CB_GETCURSEL, 0, 0) == 1;
    std::vector<std::string> args = {"rbsim", beam ? "beam" : "optimize", "--level", std::to_string(r.level), "--checkpoint", std::to_string(r.checkpoint)};
    const std::string threads = Narrow(GetText(g_sThreads));
    if (!threads.empty()) args.push_back("--threads"), args.push_back(threads);
    if (r.gless) args.push_back("--gless");
    if (beam) {
        const std::vector<uint8_t> prefix(r.in.begin(), r.in.begin() + g_s.pos);
        if (std::find(prefix.begin(), prefix.end(), (uint8_t)IN_RESTART) != prefix.end()) {
            MessageBoxW(g_wnd, L"Beam search goes on from the shown frame; the route up to it must not contain R.", L"Search", MB_ICONINFORMATION);
            return;
        }
        if (!prefix.empty()) args.push_back("--prefix"), args.push_back(EncodeInputsRLE(prefix));
        args.push_back("--memory"), args.push_back(Narrow(GetText(g_sMem)));
        args.push_back("--max-frames"), args.push_back(Narrow(GetText(g_sMaxF)));
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        args.push_back("--dir"), args.push_back(Narrow(std::wstring(tmp)) + "rbview_beam_L" + std::to_string(r.level));
        args.push_back("--save-every"), args.push_back("100000");
    } else {
        if (r.in.empty() || std::find(r.in.begin(), r.in.end(), (uint8_t)IN_RESTART) != r.in.end()) {
            MessageBoxW(g_wnd, L"Optimize improves a route that takes the flag in one attempt (no R).", L"Search", MB_ICONINFORMATION);
            return;
        }
        args.push_back("--inputs"), args.push_back(EncodeInputsRLE(r.in));
        args.push_back("--time"), args.push_back(Narrow(GetText(g_sTime)));
    }
    SendMessageW(g_sLog, LB_RESETCONTENT, 0, 0);
    std::string cmd;
    for (const std::string& a : args) cmd += a + " ";
    SendMessageW(g_sLog, LB_ADDSTRING, 0, (LPARAM)Widen("> " + cmd).c_str());
    g_searchBest.clear();
    g_searchBase = r;
    g_searchCancel = false;
    g_outSink = SearchSink;
    g_searchRunning = true;
    SetDisplayControls();
    EnableWindow(g_sStart, FALSE);
    EnableWindow(g_sStop, TRUE);
    EnableWindow(g_sLoad, FALSE);
    SetText(g_sStatus, L"running...");
    if (g_searchThread.joinable()) g_searchThread.join();
    g_searchThread = std::thread([args, beam]() {
        std::vector<std::string> a = args;
        std::vector<char*> argv;
        for (std::string& s : a) argv.push_back(&s[0]);
        const int rc = beam ? CmdBeam((int)argv.size(), argv.data()) : CmdOptimize((int)argv.size(), argv.data());
        PostMessageW(g_wnd, WM_SEARCH_DONE, (WPARAM)rc, 0);
    });
}

static bool LooksLikeRoute(const std::string& s) {
    if (s.size() < 2 || !std::isalpha((unsigned char)s[0])) return false;
    for (char c : s)
        if (!std::isdigit((unsigned char)c) && std::string("ndweaSqWR").find(c) == std::string::npos) return false;
    return true;
}

// ------------------------------------------------------------------ the mod's keys
static uint8_t HeldCode() { return tas::KeysToCode(g_heldL || g_tapL, g_heldU || g_tapU, g_heldR || g_tapR); }
static void ClearTaps() { g_tapL = g_tapU = g_tapR = false; }
// Space held down: rbview repeats it itself (Windows' key repeat only repeats the last key pressed, so pressing A/W/D
// while holding Space would stop the frame advance). Delay and rate follow the Windows keyboard settings.
static bool g_spaceHeld = false, g_spaceShift = false;
static LARGE_INTEGER g_spaceNext;
static double KeyRepeatDelay() {
    int d = 1;
    SystemParametersInfoW(SPI_GETKEYBOARDDELAY, 0, &d, 0);
    return 0.25 * (std::max(0, std::min(3, d)) + 1);
}
static double KeyRepeatInterval() {
    DWORD sp = 31;
    SystemParametersInfoW(SPI_GETKEYBOARDSPEED, 0, &sp, 0);
    return 1.0 / (2.5 + std::min<DWORD>(sp, 31) * (27.5 / 31));
}
static void ReleaseAll() { g_heldL = g_heldU = g_heldR = false, g_spaceHeld = false, ClearTaps(); }

static void ZoomAt(double f) {
    POINT p;
    GetCursorPos(&p);
    ScreenToClient(g_canvas, &p);
    RECT rc;
    GetClientRect(g_canvas, &rc);
    const double z0 = std::min(rc.right / 550.0, rc.bottom / 400.0) * g_userZoom;
    const double wx = g_viewX + (p.x - rc.right / 2.0) / z0, wy = g_viewY + (p.y - rc.bottom / 2.0) / z0;
    g_userZoom = std::max(0.1, std::min(16.0, g_userZoom * f));
    const double z1 = std::min(rc.right / 550.0, rc.bottom / 400.0) * g_userZoom;
    if (!g_follow_on && p.x >= 0 && p.y >= 0 && p.x < rc.right && p.y < rc.bottom) {  // following: zoom about the camera
        g_viewX = wx - (p.x - rc.right / 2.0) / z1;
        g_viewY = wy - (p.y - rc.bottom / 2.0) / z1;
    }
}

static void ShowTab(int t);

static void OpenCommandLine() {
    SetFocus(g_cmd);
    SetWindowTextW(g_cmd, L"/");
    SendMessageW(g_cmd, EM_SETSEL, 1, 1);
}

// The Practice Hack's KeyDownHandler / KeyUpHandler. Returns true when the key was used.
static bool TasKey(UINT msg, WPARAM wp, LPARAM lp) {
    int vk = (int)wp;
    if (vk > 0 && vk < 256 && g_remap[vk]) vk = g_remap[vk];
    const bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    const bool repeat = down && (lp & (1 << 30));
    const bool shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;
    if (!down) {
        if (vk == VK_SPACE) g_spaceHeld = false;
        else if (vk == 'A' || vk == VK_LEFT) g_heldL = false;
        else if (vk == 'D' || vk == VK_RIGHT) g_heldR = false;
        else if (vk == 'W' || vk == VK_UP) g_heldU = false;
        else return false;
        return true;
    }
    tas::Session& s = g_s;
    auto redraw = [&]() {
        RefreshRouteText(false);
        RefreshEvents(false);
        RefreshStatus();
        InvalidateRect(g_canvas, nullptr, FALSE);
    };
    // keys that work in every mode
    if (vk == VK_OEM_2 || (vk == 'C' && shift)) {  // "/" (or Shift+C): the command line
        OpenCommandLine();
        return true;
    }
    if (vk == 'Z') {
        ZoomAt(shift ? 0.5 : 2);
        InvalidateRect(g_canvas, nullptr, FALSE);
        return true;
    }
    if (vk == VK_OEM_PLUS && !shift) {
        g_userZoom = 1, g_follow_on = true;
        SetCheck(g_follow, true);
        InvalidateRect(g_canvas, nullptr, FALSE);
        return true;
    }
    if (vk == VK_F1) {
        MessageBoxW(g_wnd, kKeyHelp, L"Keys", MB_ICONINFORMATION);
        return true;
    }
    // rbview: looking at any frame without changing the route
    if (vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT) {
        s.paused = true;
        if (vk == VK_HOME) s.Seek(0);
        else if (vk == VK_END) s.Seek(s.tl.size());
        else s.Seek(s.pos + (vk == VK_NEXT ? 1 : -1) * (shift ? 10 : 1));
        redraw();
        return true;
    }
    // the brute-force environment takes the keys (KeyDownHandlerBruteForcing)
    if (s.brute.active) {
        switch (vk) {
            case 'B':
                if (!shift) return true;
                s.brute.Leave(), s.feedback = "Left brute force environment!";
                break;
            case VK_ESCAPE: s.brute.Leave(), s.feedback = "Left brute force environment!"; break;
            case 'W': case VK_UP: s.brute.Adjust(1, s.tl); break;
            case 'S': case VK_DOWN: s.brute.Adjust(-1, s.tl); break;
            case VK_SPACE:
                if (!s.brute.cands.empty()) s.brute.choice = (s.brute.choice + 1) % (int)s.brute.cands.size();
                break;
            case VK_RETURN: s.BruteApply(); break;
            default: return true;
        }
        ShowTab(4);
        RefreshAll();
        return true;
    }
    // digits: save / load / import / export states
    if (vk >= '0' && vk <= '9') {
        if (repeat) return true;
        const int n = vk - '0';
        if (g_ioWait == 1) {
            tas::Route r;
            std::string err;
            if (RouteFromText(ClipboardText(), r, err)) s.ImportSlot(n, r, shift);
            else s.feedback = "Import failed: " + err;
            g_ioWait = 0;
        } else if (g_ioWait == -1) {
            if (s.slotUsed[n]) {
                SetClipboardText(Widen(EncodeInputsRLE(s.slots[n].in)));
                s.feedback = "State " + std::to_string(n) + " exported!";
            }
            g_ioWait = 0;
        } else if (shift) {
            s.LoadSlot(n);
        } else {
            s.SaveSlot(n);
        }
        SyncSetupControls();
        RefreshStates();
        RefreshAll();
        return true;
    }
    if (!repeat) switch (vk) {
            case 'G':
                if (shift) s.SetFlags(!s.route().gless, s.route().invincible), s.feedback = s.route().gless ? "Gless enabled!" : "Gless disabled!";
                else
                    s.SetFlags(s.route().gless, !s.route().invincible),
                        s.feedback = s.route().invincible ? "Invincibility enabled!" : "Invincibility disabled!";
                SyncSetupControls();
                RefreshAll();
                return true;
            case 'C': s.feedback.clear(); RefreshStatus(); return true;
            case 'H':
                if (shift) g_marks = !g_marks, s.feedback = g_marks ? "Graphics marks enabled" : "Graphics marks disabled";
                else g_hud = !g_hud, s.feedback = g_hud ? "Heads Up Display enabled" : "Heads Up Display disabled";
                SetCheck(g_hudBox, g_hud);
                redraw();
                return true;
            case 'J':
                g_polys = !g_polys;
                s.feedback = g_polys ? "Polygon display enabled" : "Polygon display disabled";
                redraw();
                return true;
            case 'L': return true;  // stage.align: nothing to align in rbview
            default: break;
        }
    switch (vk) {
        case 'T': case 'P':
            if (!repeat) {
                s.TogglePause();
                if (!s.paused) g_follow_on = true, SetCheck(g_follow, true);  // playing: back to the game camera
            }
            RefreshStatus();
            return true;
        case VK_OEM_PERIOD: s.SpeedUp(); RefreshStatus(); return true;
        case VK_OEM_COMMA: s.SpeedDown(); RefreshStatus(); return true;
        case VK_SPACE:  // one frame; while running the first press pauses. Held: repeats from the timer (TickSpace)
            if (repeat) return true;
            if (shift && (s.speed == 0 || s.paused)) s.UndoFrame();
            else if (!shift) s.FrameAdvance(HeldCode()), ClearTaps();
            g_spaceHeld = true, g_spaceShift = shift;
            QueryPerformanceCounter(&g_spaceNext);
            g_spaceNext.QuadPart += (LONGLONG)(KeyRepeatDelay() * (double)g_qpf.QuadPart);
            redraw();
            return true;
        case VK_ESCAPE:
            if (g_ioWait) g_ioWait = 0, s.feedback.clear();
            else if (s.InPlayback()) s.DropPlayback();
            RefreshAll();
            return true;
        default: break;
    }
    // the keys the mod ignores during a playback
    const bool runningPlayback = s.InPlayback() && !s.paused && s.speed > 0;
    if (!repeat || vk == VK_BACK) switch (vk) {
            case 'R':
                if (runningPlayback || ctrl) break;
                s.RestartLevel();
                RefreshAll();
                return true;
            case 'U':
                if (shift) s.fastPlayback = !s.fastPlayback, s.feedback = std::string("RTGS ") + (s.fastPlayback ? "ON" : "OFF");
                else s.realtimeLoad = !s.realtimeLoad, s.feedback = s.realtimeLoad ? "Realtime playback enabled!" : "Realtime playback disabled!";
                RefreshStatus();
                return true;
            case 'I':
                if (runningPlayback) break;
                g_ioWait = 1;
                s.feedback = "Copy a string, then select state to import to!";
                RefreshStatus();
                return true;
            case 'O':
                if (shift) {
                    ShowTab(1);
                    RefreshStates();
                    s.feedback = "Backups: the routes you replaced (States tab)";
                } else {
                    g_ioWait = -1;
                    s.feedback = "Select state to export!";
                }
                RefreshStatus();
                return true;
            case VK_BACK:
                if (runningPlayback) break;
                s.Rewind(shift ? 31 : 10);
                RefreshAll();
                return true;
            case 'F':
                if (runningPlayback) break;
                s.CheckpointRestart();
                RefreshAll();
                return true;
            case 'B':
                if (runningPlayback) break;
                if (shift) s.BruteEnd(), ShowTab(4);
                else s.BruteStart(HeldCode());
                RefreshAll();
                return true;
            default: break;
        }
    if (ctrl && (vk == 'A' || vk == VK_LEFT || vk == 'D' || vk == VK_RIGHT)) {
        if (!repeat && !runningPlayback) ChangeLevel(s.route().level + ((vk == 'A' || vk == VK_LEFT) ? -1 : 1));
        return true;
    }
    // movement. A key tapped between two frames still counts for the next one (Level.aVariable & co.), and rbview also
    // records it, so the route replays the same (the mod's gameState would miss such a tap).
    if (vk == 'A' || vk == VK_LEFT) {
        if (!g_heldL && !s.paused) g_tapL = true;
        g_heldL = true;
        return true;
    }
    if (vk == 'D' || vk == VK_RIGHT) {
        if (!g_heldR && !s.paused) g_tapR = true;
        g_heldR = true;
        return true;
    }
    if (vk == 'W' || vk == VK_UP) {
        if (!g_heldU && !s.paused) g_tapU = true;
        g_heldU = true;
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ layout
static void ShowTab(int t) {
    TabCtrl_SetCurSel(g_tabs, t);
    for (int i = 0; i < NPAGES; ++i)
        for (HWND h : g_pages[i]) ShowWindow(h, i == t ? SW_SHOW : SW_HIDE);
}

static void Layout() {
    RECT rc;
    GetClientRect(g_wnd, &rc);
    const int W = rc.right, H = rc.bottom, pad = S(8), rowH = S(26);
    int x = pad, y = pad;
    auto put = [&](HWND h, int w, int dy = 0, int hh = 0) {
        MoveWindow(h, x, y + dy, w, hh ? hh : rowH, TRUE);
        x += w + S(6);
    };
    // row 1: level setup and spike rule
    put(g_lblLevel, S(34), S(5));
    put(g_level, S(190), 0, S(400));
    put(g_lblCp, S(62), S(5));
    put(g_cp, S(112), 0, S(300));
    put(g_gless, S(140), S(3), S(22));
    put(g_invincible, S(110), S(3), S(22));
    put(g_lblSpikes, S(38), S(5));
    put(g_spikes, S(210), 0, S(300));
    put(g_lblWindow, S(48), S(5));
    put(g_window, S(90), 0, S(300));
    put(g_scale, S(80), 0, S(300));
    MoveWindow(g_help, W - pad - S(52), y, S(52), rowH, TRUE);
    MoveWindow(g_keys, W - pad - S(52) - S(6) - S(64), y, S(64), rowH, TRUE);
    // row 2: route string
    y += rowH + S(6), x = pad;
    put(g_lblInputs, S(34), S(5));
    const int btnW = S(62);
    put(g_inputs, std::max(S(160), W - x - pad - 5 * (btnW + S(6))));
    put(g_paste, btnW), put(g_copy, btnW), put(g_open, btnW), put(g_save, btnW), put(g_load, btnW);
    // row 3: transport
    y += rowH + S(6), x = pad;
    put(g_first, S(34)), put(g_back, S(34)), put(g_play, S(90)), put(g_fwd, S(34)), put(g_last, S(34));
    put(g_lblSpeed, S(40), S(5));
    put(g_speed, S(200), 0, S(30));
    put(g_speedLbl, S(44), S(5));
    put(g_follow, S(108), S(3), S(22));
    put(g_slider, std::max(S(80), W - x - pad));
    // row 4: command line and messages
    y += rowH + S(6), x = pad;
    put(g_cmd, S(360));
    put(g_cmdRun, S(96));
    put(g_mode, S(170), S(5));
    put(g_feedback, std::max(S(80), W - x - pad), S(5));
    // main area: level view, side panel
    y += rowH + S(6);
    const int statusH = S(24), sideW = S(420);
    const int mainH = H - y - statusH - pad;
    MoveWindow(g_canvas, pad, y, W - 2 * pad - sideW - S(6), mainH, TRUE);
    const int tx = W - pad - sideW;
    MoveWindow(g_tabs, tx, y, sideW, mainH, TRUE);
    RECT tr = {0, 0, sideW, mainH};
    TabCtrl_AdjustRect(g_tabs, FALSE, &tr);
    const int px = tx + tr.left + S(4), py = y + tr.top + S(4), pw = tr.right - tr.left - S(8), ph = tr.bottom - tr.top - S(8);
    MoveWindow(g_events, px, py, pw, ph, TRUE);
    {  // States
        int yy = py;
        const int bw = (pw - 3 * S(4)) / 4, bw3 = (pw - 2 * S(4)) / 3;
        MoveWindow(g_lblSlots, px, yy, pw, S(18), TRUE), yy += S(20);
        MoveWindow(g_slots, px, yy, pw, S(150), TRUE), yy += S(152);
        MoveWindow(g_slotSave, px, yy, bw, rowH, TRUE);
        MoveWindow(g_slotLoad, px + (bw + S(4)), yy, bw, rowH, TRUE);
        MoveWindow(g_slotImport, px + 2 * (bw + S(4)), yy, bw, rowH, TRUE);
        MoveWindow(g_slotExport, px + 3 * (bw + S(4)), yy, bw, rowH, TRUE);
        yy += rowH + S(10);
        MoveWindow(g_lblNamed, px, yy, pw, S(18), TRUE), yy += S(20);
        const int rest = py + ph - yy;  // named list + 2 button rows + backups label + list + button
        const int namedH = std::max(S(60), (rest - 3 * rowH - S(40)) * 55 / 100);
        MoveWindow(g_named, px, yy, pw, namedH, TRUE), yy += namedH + S(2);
        MoveWindow(g_nSave, px, yy, bw3, rowH, TRUE);
        MoveWindow(g_nLoad, px + (bw3 + S(4)), yy, bw3, rowH, TRUE);
        MoveWindow(g_nRename, px + 2 * (bw3 + S(4)), yy, bw3, rowH, TRUE);
        yy += rowH + S(4);
        MoveWindow(g_nDelete, px, yy, bw3, rowH, TRUE);
        MoveWindow(g_nGhost, px + (bw3 + S(4)), yy, bw3, rowH, TRUE);
        MoveWindow(g_nCopy, px + 2 * (bw3 + S(4)), yy, bw3, rowH, TRUE);
        yy += rowH + S(10);
        MoveWindow(g_lblBackups, px, yy, pw, S(18), TRUE), yy += S(20);
        const int bh = std::max(S(40), py + ph - yy - rowH - S(4));
        MoveWindow(g_backups, px, yy, pw, bh, TRUE), yy += bh + S(4);
        MoveWindow(g_bLoad, px, yy, bw3, rowH, TRUE);
    }
    {  // Ghost
        int yy = py;
        MoveWindow(g_gInfo, px, yy, pw, S(90), TRUE), yy += S(96);
        for (HWND b : {g_gCur, g_gPaste, g_gOpen, g_gClear}) MoveWindow(b, px, yy, pw, rowH, TRUE), yy += rowH + S(4);
    }
    {  // Search
        int yy = py;
        MoveWindow(g_sMode, px, yy, pw, S(200), TRUE), yy += rowH + S(6);
        const int lw = S(112), ew = pw / 2 - lw - S(6);
        MoveWindow(g_lblSTime, px, yy + S(4), lw, S(20), TRUE), MoveWindow(g_sTime, px + lw, yy, ew, rowH, TRUE);
        MoveWindow(g_lblSThreads, px + pw / 2, yy + S(4), lw, S(20), TRUE), MoveWindow(g_sThreads, px + pw / 2 + lw, yy, ew, rowH, TRUE);
        yy += rowH + S(4);
        MoveWindow(g_lblSMem, px, yy + S(4), lw, S(20), TRUE), MoveWindow(g_sMem, px + lw, yy, ew, rowH, TRUE);
        MoveWindow(g_lblSMaxF, px + pw / 2, yy + S(4), lw, S(20), TRUE), MoveWindow(g_sMaxF, px + pw / 2 + lw, yy, ew, rowH, TRUE);
        yy += rowH + S(6);
        const int bw3 = (pw - 2 * S(4)) / 3;
        MoveWindow(g_sStart, px, yy, bw3, rowH, TRUE);
        MoveWindow(g_sStop, px + bw3 + S(4), yy, bw3, rowH, TRUE);
        MoveWindow(g_sLoad, px + 2 * (bw3 + S(4)), yy, bw3, rowH, TRUE);
        yy += rowH + S(4);
        MoveWindow(g_sStatus, px, yy, pw, S(36), TRUE), yy += S(38);
        MoveWindow(g_sLog, px, yy, pw, py + ph - yy, TRUE);
    }
    {  // Brute force
        int yy = py;
        MoveWindow(g_bInfo, px, yy, pw, S(70), TRUE), yy += S(74);
        const int bw = (pw - 4 * S(4)) / 5;
        HWND bs[5] = {g_bUp, g_bDown, g_bNext, g_bApply, g_bLeave};
        for (int i = 0; i < 5; ++i) MoveWindow(bs[i], px + i * (bw + S(4)), yy, bw, rowH, TRUE);
        yy += rowH + S(4);
        MoveWindow(g_bList, px, yy, pw, py + ph - yy, TRUE);
    }
    {  // Options: each group box is sized from what it holds
        int yy = py;
        const int gx = px, gw = pw, ix = px + S(10), iw = pw - S(20), lh = S(22);
        int top = 0;
        auto begin = [&]() { top = yy, yy += S(20); };
        auto end = [&](HWND g) {
            MoveWindow(g, gx, top, gw, yy - top + S(8), TRUE);
            yy += S(16);
        };
        begin();
        for (HWND c : {g_realtime, g_fast, g_lblEnd}) MoveWindow(c, ix, yy, iw, lh, TRUE), yy += lh;
        MoveWindow(g_pauseEnd, ix + S(14), yy, iw - S(14), lh, TRUE), yy += lh;
        MoveWindow(g_keepEnd, ix + S(14), yy, iw - S(14), lh, TRUE), yy += lh;
        MoveWindow(g_lblFlag, ix, yy, iw, lh, TRUE), yy += lh;
        end(g_grpPlay);
        begin();
        for (HWND c : {g_hudBox, g_marksBox, g_polysBox, g_trail}) MoveWindow(c, ix, yy, iw, lh, TRUE), yy += lh;
        MoveWindow(g_lblDec, ix, yy + S(6), S(120), lh, TRUE);
        MoveWindow(g_dec, ix + S(124), yy, iw - S(124), S(30), TRUE), yy += S(32);
        end(g_grpView);
        begin();
        MoveWindow(g_lblWarp, ix, yy, iw, S(32), TRUE), yy += S(34);
        const int cw = (iw - 4 * S(4)) / 5;
        for (int i = 0; i < 5; ++i) {
            MoveWindow(g_wLbl[i], ix + i * (cw + S(4)), yy, cw, S(18), TRUE);
            MoveWindow(g_wEdit[i], ix + i * (cw + S(4)), yy + S(18), cw, rowH - S(2), TRUE);
        }
        yy += S(18) + rowH + S(4);
        const int bw3 = (iw - 2 * S(4)) / 3;
        MoveWindow(g_wFill, ix, yy, bw3, rowH, TRUE);
        MoveWindow(g_wApply, ix + bw3 + S(4), yy, bw3, rowH, TRUE);
        MoveWindow(g_wClear, ix + 2 * (bw3 + S(4)), yy, bw3, rowH, TRUE);
        yy += rowH;
        end(g_grpWarp);
        begin();
        MoveWindow(g_lblKeys, ix, yy, iw, lh, TRUE), yy += lh;
        MoveWindow(g_oKeys, ix, yy, S(180), rowH, TRUE), yy += rowH;
        end(g_grpKeys);
    }
    MoveWindow(g_status, 0, H - statusH, W, statusH, TRUE);
}

// ------------------------------------------------------------------ windows
static HWND MakeCtl(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0, int page = -1) {
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | (page > 0 ? 0 : WS_VISIBLE) | style, 0, 0, 10, 10, g_wnd, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(nullptr), nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    if (page >= 0) g_pages[page].push_back(h);
    return h;
}

static LRESULT CALLBACK CanvasProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            if (rc.right > 0 && rc.bottom > 0) Paint(hdc, rc.right, rc.bottom);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_MOUSEWHEEL:
            ZoomAt(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1.2 : 1 / 1.2);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            SetFocus(h);
            SetCapture(h);
            g_dragging = true;
            g_dragFrom = {(short)LOWORD(lp), (short)HIWORD(lp)};
            g_dragViewX = g_viewX, g_dragViewY = g_viewY;
            return 0;
        case WM_MOUSEMOVE:
            if (g_dragging) {
                RECT rc;
                GetClientRect(h, &rc);
                const double z = std::min(rc.right / 550.0, rc.bottom / 400.0) * g_userZoom;
                const int dx = (short)LOWORD(lp) - g_dragFrom.x, dy = (short)HIWORD(lp) - g_dragFrom.y;
                if (std::abs(dx) + std::abs(dy) > 8 && g_follow_on) {
                    g_follow_on = false;
                    SetCheck(g_follow, false);
                }
                if (!g_follow_on) {
                    g_viewX = g_dragViewX - dx / z;
                    g_viewY = g_dragViewY - dy / z;
                    InvalidateRect(h, nullptr, FALSE);
                }
            }
            return 0;
        case WM_LBUTTONUP:
            g_dragging = false;
            ReleaseCapture();
            return 0;
        case WM_GETDLGCODE: return DLGC_WANTALLKEYS;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static int SelectedNamed() {
    const int i = (int)SendMessageW(g_named, LB_GETCURSEL, 0, 0);
    return i >= 0 && i < (int)g_s.named.size() ? i : -1;
}

static void SetGhost(const tas::Route& r, const std::wstring& name) {
    g_ghost.Build(r);
    g_ghostOn = true;
    g_ghostName = name;
    RefreshGhostInfo();
    InvalidateRect(g_canvas, nullptr, FALSE);
}

static void FillWarpFields() {
    if (!g_s.cur) return;
    const Body& b = g_s.cur->world.bodies[g_s.cur->playerBody];
    const double v[5] = {b.linearVelocity.x, b.linearVelocity.y, b.angularVelocity * 0.35, b.xf.position.x, b.xf.position.y};
    for (int i = 0; i < 5; ++i) {
        wchar_t t[64];
        std::swprintf(t, 64, L"%.6g", v[i]);
        SetWindowTextW(g_wEdit[i], t);
    }
}

static void OnCommand(int id, int code, HWND ctl) {
    const bool isCombo = id == ID_LEVEL || id == ID_CHECKPOINT || id == ID_SPIKES || id == ID_SCALE || id == ID_SMODE;
    if (id == ID_WINDOW) {
        if (code == CBN_SELCHANGE) {
            const int sel = (int)SendMessageW(g_window, CB_GETCURSEL, 0, 0);
            wchar_t t[64] = L"";
            if (sel >= 0 && SendMessageW(g_window, CB_GETLBTEXTLEN, (WPARAM)sel, 0) < 64) SendMessageW(g_window, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)t);
            SetWindowTextW(g_window, t);
            Resimulate();
        } else if (code == CBN_KILLFOCUS) {
            Resimulate();
        }
        return;
    }
    if (isCombo && code == CBN_CLOSEUP) {
        FocusCanvas();
        return;
    }
    if (isCombo && code != CBN_SELCHANGE) return;
    if ((id == ID_EVENTS || id == ID_SLOTS || id == ID_NAMED || id == ID_BACKUPS || id == ID_BLIST || id == ID_SLOG) && code != LBN_SELCHANGE &&
        code != LBN_DBLCLK)
        return;
    if (id == ID_INPUTS || id == IDC_CMD || id == ID_STIME || id == ID_STHREADS || id == ID_SMEM || id == ID_SMAXF) return;
    tas::Session& s = g_s;
    bool keepFocus = false;
    switch (id) {
        case ID_LEVEL:
            FillCheckpoints((int)SendMessageW(g_level, CB_GETCURSEL, 0, 0) + 1, 0);
            LoadFromControls(false);
            break;
        case ID_CHECKPOINT: LoadFromControls(false); break;
        case ID_GLESS:
        case ID_INVINCIBLE: s.SetFlags(Checked(g_gless), Checked(g_invincible)); break;
        case ID_SPIKES:
        case ID_SCALE: Resimulate(); break;
        case ID_KEYS: KeysDialog(); break;
        case ID_HELP: MessageBoxW(g_wnd, kKeyHelp, L"Keys", MB_ICONINFORMATION); break;
        case ID_LOAD: LoadFromControls(true); break;
        case ID_PASTE: {
            tas::Route r;
            std::string err;
            if (RouteFromText(ClipboardText(), r, err)) LoadRouteObject(r, false);
            else MessageBoxW(g_wnd, Widen(err).c_str(), L"Paste", MB_ICONWARNING);
            break;
        }
        case ID_COPY: SetClipboardText(Widen(s.route().in.empty() ? "" : EncodeInputsRLE(s.route().in))); break;
        case ID_OPEN: {
            std::wstring f;
            if (PickFile(false, f)) LoadRouteFile(f.c_str());
            break;
        }
        case ID_SAVE: SaveRouteFile(); break;
        case ID_FIRST: s.paused = true, s.Seek(0); break;
        case ID_BACK: s.paused = true, s.Seek(s.pos - 1); break;
        case ID_FWD:
            s.paused = true;
            if (s.InPlayback()) s.Seek(s.pos + 1);
            else s.FrameAdvance(HeldCode());
            break;
        case ID_LAST: s.paused = true, s.Seek(s.tl.size()); break;
        case ID_PLAY:
            s.TogglePause();
            if (!s.paused) g_follow_on = true, SetCheck(g_follow, true);
            break;
        case ID_REALTIME: s.realtimeLoad = Checked(g_realtime); break;
        case ID_OFAST: s.fastPlayback = Checked(g_fast); break;
        case ID_OPAUSEEND: s.stopAtEnd = true; break;
        case ID_OKEEPEND: s.stopAtEnd = false; break;
        case ID_OMARKS: g_marks = Checked(g_marksBox); break;
        case ID_OPOLYS: g_polys = Checked(g_polysBox); break;
        case ID_OKEYS: KeysDialog(); break;
        case ID_WFILL: FillWarpFields(); break;
        case ID_WAPPLY: {
            double v[5];
            for (int i = 0; i < 5; ++i) {
                const std::string t = Narrow(GetText(g_wEdit[i]));
                char* end = nullptr;
                v[i] = std::strtod(t.c_str(), &end);
                if (t.empty() || (end && *end && !std::isspace((unsigned char)*end))) {
                    MessageBoxW(g_wnd, L"The warp fields must be numbers (use . for decimals).", L"Warp", MB_ICONWARNING);
                    return;
                }
            }
            s.AddWarp({v[0], v[1], v[2], v[3], v[4]});
            s.feedback = "Warped at frame " + std::to_string(s.pos);
            break;
        }
        case ID_WCLEAR: s.RemoveWarps(); s.feedback = "Warps removed"; break;
        case ID_FOLLOW: g_follow_on = Checked(g_follow); break;
        case ID_TRAIL: g_trail_on = Checked(g_trail); break;
        case ID_HUD: g_hud = Checked(g_hudBox); break;
        case IDC_CMDRUN: {
            const std::string line = Narrow(GetText(g_cmd));
            if (line.find("mathspikes") != std::string::npos && g_searchRunning) {
                s.feedback = "Stop the search before changing the spike rule";
            } else {
                const tas::CommandResult r = tas::RunCommand(s, line, g_decimals);
                if (r.remapReset) std::fill(g_remap, g_remap + 256, 0);
                if (r.remapFrom > 0 && r.remapFrom < 256 && r.remapTo > 0 && r.remapTo < 256) g_remap[r.remapFrom] = r.remapTo;
                if (r.displayChanged) s.Load(s.route(), s.pos);
                if (!r.feedback.empty()) s.feedback = r.feedback;
            }
            SetWindowTextW(g_cmd, L"");
            break;
        }
        case ID_EVENTS: {
            const int sel = (int)SendMessageW(g_events, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < (int)s.tl.events().size()) s.paused = true, s.Seek(s.tl.events()[(size_t)sel].index);
            RefreshStatus();
            InvalidateRect(g_canvas, nullptr, FALSE);
            return;  // keep the focus in the list
        }
        case ID_SLOTS:
            if (code == LBN_DBLCLK) {
                const int n = (int)SendMessageW(g_slots, LB_GETCURSEL, 0, 0);
                if (n >= 0 && s.slotUsed[n]) s.LoadSlot(n);
                break;
            }
            return;
        case ID_SLOTSAVE: {
            const int n = (int)SendMessageW(g_slots, LB_GETCURSEL, 0, 0);
            if (n >= 0) s.SaveSlot(n);
            break;
        }
        case ID_SLOTLOAD: {
            const int n = (int)SendMessageW(g_slots, LB_GETCURSEL, 0, 0);
            if (n >= 0 && s.slotUsed[n]) s.LoadSlot(n);
            break;
        }
        case ID_SLOTIMPORT: {
            const int n = (int)SendMessageW(g_slots, LB_GETCURSEL, 0, 0);
            tas::Route r;
            std::string err;
            if (n >= 0 && RouteFromText(ClipboardText(), r, err)) s.ImportSlot(n, r, false);
            else if (n >= 0) MessageBoxW(g_wnd, Widen(err).c_str(), L"Import", MB_ICONWARNING);
            break;
        }
        case ID_SLOTEXPORT: {
            const int n = (int)SendMessageW(g_slots, LB_GETCURSEL, 0, 0);
            if (n >= 0 && s.slotUsed[n]) SetClipboardText(Widen(EncodeInputsRLE(s.slots[n].in))), s.feedback = "State " + std::to_string(n) + " exported!";
            break;
        }
        case ID_NAMED:
            if (code == LBN_DBLCLK && SelectedNamed() >= 0) {
                s.LoadRoute(s.named[(size_t)SelectedNamed()].route);
                break;
            }
            return;
        case ID_NSAVE: {
            std::wstring name = L"L" + std::to_wstring(s.route().level) + L" frame " + std::to_wstring(s.pos);
            if (InputBox(L"Save state", L"Name of the state (the route up to the shown frame):", name)) {
                s.SaveNamed(Narrow(name));
                RefreshStates();
                SendMessageW(g_named, LB_SETCURSEL, (WPARAM)s.named.size() - 1, 0);
            }
            break;
        }
        case ID_NLOAD:
            if (SelectedNamed() >= 0) s.LoadRoute(s.named[(size_t)SelectedNamed()].route);
            break;
        case ID_NRENAME:
            if (SelectedNamed() >= 0) {
                const int i = SelectedNamed();
                std::wstring name = Widen(s.named[(size_t)i].name);
                if (InputBox(L"Rename", L"New name:", name)) s.named[(size_t)i].name = Narrow(name);
            }
            break;
        case ID_NDELETE:
            if (SelectedNamed() >= 0 &&
                MessageBoxW(g_wnd, (L"Delete \"" + Widen(s.named[(size_t)SelectedNamed()].name) + L"\"?").c_str(), L"Delete", MB_OKCANCEL | MB_ICONQUESTION) == IDOK)
                s.named.erase(s.named.begin() + SelectedNamed());
            break;
        case ID_NGHOST:
            if (SelectedNamed() >= 0) SetGhost(s.named[(size_t)SelectedNamed()].route, Widen(s.named[(size_t)SelectedNamed()].name)), ShowTab(2);
            break;
        case ID_NCOPY:
            if (SelectedNamed() >= 0) SetClipboardText(Widen(EncodeInputsRLE(s.named[(size_t)SelectedNamed()].route.in)));
            break;
        case ID_BACKUPS:
            if (code == LBN_DBLCLK) {
                const int i = (int)SendMessageW(g_backups, LB_GETCURSEL, 0, 0);
                if (i >= 0 && i < (int)s.backups.size()) LoadRouteObject(tas::Route(s.backups[(size_t)i]), false);
                break;
            }
            return;
        case ID_BLOAD: {
            const int i = (int)SendMessageW(g_backups, LB_GETCURSEL, 0, 0);
            if (i >= 0 && i < (int)s.backups.size()) LoadRouteObject(tas::Route(s.backups[(size_t)i]), false);
            break;
        }
        case ID_GCUR: SetGhost(s.route(), L"a copy of the route at that time"); break;
        case ID_GPASTE: {
            tas::Route r;
            std::string err;
            if (RouteFromText(ClipboardText(), r, err)) SetGhost(r, L"pasted route");
            else MessageBoxW(g_wnd, Widen(err).c_str(), L"Ghost", MB_ICONWARNING);
            break;
        }
        case ID_GOPEN: {
            std::wstring f;
            tas::Route r;
            if (PickFile(false, f) && ReadRouteFile(f.c_str(), r)) SetGhost(r, f.substr(f.find_last_of(L"\\/") + 1));
            SetDisplayControls();
            break;
        }
        case ID_GCLEAR:
            g_ghostOn = false;
            RefreshGhostInfo();
            break;
        case ID_SSTART: StartSearch(); keepFocus = true; break;
        case ID_SSTOP: g_searchCancel = true; keepFocus = true; break;
        case ID_SLOAD:
            if (!g_searchBest.empty()) {
                tas::Route r = g_searchBase;
                r.in = DecodeInputs(g_searchBest);
                // the searches play "no input" after the end of a route: add those frames up to the flag
                tas::Timeline t;
                t.Build(r);
                for (int k = 0; k < 600 && t.winIndex() < 0; ++k) t.Append(IN_NONE);
                if (t.winIndex() >= 0) r.in = t.route().in;
                LoadRouteObject(r, false);
            }
            break;
        case ID_BUP: s.brute.Adjust(1, s.tl); break;
        case ID_BDOWN: s.brute.Adjust(-1, s.tl); break;
        case ID_BNEXT:
            if (!s.brute.cands.empty()) s.brute.choice = (s.brute.choice + 1) % (int)s.brute.cands.size();
            break;
        case ID_BAPPLY: s.BruteApply(); break;
        case ID_BLEAVE: s.brute.Leave(); break;
        case ID_BLIST:
            s.brute.choice = std::max(0, (int)SendMessageW(g_bList, LB_GETCURSEL, 0, 0));
            if (code == LBN_DBLCLK) s.BruteApply();
            break;
        case ID_SLOG: return;
        default: return;
    }
    const bool dropped = isCombo && ctl && SendMessageW(ctl, CB_GETDROPPEDSTATE, 0, 0);
    if (!dropped && !keepFocus) FocusCanvas();
    SyncSetupControls();
    RefreshAll();
    RefreshStates();
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_CREATE: {
            g_wnd = h;
            HDC dc = GetDC(h);
            g_dpi = GetDeviceCaps(dc, LOGPIXELSX) / 96.0;
            ReleaseDC(h, dc);
            NONCLIENTMETRICSW ncm = {};
            ncm.cbSize = sizeof ncm;
            SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
            g_font = CreateFontIndirectW(&ncm.lfMessageFont);
            // row 1
            g_lblLevel = MakeCtl(L"STATIC", L"Level", 0, 0);
            g_level = MakeCtl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_LEVEL);
            for (int i = 1; i <= NUM_LEVELS; ++i) {
                wchar_t b[64];
                std::swprintf(b, 64, L"%d: %ls", i, kLevelNames[i]);
                SendMessageW(g_level, CB_ADDSTRING, 0, (LPARAM)b);
            }
            g_lblCp = MakeCtl(L"STATIC", L"Start from", 0, 0);
            g_cp = MakeCtl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_CHECKPOINT);
            g_gless = MakeCtl(L"BUTTON", L"Glitchless (Shift+G)", BS_AUTOCHECKBOX | WS_TABSTOP, ID_GLESS);
            g_invincible = MakeCtl(L"BUTTON", L"Invincible (G)", BS_AUTOCHECKBOX | WS_TABSTOP, ID_INVINCIBLE);
            g_lblSpikes = MakeCtl(L"STATIC", L"Spikes", 0, 0);
            g_spikes = MakeCtl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_SPIKES);
            for (const wchar_t* n : kSpikeNames) SendMessageW(g_spikes, CB_ADDSTRING, 0, (LPARAM)n);
            g_lblWindow = MakeCtl(L"STATIC", L"Window", 0, 0);
            g_window = MakeCtl(L"COMBOBOX", L"", CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP, ID_WINDOW);
            for (const wchar_t* n : kWindowPresets) SendMessageW(g_window, CB_ADDSTRING, 0, (LPARAM)n);
            g_scale = MakeCtl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_SCALE);
            SendMessageW(g_scale, CB_ADDSTRING, 0, (LPARAM)L"Show all");
            SendMessageW(g_scale, CB_ADDSTRING, 0, (LPARAM)L"No scale");
            g_keys = MakeCtl(L"BUTTON", L"Keys...", BS_PUSHBUTTON | WS_TABSTOP, ID_KEYS);
            g_help = MakeCtl(L"BUTTON", L"Help", BS_PUSHBUTTON | WS_TABSTOP, ID_HELP);
            // row 2
            g_lblInputs = MakeCtl(L"STATIC", L"Route", 0, 0);
            g_inputs = MakeCtl(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, ID_INPUTS, WS_EX_CLIENTEDGE);
            g_paste = MakeCtl(L"BUTTON", L"Paste", BS_PUSHBUTTON | WS_TABSTOP, ID_PASTE);
            g_copy = MakeCtl(L"BUTTON", L"Copy", BS_PUSHBUTTON | WS_TABSTOP, ID_COPY);
            g_open = MakeCtl(L"BUTTON", L"Open...", BS_PUSHBUTTON | WS_TABSTOP, ID_OPEN);
            g_save = MakeCtl(L"BUTTON", L"Save...", BS_PUSHBUTTON | WS_TABSTOP, ID_SAVE);
            g_load = MakeCtl(L"BUTTON", L"Load", BS_PUSHBUTTON | WS_TABSTOP, ID_LOAD);
            // row 3
            g_first = MakeCtl(L"BUTTON", L"|<", BS_PUSHBUTTON, ID_FIRST);
            g_back = MakeCtl(L"BUTTON", L"<", BS_PUSHBUTTON, ID_BACK);
            g_play = MakeCtl(L"BUTTON", L"Play (T)", BS_PUSHBUTTON, ID_PLAY);
            g_fwd = MakeCtl(L"BUTTON", L">", BS_PUSHBUTTON, ID_FWD);
            g_last = MakeCtl(L"BUTTON", L">|", BS_PUSHBUTTON, ID_LAST);
            g_lblSpeed = MakeCtl(L"STATIC", L"Speed", 0, 0);
            g_speed = MakeCtl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | TBS_BOTTOM, ID_SPEED);
            SendMessageW(g_speed, TBM_SETRANGE, TRUE, MAKELPARAM(0, 12));
            SendMessageW(g_speed, TBM_SETPAGESIZE, 0, 1);
            SendMessageW(g_speed, TBM_SETPOS, TRUE, 6);
            g_speedLbl = MakeCtl(L"STATIC", L"1x", 0, 0);
            g_follow = MakeCtl(L"BUTTON", L"Follow camera", BS_AUTOCHECKBOX, ID_FOLLOW);
            // Options tab: everything the mod does with keys or commands, as controls
            g_grpPlay = MakeCtl(L"BUTTON", L"Playback", BS_GROUPBOX, 0, 0, 5);
            g_realtime = MakeCtl(L"BUTTON", L"Loading a state plays it back in real time (U)", BS_AUTOCHECKBOX, ID_REALTIME, 0, 5);
            g_fast = MakeCtl(L"BUTTON", L"Fast playback, 64 frames per frame (Shift+U)", BS_AUTOCHECKBOX, ID_OFAST, 0, 5);
            g_lblEnd = MakeCtl(L"STATIC", L"When a playback reaches the end of the route:", 0, 0, 0, 5);
            g_pauseEnd = MakeCtl(L"BUTTON", L"pause there", BS_AUTORADIOBUTTON | WS_GROUP, ID_OPAUSEEND, 0, 5);
            g_keepEnd = MakeCtl(L"BUTTON", L"keep running and record my keys (like the mod)", BS_AUTORADIOBUTTON, ID_OKEEPEND, 0, 5);
            g_lblFlag = MakeCtl(L"STATIC", L"(After the flag the run is over: the clock always stops there.)", 0, 0, 0, 5);
            g_grpView = MakeCtl(L"BUTTON", L"Display", BS_GROUPBOX, 0, 0, 5);
            g_hudBox = MakeCtl(L"BUTTON", L"HUD: the mod's numbers and keys (H)", BS_AUTOCHECKBOX, ID_HUD, 0, 5);
            g_marksBox = MakeCtl(L"BUTTON", L"Graphics marks: spike hitboxes, control points, jump probes (Shift+H)", BS_AUTOCHECKBOX, ID_OMARKS, 0, 5);
            g_polysBox = MakeCtl(L"BUTTON", L"Physics outlines and the ball's 16 control points (J)", BS_AUTOCHECKBOX, ID_OPOLYS, 0, 5);
            g_trail = MakeCtl(L"BUTTON", L"Show the path", BS_AUTOCHECKBOX, ID_TRAIL, 0, 5);
            g_lblDec = MakeCtl(L"STATIC", L"HUD decimals: 4", 0, 0, 0, 5);
            g_dec = MakeCtl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS, ID_ODEC, 0, 5);
            SendMessageW(g_dec, TBM_SETRANGE, TRUE, MAKELPARAM(0, 10));
            g_grpWarp = MakeCtl(L"BUTTON", L"Warp the ball (the mod's /set)", BS_GROUPBOX, 0, 0, 5);
            g_lblWarp = MakeCtl(L"STATIC", L"Replaces the ball at the shown frame. The warp stays in the route (rbview replays it, the game cannot).", 0, 0, 0, 5);
            {
                const wchar_t* wl[5] = {L"vx (m/s)", L"vy (m/s)", L"vrot (HUD)", L"x (m)", L"y (m)"};
                for (int i = 0; i < 5; ++i) {
                    g_wLbl[i] = MakeCtl(L"STATIC", wl[i], 0, 0, 0, 5);
                    g_wEdit[i] = MakeCtl(L"EDIT", L"0", ES_AUTOHSCROLL, 0, WS_EX_CLIENTEDGE, 5);
                }
            }
            g_wFill = MakeCtl(L"BUTTON", L"Take the ball's values", BS_PUSHBUTTON, ID_WFILL, 0, 5);
            g_wApply = MakeCtl(L"BUTTON", L"Warp at this frame", BS_PUSHBUTTON, ID_WAPPLY, 0, 5);
            g_wClear = MakeCtl(L"BUTTON", L"Remove all warps", BS_PUSHBUTTON, ID_WCLEAR, 0, 5);
            g_grpKeys = MakeCtl(L"BUTTON", L"Keys", BS_GROUPBOX, 0, 0, 5);
            g_lblKeys = MakeCtl(L"STATIC", L"Every key of the mod, and remapping (the mod's /remap):", 0, 0, 0, 5);
            g_oKeys = MakeCtl(L"BUTTON", L"Keys and remapping...", BS_PUSHBUTTON, ID_OKEYS, 0, 5);
            g_slider = MakeCtl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS, ID_SLIDER);
            // row 4
            g_cmd = MakeCtl(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_CMD, WS_EX_CLIENTEDGE);
            SendMessageW(g_cmd, EM_SETCUEBANNER, TRUE, (LPARAM)L"press /  for a command: /config mathspikes 0, /set -vx 5, /remap a left");
            g_cmdRun = MakeCtl(L"BUTTON", L"Run command", BS_PUSHBUTTON, IDC_CMDRUN);
            g_mode = MakeCtl(L"STATIC", L"", SS_LEFTNOWORDWRAP, ID_MODE);
            g_feedback = MakeCtl(L"STATIC", L"", SS_LEFTNOWORDWRAP, ID_FEEDBACK);
            // main
            g_canvas = CreateWindowExW(WS_EX_CLIENTEDGE, L"RbCanvas", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10, h, (HMENU)ID_CANVAS,
                                       GetModuleHandleW(nullptr), nullptr);
            g_tabs = MakeCtl(WC_TABCONTROLW, L"", WS_CLIPSIBLINGS, ID_TABS);
            const wchar_t* tabNames[NPAGES] = {L"Events", L"States", L"Ghost", L"Search", L"Brute force", L"Options"};
            for (int i = 0; i < NPAGES; ++i) {
                TCITEMW ti = {};
                ti.mask = TCIF_TEXT;
                ti.pszText = (LPWSTR)tabNames[i];
                TabCtrl_InsertItem(g_tabs, i, &ti);
            }
            g_status = MakeCtl(L"STATIC", L"", SS_LEFTNOWORDWRAP | SS_CENTERIMAGE, ID_STATUS);
            // tab pages (children of the main window, on top of the tab control)
            g_events = MakeCtl(L"LISTBOX", L"", WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY, ID_EVENTS, WS_EX_CLIENTEDGE, 0);
            g_lblSlots = MakeCtl(L"STATIC", L"Slots: 0-9 saves the route up to the shown frame, Shift+0-9 loads", 0, 0, 0, 1);
            g_slots = MakeCtl(L"LISTBOX", L"", WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY, ID_SLOTS, WS_EX_CLIENTEDGE, 1);
            g_slotSave = MakeCtl(L"BUTTON", L"Save", BS_PUSHBUTTON, ID_SLOTSAVE, 0, 1);
            g_slotLoad = MakeCtl(L"BUTTON", L"Load", BS_PUSHBUTTON, ID_SLOTLOAD, 0, 1);
            g_slotImport = MakeCtl(L"BUTTON", L"Import", BS_PUSHBUTTON, ID_SLOTIMPORT, 0, 1);
            g_slotExport = MakeCtl(L"BUTTON", L"Export", BS_PUSHBUTTON, ID_SLOTEXPORT, 0, 1);
            g_lblNamed = MakeCtl(L"STATIC", L"Saved states (no limit; kept in rbview_states.txt)", 0, 0, 0, 1);
            g_named = MakeCtl(L"LISTBOX", L"", WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY, ID_NAMED, WS_EX_CLIENTEDGE, 1);
            g_nSave = MakeCtl(L"BUTTON", L"Save as...", BS_PUSHBUTTON, ID_NSAVE, 0, 1);
            g_nLoad = MakeCtl(L"BUTTON", L"Load", BS_PUSHBUTTON, ID_NLOAD, 0, 1);
            g_nRename = MakeCtl(L"BUTTON", L"Rename...", BS_PUSHBUTTON, ID_NRENAME, 0, 1);
            g_nDelete = MakeCtl(L"BUTTON", L"Delete", BS_PUSHBUTTON, ID_NDELETE, 0, 1);
            g_nGhost = MakeCtl(L"BUTTON", L"Use as ghost", BS_PUSHBUTTON, ID_NGHOST, 0, 1);
            g_nCopy = MakeCtl(L"BUTTON", L"Copy string", BS_PUSHBUTTON, ID_NCOPY, 0, 1);
            g_lblBackups = MakeCtl(L"STATIC", L"Backups: routes replaced by R, rewinds and loads (Shift+O)", 0, 0, 0, 1);
            g_backups = MakeCtl(L"LISTBOX", L"", WS_VSCROLL | WS_HSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY, ID_BACKUPS, WS_EX_CLIENTEDGE, 1);
            g_bLoad = MakeCtl(L"BUTTON", L"Load backup", BS_PUSHBUTTON, ID_BLOAD, 0, 1);
            g_gInfo = MakeCtl(L"STATIC", L"", 0, ID_GINFO, 0, 2);
            g_gCur = MakeCtl(L"BUTTON", L"Current route as ghost", BS_PUSHBUTTON, ID_GCUR, 0, 2);
            g_gPaste = MakeCtl(L"BUTTON", L"Paste a route as ghost", BS_PUSHBUTTON, ID_GPASTE, 0, 2);
            g_gOpen = MakeCtl(L"BUTTON", L"Open a route or log as ghost...", BS_PUSHBUTTON, ID_GOPEN, 0, 2);
            g_gClear = MakeCtl(L"BUTTON", L"No ghost", BS_PUSHBUTTON, ID_GCLEAR, 0, 2);
            g_sMode = MakeCtl(L"COMBOBOX", L"", CBS_DROPDOWNLIST, ID_SMODE, 0, 3);
            SendMessageW(g_sMode, CB_ADDSTRING, 0, (LPARAM)L"Optimize: improve the whole route (rbsim optimize)");
            SendMessageW(g_sMode, CB_ADDSTRING, 0, (LPARAM)L"Beam: search on from the shown frame (rbsim beam)");
            SendMessageW(g_sMode, CB_SETCURSEL, 0, 0);
            g_lblSTime = MakeCtl(L"STATIC", L"Time (s, optimize)", 0, 0, 0, 3);
            g_sTime = MakeCtl(L"EDIT", L"60", ES_AUTOHSCROLL, ID_STIME, WS_EX_CLIENTEDGE, 3);
            g_lblSThreads = MakeCtl(L"STATIC", L"Threads", 0, 0, 0, 3);
            g_sThreads = MakeCtl(L"EDIT", std::to_wstring(std::max(1u, std::thread::hardware_concurrency() - 1)).c_str(), ES_AUTOHSCROLL, ID_STHREADS,
                                 WS_EX_CLIENTEDGE, 3);
            g_lblSMem = MakeCtl(L"STATIC", L"Memory (beam)", 0, 0, 0, 3);
            g_sMem = MakeCtl(L"EDIT", L"2G", ES_AUTOHSCROLL, ID_SMEM, WS_EX_CLIENTEDGE, 3);
            g_lblSMaxF = MakeCtl(L"STATIC", L"Max frames (beam)", 0, 0, 0, 3);
            g_sMaxF = MakeCtl(L"EDIT", L"3000", ES_AUTOHSCROLL, ID_SMAXF, WS_EX_CLIENTEDGE, 3);
            g_sStart = MakeCtl(L"BUTTON", L"Start", BS_PUSHBUTTON, ID_SSTART, 0, 3);
            g_sStop = MakeCtl(L"BUTTON", L"Stop", BS_PUSHBUTTON | WS_DISABLED, ID_SSTOP, 0, 3);
            g_sLoad = MakeCtl(L"BUTTON", L"Load result", BS_PUSHBUTTON | WS_DISABLED, ID_SLOAD, 0, 3);
            g_sStatus = MakeCtl(L"STATIC", L"Searches use the level, start, glitchless and spike settings above.", 0, ID_SSTATUS, 0, 3);
            g_sLog = MakeCtl(L"LISTBOX", L"", WS_VSCROLL | WS_HSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY, ID_SLOG, WS_EX_CLIENTEDGE, 3);
            SendMessageW(g_sLog, LB_SETHORIZONTALEXTENT, (WPARAM)S(2400), 0);
            SendMessageW(g_backups, LB_SETHORIZONTALEXTENT, (WPARAM)S(1200), 0);
            g_bInfo = MakeCtl(L"STATIC", L"", 0, ID_BINFO, 0, 4);
            g_bUp = MakeCtl(L"BUTTON", L"Higher (W)", BS_PUSHBUTTON, ID_BUP, 0, 4);
            g_bDown = MakeCtl(L"BUTTON", L"Lower (S)", BS_PUSHBUTTON, ID_BDOWN, 0, 4);
            g_bNext = MakeCtl(L"BUTTON", L"Next (Space)", BS_PUSHBUTTON, ID_BNEXT, 0, 4);
            g_bApply = MakeCtl(L"BUTTON", L"Load (Enter)", BS_PUSHBUTTON, ID_BAPPLY, 0, 4);
            g_bLeave = MakeCtl(L"BUTTON", L"Leave (Esc)", BS_PUSHBUTTON, ID_BLEAVE, 0, 4);
            g_bList = MakeCtl(L"LISTBOX", L"", WS_VSCROLL | WS_HSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY, ID_BLIST, WS_EX_CLIENTEDGE, 4);
            SendMessageW(g_bList, LB_SETHORIZONTALEXTENT, (WPARAM)S(1600), 0);
            for (int p = 0; p < NPAGES; ++p)
                for (HWND c : g_pages[p]) SetWindowPos(c, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            // settings next to the exe (portable)
            wchar_t exe[MAX_PATH];
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            std::wstring base = exe;
            const size_t dot = base.rfind(L'.');
            if (dot != std::wstring::npos) base = base.substr(0, dot);
            g_iniPath = base + L".ini";
            g_statesPath = base + L"_states.txt";
            const wchar_t* ini = g_iniPath.c_str();
            {
                const int ms = (int)GetPrivateProfileIntW(L"display", L"mathspikes", 1, ini);
                wchar_t win[64] = L"", sc[32] = L"";
                GetPrivateProfileStringW(L"display", L"window", L"550x400", win, 64, ini);
                GetPrivateProfileStringW(L"display", L"scale", L"showAll", sc, 32, ini);
                DisplayConfig& dc = GetDisplayConfig();
                dc.mathSpikes = ms >= -1 && ms <= 2 ? ms : 1;
                int ww = 550, wh = 400;
                if (ParseWindowSize(win, ww, wh)) dc.windowW = ww, dc.windowH = wh;
                dc.showAll = _wcsicmp(sc, L"noScale") != 0;
            }
            g_hud = GetPrivateProfileIntW(L"view", L"hud", 1, ini) != 0;
            g_marks = GetPrivateProfileIntW(L"view", L"marks", 0, ini) != 0;
            g_polys = GetPrivateProfileIntW(L"view", L"polygons", 0, ini) != 0;
            g_follow_on = true;  // the game camera is the standard view
            g_trail_on = GetPrivateProfileIntW(L"view", L"trail", 1, ini) != 0;
            g_decimals = (int)GetPrivateProfileIntW(L"view", L"decimals", 4, ini);
            g_s.realtimeLoad = GetPrivateProfileIntW(L"view", L"realtime", 0, ini) != 0;
            g_s.stopAtEnd = GetPrivateProfileIntW(L"view", L"stopatend", 0, ini) != 0;
            {
                wchar_t rm[4096] = L"";
                GetPrivateProfileStringW(L"keys", L"remap", L"", rm, 4096, ini);
                int a, b, n = 0;
                const wchar_t* p = rm;
                while (std::swscanf(p, L"%d=%d%n", &a, &b, &n) == 2) {
                    if (a > 0 && a < 256 && b > 0 && b < 256) g_remap[a] = b;
                    p += n;
                }
            }
            SetCheck(g_follow, g_follow_on);
            g_s.LoadStatesFile(Narrow(g_statesPath));
            tas::Route r;
            {
                static wchar_t line[65536];
                line[0] = 0;
                GetPrivateProfileStringW(L"route", L"line", L"", line, 65536, ini);
                std::string err;
                if (!line[0] || !tas::RouteFromLine(Narrow(line), r, err)) {
                    // an older rbview.ini: level / checkpoint / gless / inputs
                    r = tas::Route();
                    r.level = std::max(1, std::min(NUM_LEVELS, (int)GetPrivateProfileIntW(L"route", L"level", 1, ini)));
                    r.checkpoint = (int)GetPrivateProfileIntW(L"route", L"checkpoint", 0, ini);
                    r.gless = GetPrivateProfileIntW(L"route", L"gless", 0, ini) != 0;
                    static wchar_t inputs[32768];
                    inputs[0] = 0;
                    GetPrivateProfileStringW(L"route", L"inputs", L"", inputs, 32768, ini);
                    tas::ParseRLE(Narrow(inputs), r.in, err);
                }
            }
            {
                LevelTemplate t(r.level);
                if (r.checkpoint >= t.CheckpointCount()) r.checkpoint = 0;
            }
            g_s.Load(r, 0);
            g_s.paused = true;
            SendMessageW(g_level, CB_SETCURSEL, (WPARAM)r.level - 1, 0);
            FillCheckpoints(r.level, r.checkpoint);
            SyncSetupControls();
            Layout();
            ShowTab(0);
            RefreshAll();
            RefreshStates();
            QueryPerformanceFrequency(&g_qpf);
            QueryPerformanceCounter(&g_lastTick);
            SetTimer(h, ID_TIMER, 5, nullptr);
            DragAcceptFiles(h, TRUE);
            return 0;
        }
        case WM_DROPFILES: {
            wchar_t file[MAX_PATH];
            if (DragQueryFileW((HDROP)wp, 0, file, MAX_PATH)) LoadRouteFile(file);
            DragFinish((HDROP)wp);
            return 0;
        }
        case WM_SIZE:
            Layout();
            RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
            return 0;
        case WM_GETMINMAXINFO: {
            MINMAXINFO* mm = (MINMAXINFO*)lp;
            mm->ptMinTrackSize = {S(1280), S(660)};
            return 0;
        }
        case WM_ACTIVATEAPP:
            if (!wp) ReleaseAll();
            return 0;
        case WM_TIMER: {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            const double sec = double(now.QuadPart - g_lastTick.QuadPart) / double(g_qpf.QuadPart);
            g_lastTick = now;
            if (g_modal) return 0;
            if (g_spaceHeld && GetKeyState(VK_SPACE) >= 0) g_spaceHeld = false;  // released while another window had the keys
            if (g_spaceHeld && now.QuadPart >= g_spaceNext.QuadPart) {
                const LONGLONG step = (LONGLONG)(KeyRepeatInterval() * (double)g_qpf.QuadPart);
                int n = 0;
                while (now.QuadPart >= g_spaceNext.QuadPart && n < 8) {
                    if (g_s.speed == 0 || g_s.paused) {
                        if (g_spaceShift) g_s.UndoFrame();
                        else g_s.FrameAdvance(HeldCode()), ClearTaps();
                    }
                    g_spaceNext.QuadPart += step, ++n;
                }
                if (now.QuadPart >= g_spaceNext.QuadPart) g_spaceNext = now;
                RefreshRouteText(false);
                RefreshEvents(false);
                RefreshStatus();
                InvalidateRect(g_canvas, nullptr, FALSE);
            }
            const bool wasPaused = g_s.paused;
            const int n = g_s.Run(std::min(sec, 0.25) * 31.0, HeldCode());
            {  // autosave every few seconds while the route changes (a crash or a killed process keeps the work)
                static LARGE_INTEGER lastSave = now;
                static int savedSize = -1;
                if (double(now.QuadPart - lastSave.QuadPart) / double(g_qpf.QuadPart) > 3 && savedSize != g_s.tl.size()) {
                    lastSave = now;
                    savedSize = g_s.tl.size();
                    SaveSettings();
                }
            }
            if (n > 0) {
                ClearTaps();
                RefreshRouteText(false);
                RefreshEvents(false);
                RefreshStatus();
                InvalidateRect(g_canvas, nullptr, FALSE);
            } else if (wasPaused != g_s.paused) {
                RefreshStatus();
            }
            return 0;
        }
        case WM_HSCROLL:
            if ((HWND)lp == g_speed) {
                static const double speeds[] = {0, 1.0 / 32, 1.0 / 16, 1.0 / 8, 0.25, 0.5, 1, 2, 4, 8, 16, 32, 64};
                const int i = (int)SendMessageW(g_speed, TBM_GETPOS, 0, 0);
                if (i >= 0 && i < 13 && g_s.speed != speeds[i]) g_s.speed = speeds[i], g_s.tickAcc = 0;
                RefreshStatus();
                if (LOWORD(wp) == TB_ENDTRACK) FocusCanvas();
                return 0;
            }
            if ((HWND)lp == g_dec) {
                g_decimals = (int)SendMessageW(g_dec, TBM_GETPOS, 0, 0);
                RefreshStatus();
                InvalidateRect(g_canvas, nullptr, FALSE);
                if (LOWORD(wp) == TB_ENDTRACK) FocusCanvas();
                return 0;
            }
            if ((HWND)lp == g_slider) {
                g_s.paused = true;
                g_s.Seek((int)SendMessageW(g_slider, TBM_GETPOS, 0, 0));
                RefreshStatus();
                InvalidateRect(g_canvas, nullptr, FALSE);
            }
            return 0;
        case WM_NOTIFY: {
            const NMHDR* nm = (const NMHDR*)lp;
            if (nm->hwndFrom == g_tabs && nm->code == TCN_SELCHANGE) {
                ShowTab(TabCtrl_GetCurSel(g_tabs));
                RefreshStates();
                if (TabCtrl_GetCurSel(g_tabs) == 5) FillWarpFields();
            }
            return 0;
        }
        case WM_SEARCH_LINE: {
            std::string* s = (std::string*)lp;
            const int i = (int)SendMessageW(g_sLog, LB_ADDSTRING, 0, (LPARAM)Widen(*s).c_str());
            SendMessageW(g_sLog, LB_SETTOPINDEX, (WPARAM)std::max(0, i - 6), 0);
            // an improvement ("  frame N, margin ...  RLE") or the final route line
            std::string t = *s;
            while (!t.empty() && t.back() == ' ') t.pop_back();
            const size_t sp = t.find_last_of(' ');
            const std::string last = sp == std::string::npos ? t : t.substr(sp + 1);
            if (LooksLikeRoute(last)) {
                g_searchBest = last;
                EnableWindow(g_sLoad, TRUE);
                std::string head = t.substr(0, t.size() - last.size());
                while (!head.empty() && head.back() == ' ') head.pop_back();
                SetText(g_sStatus, Widen("best so far: " + (head.empty() ? std::to_string(DecodeInputs(last).size()) + " frames" : head)));
            }
            delete s;
            return 0;
        }
        case WM_SEARCH_DONE:
            if (g_searchThread.joinable()) g_searchThread.join();
            g_outSink = nullptr;
            g_searchRunning = false;
            EnableWindow(g_sStart, TRUE);
            EnableWindow(g_sStop, FALSE);
            SetText(g_sStatus, g_searchBest.empty() ? L"finished without a route." : L"finished. Load result puts the best route in the editor.");
            SetDisplayControls();
            return 0;
        case WM_COMMAND:
            OnCommand(LOWORD(wp), HIWORD(wp), (HWND)lp);
            return 0;
        case WM_CTLCOLORSTATIC:
            if ((HWND)lp == g_mode) {
                SetTextColor((HDC)wp, g_s.paused || g_s.speed == 0 ? RGB(90, 90, 90) : g_s.InPlayback() ? RGB(0, 90, 200) : RGB(200, 0, 0));
                SetBkMode((HDC)wp, TRANSPARENT);
                return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
            }
            break;
        case WM_CLOSE:
            if (g_searchRunning) {
                g_searchCancel = true;
                if (g_searchThread.joinable()) g_searchThread.join();
                g_outSink = nullptr;
                g_searchRunning = false;
            }
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            SaveSettings();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

// Is the user typing in a text box? Then the TAS keys stay out of the way (digits must not save states).
static bool Typing(HWND f) {
    if (!f) return false;
    wchar_t cls[32] = L"";
    GetClassNameW(f, cls, 32);
    if (!_wcsicmp(cls, L"Edit")) return true;
    if (!_wcsicmp(cls, L"ComboBox") && SendMessageW(f, CB_GETDROPPEDSTATE, 0, 0)) return true;
    return false;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_TAB_CLASSES};
    InitCommonControlsEx(&icc);
    gp::GdiplusStartupInput gsi;
    ULONG_PTR token;
    gp::GdiplusStartup(&token, &gsi, nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"RbViewer";
    wc.hIcon = LoadIconW(inst, L"APPICON");
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);
    WNDCLASSEXW cc{};
    cc.cbSize = sizeof cc;
    cc.lpfnWndProc = CanvasProc;
    cc.hInstance = inst;
    cc.hCursor = LoadCursor(nullptr, IDC_HAND);
    cc.lpszClassName = L"RbCanvas";
    RegisterClassExW(&cc);
    WNDCLASSEXW mc{};
    mc.cbSize = sizeof mc;
    mc.lpfnWndProc = ModalProc;
    mc.hInstance = inst;
    mc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    mc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    mc.lpszClassName = L"RbModal";
    RegisterClassExW(&mc);

    HDC dc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSX) / 96.0;
    ReleaseDC(nullptr, dc);
    HWND w = CreateWindowExW(0, L"RbViewer", L"Red Ball TAS (rbview)", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, S(1440), S(860), nullptr,
                             nullptr, inst, nullptr);
    ShowWindow(w, show);
    UpdateWindow(w);
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; argv && i < argc; ++i) {
            if (!wcscmp(argv[i], L"--frame") && i + 1 < argc) {
                g_s.Seek(_wtoi(argv[++i]));
                RefreshStatus();
                InvalidateRect(g_canvas, nullptr, FALSE);
            } else {
                LoadRouteFile(argv[i]);
            }
        }
        if (argv) LocalFree(argv);
    }
    FocusCanvas();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!g_modal && (msg.message == WM_KEYDOWN || msg.message == WM_KEYUP || msg.message == WM_SYSKEYDOWN || msg.message == WM_SYSKEYUP) &&
            msg.wParam != VK_TAB && msg.wParam != VK_MENU && msg.wParam != VK_F4) {
            HWND f = GetFocus();
            if (Typing(f)) {
                if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
                    if (f == g_inputs) {
                        LoadFromControls(true);
                        FocusCanvas();
                        continue;
                    }
                    if (f == g_cmd) {
                        OnCommand(IDC_CMDRUN, BN_CLICKED, nullptr);
                        continue;
                    }
                    if (GetParent(f) == g_window) {
                        Resimulate();
                        FocusCanvas();
                        continue;
                    }
                }
                if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
                    FocusCanvas();
                    continue;
                }
            } else if (TasKey(msg.message, msg.wParam, msg.lParam)) {
                continue;
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    gp::GdiplusShutdown(token);
    return 0;
}
