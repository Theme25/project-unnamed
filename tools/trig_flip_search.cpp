// trig_flip_search: diagnostic that explains sim/Flash divergences by
// searching for +-1 ulp overrides of sin/cos results (Flash's libm differs
// from glibc by 1 ulp in ~3.5% of inputs). Greedy: at each divergence it
// picks the single flip (among all angles used so far) that pushes the
// first mismatch furthest.
//   usage: trig_flip_search <rb1_stats.tsv> <LEVEL-line-number> [horizon]
#include "redball.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>
using namespace rb;
static uint64_t B(double d){uint64_t u;memcpy(&u,&d,8);return u;}
static double H(const std::string&h){uint64_t u=std::stoull(h,nullptr,16);double d;memcpy(&d,&u,8);return d;}
static std::map<uint64_t,int> sinD, cosD; static std::set<uint64_t> seenS, seenC; static bool rec=false;
static double stepU(double v,int u){ if(!u) return v; int64_t b; memcpy(&b,&v,8); b+=u; double r; memcpy(&r,&b,8); return r; }
static double hs(double a){ if(rec) seenS.insert(B(a)); auto it=sinD.find(B(a)); return stepU(std::sin(a), it==sinD.end()?0:it->second); }
static double hc(double a){ if(rec) seenC.insert(B(a)); auto it=cosD.find(B(a)); return stepU(std::cos(a), it==cosD.end()?0:it->second); }
static std::vector<std::string> split(const std::string&s){std::vector<std::string> o;size_t a=0;for(;;){size_t b=s.find('\t',a);o.push_back(s.substr(a,b==std::string::npos?std::string::npos:b-a));if(b==std::string::npos)break;a=b+1;}return o;}
static std::vector<std::vector<std::string>> rows; static LevelTemplate* T;
static bool match(const Sim& s, const std::vector<std::string>& f){ const Body& b=s.world.bodies[s.playerBody];
  return B(b.xf.position.x)==B(H(f[3]))&&B(b.xf.position.y)==B(H(f[4]))&&B(b.linearVelocity.x)==B(H(f[5]))&&B(b.linearVelocity.y)==B(H(f[6]))&&B(b.sweep.a)==B(H(f[7]))&&B(b.angularVelocity)==B(H(f[8])); }
// replay; returns first mismatching tick index (or rows.size())
static size_t run(size_t upto, bool record){ static std::unique_ptr<Sim> s(new Sim); s->Load(T); rec=record;
  for(size_t i=1;i<=upto && i<rows.size();++i){ s->Tick((uint8_t)std::stoi(rows[i][2])); if(!match(*s,rows[i])){rec=false;return i;} if(rows[i][20]=="1"||!s->playerAlive){rec=false;return rows.size();} }
  rec=false; return upto+1; }
int main(int argc,char**argv){
  if(argc<3){fprintf(stderr,"usage: %s <log.tsv> <LEVEL line> [horizon]\n",argv[0]);return 2;}
  int segLine=atoi(argv[2]); size_t horizon=argc>3?atoi(argv[3]):3000;
  std::ifstream in(argv[1]); std::string l; int ln=0; bool on=false;
  while(std::getline(in,l)){++ln; if(ln==segLine){on=true;continue;} if(!on) continue; if(l.rfind("LEVEL",0)==0) break; if(l.empty()||l=="R") continue; rows.push_back(split(l));}
  g_sinHook=hs; g_cosHook=hc; LevelTemplate tpl(1); T=&tpl;
  horizon=std::min(horizon,rows.size()-1);
  int iter=0;
  for(;;){
    size_t bad=run(horizon,false);
    if(bad>horizon){ printf("segment %d: ALL %zu ticks match with %zu sin + %zu cos overrides\n",segLine,horizon,sinD.size(),cosD.size()); return 0; }
    seenS.clear(); seenC.clear(); run(bad,true);
    std::vector<std::pair<int,uint64_t>> cands; for(auto a:seenS) if(!sinD.count(a)) cands.push_back({0,a}); for(auto a:seenC) if(!cosD.count(a)) cands.push_back({1,a});
    // score single flips by how far they push the first mismatch
    size_t best=bad; int bi=-1, bd=0;
    for(size_t c=0;c<cands.size();++c) for(int d:{1,-1}){ auto&M=cands[c].first?cosD:sinD; M[cands[c].second]=d; size_t r=run(horizon,false); M.erase(cands[c].second); if(r>best){best=r;bi=(int)c;bd=d;} }
    if(bi<0){ printf("tick %s: no single flip among %zu candidate angles (ticks 1..%zu) advances the match\n", rows[bad][0].c_str(), cands.size(), bad); return 1; }
    auto&M=cands[bi].first?cosD:sinD; M[cands[bi].second]=bd; double a; memcpy(&a,&cands[bi].second,8);
    printf("divergence at tick %-5s -> flip %s(%.17g) %+d ulp  => now matches through tick %s\n", rows[bad][0].c_str(), cands[bi].first?"cos":"sin", a, bd, best>horizon? "END": rows[best][0].c_str());
    if(++iter>200) return 1;
  }
}
