// ─────────────────────────────────────────────────────────────────────────────
// tcp-latency-lab / client.cpp
//
// Benchmarking client that measures round-trip latency across 4 scenarios:
//
//   Mode 0 — Baseline      (Nagle ON,  no affinity)
//   Mode 1 — TCP_NODELAY   (Nagle OFF, no affinity)
//   Mode 2 — NODELAY + Pin (Nagle OFF, pinned to CPU)
//   Mode 3 — All opts      (Nagle OFF, QUICKACK, pinned, large buffers)
//
// At the end, it prints a side-by-side comparison table and saves CSV.
//
// Usage:
//   ./client [OPTIONS]
//
// Options:
//   --host H         Server IP (default: 127.0.0.1)
//   --port N         Server port (default: 9999)
//   --count N        Messages per scenario (default: 100000)
//   --warmup N       Warmup messages to discard (default: 5000)
//   --affinity N     CPU to pin client thread to (default: -1)
//   --mode N         Run only scenario N (0-3), default: all
//   --nodelay        Force enable TCP_NODELAY
//   --quickack       Force enable TCP_QUICKACK
// ─────────────────────────────────────────────────────────────────────────────

#include "common.hpp"
#include "stats.hpp"
#include <getopt.h>
#include <vector>
#include <array>
#include <functional>

// ─────────────────────────────────────────────────────────────────────────────
// Benchmark scenario descriptor
// ─────────────────────────────────────────────────────────────────────────────
struct Scenario {
    std::string name;
    bool nodelay;
    bool quickack;
    bool affinity;
    std::string description;  // one-line explanation of what this tests
};

// ─────────────────────────────────────────────────────────────────────────────
// Client config
// ─────────────────────────────────────────────────────────────────────────────
struct ClientConfig {
    std::string host    = "127.0.0.1";
    int  port           = 9999;
    int  count          = 100'000;
    int  warmup         = 5'000;
    int  cpu            = -1;
    int  mode           = -1;   // -1 = run all scenarios
    bool nodelay_force  = false;
    bool quickack_force = false;
    int  bufsize        = 4 * 1024 * 1024;
};

// ─────────────────────────────────────────────────────────────────────────────
// Connect to server and apply socket options for this scenario
// ─────────────────────────────────────────────────────────────────────────────
static int connect_to_server(const ClientConfig& cfg, const Scenario& sc) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) throw std::system_error(errno, std::generic_category(), "socket");

    set_reuseaddr(fd);
    set_buffers(fd, cfg.bufsize);

    // CRITICAL: Set TCP_NODELAY BEFORE connect().
    // Some guides say set it after connect — both work, but setting it before
    // ensures the SYN-ACK exchange isn't buffered by Nagle before any data flows.
    // Technically Nagle only applies to data (not SYN), but it's good habit.
    if (sc.nodelay || cfg.nodelay_force)   set_nodelay(fd);
    if (sc.quickack || cfg.quickack_force) set_quickack(fd);

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(cfg.port);
    if (inet_pton(AF_INET, cfg.host.c_str(), &addr.sin_addr) <= 0)
        throw std::runtime_error("Invalid host: " + cfg.host);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        throw std::system_error(errno, std::generic_category(), "connect");
    }

    return fd;
}

