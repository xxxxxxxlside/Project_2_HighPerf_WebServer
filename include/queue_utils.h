// 队列与延迟关闭模块
// 负责 ReadyQueue 和 pending_close_queue 的管理：
// 包括连接入队、移除、请求关闭，以及统一释放待关闭连接。

#ifndef QUEUE_UTILS_H
#define QUEUE_UTILS_H

#include "connection.h"

void enqueue_ready(int fd, Connection& conn);
void remove_from_ready_queue(int fd, Connection& conn);
void request_close(int epoll_fd, int fd, Connection& conn);
void flush_pending_close_queue();

#endif
