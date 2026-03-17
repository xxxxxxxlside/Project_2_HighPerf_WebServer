// 缓冲区工具模块
// 负责连接输入/输出缓冲区的操作：
// 包括追加数据、删除已消费数据、判断请求是否完整、释放缓冲区占用等。

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
