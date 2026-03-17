// 这个文件负责启动期 socket/epoll 初始化，以及 day4 为 sanitizer 自测补的信号退出支持。
#include "startup_utils.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <csignal>

namespace {
// 只做最小标记，不在信号处理器里执行复杂逻辑。
volatile std::sig_atomic_t g_stop_requested = 0;

void handle_stop_signal(int) {
    g_stop_requested = 1;
}
}

// 主循环通过这个标记判断是否该退出，并进入统一收尾。
bool stop_requested() {
    return g_stop_requested != 0;
}

// 自测脚本结束时会发 SIGTERM，这里把它收口成一个可轮询的退出标记。
void install_signal_handlers() {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_stop_signal;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

// 启动时一次性完成监听 socket 和 epoll 初始化。
void init_server(int& listen_fd, int& epoll_fd) {
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    
    if (listen_fd < 0) {
        std::cerr << "create socket failed\n";
        return;
    }

    int opt = 1; // 选项值，1 表示开启/真
    
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "setsockopt failed" << std::endl;
        return ;
    }

    struct sockaddr_in server_addr; // 定义一个 IPv4 专用的地址结构体变量
    
    memset(&server_addr, 0, sizeof(server_addr));
    
    server_addr.sin_family = AF_INET;             // 指定协议族：IPv4 (必须与 socket() 一致)
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(8080);
    
    if (bind(listen_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "bind failed\n";
        return;
    }

    if (listen(listen_fd, 128) < 0) {
        std::cerr << "listen failed (errno: " << errno << ")\n";
        return;
    }

    int listen_flags = fcntl(listen_fd, F_GETFL, 0);

    if (fcntl(listen_fd, F_SETFL, listen_flags | O_NONBLOCK) < 0) {
        std::cerr << "fcntl(F_SETFL) on listen_fd failed (errno: " << errno << ")\n";
        return;
    }

    std::cout << ">>> Server started! Switching to Epoll mode ..." << std::endl;
    
    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        std::cerr << "epoll_createl failed" << std::endl;
        return ;
    }

    struct epoll_event event;
    event.events = EPOLLIN;
    event.data.fd = listen_fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &event) == -1) {
        std::cerr << "epoll_ctl ADD listen_fd failed" << std::endl;
        return ;
    }
    std::cout << ">>> Epoll initiallized. Listening for events..." << std::endl;
    
}
