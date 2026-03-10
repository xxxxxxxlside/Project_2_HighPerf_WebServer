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
static const int kMaxRequestsPerRound = 5;

// ===========================================================================
// 【Week2 Day2 新增】
// ===========================================================================
// 单轮读预算：最多读取 256KB
static const size_t kMaxReadBytesPerEvent = 256 * 1024;

// 单轮写预算：最多写出 256KB
static const size_t kMaxWriteBytesPerEvent = 256 * 1024;

// =====================【Week2 Day4 新增】======================================
// 全局 inflight 内存上限: 512MB
static const size_t kMaxInflightBytes = 512 * 1024 *1024;

// 单请求 body 上限: 8MB
static const int kMaxBodyBytes = 8 * 1024 * 1024;

// ======= [Week2 Day5 新增开始: IP限流常量] ================
static const size_t kIpBucketMaxEntries = 100000;       // [Day5新增] 最多记录 10 万个 IP
static const int kIpBucketTtlSeconds = 600;             // [Day5新增] 10 分钟没访问就过期
static const double kIpBucketCapcity = 200.0;           // [Day5新增] 桶容量 200
static const double kIpBucketRefillPerSec = 50.0;       // [Day5新增] 每秒补充 50 个 token
// 本地测试时可临时改成：
// static const double kIpBucketCapacity = 20.0;
// static const double kIpBucketRefillPerSec = 1.0;
// ======= [Week2 Day5 新增结束: IP限流常量] ================
// ============================================================================
// 连接结构
// ============================================================================

// 连接状态结构体
struct Connection {
    std::string in_buffer; // 动态缓冲区，累积数据
    // [Day 6 新增]: 输出缓冲区。所有要发给客户端的数据，必须先放进这里排队，决不允许直接 write
    std::string out_buffer;
    
    // [Day 6 终极修复]: 必须把关闭状态绑定在连接对象上，防止异步写回期间局部变量丢失导致假死
    bool is_closing;
    bool in_ready_queue;
    // ===================【Week2 Day3 新增】 ================
    // 标记这个 fd 是否已经做过 close(fd)
    // 防止重复 close
    bool fd_closed;
    Connection() : is_closing(false), in_ready_queue(false), fd_closed(false) {}
};

// 全局地图：fd -> Connection 对象
std::unordered_map<int, Connection> connections;

// ReadyQueue: 存“还有完整请求没处理完，但这轮预算已用完”的 fd
std::deque<int> ready_queue; 

// =====================【Week2 Day3 新增】======================================
// 延迟释放队列：本轮里只登记，等本轮末尾再统一 erase
std::vector<int> pending_close_queue;

// =====================【Week2 Day4 新增】======================================
// 全局 inflight 字节计数器
size_t global_inflight_bytes = 0;

// ======= [Week2 Day5 新增开始: IP桶结构和全局变量] ================
struct IpBucket {
    double tokens;      // [Day5新增] 当前剩余 token
    std::chrono::steady_clock::time_point last_refill; // [Day5新增] 上次补充时间
    std::chrono::steady_clock::time_point last_seen;   // [Day5新增] 上次访问时间
    std::list<std::string>::iterator lru_it;           // [Day5新增] 在 LRU 链表中的位置
};

std::unordered_map<std::string, IpBucket> ip_buckets;  // [Day5新增] IP -> Bucket
std::list<std::string> ip_lru;                         // [Day5新增] LRU 链表：头新尾旧
// ======= [Week2 Day5 新增结束: IP桶结构和全局变量] ================

// ============================================================================
// 工具函数
// ============================================================================

// 判断 in_buffer 里是否至少有一个完整 HTTP 请求头
bool has_complete_request(const Connection& conn) {
    return conn.in_buffer.find("\r\n\r\n") != std::string::npos;
}

// =====================【Week2 Day4 新增】======================================
// 统一追加到 in_buffer, 并维护 global_inflight_bytes
bool append_to_in_buffer(Connection& conn, const char* data, size_t len) {
    if (global_inflight_bytes + len > kMaxInflightBytes) {
        return false;
    }

    conn.in_buffer.append(data, len);
    global_inflight_bytes += len;
    return true;
}

