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

int main() {
    // =========================================================================
    // 第一步：创建 Socket (创建“听筒”)
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

    // =========================================================================
    // 第六步：打印日志与保持运行
    // =========================================================================

    // ... (前面的 socket, bind, listen 代码保持不变) ...
    std::cout << ">>> Server started successfully!" << std::endl;
    std::cout << ">>> Listening on port 8080..." << std::endl;
    std::cout << ">>> Press Ctrl+C to stop." << std::endl;
    
    // 用于存储所有已连接的客户端 fd (简单期间，今天先不用map，只用一个变量演示)
    // 实际项目中会用 unordered_map, Connection*> 管理
    int client_fd = -1;

    while (true) {
        // -------------------------------------------
        // 核心动作：接受连接 (Accept)
        // -------------------------------------------
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        // accept 是一个阻塞函数：如果没有人连接，程序员会停在这里等待
        // 一旦有人连接，它会返回一个新的 fd (client_fd), 专门用于和这个客户通信
        client_fd = accept(listen_fd,(struct sockaddr*)&client_addr,&client_len);

        if (client_fd < 0) {
            // 如果出错 (比如被信号中断)，打印错误并继续循环
            std::cerr << "accept failed" << std::endl;
            continue;
        }
        
        // -------------------------------------------------
        // 【关键步骤】，设置非阻塞模式 (Non-blocking)
        // -------------------------------------------------
        // 为什么必须做？
        // 如果后续 read/write 时没有数据，阻塞模式会让整个服务器卡死。
        // 设置为非阻塞后，如果没有数据，read会立即返回 -1 (errno = EAGAIN), 而不是卡住。
        // 这是下周使用 Epoll 的前提条件
        int flags = fcntl(client_fd, F_GETFL, 0); // 获取当前标志
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK); // 加上 O_NONBLOCK 标志

        // -------------------------------------------------
        // 打印连接信息
        // -------------------------------------------------
        // 将客户端 IP 从网络字节序换位字符串
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);

        std::cout << ">>> New connection accepted!" << std::endl;
        std::cout << "      Client IP: " << client_ip << std::endl;
        std::cout << "      Client Port: " << ntohs(client_addr.sin_port) << std::endl;
        std::cout << "      Client FD: " << client_fd << std::endl;
        std::cout << "      Status: Non-blocking set." << std::endl;
        std::cout << "-------------------------------" << std::endl;

        // ---------------------------------------------------
        // 简单测试，接收数据（今天先不做负责处理，只读一下试试）
        // ---------------------------------------------------
        char buffer[1024] = {0};
        // 尝试读取数据
        ssize_t n = read(client_fd, buffer, sizeof(buffer));

        if (n > 0) {
            std::cout << ">>> Recelved data from client: " << buffer << std::endl;

            // 简单回显 (Echo)
            write(client_fd, buffer, n);
        } else if (n == 0) {
            // n=0 表示客户端正常关闭连接
            std::cout << ">>> Client disconnected normally." << std::endl;
            close(client_fd); // 关闭 fd, 释放资源
            client_fd = -1;   // 重置标记
        } else {
            // n<0 且 errno == EAGAIN 表示暂时没数据 (因为是非阻塞)
            // 其他错误则关闭
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                std::cerr << "read error, closing connection." << std::endl;
                close(client_fd);
                client_fd = -1;
            } else {
                // 非阻塞模式下，没数据是常态，这里可以选择不打印或者打印调试信息
                // std::cout << "No data avaliable right now (EAGAIN)." << std::endl;
            }
        }

        // 注意：今天的代码是一次性处理，处理完就关闭连接或继续等待下一个 accept
        // 下周引入 Epoll 后，我们会同时管理多个连接

    }

    // 清理资源 (正常流程不会走到这里，只有 Ctrl+C 中断或出错返回时才会涉及清理)
    close(listen_fd); // 关闭 socket，释放内核资源

    return 0;
}