// Build with the accepted engine and strict floating-point flags. This includes
// Smartforce to check state isolation and generation without a UI dependency.
#include "../src/smartforce.cpp"
#include <iostream>

static void Require(bool yes, const char* message) {
    if (!yes) throw std::runtime_error(message);
}

// Independent transcription of the pre-refactor BruteEnv implementation.
static bool Original(int frames, double vy, int up, int offset, Seq& out) {
    const double pi = 3.141592653589793;
    const int x = frames - 1;
    if (up > std::ceil(vy / (40 / (147 * pi) - 1.0 / 3)) || up > x) return false;
    const double reach = (x - up + (up + 1) / 2.0) * up;
    if (reach < offset) return false;
    const int possibleup = (int)std::min(std::ceil(-vy * 3 + (up - 1) * 40 / (49 * pi)), (double)x);
    if ((x - possibleup + (up + 1) / 2.0) * up > offset) return false;
    auto asInt = [](double v) { return std::isfinite(v) ? (int)std::trunc(v) : 0; };
    const int middle = asInt(std::fmod(reach - offset, (double)(possibleup - up)));
    const int end = asInt((reach - offset) / (double)(possibleup - up));
    Seq jump;
    int i = 0;
    while (i < up - end - 1) jump.push_back(2), ++i;
    while (i < possibleup - end) jump.push_back(i == up - end - 1 + middle ? 2 : 0), ++i;
    for (i = 0; i < end; ++i) jump.push_back(2);
    jump.resize((size_t)frames, 0);
    out = jump;
    return true;
}

