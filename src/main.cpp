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
// [Day 4 新增] 引入 string 和 unordered_map
#include <string>
#include <unordered_map>
#include <sstream>        // [Day 5 新增] 用于字符串串流解析，但我们暂用基础的 find 即可
#include <deque>
#include <cstdlib>
#include <vector>       // =====================【Week2 Day3 新增】=====================
// 用于从 ReadyQueue 中移除 fd. 以及统一处理 pending_close_queue
// ======= [Week2 Day5 新增开始: 头文件] ================
#include <list>         // LRU 链表
#include <chrono>       // Token Bucket 时间计算
// ======= [Week2 Day5 新增结束: 头文件] ========================
#include "ip_rate_limit.h"
#include "connection.h"
#include "buffer_utils.h"
#include "queue_utils.h"
#include "io_utils.h"
#include "http_logic.h"
#include "accept_utils.h"

// ============================================================================
// 宏定义区域：错误处理工具
// ============================================================================
// 这是一个调试神器：如果系统调用返回值 < 0 (表示失败)，自动打印错误信息并退出
// expr: 要执行的表达式 (如 socket(...))
// msg: 自定义的错误提示文字
#define CHECK_RET(expr, msg) \
    if ((expr) < 0) { \
        std::cerr << "Error: " << msg << " (errno: " << errno << ")" << std::endl; \
        return -1; \
    }
// ===========================================================================
// 常量定义
// ===========================================================================
static const int kMaxEvents = 128;
const int kMaxRequestsPerRound = 5;

// ===========================================================================
// 【Week2 Day2 新增】
// ===========================================================================
// 单轮读预算：最多读取 256KB
const size_t kMaxReadBytesPerEvent = 256 * 1024;

// 单轮写预算：最多写出 256KB
const size_t kMaxWriteBytesPerEvent = 256 * 1024;

// =====================【Week2 Day4 新增】======================================
// 全局 inflight 内存上限: 512MB
const size_t kMaxInflightBytes = 512 * 1024 *1024;

// 单请求 body 上限: 8MB
const int kMaxBodyBytes = 8 * 1024 * 1024;


// ======= [Week2 Day6 新增开始: 连接数上限常量] ================
const size_t kMaxConnections = 200000;          // [Day6新增] 全局最大连接数 20 万

// =============================================================================
// 主函数
//
// =============================================================================

