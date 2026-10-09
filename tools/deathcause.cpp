// tools/deathcause.cpp: replay inputs on a level and report how each death or the win happened
// (contacts at death, or the spike row that killed). Build: make deathcause. Usage: deathcause <level> <RLE> [checkpoint] [gless 0|1]
#include "../src/redball.h"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
using namespace rb;
static std::string g_contacts;
static void OnDie(const Sim& s) {
    g_contacts.clear();
    for (int i = 0; i < s.world.listener.count; i++) g_contacts += " " + s.BodyName(s.world.listener.bodies[i]);
    if (s.spriteY[s.playerBody] > 0 && g_contacts.empty()) g_contacts = " (none)";
}
int main(int argc, char** argv) {
    g_playerDieHook = OnDie;
    int lv = atoi(argv[1]);
    LevelTemplate t(lv);
    auto s = std::make_unique<Sim>();
    s->Load(&t, argc > 3 ? atoi(argv[3]) : 0);
    if (argc > 4) s->gless = atoi(argv[4]) != 0;  // Game.isGless (camera steps after the spike test)
    auto in = DecodeInputs(argv[2]);
    for (auto k : in) {
        bool alive = s->playerAlive;
        if (k == IN_RESTART) {
            std::printf("R at frame %d (lastCheckNum %d, isStrelka %d)\n", s->frameCount, s->lastCheckNum, (int)s->staticFlag[2]);
            s->Restart();
            continue;
        }
        s->Tick(k);
        if (alive && !s->playerAlive) {
            double x = s->spriteX[s->playerBody], y = s->spriteY[s->playerBody];
            int sp = -1;
            for (int i = 0; i < t.spikeCount; i++) {
                auto r = BallHitsSpike(x, y, s->spriteRotW[s->playerBody], s->dpX, s->dpY, t.spikes[i]);
                if (r.hit) { sp = i; break; }
            }
            std::printf("death at frame %d, sprite (%.2f, %.2f), spike index %d", s->deathFrame, x, y, sp);
            if (sp >= 0) {
                auto& S = t.spikes[sp];
                std::printf(" (matrix a=%.4f b=%.4f c=%.4f d=%.4f at %.2f,%.2f)", S.a, S.b, S.c, S.d, S.tx, S.ty);
            }
            std::printf(", contacts at death:%s\n", g_contacts.c_str());
        }
        if (s->isTimeStop) { std::printf("win at frame %d\n", s->winFrame); break; }
    }
    std::printf("end: frame %d lastCheckNum %d uncertain %d\n", s->frameCount, s->lastCheckNum, s->displayUncertain);
}
