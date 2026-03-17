#include <iostream>
#include <unistd.h>
#include <sys/epoll.h>

#include "connection.h"
#include "queue_utils.h"
#include "io_utils.h"
#include "http_logic.h"
#include "accept_utils.h"
#include "startup_utils.h"
#include "timer_utils.h"

static const int kMaxEvents = 128;
const int kMaxRequestsPerRound = 5;
const size_t kMaxReadBytesPerEvent = 256 * 1024;
const size_t kMaxWriteBytesPerEvent = 256 * 1024;
const size_t kMaxInflightBytes = 512 * 1024 * 1024;
const int kMaxBodyBytes = 8 * 1024 * 1024;
const size_t kMaxConnections = 200000;

static void flush_write_and_maybe_close(int epoll_fd, int fd, Connection& conn, bool try_write_now) {
    bool io_error = false;

    if (try_write_now && !conn.out_buffer.empty()) {
        write_best_effort_with_budget(fd, conn, io_error);
        update_epoll_events(epoll_fd, fd, conn);
    }

    if (io_error || (conn.is_closing && conn.out_buffer.empty())) {
        request_close(epoll_fd, fd, conn);
    }
}

int main() {
    int listen_fd = -1;
    int epoll_fd = -1;
    init_server(listen_fd, epoll_fd);

    struct epoll_event events[kMaxEvents];

    while (true) {
        int wait_ms = get_next_timer_wait_ms(1000);
        int nfds = epoll_wait(epoll_fd, events, kMaxEvents, wait_ms);

        if (nfds == -1) {
            std::cerr << "epoll_wait failed" << std::endl;
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;

            if (fd == listen_fd) {
                accept_new_connections(epoll_fd, listen_fd);
                continue;
            }

            auto it = connections.find(fd);
            if (it == connections.end()) {
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                close(fd);
                continue;
            }

            Connection& conn = it->second;
            bool io_error = false;

            if (events[i].events & EPOLLIN) {
                read_with_budget(epoll_fd, fd, conn, io_error);
            }

            if (!io_error && (events[i].events & EPOLLOUT)) {
                flush_write_and_maybe_close(epoll_fd, fd, conn, true);
            } else if (io_error || (conn.is_closing && conn.out_buffer.empty())) {
                request_close(epoll_fd, fd, conn);
            }
        }

        size_t round_count = ready_queue.size();
        while (round_count-- > 0 && !ready_queue.empty()) {
            int fd = ready_queue.front();
            ready_queue.pop_front();

            auto it = connections.find(fd);
            if (it == connections.end()) {
                continue;
            }

            Connection& conn = it->second;
            conn.in_ready_queue = false;

            if (conn.is_closing || conn.fd_closed) {
                if (conn.is_closing && conn.out_buffer.empty()) {
                    request_close(epoll_fd, fd, conn);
                }
                continue;
            }

            process_requests_with_limit(epoll_fd, fd, conn);
            flush_write_and_maybe_close(epoll_fd, fd, conn, true);
        }

        int64_t now_ms = now_ms_monotonic();
        auto expired = collect_expired_timers(now_ms);

        for (const auto& node : expired) {
            auto it = connections.find(node.fd);
            if (it == connections.end()) {
                continue;
            }

            Connection& conn = it->second;
            if (conn.is_closing) {
                continue;
            }

            if (node.type == TimerType::HeaderTimeout) {
                if (node.version != conn.header_timer_version) {
                    continue;
                }
                if (conn.header_complete) {
                    continue;
                }

                std::cout << ">>> [Timer] Header timeout, fd=" << node.fd << std::endl;
                request_close(epoll_fd, node.fd, conn);
                continue;
            }

            if (node.type == TimerType::IdleTimeout) {
                if (node.version != conn.idle_timer_version) {
                    continue;
                }

                int64_t idle_expire_ms = conn.last_active_ms + 60 * 1000;
                if (now_ms < idle_expire_ms) {
                    continue;
                }

                std::cout << ">>> [Timer] Idle timeout, fd=" << node.fd << std::endl;
                request_close(epoll_fd, node.fd, conn);
                continue;
            }
        }

        flush_pending_close_queue();
    }

    close(listen_fd);
    close(epoll_fd);
    return 0;
}
