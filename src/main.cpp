#include "engine.h"
#include "resource.h"
#include "updater.h"
#include "version.h"

#include <hidusage.h>
#include <shellapi.h>
#include <windowsx.h>
#include <wtsapi32.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string_view>
#include <utility>

namespace {

constexpr wchar_t AppName[] = L"PopTappo";
constexpr wchar_t WindowClass[] = L"PopTappo.Tray";
constexpr UINT TrayEvent = WM_APP + 1;
constexpr UINT UpdateChecked = WM_APP + 3;
constexpr UINT_PTR TapTimer = 1;
constexpr UINT_PTR MenuPaintTimer = 2;
constexpr UINT_PTR ChestTimer = 3;
constexpr UINT ChestStatusRow = 1;
constexpr UINT SpeedMenuRow = 7;

enum Command : UINT {
    ToggleTap = 100, ToggleChests, ToggleAwake, PauseAll, Refresh, Quit, CheckUpdate,
    Speed10 = 120, Speed25, Speed100, CustomSpeed, Key24 = 130, Key23, Key22, Key21,
    SentStatus = 200, ChestStatus, TotalStatus, VersionStatus
};

constexpr UINT PresetRates[] = {10, 25, 100};
constexpr UINT MaxRate = 100;
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
            SetDlgItemTextW(dialog, IDC_TAP_RATE_ERROR, L"请输入 1 到 100 的整数。");
            SetFocus(GetDlgItem(dialog, IDC_TAP_RATE));
            SendDlgItemMessageW(dialog, IDC_TAP_RATE, EM_SETSEL, 0, -1);
        }
        return TRUE;
    }
    return FALSE;
}

bool UpdateMenuText(HMENU menu, UINT itemId, std::wstring value, bool byPosition = false) {
    wchar_t text[256]{};
    GetMenuStringW(menu, itemId, text, ARRAYSIZE(text), byPosition ? MF_BYPOSITION : MF_BYCOMMAND);
    if (value == text) {
        return false;
    }
    MENUITEMINFOW item{sizeof(item)};
    item.fMask = MIIM_STRING;
    item.dwTypeData = value.data();
    return SetMenuItemInfoW(menu, itemId, byPosition, &item);
}

struct MenuRepaint {
    HMENU menu;
    bool chestOnly;
};

BOOL CALLBACK RepaintPopup(HWND popup, LPARAM parameter) {
    const auto& repaint = *reinterpret_cast<const MenuRepaint*>(parameter);
    MENUBARINFO info{sizeof(info)};
    if (!IsWindowVisible(popup) || !GetMenuBarInfo(popup, OBJID_CLIENT, 0, &info) || info.hMenu != repaint.menu) {
        return TRUE;
    }
    RECT row{};
    if (repaint.chestOnly && GetMenuItemRect(popup, repaint.menu, ChestStatusRow, &row)) {
        MapWindowPoints(nullptr, popup, reinterpret_cast<POINT*>(&row), 2);
        RedrawWindow(popup, &row, nullptr, RDW_INVALIDATE | RDW_NOERASE | RDW_UPDATENOW);
    } else {
        RedrawWindow(popup, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW);
    }
    return FALSE;
}

void SyncRawInput(HWND window, USHORT usage, bool needed, bool& registered) {
    if (needed == registered) {
        return;
    }
    const RAWINPUTDEVICE device{HID_USAGE_PAGE_GENERIC, usage,
        DWORD(needed ? RIDEV_INPUTSINK : RIDEV_REMOVE), needed ? window : nullptr};
    if (RegisterRawInputDevices(&device, 1, sizeof(device))) {
        registered = needed;
    }
}

bool IsInlineCommand(UINT command) {
    return (command >= ToggleTap && command <= Refresh) ||
        (command >= Speed10 && command <= CustomSpeed) ||
        (command >= Key24 && command <= Key21);
}

bool UpdateMenuCheck(HMENU menu, UINT id, bool checked) {
    if (((GetMenuState(menu, id, MF_BYCOMMAND) & MF_CHECKED) != 0) == checked) {
        return false;
    }
    CheckMenuItem(menu, id, MF_BYCOMMAND | (checked ? MF_CHECKED : MF_UNCHECKED));
    return true;
}