int main() {
    // =========================================================================
    // 1. 创建监听 Socket (创建“听筒”)
    // =========================================================================
    // AF_INET: 使用 IPv4 协议族 (Address Family Internet)
    // SOCK_STREAM: 使用 TCP 协议 (提供可靠的、面向连接的字节流)
    // 0: 让系统自动选择默认的协议 (对于 TCP 就是 IPPROTO_TCP)
    // 返回值 listen_fd: 文件描述符 (File Descriptor)，相当于这个 socket 的“身份证号”
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    
    // 检查创建是否成功，如果失败 (返回-1)，宏会自动打印错误并退出
    CHECK_RET(listen_fd, "create socket failed");

    // =========================================================================
    // 第二步：设置 Socket 选项 (获取“特权”)
    // =========================================================================
    int opt = 1; // 选项值，1 表示开启/真
    
    // setsockopt: 设置 Socket 选项
    // 参数1: listen_fd (操作哪个socket)
    // 参数2: SOL_SOCKET (选项层级：Socket 层级)
    // 参数3: SO_REUSEADDR (选项名称：地址复用)
    //        【核心原理】：默认情况下，服务器关闭后端口会进入 TIME_WAIT 状态 (约2分钟)，期间无法再次绑定。
    //        开启此选项后，允许立即绑定处于 TIME_WAIT 状态的端口，方便开发调试重启。
    // 参数4: &opt (选项值的指针)
    // 参数5: sizeof(opt) (选项值的长度)
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "setsockopt failed" << std::endl;
        return -1;
    }

    // =========================================================================
    // 第三步：初始化地址结构体 (准备“信封”)
    // =========================================================================
    struct sockaddr_in server_addr; // 定义一个 IPv4 专用的地址结构体变量
    
    // 【关键步骤】：内存清零
    // memset: 将内存块设置为指定值
    // &server_addr: 目标内存地址
    // 0: 设置为 0 (二进制全0)
    // sizeof(server_addr): 要清零的字节数
    // 【为什么要做？】：结构体中可能包含未初始化的随机垃圾数据，清零能保证所有字段初始状态安全。
    memset(&server_addr, 0, sizeof(server_addr));
    
    server_addr.sin_family = AF_INET;             // 指定协议族：IPv4 (必须与 socket() 一致)
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(8080);
    CHECK_RET(bind(listen_fd, (struct sockaddr*)&server_addr,sizeof(server_addr)),"bind failed");
    CHECK_RET(listen(listen_fd, 128), "listen failed");
    // ======= [Week2 Day5 必要修复开始: listen_fd 非阻塞] ================
    // 现在 accept 分支已经改成 while(true) 循环 accept
    // 如果 listen_fd 仍然是阻塞的，那么队列取空后下一次 accept 会直接卡住
    int listen_flags = fcntl(listen_fd, F_GETFL, 0);
    CHECK_RET(listen_flags, "fcntl(F_GETFL) on listen_fd failed");
    CHECK_RET(fcntl(listen_fd, F_SETFL, listen_flags | O_NONBLOCK), "fcntl(F_SETFL) on listen_fd failed");
    // ======= [Week2 Day5 必要修复结束: listen_fd 非阻塞] ================

    std::cout << ">>> Server started! Switching to Epoll mode ..." << std::endl;
    
    // =========================================================
    // 2. 创建 epoll
    // =========================================================
    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        std::cerr << "epoll_createl failed" << std::endl;
        return -1;
    }

    struct epoll_event event;
    event.events = EPOLLIN;
    event.data.fd = listen_fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &event) == -1) {
        std::cerr << "epoll_ctl ADD listen_fd failed" << std::endl;
        return -1;
    }
    std::cout << ">>> Epoll initiallized. Listening for events..." << std::endl;
    
    // ==========================================================
    // 3. 事件循环 (Event Loop)
    // ==========================================================
    struct epoll_event events[kMaxEvents]; // 用于存放就绪的事件数组

    while (true) {
        // 【核心】等待事件发生
        // 参数：
        // 1. epoll_fd: 监听哪个 epoll 实例
        // 2. events: 数组，用来存“谁准备好了”
        // 3. 128: 最多存多少个事件
        // 4. -1: 无限等待 (阻塞), 直到有事件发生
        int nfds = epoll_wait(epoll_fd, events, kMaxEvents, -1);

        if (nfds == -1) {
            std::cerr << "epoll_wait failed" << std::endl;
            break;
        }

        // 遍历所有就绪的事件
        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;
            // ------------------------------------------------------
            // A. 新连接
            // ------------------------------------------------------
            if (fd == listen_fd) {
                // ===== [Week2 Day5 修改开始：accept后增加IP限流] =====
                accept_new_connections(epoll_fd, listen_fd);
                // ===== [Week2 Day5 修改结束：accept后增加IP限流] =====
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
            // ===================【Week2 Day2 修改】====================
            // 可读事件: 改成"带预算循环读"
            if (events[i].events & EPOLLIN) {
                read_with_budget(epoll_fd, fd, conn, io_error);
            }
            // 可写事件
            if (!io_error && (events[i].events & EPOLLOUT)) {
                write_best_effort_with_budget(fd, conn, io_error);
                update_epoll_events(epoll_fd, fd, conn);
            }
            // =====================【Week2 Day3 修改】======================================
            // 不再直接 close_connection
            // 改成统一走 request_close
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

            // =====================【Week2 Day3 修改】======================================
            // 出队后先清标记
            conn.in_ready_queue = false;
            // =====================【Week2 Day3 新增】======================================
            // 如果已经进入关闭态或 fd 已关闭，跳过
            if (conn.is_closing || conn.fd_closed) {
                if (conn.is_closing && conn.out_buffer.empty()) {
                    request_close(epoll_fd, fd, conn);
                }
                continue;

            }

            process_requests_with_limit(epoll_fd, fd, conn);

            bool io_error = false;
            if (!conn.out_buffer.empty()) {
                // ================【week2 Day2 修改】=======================
                // Readyqueue 后续写回，也要受单轮写预算限制
                write_best_effort_with_budget(fd, conn, io_error);
                update_epoll_events(epoll_fd, fd, conn);
            }
            // =====================【Week2 Day3 修改】======================================
            if (io_error || (conn.is_closing && conn.out_buffer.empty())) {
                request_close(epoll_fd, fd, conn);
            }
        }

        // =====================【Week2 Day3 新增】======================================
        // 本轮事件循环最后，统一真正释放连接对象
        flush_pending_close_queue();
    }

    close (listen_fd);
    close (epoll_fd);
    return 0;
}
