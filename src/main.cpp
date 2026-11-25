#include "engine.h"
#include "resource.h"

#include <shellapi.h>
#include <windowsx.h>
#include <wtsapi32.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string_view>

namespace {

constexpr wchar_t AppName[] = L"PopTappo";
constexpr wchar_t WindowClass[] = L"PopTappo.Tray";
constexpr UINT TrayEvent = WM_APP + 1;
constexpr UINT_PTR TapTimer = 1;

enum Command : UINT {
    ToggleTap = 100, ToggleChests, ToggleAwake, PauseAll, Refresh, Quit,
    Speed5 = 120, Speed10, Speed20, CustomSpeed, Key24 = 130, Key23, Key22, Key21,
    SentStatus = 200, ChestStatus, TotalStatus
};

constexpr UINT PresetRates[] = {5, 10, 20};
constexpr UINT MaxRate = 20;
constexpr WORD Keys[] = {VK_F24, VK_F23, VK_F22, VK_F21};

INT_PTR CALLBACK RateDialog(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_INITDIALOG) {
        SetDlgItemInt(dialog, IDC_TAP_RATE, static_cast<UINT>(lParam), FALSE);
        SendDlgItemMessageW(dialog, IDC_TAP_RATE, EM_SETLIMITTEXT, 6, 0);
        SendDlgItemMessageW(dialog, IDC_TAP_RATE, EM_SETSEL, 0, -1);
        return TRUE;
    }
    if (message != WM_COMMAND) {
        return FALSE;
    }
    if (LOWORD(wParam) == IDCANCEL) {
        EndDialog(dialog, 0);
        return TRUE;
    }
    if (LOWORD(wParam) == IDOK) {
        wchar_t text[8]{};
        GetDlgItemTextW(dialog, IDC_TAP_RATE, text, ARRAYSIZE(text));
        wchar_t* end{};
        const auto rate = std::wcstoul(text, &end, 10);
        if (end != text && *end == L'\0' && rate >= 1 && rate <= MaxRate) {
            EndDialog(dialog, rate);
        } else {
            SetDlgItemTextW(dialog, IDC_TAP_RATE_ERROR, L"请输入 1 到 20 的整数。");
            SetFocus(GetDlgItem(dialog, IDC_TAP_RATE));
            SendDlgItemMessageW(dialog, IDC_TAP_RATE, EM_SETSEL, 0, -1);
        }
        return TRUE;
    }
    return FALSE;
}

bool UpdateMenuText(HMENU menu, UINT id, std::wstring value) {
    wchar_t text[256]{};
    GetMenuStringW(menu, id, text, ARRAYSIZE(text), MF_BYCOMMAND);
    if (value == text) {
        return false;
    }
    MENUITEMINFOW item{sizeof(item)};
    item.fMask = MIIM_STRING;
    item.dwTypeData = value.data();
    return SetMenuItemInfoW(menu, id, FALSE, &item);
}

BOOL CALLBACK RepaintPopup(HWND popup, LPARAM menu) {
    MENUBARINFO info{sizeof(info)};
    if (!IsWindowVisible(popup) || !GetMenuBarInfo(popup, OBJID_CLIENT, 0, &info) ||
        info.hMenu != reinterpret_cast<HMENU>(menu)) {
        return TRUE;
    }
    RedrawWindow(popup, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW);
    return FALSE;
}

class App {
public:
    int Run(HINSTANCE instance);

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam);
    bool AddTray();
    void SyncWork();
    void SyncEngine();
    void SyncPower();
    void Tick();
    void OnInput(HRAWINPUT handle);
    void ShowMenu(POINT point);
    void OnCommand(UINT command);
    void UpdateMenuStatus();
    void UpdateTooltip();
    std::wstring TapStatus() const;
    DesktopAccess Access() const;

    HWND window_{};
    HMENU menu_{};
    UINT taskbarCreated_{};
    HPOWERNOTIFY displayNotify_{};
    bool sessionRegistered_{};
    bool suspended_{};
    bool sessionLocked_{};
    bool sessionDisconnected_{};
    bool displayOff_{};
    bool editingRate_{};
    bool tap_{};
    bool open_{};
    bool keepAwake_{};
    bool awakeRequested_{};
    bool powerError_{};
    bool rawInput_{};
    bool tapError_{};
    UINT rate_ = 10;
    unsigned key_{};
    ULONGLONG deferTapUntil_{};
    unsigned long long sent_{};
    NOTIFYICONDATAW icon_{};
    std::unique_ptr<Engine> engine_;
    Snapshot status_;
};

