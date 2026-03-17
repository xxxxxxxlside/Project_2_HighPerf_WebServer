#include "queue_utils.h"

#include <sys/epoll.h>
#include <unistd.h>
#include <iostream>
#include "buffer_utils.h"

void enqueue_ready(int fd, Connection& conn) {
    // =====================【Week2 Day3 新增】======================================
    // closing 状态下不再入队
    if (conn.is_closing || conn.fd_closed) {
        return;
    }
    
    if (!conn.in_ready_queue) {
        ready_queue.push_back(fd);
        conn.in_ready_queue = true;
    }
}

void remove_from_ready_queue(int fd, Connection& conn) {
    if (!conn.in_ready_queue) {
        return;
    }

    std::deque<int> new_queue;
    while(!ready_queue.empty()) {
        int cur = ready_queue.front();
        ready_queue.pop_front();
        if (cur != fd) {
            new_queue.push_back(cur);
        }
    }
    ready_queue.swap(new_queue);
    conn.in_ready_queue = false;
}

void request_close(int epoll_fd, int fd, Connection& conn) {
    if (conn.is_closing && conn.fd_closed) {
        return;
    }

    if (!conn.is_closing) {
        conn.is_closing = true;
    }

    remove_from_ready_queue(fd, conn);

    if (!conn.fd_closed) {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
        conn.fd_closed = true;
        pending_close_queue.push_back(fd);

        std::cout << ">>> [Close Requested] FD: " << fd << std::endl;
    }
}

void flush_pending_close_queue() {
    for (int fd : pending_close_queue) {
        auto it = connections.find(fd);
        if (it != connections.end()) {
            std::cout << ">>> [Final Release] FD: " << fd << std::endl;

            // =====================【Week2 Day4 新增】======================================
            // 真正 erase 前，把这个连接还占着的 buffer 字节数扣掉
            release_connection_buffers(it->second);
            if (conns_current > 0) {
                --conns_current;
            }

            connections.erase(it);
        }
    }
    pending_close_queue.clear();
}
