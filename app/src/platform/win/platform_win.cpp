// SPDX-License-Identifier: MIT
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// After <windows.h>, which it needs.
#include <tlhelp32.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "platform/env.h"
#include "platform/game_window.h"
#include "platform/platform.h"

namespace curia::platform {

namespace {

// The executable's file name as ASCII; a name with any other character is not CK3's (empty).
std::string asciiName(const wchar_t* wide) {
    std::string out;
    for (const wchar_t* c = wide; *c != L'\0'; ++c) {
        if (static_cast<unsigned>(*c) > 0x7FU) {
            return {};
        }
        out.push_back(static_cast<char>(*c));
    }
    return out;
}

// The process ids of every running ck3.exe (a snapshot of the process list; no permission is
// needed to list processes).
std::optional<std::vector<DWORD>> gameProcessIds() {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    std::vector<DWORD> ids;
    PROCESSENTRY32W entry{};
    entry.dwSize = static_cast<DWORD>(sizeof(entry));
    for (BOOL more = Process32FirstW(snapshot, &entry); more != FALSE;
         more = Process32NextW(snapshot, &entry)) {
        if (isGameProcessName(asciiName(entry.szExeFile))) {
            ids.push_back(entry.th32ProcessID);
        }
    }
    CloseHandle(snapshot);
    return ids;
}

// The executable name of a process ("" if it cannot be asked, e.g. a protected process).
std::string processName(DWORD processId) {
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr) {
        return {};
    }
    wchar_t path[MAX_PATH];
    DWORD size = MAX_PATH;
    std::string name;
    if (QueryFullProcessImageNameW(process, 0, path, &size) != FALSE) {
        const std::wstring full(path, size);
        const std::size_t slash = full.find_last_of(L"\\/");
        name = asciiName(full.c_str() + (slash == std::wstring::npos ? 0 : slash + 1));
    }
    CloseHandle(process);
    return name;
}

struct WindowSearch {
    std::vector<WindowCandidate> windows;
};

BOOL CALLBACK collectWindow(HWND window, LPARAM data) {
    auto* search = reinterpret_cast<WindowSearch*>(data);
    if (IsWindowVisible(window) == FALSE || GetWindow(window, GW_OWNER) != nullptr ||
        IsIconic(window) != FALSE) {
        // Hidden windows and owned ones (dialogs, tool windows) are not the game, and a minimised
        // window's rectangle is where Windows parks it, not where the game is.
        return TRUE;
    }
    RECT rect{};
    if (GetWindowRect(window, &rect) == FALSE) {
        return TRUE;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    WindowCandidate candidate;
    candidate.pid = static_cast<long long>(processId);
    candidate.layer = 0;
    candidate.rect = ScreenRect{static_cast<double>(rect.left), static_cast<double>(rect.top),
                                static_cast<double>(rect.right - rect.left),
                                static_cast<double>(rect.bottom - rect.top)};
    search->windows.push_back(candidate);
    return TRUE;
}

class WindowsPlatform final : public Platform {
public:
    std::string_view name() const override { return "windows"; }

    long long processId() const override { return static_cast<long long>(GetCurrentProcessId()); }

    // TODO(M3): use SHGetKnownFolderPath(FOLDERID_Documents) because Documents can be redirected.
    std::vector<std::filesystem::path> candidateCk3UserDirs() const override {
        const std::filesystem::path user = getEnvPath(L"USERPROFILE", "USERPROFILE");
        if (user.empty()) {
            return {};
        }
        const std::filesystem::path suffix =
            std::filesystem::path("Paradox Interactive") / "Crusader Kings III";
        return {user / "Documents" / suffix, user / "OneDrive" / "Documents" / suffix};
    }

    std::optional<bool> isGameRunning() const override {
        const std::optional<std::vector<DWORD>> ids = gameProcessIds();
        if (!ids.has_value()) {
            return std::nullopt;
        }
        return !ids->empty();
    }

    // The process of the foreground window. A window of a process Curia may not ask (an elevated
    // one) counts as not CK3.
    std::optional<bool> gameIsFrontmost() const override {
        const HWND front = GetForegroundWindow();
        if (front == nullptr) {
            return false;
        }
        DWORD processId = 0;
        GetWindowThreadProcessId(front, &processId);
        return processId != 0 && isGameProcessName(processName(processId));
    }

    // GetWindowRect's rectangle: in physical pixels once SDL's video subsystem has started (SDL
    // 3.4 declares the process per-monitor DPI aware there), else scaled by Windows. Not used for
    // placement in M4 (the companion window is not moved).
    std::optional<ScreenRect> findGameWindow() const override {
        const std::optional<std::vector<DWORD>> ids = gameProcessIds();
        if (!ids.has_value() || ids->empty()) {
            return std::nullopt;
        }
        WindowSearch search;
        EnumWindows(collectWindow, reinterpret_cast<LPARAM>(&search));
        std::vector<long long> pids;
        pids.reserve(ids->size());
        std::transform(ids->begin(), ids->end(), std::back_inserter(pids),
                       [](DWORD id) { return static_cast<long long>(id); });
        return pickGameWindow(search.windows, pids);
    }
};

}  // namespace

std::unique_ptr<Platform> create() {
    return std::make_unique<WindowsPlatform>();
}

}  // namespace curia::platform
