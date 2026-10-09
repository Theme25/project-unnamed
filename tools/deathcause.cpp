// tools/deathcause.cpp: replay inputs on a level and report how each death or the win happened
// (contacts at death, or the spike row that killed). Build: make deathcause. Usage: deathcause <level> <RLE> [checkpoint]
#include "../src/redball.h"
#include <cstdio>
#include <cstdlib>
#include <memory>
using namespace rb;
int main(int argc, char** argv) {
    int lv = atoi(argv[1]);
    LevelTemplate t(lv);
    auto s = std::make_unique<Sim>();
    s->Load(&t, argc > 3 ? atoi(argv[3]) : 0);
    auto in = DecodeInputs(argv[2]);
    for (auto k : in) {
        bool alive = s->playerAlive;
        if (k == IN_RESTART) { s->Restart(); continue; }
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
            std::printf(", contacts:");
            for (int i = 0; i < s->world.listener.count; i++) std::printf(" %s", s->BodyName(s->world.listener.bodies[i]).c_str());
            std::printf("\n");
        }
        if (s->isTimeStop) { std::printf("win at frame %d\n", s->winFrame); break; }
    }
    std::printf("end: frame %d lastCheckNum %d uncertain %d\n", s->frameCount, s->lastCheckNum, s->displayUncertain);
}