DesktopAccess App::Access() const {
    if (suspended_ || sessionDisconnected_ || editingRate_) {
        return DesktopAccess::Paused;
    }
    if (sessionLocked_) {
        return open_ ? DesktopAccess::Locked : DesktopAccess::Paused;
    }
    return DesktopAccess::Interactive;
}

int App::Run(HINSTANCE instance) {
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = WindowClass;
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!RegisterClassW(&windowClass)) {
        return 1;
    }

    if (!CreateWindowExW(WS_EX_TOOLWINDOW, WindowClass, AppName, WS_POPUP,
                         0, 0, 0, 0, nullptr, nullptr, instance, this)) {
        return 1;
    }
    if (taskbarCreated_) {
        ChangeWindowMessageFilterEx(window_, taskbarCreated_, MSGFLT_ALLOW, nullptr);
    }
    if (!AddTray()) {
        MessageBoxW(nullptr, L"无法创建系统托盘图标。", AppName, MB_OK | MB_ICONERROR);
        DestroyWindow(window_);
        return 1;
    }

    engine_ = std::make_unique<Engine>(window_);
    displayNotify_ = RegisterPowerSettingNotification(window_, &GUID_SESSION_DISPLAY_STATUS,
                                                     DEVICE_NOTIFY_WINDOW_HANDLE);
    sessionRegistered_ = WTSRegisterSessionNotification(window_, NOTIFY_FOR_THIS_SESSION);
    SyncWork();
    MSG message{};
    BOOL result;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (result == -1 && IsWindow(window_)) {
        DestroyWindow(window_);
    }
    return result == -1 ? 1 : static_cast<int>(message.wParam);
}

LRESULT CALLBACK App::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        app->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    auto app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    return app ? app->Handle(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

bool App::AddTray() {
    icon_.cbSize = sizeof(icon_);
    icon_.hWnd = window_;
    icon_.uID = 1;
    icon_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    icon_.uCallbackMessage = TrayEvent;
    icon_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(icon_.szTip, AppName);
    if (!Shell_NotifyIconW(NIM_ADD, &icon_)) {
        return false;
    }
    icon_.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &icon_);
    UpdateTooltip();
    return true;
}

void App::SyncWork() {
    KillTimer(window_, TapTimer);
    const auto access = Access();
    const bool shouldTap = tap_ && access == DesktopAccess::Interactive && !displayOff_;
    if (shouldTap != rawInput_) {
        const RAWINPUTDEVICE device{0x01, 0x06, DWORD(shouldTap ? RIDEV_INPUTSINK : RIDEV_REMOVE),
                                    shouldTap ? window_ : nullptr};
        if (RegisterRawInputDevices(&device, 1, sizeof(device))) {
            rawInput_ = shouldTap;
        }
    }
    if (shouldTap) {
        const UINT interval = (1000 + rate_ - 1) / rate_;
        if (rawInput_ && SetTimer(window_, TapTimer, interval, nullptr)) {
            deferTapUntil_ = GetTickCount64() + 300;
        } else {
            tapError_ = true;
            tap_ = false;
        }
    }
    SyncEngine();
    SyncPower();
    UpdateMenuStatus();
    UpdateTooltip();
}

void App::SyncEngine() {
    if (engine_) {
        engine_->Update(tap_ || open_, open_, Access(), menu_ != nullptr);
    }
}

