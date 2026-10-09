// viewer/rbview.cpp - "Red Ball route viewer": a single-file Windows program (Win32 + GDI+, no installer, no console)
// that plays a route through the bit-exact simulator and draws what happens: level geometry, moving parts, spikes,
// checkpoints, switches, the flag and the ball, with the game's own camera.
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
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../src/redball.h"

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

// ------------------------------------------------------------------ timeline: the route simulated once, with snapshots
struct FrameInfo {
    double bx, by, brot;  // ball sprite (px, degrees written)
    bool alive, won;
    int frame;            // Game.frameCount after this input
    int lastCheck;
};
struct Event {
    int index;  // input index after which it happened
    std::wstring text;
};
struct Timeline {
    std::unique_ptr<LevelTemplate> tpl;
    int level = 1, checkpoint = 0;
    bool gless = false;
    std::vector<uint8_t> in;
    static constexpr int K = 16;  // snapshot every K inputs
    std::vector<std::unique_ptr<Sim>> snaps;
    std::vector<FrameInfo> info;  // info[i] = state after i inputs
    std::vector<Event> events;
    int deathFrame = -1, winFrame = -1, winIndex = -1;
    bool warpValid = true;
};

static FrameInfo Info(const Sim& s) {
    FrameInfo f;
    f.bx = s.spriteX[s.playerBody];
    f.by = s.spriteY[s.playerBody];
    f.brot = s.spriteRotW[s.playerBody];
    f.alive = s.playerAlive;
    f.won = s.isTimeStop;
    f.frame = s.frameCount;
    f.lastCheck = s.lastCheckNum;
    return f;
}

static void Apply(Sim& s, uint8_t code) {
    if (code == IN_RESTART) s.Restart();
    else s.Tick(code);
}

static void BuildTimeline(Timeline& t) {
    t.tpl.reset(new LevelTemplate(t.level));
    t.snaps.clear();
    t.info.clear();
    t.events.clear();
    t.deathFrame = t.winFrame = t.winIndex = -1;
    auto s = std::make_unique<Sim>();
    s->Load(t.tpl.get(), t.checkpoint);
    s->gless = t.gless;
    t.snaps.push_back(std::make_unique<Sim>(*s));
    t.info.push_back(Info(*s));
    int lastDeath = -1, lastCheck = s->lastCheckNum;
    int lastSwitch[4] = {s->switchFrame[0], s->switchFrame[1], s->switchFrame[2], s->switchFrame[3]};
    bool lastStatic[4] = {s->staticFlag[0], s->staticFlag[1], s->staticFlag[2], s->staticFlag[3]};
    for (size_t i = 0; i < t.in.size(); ++i) {
        const bool wasWon = s->isTimeStop;
        Apply(*s, t.in[i]);
        wchar_t buf[160];
        if (t.in[i] == IN_RESTART) {
            std::swprintf(buf, 160, L"R: restart from checkpoint %d", s->lastCheckNum);
            t.events.push_back({(int)i + 1, buf});
            lastDeath = -1;
        }
        if (s->deathFrame >= 0 && s->deathFrame != lastDeath) {
            lastDeath = s->deathFrame;
            std::swprintf(buf, 160, L"death at frame %d", s->deathFrame);
            t.events.push_back({(int)i + 1, buf});
            t.deathFrame = s->deathFrame;
        }
        if (s->lastCheckNum > lastCheck && t.in[i] != IN_RESTART) {
            std::swprintf(buf, 160, L"checkpoint %d at frame %d%ls", s->lastCheckNum, s->frameCount, s->playerAlive ? L"" : L" (dead ball: warp)");
            t.events.push_back({(int)i + 1, buf});
        }
        lastCheck = s->lastCheckNum;
        for (int k = 0; k < 4; ++k) {
            if (t.in[i] != IN_RESTART && s->switchFrame[k] != lastSwitch[k] && s->switchFrame[k] != 1) {
                std::swprintf(buf, 160, L"switch pressed (frame %d)%ls", s->frameCount, s->playerAlive ? L"" : L" by the dead ball");
                t.events.push_back({(int)i + 1, buf});
            }
            lastSwitch[k] = s->switchFrame[k];
            if (s->staticFlag[k] != lastStatic[k] && s->staticFlag[k]) {
                const wchar_t* what = k == 2 ? L"wrong way: checkpoint reset to the start" : L"switch state saved for restarts";
                std::swprintf(buf, 160, L"%ls (frame %d)%ls", what, s->frameCount, s->playerAlive ? L"" : L", dead ball");
                t.events.push_back({(int)i + 1, buf});
            }
            lastStatic[k] = s->staticFlag[k];
        }
        if (s->isTimeStop && !wasWon) {
            t.winFrame = s->winFrame;
            t.winIndex = (int)i + 1;
            const bool afterDeath = s->deathFrame >= 0;
            t.warpValid = DeathWarpFinishValid(s->deathFrame, s->winFrame);
            std::swprintf(buf, 160, L"FLAG at frame %d (%.2f s)", s->winFrame, s->winFrame / 31.0);
            std::wstring text = buf;
            if (afterDeath) text += t.warpValid ? L", death warp" : L", death warp TOO LATE for a real run";
            t.events.push_back({(int)i + 1, text});
        }
        t.info.push_back(Info(*s));
        if ((i + 1) % Timeline::K == 0) t.snaps.push_back(std::make_unique<Sim>(*s));
    }
}

