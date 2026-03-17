// 新连接接收模块
// 负责 accept 新客户端连接，并完成连接初始化：
// 包括非阻塞设置、连接数限制、IP 限流检查，以及注册到 epoll。

#ifndef ACCEPT_UTILS_H
#define ACCEPT_UTILS_H

void accept_new_connections(int epoll_fd, int listen_fd);

#endif