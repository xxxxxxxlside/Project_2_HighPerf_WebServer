#ifndef BUFFER_UTILS_H
#define BUFFER_UTILS_H

#include "connection.h"

bool has_complete_request(const Connection& conn);
bool append_to_in_buffer(Connection& conn, const char* data, size_t len);
bool append_to_out_buffer(Connection& conn, const std::string& data);
void erase_from_in_buffer_prefix(Connection& conn, size_t len);
void erase_from_out_buffer_prefix(Connection& conn, size_t len);
void release_connection_buffers(Connection& conn);

#endif