// ─────────────────────────────────────────────────────────────────────────────
// Run one benchmark scenario
// ─────────────────────────────────────────────────────────────────────────────
static LatencyStats run_scenario(const ClientConfig& cfg, const Scenario& sc) {
    std::cout << "\n" << CLR_BOLD << CLR_CYAN
              << "  ── Running: " << sc.name << " ──" << CLR_RESET << "\n"
              << "  " << CLR_DIM << sc.description << CLR_RESET << "\n";

    // ── CPU affinity ─────────────────────────────────────────────────────────
    if (sc.affinity && cfg.cpu >= 0) {
        if (pin_to_cpu(cfg.cpu))
            std::cout << "  " << CLR_GREEN << "  ✓ Pinned to CPU " << cfg.cpu
                      << CLR_RESET << "\n";
    }

    int fd = connect_to_server(cfg, sc);

    // ── Warmup — TCP connection starts slow (slow start, cache cold, TLB cold) ──
    // The first N messages are always slower. Discard them so we measure
    // steady-state latency, not connection setup overhead.
    // This is NOT cheating — in production, connections are always pre-established.
    std::cout << "  " << CLR_DIM << "  Warming up (" << cfg.warmup
              << " msgs)..." << CLR_RESET << std::flush;

    Message msg{};
    memset(msg.payload, 0xAB, sizeof(msg.payload));

    for (int i = 0; i < cfg.warmup; ++i) {
        msg.seq     = i;
        msg.send_ts = now_ns();
        send_all(fd, &msg, sizeof(msg));
        recv_all(fd, &msg, sizeof(msg));
        if (sc.quickack || cfg.quickack_force) set_quickack(fd);
    }
    std::cout << " done.\n";

    // ── Measurement loop ─────────────────────────────────────────────────────
    LatencyStats stats;
    stats.reserve(cfg.count);

    std::cout << "  " << CLR_DIM << "  Measuring (" << cfg.count
              << " msgs)..." << CLR_RESET << std::flush;

    for (int i = 0; i < cfg.count; ++i) {
        msg.seq     = i;
        msg.send_ts = now_ns();

        if (!send_all(fd, &msg, sizeof(msg))) {
            std::cerr << CLR_RED << "  send failed at msg " << i << CLR_RESET << "\n";
            break;
        }

        Message reply{};
        if (!recv_all(fd, &reply, sizeof(reply))) {
            std::cerr << CLR_RED << "  recv failed at msg " << i << CLR_RESET << "\n";
            break;
        }

        // RTT = now - original send timestamp embedded in the message
        int64_t rtt = now_ns() - reply.send_ts;
        stats.record(rtt);

        // Re-apply QUICKACK after every recv (Linux resets it)
        if (sc.quickack || cfg.quickack_force) set_quickack(fd);

        // Progress bar every 10%
        if ((i + 1) % (cfg.count / 10) == 0) {
            std::cout << CLR_GREEN << "█" << CLR_RESET << std::flush;
        }
    }
    std::cout << " done.\n";

    close(fd);
    stats.finalize();
    return stats;
}