void App::SyncPower() {
    const bool needAwake = keepAwake_ && !suspended_ && !sessionDisconnected_ &&
        (open_ || (tap_ && !sessionLocked_)) && tappo::IsSameWindow(status_.target, !sessionLocked_);
    if (needAwake == awakeRequested_) {
        powerError_ = false;
        return;
    }
    const auto request = needAwake ? ES_CONTINUOUS | ES_SYSTEM_REQUIRED : ES_CONTINUOUS;
    powerError_ = SetThreadExecutionState(request) == 0;
    if (!powerError_) {
        awakeRequested_ = needAwake;
    }
}

void App::OnInput(HRAWINPUT handle) {
    RAWINPUT input{};
    UINT size = sizeof(input);
    if (GetRawInputData(handle, RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
        input.header.dwType == RIM_TYPEKEYBOARD && input.header.hDevice) {
        deferTapUntil_ = GetTickCount64() + 250;
    }
}

void App::Tick() {
    if (!tap_ || menu_ || Access() != DesktopAccess::Interactive || displayOff_ || !status_.petReady ||
        !tappo::IsSameWindow(status_.target)) {
        return;
    }
    const auto now = GetTickCount64();
    GUITHREADINFO foreground{sizeof(foreground)};
    constexpr DWORD BusyUI = GUI_INMENUMODE | GUI_SYSTEMMENUMODE | GUI_POPUPMENUMODE | GUI_INMOVESIZE;
    if (tappo::IsGestureActive() || (GetAsyncKeyState(Keys[key_]) & 0x8000) ||
        (GetGUIThreadInfo(0, &foreground) && (foreground.flags & BusyUI))) {
        deferTapUntil_ = now + 250;
        return;
    }
    if (now < deferTapUntil_ || !tappo::IsUserDesktopActive()) {
        return;
    }
    if (tappo::SendTap(Keys[key_])) {
        ++sent_;
    } else {
        tap_ = false;
        tapError_ = true;
        SyncWork();
    }
}

std::wstring App::TapStatus() const {
    if (tapError_) {
        return L"连点已暂停：输入操作失败";
    }
    if (!tap_) {
        return L"连点已暂停";
    }
    if (Access() != DesktopAccess::Interactive) {
        return L"等待桌面恢复";
    }
    if (displayOff_) {
        return L"息屏时暂停连点";
    }
    return status_.petReady ? L"连点已启用" : L"等待 Wuwa Tappo 桌宠";
}

void App::UpdateTooltip() {
    const auto label = std::wstring(AppName) + L" · " + TapStatus() +
        (open_ ? L" · 自动开箱中" : L"");
    if (icon_.hWnd && label != icon_.szTip) {
        wcsncpy_s(icon_.szTip, label.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &icon_);
    }
}

void App::ShowMenu(POINT point) {
    if (menu_ || !engine_) {
        return;
    }
    const auto menu = CreatePopupMenu();
    const auto speed = CreatePopupMenu();
    const auto keys = CreatePopupMenu();
    if (!menu || !speed || !keys) {
        if (menu) DestroyMenu(menu);
        if (speed) DestroyMenu(speed);
        if (keys) DestroyMenu(keys);
        return;
    }

    AppendMenuW(menu, MF_STRING | MF_GRAYED, SentStatus, L"");
    AppendMenuW(menu, MF_STRING | MF_GRAYED, ChestStatus, L"");
    AppendMenuW(menu, MF_STRING | MF_GRAYED, TotalStatus, L"");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (tap_ ? MF_CHECKED : 0), ToggleTap, L"自动连点");
    AppendMenuW(menu, MF_STRING | (open_ ? MF_CHECKED : 0), ToggleChests, L"自动开箱");
    AppendMenuW(menu, MF_STRING | (keepAwake_ ? MF_CHECKED : 0), ToggleAwake, L"防自动睡眠");

    UINT selectedSpeed = CustomSpeed;
    for (UINT i = 0; i < ARRAYSIZE(PresetRates); ++i) {
        const auto label = std::to_wstring(PresetRates[i]) + L" 次/秒";
        AppendMenuW(speed, MF_STRING, Speed5 + i, label.c_str());
        if (rate_ == PresetRates[i]) {
            selectedSpeed = Speed5 + i;
        }
    }
    AppendMenuW(speed, MF_STRING, CustomSpeed, L"自定义…");
    CheckMenuRadioItem(speed, Speed5, CustomSpeed, selectedSpeed, MF_BYCOMMAND);
    const auto speedLabel = L"连点速度（" + std::to_wstring(rate_) + L" 次/秒）";
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(speed), speedLabel.c_str());
    for (UINT i = 0; i < ARRAYSIZE(Keys); ++i) {
        const auto label = L"F" + std::to_wstring(Keys[i] - VK_F1 + 1);
        AppendMenuW(keys, MF_STRING, Key24 + i, label.c_str());
    }
    CheckMenuRadioItem(keys, Key24, Key21, Key24 + key_, MF_BYCOMMAND);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(keys), L"模拟按键");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, Refresh, L"刷新桌宠状态");
    AppendMenuW(menu, MF_STRING | (tap_ || open_ ? 0 : MF_GRAYED), PauseAll, L"暂停全部");
    AppendMenuW(menu, MF_STRING, Quit, L"退出");

    menu_ = menu;
    UpdateMenuStatus();
    SyncEngine();
    SetForegroundWindow(window_);
    const UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                         point.x, point.y, window_, nullptr);
    menu_ = nullptr;
    PostMessageW(window_, WM_NULL, 0, 0);
    if (!command) {
        Shell_NotifyIconW(NIM_SETFOCUS, &icon_);
    }
    DestroyMenu(menu);
    OnCommand(command);
    if (IsWindow(window_)) {
        SyncWork();
    }
}

