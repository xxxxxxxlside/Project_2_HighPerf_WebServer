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
    std::cout << ">>> Server started successfully!" << std::endl;
    std::cout << ">>> Listening on port 8080..." << std::endl;
    std::cout << ">>> WSL2 Tip: Find your IP using 'ip addr show eth0' to connect from Windows." << std::endl;
    std::cout << ">>> Press Ctrl+C to stop." << std::endl;
    
    // 【死循环】：保持进程存活
    // 如果 main 函数执行完，进程就会退出，socket 会关闭，服务器就停了。
    // 我们需要它一直跑着，等待事件发生。
    while (true) {
        sleep(1); // 休眠 1 秒。【优化点】：如果不 sleep，CPU 会以 100% 占用率空转 (忙等)，浪费资源。
    }

    // 清理资源 (正常流程不会走到这里，只有 Ctrl+C 中断或出错返回时才会涉及清理)
    close(listen_fd); // 关闭 socket，释放内核资源

    return 0;
}