// =====================【Week2 Day4 新增】======================================
// 统一追加到 out_buffer, 并维护 global_inflight_bytes
bool append_to_out_buffer(Connection& conn, const std::string& data) {
    if (global_inflight_bytes + data.size() > kMaxInflightBytes) {
        return false;
    }

    conn.out_buffer += data;
    global_inflight_bytes += data.size();
    return true;
}

// =====================【Week2 Day4 新增】======================================
// 从 in_buffer 删除前缀，并维护 global_inflight_bytes
void erase_from_in_buffer_prefix(Connection& conn, size_t len) {
    if (len > conn.in_buffer.size()) {
        len = conn.in_buffer.size();
    }

    conn.in_buffer.erase(0, len);
    global_inflight_bytes -= len;
}

// =====================【Week2 Day4 新增】======================================
// 从 out_buffer 删除前缀，并维护 global_inflight_bytes
void erase_from_out_buffer_prefix(Connection& conn, size_t len) {
    if (len > conn.out_buffer.size()) {
        len = conn.out_buffer.size();
    }

    conn.out_buffer.erase(0, len);
    global_inflight_bytes -= len;
}

// =====================【Week2 Day4 新增】======================================
// 释放某个连续剩余 buffer 占用的 inflight 计数
void release_connection_buffers(Connection& conn) {
    global_inflight_bytes -= conn.in_buffer.size();
    global_inflight_bytes -= conn.out_buffer.size();
    conn.in_buffer.clear();
    conn.out_buffer.clear();
}

// 更新 epoll 里这个 fd 关心的事件
void update_epoll_events(int epoll_fd, int fd, const Connection& conn) {
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
// =====================【Week2 Day3 新增】======================================
// 从 ReadyQueue 中摘掉指定 fd
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

// =====================【Week2 Day3 新增】======================================
// 唯一关闭入口：只负责“发起关闭”
// 流程：closing-true -> 从 ReadyQueue 摘除 -> epoll DEL -> close(fd)
// -> 标记 fd_closed -> 放入 pending_close_queue 等待本轮末尾统一 erase
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

// =====================【Week2 Day3 新增】======================================
// 本轮末尾统一真正释放 connection 对象
void flush_pending_close_queue() {
    for (int fd : pending_close_queue) {
        auto it = connections.find(fd);
        if (it != connections.end()) {
            std::cout << ">>> [Final Release] FD: " << fd << std::endl;

            // =====================【Week2 Day4 新增】======================================
            // 真正 erase 前，把这个连接还占着的 buffer 字节数扣掉
            release_connection_buffers(it->second);

            connections.erase(it);
        }
    }
    pending_close_queue.clear();
}


// 入队 ReadyQueue (防止重复入队)
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

// ======= [Week2 Day5 新增开始: 更新LRU位置] ================
void touch_ip_bucket_lru(const std::string& ip) {
    auto it = ip_buckets.find(ip);
    if (it == ip_buckets.end()) {
        return;
    }

    ip_lru.erase(it->second.lru_it);
    ip_lru.push_front(ip);
    it->second.lru_it = ip_lru.begin();
}
// ======= [Week2 Day5 新增结束: 更新LRU位置] ================

// ======= [Week2 Day5 新增开始: 清理过期IP桶] ================
void cleanup_expired_ip_buckets() {
    auto now = std::chrono::steady_clock::now();

    while (!ip_lru.empty()) {
        const std::string& oldest_ip = ip_lru.back();
        auto it = ip_buckets.find(oldest_ip);
        if (it == ip_buckets.end()) {
            ip_lru.pop_back();
            continue;
        }

        auto idle_sec = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.last_seen).count();
        if (idle_sec < kIpBucketTtlSeconds) {
            break;
        }

        ip_lru.pop_back();
        ip_buckets.erase(it);
    }
}
// ======= [Week2 Day5 新增结束: 清理过期IP桶] ================

