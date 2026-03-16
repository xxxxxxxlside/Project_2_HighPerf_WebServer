#ifndef IP_RATE_LIMIT_H
#define IP_RATE_LIMIT_H

#include <string>
#include <unordered_map>
#include <list>
#include <cstdint>
#include <chrono>


static const size_t kIpBucketMaxEntries = 100000;       // [Day5新增] 最多记录 10 万个 IP
static const int kIpBucketTtlSeconds = 600;             // [Day5新增] 10 分钟没访问就过期
static const double kIpBucketCapcity = 200.0;           // [Day5新增] 桶容量 200
static const double kIpBucketRefillPerSec = 50.0;       // [Day5新增] 每秒补充 50 个 token
// 本地测试时可临时改成：
// static const double kIpBucketCapacity = 20.0;
// static const double kIpBucketRefillPerSec = 1.0;

struct IpBucket {
    double tokens;
    std::chrono::steady_clock::time_point last_refill;
    std::chrono::steady_clock::time_point last_seen;
    std::list<std::string>::iterator lru_it;
};

extern std::unordered_map<std::string, IpBucket> ip_buckets;
extern std::list<std::string> ip_lru;

void touch_ip_bucket_lru(const std::string& ip);
void cleanup_expired_ip_buckets(uint64_t now_ms);
void evict_ip_buckets_if_needed();
void refill_ip_bucket(IpBucket& bucket, uint64_t now_ms);
bool consume_ip_token(const std::string& ip);
void reject_new_connection_with_429(int connfd);

#endif