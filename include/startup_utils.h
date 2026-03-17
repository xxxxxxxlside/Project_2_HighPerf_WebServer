// 这个文件负责服务器启动初始化，以及 day4 为 sanitizer 自测补的停机信号接口。

#ifndef STARTUP_UTILS_H
#define STARTUP_UTILS_H

// 查询是否收到退出信号，主循环用它决定何时进入收尾流程。
bool stop_requested();

// 安装 SIGINT/SIGTERM 处理器，方便自测脚本优雅停服并触发资源清理。
void install_signal_handlers();

// 完成 listen fd / epoll fd 的初始化。
void init_server(int& listen_fd, int& epoll_fd);

#endif
