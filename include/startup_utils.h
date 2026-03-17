// 服务器启动初始化模块
// 负责服务器启动时的一次性初始化工作：
// 包括创建监听 socket、bind、listen、设置非阻塞、创建 epoll 和注册监听 fd。

#ifndef STARTUP_UTILS_H
#define STARTUP_UTILS_H

void init_server(int& listen_fd, int& epoll_fd);

#endif