bool UpdateMenuRadio(HMENU menu, UINT first, UINT last, UINT selected) {
    if (GetMenuState(menu, selected, MF_BYCOMMAND) & MF_CHECKED) {
        return false;
    }
    return CheckMenuRadioItem(menu, first, last, selected, MF_BYCOMMAND);
}

class App {
public:
    ~App();
    int Run(HINSTANCE instance);

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK MenuFilter(int code, WPARAM wParam, LPARAM lParam);
    static thread_local App* activeMenuOwner_;
    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam);
    bool AddTray();
    void SyncWork();
    void SyncEngine();
    void SyncPower();
    void Tick();
    void OnInput(HRAWINPUT handle);
    void ShowMenu(POINT point);
    bool HandleMenuInput(const MSG& message);
    UINT InlineCommandAt(POINT point) const;
    void ApplyInlineCommand(UINT command);
    void RequestMenuSnapshot();
    void OnCommand(UINT command);
    void StartUpdateCheck();
    void OnUpdateChecked();
    std::wstring CheckUpdateLabel() const;
    void QueueMenuUpdate();
    void UpdateMenuStatus();
    void SyncChestTimer();
    std::wstring ChestText() const;
    bool IsOwnMenu(HMENU menu) const;
    void UpdateTooltip();
    std::wstring TapStatus() const;
    DesktopAccess Access() const;

    HWND window_{};
    HMENU menu_{};
    HMENU speedMenu_{};
    HMENU keyMenu_{};
    HHOOK menuHook_{};
    UINT menuSelectedCommand_{};
    UINT menuPressedCommand_[2]{};
    bool customSpeedRequested_{};
    UINT taskbarCreated_{};
    HPOWERNOTIFY displayNotify_{};
    bool sessionRegistered_{};
    bool suspended_{};
    bool sessionLocked_{};
    bool sessionDisconnected_{};
    bool displayOff_{};
    bool editingRate_{};
    bool showingUpdateDialog_{};
    bool checkingUpdate_{};
    std::atomic<bool> closing_{};
    bool tap_{};
    bool open_{};
    bool keepAwake_{};
    bool awakeRequested_{};
    bool powerError_{};
    bool rawKeyboard_{};
    bool tapError_{};
    UINT rate_ = 10;
    unsigned key_{};
    ULONGLONG deferTapUntil_{};
    unsigned long long sent_{};
    unsigned long long menuSent_{};
    unsigned menuSession_{};
    std::optional<unsigned> menuTotal_;
    bool menuSamplePending_{};
    bool menuUpdatePending_{};
    std::wstring latestVersion_;
    std::mutex updateMutex_;
    std::optional<updater::Result> updateResult_;
    std::thread updateThread_;
    NOTIFYICONDATAW icon_{};
    std::unique_ptr<Engine> engine_;
    Snapshot status_;
};

thread_local App* App::activeMenuOwner_{};

App::~App() {
    closing_.store(true);
    if (updateThread_.joinable()) {
        updateThread_.join();
    }
}

