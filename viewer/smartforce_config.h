#pragma once

#include <map>
#include <string>
#include <vector>

namespace smartui {
enum class Kind { Text, Integer, Unsigned, Real, Check, Choice };
struct Choice { const wchar_t* label; const wchar_t* value; };
struct Setting {
    const wchar_t* key;
    const wchar_t* label;
    const wchar_t* flag;
    const wchar_t* initial;
    const wchar_t* help;
    Kind kind;
    int page;  // -1: common input fields, 0: Settings, 1: Advanced settings
    double minimum = 0;
    double maximum = 2147483647;
    std::vector<Choice> choices = {};
};
using Values = std::map<std::wstring, std::wstring>;
const std::vector<Setting>& Settings();
Values Defaults(unsigned threads);
bool Enabled(const Setting& s, const Values& v);
// Produces individual arguments, never shell text. ErrorKey identifies the control to focus.
bool Arguments(const Values& v, std::vector<std::wstring>& args, std::wstring& error, std::wstring& errorKey);
std::wstring QuoteWindows(const std::wstring& arg);
std::wstring CommandLine(const std::vector<std::wstring>& args);
}  // namespace smartui
