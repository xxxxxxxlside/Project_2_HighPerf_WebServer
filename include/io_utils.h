// IO 控制模块
// 负责与 epoll 和写回相关的底层 IO 逻辑：
// 包括更新 epoll 关注事件，以及按预算尽力发送输出缓冲区数据。

#ifndef IO_UTILS_H
#define IO_UTILS_H

#include "connection.h"

void update_epoll_events(int epoll_fd, int fd, Connection& conn);
void write_best_effort_with_budget(int fd, Connection& conn, bool& io_error);

#endif
