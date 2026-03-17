#include "http_logic.h"
#include "buffer_utils.h"
#include "queue_utils.h"
#include "io_utils.h"

#include <iostream>
#include <cstring>
#include <unistd.h>


void handle_request(int fd, Connection& conn, const std::string& request_line) {
   
    std::cout << ">>> [Request] FD " << fd << " Method-Line: " << request_line << std::endl;

    std::string resp = 
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 2\r\n"
        "Connection: close\r\n"
        "\r\n"
        "OK";
    
    // =====================【Week2 Day4 新增】======================================
    // 不再直接 out_buffer += resp
    // 改成统一走 append_to_out_buffer,顺带做 inflight budget 检查
    if (!append_to_out_buffer(conn, resp)) {
        std::cerr << ">>> [Inflight Budget] Response append exceeds limit." << "Sending 503..." << std::endl;
        conn.out_buffer.clear(); // 这里 clear 不需要扣计数，因为 append 失败根本没加进去

        std::string err_resp =
            "HTTP/1.1 503 Service Unavailable\r\n"
            "Connection: Close\r\n"
            "\r\n";
        append_to_out_buffer(conn, err_resp);
    }

    
    conn.is_closing = true;
}

void process_requests_with_limit(int epoll_fd, int fd, Connection& conn) {
    int processed = 0;

    while (!conn.is_closing && processed < kMaxRequestsPerRound) {
        std::size_t pos = conn.in_buffer.find("\r\n\r\n");
        if (pos == std::string::npos) {
            break;
        }

        // Header 总长度
        int header_total_len = static_cast<int>(pos + 4);
        std::string headers = conn.in_buffer.substr(0, pos);

        // 1. Request Line
        std::size_t line_end = headers.find("\r\n");
        std::string request_line;
        if (line_end != std::string::npos) {
            request_line = headers.substr(0, line_end);
            std::cout << ">>> [Parse] FD " << fd << " Request: " << request_line << std::endl;
        }

        // 2. 防御: 拒绝 chunked
        if (headers.find("Transfer-Encoding: chunked") != std::string::npos || headers.find("Transfer-encoding: Chunked") != std::string::npos) {
            std::cerr << ">>> [DoS Defense] Chunked not supported on FD " << fd << ". Sending 501..." << std::endl;

            // =====================【Week2 Day4 修改】======================================
            std::string err_resp = 
                "HTTP/1.1 501 Not Implemented\r\n"
                "Connection: close\r\n"
                "\r\n";
            append_to_out_buffer(conn, err_resp);
            // =============================================
            conn.is_closing = true;
            break;
        
        }

        // 3. 解析 Content-Length
        int content_length = 0;
        std::size_t cl_pos = headers.find("Content-Length:");
        if (cl_pos != std::string::npos) {
            std::size_t value_start = cl_pos + std::strlen("Content-Length:");
            while (value_start < headers.size() && headers[value_start] == ' ') {
                ++value_start;
            }

            std::size_t value_end = headers.find("\r\n", value_start);
            std::string value_str = headers.substr(value_start, value_end - value_start);
            content_length = std::atoi(value_str.c_str());
        }

        // =====================【Week2 Day4 新增】==============================
        // Body 大小限制: > 8MB 直接 413 + close
        if (content_length > kMaxBodyBytes) {
            std::cerr << ">>> [Body Limit] Content-Length too large on Fd" << fd << ". Sending 413..." << std::endl;
            std::string err_resp = 
                "HTTP/1.1 413 Payload Too Large\r\n"
                "Connection: close\r\n"
                "\r\n";
            append_to_out_buffer(conn, err_resp);
            conn.is_closing = true;
            break;
        }
        // =====================================================
        // 4. 检查整个请求是否收全 (header + body)
        if (static_cast<int>(conn.in_buffer.size()) < header_total_len + content_length) {
            break;
        }

        // 5. 完整请求到齐，交给业务处理
        handle_request(fd,conn, request_line);

        // =====================【Week2 Day4 修改】==============================
        // 不再直接 erase in_buffer, 改成统一扣减 inflight 计数
        erase_from_in_buffer_prefix(conn, static_cast<size_t>(header_total_len + content_length));
        // ==========================================================
        ++processed;
    }

    // 如果这轮预算用完了, 但 buffer 里还有完整请求, 加入 ReadyQueue
    if (!conn.is_closing && processed >= kMaxRequestsPerRound && has_complete_request(conn)) {
        std::cout << ">>> [ReadyQueue] FD " << fd << " still has complete requests, defer to next round." << std::endl;
        enqueue_ready(fd, conn);
    }

    update_epoll_events(epoll_fd, fd, conn);
}

void read_with_budget(int epoll_fd, int fd, Connection& conn, bool& io_error) {
    char temp_buf[4096];
    size_t read_this_round = 0;

    while (!conn.is_closing && read_this_round < kMaxReadBytesPerEvent) {
        size_t remain_budget = kMaxReadBytesPerEvent - read_this_round;
        size_t try_read = sizeof(temp_buf);

        if (try_read > remain_budget) {
            try_read = remain_budget;
        }

        ssize_t n = read(fd, temp_buf, try_read);

        if (n > 0) {
            // =====================【Week2 Day4 修改】==============================
            // 不再直接 append 到 in_buffer
            // 改正统一做 inflight budget 检查
            if (!append_to_in_buffer(conn, temp_buf, static_cast<size_t>(n))) {
                std::cerr << ">>> [Inflight Budget] In-buffer append exceeds limit on FD " << fd << ". Sending 503..." << std::endl;
                std::string err_resp =
                    "HTTP/1.1 503 Service Unavailable\r\n"
                    "Connection: close\r\n"
                    "\r\n";
                append_to_out_buffer(conn, err_resp);
                conn.is_closing = true;
                break;
            }
            // ================================================
           
            read_this_round += static_cast<size_t>(n);

            // Header 超过 8KB 的防御
            std::size_t pos = conn.in_buffer.find("\r\n\r\n");
            if ((pos == std::string::npos && conn.in_buffer.size() > 8192) || (pos != std::string::npos && pos > 8192)) {
                std::cerr << ">>> [DoS Defense] Header > 8KB from FD " << fd << ". Sending 431..." << std::endl;
                // =====================【Week2 Day4 修改】==============================
                std::string err_resp=
                    "HTTP/1.1 431 Request Header Fields Too Large\r\n"
                    "Connection: close\r\n"
                    "\r\n";
                append_to_out_buffer(conn, err_resp);
                // =====================================================
                conn.is_closing = true;
                break; 
            }

            // 每次读进来后，尽量解析请求
            process_requests_with_limit(epoll_fd, fd, conn);

            // 如果业务已经决定关闭，就不用继续读了
            if (conn.is_closing) {
                break;
            }
        }else if (n == 0) {
            // 对端关闭连接
            conn.is_closing = true;
            break;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // 当前 socket 已经没数据了
            break;
        } else {
            io_error = true;
            break;
        }
    }
}

