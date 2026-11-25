#include "tappo.h"

#include <oleauto.h>

#include <algorithm>
#include <cwchar>
#include <string_view>

namespace tappo {
namespace {

std::wstring ClassName(HWND window) {
    wchar_t buffer[128]{};
    return std::wstring(buffer, GetClassNameW(window, buffer, ARRAYSIZE(buffer)));
}

std::wstring Title(HWND window) {
    wchar_t buffer[128]{};
    return std::wstring(buffer, GetWindowTextW(window, buffer, ARRAYSIZE(buffer)));
}

DWORD Owner(HWND window) {
    DWORD process{};
    GetWindowThreadProcessId(window, &process);
    return process;
}

bool IsTappo(DWORD process) {
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process);
    if (!handle) {
        return false;
    }
    wchar_t path[MAX_PATH]{};
    DWORD size = ARRAYSIZE(path);
    const bool read = QueryFullProcessImageNameW(handle, 0, path, &size);
    CloseHandle(handle);
    const wchar_t* filename = wcsrchr(path, L'\\');
    return read && filename && _wcsicmp(filename + 1, L"WuwaTappo.exe") == 0;
}

BOOL CALLBACK FindWebView(HWND window, LPARAM context) {
    if (IsWindowVisible(window) && ClassName(window) == L"Chrome_WidgetWin_1" &&
        Title(window) == L"Wuwa Tappo") {
        *reinterpret_cast<HWND*>(context) = window;
        return FALSE;
    }
    return TRUE;
}

BOOL CALLBACK FindVisual(HWND window, LPARAM context) {
    auto& candidates = *reinterpret_cast<std::vector<Window>*>(context);
    if (IsWindowVisible(window) && !IsIconic(window) && Title(window) == L"Wuwa Tappo" &&
        ClassName(window) == L"Tauri Window") {
        const auto process = Owner(window);
        if (IsTappo(process)) {
            Window candidate{window, nullptr, nullptr, process};
            EnumChildWindows(window, FindWebView, reinterpret_cast<LPARAM>(&candidate.webView));
            candidates.push_back(candidate);
        }
    }
    return TRUE;
}

BOOL CALLBACK FindInput(HWND window, LPARAM context) {
    auto& target = *reinterpret_cast<Window*>(context);
    if (Owner(window) == target.process && IsWindowVisible(window) &&
        ClassName(window) == L"DesktopCatInput") {
        target.input = window;
        return FALSE;
    }
    return TRUE;
}

bool HasClass(std::wstring_view classes, std::wstring_view name) {
    while (!classes.empty()) {
        const auto end = classes.find(L' ');
        if (classes.substr(0, end) == name) {
            return true;
        }
        if (end == classes.npos) {
            break;
        }
        classes.remove_prefix(end + 1);
    }
    return false;
}

using ElementStringGetter = HRESULT(STDMETHODCALLTYPE IUIAutomationElement::*)(BSTR*);

std::wstring ReadString(IUIAutomationElement* element, ElementStringGetter getter) {
    BSTR value{};
    const auto result = (element->*getter)(&value);
    std::wstring text;
    if (SUCCEEDED(result) && value) {
        text.assign(value, SysStringLen(value));
    }
    SysFreeString(value);
    return text;
}

std::optional<unsigned> CounterValue(std::wstring_view text) {
    unsigned value{};
    unsigned digits{};
    for (const wchar_t ch : text) {
        if (ch == L',') {
            continue;
        }
        if (ch < L'0' || ch > L'9' || ++digits > 9) {
            return std::nullopt;
        }
        value = value * 10 + (ch - L'0');
    }
    return digits ? std::optional{value} : std::nullopt;
}

}  // namespace

Window FindTarget() {
    std::vector<Window> candidates;
    EnumWindows(FindVisual, reinterpret_cast<LPARAM>(&candidates));
    const auto preferred = std::ranges::find_if(candidates, [](const Window& window) {
        return window.webView != nullptr;
    });
    if (candidates.empty()) {
        return {};
    }
    auto target = preferred != candidates.end() ? *preferred : candidates.front();
    EnumWindows(FindInput, reinterpret_cast<LPARAM>(&target));
    return target;
}

bool IsSameWindow(const Window& target, bool requireVisible) {
    if (!target.visual || !target.process || Owner(target.visual) != target.process) {
        return false;
    }
    if (requireVisible) {
        return IsWindowVisible(target.visual) && !IsIconic(target.visual);
    }
    return ClassName(target.visual) == L"Tauri Window" && Title(target.visual) == L"Wuwa Tappo";
}

bool SendTap(WORD key) {
    INPUT input[2]{};
    input[0].type = input[1].type = INPUT_KEYBOARD;
    input[0].ki.wVk = input[1].ki.wVk = key;
    input[0].ki.dwExtraInfo = input[1].ki.dwExtraInfo = TapInputMarker;
    input[1].ki.dwFlags = KEYEVENTF_KEYUP;
    const UINT sent = SendInput(ARRAYSIZE(input), input, sizeof(INPUT));
    if (sent == 1) {
        SendInput(1, &input[1], sizeof(INPUT));
    }
    return sent == ARRAYSIZE(input);
}

