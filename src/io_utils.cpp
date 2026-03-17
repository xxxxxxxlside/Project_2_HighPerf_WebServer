#include "io_utils.h"
#include "buffer_utils.h"
#include "timer_utils.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <iostream>
#include <cerrno>


void update_epoll_events(int epoll_fd, int fd, Connection& conn) {
    // =====================【Week2 Day3 新增】======================================
    // 只有 fd 真正关闭后，才不再修改 epoll 事件
    if (conn.fd_closed) {
        return;
    }
    
    struct epoll_event ev;
    ev.data.fd = fd;
    ev.events = EPOLLIN;

    // 只要还有数据没发完，就继续关心 EPOLLOUT
    if (!conn.out_buffer.empty()) {
        ev.events |= EPOLLOUT;
    }

    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev) == -1) {
        std::cerr << "epoll_ctl MOD failed for fd =" << fd << " errno=" << errno << std::endl;
    }
}

void write_best_effort_with_budget(int fd, Connection& conn, bool& io_error) {
    
    ssize_t written_this_round = 0;

    while (!conn.out_buffer.empty() && written_this_round < kMaxWriteBytesPerEvent) {
        size_t remain_budget = kMaxWriteBytesPerEvent - written_this_round;
        size_t try_write = conn.out_buffer.size();

        if (try_write > remain_budget) {
            try_write = remain_budget;
        }

        ssize_t w = send(fd,
                        conn.out_buffer.data(),
                        try_write,
                        MSG_NOSIGNAL);
        if (w > 0) {
            int64_t now_ms = now_ms_monotonic();
            conn.last_active_ms = now_ms;

            ++conn.idle_timer_version;
            push_timer(fd, now_ms + 60 * 1000,conn.idle_timer_version, TimerType::IdleTimeout);
            // =====================【Week2 Day4 修改】======================================
            // 不再直接 erase. 改成统一扣减 inflight 计数
            erase_from_out_buffer_prefix(conn, static_cast<size_t>(w));

            written_this_round += static_cast<size_t>(w);
        } else if (w == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // 本轮先停，等下一次 EPOLLOUT
            break;
        } else {
            io_error = true;
            break;
        }
    }
    
}
