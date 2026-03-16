#ifndef QUEUE_UTILS_H
#define QUEUE_UTILS_H

#include "connection.h"

void enqueue_ready(int fd, Connection& conn);
void remove_from_ready_queue(int fd, Connection& conn);
void request_close(int epoll_fd, int fd, Connection& conn);
void flush_pending_close_queue();

#endif
