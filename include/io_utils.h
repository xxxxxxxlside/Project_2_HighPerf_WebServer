#ifndef IO_UTILS_H
#define IO_UTILS_H

#include "connection.h"

void update_epoll_events(int epoll_fd, int fd, Connection& conn);
void write_best_effort_with_budget(int fd, Connection& conn, bool& io_error);

#endif
