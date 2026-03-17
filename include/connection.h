// 连接与全局状态模块
// 负责定义 Connection 结构体，以及服务器运行时用到的全局连接数据：
// 如 connections、ready_queue、pending_close_queue、全局 inflight 统计等。

#ifndef CONNECTION_H
#define CONNECTION_H

#include <string>
#include <unordered_map>
#include <deque>

struct Connection {
    std::string in_buffer;
    std::string out_buffer;

    bool is_closing;
    bool in_ready_queue;
    bool fd_closed;

    Connection() : is_closing(false), in_ready_queue(false), fd_closed(false) {}
};

extern std::unordered_map<int, Connection> connections;
extern std::deque<int> ready_queue;
extern std::deque<int> pending_close_queue;
extern size_t global_inflight_bytes;
extern const size_t kMaxInflightBytes;
extern const int kMaxRequestsPerRound;
extern const int kMaxBodyBytes;
extern const size_t kMaxWriteBytesPerEvent;
extern const size_t kMaxReadBytesPerEvent;
extern const size_t kMaxConnections;
extern size_t conn_reject_total;

#endif