// ======= [Week2 Day5 新增开始: LRU淘汰超限IP] ================
void evict_ip_buckets_if_needed() {
    while (ip_buckets.size() > kIpBucketMaxEntries && !ip_lru.empty()) {
        std::string oldest_ip = ip_lru.back();
        ip_lru.pop_back();
        ip_buckets.erase(oldest_ip);
    }
}
// ======= [Week2 Day5 新增结束: LRU淘汰超限IP] ================

// ======= [Week2 Day5 新增开始: 补充token] ================
void refill_ip_bucket(IpBucket& bucket, const std::chrono::steady_clock::time_point& now) {
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - bucket.last_refill).count();

    if (elapsed_ms <= 0) {
        return;
    }

    double add_tokens = (elapsed_ms / 1000.0) * kIpBucketRefillPerSec;
    bucket.tokens += add_tokens;
    if (bucket.tokens > kIpBucketCapcity) {
        bucket.tokens = kIpBucketCapcity;
    }

    bucket.last_refill = now;
}
// ======= [Week2 Day5 新增结束: 补充token] ================

// ======= [Week2 Day5 新增开始: 消耗token并判断是否放行] ================
bool consume_ip_token(const std::string& ip) {
    cleanup_expired_ip_buckets();

    auto now = std::chrono::steady_clock::now();
    auto it = ip_buckets.find(ip);

    // 第一次看到这个 IP: 创建一个满桶
    if (it == ip_buckets.end()) {
        ip_lru.push_front(ip);

        IpBucket bucket;
        bucket.tokens = kIpBucketCapcity;
        bucket.last_refill = now;
        bucket.last_seen = now;
        bucket.lru_it = ip_lru.begin();

        auto ret = ip_buckets.emplace(ip, bucket);
        it = ret.first;

        evict_ip_buckets_if_needed();
    }

    IpBucket& bucket = it->second;
    refill_ip_bucket(bucket, now);
    bucket.last_seen = now;
    touch_ip_bucket_lru(ip);

    if (bucket.tokens < 1.0) {
        return false;
    }

    bucket.tokens -= 1.0;
    return true;
}
// ======= [Week2 Day5 新增结束: 消耗token并判断是否放行] ================

// ======= [Week2 Day5 新增开始: 429拒绝函数] ================
void reject_new_connection_with_429(int fd) {
    const char* resp =
        "HTTP/1.1 429 Too Many Requests\r\n"
        "Content-Length: 17\r\n"
        "Connection: close\r\n"
        "\r\n"
        "Too Many Requests";

    send(fd, resp, std::strlen(resp), MSG_NOSIGNAL);
    close(fd);
}
// ======= [Week2 Day5 新增结束: 429拒绝函数] ================


// 业务处理:构造响应
void handle_request(int fd, Connection& conn, const std::string& request_line) {
   
    std::cout << ">>> [Request] FD " << fd << " Method-Line: " << request_line << std::endl;

    std::string resp = 
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 2\r\n"
        "Connection: close\r\n"
        "\r\n"
        "OK";
    
    // =====================【Week2 Day4 新增】======================================
    // 不再直接 out_buffer += resp
    // 改成统一走 append_to_out_buffer,顺带做 inflight budget 检查
    if (!append_to_out_buffer(conn, resp)) {
        std::cerr << ">>> [Inflight Budget] Response append exceeds limit." << "Sending 503..." << std::endl;
        conn.out_buffer.clear(); // 这里 clear 不需要扣计数，因为 append 失败根本没加进去

        std::string err_resp =
            "HTTP/1.1 503 Service Unavailable\r\n"
            "Connection: Close\r\n"
            "\r\n";
        append_to_out_buffer(conn, err_resp);
    }

    
    conn.is_closing = true;
}

// =============================【Week Day2 修改】=============================================
// 改成"代写预算"的 best-effort write
// 一次可写事件里，最多写 kMaxWriteBytesPerEvent

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