// ------------------------------------------------------------------ input parsing (the game's RLE format)
static bool ParseRoute(const std::string& raw, std::vector<uint8_t>& out, std::wstring& err) {
    std::string s;
    for (char c : raw)
        if (!std::isspace((unsigned char)c)) s += c;
    out.clear();
    if (s.empty()) return true;
    const std::string letters = "ndweaSqWR";
    long total = 0;
    for (size_t i = 0; i < s.size();) {
        if (letters.find(s[i]) == std::string::npos) {
            err = L"Unexpected character in the inputs. Use the game's format, e.g. d12e1w3n5 (n d w e a S q W, R = restart).";
            return false;
        }
        const bool r = s[i] == 'R';
        ++i;
        long count = 0;
        size_t digits = 0;
        while (i < s.size() && std::isdigit((unsigned char)s[i])) {
            count = count * 10 + (s[i] - '0');
            ++i;
            if (++digits > 6) {
                err = L"A frame count in the inputs is too large.";
                return false;
            }
        }
        if (!digits && !r) {
            err = L"Every letter must be followed by a frame count, e.g. d12.";
            return false;
        }
        total += r ? 1 : count;
        if (total > 200000) {
            err = L"The route is longer than 200,000 frames.";
            return false;
        }
    }
    out = DecodeInputs(s);
    return true;
}

// ------------------------------------------------------------------ app state
enum : int {
    ID_LEVEL = 100, ID_CHECKPOINT, ID_GLESS, ID_INPUTS, ID_LOAD, ID_OPEN, ID_PASTE, ID_FIRST, ID_BACK, ID_PLAY, ID_FWD, ID_LAST,
    ID_SPEED, ID_FOLLOW, ID_TRAIL, ID_SLIDER, ID_CANVAS, ID_STATUS, ID_EVENTS, ID_HELP, ID_TIMER = 1
};
static HWND g_wnd, g_level, g_cp, g_gless, g_inputs, g_load, g_open, g_paste, g_first, g_back, g_play, g_fwd, g_last, g_speed, g_follow,
    g_trail, g_slider, g_canvas, g_status, g_events, g_help, g_lblLevel, g_lblCp, g_lblInputs, g_lblSpeed;
static HFONT g_font;
static double g_dpi = 1.0;
static Timeline g_tl;
static std::unique_ptr<Sim> g_cur;
static int g_pos = 0;  // inputs applied to g_cur
static bool g_playing = false;
static double g_speedMul = 1.0;
static LARGE_INTEGER g_qpf, g_playStart;
static int g_playStartPos = 0;
static double g_userZoom = 1.0;
static bool g_follow_on = true, g_trail_on = true;
static double g_viewX = 0, g_viewY = 0;  // free view centre (world px) when not following
static bool g_dragging = false;
static POINT g_dragFrom;
static double g_dragViewX, g_dragViewY;
static std::wstring g_iniPath;

static int S(int v) { return (int)std::lround(v * g_dpi); }

static void Seek(int pos) {
    pos = std::max(0, std::min(pos, (int)g_tl.in.size()));
    const int k = pos / Timeline::K;
    g_cur.reset(new Sim(*g_tl.snaps[(size_t)k]));
    for (int i = k * Timeline::K; i < pos; ++i) Apply(*g_cur, g_tl.in[(size_t)i]);
    g_pos = pos;
}
static void StepForward() {
    if (g_pos >= (int)g_tl.in.size()) return;
    Apply(*g_cur, g_tl.in[(size_t)g_pos]);
    ++g_pos;
}

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

static void UpdateStatus();
static void UpdateControls();

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
    wchar_t b[64];
    std::swprintf(b, 64, L"%d", g_tl.level);
    WritePrivateProfileStringW(L"route", L"level", b, g_iniPath.c_str());
    std::swprintf(b, 64, L"%d", g_tl.checkpoint);
    WritePrivateProfileStringW(L"route", L"checkpoint", b, g_iniPath.c_str());
    WritePrivateProfileStringW(L"route", L"gless", g_tl.gless ? L"1" : L"0", g_iniPath.c_str());
    WritePrivateProfileStringW(L"route", L"inputs", GetText(g_inputs).c_str(), g_iniPath.c_str());
}

