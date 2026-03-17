// HTTP 处理模块
// 负责请求读取、请求解析、请求处理和响应生成：
// 是服务器处理 HTTP 请求主链路的核心模块。

#ifndef HTTP_LOGIC_H
#define HTTP_LOGIC_H

#include "connection.h"


void handle_request(int fd, Connection& conn, const std::string& request_line);
void process_requests_with_limit(int epoll_fd, int fd, Connection& conn);
void read_with_budget(int epoll_fd, int fd, Connection& conn, bool& io_error);

#endif
