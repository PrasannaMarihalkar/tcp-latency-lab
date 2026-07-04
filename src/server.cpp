// ─────────────────────────────────────────────────────────────────────────────
// tcp-latency-lab / server.cpp
//
// A low-latency echo server that demonstrates every socket optimization
// relevant to HFT order entry connections.
//
// Usage:
//   ./server [OPTIONS]
//
// Options:
//   --port N         Port to listen on (default: 9999)
//   --nodelay        Enable TCP_NODELAY (disable Nagle's algorithm)
//   --quickack       Enable TCP_QUICKACK (re-applied after each recv)
//   --affinity N     Pin server thread to CPU N
//   --bufsize N      Socket buffer size in bytes (default: 4MB)
//   --verbose        Log each echoed message
//
// The server is single-threaded intentionally.
// In production, you'd have one dedicated pinned thread per connection.
// Multi-threading with locking adds jitter — the enemy of low latency.
// ─────────────────────────────────────────────────────────────────────────────

#include "common.hpp"
#include <csignal>
#include <cstdio>
#include <iomanip>
#include <getopt.h>
#include <atomic>

// ─────────────────────────────────────────────────────────────────────────────
// Config — parsed from command-line args
// ─────────────────────────────────────────────────────────────────────────────
struct ServerConfig {
    int  port       = 9999;
    bool nodelay    = false;
    bool quickack   = false;
    int  cpu        = -1;     // -1 = no pinning
    int  bufsize    = 4 * 1024 * 1024;  // 4MB
    bool verbose    = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// Graceful shutdown on Ctrl+C
// ─────────────────────────────────────────────────────────────────────────────
static std::atomic<bool> g_running{true};
static void on_signal(int) { g_running.store(false); }

// ─────────────────────────────────────────────────────────────────────────────
// Print startup banner — shows which optimizations are active
// ─────────────────────────────────────────────────────────────────────────────
static void print_banner(const ServerConfig& cfg) {
    std::cout << CLR_BOLD << CLR_CYAN
              << "\n  ╔══════════════════════════════════════╗\n"
              << "  ║   TCP Latency Lab — Echo Server      ║\n"
              << "  ╚══════════════════════════════════════╝\n"
              << CLR_RESET;

    auto flag = [](const char* name, bool enabled) {
        std::cout << "  " << std::left << std::setw(18) << name
                  << (enabled ? (CLR_GREEN "  ✓ ON" CLR_RESET)
                              : (CLR_DIM   "  ✗ off" CLR_RESET))
                  << "\n";
    };

    std::cout << "\n  " << CLR_DIM << "Listening on port  " << CLR_RESET
              << CLR_BOLD << cfg.port << CLR_RESET << "\n";
    flag("TCP_NODELAY",  cfg.nodelay);
    flag("TCP_QUICKACK", cfg.quickack);
    if (cfg.cpu >= 0)
        std::cout << "  " << CLR_DIM << "CPU affinity       " << CLR_RESET
                  << CLR_BOLD << "  core " << cfg.cpu << CLR_RESET << "\n";
    else
        std::cout << "  " << CLR_DIM << "CPU affinity       " << CLR_RESET
                  << CLR_DIM << "  floating\n" << CLR_RESET;
    std::cout << "  " << CLR_DIM << "Socket buffers     " << CLR_RESET
              << CLR_BOLD << cfg.bufsize / 1024 << " KB\n" << CLR_RESET;
    std::cout << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Handle a single client connection — the hot loop
//
// This function blocks for the lifetime of the connection.
// In a real HFT system this runs pinned on a dedicated core.
// ─────────────────────────────────────────────────────────────────────────────
static void handle_client(int client_fd, const ServerConfig& cfg,
                           const std::string& client_ip) {
    // Apply socket options to the accepted connection (not just the listen socket)
    // TCP_NODELAY on accept fd is what actually disables Nagle for data flow
    if (cfg.nodelay)  set_nodelay(client_fd);
    if (cfg.quickack) set_quickack(client_fd);
    set_buffers(client_fd, cfg.bufsize);
    set_keepalive(client_fd);

    std::cout << CLR_GREEN << "  [+] Client connected: " << client_ip
              << CLR_RESET << "\n";

    uint64_t msg_count = 0;
    Message  msg;

    while (g_running.load(std::memory_order_relaxed)) {
        // ── Receive a full message (loop until all 44 bytes arrive) ──────────
        if (!recv_all(client_fd, &msg, sizeof(msg))) break;

        // ── Echo it straight back — no modification needed ───────────────────
        // The client embedded its send timestamp. When it gets this back,
        // it computes: RTT = now - msg.send_ts
        if (!send_all(client_fd, &msg, sizeof(msg))) break;

        ++msg_count;

        // ── Re-apply TCP_QUICKACK after EVERY recv() ─────────────────────────
        // Linux resets TCP_QUICKACK after each receive operation.
        // If you set it once and forget, you'll see delayed ACKs creep back.
        // This is the #1 TCP_QUICKACK gotcha. Almost nobody knows this.
        if (cfg.quickack) set_quickack(client_fd);

        if (cfg.verbose && msg_count % 10'000 == 0)
            std::cout << CLR_DIM << "  [server] echoed " << msg_count
                      << " messages\n" << CLR_RESET;
    }

    std::cout << CLR_YELLOW << "  [-] Client disconnected after "
              << msg_count << " messages\n" << CLR_RESET;
    close(client_fd);
}

// ─────────────────────────────────────────────────────────────────────────────
// main — set up listen socket and accept loop
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    ServerConfig cfg;

    // ── Parse arguments ───────────────────────────────────────────────────────
    static struct option opts[] = {
        {"port",      required_argument, 0, 'p'},
        {"nodelay",   no_argument,       0, 'n'},
        {"quickack",  no_argument,       0, 'q'},
        {"affinity",  required_argument, 0, 'a'},
        {"bufsize",   required_argument, 0, 'b'},
        {"verbose",   no_argument,       0, 'v'},
        {0, 0, 0, 0}
    };

    int opt, idx;
    while ((opt = getopt_long(argc, argv, "p:nqa:b:v", opts, &idx)) != -1) {
        switch (opt) {
            case 'p': cfg.port    = std::stoi(optarg); break;
            case 'n': cfg.nodelay = true;               break;
            case 'q': cfg.quickack= true;               break;
            case 'a': cfg.cpu     = std::stoi(optarg); break;
            case 'b': cfg.bufsize = std::stoi(optarg); break;
            case 'v': cfg.verbose = true;               break;
        }
    }

    // ── Pin server thread if requested ────────────────────────────────────────
    if (cfg.cpu >= 0) pin_to_cpu(cfg.cpu);

    // ── Set up signal handler for clean shutdown ──────────────────────────────
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);  // ignore broken pipe — handle it via send() return value

    print_banner(cfg);

    // ── Create listen socket ──────────────────────────────────────────────────
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    // SO_REUSEADDR: if you kill and restart the server quickly,
    // the old port is still in TIME_WAIT. Without this you get
    // "Address already in use" for up to 2 minutes. Unacceptable.
    set_reuseaddr(server_fd);

    // Increase listen socket buffers too
    set_buffers(server_fd, cfg.bufsize);

    // ── Bind ─────────────────────────────────────────────────────────────────
    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(cfg.port);
    addr.sin_addr.s_addr = INADDR_ANY;  // bind to all interfaces

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    // listen(fd, backlog): backlog = how many pending connections to queue
    // before the kernel starts refusing new SYNs.
    // We use 128 — the Linux default max (see /proc/sys/net/core/somaxconn).
    // For a latency-critical server, we'd tune this down and keep only
    // one pre-established connection. Never accept strangers during trading.
    if (listen(server_fd, 128) < 0) {
        perror("listen");
        return 1;
    }

    std::cout << "  " << CLR_GREEN << "Listening... (Ctrl+C to stop)"
              << CLR_RESET << "\n\n";

    // ── Accept loop ───────────────────────────────────────────────────────────
    // Single-threaded: handle one client at a time.
    // For our benchmark this is fine — we have exactly one client.
    while (g_running.load()) {
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;  // interrupted by signal, check g_running
            perror("accept");
            break;
        }

        char ip_buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_buf, sizeof(ip_buf));
        std::string client_ip = std::string(ip_buf) + ":" +
                                 std::to_string(ntohs(client_addr.sin_port));

        handle_client(client_fd, cfg, client_ip);
    }

    close(server_fd);
    std::cout << "\n  Server shut down cleanly.\n";
    return 0;
}
