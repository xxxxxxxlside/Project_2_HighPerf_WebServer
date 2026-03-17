#include "accept_utils.h"
#include "connection.h"
#include "ip_rate_limit.h"
#include "timer_utils.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <iostream>

void accept_new_connections(int epoll_fd, int listen_fd) {
    while(true) {

        struct sockaddr_in client_addr;
        std::memset(&client_addr, 0, sizeof(client_addr));
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd == -1) {
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            std::cerr << "accept failed, errno=" << errno << std::endl;
            break;
        }

        char client_ip[INET_ADDRSTRLEN] = {0}; // [Day5新增]
        const char* ip_cstr = inet_ntop(
            AF_INET,
            &client_addr.sin_addr,
            client_ip,
            INET_ADDRSTRLEN
        );
        std::string client_ip_str = ip_cstr ? client_ip : "unknown"; // [Day5新增]
        // [Day5新增]
        if (!consume_ip_token(client_ip_str)) {
            ++reject_total;
            ++reject_429_total;

            std::cout << ">>> [429 Reject] ip=" << client_ip_str << std::endl;
            reject_new_connection_with_429(client_fd); 
            continue;
        }
        int flags = fcntl(client_fd, F_GETFL, 0);
        if (flags == -1) {
            std::cerr << "fcntl(F_GETFL) failed" << std::endl;
            close(client_fd);
            continue;
        }

        if (fcntl(client_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
            std::cerr << "fcntl(F_SETFL) failed" << std::endl;
            close(client_fd);
            continue;
        }

        // ======= [Week2 Day6 新增开始: 最大连接数限制] ================
        if (connections.size() >= kMaxConnections) {
            ++conn_reject_total;
            std::cout << ">>> [Conn Reject] max_conns reached. "
                        << "FD: " << client_fd
                        << ", IP: " << client_ip_str
                        << ", conn_reject_total=" << conn_reject_total
                        << std::endl;
            close(client_fd);
            continue;
        }
        // ======= [Week2 Day6 新增结束: 最大连接数限制] ================

        struct epoll_event client_event;
        std::memset(&client_event, 0, sizeof(client_event));
        client_event.events = EPOLLIN;
        client_event.data.fd = client_fd;

        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &client_event) == -1) {
            std::cerr << "epoll_ctl ADD client failed" << std::endl;
            close(client_fd);
            continue;
        }
        connections[client_fd] = Connection();
        ++accept_total;
        ++conns_current;

        Connection& conn = connections[client_fd];
        int64_t now_ms = now_ms_monotonic();

        conn.last_active_ms = now_ms;
        conn.header_deadline_ms = now_ms + 10 * 1000;
        conn.header_complete = false;

        ++conn.header_timer_version;
        push_timer(client_fd, conn.header_deadline_ms, conn.header_timer_version, TimerType::HeaderTimeout);

        ++conn.idle_timer_version;
        push_timer(client_fd, now_ms + 60 * 1000, conn.idle_timer_version, TimerType::IdleTimeout);
        

        std::cout << ">>> New Connection! IP: " << client_ip_str << ", FD: " << client_fd << std::endl;
        
    }
}
