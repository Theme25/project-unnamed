#include "smartforce_config.h"
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

static int checks = 0;
static void Check(bool ok, const char* name) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", name); std::exit(1); }
}
int main() {
    using namespace smartui;
    auto v = Defaults(12);
    v[L"directory"] = L"D:\\Space & apostrophe's\\";
    v[L"inputs"] = L"{prefix's route}[-1]e1<d57+j6..35,55..70%>d47..51e1d1";
    v[L"report"] = L"vx > 11.5, vy < -2.1";
    std::vector<std::wstring> args;
    std::wstring error, key;
    auto build = [&] { return Arguments(v, args, error, key); };
    auto has = [&](const wchar_t* s) { return std::find(args.begin(), args.end(), s) != args.end(); };
    auto value = [&](const wchar_t* s) { const auto i = std::find(args.begin(), args.end(), s); return i == args.end() || i + 1 == args.end() ? L"<absent>" : *(i + 1); };
    Check(build(), "default form validates");
    Check(value(L"--threads") == L"12", "all 12 threads");
    Check(value(L"--inputs") == v[L"inputs"] && value(L"--report") == v[L"report"], "pattern and report kept verbatim");
    Check(!has(L"--seed") && !has(L"--time") && !has(L"--samples") && !has(L"--sortby"), "optional defaults stay absent");
    v[L"stochastic"] = v[L"hardfilter"] = v[L"merge-report"] = v[L"raw-states"] = v[L"idle-up"] = v[L"gless"] = v[L"save"] = v[L"ascending"] = L"1";
    v[L"seed"] = L"0"; v[L"sortby"] = L"vx"; v[L"scale"] = L"noscale"; v[L"window"] = L"1100X800";
    v[L"savename"] = L"result's & all.txt";
    Check(build(), "all settings enabled");
    for (const auto* flag : {L"--inputs", L"--report", L"--level", L"--checkpoint", L"--threads", L"--stochastic", L"--time", L"--hardfilter",
             L"--sortby", L"--save", L"--gless", L"--maxmem", L"--pad", L"--top", L"--update", L"--samples", L"--seed", L"--idle-up",
             L"--tolerance", L"--merge-every", L"--merge-report", L"--raw-states", L"--mathspikes", L"--window", L"--noscale", L"--trig", L"--player"})
        Check(has(flag), "CLI option is available");
    Check(value(L"--seed") == L"0" && value(L"--sortby") == L"-vx", "zero seed and ascending sort");
    Check(value(L"--window") == L"1100x800", "window normalized for CLI");
    const std::vector<std::wstring> awkward = {L"rbview.exe", L"", L"plain", L"C:\\a b\\", L"a\"b", L"\\\"", L"trailing\\\\", v[L"inputs"], v[L"report"]};
    int count = 0;
    LPWSTR* decoded = CommandLineToArgvW(CommandLine(awkward).c_str(), &count);
    Check(decoded && count == static_cast<int>(awkward.size()), "Windows argument count");
    for (int i = 0; i < count; ++i) Check(decoded[i] == awkward[i], "Windows argument round trip");
    LocalFree(decoded);
    v[L"samples"] = L"18446744073709551615";
    Check(build(), "maximum 64-bit sample value");
    for (const auto* bad : {L"-1", L"18446744073709551616", L"1.5", L"12no"}) {
        v[L"samples"] = bad; Check(!build() && key == L"samples", "reject invalid sample limit");
    }
    v[L"samples"] = L"0";
    for (const auto* bad : {L"nan", L"inf", L"-0.5", L"1x"}) {
        v[L"tolerance"] = bad; Check(!build() && key == L"tolerance", "reject invalid tolerance");
    }
    v[L"tolerance"] = L"0"; v[L"maxmem"] = L"0";
    Check(!build() && key == L"maxmem", "reject zero memory"); v[L"maxmem"] = L"0.1";
    v[L"window"] = L"0x400";
    Check(!build() && key == L"window", "reject zero window"); v[L"window"] = L"550x400";
    v[L"mode"] = L"names"; v[L"inputs"] = L""; v[L"report"] = L"";
    Check(build() && has(L"--names") && !has(L"--inputs") && !has(L"--report"), "body names needs no pattern");
    v[L"mode"] = L"list"; v[L"inputs"] = L"d1..2";
    Check(build() && has(L"--list") && !has(L"--report"), "list needs no conditions");
    v[L"savename"] = L"";
    Check(build() && args.back() == L"--save", "automatic save without filename");
    v[L"savename"] = L"--unintended-option";
    Check(!build() && key == L"savename", "option-like save filename diagnosed");
    v[L"savename"] = L"";
    v[L"exclude"] = L"contained\\saved strings.txt";
    Check(build() && value(L"--exclude") == v[L"exclude"], "list forwards exclusion filename with spaces");
    v[L"mode"] = L"search"; v[L"report"] = L"win"; v[L"stochastic"] = L"1";
    Check(build() && value(L"--exclude") == v[L"exclude"], "random search forwards exclusions");
    v[L"stochastic"] = L"0";
    Check(build() && !has(L"--exclude"), "deterministic search does not forward disabled exclusions");
    v[L"stochastic"] = L"1"; v[L"exclude"] = L"";
    Check(build() && !has(L"--exclude"), "blank exclusions are omitted");
    v[L"start-percent"] = L"37.25%";
    Check(build() && value(L"--start-percent") == L"37.25%", "resume percentage forwarded exactly");
    v[L"seed"] = L"";
    Check(!build() && key == L"start-percent", "resume requires seed");
    v[L"seed"] = L"0";
    for (const auto* bad : {L"", L"-1", L"101", L"100.000000001", L"nan", L"1e1", L"2.1234567890"}) {
        v[L"start-percent"] = bad;
        Check(!build() && key == L"start-percent", "reject invalid resume percentage");
    }
    v[L"start-percent"] = L"100"; v[L"mode"] = L"list";
    Check(build() && value(L"--start-percent") == L"100", "listing accepts resume end boundary");
    v[L"stochastic"] = L"0";
    Check(build() && !has(L"--start-percent"), "disabled resume is not forwarded");
    v[L"stochastic"] = L"1"; v[L"start-percent"] = L"0";
    Check(build() && !has(L"--start-percent"), "default resume leaves existing invocations unchanged");
    std::printf("PASS %d configuration and quoting checks\n", checks);
}
