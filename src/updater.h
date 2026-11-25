#pragma once

#include <string>

namespace updater {

inline constexpr wchar_t ReleasesUrl[] = L"https://github.com/bangbang0103/PopTappo/releases";

enum class State {
    UpdateAvailable,
    UpToDate,
    NoRelease,
    Error,
};

struct Result {
    State state = State::Error;
    std::wstring latestVersion;
    std::wstring releaseUrl;
    std::wstring message;
};

Result CheckLatestRelease();

}  // namespace updater