int main() {
    try {
        uint64_t signatures = 0, states = 0;
        std::vector<double> speeds{-50, -10, -8.9235480089, -5, -1.2637918858328288, -1, -.5, -.01, 0, .01, 1};
        for (int k = 1; k <= 12; ++k) {
            const double boundary = k * (40 / (147 * JumpBrute::PI) - 1.0 / 3);
            speeds.push_back(boundary);
            speeds.push_back(std::nextafter(boundary, -INFINITY));
            speeds.push_back(std::nextafter(boundary, INFINITY));
        }
        for (int frames = 1; frames <= 26; ++frames) for (double vy : speeds) {
            const JumpBrute model(frames, vy);
            for (int up = 0; up <= frames; ++up) for (int sum = 0; sum <= frames * (frames - 1) / 2 + 1; ++sum) {
                Seq old, actual((size_t)frames, 0);
                const bool allowed = Original(frames, vy, up, sum, old);
                Require(allowed == model.Allows(up, (uint64_t)sum), "built-in eligibility changed");
                if (allowed) {
                    model.Place(up, (uint64_t)sum, [&](int at) { Require(at >= 0 && at < frames, "placement out of range"); actual[(size_t)at] = 2; });
                    Require(actual == old, "built-in placement changed");
                }
                ++signatures;
            }
        }

        // A long marked interval must not add ineffective Up variants once the
        // maximum rising window (including holds) has ended. Its fixed tail stays.
        Options shortJump, longJump;
        for (int frames : {13, 100}) {
            double bytes;
            std::string error;
            std::vector<JumpSummary> summaries;
            auto parts = ParsePattern("<d" + std::to_string(frames) + "+b" + std::to_string(frames) + ">",
                                      1e8, bytes, error, summaries);
            Require(error.empty() && parts.size() == 1, "long jump test pattern");
            const JumpBrute model(frames, -2.8);
            Require(model.MaxCount() == 12 && model.Window(12) == 12, "twelve-frame test rising window");
            Options& all = frames == 13 ? shortJump : longJump;
            BruteRows(*parts[0].node, frames, -2.8, [&](int k, uint64_t low, uint64_t high, uint64_t) {
                for (uint64_t sum = low; sum <= high; ++sum) {
                    Seq input(100, IN_R);
                    model.Place(k, sum, [&](int at) {
                        Require(at < 12, "ineffective late Up frame was generated");
                        input[(size_t)at] |= IN_U;
                    });
                    all.push_back(std::move(input));
                }
            });
        }
        Require(shortJump == longJump, "b100 added variants beyond the twelve-frame rising window");

        const Seq route = DecodeInputs("d4n1a1e1d10e2d5n2d1a2n1d1n3d10w1d2e1d35e3d21e19d19e1d14e1d54e1w9n1d9e3d2e1d1e33d1e1d2e8d9");
        LevelTemplate level(17);
        auto initial = std::make_unique<State>();
        initial->sim.Load(&level, 0);
        std::memset(initial->met, 0, sizeof initial->met);
        for (int f = 0; f < 282; ++f) { initial->sim.Tick(route[(size_t)f]); initial->played.push_back(route[(size_t)f]); }
        auto before = std::make_unique<Sim>(initial->sim);
        auto probe = std::make_unique<Sim>();
        auto native = std::make_unique<Sim>();
        auto played = std::make_unique<State>();
        Force force;
        Cond waiting;
        std::string err;
        Require(ParseCond("alive@1000000", waiting, err), "test condition");
        force.conds.push_back(waiting);
        force.groups.push_back({0, 1, 1000000, false});
        force.segs.resize(1);
        force.segs[0].node = std::make_unique<Node>();
        Walker walker(force);
        bool recovered = false;
        for (const char* pattern : {"<d6+b6,j2>", "<d6+b6,0..40%>", "<e6a2w2+b3..6,j0..4,25..100%>", "<d3..8+b2..6,j0..3>"}) {
            double bytes;
            std::vector<JumpSummary> summaries;
            auto parts = ParsePattern(pattern, 1e8, bytes, err, summaries);
            Require(err.empty() && parts.size() == 1, "parse brute test pattern");
            const Node& node = *parts[0].node;
            BruteChoices choices;
            Scratch scratch;
            Seq input;
            Options expected;
            uint64_t minimum = UINT64_MAX, maximum = 0;
            std::set<std::string> minRoutes, maxRoutes;
            for (uint64_t index = 0; index < node.total;) {
                uint64_t skip = index + 1;
                if (!choices.Get(node, index, initial->sim, *probe, input, scratch, skip)) { index = skip; continue; }
                ++index;
                expected.push_back(input);
                uint64_t sum = 0;
                for (int i = 0; i < choices.frames; ++i)
                    if (input[(size_t)i] & IN_U) sum += (uint64_t)(choices.frames - i - 1);
                if (sum < minimum) { minimum = sum; minRoutes.clear(); }
                if (sum > maximum) { maximum = sum; maxRoutes.clear(); }
                if (sum == minimum) minRoutes.insert(EncodeInputsRLE(input));
                if (sum == maximum) maxRoutes.insert(EncodeInputsRLE(input));
                Require(!std::memcmp(before.get(), &initial->sim, sizeof(Sim)), "generation changed its incoming state");
                if (input == DecodeInputs("d2e2d2")) recovered = true;
                if (std::string(pattern).find("e6a2w2") != std::string::npos) {
                    Require(input.size() == 10 && input[6] == IN_L && input[7] == IN_L && input[8] == IN_U && input[9] == IN_U,
                            "base length or fixed tail changed");
                }
                CopySim(*native, initial->sim);
                for (uint8_t in : input) native->Tick(in);
                CopyState(*played, *initial);
                Require(walker.Play(*played, input, 0, 0) == GOING, "native candidate unexpectedly stopped");
                Require(!std::memcmp(native.get(), &played->sim, sizeof(Sim)), "Smartforce/native complete-state mismatch");
                ++states;
            }
            double preparation = 0;
            bool timedOut = false;
            Require(PrepareBrute(parts, initial->sim, 12, 1e8, bytes, 0, preparation, timedOut, &summaries) == PrepareResult::Ready,
                    "jump preparation did not produce a static generator");
            Require(parts.size() == 1 && parts[0].prepared && !parts[0].needsState() && parts[0].size() == expected.size(),
                    "prepared domain differs from the on-demand candidate count");
            for (uint64_t index = 0; index < parts[0].size(); ++index) {
                parts[0].Get(index, input, scratch);
                Require(input == expected[(size_t)index], "prepared candidate or ordering changed");
            }
            Require(!std::memcmp(before.get(), &initial->sim, sizeof(Sim)), "preparation changed the incoming state");
            Require(summaries.size() == 1 && !summaries[0].stateDependent &&
                    minRoutes.count(summaries[0].minimum) && maxRoutes.count(summaries[0].maximum),
                    "prepared min/max strings do not attain the allowed index-sum extremes");
        }
        Require(recovered, "near-apex two-effective-press witness was lost");
        std::cout << "PASS " << signatures << " original built-in signatures; " << states
                  << " complete-state replays; prepared/on-demand equivalence; near-apex witness; base length and tail; input-state isolation\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
