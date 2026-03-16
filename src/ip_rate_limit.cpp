#include "ip_rate_limit.h"

#include <chrono>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

std::unordered_map<std::string, IpBucket> ip_buckets;
std::list<std::string> ip_lru;


void touch_ip_bucket_lru(const std::string& ip) {
    auto it = ip_buckets.find(ip);
    if (it == ip_buckets.end()) {
        return;
    }

    ip_lru.erase(it->second.lru_it);
    ip_lru.push_front(ip);
    it->second.lru_it = ip_lru.begin();
}

void cleanup_expired_ip_buckets() {
    auto now = std::chrono::steady_clock::now();

    while (!ip_lru.empty()) {
        const std::string& oldest_ip = ip_lru.back();
        auto it = ip_buckets.find(oldest_ip);
        if (it == ip_buckets.end()) {
            ip_lru.pop_back();
            continue;
        }

        auto idle_sec = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.last_seen).count();
        if (idle_sec < kIpBucketTtlSeconds) {
            break;
        }

        ip_lru.pop_back();
        ip_buckets.erase(it);
    }
}

void evict_ip_buckets_if_needed() {
    while (ip_buckets.size() > kIpBucketMaxEntries && !ip_lru.empty()) {
        std::string oldest_ip = ip_lru.back();
        ip_lru.pop_back();
        ip_buckets.erase(oldest_ip);
    }
}

void refill_ip_bucket(IpBucket& bucket, const std::chrono::steady_clock::time_point& now) {
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - bucket.last_refill).count();

    if (elapsed_ms <= 0) {
        return;
    }

    double add_tokens = (elapsed_ms / 1000.0) * kIpBucketRefillPerSec;
    bucket.tokens += add_tokens;
    if (bucket.tokens > kIpBucketCapcity) {
        bucket.tokens = kIpBucketCapcity;
    }

    bucket.last_refill = now;
}

bool consume_ip_token(const std::string& ip) {
    cleanup_expired_ip_buckets();

    auto now = std::chrono::steady_clock::now();
    auto it = ip_buckets.find(ip);

    // 第一次看到这个 IP: 创建一个满桶
    if (it == ip_buckets.end()) {
        ip_lru.push_front(ip);

        IpBucket bucket;
        bucket.tokens = kIpBucketCapcity;
        bucket.last_refill = now;
        bucket.last_seen = now;
        bucket.lru_it = ip_lru.begin();

        auto ret = ip_buckets.emplace(ip, bucket);
        it = ret.first;

        evict_ip_buckets_if_needed();
    }

    IpBucket& bucket = it->second;
    refill_ip_bucket(bucket, now);
    bucket.last_seen = now;
    touch_ip_bucket_lru(ip);

    if (bucket.tokens < 1.0) {
        return false;
    }

    bucket.tokens -= 1.0;
    return true;
}

void reject_new_connection_with_429(int fd) {
    const char* resp =
        "HTTP/1.1 429 Too Many Requests\r\n"
        "Content-Length: 17\r\n"
        "Connection: close\r\n"
        "\r\n"
        "Too Many Requests";

    send(fd, resp, std::strlen(resp), MSG_NOSIGNAL);
    close(fd);
}


