#include "updater.h"
#include "version.h"

#include <windows.h>
#include <winhttp.h>

#include <array>
#include <cstdio>
#include <optional>
#include <string_view>

namespace updater {
namespace {

constexpr wchar_t ApiHost[] = L"api.github.com";
constexpr wchar_t ApiPath[] = L"/repos/bangbang0103/PopTappo/releases/latest";
constexpr wchar_t RepoUrlPrefix[] = L"https://github.com/bangbang0103/PopTappo/";
constexpr size_t MaxResponseBytes = 512 * 1024;

struct WinHttpHandle {
    HINTERNET handle{};
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET h) : handle(h) {}
    ~WinHttpHandle() {
        if (handle) {
            WinHttpCloseHandle(handle);
        }
    }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    explicit operator bool() const { return handle != nullptr; }
    operator HINTERNET() const { return handle; }
};

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring result(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), needed);
    return result;
}

std::optional<std::string> ParseJsonString(std::string_view json, size_t& pos) {
    if (pos >= json.size() || json[pos] != '"') {
        return std::nullopt;
    }
    ++pos;
    std::string out;
    while (pos < json.size()) {
        const char ch = json[pos++];
        if (ch == '"') {
            return out;
        }
        if (ch == '\\') {
            if (pos >= json.size()) {
                return std::nullopt;
            }
            const char esc = json[pos++];
            switch (esc) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u':
                if (pos + 4 > json.size()) {
                    return std::nullopt;
                }
                pos += 4;
                break;
            default:
                out.push_back(esc);
                break;
            }
        } else {
            out.push_back(ch);
        }
    }
    return std::nullopt;
}

void SkipWhitespace(std::string_view json, size_t& pos) {
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) {
        ++pos;
    }
}

std::optional<std::string> ExtractTopLevelJsonString(std::string_view json, std::string_view targetKey) {
    size_t pos = 0;
    int depth = 0;
    bool expectKey = false;
    while (pos < json.size()) {
        SkipWhitespace(json, pos);
        if (pos >= json.size()) {
            break;
        }
        const char ch = json[pos];
        if (ch == '{') {
            ++depth;
            ++pos;
            expectKey = (depth == 1);
            continue;
        }
        if (ch == '}') {
            --depth;
            ++pos;
            expectKey = false;
            if (depth <= 0) {
                break;
            }
            continue;
        }
        if (ch == '[') {
            ++depth;
            ++pos;
            expectKey = false;
            continue;
        }
        if (ch == ']') {
            --depth;
            ++pos;
            expectKey = false;
            continue;
        }
        if (ch == ',') {
            ++pos;
            if (depth == 1) {
                expectKey = true;
            }
            continue;
        }
        if (ch == '"') {
            const auto parsed = ParseJsonString(json, pos);
            if (!parsed) {
                return std::nullopt;
            }
            if (depth == 1 && expectKey) {
                expectKey = false;
                SkipWhitespace(json, pos);
                if (pos < json.size() && json[pos] == ':') {
                    ++pos;
                    SkipWhitespace(json, pos);
                    if (*parsed == targetKey && pos < json.size() && json[pos] == '"') {
                        return ParseJsonString(json, pos);
                    }
                }
            }
            continue;
        }
        ++pos;
    }
    return std::nullopt;
}

struct SemVer {
    unsigned major{};
    unsigned minor{};
    unsigned patch{};
    std::wstring prerelease;
};

std::optional<SemVer> ParseVersion(std::wstring_view text) {
    if (!text.empty() && (text.front() == L'v' || text.front() == L'V')) {
        text.remove_prefix(1);
    }
    const auto plusPos = text.find(L'+');
    if (plusPos != text.npos) {
        text = text.substr(0, plusPos);
    }
    std::wstring prerelease;
    if (const auto dashPos = text.find(L'-'); dashPos != text.npos) {
        prerelease.assign(text.substr(dashPos + 1));
        text = text.substr(0, dashPos);
    }
    std::array<unsigned, 3> parts{};
    size_t count = 0;
    size_t pos = 0;
    while (pos < text.size() && count < parts.size()) {
        unsigned value = 0;
        size_t digits = 0;
        while (pos < text.size() && text[pos] >= L'0' && text[pos] <= L'9') {
            if (++digits > 9) {
                return std::nullopt;
            }
            value = value * 10 + static_cast<unsigned>(text[pos] - L'0');
            ++pos;
        }
        if (digits == 0) {
            return std::nullopt;
        }
        parts[count++] = value;
        if (pos < text.size()) {
            if (text[pos] != L'.' || count == parts.size()) {
                return std::nullopt;
            }
            ++pos;
        }
    }
    if (count < 2 || pos != text.size()) {
        return std::nullopt;
    }
    return SemVer{parts[0], parts[1], parts[2], std::move(prerelease)};
}

