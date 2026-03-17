// 这个文件负责 HTTP 请求解析、响应生成，以及 day5 需要的连接复用逻辑。

#ifndef HTTP_LOGIC_H
#define HTTP_LOGIC_H

#include "connection.h"

// 根据请求生成业务响应，并决定响应后是否保持连接。
void handle_request(int fd, Connection& conn, const std::string& request_line, bool keep_alive);

// 在单连接单轮预算内解析并处理完整请求。
void process_requests_with_limit(int epoll_fd, int fd, Connection& conn);

// 按预算读取 socket 数据，并在读路径里完成协议边界检查。
void read_with_budget(int epoll_fd, int fd, Connection& conn, bool& io_error);

#endif
