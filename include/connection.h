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

#endif
