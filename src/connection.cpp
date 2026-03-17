#include "connection.h"

std::unordered_map<int, Connection> connections;
std::deque<int> ready_queue;
std::deque<int> pending_close_queue;
size_t global_inflight_bytes = 0;
size_t conn_reject_total = 0;

uint64_t accept_total = 0;
uint64_t conns_current = 0;
uint64_t requests_total = 0;
uint64_t errors_total = 0;
uint64_t reject_total = 0;

uint64_t reject_411_total = 0;
uint64_t reject_413_total = 0;
uint64_t reject_429_total = 0;
uint64_t reject_431_total = 0;
uint64_t reject_501_total = 0;
uint64_t reject_503_total = 0;