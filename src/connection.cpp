#include "connection.h"

std::unordered_map<int, Connection> connections;
std::deque<int> ready_queue;
std::deque<int> pending_close_queue;
size_t global_inflight_bytes = 0;