// Read the controls, simulate the route, rewind to frame 0.
static bool LoadRoute(bool showErrors) {
    std::vector<uint8_t> in;
    std::wstring err;
    if (!ParseRoute(Narrow(GetText(g_inputs)), in, err)) {
        if (showErrors) MessageBoxW(g_wnd, err.c_str(), L"Inputs", MB_ICONWARNING);
        return false;
    }
    g_tl.level = (int)SendMessageW(g_level, CB_GETCURSEL, 0, 0) + 1;
    g_tl.checkpoint = std::max(0, (int)SendMessageW(g_cp, CB_GETCURSEL, 0, 0));
    g_tl.gless = SendMessageW(g_gless, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_tl.in = in;
    BuildTimeline(g_tl);
    g_playing = false;
    Seek(0);
    g_userZoom = 1.0;
    SaveSettings();
    UpdateControls();
    InvalidateRect(g_canvas, nullptr, FALSE);
    return true;
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
    double z, cx, cy;  // zoom, view centre in world px
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

static void DrawLabel(gp::Graphics& g, const gp::PointF& p, const wchar_t* text, const gp::Color& c, float size) {
    gp::FontFamily ff(L"Segoe UI");
    gp::Font font(ff.IsAvailable() ? &ff : gp::FontFamily::GenericSansSerif(), size, gp::FontStyleBold, gp::UnitPixel);
    gp::SolidBrush br(c);
    g.DrawString(text, -1, &font, p, &br);
}

static void Paint(HDC hdc, int w, int h) {
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    {
        gp::Graphics g(mem);
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(gp::TextRenderingHintAntiAliasGridFit);
        // sky
        gp::LinearGradientBrush sky(gp::Point(0, 0), gp::Point(0, h), Rgb(170, 214, 245), Rgb(232, 244, 252));
        g.FillRectangle(&sky, 0, 0, w, h);
        if (!g_cur) {
            DrawLabel(g, gp::PointF(20, 20), L"Pick a level, paste a route and press Load.", Rgb(40, 40, 40), 18);
        } else {
            const Sim& s = *g_cur;
            const World& wd = s.world;
            View v;
            v.w = w, v.h = h;
            v.z = std::min(w / 550.0, h / 400.0) * g_userZoom;
            if (g_follow_on) {
                v.cx = -s.camX + 275, v.cy = -s.camY + 200;  // what the game's 550x400 view is centred on
                g_viewX = v.cx, g_viewY = v.cy;
            } else {
                v.cx = g_viewX, v.cy = g_viewY;
            }
            const float lw = (float)std::max(1.0, v.z);
            // bodies (not the ball)
            for (int32_t b = 0; b < wd.numBodies; ++b) {
                const Body& bd = wd.bodies[b];
                if (!bd.inWorld || b == s.playerBody) continue;
                const std::string name = s.BodyName(b);
                const bool kill = IsKillBody(g_tl.level, name);
                gp::Color fill = bd.IsStatic() ? Rgb(64, 78, 96) : Rgb(96, 140, 190);
                if (kill) fill = Rgb(176, 40, 40);
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
                        // a spoke so rotation is visible
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
            // spikes
            {
                gp::SolidBrush br(Rgb(200, 30, 30));
                for (int32_t i = 0; i < s.tpl->spikeCount; ++i) {
                    const SpikeObj& sp = s.tpl->spikes[i];
                    const double lx[3] = {0, 3, 6}, ly[3] = {0, -9.65, 0};
                    gp::PointF pts[3];
                    for (int k = 0; k < 3; ++k) pts[k] = v.P(sp.a * lx[k] + sp.c * ly[k] + sp.tx, sp.b * lx[k] + sp.d * ly[k] + sp.ty);
                    g.FillPolygon(&br, pts, 3);
                }
            }
            // checkpoints
            for (int i = 0; i < 5; ++i) {
                if (!s.tpl->cps[i]) continue;
                const bool taken = s.cpFrame[i] != 1;
                const gp::Color fc = Rgb(40, 200, 220, 110);
                DrawBox(g, v, *s.tpl->cps[i], Rgb(20, 140, 160), taken ? &fc : nullptr, lw * 1.5f);
                wchar_t b[8];
                std::swprintf(b, 8, L"%d", i);
                DrawLabel(g, v.P(s.tpl->cps[i]->x0 / 20.0, s.tpl->cps[i]->y0 / 20.0 - 14), b, Rgb(20, 120, 140), (float)std::max(10.0, 11 * v.z));
            }
            // switches and triggers
            for (const LevelDisplay& ld : kDisplayTable) {
                if (ld.id != g_tl.level) continue;
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
                    // which switchFrame slot this clip uses (redball.cpp SwitchClip / level scripts)
                    const int L = g_tl.level;
                    if (L == 9 || L == 16) idx = n.back() - '1';
                    else if (L == 5) idx = n == "blueCheck" ? 0 : 1;
                    else if (L == 7) idx = n == "redCheck" ? 0 : 1;
                    else if (L == 13 || L == 14 || L == 15) idx = 0;
                    const bool pressed = idx >= 0 && idx < 4 && s.switchFrame[idx] != 1;
                    const gp::Color fc(120, c.GetR(), c.GetG(), c.GetB());
                    DrawBox(g, v, o, c, pressed ? &fc : nullptr, lw * 1.5f);
                    if (n == "wrongWay") DrawLabel(g, v.P(o.x0 / 20.0, o.y0 / 20.0 - 14), L"wrong way", c, (float)std::max(10.0, 11 * v.z));
                }
            }
            // Level 15's sweeping kill line
            if (g_tl.level == 15) {
                if (const DisplayObj* kl = s.tpl->Display("killLine")) {
                    DisplayObj o = *kl;
                    const double dx = s.lvInt[1] - s.tpl->Place("killLine").tx;
                    o.x0 += dx, o.x1 += dx;
                    const gp::Color fc = Rgb(200, 30, 30, 160);
                    DrawBox(g, v, o, Rgb(160, 20, 20), &fc, lw);
                }
            }
            // flag
            if (s.tpl->aim) {
                const DisplayObj goal = const_cast<Sim&>(s).GoalTarget();
                const gp::Color fc = Rgb(255, 210, 40, s.isTimeStop ? 200 : 70);
                DrawBox(g, v, goal, Rgb(200, 150, 0), &fc, lw * 2);
                DrawLabel(g, v.P(goal.x0 / 20.0, goal.y0 / 20.0 - 16), L"FLAG", Rgb(160, 110, 0), (float)std::max(11.0, 12 * v.z));
            }
            // path of the ball
            if (g_trail_on && g_pos > 1) {
                std::vector<gp::PointF> pts;
                gp::Pen pen(Rgb(220, 30, 30, 120), (float)std::max(1.0, 1.5 * v.z));
                for (int i = 0; i <= g_pos; ++i) {
                    const FrameInfo& f = g_tl.info[(size_t)i];
                    if (i > 0 && g_tl.in[(size_t)i - 1] == IN_RESTART) {
                        if (pts.size() > 1) g.DrawLines(&pen, pts.data(), (INT)pts.size());
                        pts.clear();
                    }
                    pts.push_back(v.P(f.bx, f.by));
                }
                if (pts.size() > 1) g.DrawLines(&pen, pts.data(), (INT)pts.size());
            }
            // ball (its sprite: what the game shows)
            {
                const double bx = s.spriteX[s.playerBody], by = s.spriteY[s.playerBody];
                const gp::PointF c = v.P(bx, by);
                const float r = (float)(10.5 * v.z);
                const double ang = s.spriteRotW[s.playerBody] * AS3_PI / 180;
                if (s.playerAlive) {
                    gp::SolidBrush br(Rgb(230, 30, 30));
                    gp::Pen pen(Rgb(110, 0, 0), lw * 1.2f);
                    g.FillEllipse(&br, c.X - r, c.Y - r, 2 * r, 2 * r);
                    g.DrawEllipse(&pen, c.X - r, c.Y - r, 2 * r, 2 * r);
                    gp::SolidBrush eye(Rgb(255, 255, 255));
                    const float er = r * 0.28f;
                    const gp::PointF e(c.X + (gp::REAL)(std::cos(ang) * r * 0.45), c.Y + (gp::REAL)(std::sin(ang) * r * 0.45));
                    g.FillEllipse(&eye, e.X - er, e.Y - er, 2 * er, 2 * er);
                } else {
                    // Death-warp ghost: after death the frozen ball is tested against targets shifted by the camera
                    // (Level.x/y), which is the same as testing a ghost of the ball shifted by -camera.
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
            // game view frame (the 550x400 the player sees), when zoomed out
            if (g_follow_on && g_userZoom < 0.999) {
                gp::Pen pen(Rgb(0, 0, 0, 90), 1);
                const gp::PointF a = v.P(v.cx - 275, v.cy - 200), b = v.P(v.cx + 275, v.cy + 200);
                g.DrawRectangle(&pen, a.X, a.Y, b.X - a.X, b.Y - a.Y);
            }
            // overlay
            wchar_t b[256];
            const FrameInfo& f = g_tl.info[(size_t)g_pos];
            const int shownFrame = s.isTimeStop ? s.winFrame : f.frame;  // Game.frameCount stops one short on the flag frame
            std::swprintf(b, 256, L"Level %d: %ls    frame %d  (%.2f s)", g_tl.level, kLevelNames[g_tl.level], shownFrame, shownFrame / 31.0);
            {
                gp::SolidBrush back(Rgb(255, 255, 255, 170));
                g.FillRectangle(&back, 4, 4, 520, (s.isTimeStop || !s.playerAlive) ? 54 : 30);
            }
            DrawLabel(g, gp::PointF(10, 8), b, Rgb(20, 20, 30), 16);
            if (s.isTimeStop) {
                std::swprintf(b, 256, L"FLAG  frame %d  (%.2f s)%ls", s.winFrame, s.winFrame / 31.0,
                              s.deathFrame >= 0 ? (g_tl.warpValid ? L"  death warp" : L"  death warp, too late for a real run") : L"");
                DrawLabel(g, gp::PointF(10, 30), b, Rgb(170, 110, 0), 20);
            } else if (!s.playerAlive) {
                std::swprintf(b, 256, L"DEAD  (frame %d)", s.deathFrame);
                DrawLabel(g, gp::PointF(10, 30), b, Rgb(190, 0, 0), 20);
            }
        }
    }
    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

// ------------------------------------------------------------------ controls
static const wchar_t* InputName(uint8_t c) {
    static const wchar_t* n[9] = {L"none", L"right", L"up", L"up+right", L"left", L"left+right", L"left+up", L"left+up+right", L"R (restart)"};
    return c < 9 ? n[c] : L"?";
}

static void UpdateStatus() {
    if (!g_cur) {
        SetWindowTextW(g_status, L"");
        return;
    }
    const Sim& s = *g_cur;
    const Body& b = s.world.bodies[s.playerBody];
    wchar_t buf[512];
    std::swprintf(buf, 512, L"  input %d / %d   next: %ls   |   ball x %.2f  y %.2f px   vx %.3f  vy %.3f   |   %ls%ls", g_pos,
                  (int)g_tl.in.size(), g_pos < (int)g_tl.in.size() ? InputName(g_tl.in[(size_t)g_pos]) : L"end", s.spriteX[s.playerBody],
                  s.spriteY[s.playerBody], b.linearVelocity.x, b.linearVelocity.y, s.playerAlive ? L"alive" : L"dead",
                  s.displayUncertain ? L"   |   a hit test was within a rounding unit: check in Flash" : L"");
    SetWindowTextW(g_status, buf);
    SendMessageW(g_slider, TBM_SETPOS, TRUE, g_pos);
    SetWindowTextW(g_play, g_playing ? L"Pause" : L"Play");
}

static void UpdateControls() {
    SendMessageW(g_slider, TBM_SETRANGEMIN, FALSE, 0);
    SendMessageW(g_slider, TBM_SETRANGEMAX, TRUE, (LPARAM)g_tl.in.size());
    // events list
    SendMessageW(g_events, LB_RESETCONTENT, 0, 0);
    if (g_tl.events.empty()) SendMessageW(g_events, LB_ADDSTRING, 0, (LPARAM)L"(no deaths, checkpoints or flag in this route)");
    for (const Event& e : g_tl.events) {
        const std::wstring line = L"[" + std::to_wstring(e.index) + L"]  " + e.text;
        SendMessageW(g_events, LB_ADDSTRING, 0, (LPARAM)line.c_str());
    }
    UpdateStatus();
}

static void Layout() {
    RECT rc;
    GetClientRect(g_wnd, &rc);
    const int W = rc.right, H = rc.bottom, pad = S(8), rowH = S(26), lblW = S(44);
    int x = pad, y = pad;
    MoveWindow(g_lblLevel, x, y + S(5), lblW, rowH, TRUE);
    x += lblW;
    MoveWindow(g_level, x, y, S(210), S(400), TRUE);
    x += S(218);
    MoveWindow(g_lblCp, x, y + S(5), S(72), rowH, TRUE);
    x += S(72);
    MoveWindow(g_cp, x, y, S(130), S(300), TRUE);
    x += S(140);
    MoveWindow(g_gless, x, y + S(3), S(100), S(22), TRUE);
    x += S(108);
    MoveWindow(g_help, W - pad - S(60), y, S(60), rowH, TRUE);
    y += rowH + S(6);
    x = pad;
    MoveWindow(g_lblInputs, x, y + S(5), lblW, rowH, TRUE);
    x += lblW;
    const int btnW = S(70);
    const int editW = std::max(S(120), W - x - pad - 3 * (btnW + S(6)));
    MoveWindow(g_inputs, x, y, editW, rowH, TRUE);
    x += editW + S(6);
    MoveWindow(g_paste, x, y, btnW, rowH, TRUE);
    x += btnW + S(6);
    MoveWindow(g_open, x, y, btnW, rowH, TRUE);
    x += btnW + S(6);
    MoveWindow(g_load, x, y, btnW, rowH, TRUE);
    y += rowH + S(6);
    x = pad;
    const int b = S(40);
    MoveWindow(g_first, x, y, b, rowH, TRUE);
    x += b + S(4);
    MoveWindow(g_back, x, y, b, rowH, TRUE);
    x += b + S(4);
    MoveWindow(g_play, x, y, S(70), rowH, TRUE);
    x += S(74);
    MoveWindow(g_fwd, x, y, b, rowH, TRUE);
    x += b + S(4);
    MoveWindow(g_last, x, y, b, rowH, TRUE);
    x += b + S(10);
    MoveWindow(g_lblSpeed, x, y + S(5), S(42), rowH, TRUE);
    x += S(42);
    MoveWindow(g_speed, x, y, S(80), S(300), TRUE);
    x += S(88);
    MoveWindow(g_follow, x, y + S(3), S(118), S(22), TRUE);
    x += S(122);
    MoveWindow(g_trail, x, y + S(3), S(90), S(22), TRUE);
    x += S(96);
    MoveWindow(g_slider, x, y, std::max(S(80), W - x - pad), rowH, TRUE);
    y += rowH + S(6);
    const int statusH = S(24), eventsW = S(330);
    MoveWindow(g_canvas, pad, y, W - 2 * pad - eventsW - S(6), H - y - statusH - pad, TRUE);
    MoveWindow(g_events, W - pad - eventsW, y, eventsW, H - y - statusH - pad, TRUE);
    MoveWindow(g_status, 0, H - statusH, W, statusH, TRUE);
}

static void SetPlaying(bool on) {
    g_playing = on && g_pos < (int)g_tl.in.size();
    QueryPerformanceCounter(&g_playStart);
    g_playStartPos = g_pos;
    UpdateStatus();
}

static void LoadRouteFile(const wchar_t* file) {
    FILE* f = _wfopen(file, L"rb");
    if (!f) {
        MessageBoxW(g_wnd, L"Cannot open that file.", L"Open", MB_ICONWARNING);
        return;
    }
    std::string text;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0 && text.size() < (8u << 20)) text.append(buf, n);
    fclose(f);
    // A stats log from the mod: "# inputs<TAB>RLE" and "LEVEL <id> <checkpoint>". Otherwise: the first non-empty line.
    // The header's input string is the file's last recording; the first LEVEL line of that recording is the one after
    // the last line that starts a new recording (a LEVEL line not preceded by an R). isGless is detected like
    // rbsim verify does: with it on, Level.dp (trailing block) is always 0 while the camera (Level.x/y) moves.
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
        } else if (line.rfind("LEVEL ", 0) == 0) {
            if (!prevR) {  // a new recording starts here
                std::sscanf(line.c_str() + 6, "%d %d", &level, &cp);
                camMoved = dpNonzero = false;
                lastCam.clear();
            }
            prevR = false;
        } else if (line == "R") {
            prevR = true;
        } else if (isLog && !line.empty() && std::isdigit((unsigned char)line[0])) {
            std::vector<std::string> f;
            size_t q = 0;
            while (q <= line.size()) {
                size_t t = line.find('\t', q);
                if (t == std::string::npos) t = line.size();
                f.push_back(line.substr(q, t - q));
                q = t + 1;
            }
            if (f.size() >= 29 && (f.size() - 21) % 7 == 1) {  // v2 trailing block: a b Lx Ly dp0 dp1 lastCheckNum flags
                const size_t c = f.size() - 6;
                const std::string cam = f[c] + f[c + 1];
                if (!lastCam.empty() && cam != lastCam) camMoved = true;
                lastCam = cam;
                for (size_t k = c + 2; k < c + 4; ++k)
                    if (f[k] != "0000000000000000" && f[k] != "8000000000000000" && f[k].rfind("7ff", 0) != 0) dpNonzero = true;
            }
        } else if (!isLog && route.empty() && !line.empty() && line[0] != '#') {
            route = line;
            break;
        }
    }
    if (level >= 1 && level <= NUM_LEVELS) {
        SendMessageW(g_level, CB_SETCURSEL, (WPARAM)level - 1, 0);
        FillCheckpoints(level, cp);
    }
    if (isLog) SendMessageW(g_gless, BM_SETCHECK, (camMoved && !dpNonzero) ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowTextW(g_inputs, Widen(route).c_str());
    LoadRoute(true);
}

static void OpenRouteFile() {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW of = {};
    of.lStructSize = sizeof of;
    of.hwndOwner = g_wnd;
    of.lpstrFilter = L"Routes and logs (*.txt;*.tsv)\0*.txt;*.tsv\0All files\0*.*\0";
    of.lpstrFile = file;
    of.nMaxFile = MAX_PATH;
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&of)) LoadRouteFile(file);
}

static void PasteRoute() {
    if (!OpenClipboard(g_wnd)) return;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t* t = (const wchar_t*)GlobalLock(h);
        if (t) {
            std::wstring s(t);
            GlobalUnlock(h);
            // accept "L4: d12e1..." style pastes: keep the part after the last ':' if it looks like a route
            const size_t c = s.rfind(L':');
            if (c != std::wstring::npos) s = s.substr(c + 1);
            SetWindowTextW(g_inputs, s.c_str());
        }
    }
    CloseClipboard();
    LoadRoute(true);
}