// ─────────────────────────────────────────────────────────────────────────────
// Print comparison table — the README-worthy output
// ─────────────────────────────────────────────────────────────────────────────
static void print_comparison(
    const std::vector<std::string>&    names,
    const std::vector<LatencyStats*>& results)
{
    std::cout << "\n\n" << CLR_BOLD << CLR_CYAN
              << "  ╔══════════════════════════════════════════════════════════════════╗\n"
              << "  ║              SCENARIO COMPARISON — Round-Trip Latency           ║\n"
              << "  ╚══════════════════════════════════════════════════════════════════╝\n"
              << CLR_RESET;

    std::cout << CLR_BOLD
              << "  " << std::left  << std::setw(24) << "Scenario"
              << "│" << std::right << std::setw(12) << "P50"
              << "│" << std::right << std::setw(12) << "P99"
              << "│" << std::right << std::setw(12) << "P99.9"
              << "│" << std::right << std::setw(12) << "max"
              << CLR_RESET << "\n";
    std::cout << "  " << std::string(72, '-') << "\n";

    // Find baseline (first result) for speedup calculation
    int64_t baseline_p99 = results[0]->percentile(99);

    for (size_t i = 0; i < results.size(); ++i) {
        auto* s = results[i];
        int64_t p99 = s->percentile(99);
        double speedup = static_cast<double>(baseline_p99) / p99;

        const char* row_color = (i == 0) ? CLR_DIM : CLR_GREEN;
        if (p99 > 1'000'000) row_color = CLR_RED;
        if (p99 > 100'000 && p99 <= 1'000'000) row_color = CLR_YELLOW;

        std::cout << "  " << row_color
                  << std::left  << std::setw(24) << names[i]
                  << "│" << std::right << std::setw(12) << fmt_ns(s->percentile(50))
                  << "│" << std::right << std::setw(12) << fmt_ns(p99)
                  << "│" << std::right << std::setw(12) << fmt_ns(s->percentile(99.9))
                  << "│" << std::right << std::setw(12) << fmt_ns(s->max())
                  << CLR_RESET;

        if (i > 0 && speedup > 1.5)
            std::cout << "  " << CLR_GREEN << CLR_BOLD << std::fixed
                      << std::setprecision(1) << speedup << "x faster" << CLR_RESET;
        std::cout << "\n";
    }

    std::cout << "\n  " << CLR_DIM
              << "Speedup calculated vs Scenario 0 at P99 latency.\n"
              << CLR_RESET;
}

// ─────────────────────────────────────────────────────────────────────────────
// Save results to CSV for README tables
// ─────────────────────────────────────────────────────────────────────────────
static void save_csv(const std::string& path,
                     const std::vector<std::string>&   names,
                     const std::vector<LatencyStats*>& results) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) { std::cerr << "Cannot write " << path << "\n"; return; }
    fprintf(f, "scenario,min_ns,mean_ns,p50_ns,p99_ns,p99_9_ns,max_ns\n");
    for (size_t i = 0; i < results.size(); ++i)
        fprintf(f, "%s\n", results[i]->to_csv(names[i]).c_str());
    fclose(f);
    std::cout << "\n  " << CLR_DIM << "Results saved to: " << path
              << CLR_RESET << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    ClientConfig cfg;

    static struct option opts[] = {
        {"host",     required_argument, 0, 'h'},
        {"port",     required_argument, 0, 'p'},
        {"count",    required_argument, 0, 'c'},
        {"warmup",   required_argument, 0, 'w'},
        {"affinity", required_argument, 0, 'a'},
        {"mode",     required_argument, 0, 'm'},
        {"nodelay",  no_argument,       0, 'n'},
        {"quickack", no_argument,       0, 'q'},
        {0, 0, 0, 0}
    };

    int opt, idx;
    while ((opt = getopt_long(argc, argv, "h:p:c:w:a:m:nq", opts, &idx)) != -1) {
        switch (opt) {
            case 'h': cfg.host           = optarg;           break;
            case 'p': cfg.port           = std::stoi(optarg); break;
            case 'c': cfg.count          = std::stoi(optarg); break;
            case 'w': cfg.warmup         = std::stoi(optarg); break;
            case 'a': cfg.cpu            = std::stoi(optarg); break;
            case 'm': cfg.mode           = std::stoi(optarg); break;
            case 'n': cfg.nodelay_force  = true;              break;
            case 'q': cfg.quickack_force = true;              break;
        }
    }

    // ── Define the 4 scenarios ────────────────────────────────────────────────
    std::vector<Scenario> scenarios = {
        {
            "0: Baseline",
            false, false, false,
            "No optimizations. Nagle's algorithm ON. OS decides scheduling."
        },
        {
            "1: TCP_NODELAY",
            true,  false, false,
            "Nagle OFF. Every write goes out immediately. The #1 HFT TCP setting."
        },
        {
            "2: NODELAY + Pin",
            true,  false, true,
            "Nagle OFF + CPU pinning. Reduces cache misses and scheduler jitter."
        },
        {
            "3: All Opts",
            true,  true,  true,
            "NODELAY + QUICKACK + CPU pin. Fastest possible with normal kernel."
        },
    };

    std::cout << CLR_BOLD << CLR_CYAN
              << "\n  TCP Latency Lab — Benchmark Client\n"
              << "  Connecting to " << cfg.host << ":" << cfg.port << "\n"
              << "  " << cfg.count << " messages per scenario, "
              << cfg.warmup << " warmup\n"
              << CLR_RESET;

    // ── Run scenarios ─────────────────────────────────────────────────────────
    std::vector<LatencyStats> all_stats;
    std::vector<std::string>  all_names;
    std::vector<LatencyStats*> all_ptrs;

    for (size_t i = 0; i < scenarios.size(); ++i) {
        if (cfg.mode >= 0 && static_cast<int>(i) != cfg.mode) continue;

        try {
            all_stats.push_back(run_scenario(cfg, scenarios[i]));
            all_names.push_back(scenarios[i].name);
        } catch (const std::exception& e) {
            std::cerr << CLR_RED << "  Scenario failed: " << e.what()
                      << CLR_RESET << "\n";
            std::cerr << "  Is the server running? "
                      << "Try: ./server --nodelay --quickack\n";
            return 1;
        }
    }

    // ── Print individual reports ──────────────────────────────────────────────
    for (size_t i = 0; i < all_stats.size(); ++i) {
        all_ptrs.push_back(&all_stats[i]);
        all_stats[i].print_report(all_names[i]);
        all_stats[i].print_histogram(all_names[i]);
    }

    // ── Comparison table ─────────────────────────────────────────────────────
    if (all_stats.size() > 1)
        print_comparison(all_names, all_ptrs);

    // ── Save CSV ──────────────────────────────────────────────────────────────
    save_csv("results/benchmark.csv", all_names, all_ptrs);

    std::cout << "\n" << CLR_BOLD
              << "  Done! Check results/benchmark.csv for README table data.\n"
              << CLR_RESET << "\n";
    return 0;
}