bool IsUserDesktopActive() {
    HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!desktop) {
        return false;
    }
    wchar_t name[32]{};
    const bool active = GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), nullptr) &&
        _wcsicmp(name, L"Default") == 0;
    CloseDesktop(desktop);
    return active;
}

bool IsGestureActive() {
    for (const auto key : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2,
                           VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN}) {
        if (GetAsyncKeyState(key) & 0x8000) {
            return true;
        }
    }
    return false;
}

Reader::Reader() : initialized_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {
    if (FAILED(initialized_)) {
        return;
    }
    Microsoft::WRL::ComPtr<IUIAutomation2> newer;
    if (SUCCEEDED(CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(newer.GetAddressOf())))) {
        newer->put_ConnectionTimeout(1200);
        newer->put_TransactionTimeout(1800);
        automation_ = newer;
    } else if (FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(automation_.GetAddressOf())))) {
        return;
    }
    if (FAILED(automation_->CreateCacheRequest(cache_.GetAddressOf()))) {
        return;
    }
    for (const auto property : {UIA_ClassNamePropertyId,
                                UIA_IsEnabledPropertyId, UIA_BoundingRectanglePropertyId}) {
        if (FAILED(cache_->AddProperty(property))) {
            cache_.Reset();
            return;
        }
    }
    if (SUCCEEDED(cache_->Clone(totalCache_.GetAddressOf())) &&
        FAILED(totalCache_->AddProperty(UIA_NamePropertyId))) {
        totalCache_.Reset();
    }
}

Reader::~Reader() {
    totalCache_.Reset();
    cache_.Reset();
    automation_.Reset();
    if (SUCCEEDED(initialized_)) {
        CoUninitialize();
    }
}

Reading Reader::Read(const Window& target, bool requireVisible, bool readTotal) {
    if (!IsSameWindow(target, requireVisible) || !cache_) {
        return {};
    }
    readTotal = readTotal && totalCache_;
    const bool useWebView = target.webView && IsChild(target.visual, target.webView);
    auto reading = ReadWindow(useWebView ? target.webView : target.visual, requireVisible, readTotal);
    if (useWebView && !reading.petFound && reading.chests.empty()) {
        auto fallback = ReadWindow(target.visual, requireVisible, readTotal);
        if (fallback.petFound || !fallback.chests.empty()) {
            return fallback;
        }
    }
    return reading;
}

Reading Reader::ReadWindow(HWND window, bool requireVisible, bool readTotal) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IUIAutomationElement> root;
    ComPtr<IUIAutomationCondition> condition;
    ComPtr<IUIAutomationElementArray> elements;
    int length{};
    auto* request = readTotal ? totalCache_.Get() : cache_.Get();
    if (FAILED(automation_->ElementFromHandle(window, &root)) ||
        FAILED(automation_->CreateTrueCondition(&condition)) ||
        FAILED(root->FindAllBuildCache(TreeScope_Descendants, condition.Get(), request, &elements)) ||
        FAILED(elements->get_Length(&length))) {
        return {};
    }

    Reading result;
    result.ok = true;
    for (int i = 0; i < std::min(length, 400); ++i) {
        ComPtr<IUIAutomationElement> element;
        if (FAILED(elements->GetElement(i, &element))) {
            continue;
        }
        const auto classes = ReadString(element.Get(), &IUIAutomationElement::get_CachedClassName);
        std::wstring name;
        result.petFound |= HasClass(classes, L"pet-dock-content");
        result.popupVisible |= classes.find(L"chest-reward-popup") != classes.npos ||
            classes.find(L"chest-drop-motion") != classes.npos;
        if (readTotal) {
            name = ReadString(element.Get(), &IUIAutomationElement::get_CachedName);
            if (const auto number = CounterValue(name); number && (!result.total || *number > *result.total)) {
                result.total = number;
            }
        }
        const bool ready = HasClass(classes, L"reward-btn");
        const bool cooling = HasClass(classes, L"reward-countdown");
        if (!ready && !cooling) {
            continue;
        }
        RECT bounds{};
        const auto boundsResult = element->get_CachedBoundingRectangle(&bounds);
        if (requireVisible && (FAILED(boundsResult) || IsRectEmpty(&bounds))) {
            continue;
        }
        BOOL enabled{};
        element->get_CachedIsEnabled(&enabled);
        const auto state = ready && enabled ? ChestState::Ready :
            cooling ? ChestState::Cooling : ChestState::Unavailable;
        if (cooling && !readTotal) {
            name = ReadString(element.Get(), &IUIAutomationElement::get_CurrentName);
        }
        result.chests.push_back({state, bounds, cooling ? std::move(name) : std::wstring{}, std::move(element)});
    }
    std::ranges::stable_sort(result.chests, {}, [](const Chest& chest) { return chest.bounds.left; });
    return result;
}

HRESULT Reader::Open(const Chest& chest) {
    if (chest.state != ChestState::Ready) {
        return E_INVALIDARG;
    }
    Microsoft::WRL::ComPtr<IUIAutomationInvokePattern> invoke;
    const auto result = chest.element->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&invoke));
    if (FAILED(result)) {
        return result;
    }
    return invoke ? invoke->Invoke() : E_NOINTERFACE;
}

}  // namespace tappo
