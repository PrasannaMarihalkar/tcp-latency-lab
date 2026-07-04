#pragma once

#include <cstdint>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <stdexcept>
#include <system_error>

#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sched.h>

// ─────────────────────────────────────────────────────────────────────────────
// Timestamp helpers
// ─────────────────────────────────────────────────────────────────────────────

// Returns current time in nanoseconds (monotonic clock — never goes backward)
// This is the ONLY clock you should use for latency measurement.
// CLOCK_REALTIME can jump when NTP adjusts the system clock.
// CLOCK_MONOTONIC never jumps — perfect for deltas.
inline int64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

// Nanoseconds → human-readable string (ns / µs / ms)
inline std::string fmt_ns(int64_t ns) {
    if (ns < 1'000)       return std::to_string(ns) + " ns";
    if (ns < 1'000'000)   return std::to_string(ns / 1'000) + "." +
                                 std::to_string((ns % 1'000) / 100) + " µs";
    return std::to_string(ns / 1'000'000) + "." +
           std::to_string((ns % 1'000'000) / 100'000) + " ms";
}

// ─────────────────────────────────────────────────────────────────────────────
// The wire message — exactly what travels over TCP
// We embed send_ts so the client can measure RTT without extra bookkeeping.
// ─────────────────────────────────────────────────────────────────────────────
#pragma pack(push, 1)
struct Message {
    uint64_t seq;         // sequence number — detect reordering (shouldn't happen on TCP)
    int64_t  send_ts;     // nanoseconds at send time (CLOCK_MONOTONIC)
    char     payload[32]; // pad to 48 bytes total — realistic small order message size
};
#pragma pack(pop)

static_assert(sizeof(Message) == 48, "Message size mismatch");

// ─────────────────────────────────────────────────────────────────────────────
// Socket option helpers — these wrap setsockopt with error checking
// so every caller gets a clear error message, not a silent failure.
// ─────────────────────────────────────────────────────────────────────────────

inline void set_reuseaddr(int fd) {
    int v = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &v, sizeof(v)) < 0)
        throw std::system_error(errno, std::generic_category(), "SO_REUSEADDR");
}

// TCP_NODELAY: disable Nagle's algorithm.
// Without this, the kernel batches small writes and can delay your 44-byte
// message for up to 40ms waiting to see if more data arrives.
// In HFT, 40ms is not a latency spike — it is a disaster.
inline void set_nodelay(int fd) {
    int v = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v)) < 0)
        throw std::system_error(errno, std::generic_category(), "TCP_NODELAY");
}

// TCP_QUICKACK: ACK immediately, don't wait to batch ACKs.
// CRITICAL GOTCHA: Linux resets TCP_QUICKACK after every recv() call.
// You must re-set it after each receive. We handle this in the server loop.
inline void set_quickack(int fd) {
    int v = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &v, sizeof(v)) < 0)
        throw std::system_error(errno, std::generic_category(), "TCP_QUICKACK");
}

// SO_SNDBUF / SO_RCVBUF: kernel socket buffer sizes.
// Default is ~128KB. We set 4MB for bursty workloads.
// The kernel actually doubles whatever you set (for overhead), so 4MB → ~8MB actual.
inline void set_buffers(int fd, int size_bytes) {
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size_bytes, sizeof(size_bytes)) < 0)
        throw std::system_error(errno, std::generic_category(), "SO_SNDBUF");
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size_bytes, sizeof(size_bytes)) < 0)
        throw std::system_error(errno, std::generic_category(), "SO_RCVBUF");
}

// SO_KEEPALIVE: detect dead connections.
// In HFT, a silently dead TCP connection to the exchange means your orders
// disappear into the void. Always enable keepalive.
inline void set_keepalive(int fd, int idle_s = 5, int intvl_s = 2, int cnt = 3) {
    int v = 1;
    setsockopt(fd, SOL_SOCKET,   SO_KEEPALIVE,  &v,      sizeof(v));
    setsockopt(fd, IPPROTO_TCP,  TCP_KEEPIDLE,  &idle_s, sizeof(idle_s));
    setsockopt(fd, IPPROTO_TCP,  TCP_KEEPINTVL, &intvl_s,sizeof(intvl_s));
    setsockopt(fd, IPPROTO_TCP,  TCP_KEEPCNT,   &cnt,    sizeof(cnt));
}

// ─────────────────────────────────────────────────────────────────────────────
// CPU Affinity — pin a thread to a specific core
// ─────────────────────────────────────────────────────────────────────────────

// Pinning to a CPU core has two effects:
// 1. Thread never migrates to another core → no cache warm-up cost
// 2. If combined with isolcpus boot param, no kernel threads disturb you
//
// In production HFT, you boot with isolcpus=2,3 so the kernel scheduler
// never touches cores 2 and 3. You pin your hot threads there.
// We can't change boot params here, but we can still demonstrate the
// cache-locality benefit of pinning vs unpinned.
inline bool pin_to_cpu(int cpu_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    if (rc != 0) {
        std::cerr << "[WARN] Could not pin to CPU " << cpu_id
                  << " (need root for some systems): " << strerror(rc) << "\n";
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Reliable send/recv — TCP is a stream protocol.
// send() and recv() can return partial data. You MUST loop.
// Every beginner gets burned by this once. Don't be that person in an interview.
// ─────────────────────────────────────────────────────────────────────────────

inline bool send_all(int fd, const void* buf, size_t len) {
    const char* ptr = static_cast<const char*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t sent = send(fd, ptr, remaining, MSG_NOSIGNAL);
        if (sent <= 0) return false;  // connection closed or error
        ptr       += sent;
        remaining -= sent;
    }
    return true;
}

inline bool recv_all(int fd, void* buf, size_t len) {
    char* ptr = static_cast<char*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t got = recv(fd, ptr, remaining, 0);
        if (got <= 0) return false;  // connection closed or error
        ptr       += got;
        remaining -= got;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ANSI color codes — makes terminal output readable
// ─────────────────────────────────────────────────────────────────────────────
#define CLR_RESET  "\033[0m"
#define CLR_BOLD   "\033[1m"
#define CLR_GREEN  "\033[32m"
#define CLR_YELLOW "\033[33m"
#define CLR_CYAN   "\033[36m"
#define CLR_RED    "\033[31m"
#define CLR_DIM    "\033[2m"
