// ─────────────────────────────────────────────────────────────────────────────
// benchmark.cpp — single-binary latency benchmark
//
// Runs the echo server in a background thread and the client in the main thread.
// This lets us demo all 4 scenarios (Nagle ON/OFF, QUICKACK, CPU pin) in one run
// without needing two terminals.
//
// The server thread re-creates its listen socket for each scenario so that
// socket options apply cleanly between runs.
// ─────────────────────────────────────────────────────────────────────────────

#include "common.hpp"
#include "stats.hpp"

#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <csignal>
#include <getopt.h>
#include <vector>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// Scenario definition
// ─────────────────────────────────────────────────────────────────────────────
struct Scenario {
    std::string name;
    std::string desc;
    bool nodelay;
    bool quickack;
};

static const std::vector<Scenario> SCENARIOS = {
    {
        "0  Baseline      (Nagle ON)",
        "Default kernel behavior. Nagle buffers small writes. See the damage.",
        false, false
    },
    {
        "1  TCP_NODELAY   (Nagle OFF)",
        "Nagle disabled. Every write goes to the wire immediately. The #1 HFT fix.",
        true,  false
    },
    {
        "2  NODELAY+QACK  (both ON)",
        "Nagle OFF + immediate ACKs. No delay anywhere in the ACK path.",
        true,  true
    },
    {
        "3  All Opts      (full HFT)",
        "NODELAY + QUICKACK + large buffers. Production-equivalent socket config.",
        true,  true
    },
};

// ─────────────────────────────────────────────────────────────────────────────
// Server thread — echo server for one scenario
// ─────────────────────────────────────────────────────────────────────────────
static void run_server_thread(int port, bool nodelay, bool quickack,
                               std::atomic<bool>& server_ready,
                               std::atomic<bool>& server_stop) {
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) { perror("server socket"); return; }

    set_reuseaddr(sfd);

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("server bind"); close(sfd); return;
    }
    if (listen(sfd, 5) < 0) {
        perror("server listen"); close(sfd); return;
    }

    server_ready.store(true);

    // Set non-blocking so we can check server_stop
    struct timeval tv;
    tv.tv_sec  = 0;
    tv.tv_usec = 200000; // 200ms timeout
    setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int cfd = -1;
    while (!server_stop.load()) {
        struct sockaddr_in ca{};
        socklen_t cl = sizeof(ca);
        cfd = accept(sfd, (struct sockaddr*)&ca, &cl);
        if (cfd >= 0) break;
    }

    if (cfd < 0) { close(sfd); return; }

    if (nodelay)  set_nodelay(cfd);
    if (quickack) set_quickack(cfd);
    set_buffers(cfd, 4 * 1024 * 1024);

    Message msg;
    while (!server_stop.load()) {
        if (!recv_all(cfd, &msg, sizeof(msg))) break;
        if (!send_all(cfd, &msg, sizeof(msg))) break;
        // Re-apply QUICKACK every recv — Linux resets it silently
        if (quickack) set_quickack(cfd);
    }

    close(cfd);
    close(sfd);
}

