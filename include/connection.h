// 连接与全局状态模块
// 负责定义 Connection 结构体，以及服务器运行时用到的全局连接数据：
// 如 connections、ready_queue、pending_close_queue、全局 inflight 统计等。

#ifndef CONNECTION_H
#define CONNECTION_H

#include <string>
#include <unordered_map>
#include <deque>
#include <cstdint>


struct Connection {
    std::string in_buffer;
    std::string out_buffer;

    bool is_closing;
    bool in_ready_queue;
    bool fd_closed;
    bool header_complete = false;
    bool body_receiving = false;

    Connection() : is_closing(false), in_ready_queue(false), fd_closed(false) {}
    
    int64_t last_active_ms = 0;
    int64_t header_deadline_ms = 0;
    uint64_t header_timer_version = 0;
    uint64_t idle_timer_version = 0;
    int body_expected_bytes = 0;
    int64_t body_deadline_ms = 0;
    uint64_t body_timer_version = 0;
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

extern uint64_t accept_total;
extern uint64_t conns_current;
extern uint64_t requests_total;
extern uint64_t errors_total;
extern uint64_t reject_total;

extern uint64_t reject_411_total;
extern uint64_t reject_413_total;
extern uint64_t reject_429_total;
extern uint64_t reject_431_total;
extern uint64_t reject_501_total;
extern uint64_t reject_503_total;

#endif
