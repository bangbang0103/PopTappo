#include "engine.h"

#include <algorithm>
#include <chrono>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

std::wstring Describe(const tappo::Reading& reading) {
    if (!reading.ok) {
        return L"无法读取桌宠界面";
    }
    if (reading.popupVisible) {
        return L"等待奖励动画";
    }
    if (reading.chests.empty()) {
        return L"暂未看到宝箱";
    }
    std::wstring text;
    for (const auto& chest : reading.chests) {
        if (!text.empty()) {
            text += L" / ";
        }
        switch (chest.state) {
        case tappo::ChestState::Ready:
            text += L"就绪";
            break;
        case tappo::ChestState::Cooling:
            text += chest.countdown.empty() ? L"冷却中" : chest.countdown;
            break;
        case tappo::ChestState::Unavailable:
            text += L"暂不可用";
            break;
        }
    }
    return text;
}

struct PendingChest {
    Clock::time_point retryAfter;
    size_t position{};
    size_t count{};
};

}  // namespace

Engine::Engine(HWND receiver) : receiver_(receiver), thread_([this] { Run(); }) {}

Engine::~Engine() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    changed_.notify_one();
    thread_.join();
}

void Engine::Update(bool watch, bool open, DesktopAccess access, bool menuVisible) {
    {
        std::lock_guard lock(mutex_);
        watch_ = watch;
        open_ = open;
        menuVisible_ = menuVisible;
        access_ = access;
        ++revision_;
    }
    changed_.notify_one();
}

Snapshot Engine::Status() {
    std::lock_guard lock(mutex_);
    return status_;
}

bool Engine::MayOpen(const tappo::Window& target) {
    DesktopAccess access;
    {
        std::lock_guard lock(mutex_);
        if (stop_ || !open_ || access_ == DesktopAccess::Paused) {
            return false;
        }
        access = access_;
    }
    const bool locked = access == DesktopAccess::Locked;
    return (locked || (tappo::IsUserDesktopActive() && !tappo::IsGestureActive())) &&
        tappo::IsSameWindow(target, !locked);
}

void Engine::Run() {
    std::optional<tappo::Reader> reader;
    std::optional<PendingChest> pending;
    tappo::Window target;
    unsigned confirmed{};
    auto interval = 2s;
    auto nextAttempt = Clock::time_point::min();
    std::wstring retryStatus;
    unsigned observedRevision{};
    for (;;) {
        DesktopAccess access;
        bool open;
        bool menuVisible;
        {
            std::unique_lock lock(mutex_);
            const auto changed = [&] { return stop_ || revision_ != observedRevision; };
            if ((watch_ || menuVisible_) && access_ != DesktopAccess::Paused) {
                changed_.wait_for(lock, interval, changed);
            } else {
                changed_.wait(lock, changed);
            }
            if (stop_) {
                return;
            }
            observedRevision = revision_;
            access = access_;
            open = open_;
            menuVisible = menuVisible_;
        }

        if (access == DesktopAccess::Paused ||
            (access == DesktopAccess::Interactive && !tappo::IsUserDesktopActive())) {
            continue;
        }
        const bool requireVisible = access == DesktopAccess::Interactive;
        const auto previous = target;
        const auto discovered = tappo::FindTarget();
        if (discovered.visual || requireVisible || !tappo::IsSameWindow(previous, requireVisible)) {
            target = discovered;
        }
        if (previous.visual != target.visual || previous.process != target.process) {
            pending.reset();
            nextAttempt = Clock::time_point::min();
            retryStatus.clear();
        }
        if (target.visual && !reader) {
            reader.emplace();
        }
        const auto reading = target.visual ? reader->Read(target, requireVisible) : tappo::Reading{};
        Snapshot status;
        status.target = target;
        status.petReady = reading.petFound || target.input;
        status.total = reading.total;
        status.chestStatus = target.visual ? Describe(reading) : L"等待 Wuwa Tappo 桌宠";
        interval = menuVisible ? 1s : (open || !target.visual ? 2s : 5s);

        const auto ready = [](const tappo::Chest& chest) {
            return chest.state == tappo::ChestState::Ready;
        };
        const auto now = Clock::now();
        if (pending && reading.ok) {
            const bool cooling = reading.chests.size() == pending->count &&
                reading.chests[pending->position].state == tappo::ChestState::Cooling;
            if (reading.popupVisible || cooling) {
                ++confirmed;
                pending.reset();
            } else if (now >= pending->retryAfter) {
                pending.reset();
                nextAttempt = now + 4s;
                retryStatus = L"上次开箱未确认，等待重试";
            }
        }
        const auto chest = std::ranges::find_if(reading.chests, ready);
        if (pending) {
            status.chestStatus = L"已提交，等待桌宠确认";
        } else if (open && now < nextAttempt && !retryStatus.empty() && chest != reading.chests.end()) {
            status.chestStatus = retryStatus;
        } else if (open && !reading.popupVisible && now >= nextAttempt &&
                   chest != reading.chests.end() && MayOpen(target)) {
            const auto result = reader->Open(*chest);
            const auto finished = Clock::now();
            if (SUCCEEDED(result)) {
                const auto position = static_cast<size_t>(chest - reading.chests.begin());
                pending = PendingChest{finished + 10s, position, reading.chests.size()};
                nextAttempt = finished + 4s;
                retryStatus.clear();
                status.chestStatus = L"已提交，等待桌宠确认";
            } else {
                nextAttempt = finished + 12s;
                wchar_t code[16]{};
                swprintf_s(code, L"0x%08lX", static_cast<unsigned long>(result));
                retryStatus = std::wstring(L"开箱调用失败 ") + code;
                status.chestStatus = retryStatus;
            }
        }
        status.confirmed = confirmed;
        {
            std::lock_guard lock(mutex_);
            status_ = std::move(status);
        }
        PostMessageW(receiver_, Updated, 0, 0);
    }
}
