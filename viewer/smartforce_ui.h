#pragma once
#include <windows.h>
#include <string>
#include "tas.h"

namespace smartui {
constexpr UINT WM_SEARCH_FINISHED = WM_APP + 71;
constexpr UINT WM_CURRENT_ROUTE = WM_APP + 72;
struct RouteContext { tas::Route route; rb::DisplayConfig display; rb::Player player = rb::Player::Flash11_4; };
// Call before any viewer initialization. A worker never creates a viewer window.
bool WorkerMainIfRequested(int& exitCode);
HWND Create(HWND parent, HFONT font, double dpi, const std::wstring& ini);
bool Running(HWND panel);
void Stop(HWND panel);
bool DialogMessage(HWND panel, MSG& message);
}  // namespace smartui
