#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>

// 简单的错误检查宏，方便调试
#define CHECK_RET(expr, msg) \
    if((expr) < 0) { \
        std::cerr << "Error: " << msg << " (errno: " << errno << ")" << std::endl; \
        return -1; \
    }

int main() {
    // 1. 创建 Socket
    // AF_INET: IPv4, SOCK_STREAM: TCP, 0: 默认协议
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK_RET(listen_fd, "create socket failed");

    // 2. 设置地址复用 (SO_REUSEADDR)
    // 作用：服务器重启时，如果端口处于 TIME_WAIT 状态，
    // 可以立即绑定，不会报错 "Address already in use"
    int opt = 1;
    if(setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "setsockopt failed" << std::endl;
        return -1;
    }

    // 3. 绑定地址和端口
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;

    // 【WSL2 重点】：绑定 0.0.0.0 而不是 127.0.0.1
    // 这样 Windows 宿主机才能通过 WSL2 的 IP 访问到服务器
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY); // 等价于 0.0.0.0
    server_addr.sin_port = htons(8080);              // 监听 8080端口

    CHECK_RET(bind(listen_fd, (struct sockaddr*)&server_addr,sizeof(server_addr)), "bind failed");

    // 4. 开始监听
    // backlog: 等待连接队列的最大长度，设为 128 足够测试试用
    CHECK_RET(listen(listen_fd, 128), "listen failed");

    std::cout << ">>> Server started successfully!" << std::endl;
    std::cout << ">>> Listening on port 8080..." << std::endl;
    std::cout << ">>> WSL2 Tip: Find your IP using 'ip addr show eth0' to connect from Windows." << std::endl;

    // 5. 暂时先不写 accept 循环，先验证能不能启动和绑定
    // 下周我们会在这里加入 epoll 和 accpet 循环
    std::cout << ">>> Press Ctrl+C to stop." << std::endl;

    // 让程序暂停，保持监听状态
    while (true) {
        sleep(1);
    }

    close (listen_fd);

    return 0;
}
