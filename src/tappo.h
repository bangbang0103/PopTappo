#pragma once

#include <windows.h>
#include <objbase.h>
#include <UIAutomation.h>
#include <wrl/client.h>

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace tappo {

struct Window {
    HWND visual{};
    HWND webView{};
    HWND input{};
    DWORD process{};
};

enum class ChestState { Ready, Cooling, Unavailable };

struct Chest {
    ChestState state{};
    RECT bounds{};
    std::wstring countdown;
    Microsoft::WRL::ComPtr<IUIAutomationElement> element;
};

struct Reading {
    bool ok{};
    bool petFound{};
    bool popupVisible{};
    std::optional<unsigned> total;
    std::vector<Chest> chests;
};

Window FindTarget();
bool IsSameWindow(const Window& target, bool requireVisible = true);
bool SendTap(WORD key);
bool IsUserDesktopActive();
bool IsGestureActive();

class Reader {
public:
    explicit Reader();
    ~Reader();
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    // Construct and use on a worker thread without windows.
    Reading Read(const Window& target, bool requireVisible = true);
    HRESULT Open(const Chest& chest);

private:
    HRESULT initialized_;
    Microsoft::WRL::ComPtr<IUIAutomation> automation_;
    Microsoft::WRL::ComPtr<IUIAutomationCacheRequest> cache_;

    Reading ReadWindow(HWND window, bool requireVisible);
};

}  // namespace tappo
