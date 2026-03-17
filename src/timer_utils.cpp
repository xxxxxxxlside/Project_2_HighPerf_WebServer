#include "timer_utils.h"

#include <chrono>
#include <queue>
#include <vector>
#include <functional>

namespace {
// 优先使用队列 (最小堆) 来管理定时器，过期时间最早的在堆顶
std::priority_queue<
    TimerNode,
    std::vector<TimerNode>,
    std::greater<TimerNode>
> g_timer_heap;
}

// 获取当前单调时钟的毫秒数
int64_t now_ms_monotonic() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()
    ).count();
}

// 推入一个新的定时器节点
void push_timer(int fd, int64_t expire_ms, uint64_t version, TimerType type) {
    g_timer_heap.push(TimerNode{expire_ms, fd, version, type});
}

// 获取距离下一个定时器过期的等待时间 (毫秒)
// 如果堆为空，返回默认等待时间
// 如果下一个定时器已经过期，返回 0
// 否则返回距离过期的时间差 (不超过 default_wait_ms) 
int get_next_timer_wait_ms (int default_wait_ms) {
    if (g_timer_heap.empty()) {
        return default_wait_ms;
    }

    int64_t now = now_ms_monotonic();
    int64_t diff = g_timer_heap.top().expire_ms - now;

    if (diff <= 0) {
        return 0; // 已经过期，立即返回
    }

    if (diff > default_wait_ms) {
        return default_wait_ms; // 超过默认等待时间，只等待默认时间
    }

    return static_cast<int>(diff);
}

// 收集所有已过期的定时器节点
std::vector<TimerNode> collect_expired_timers(int64_t now_ms) {
    std::vector<TimerNode> expired;

    // 只要堆顶元素过期， 就弹出并加入结果集
    while (!g_timer_heap.empty() && g_timer_heap.top().expire_ms <= now_ms) {
        expired.push_back(g_timer_heap.top());
        g_timer_heap.pop();
    }

    return expired;
}

// 清空所有定时器 (通常在服务器关闭或重置时使用)
void clear_all_timers() {
    while (!g_timer_heap.empty()){
        g_timer_heap.pop();
    }
}
