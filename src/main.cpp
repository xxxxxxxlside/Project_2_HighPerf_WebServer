// ============================================================================
// 头文件区域：引入工具包
// ============================================================================
#include <iostream>       // [IO Stream] C++标准输入输出流 (用于 std::cout 打印日志)
#include <cstring>        // [C String] C风格字符串处理库 (核心函数: memset 用于内存清零)
#include <unistd.h>       // [Unix Standard] Linux/Unix 系统核心API (核心函数: close, sleep, read, write)
#include <arpa/inet.h>    // [ARPA Internet] 互联网协议操作库 (核心函数: htons, htonl, inet_addr)
                          // 注意：这里主要做 IP地址 和 端口号 的格式转换
#include <sys/socket.h>   // [System Socket] 套接字核心库 (核心函数: socket, bind, listen, accept)
                          // 这是网络编程最核心的头文件
#include <fcntl.h>        // [File Control] 文件控制库 (核心函数: fcntl)
                          // 用于设置文件描述符的属性，比如最重要的“非阻塞模式”
#include <sys/epoll.h>    // [新增] Epoll 核心头文件
#include <errno.h>
#include <string>
#include <unordered_map>
#include <sstream>        // [Day 5 新增] 用于字符串串流解析，但我们暂用基础的 find 即可
#include <deque>
#include <cstdlib>
#include <vector>       
// 用于从 ReadyQueue 中移除 fd. 以及统一处理 pending_close_queue

#include <list>         // LRU 链表
#include <chrono>       // Token Bucket 时间计算
#include "ip_rate_limit.h"
#include "connection.h"
#include "buffer_utils.h"
#include "queue_utils.h"
#include "io_utils.h"
#include "http_logic.h"
#include "accept_utils.h"
#include "startup_utils.h"

// ===========================================================================
// 常量定义
// ===========================================================================
static const int kMaxEvents = 128;
const int kMaxRequestsPerRound = 5;

// 单轮读预算：最多读取 256KB
const size_t kMaxReadBytesPerEvent = 256 * 1024;

// 单轮写预算：最多写出 256KB
const size_t kMaxWriteBytesPerEvent = 256 * 1024;

// 全局 inflight 内存上限: 512MB
const size_t kMaxInflightBytes = 512 * 1024 *1024;

// 单请求 body 上限: 8MB
const int kMaxBodyBytes = 8 * 1024 * 1024;

// ======= [Week2 Day6 新增开始: 连接数上限常量] ================
const size_t kMaxConnections = 200000;          // [Day6新增] 全局最大连接数 20 万

int main() {
    
    int listen_fd = -1;
    int epoll_fd = -1;
    init_server(listen_fd, epoll_fd);

    struct epoll_event events[kMaxEvents]; // 用于存放就绪的事件数组

    while (true) {
    
        int nfds = epoll_wait(epoll_fd, events, kMaxEvents, -1);

        if (nfds == -1) {
            std::cerr << "epoll_wait failed" << std::endl;
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;
            // ------------------------------------------------------
            // A. 新连接
            // ------------------------------------------------------
            if (fd == listen_fd) {
                accept_new_connections(epoll_fd, listen_fd);
                continue;
            } 
            // --------------------------------------------------
            // 情况 B: 客户端数据达到 (Day 4 彻底重写)
            // --------------------------------------------------
                
            auto it = connections.find(fd);
            if (it == connections.end()) {
                // [Day 6 新增]: 补充清理和跳过逻辑，防崩溃
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                close(fd);
                continue;
            }
                
            Connection& conn = it ->second;
            bool io_error = false;
            if (events[i].events & EPOLLIN) {
                read_with_budget(epoll_fd, fd, conn, io_error);
            }

            if (!io_error && (events[i].events & EPOLLOUT)) {
                write_best_effort_with_budget(fd, conn, io_error);
                update_epoll_events(epoll_fd, fd, conn);
            }

            if (io_error || (conn.is_closing && conn.out_buffer.empty())){
                request_close(epoll_fd, fd, conn);
            }     
        }
        // -------------------------------------------------------------
        // C. Week2 Day1 核心: 主动消费 ReadyQueue
        // 这一轮 epoll 事件处理完后，继续处理"已有完整请求但被预算打断"的连接
        // -------------------------------------------------------------

        size_t round_count = ready_queue.size();
        while (round_count--> 0 && !ready_queue.empty()) {
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

            bool io_error = false;
            if (!conn.out_buffer.empty()) {
                write_best_effort_with_budget(fd, conn, io_error);
                update_epoll_events(epoll_fd, fd, conn);
            }
            if (io_error || (conn.is_closing && conn.out_buffer.empty())) {
                request_close(epoll_fd, fd, conn);
            }
        }

        flush_pending_close_queue();
    }

    close (listen_fd);
    close (epoll_fd);
    return 0;
}
