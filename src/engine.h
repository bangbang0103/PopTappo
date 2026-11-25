#pragma once

#include "tappo.h"

#include <condition_variable>
#include <mutex>
#include <thread>

enum class DesktopAccess { Paused, Interactive, Locked };

struct Snapshot {
    tappo::Window target;
    bool petReady{};
    std::optional<unsigned> total;
    std::wstring chestStatus = L"等待检测";
    unsigned confirmed{};
};

class Engine {
public:
    explicit Engine(HWND receiver);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void Update(bool watch, bool open, DesktopAccess access, bool menuVisible);
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
    bool menuVisible_{};
    DesktopAccess access_ = DesktopAccess::Paused;
    bool stop_{};
    unsigned revision_{};
    std::thread thread_;
};