// ─────────────────────────────────────────────────────────────────────────────
// Run one full scenario: start server thread, connect client, measure, teardown
// ─────────────────────────────────────────────────────────────────────────────
static LatencyStats run_scenario(const Scenario& sc, int port,
                                  int count, int warmup) {
    std::atomic<bool> ready{false}, stop{false};

    // Spin up server thread
    std::thread srv([&]() {
        run_server_thread(port, sc.nodelay, sc.quickack, ready, stop);
    });

    // Wait for server to be ready
    while (!ready.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::this_thread::sleep_for(std::chrono::milliseconds(10)); // settle

    // ── Client: connect ───────────────────────────────────────────────────────
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    set_reuseaddr(fd);
    set_buffers(fd, 4 * 1024 * 1024);
    if (sc.nodelay)  set_nodelay(fd);
    if (sc.quickack) set_quickack(fd);

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    connect(fd, (struct sockaddr*)&addr, sizeof(addr));

    Message msg{};
    memset(msg.payload, 0xAB, sizeof(msg.payload));

    // ── Warmup ────────────────────────────────────────────────────────────────
    std::cout << CLR_DIM << "    Warming up (" << warmup << " msgs)..." << CLR_RESET << std::flush;
    for (int i = 0; i < warmup; ++i) {
        msg.seq = i; msg.send_ts = now_ns();
        send_all(fd, &msg, sizeof(msg));
        recv_all(fd, &msg, sizeof(msg));
        if (sc.quickack) set_quickack(fd);
    }
    std::cout << CLR_GREEN << " done\n" << CLR_RESET;

    // ── Measurement ───────────────────────────────────────────────────────────
    LatencyStats stats;
    stats.reserve(count);

    std::cout << CLR_DIM << "    Measuring  (" << count << " msgs) " << CLR_RESET << std::flush;

    int tick = count / 20; // print a dot every 5%
    for (int i = 0; i < count; ++i) {
        msg.seq     = i;
        msg.send_ts = now_ns();

        send_all(fd, &msg, sizeof(msg));
        Message reply{};
        recv_all(fd, &reply, sizeof(reply));

        stats.record(now_ns() - reply.send_ts);

        if (sc.quickack) set_quickack(fd);
        if (tick > 0 && (i + 1) % tick == 0)
            std::cout << CLR_GREEN << "▓" << CLR_RESET << std::flush;
    }
    std::cout << CLR_GREEN << " done\n" << CLR_RESET;

    close(fd);
    stop.store(true);
    srv.join();

    stats.finalize();
    return stats;
}

// ─────────────────────────────────────────────────────────────────────────────
// Pretty banner
// ─────────────────────────────────────────────────────────────────────────────
static void print_header() {
    std::cout << "\n"
        << CLR_BOLD << CLR_CYAN
        << "  ╔══════════════════════════════════════════════════════════╗\n"
        << "  ║         TCP Latency Lab  —  HFT Socket Benchmark        ║\n"
        << "  ║   Proving why every HFT engineer disables Nagle's algo  ║\n"
        << "  ╚══════════════════════════════════════════════════════════╝\n"
        << CLR_RESET << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Final comparison table
// ─────────────────────────────────────────────────────────────────────────────
static void print_comparison_table(const std::vector<LatencyStats>& results) {
    // Get baseline for speedup calculation
    int64_t baseline_p99 = results[0].percentile(99);

    std::cout << "\n"
        << CLR_BOLD << CLR_CYAN
        << "  ╔══════════════════════════════════════════════════════════════════════╗\n"
        << "  ║               FINAL COMPARISON — All Scenarios                     ║\n"
        << "  ╠══════════════════════════════════════════════════════════════════════╣\n"
        << CLR_RESET;

    // Header
    std::cout << CLR_BOLD
        << "  │ " << std::left  << std::setw(27) << "Scenario"
        << " │" << std::right << std::setw(9)  << "P50"
        << " │" << std::right << std::setw(9)  << "P99"
        << " │" << std::right << std::setw(9)  << "P99.9"
        << " │" << std::right << std::setw(9)  << "max"
        << " │" << std::right << std::setw(9)  << "speedup"
        << " │\n" << CLR_RESET;

    std::cout << "  ├" << std::string(28, '-')
              << "┼" << std::string(10, '-')
              << "┼" << std::string(10, '-')
              << "┼" << std::string(10, '-')
              << "┼" << std::string(10, '-')
              << "┼" << std::string(10, '-') << "┤\n";

    for (size_t i = 0; i < results.size(); ++i) {
        const auto& s = results[i];
        int64_t p99 = s.percentile(99);
        double speedup = static_cast<double>(baseline_p99) / p99;

        const char* c = CLR_GREEN;
        if (i == 0) c = CLR_DIM;
        if (p99 > 500'000) c = CLR_YELLOW;
        if (p99 > 5'000'000) c = CLR_RED;

        std::string spd_str = (i == 0)
            ? "baseline"
            : (std::to_string(static_cast<int>(speedup * 10) / 10) + "." +
               std::to_string(static_cast<int>(speedup * 10) % 10) + "x");

        std::cout << c
            << "  │ " << std::left  << std::setw(27) << SCENARIOS[i].name
            << " │" << std::right << std::setw(9)  << fmt_ns(s.percentile(50))
            << " │" << std::right << std::setw(9)  << fmt_ns(p99)
            << " │" << std::right << std::setw(9)  << fmt_ns(s.percentile(99.9))
            << " │" << std::right << std::setw(9)  << fmt_ns(s.max())
            << " │" << std::right << std::setw(9)  << spd_str
            << " │\n" << CLR_RESET;
    }

    std::cout << "  └" << std::string(28, '-')
              << "┴" << std::string(10, '-')
              << "┴" << std::string(10, '-')
              << "┴" << std::string(10, '-')
              << "┴" << std::string(10, '-')
              << "┴" << std::string(10, '-') << "┘\n";

    // Insight line
    if (results.size() >= 2) {
        int64_t p99_base = results[0].percentile(99);
        int64_t p99_fast = results.back().percentile(99);
        double  gain     = static_cast<double>(p99_base) / p99_fast;
        std::cout << "\n  " << CLR_BOLD << CLR_GREEN
                  << "  ► P99 improvement: " << fmt_ns(p99_base)
                  << "  →  " << fmt_ns(p99_fast)
                  << "  (" << static_cast<int>(gain) << "x faster)"
                  << CLR_RESET << "\n";
    }
    std::cout << "\n  " << CLR_DIM
              << "  This is why every HFT firm sets TCP_NODELAY on every socket.\n"
              << CLR_RESET << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Save CSV for README tables
// ─────────────────────────────────────────────────────────────────────────────
static void save_csv(const std::vector<LatencyStats>& results) {
    FILE* f = fopen("results/benchmark.csv", "w");
    if (!f) return;
    fprintf(f, "scenario,min_ns,mean_ns,p50_ns,p99_ns,p999_ns,max_ns\n");
    for (size_t i = 0; i < results.size(); ++i) {
        const auto& s = results[i];
        fprintf(f, "\"%s\",%ld,%ld,%ld,%ld,%ld,%ld\n",
            SCENARIOS[i].name.c_str(),
            s.min(),
            static_cast<int64_t>(s.mean()),
            s.percentile(50),
            s.percentile(99),
            s.percentile(99.9),
            s.max());
    }
    fclose(f);
    std::cout << "  " << CLR_DIM << "CSV saved → results/benchmark.csv\n" << CLR_RESET;
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    signal(SIGPIPE, SIG_IGN);

    int count  = 100'000;
    int warmup = 5'000;
    int port   = 9991;

    static struct option opts[] = {
        {"count",  required_argument, 0, 'c'},
        {"warmup", required_argument, 0, 'w'},
        {"port",   required_argument, 0, 'p'},
        {0, 0, 0, 0}
    };
    int opt, idx;
    while ((opt = getopt_long(argc, argv, "c:w:p:", opts, &idx)) != -1) {
        switch (opt) {
            case 'c': count  = std::stoi(optarg); break;
            case 'w': warmup = std::stoi(optarg); break;
            case 'p': port   = std::stoi(optarg); break;
        }
    }

    print_header();
    std::cout << "  " << CLR_DIM
              << count << " messages per scenario, "
              << warmup << " warmup messages\n\n" << CLR_RESET;

    std::vector<LatencyStats> all_results;

    for (size_t i = 0; i < SCENARIOS.size(); ++i) {
        const auto& sc = SCENARIOS[i];

        std::cout << CLR_BOLD << "\n  ┌─ Scenario " << i << ": "
                  << sc.name << " ─\n" << CLR_RESET;
        std::cout << "  " << CLR_DIM << "  " << sc.desc << "\n" << CLR_RESET;

        // Use different port per scenario to avoid TIME_WAIT issues
        auto stats = run_scenario(sc, port + static_cast<int>(i), count, warmup);

        stats.print_report(sc.name);
        stats.print_histogram(sc.name);

        all_results.push_back(std::move(stats));

        // Brief pause between scenarios so TIME_WAIT sockets clear
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    print_comparison_table(all_results);
    save_csv(all_results);

    return 0;
}