// =============================================================================
// Week2 Day1 核心: 单轮最多处理 5 个完整请求
// 如果还有完整请求残留，则放入 ReadyQueue 等待下一轮继续处理
// =============================================================================
void process_requests_with_limit(int epoll_fd, int fd, Connection& conn) {
    int processed = 0;

    while (!conn.is_closing && processed < kMaxRequestsPerRound) {
        std::size_t pos = conn.in_buffer.find("\r\n\r\n");
        if (pos == std::string::npos) {
            break;
        }

        // Header 总长度
        int header_total_len = static_cast<int>(pos + 4);
        std::string headers = conn.in_buffer.substr(0, pos);

        // 1. Request Line
        std::size_t line_end = headers.find("\r\n");
        std::string request_line;
        if (line_end != std::string::npos) {
            request_line = headers.substr(0, line_end);
            std::cout << ">>> [Parse] FD " << fd << " Request: " << request_line << std::endl;
        }

        // 2. 防御: 拒绝 chunked
        if (headers.find("Transfer-Encoding: chunked") != std::string::npos || headers.find("Transfer-encoding: Chunked") != std::string::npos) {
            std::cerr << ">>> [DoS Defense] Chunked not supported on FD " << fd << ". Sending 501..." << std::endl;

            // =====================【Week2 Day4 修改】======================================
            std::string err_resp = 
                "HTTP/1.1 501 Not Implemented\r\n"
                "Connection: close\r\n"
                "\r\n";
            append_to_out_buffer(conn, err_resp);
            // =============================================
            conn.is_closing = true;
            break;
        
        }

        // 3. 解析 Content-Length
        int content_length = 0;
        std::size_t cl_pos = headers.find("Content-Length:");
        if (cl_pos != std::string::npos) {
            std::size_t value_start = cl_pos + std::strlen("Content-Length:");
            while (value_start < headers.size() && headers[value_start] == ' ') {
                ++value_start;
            }

            std::size_t value_end = headers.find("\r\n", value_start);
            std::string value_str = headers.substr(value_start, value_end - value_start);
            content_length = std::atoi(value_str.c_str());
        }

        // =====================【Week2 Day4 新增】==============================
        // Body 大小限制: > 8MB 直接 413 + close
        if (content_length > kMaxBodyBytes) {
            std::cerr << ">>> [Body Limit] Content-Length too large on Fd" << fd << ". Sending 413..." << std::endl;
            std::string err_resp = 
                "HTTP/1.1 413 Payload Too Large\r\n"
                "Connection: close\r\n"
                "\r\n";
            append_to_out_buffer(conn, err_resp);
            conn.is_closing = true;
            break;
        }
        // =====================================================
        // 4. 检查整个请求是否收全 (header + body)
        if (static_cast<int>(conn.in_buffer.size()) < header_total_len + content_length) {
            break;
        }

        // 5. 完整请求到齐，交给业务处理
        handle_request(fd,conn, request_line);

        // =====================【Week2 Day4 修改】==============================
        // 不再直接 erase in_buffer, 改成统一扣减 inflight 计数
        erase_from_in_buffer_prefix(conn, static_cast<size_t>(header_total_len + content_length));
        // ==========================================================
        ++processed;
    }

    // 如果这轮预算用完了, 但 buffer 里还有完整请求, 加入 ReadyQueue
    if (!conn.is_closing && processed >= kMaxRequestsPerRound && has_complete_request(conn)) {
        std::cout << ">>> [ReadyQueue] FD " << fd << " still has complete requests, defer to next round." << std::endl;
        enqueue_ready(fd, conn);
    }

    update_epoll_events(epoll_fd, fd, conn);
}

