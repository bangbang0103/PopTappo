#pragma once

#include "tappo.h"

#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

enum class DesktopAccess { Paused, Interactive, Locked };

struct ChestDisplay {
    std::wstring label;
    std::optional<unsigned> seconds;
};

struct Snapshot {
    tappo::Window target;
    bool petReady{};
    std::optional<unsigned> total;
    unsigned menuSession{};
    std::wstring chestStatus = L"等待检测";
    std::vector<ChestDisplay> chests;
    ULONGLONG chestSampleTick{};
    unsigned confirmed{};
};

class Engine {
public:
    explicit Engine(HWND receiver);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void Update(bool watch, bool open, DesktopAccess access, unsigned menuSession);
    Snapshot Status();

    static constexpr UINT Updated = WM_APP + 2;

private:
    void Run();
    bool MayOpen(const tappo::Window& target);

    HWND receiver_;
    std::mutex mutex_;
    std::condition_variable changed_;
    Snapshot status_;
    bool watch_{};
    bool open_{};
    unsigned menuSession_{};
    DesktopAccess access_ = DesktopAccess::Paused;
    bool stop_{};
    unsigned revision_{};
    std::thread thread_;
};