bool IsNewer(const SemVer& candidate, const SemVer& current) {
    if (candidate.major != current.major) return candidate.major > current.major;
    if (candidate.minor != current.minor) return candidate.minor > current.minor;
    if (candidate.patch != current.patch) return candidate.patch > current.patch;
    if (candidate.prerelease.empty() != current.prerelease.empty()) {
        return candidate.prerelease.empty();
    }
    return candidate.prerelease > current.prerelease;
}

std::wstring FormatWinError(DWORD error) {
    wchar_t code[32]{};
    swprintf_s(code, L"错误码 %lu", error);
    return code;
}

}  // namespace

Result CheckLatestRelease() {
    const std::wstring userAgent = std::wstring(L"PopTappo/") + POPTAPPO_VERSION_WSTR;
    WinHttpHandle session(WinHttpOpen(userAgent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        return {State::Error, {}, {}, L"无法初始化网络组件（" + FormatWinError(GetLastError()) + L"）"};
    }
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 8000);

    WinHttpHandle connect(WinHttpConnect(session, ApiHost, INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connect) {
        return {State::Error, {}, {}, L"无法连接 GitHub（" + FormatWinError(GetLastError()) + L"）"};
    }

    WinHttpHandle request(WinHttpOpenRequest(connect, L"GET", ApiPath, nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request) {
        return {State::Error, {}, {}, L"无法创建更新请求（" + FormatWinError(GetLastError()) + L"）"};
    }

    constexpr wchar_t headers[] =
        L"Accept: application/vnd.github+json\r\n"
        L"X-GitHub-Api-Version: 2022-11-28\r\n";
    if (!WinHttpSendRequest(request, headers, static_cast<DWORD>(-1),
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        return {State::Error, {}, {}, L"无法连接 GitHub（" + FormatWinError(GetLastError()) + L"）"};
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
        return {State::Error, {}, {}, L"无法读取服务器响应"};
    }

    if (statusCode == 404) {
        return {State::NoRelease, {}, ReleasesUrl, L"GitHub 仓库暂无已发布的 Release。"};
    }
    if (statusCode != 200) {
        return {State::Error, {}, ReleasesUrl, L"GitHub 返回状态码 HTTP " + std::to_wstring(statusCode)};
    }

    std::string body;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            return {State::Error, {}, {}, L"读取更新信息失败（" + FormatWinError(GetLastError()) + L"）"};
        }
        if (available == 0) {
            break;
        }
        if (body.size() + available > MaxResponseBytes) {
            available = static_cast<DWORD>(MaxResponseBytes - body.size());
            if (available == 0) {
                break;
            }
        }
        const size_t offset = body.size();
        body.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request, body.data() + offset, available, &read)) {
            return {State::Error, {}, {}, L"读取更新信息失败（" + FormatWinError(GetLastError()) + L"）"};
        }
        body.resize(offset + read);
        if (read == 0) {
            break;
        }
    }

    const auto tagUtf8 = ExtractTopLevelJsonString(body, "tag_name");
    if (!tagUtf8 || tagUtf8->empty()) {
        return {State::Error, {}, ReleasesUrl, L"无法解析最新版本信息"};
    }

    std::wstring latestTag = Utf8ToWide(*tagUtf8);
    std::wstring releaseUrl = std::wstring(ReleasesUrl) + L"/latest";
    if (const auto urlUtf8 = ExtractTopLevelJsonString(body, "html_url"); urlUtf8 && !urlUtf8->empty()) {
        const auto parsedUrl = Utf8ToWide(*urlUtf8);
        if (parsedUrl.starts_with(RepoUrlPrefix)) {
            releaseUrl = parsedUrl;
        }
    }

    const auto latestSemVer = ParseVersion(latestTag);
    const auto currentSemVer = ParseVersion(POPTAPPO_VERSION_WSTR);
    if (!latestSemVer || !currentSemVer) {
        const std::wstring currentTag = std::wstring(L"v") + POPTAPPO_VERSION_WSTR;
        if (latestTag != currentTag && latestTag != POPTAPPO_VERSION_WSTR) {
            return {State::UpdateAvailable, latestTag, releaseUrl, {}};
        }
        return {State::UpToDate, latestTag, releaseUrl, {}};
    }

    if (IsNewer(*latestSemVer, *currentSemVer)) {
        return {State::UpdateAvailable, latestTag, releaseUrl, {}};
    }
    return {State::UpToDate, latestTag, releaseUrl, {}};
}

}  // namespace updater