void App::OnCommand(UINT command) {
    if (command >= Speed5 && command <= Speed20) {
        rate_ = PresetRates[command - Speed5];
    } else if (command >= Key24 && command <= Key21) {
        key_ = command - Key24;
    } else {
        switch (command) {
        case ToggleTap: tap_ = !tap_; tapError_ = false; break;
        case ToggleChests: open_ = !open_; break;
        case ToggleAwake: keepAwake_ = !keepAwake_; break;
        case PauseAll: tap_ = open_ = tapError_ = false; break;
        case CustomSpeed: {
            editingRate_ = true;
            SyncWork();
            const auto rate = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_TAP_RATE),
                                               window_, RateDialog, rate_);
            editingRate_ = false;
            if (rate > 0) {
                rate_ = static_cast<UINT>(rate);
            }
            break;
        }
        case Quit: DestroyWindow(window_); break;
        }
    }
}

void App::UpdateMenuStatus() {
    if (!menu_) {
        return;
    }
    bool changed = UpdateMenuText(menu_, SentStatus,
        TapStatus() + L" · 本次发送 " + std::to_wstring(sent_) + L" 次");
    changed |= UpdateMenuText(menu_, ChestStatus,
        L"宝箱 · " + status_.chestStatus + L" · 已确认 " + std::to_wstring(status_.confirmed) + L" 个");
    changed |= UpdateMenuText(menu_, TotalStatus, L"桌宠界面读数 · " +
        (status_.total ? std::to_wstring(*status_.total) : L"—"));
    const auto power = powerError_ ? L"防自动睡眠（请求失败）" :
        awakeRequested_ ? L"防自动睡眠（生效中）" : L"防自动睡眠";
    changed |= UpdateMenuText(menu_, ToggleAwake, power);
    if (changed) {
        DrawMenuBar(window_);
        EnumThreadWindows(GetCurrentThreadId(), RepaintPopup, reinterpret_cast<LPARAM>(menu_));
    }
}