static const wchar_t* kHelp =
    L"Red Ball route viewer\n\n"
    L"1. Pick the level (and the checkpoint the route starts from).\n"
    L"2. Paste the route's input string (the game's TAS format, e.g. d12e1w3n5),\n"
    L"   or Open (or drag onto the window) a .txt with the route or a stats log (.tsv).\n"
    L"3. Load, then Play.\n\n"
    L"Keys (when the input box is not focused): Space play/pause, Left/Right one frame,\n"
    L"Home/End start/end, +/- zoom. Mouse: wheel zooms, drag pans (turns off Follow camera).\n\n"
    L"Inputs: n none, d right, w up, e up+right, a left, S left+right, q left+up, W all three,\n"
    L"each followed by a frame count; R = restart from the checkpoint.\n"
    L"Glitchless: the spike glitch off (Game.isGless).\n\n"
    L"Everything shown is computed by the bit-exact simulator (rbsim): positions, deaths and\n"
    L"the flag frame match Flash Player 11.4 with the Practice Hack (mathspikes 1).\n"
    L"After a death the purple dashed \"warp ghost\" shows where the game tests the dead ball\n"
    L"against the flag, checkpoints and switches (shifted by the camera): when it touches the\n"
    L"flag, that is a death warp.\n"
    L"Not shown: the 8 random debris pieces after a death.";

