#include "buffer_utils.h"


bool has_complete_request(const Connection& conn) {
    return conn.in_buffer.find("\r\n\r\n") != std::string::npos;
}

bool append_to_in_buffer(Connection& conn, const char* data, size_t len) {
    if (global_inflight_bytes + len > kMaxInflightBytes) {
        return false;
    }

    conn.in_buffer.append(data, len);
    global_inflight_bytes += len;
    return true;
}

bool append_to_out_buffer(Connection& conn, const std::string& data) {
    if (global_inflight_bytes + data.size() > kMaxInflightBytes) {
        return false;
    }

    conn.out_buffer += data;
    global_inflight_bytes += data.size();
    return true;
}

void erase_from_in_buffer_prefix(Connection& conn, size_t len) {
    if (len > conn.in_buffer.size()) {
        len = conn.in_buffer.size();
    }

    conn.in_buffer.erase(0, len);
    global_inflight_bytes -= len;
}

void erase_from_out_buffer_prefix(Connection& conn, size_t len) {
    if (len > conn.out_buffer.size()) {
        len = conn.out_buffer.size();
    }

    conn.out_buffer.erase(0, len);
    global_inflight_bytes -= len;
}

void release_connection_buffers(Connection& conn) {
    global_inflight_bytes -= conn.in_buffer.size();
    global_inflight_bytes -= conn.out_buffer.size();
    conn.in_buffer.clear();
    conn.out_buffer.clear();
}

