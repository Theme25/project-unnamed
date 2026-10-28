#include "../src/search_offset.h"
#include <cstdio>
#include <cstdlib>
int main() {
    rb::SearchOffset p;
    auto check=[](bool good){if(!good) std::exit(1);};
    check(p.Parse(std::string("50")) && p.Index(4611686018427387903ULL)==2305843009213693951ULL);
    check(p.Parse(std::string("100")) && p.Index(4611686018427387903ULL)==4611686018427387903ULL);
    check(p.Parse(std::wstring(L"0.000000001%")) && p.Index(4611686018427387903ULL)==46116860ULL);
    check(p.Parse(std::string("99.999999999")) && p.Index(4611686018427387903ULL)==4611686018381271042ULL);
    check(p.Parse(std::string("37.25")) && p.Index(243)==90);
    std::puts("PASS exact percentage arithmetic at the 62-bit domain boundary");
}