LRESULT App::Handle(UINT message, WPARAM wParam, LPARAM lParam) {
    if (taskbarCreated_ && message == taskbarCreated_) {
        AddTray();
        return 0;
    }
    switch (message) {
    case TrayEvent: {
        const auto event = LOWORD(lParam);
        if (HIWORD(lParam) == icon_.uID &&
            (event == WM_CONTEXTMENU || event == NIN_SELECT || event == NIN_KEYSELECT)) {
            POINT point{GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam)};
            if (point.x == -1 && point.y == -1) {
                GetCursorPos(&point);
            }
            ShowMenu(point);
        }
        return 0;
    }
    case Engine::Updated:
        if (engine_) {
            status_ = engine_->Status();
            SyncPower();
            UpdateMenuStatus();
            UpdateTooltip();
        }
        return 0;
    case WM_INPUT:
        if (tap_) {
            OnInput(reinterpret_cast<HRAWINPUT>(lParam));
        }
        break;
    case WM_TIMER:
        if (wParam == TapTimer) {
            Tick();
        }
        return 0;
    case WM_WTSSESSION_CHANGE:
        switch (wParam) {
        case WTS_SESSION_LOCK:
            sessionLocked_ = true;
            SyncWork();
            break;
        case WTS_SESSION_UNLOCK:
            sessionLocked_ = false;
            SyncWork();
            break;
        case WTS_CONSOLE_DISCONNECT:
        case WTS_REMOTE_DISCONNECT:
            sessionDisconnected_ = true;
            SyncWork();
            break;
        case WTS_CONSOLE_CONNECT:
        case WTS_REMOTE_CONNECT:
            sessionDisconnected_ = false;
            SyncWork();
            break;
        }
        return 0;
    case WM_POWERBROADCAST:
        if (wParam == PBT_APMSUSPEND || wParam == PBT_APMRESUMEAUTOMATIC ||
            wParam == PBT_APMRESUMESUSPEND) {
            suspended_ = wParam == PBT_APMSUSPEND;
            SyncWork();
        } else if (wParam == PBT_POWERSETTINGCHANGE && lParam) {
            const auto* setting = reinterpret_cast<const POWERBROADCAST_SETTING*>(lParam);
            if (setting->PowerSetting == GUID_SESSION_DISPLAY_STATUS && setting->DataLength == sizeof(DWORD)) {
                displayOff_ = setting->Data[0] == 0;
                SyncWork();
            }
        }
        return TRUE;
    case WM_DESTROY:
        KillTimer(window_, TapTimer);
        engine_.reset();
        Shell_NotifyIconW(NIM_DELETE, &icon_);
        if (displayNotify_) {
            UnregisterPowerSettingNotification(displayNotify_);
        }
        if (sessionRegistered_) {
            WTSUnRegisterSessionNotification(window_);
        }
        SetThreadExecutionState(ES_CONTINUOUS);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}

int Diagnose(const wchar_t* output) {
    std::wofstream report{std::filesystem::path(output)};
    if (!report) {
        return 2;
    }
    const auto window = tappo::FindTarget();
    tappo::Reader reader;
    const auto reading = reader.Read(window);
    report << L"desktop=" << tappo::IsUserDesktopActive()
           << L" visual=" << (window.visual != nullptr) << L" webview=" << (window.webView != nullptr)
           << L" input=" << (window.input != nullptr) << L" automation=" << reading.ok
           << L" pet=" << reading.petFound << L" chests=" << reading.chests.size()
           << L" total=" << (reading.total ? std::to_wstring(*reading.total) : L"-") << L'\n';
    for (const auto& chest : reading.chests) {
        report << L"ready=" << (chest.state == tappo::ChestState::Ready) << L'\n';
    }
    return reading.ok ? 0 : 1;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int count{};
    auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (count == 3 && std::wstring_view(arguments[1]) == L"--diagnose") {
        const auto result = Diagnose(arguments[2]);
        LocalFree(arguments);
        return result;
    }
    LocalFree(arguments);

    // Share the mutex with TapTappo so an older copy cannot automate alongside PopTappo.
    const auto mutex = CreateMutexW(nullptr, FALSE, L"Local\\TapTappo.Tray.Singleton");
    if (!mutex) {
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return 0;
    }
    App app;
    const auto result = app.Run(instance);
    CloseHandle(mutex);
    return result;
}