// ==========================【Week Day2 新增】===============================
// 带读预算的读取逻辑：一次 EPOLLIN 最多读 256KB
void read_with_budget(int epoll_fd, int fd, Connection& conn, bool& io_error) {
    char temp_buf[4096];
    size_t read_this_round = 0;

    while (!conn.is_closing && read_this_round < kMaxReadBytesPerEvent) {
        size_t remain_budget = kMaxReadBytesPerEvent - read_this_round;
        size_t try_read = sizeof(temp_buf);

        if (try_read > remain_budget) {
            try_read = remain_budget;
        }

        ssize_t n = read(fd, temp_buf, try_read);

        if (n > 0) {
            // =====================【Week2 Day4 修改】==============================
            // 不再直接 append 到 in_buffer
            // 改正统一做 inflight budget 检查
            if (!append_to_in_buffer(conn, temp_buf, static_cast<size_t>(n))) {
                std::cerr << ">>> [Inflight Budget] In-buffer append exceeds limit on FD " << fd << ". Sending 503..." << std::endl;
                std::string err_resp =
                    "HTTP/1.1 503 Service Unavailable\r\n"
                    "Connection: close\r\n"
                    "\r\n";
                append_to_out_buffer(conn, err_resp);
                conn.is_closing = true;
                break;
            }
            // ================================================

            
            
            read_this_round += static_cast<size_t>(n);

            // Header 超过 8KB 的防御
            std::size_t pos = conn.in_buffer.find("\r\n\r\n");
            if ((pos == std::string::npos && conn.in_buffer.size() > 8192) || (pos != std::string::npos && pos > 8192)) {
                std::cerr << ">>> [DoS Defense] Header > 8KB from FD " << fd << ". Sending 431..." << std::endl;
                // =====================【Week2 Day4 修改】==============================
                std::string err_resp=
                    "HTTP/1.1 431 Request Header Fields Too Large\r\n"
                    "Connection: close\r\n"
                    "\r\n";
                append_to_out_buffer(conn, err_resp);
                // =====================================================
                conn.is_closing = true;
                break; 
            }

            // 每次读进来后，尽量解析请求
            process_requests_with_limit(epoll_fd, fd, conn);

            // 如果业务已经决定关闭，就不用继续读了
            if (conn.is_closing) {
                break;
            }
        }else if (n == 0) {
            // 对端关闭连接
            conn.is_closing = true;
            break;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // 当前 socket 已经没数据了
            break;
        } else {
            io_error = true;
            break;
        }
    }
}

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
                while(true) {
                    struct sockaddr_in client_addr;
                    std::memset(&client_addr, 0, sizeof(client_addr));
                    socklen_t client_len = sizeof(client_addr);
                    int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
                    if (client_fd == -1) {
                        if(errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        std::cerr << "accept failed, errno=" << errno << std::endl;
                        break;
                    }

                    char client_ip[INET_ADDRSTRLEN] = {0}; // [Day5新增]
                    const char* ip_cstr = inet_ntop(
                        AF_INET,
                        &client_addr.sin_addr,
                        client_ip,
                        INET_ADDRSTRLEN
                    );
                    std::string client_ip_str = ip_cstr ? client_ip : "unknown"; // [Day5新增]
                    // [Day5新增]
                    if (!consume_ip_token(client_ip_str)) {
                        std::cout << ">>> [429 Reject] ip=" << client_ip_str << std::endl;
                        reject_new_connection_with_429(client_fd); 
                        continue;
                    }
                    int flags = fcntl(client_fd, F_GETFL, 0);
                    if (flags == -1) {
                        std::cerr << "fcntl(F_GETFL) failed" << std::endl;
                        close(client_fd);
                        continue;
                    }

                    if (fcntl(client_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
                        std::cerr << "fcntl(F_SETFL) failed" << std::endl;
                        close(client_fd);
                        continue;
                    }
                    struct epoll_event client_event;
                    std::memset(&client_event, 0, sizeof(client_event));
                    client_event.events = EPOLLIN;
                    client_event.data.fd = client_fd;

                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &client_event) == -1) {
                        std::cerr << "epoll_ctl ADD client failed" << std::endl;
                        close (client_fd);
                        continue;
                    }
                    connections[client_fd] = Connection();
                    
                    std::cout << ">>> New Connection! IP: " << client_ip_str << ", FD: " << client_fd << std::endl;
                    
                }
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