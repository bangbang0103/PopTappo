#include "engine.h"

#include <algorithm>
#include <chrono>
#include <string_view>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

std::optional<unsigned> ParseCountdown(std::wstring_view text) {
    unsigned total{}, part{}, digits{}, separators{};
    for (const auto ch : text) {
        if (ch >= L'0' && ch <= L'9' && digits < 3) {
            part = part * 10 + (ch - L'0');
            ++digits;
        } else if (ch == L':' && digits && separators < 2 &&
                   (!separators || (digits == 2 && part < 60))) {
            total = total * 60 + part;
            part = digits = 0;
            ++separators;
        } else {
            return std::nullopt;
        }
    }
    if (!separators || digits != 2 || part >= 60) {
        return std::nullopt;
    }
    return total * 60 + part;
}

void DescribeChests(Snapshot& status, const tappo::Reading& reading) {
    if (!reading.ok) {
        status.chestStatus = L"无法读取桌宠界面";
        return;
    }
    if (reading.popupVisible) {
        status.chestStatus = L"等待奖励动画";
        return;
    }
    if (reading.chests.empty()) {
        status.chestStatus = L"暂未看到宝箱";
        return;
    }
    status.chestStatus.clear();
    status.chestSampleTick = GetTickCount64();
    for (const auto& chest : reading.chests) {
        switch (chest.state) {
        case tappo::ChestState::Ready:
            status.chests.push_back({L"就绪", {}});
            break;
        case tappo::ChestState::Cooling:
            status.chests.push_back({chest.countdown.empty() ? L"冷却中" : chest.countdown,
                                     ParseCountdown(chest.countdown)});
            break;
        case tappo::ChestState::Unavailable:
            status.chests.push_back({L"暂不可用", {}});
            break;
        }
    }
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

void Engine::Update(bool watch, bool open, DesktopAccess access, unsigned menuSession) {
    {
        std::lock_guard lock(mutex_);
        watch_ = watch;
        open_ = open;
        menuSession_ = menuSession;
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
    std::optional<unsigned> total;
    unsigned confirmed{};
    auto interval = 2s;
    auto nextAttempt = Clock::time_point::min();
    std::wstring retryStatus;
    unsigned observedRevision{};
    unsigned sampledMenu{};
    for (;;) {
        DesktopAccess access;
        bool open;
        unsigned menuSession;
        {
            std::unique_lock lock(mutex_);
            const auto changed = [&] { return stop_ || revision_ != observedRevision; };
            if ((watch_ || menuSession_) && access_ != DesktopAccess::Paused) {
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
            menuSession = menuSession_;
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
        const bool targetChanged = previous.visual != target.visual || previous.process != target.process;
        if (targetChanged) {
            pending.reset();
            nextAttempt = Clock::time_point::min();
            retryStatus.clear();
        }
        if (target.visual && !reader) {
            reader.emplace();
        }
        const bool readTotal = menuSession && menuSession != sampledMenu;
        const auto reading = target.visual ? reader->Read(target, requireVisible, readTotal) : tappo::Reading{};
        if (readTotal) {
            total = reading.total;
        }
        sampledMenu = menuSession;
        Snapshot status;
        status.target = target;
        status.petReady = reading.petFound || target.input;
        status.total = total;
        status.menuSession = menuSession;
        if (target.visual) {
            DescribeChests(status, reading);
        } else {
            status.chestStatus = L"等待 Wuwa Tappo 桌宠";
        }
        interval = open || !target.visual ? 2s : 5s;

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
