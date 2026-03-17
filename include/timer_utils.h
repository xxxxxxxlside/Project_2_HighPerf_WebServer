#pragma once

#include <cstdint>
#include <queue>
#include <vector>

enum class TimerType {
    HeaderTimeout,
    IdleTimeout
};

struct TimerNode {
    int64_t expire_ms;
    int fd;
    uint64_t version;
    TimerType type;

    bool operator>(const TimerNode& other) const {
        return expire_ms > other.expire_ms;
    }
};

int64_t now_ms_monotonic();

void push_timer(int fd, int64_t expire_ms, uint64_t version, TimerType type);

int get_next_timer_wait_ms(int default_wait_ms = 1000);

std::vector<TimerNode> collect_expired_timers(int64_t now_ms);

void clear_all_timers();