// ------------------------------------------------------------------ canvas window
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
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEWHEEL: {
            const int d = GET_WHEEL_DELTA_WPARAM(wp);
            g_userZoom = std::max(0.1, std::min(12.0, g_userZoom * (d > 0 ? 1.2 : 1 / 1.2)));
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
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
                if (std::abs(dx) + std::abs(dy) > 2 && g_follow_on) {
                    g_follow_on = false;
                    SendMessageW(g_follow, BM_SETCHECK, BST_UNCHECKED, 0);
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
    }
    return DefWindowProcW(h, m, wp, lp);
}

// ------------------------------------------------------------------ main window
static HWND MakeCtl(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g_wnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    return h;
}

static void HandleKey(WPARAM vk) {
    switch (vk) {
        case VK_SPACE: SetPlaying(!g_playing); break;
        case VK_RIGHT: g_playing = false; StepForward(); break;
        case VK_LEFT: g_playing = false; Seek(g_pos - 1); break;
        case VK_HOME: g_playing = false; Seek(0); break;
        case VK_END: g_playing = false; Seek((int)g_tl.in.size()); break;
        case VK_OEM_PLUS: case VK_ADD: g_userZoom = std::min(12.0, g_userZoom * 1.2); break;
        case VK_OEM_MINUS: case VK_SUBTRACT: g_userZoom = std::max(0.1, g_userZoom / 1.2); break;
        default: return;
    }
    UpdateStatus();
    InvalidateRect(g_canvas, nullptr, FALSE);
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
            g_lblLevel = MakeCtl(L"STATIC", L"Level", 0, 0);
            g_level = MakeCtl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_LEVEL);
            for (int i = 1; i <= NUM_LEVELS; ++i) {
                wchar_t b[64];
                std::swprintf(b, 64, L"%d: %ls", i, kLevelNames[i]);
                SendMessageW(g_level, CB_ADDSTRING, 0, (LPARAM)b);
            }
            g_lblCp = MakeCtl(L"STATIC", L"Start from", 0, 0);
            g_cp = MakeCtl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_CHECKPOINT);
            g_gless = MakeCtl(L"BUTTON", L"Glitchless", BS_AUTOCHECKBOX | WS_TABSTOP, ID_GLESS);
            g_help = MakeCtl(L"BUTTON", L"Help", BS_PUSHBUTTON | WS_TABSTOP, ID_HELP);
            g_lblInputs = MakeCtl(L"STATIC", L"Route", 0, 0);
            g_inputs = MakeCtl(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, ID_INPUTS, WS_EX_CLIENTEDGE);
            g_paste = MakeCtl(L"BUTTON", L"Paste", BS_PUSHBUTTON | WS_TABSTOP, ID_PASTE);
            g_open = MakeCtl(L"BUTTON", L"Open...", BS_PUSHBUTTON | WS_TABSTOP, ID_OPEN);
            g_load = MakeCtl(L"BUTTON", L"Load", BS_DEFPUSHBUTTON | WS_TABSTOP, ID_LOAD);
            g_first = MakeCtl(L"BUTTON", L"|<", BS_PUSHBUTTON, ID_FIRST);
            g_back = MakeCtl(L"BUTTON", L"<", BS_PUSHBUTTON, ID_BACK);
            g_play = MakeCtl(L"BUTTON", L"Play", BS_PUSHBUTTON, ID_PLAY);
            g_fwd = MakeCtl(L"BUTTON", L">", BS_PUSHBUTTON, ID_FWD);
            g_last = MakeCtl(L"BUTTON", L">|", BS_PUSHBUTTON, ID_LAST);
            g_lblSpeed = MakeCtl(L"STATIC", L"Speed", 0, 0);
            g_speed = MakeCtl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, ID_SPEED);
            for (const wchar_t* s : {L"0.1x", L"0.25x", L"0.5x", L"1x (31 fps)", L"2x", L"4x", L"8x"}) SendMessageW(g_speed, CB_ADDSTRING, 0, (LPARAM)s);
            SendMessageW(g_speed, CB_SETCURSEL, 3, 0);
            g_follow = MakeCtl(L"BUTTON", L"Follow camera", BS_AUTOCHECKBOX, ID_FOLLOW);
            SendMessageW(g_follow, BM_SETCHECK, BST_CHECKED, 0);
            g_trail = MakeCtl(L"BUTTON", L"Show path", BS_AUTOCHECKBOX, ID_TRAIL);
            SendMessageW(g_trail, BM_SETCHECK, BST_CHECKED, 0);
            g_slider = MakeCtl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS, ID_SLIDER);
            g_canvas = CreateWindowExW(WS_EX_CLIENTEDGE, L"RbCanvas", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, h, (HMENU)ID_CANVAS,
                                       GetModuleHandleW(nullptr), nullptr);
            g_events = MakeCtl(L"LISTBOX", L"", WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY, ID_EVENTS, WS_EX_CLIENTEDGE);
            g_status = MakeCtl(L"STATIC", L"", SS_LEFTNOWORDWRAP | SS_CENTERIMAGE, ID_STATUS);
            // settings next to the exe (portable)
            wchar_t exe[MAX_PATH];
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            g_iniPath = exe;
            const size_t dot = g_iniPath.rfind(L'.');
            g_iniPath = (dot == std::wstring::npos ? g_iniPath : g_iniPath.substr(0, dot)) + L".ini";
            const int level = std::max(1, std::min(NUM_LEVELS, (int)GetPrivateProfileIntW(L"route", L"level", 4, g_iniPath.c_str())));
            const int cp = (int)GetPrivateProfileIntW(L"route", L"checkpoint", 0, g_iniPath.c_str());
            const bool gl = GetPrivateProfileIntW(L"route", L"gless", 0, g_iniPath.c_str()) != 0;
            wchar_t inputs[32768] = L"";
            GetPrivateProfileStringW(L"route", L"inputs", L"d12e1w3a2n5a2n31d8n78a1n1e1n13w2n1w1e1w12n9w1n43w6n1w3n9w2n2w1n2w17n3", inputs, 32768,
                                     g_iniPath.c_str());
            SendMessageW(g_level, CB_SETCURSEL, (WPARAM)level - 1, 0);
            FillCheckpoints(level, cp);
            SendMessageW(g_gless, BM_SETCHECK, gl ? BST_CHECKED : BST_UNCHECKED, 0);
            SetWindowTextW(g_inputs, inputs);
            Layout();
            LoadRoute(false);
            QueryPerformanceFrequency(&g_qpf);
            SetTimer(h, ID_TIMER, 10, nullptr);
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
            InvalidateRect(g_canvas, nullptr, FALSE);
            return 0;
        case WM_GETMINMAXINFO: {
            MINMAXINFO* mm = (MINMAXINFO*)lp;
            mm->ptMinTrackSize = {S(900), S(560)};
            return 0;
        }
        case WM_TIMER:
            if (g_playing) {
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                const double sec = double(now.QuadPart - g_playStart.QuadPart) / double(g_qpf.QuadPart);
                const int target = g_playStartPos + (int)(sec * 31.0 * g_speedMul);
                int steps = 0;
                while (g_pos < target && g_pos < (int)g_tl.in.size() && steps < 64) StepForward(), ++steps;
                if (g_pos >= (int)g_tl.in.size()) g_playing = false;
                if (steps) {
                    UpdateStatus();
                    InvalidateRect(g_canvas, nullptr, FALSE);
                }
            }
            return 0;
        case WM_HSCROLL:
            if ((HWND)lp == g_slider) {
                g_playing = false;
                Seek((int)SendMessageW(g_slider, TBM_GETPOS, 0, 0));
                UpdateStatus();
                InvalidateRect(g_canvas, nullptr, FALSE);
            }
            return 0;
        case WM_COMMAND: {
            const int id = LOWORD(wp), code = HIWORD(wp);
            switch (id) {
                case ID_LEVEL:
                    if (code == CBN_SELCHANGE) {
                        FillCheckpoints((int)SendMessageW(g_level, CB_GETCURSEL, 0, 0) + 1, 0);
                        LoadRoute(false);
                    }
                    break;
                case ID_CHECKPOINT:
                    if (code == CBN_SELCHANGE) LoadRoute(false);
                    break;
                case ID_GLESS:
                    LoadRoute(false);
                    break;
                case ID_LOAD:
                    LoadRoute(true);
                    break;
                case ID_OPEN:
                    OpenRouteFile();
                    break;
                case ID_PASTE:
                    PasteRoute();
                    break;
                case ID_PLAY:
                    if (!g_playing && g_pos >= (int)g_tl.in.size()) Seek(0);
                    SetPlaying(!g_playing);
                    break;
                case ID_FIRST: HandleKey(VK_HOME); break;
                case ID_BACK: HandleKey(VK_LEFT); break;
                case ID_FWD: HandleKey(VK_RIGHT); break;
                case ID_LAST: HandleKey(VK_END); break;
                case ID_SPEED:
                    if (code == CBN_SELCHANGE) {
                        static const double sp[7] = {0.1, 0.25, 0.5, 1, 2, 4, 8};
                        g_speedMul = sp[std::max(0, std::min(6, (int)SendMessageW(g_speed, CB_GETCURSEL, 0, 0)))];
                        SetPlaying(g_playing);
                    }
                    break;
                case ID_FOLLOW:
                    g_follow_on = SendMessageW(g_follow, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    break;
                case ID_TRAIL:
                    g_trail_on = SendMessageW(g_trail, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    break;
                case ID_EVENTS:
                    if (code == LBN_DBLCLK || code == LBN_SELCHANGE) {
                        const int sel = (int)SendMessageW(g_events, LB_GETCURSEL, 0, 0);
                        if (sel >= 0 && sel < (int)g_tl.events.size()) {
                            g_playing = false;
                            Seek(g_tl.events[(size_t)sel].index);
                        }
                    }
                    break;
                case ID_HELP:
                    MessageBoxW(h, kHelp, L"Help", MB_ICONINFORMATION);
                    break;
                default:
                    return DefWindowProcW(h, m, wp, lp);
            }
            if (id != ID_INPUTS) SetFocus(g_canvas);
            UpdateStatus();
            InvalidateRect(g_canvas, nullptr, FALSE);
            return 0;
        }
        case WM_DESTROY:
            SaveSettings();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
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

    HDC dc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSX) / 96.0;
    ReleaseDC(nullptr, dc);
    HWND w = CreateWindowExW(0, L"RbViewer", L"Red Ball route viewer", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, S(1280), S(820), nullptr,
                             nullptr, inst, nullptr);
    ShowWindow(w, show);
    UpdateWindow(w);
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; argv && i < argc; ++i) {
            if (!wcscmp(argv[i], L"--frame") && i + 1 < argc) {
                Seek(_wtoi(argv[++i]));
                UpdateStatus();
                InvalidateRect(g_canvas, nullptr, FALSE);
            } else {
                LoadRouteFile(argv[i]);
            }
        }
        if (argv) LocalFree(argv);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && GetFocus() != g_inputs) {
            const WPARAM k = msg.wParam;
            if (k == VK_SPACE || k == VK_LEFT || k == VK_RIGHT || k == VK_HOME || k == VK_END || k == VK_OEM_PLUS || k == VK_OEM_MINUS ||
                k == VK_ADD || k == VK_SUBTRACT) {
                HWND f = GetFocus();
                wchar_t cls[32] = L"";
                if (f) GetClassNameW(f, cls, 32);
                if (_wcsicmp(cls, L"ComboBox") != 0 && _wcsicmp(cls, L"msctls_trackbar32") != 0) {
                    HandleKey(k);
                    continue;
                }
            }
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && GetFocus() == g_inputs) {
            LoadRoute(true);
            SetFocus(g_canvas);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    gp::GdiplusShutdown(token);
    return 0;
}
