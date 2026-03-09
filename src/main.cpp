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
    Connection() : is_closing(false), in_ready_queue(false) {}
};

// 全局地图：fd -> Connection 对象
std::unordered_map<int, Connection> connections;

// ReadyQueue: 存“还有完整请求没处理完，但这轮预算已用完”的 fd
std::deque<int> ready_queue; 

// ============================================================================
// 工具函数
// ============================================================================

// 判断 in_buffer 里是否至少有一个完整 HTTP 请求头
bool has_complete_request(const Connection& conn) {
    return conn.in_buffer.find("\r\n\r\n") != std::string::npos;
}

// 更新 epoll 里这个 fd 关心的事件
void update_epoll_events(int epoll_fd, int fd, const Connection& conn) {
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

// 关闭连接并清理
void close_connection(int epoll_fd, int fd) {
    std::cout << ">>> Closing FD: " << fd << std::endl;
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
    connections.erase(fd);
}

// 入队 ReadyQueue (防止重复入队)
void enqueue_ready(int fd, Connection& conn) {
    if (!conn.in_ready_queue) {
        ready_queue.push_back(fd);
        conn.in_ready_queue = true;
    }
}

// 业务处理:构造响应
void handle_request(int fd, Connection& conn, const std::string& request_line) {
   
    std::cout << ">>> [Request] FD " << fd << " Method-Line: " << request_line << std::endl;

    std::string resp = 
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 2\r\n"
        "Connection: close\r\n"
        "\r\n"
        "OK";
    
    conn.out_buffer += resp;

    // 你当前 day6 的响应仍然是处理完就 close
    conn.is_closing = true;
}

// 尽力写回
void write_best_effort(int fd, Connection& conn, bool& io_error) {
    if (conn.out_buffer.empty()) return;

    // [Day 6 终极修复]: 换用 send 并加 MSG_NOSIGNAL，防备客户端暴力断开引发 SIGPIPE 杀掉整个服务器
    ssize_t w = send(fd, conn.out_buffer.c_str(), conn.out_buffer.size(), MSG_NOSIGNAL);

    if (w > 0) {
        // [Day 6 核心逻辑]: 发出去多少，就从 out_buffer 头部删掉多少。剩下的下次继续发
        conn.out_buffer.erase(0, static_cast<size_t>(w));
    } else if (w == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
        // 如果不是 EAGAIN(缓冲区满) 导致的写入失败，说明连接出问题了
        io_error = true;
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

            conn.out_buffer +=
                "HTTP/1.1 501 Not Implemented\r\n"
                "Connection: close\r\n"
                "\r\n";
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

        // 4. 检查整个请求是否收全 (header + body)
        if (static_cast<int>(conn.in_buffer.size()) < header_total_len + content_length) {
            break;
        }

        // 5. 完整请求到齐，交给业务处理
        handle_request(fd,conn, request_line);

        // 6. 从 in_buffer 移除一个完整请求
        conn.in_buffer.erase(0, static_cast<size_t>(header_total_len + content_length));

        ++processed;
    }

    // 如果这轮预算用完了, 但 buffer 里还有完整请求, 加入 ReadyQueue
    if (!conn.is_closing && processed >= kMaxRequestsPerRound && has_complete_request(conn)) {
        std::cout << ">>> [ReadyQueue] FD " << fd << " still has complete requests, defer to next round." << std::endl;
        enqueue_ready(fd, conn);
    }

    update_epoll_events(epoll_fd, fd, conn);
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
    
    // 【WSL2 重点配置】：
    // htonl: Host TO Network Long (主机字节序 转 网络字节序)
    // INADDR_ANY: 是一个宏，值为 0.0.0.0
    // 含义：绑定本机所有可用的网卡 IP。
    // 好处：无论是通过 127.0.0.1 (本地)，还是 WSL2 的虚拟IP (如 172.x.x.x)，都能访问进来。
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY); 
    
    // htons: Host TO Network Short (主机字节序 转 网络字节序)
    // 8080: 我们想要监听的端口号
    // 【为什么转换？】：不同 CPU 存储数字的顺序不同 (大端/小端)，网络传输统一规定用“大端序”。
    // 如果不转换，在 Intel CPU (小端) 上，8080 可能会被网络对面解析成另一个奇怪的端口。
    server_addr.sin_port = htons(8080);              

    // =========================================================================
    // 第四步：绑定地址 (挂“招牌”)
    // =========================================================================
    // bind: 将 socket (listen_fd) 与具体的 IP 和端口 (server_addr) 绑定
    // (struct sockaddr*): 强制类型转换。因为 bind 函数设计时为了兼容 IPv6 等其他协议，
    //                     接收的是通用结构体 sockaddr*，而我们要传的是 IPv4 专用结构体 sockaddr_in*。
    CHECK_RET(bind(listen_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)), "bind failed");

    // =========================================================================
    // 第五步：开始监听 (开门“营业”)
    // =========================================================================
    // listen: 将主动套接字转为被动套接字，开始等待客户端连接
    // 参数 128: backlog (等待队列长度)
    // 【核心原理】：当并发连接瞬间涌入，内核来不及让程序 accept 时，会把连接先排在这个队列里。
    // 128 表示队列最多容纳 128 个等待中的连接，超过的直接拒绝。
    CHECK_RET(listen(listen_fd, 128), "listen failed");


    std::cout << ">>> Server started! Switching to Epoll mode ..." << std::endl;

    // ==========================================================================
    // 2. 创建 Epoll 
    // ==========================================================================
    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        std::cerr << "epoll_create1 failed" << std::endl;
        return -1;
    }

    // ------------------------------------------
    // 2. 创建事件结构体，准备注册 listen_fd
    // ------------------------------------------
    struct epoll_event event;
    event.events = EPOLLIN;         // 我们关心“读”事件（有新连接也是读事件的一种）
    event.data.fd = listen_fd;      // 把 listen_fd 绑定到这个事件上

    // ------------------------------------------
    // 3. 将 listen_fd 添加到 Epoll 监听列表
    // ------------------------------------------
    // EPOLL_CTL_ADD: 添加操作
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &event) == -1) {
        std::cerr << "epoll_ctl ADD failed" << std::endl;
        return -1; 
    }

    std::cout << ">>> Epoll initialized. Listening for events..." << std::endl;

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

            if (fd == listen_fd) {
                // --------------------------------------
                // 情况A: 新连接 (基本不变，仅增加 Map 初始化)
                // --------------------------------------
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);

                // 接受连接
                int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
                if (client_fd == -1) {
                    std::cerr << "accept failed" << std::endl;
                    continue;
                }

                // 【关键】设置非阻塞(必须!)
                int flags = fcntl(client_fd, F_GETFL, 0);
                fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

                // 打印日志
                char client_ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
                std::cout << ">>> New Connection! IP: " << client_ip << ", FD: " << client_fd << std::endl;

                // 【新增】将连接的 client_fd 也加入 Epoll 监听!
                // 这样下次它有数据时, epoll_wait 也会通知我们
                struct epoll_event client_event;
                client_event.events = EPOLLIN;  // 关心它的读事件
                client_event.data.fd = client_fd;

                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &client_event) == -1) {
                    std::cerr << "epoll_ctl ADD client failed" << std::endl;
                    close (client_fd);
                    continue;
                } 

                connections[client_fd] = Connection();
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

            // 可读事件
            if (events[i].events & EPOLLIN) {
                char temp_buf[4096]; 
                ssize_t n = read(fd, temp_buf, sizeof(temp_buf));

                if (n > 0) {
                    // [Day 6 终极修复]: 如果已经判定该关了，就别再往里塞数据、走解析了，防止 431 循环触发
                    if (!conn.is_closing) {
                        // [Day 4 核心] 追加数据到动态 Buffer
                        conn.in_buffer.append(temp_buf, n);

                        // ========= [Day 5 新增: Dos 防御 1 (Header > 8KB)] =========
                        // 【修正版逻辑】：
                        // 如果一直没找到 \r\n\r\n 且堆积长度超过8KB，或者找到的 \r\n\r\n 位置本身就超过了8KB
                        // 这两种情况都代表了 Header 字段过大，直接触发 431 并断开连接
                        // Header 超过 8KB 防御
                        std::size_t pos = conn.in_buffer.find("\r\n\r\n");
                        if ((pos == std::string::npos && conn.in_buffer.size() > 8192) || 
                            (pos != std::string::npos && pos > 8192)) {
                            std::cerr << ">>> [DoS Defense] Header > 8KB from FD " << fd << ". Sending 431..." << std::endl;
                            
                            conn.out_buffer +=
                                "HTTP/1.1 431 Request Header Fields Too Large\r\n"
                                "Connection: close\r\n"
                                "\r\n";
                            conn.is_closing = true;
                            update_epoll_events(epoll_fd, fd, conn);
                            
                        } else {
                            process_requests_with_limit(epoll_fd, fd, conn);   
                                
                            }
                        }
                        
                    } else if ( n == 0) {
                        conn.is_closing = true;
                    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        io_error =true;
                    }
                } 
                // 可写事件
                if (!io_error && (events[i].events & EPOLLOUT)) {
                    write_best_effort(fd, conn, io_error);
                    update_epoll_events(epoll_fd, fd, conn);
                }

                // 收尾关闭
                if (io_error || (conn.is_closing && conn.out_buffer.empty())){
                    close_connection(epoll_fd, fd);
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

                if (conn.is_closing) {
                    if (conn.out_buffer.empty()) {
                        close_connection(epoll_fd, fd);
                    }
                    continue;

                }

                process_requests_with_limit(epoll_fd, fd, conn);

                bool io_error = false;
                if (!conn.out_buffer.empty()) {
                    write_best_effort(fd, conn, io_error);
                    update_epoll_events(epoll_fd, fd, conn);
                }

                if (io_error || (conn.is_closing && conn.out_buffer.empty())) {
                    close_connection(epoll_fd, fd);
                }
            }
        }

        close (listen_fd);
        close (epoll_fd);
        return 0;
}