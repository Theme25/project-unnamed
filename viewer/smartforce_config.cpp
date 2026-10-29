#include "smartforce_config.h"
#include "../src/search_offset.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cwchar>
#include <limits>

namespace smartui {
const std::vector<Setting>& Settings() {
    static const std::vector<Setting> fields = {
        {L"inputs", L"Input pattern", L"--inputs", L"", L"The input strings to try. Insert file adds saved strings at the cursor. Open String syntax for the guide.", Kind::Text, -1},
        {L"report", L"Find results where", L"--report", L"win", L"The conditions a result must meet. Use commas for conditions on the same frame, or semicolons for successive frames.", Kind::Text, -1},
        {L"level", L"Level", L"--level", L"1", L"Level 1 to 17. Search settings are independent of the route above; Use current route copies its setup.", Kind::Integer, 0, 1, 17},
        {L"checkpoint", L"Checkpoint", L"--checkpoint", L"0", L"0 starts at the beginning. The search engine checks which checkpoint numbers exist in this level.", Kind::Integer, 0},
        {L"threads", L"CPU threads", L"--threads", L"1", L"How many search workers can run at once. Defaults to all available CPU threads.", Kind::Integer, 0, 1},
        {L"stochastic", L"Random search order", L"--stochastic", L"0", L"Try strings in random order without repeating them. Enables the time limit, update interval, sample limit and seed.", Kind::Check, 0},
        {L"time", L"Time limit (s)", L"--time", L"0", L"Stop a random search after this many seconds. 0 means no time limit. Current work and any result saving finish before stopping.", Kind::Real, 0},
        {L"update", L"Update interval (s)", L"--update", L"60", L"How often a random search shows progress and its ten best results. 0 turns periodic updates off.", Kind::Real, 0},
        {L"hardfilter", L"Merge similar states", L"--hardfilter", L"0", L"Avoid repeating work when different strings reach the same game state. Keeps a representative string and counts the merged alternatives. Advanced settings control how closely states must match.", Kind::Check, 0},
        {L"gless", L"Glitchless", L"--gless", L"0", L"Apply the game's glitchless rules to this search.", Kind::Check, 0},
        {L"sortby", L"Sort results by", L"", L"", L"Default orders results by frame, then distance to the flag. Choose a field to put its highest values first, or select Lowest first to reverse it.", Kind::Choice, 0, 0, 0,
            {{L"Default", L""}, {L"Frame", L"frame"}, {L"px", L"px"}, {L"py", L"py"}, {L"vx", L"vx"}, {L"vy", L"vy"}, {L"Flag", L"flag"}}},
        {L"ascending", L"Lowest first", L"", L"0", L"Show the lowest values of the selected sort field first.", Kind::Check, 0},
        {L"save", L"Save all results", L"", L"0", L"Save every result without a row limit. Existing files are kept; a number is added to the new filename when needed.", Kind::Check, 0},
        {L"savename", L"Save file / folder", L"", L"", L"Leave blank for an automatic filename. End a path with a slash to choose a folder. Short paths start in the Search folder.", Kind::Text, 0},
        {L"mode", L"Operation", L"", L"search", L"Search plays the input strings and finds matches. List input strings previews the generated strings. Show object names lists the level's objects for contact conditions.", Kind::Choice, 0, 0, 0,
            {{L"Search", L"search"}, {L"List input strings", L"list"}, {L"Show object names", L"names"}}},
        {L"pad", L"Extra no-input frames", L"--pad", L"0", L"Play this many no-input frames after each candidate. This is separate from [+N] inside the pattern.", Kind::Integer, 0},
        {L"maxmem", L"Memory budget (GB)", L"--maxmem", L"4", L"Smartforce's memory budget, in decimal GB. Must be positive and at most this PC's physical memory.", Kind::Real, 1, 0},
        {L"top", L"Output rows (0 = all)", L"--top", L"30", L"How many result rows to print. 0 prints all rows. Saved results always include every row; Open full log shows the complete printed output.", Kind::Integer, 1},
        {L"samples", L"Sample limit (0 = all)", L"--samples", L"0", L"Stop a random search after trying this many candidates. 0 tries the entire search space.", Kind::Unsigned, 1},
        {L"seed", L"Seed (blank = random)", L"--seed", L"", L"Use the same whole number to repeat a random search order. Leave blank for a new order. 0 is a valid seed.", Kind::Unsigned, 1},
        {L"start-percent", L"Start from (%)", L"--start-percent", L"0", L"Skip directly to this percentage of the random order (0 to 100, up to nine decimal places). Use the previous seed and exactly the same input files and pattern. Choose a little earlier if the stopping point is approximate. The sample limit counts only new candidates.", Kind::Text, 1},
        {L"exclude", L"Skip saved strings", L"--exclude", L"", L"File of complete input strings to skip before replay. Other jumps from the same starts remain. Available for random search or listing; compares inputs exactly, before removing idle Up presses. Short paths start in the Search folder.", Kind::Text, 1},
        {L"idle-up", L"Keep idle Up presses", L"--idle-up", L"0", L"Preserve Up exactly as written, even when it cannot produce a jump. Otherwise Smartforce removes irrelevant Up presses.", Kind::Check, 1},
        {L"tolerance", L"Merge tolerance", L"--tolerance", L"1e-10", L"How much numbers may differ for states to count as similar. 0 requires exact values. Larger tolerances can merge paths that later behave differently.", Kind::Real, 1},
        {L"merge-every", L"Compare every N frames", L"--merge-every", L"32", L"When merging states, also compare them every N frames within a pattern part. 0 compares only between parts.", Kind::Integer, 1},
        {L"merge-report", L"Show merge details", L"--merge-report", L"0", L"Show detailed comparisons of similar game states. Also enables state merging.", Kind::Check, 1},
        {L"raw-states", L"Compare complete states", L"--raw-states", L"0", L"Include all internal physics data when deciding whether states can be merged.", Kind::Check, 1},
        {L"mathspikes", L"Spike rule", L"--mathspikes", L"1", L"1 is the accepted Practice Hack default. 0 uses Flash pixel hit tests; 2 uses the alternative math rule; -1 disables spikes for analysis.", Kind::Choice, 1, 0, 0,
            {{L"1 - Practice Hack", L"1"}, {L"0 - Flash pixels", L"0"}, {L"2 - Mathspikes 2", L"2"}, {L"-1 - Off (analysis)", L"-1"}}},
        {L"window", L"Game size (W x H)", L"--window", L"550x400", L"Game dimensions used for Flash pixel collisions. Resizing this app does not change these dimensions.", Kind::Text, 1},
        {L"scale", L"Game scale mode", L"", L"showall", L"Flash showAll or noScale, used by the display-layer collision rules.", Kind::Choice, 1, 0, 0,
            {{L"showAll", L"showall"}, {L"noScale", L"noscale"}}},
        {L"trig", L"Trigonometry", L"--trig", L"intel", L"Intel is the accepted Flash-compatible implementation. glibc is a diagnostic alternative and changes results.", Kind::Choice, 1, 0, 0,
            {{L"Intel (accepted port)", L"intel"}, {L"glibc (diagnostic)", L"glibc"}}},
        {L"player", L"Flash Player", L"--player", L"11.4", L"Whose arithmetic to reproduce. 11.4 covers every 32-bit Flash Player (the accepted reference). The 64-bit players compute some Math.sin/cos results and display matrices differently; x64 needs a CPU with FMA3.", Kind::Choice, 1, 0, 0,
            {{L"32-bit (11.4, 30, 32)", L"11.4"}, {L"ActiveX + 11.5 x64 OCX", L"activex"}, {L"Flash 30 x64 (FMA3 CPU)", L"x64"}, {L"Flash 30 x64 (no FMA3)", L"x64-nofma"}}},
        {L"directory", L"Search folder", L"", L"", L"The base folder for input files, saved results and full run logs. Defaults to the folder containing this app.", Kind::Text, 1},
    };
    return fields;
}

Values Defaults(unsigned threads) {
    Values v;
    for (const auto& s : Settings()) v[s.key] = s.initial;
    v[L"threads"] = std::to_wstring(std::max(1u, threads));
    return v;
}

bool Enabled(const Setting& s, const Values& v) {
    const auto& key = s.key;
    const auto is = [&](const wchar_t* k) { return !std::wcscmp(key, k); };
    if (is(L"time") || is(L"update") || is(L"samples")) return v.at(L"stochastic") == L"1" && v.at(L"mode") == L"search";
    if (is(L"seed") || is(L"start-percent")) return v.at(L"stochastic") == L"1" && v.at(L"mode") != L"names";
    if (is(L"exclude")) return v.at(L"mode") == L"list" || (v.at(L"mode") == L"search" && v.at(L"stochastic") == L"1");
    if (is(L"savename")) return v.at(L"save") == L"1";
    if (is(L"ascending")) return !v.at(L"sortby").empty();
    if (is(L"tolerance") || is(L"merge-every") || is(L"raw-states")) return v.at(L"hardfilter") == L"1" || v.at(L"merge-report") == L"1";
    if (is(L"inputs")) return v.at(L"mode") != L"names";
    if (is(L"report")) return v.at(L"mode") == L"search";
    return true;
}

static bool Digits(const std::wstring& s, unsigned long long& n) {
    if (s.empty() || s.find_first_not_of(L"0123456789") != std::wstring::npos) return false;
    errno = 0;
    n = std::wcstoull(s.c_str(), nullptr, 10);
    return errno != ERANGE;
}

bool Arguments(const Values& v, std::vector<std::wstring>& args, std::wstring& error, std::wstring& errorKey) {
    args = {L"smartforce"};
    error.clear(); errorKey.clear();
    auto fail = [&](const Setting& s, const wchar_t* reason) {
        error = std::wstring(s.label) + L": " + reason; errorKey = s.key; return false;
    };
    for (const auto& s : Settings()) {
        if (!Enabled(s, v)) continue;
        const auto& value = v.at(s.key);
        const std::wstring key = s.key;
        if ((key == L"seed" || key == L"exclude") && value.empty()) continue;
        if (key == L"start-percent") {
            rb::SearchOffset offset;
            if (!offset.Parse(value)) return fail(s, L"enter 0 to 100 with at most nine decimal places (optional % suffix).");
            if (offset.units && v.at(L"seed").empty()) return fail(s, L"enter the previous random seed before skipping ahead.");
            if (!offset.units) continue;
        }
        if (s.kind == Kind::Integer || s.kind == Kind::Unsigned) {
            unsigned long long n;
            if (!Digits(value, n)) return fail(s, L"enter an unsigned whole number (at most 18446744073709551615).");
            if (s.kind == Kind::Integer && (n < s.minimum || n > s.maximum)) return fail(s, L"value is outside the supported range.");
        } else if (s.kind == Kind::Real) {
            wchar_t* end = nullptr;
            errno = 0;
            const double n = std::wcstod(value.c_str(), &end);
            if (value.empty() || end == value.c_str() || *end || errno == ERANGE || !std::isfinite(n) || n < s.minimum ||
                (key == L"maxmem" && n <= 0)) return fail(s, L"enter a finite non-negative number (memory must be greater than zero).");
        } else if (s.kind == Kind::Choice) {
            if (std::none_of(s.choices.begin(), s.choices.end(), [&](const Choice& c) { return value == c.value; })) return fail(s, L"choose a value from the list.");
        } else if (s.kind == Kind::Check && value != L"0" && value != L"1") return fail(s, L"invalid checkbox value.");
        if (key == L"inputs" && value.empty()) return fail(s, L"enter a pattern or insert a file.");
        if (key == L"report" && value.empty()) return fail(s, L"enter conditions, for example win.");
        if (key == L"directory" && value.empty()) return fail(s, L"choose a folder for this search.");
        if (key == L"window") {
            const auto x = value.find_first_of(L"xX");
            unsigned long long w = 0, h = 0;
            if (x == std::wstring::npos || !Digits(value.substr(0, x), w) || !Digits(value.substr(x + 1), h) || !w || !h ||
                w > 2147483647 || h > 2147483647) return fail(s, L"use WIDTHxHEIGHT with positive integer dimensions.");
        }
        if (!*s.flag) continue;
        if (s.kind == Kind::Check) {
            if (value == L"1") args.push_back(s.flag);
        } else {
            args.push_back(s.flag);
            std::wstring normalized = value;
            if (key == L"window") std::replace(normalized.begin(), normalized.end(), L'X', L'x');
            args.push_back(normalized);
        }
    }
    if (v.at(L"mode") != L"search") args.push_back(v.at(L"mode") == L"list" ? L"--list" : L"--names");
    if (!v.at(L"sortby").empty()) {
        args.push_back(L"--sortby");
        args.push_back((v.at(L"ascending") == L"1" ? L"-" : L"") + v.at(L"sortby"));
    }
    args.push_back(v.at(L"scale") == L"noscale" ? L"--noscale" : L"--showall");
    if (v.at(L"save") == L"1") {
        args.push_back(L"--save");
        if (!v.at(L"savename").empty()) {
            if (v.at(L"savename").rfind(L"--", 0) == 0) {
                error = L"Save file / folder: prefix a filename starting with -- with .\\"; errorKey = L"savename"; return false;
            }
            args.push_back(v.at(L"savename"));
        }
    }
    return true;
}

// CommandLineToArgvW / Microsoft CRT rules, including quotes and trailing backslashes.
std::wstring QuoteWindows(const std::wstring& arg) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') { ++slashes; continue; }
        out.append(slashes * (c == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (c == L'"') out += L'\\';
        out += c;
    }
    out.append(slashes * 2, L'\\');
    return out + L'"';
}
std::wstring CommandLine(const std::vector<std::wstring>& args) {
    std::wstring out;
    for (const auto& arg : args) { if (!out.empty()) out += L' '; out += QuoteWindows(arg); }
    return out;
}
}  // namespace smartui