DesktopAccess App::Access() const {
    if (suspended_ || sessionDisconnected_ || editingRate_ || showingUpdateDialog_) {
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
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    if (!windowClass.hIcon) {
        windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
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
    icon_.hIcon = static_cast<HICON>(LoadImageW(
        GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    if (!icon_.hIcon) {
        icon_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
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
    const bool shouldTap = tap_ && access == DesktopAccess::Interactive && !displayOff_ &&
        (!menu_ || menuHook_);
    SyncRawInput(window_, HID_USAGE_GENERIC_KEYBOARD, shouldTap, rawKeyboard_);
    if (shouldTap) {
        const UINT interval = (1000 + rate_ - 1) / rate_;
        if (rawKeyboard_ && SetTimer(window_, TapTimer, interval, nullptr)) {
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
        engine_->Update(tap_ || open_, open_, Access(), menu_ ? menuSession_ : 0);
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
        input.header.dwType == RIM_TYPEKEYBOARD && input.header.hDevice &&
        input.data.keyboard.ExtraInformation != tappo::TapInputMarker) {
        deferTapUntil_ = GetTickCount64() + 250;
    }
}

void App::Tick() {
    if (!tap_ || (menu_ && !menuHook_) || Access() != DesktopAccess::Interactive || displayOff_ || !status_.petReady ||
        !tappo::IsSameWindow(status_.target)) {
        return;
    }
    const auto now = GetTickCount64();
    GUITHREADINFO foreground{sizeof(foreground)};
    constexpr DWORD BusyUI = GUI_INMENUMODE | GUI_SYSTEMMENUMODE | GUI_POPUPMENUMODE | GUI_INMOVESIZE;
    const bool otherMenu = GetGUIThreadInfo(0, &foreground) && (foreground.flags & BusyUI) &&
        (!menuHook_ || foreground.hwndMenuOwner != window_);
    if (tappo::IsGestureActive() || (GetAsyncKeyState(Keys[key_]) & 0x8000) ||
        otherMenu) {
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

LRESULT CALLBACK App::MenuFilter(int code, WPARAM wParam, LPARAM lParam) {
    if (code == MSGF_MENU && activeMenuOwner_ &&
        activeMenuOwner_->HandleMenuInput(*reinterpret_cast<const MSG*>(lParam))) {
        return 1;
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

bool App::IsOwnMenu(HMENU menu) const {
    return menu && (menu == menu_ || menu == speedMenu_ || menu == keyMenu_);
}

UINT App::InlineCommandAt(POINT point) const {
    const auto popup = WindowFromPoint(point);
    MENUBARINFO info{sizeof(info)};
    if (!popup || !GetMenuBarInfo(popup, OBJID_CLIENT, 0, &info) || !IsOwnMenu(info.hMenu)) {
        return 0;
    }
    const int index = MenuItemFromPoint(popup, info.hMenu, point);
    if (index < 0 || (GetMenuState(info.hMenu, index, MF_BYPOSITION) & (MF_DISABLED | MF_GRAYED))) {
        return 0;
    }
    const UINT command = GetMenuItemID(info.hMenu, index);
    return IsInlineCommand(command) ? command : 0;
}

void App::ApplyInlineCommand(UINT command) {
    if (command == Refresh) {
        RequestMenuSnapshot();
        return;
    }
    if (command == CustomSpeed) {
        customSpeedRequested_ = EndMenu() != FALSE;
        return;
    }
    OnCommand(command);
    SyncWork();
}

void App::RequestMenuSnapshot() {
    if (++menuSession_ == 0) {
        ++menuSession_;
    }
    menuSent_ = sent_;
    menuSamplePending_ = true;
    SyncEngine();
}

bool App::HandleMenuInput(const MSG& message) {
    const auto type = message.message;
    const bool keyDown = type == WM_KEYDOWN || type == WM_SYSKEYDOWN;
    const bool keyUp = type == WM_KEYUP || type == WM_SYSKEYUP;
    if (keyDown || keyUp) {
        const auto key = message.wParam;
        if (key >= VK_F21 && key <= VK_F24 &&
            static_cast<ULONG_PTR>(GetMessageExtraInfo()) == tappo::TapInputMarker) {
            return true;
        }
        deferTapUntil_ = GetTickCount64() + 250;
        const bool firstPress = keyDown && !(message.lParam & (LPARAM{1} << 30));
        if (menuSelectedCommand_ && (key == VK_RETURN || key == VK_SPACE)) {
            if (firstPress) {
                ApplyInlineCommand(menuSelectedCommand_);
            }
            return true;
        }
        return false;
    }

    const bool leftDown = type == WM_LBUTTONDOWN || type == WM_LBUTTONDBLCLK;
    const bool rightDown = type == WM_RBUTTONDOWN || type == WM_RBUTTONDBLCLK;
    const bool leftUp = type == WM_LBUTTONUP;
    const bool rightUp = type == WM_RBUTTONUP;
    if (leftDown || rightDown || type == WM_MBUTTONDOWN || type == WM_XBUTTONDOWN) {
        deferTapUntil_ = GetTickCount64() + 250;
    }
    if (leftDown || rightDown || leftUp || rightUp) {
        auto& pressed = menuPressedCommand_[rightDown || rightUp];
        const auto command = InlineCommandAt(message.pt);
        if (leftDown || rightDown) {
            pressed = command;
            return pressed != 0;
        }
        if (const auto prior = std::exchange(pressed, 0u)) {
            if (prior == command) {
                ApplyInlineCommand(command);
            }
            return true;
        }
    }
    return false;
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
        (open_ ? L" · 自动开箱中" : L"") + (powerError_ ? L" · 防睡眠请求失败" : L"");
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

    for (UINT i = 0; i < ARRAYSIZE(PresetRates); ++i) {
        const auto label = std::to_wstring(PresetRates[i]) + L" 次/秒";
        AppendMenuW(speed, MF_STRING, Speed10 + i, label.c_str());
    }
    AppendMenuW(speed, MF_STRING, CustomSpeed, L"自定义…");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(speed), L"连点速度");
    for (UINT i = 0; i < ARRAYSIZE(Keys); ++i) {
        const auto label = L"F" + std::to_wstring(Keys[i] - VK_F1 + 1);
        AppendMenuW(keys, MF_STRING, Key24 + i, label.c_str());
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(keys), L"模拟按键");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, Refresh, L"刷新桌宠状态");
    AppendMenuW(menu, MF_STRING | (tap_ || open_ ? 0 : MF_GRAYED), PauseAll, L"暂停全部");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | MF_GRAYED, VersionStatus,
                (std::wstring(L"版本 · v") + POPTAPPO_VERSION_WSTR).c_str());
    AppendMenuW(menu, MF_STRING | (checkingUpdate_ ? MF_GRAYED : 0), CheckUpdate,
                CheckUpdateLabel().c_str());
    AppendMenuW(menu, MF_STRING, Quit, L"退出");

    menu_ = menu;
    speedMenu_ = speed;
    keyMenu_ = keys;
    menuTotal_.reset();
    RequestMenuSnapshot();
    UINT command{};
    do {
        menuSelectedCommand_ = menuPressedCommand_[0] = menuPressedCommand_[1] = 0;
        activeMenuOwner_ = this;
        // TrackPopupMenuEx owns the message loop while our menu is visible.
        menuHook_ = SetWindowsHookExW(WH_MSGFILTER, MenuFilter, nullptr, GetCurrentThreadId());
        if (command == CustomSpeed) {
            SyncWork();
        } else {
            UpdateMenuStatus();
        }
        SyncChestTimer();
        SetForegroundWindow(window_);
        command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, window_, nullptr);
        if (std::exchange(customSpeedRequested_, false)) {
            command = CustomSpeed;
        }
        if (menuHook_) {
            UnhookWindowsHookEx(menuHook_);
            menuHook_ = nullptr;
        }
        activeMenuOwner_ = nullptr;
        KillTimer(window_, ChestTimer);
        PostMessageW(window_, WM_NULL, 0, 0);
        if (command == CustomSpeed) {
            OnCommand(command);
        }
    } while (command == CustomSpeed && IsWindow(window_));
    menu_ = nullptr;
    speedMenu_ = keyMenu_ = nullptr;
    KillTimer(window_, MenuPaintTimer);
    menuUpdatePending_ = false;
    if (!command) {
        Shell_NotifyIconW(NIM_SETFOCUS, &icon_);
    }
    DestroyMenu(menu);
    if (command != CustomSpeed) {
        OnCommand(command);
    }
    if (IsWindow(window_)) {
        SyncWork();
    }
}

void App::OnCommand(UINT command) {
    if (command >= Speed10 && command <= Speed100) {
        rate_ = PresetRates[command - Speed10];
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
        case CheckUpdate: StartUpdateCheck(); break;
        case Quit: DestroyWindow(window_); break;
        }
    }
}

std::wstring App::CheckUpdateLabel() const {
    if (checkingUpdate_) {
        return L"检查更新中…";
    }
    if (!latestVersion_.empty()) {
        return L"检查更新（发现 " + latestVersion_ + L"）";
    }
    return L"检查更新";
}

void App::StartUpdateCheck() {
    if (checkingUpdate_ || closing_.load()) {
        return;
    }
    if (updateThread_.joinable()) {
        updateThread_.join();
    }
    checkingUpdate_ = true;
    UpdateMenuStatus();
    const HWND targetWindow = window_;
    updateThread_ = std::thread([this, targetWindow] {
        auto result = updater::CheckLatestRelease();
        {
            std::lock_guard lock(updateMutex_);
            updateResult_ = std::move(result);
        }
        if (!closing_.load()) {
            PostMessageW(targetWindow, UpdateChecked, 0, 0);
        }
    });
}

void App::OnUpdateChecked() {
    if (updateThread_.joinable()) {
        updateThread_.join();
    }
    checkingUpdate_ = false;
    std::optional<updater::Result> result;
    {
        std::lock_guard lock(updateMutex_);
        result = std::exchange(updateResult_, std::nullopt);
    }
    if (!result || closing_.load()) {
        return;
    }
    if (result->state == updater::State::UpdateAvailable) {
        latestVersion_ = result->latestVersion;
    } else if (result->state == updater::State::UpToDate) {
        latestVersion_.clear();
    }
    UpdateMenuStatus();
    if (menu_) {
        EndMenu();
    }
    showingUpdateDialog_ = true;
    SyncWork();
    const std::wstring current = std::wstring(L"v") + POPTAPPO_VERSION_WSTR;
    switch (result->state) {
    case updater::State::UpdateAvailable: {
        const auto text = L"发现新版本 " + result->latestVersion + L"（当前版本 " + current +
            L"）。\n\n是否前往 GitHub Release 页面下载？";
        if (MessageBoxW(nullptr, text.c_str(), AppName, MB_YESNO | MB_ICONINFORMATION | MB_SETFOREGROUND) == IDYES) {
            ShellExecuteW(nullptr, L"open", result->releaseUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        break;
    }
    case updater::State::UpToDate: {
        const auto text = L"当前已是最新版本（" + current + L"）。";
        MessageBoxW(nullptr, text.c_str(), AppName, MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        break;
    }
    case updater::State::NoRelease: {
        const auto text = L"GitHub 仓库暂无已发布的 Release。\n当前版本：" + current;
        MessageBoxW(nullptr, text.c_str(), AppName, MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        break;
    }
    case updater::State::Error: {
        const auto text = L"检查更新失败：" + result->message;
        MessageBoxW(nullptr, text.c_str(), AppName, MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
        break;
    }
    }
    showingUpdateDialog_ = false;
    if (IsWindow(window_)) {
        SyncWork();
    }
}

void App::QueueMenuUpdate() {
    if (menu_ && !menuUpdatePending_) {
        menuUpdatePending_ = SetTimer(window_, MenuPaintTimer, 150, nullptr) != 0;
        if (!menuUpdatePending_) {
            UpdateMenuStatus();
        }
    }
}

std::wstring App::ChestText() const {
    if (!status_.chestStatus.empty()) {
        return status_.chestStatus;
    }
    const auto elapsed = (GetTickCount64() - status_.chestSampleTick) / 1000;
    std::wstring text;
    for (const auto& chest : status_.chests) {
        if (!text.empty()) {
            text += L" / ";
        }
        if (!chest.seconds) {
            text += chest.label;
            continue;
        }
        const auto seconds = *chest.seconds > elapsed ? static_cast<unsigned>(*chest.seconds - elapsed) : 0;
        wchar_t countdown[20]{};
        if (seconds >= 3600) {
            swprintf_s(countdown, L"%02u:%02u:%02u", seconds / 3600, seconds / 60 % 60, seconds % 60);
        } else {
            swprintf_s(countdown, L"%02u:%02u", seconds / 60, seconds % 60);
        }
        text += countdown;
    }
    return text;
}

void App::SyncChestTimer() {
    KillTimer(window_, ChestTimer);
    if (!menu_ || editingRate_ || !status_.chestStatus.empty()) {
        return;
    }
    const auto elapsed = GetTickCount64() - status_.chestSampleTick;
    if (std::ranges::any_of(status_.chests, [elapsed](const ChestDisplay& chest) {
            return chest.seconds && *chest.seconds > elapsed / 1000;
        })) {
        SetTimer(window_, ChestTimer, static_cast<UINT>(1000 - elapsed % 1000), nullptr);
    }
}

void App::UpdateMenuStatus() {
    if (!menu_) {
        return;
    }
    bool changed = UpdateMenuText(menu_, SentStatus,
        TapStatus() + L" · 本次发送 " + std::to_wstring(menuSent_) + L" 次");
    const bool chestChanged = UpdateMenuText(menu_, ChestStatus,
        L"宝箱 · " + ChestText() + L" · 已确认 " + std::to_wstring(status_.confirmed) + L" 个");
    const auto total = menuTotal_ ? std::to_wstring(*menuTotal_) :
        std::wstring(menuSamplePending_ ? L"读取中…" : L"—");
    changed |= UpdateMenuText(menu_, TotalStatus, L"桌宠界面读数 · " + total);
    changed |= UpdateMenuCheck(menu_, ToggleTap, tap_);
    changed |= UpdateMenuCheck(menu_, ToggleChests, open_);
    changed |= UpdateMenuCheck(menu_, ToggleAwake, keepAwake_);
    changed |= UpdateMenuText(menu_, SpeedMenuRow,
        L"连点速度（" + std::to_wstring(rate_) + L" 次/秒）", true);
    UINT selectedSpeed = CustomSpeed;
    for (UINT i = 0; i < ARRAYSIZE(PresetRates); ++i) {
        if (rate_ == PresetRates[i]) {
            selectedSpeed = Speed10 + i;
        }
    }
    const bool speedChanged = UpdateMenuRadio(speedMenu_, Speed10, CustomSpeed, selectedSpeed);
    const bool keyChanged = UpdateMenuRadio(keyMenu_, Key24, Key21, Key24 + key_);
    const bool canPause = tap_ || open_;
    if (((GetMenuState(menu_, PauseAll, MF_BYCOMMAND) & MF_GRAYED) == 0) != canPause) {
        EnableMenuItem(menu_, PauseAll, MF_BYCOMMAND | (canPause ? MF_ENABLED : MF_GRAYED));
        changed = true;
    }
    changed |= UpdateMenuText(menu_, CheckUpdate, CheckUpdateLabel());
    const bool canCheckUpdate = !checkingUpdate_;
    if (((GetMenuState(menu_, CheckUpdate, MF_BYCOMMAND) & MF_GRAYED) == 0) != canCheckUpdate) {
        EnableMenuItem(menu_, CheckUpdate, MF_BYCOMMAND | (canCheckUpdate ? MF_ENABLED : MF_GRAYED));
        changed = true;
    }
    if (changed) {
        DrawMenuBar(window_);
    }
    if (changed || chestChanged) {
        MenuRepaint repaint{menu_, !changed};
        EnumThreadWindows(GetCurrentThreadId(), RepaintPopup, reinterpret_cast<LPARAM>(&repaint));
    }
    if (speedChanged) {
        MenuRepaint repaint{speedMenu_, false};
        EnumThreadWindows(GetCurrentThreadId(), RepaintPopup, reinterpret_cast<LPARAM>(&repaint));
    }
    if (keyChanged) {
        MenuRepaint repaint{keyMenu_, false};
        EnumThreadWindows(GetCurrentThreadId(), RepaintPopup, reinterpret_cast<LPARAM>(&repaint));
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
            const auto oldChest = menu_ ? ChestText() : std::wstring{};
            auto updated = engine_->Status();
            bool menuChanged = status_.petReady != updated.petReady || status_.confirmed != updated.confirmed;
            if (menu_ && menuSamplePending_ && updated.menuSession == menuSession_) {
                menuTotal_ = updated.total;
                menuSamplePending_ = false;
                menuChanged = true;
            }
            status_ = std::move(updated);
            SyncPower();
            if (menu_) {
                if (menuChanged || oldChest != ChestText()) {
                    QueueMenuUpdate();
                }
                SyncChestTimer();
            }
            UpdateTooltip();
        }
        return 0;
    case UpdateChecked:
        OnUpdateChecked();
        return 0;
    case WM_INPUT:
        if (rawKeyboard_) {
            OnInput(reinterpret_cast<HRAWINPUT>(lParam));
        }
        break;
    case WM_MENUSELECT: {
        const auto command = LOWORD(wParam);
        menuSelectedCommand_ = IsOwnMenu(reinterpret_cast<HMENU>(lParam)) &&
            !(HIWORD(wParam) & (MF_POPUP | MF_DISABLED | MF_GRAYED)) &&
            IsInlineCommand(command) ? command : 0;
        break;
    }
    case WM_TIMER:
        if (wParam == TapTimer) {
            Tick();
        } else if (wParam == MenuPaintTimer) {
            KillTimer(window_, MenuPaintTimer);
            menuUpdatePending_ = false;
            UpdateMenuStatus();
        } else if (wParam == ChestTimer) {
            UpdateMenuStatus();
            SyncChestTimer();
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
        closing_.store(true);
        KillTimer(window_, TapTimer);
        KillTimer(window_, MenuPaintTimer);
        KillTimer(window_, ChestTimer);
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
