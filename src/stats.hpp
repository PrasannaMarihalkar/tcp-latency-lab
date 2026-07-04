#pragma once

#include <vector>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <iomanip>
#include <string>
#include <sstream>
#include "common.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// LatencyStats — collects samples and computes percentiles
//
// Why not a running average? Because average latency is MEANINGLESS in HFT.
// A strategy with avg=1µs but P99=10ms will miss trades at the worst moment.
// Tail latency (P99, P99.9) is what matters. A spike at P99.9 happens every
// 1-in-1000 messages. At 100K msgs/sec that's 100 spikes per second.
// ─────────────────────────────────────────────────────────────────────────────
class LatencyStats {
public:
    void reserve(size_t n) { samples_.reserve(n); }

    void record(int64_t ns) {
        samples_.push_back(ns);
    }

    size_t count() const { return samples_.size(); }

    // Call once before querying percentiles — sorts in place
    void finalize() {
        std::sort(samples_.begin(), samples_.end());
    }

    int64_t min()  const { return samples_.front(); }
    int64_t max()  const { return samples_.back(); }

    double mean() const {
        double sum = std::accumulate(samples_.begin(), samples_.end(), 0.0);
        return sum / samples_.size();
    }

    // Percentile: p50, p90, p99, p99_9, p99_99
    int64_t percentile(double p) const {
        size_t idx = static_cast<size_t>(std::ceil(p / 100.0 * samples_.size())) - 1;
        idx = std::min(idx, samples_.size() - 1);
        return samples_[idx];
    }

    // Standard deviation — useful to see if you have bimodal distribution
    double stddev() const {
        double m = mean();
        double variance = 0;
        for (auto s : samples_) variance += (s - m) * (s - m);
        return std::sqrt(variance / samples_.size());
    }

    // ─────────────────────────────────────────────────────────────────────
    // ASCII Latency Histogram — the money shot of your README
    // Bins are in nanoseconds. Logarithmic-ish scale to capture the range.
    // ─────────────────────────────────────────────────────────────────────
    void print_histogram(const std::string& label) const {
        // Define histogram bins (in nanoseconds)
        struct Bin { int64_t lo; int64_t hi; std::string label; };
        std::vector<Bin> bins = {
            {0,        10'000,   "< 10µs   "},
            {10'000,   25'000,   "10-25µs  "},
            {25'000,   50'000,   "25-50µs  "},
            {50'000,   100'000,  "50-100µs "},
            {100'000,  250'000,  "100-250µs"},
            {250'000,  500'000,  "250-500µs"},
            {500'000,  1'000'000,"500µs-1ms"},
            {1'000'000,INT64_MAX,"> 1ms    "},
        };

        // Count samples per bin
        std::vector<size_t> counts(bins.size(), 0);
        for (auto s : samples_) {
            for (size_t i = 0; i < bins.size(); ++i) {
                if (s >= bins[i].lo && s < bins[i].hi) {
                    counts[i]++;
                    break;
                }
            }
        }

        size_t max_count = *std::max_element(counts.begin(), counts.end());
        size_t bar_width = 40;

        std::cout << "\n" << CLR_BOLD << CLR_CYAN
                  << "  Latency Histogram — " << label << CLR_RESET << "\n";
        std::cout << CLR_DIM
                  << "  ┌──────────────────────────────────────────────────────────┐\n"
                  << CLR_RESET;

        for (size_t i = 0; i < bins.size(); ++i) {
            size_t bar_len = max_count > 0 ? (counts[i] * bar_width / max_count) : 0;
            double pct = 100.0 * counts[i] / samples_.size();

            // Color the bar: green=fast, yellow=medium, red=slow
            const char* color = CLR_GREEN;
            if (i >= 5) color = CLR_YELLOW;
            if (i >= 7) color = CLR_RED;

            std::cout << "  │ " << CLR_DIM << bins[i].label << CLR_RESET << " │"
                      << color;
            for (size_t b = 0; b < bar_len; ++b) std::cout << "█";
            for (size_t b = bar_len; b < bar_width; ++b) std::cout << " ";
            std::cout << CLR_RESET
                      << "│ " << CLR_DIM
                      << std::setw(6) << counts[i]
                      << " (" << std::fixed << std::setprecision(1) << pct << "%)"
                      << CLR_RESET << "\n";
        }
        std::cout << CLR_DIM
                  << "  └──────────────────────────────────────────────────────────┘\n"
                  << CLR_RESET;
    }

    // ─────────────────────────────────────────────────────────────────────
    // Full percentile report — the table you put in your README
    // ─────────────────────────────────────────────────────────────────────
    void print_report(const std::string& label) const {
        std::cout << "\n" << CLR_BOLD << CLR_CYAN
                  << "  ══════════════════════════════════════════\n"
                  << "   Latency Report: " << label << "\n"
                  << "  ══════════════════════════════════════════" << CLR_RESET << "\n";

        auto row = [](const std::string& name, int64_t ns) {
            const char* color = CLR_GREEN;
            if (ns > 100'000) color = CLR_YELLOW;
            if (ns > 1'000'000) color = CLR_RED;
            std::cout << "  " << CLR_DIM << std::left << std::setw(12) << name
                      << CLR_RESET << " │ " << color << std::right
                      << std::setw(12) << fmt_ns(ns) << CLR_RESET << "\n";
        };

        row("min",     min());
        row("mean",    static_cast<int64_t>(mean()));
        row("P50",     percentile(50));
        row("P75",     percentile(75));
        row("P90",     percentile(90));
        row("P99",     percentile(99));
        row("P99.9",   percentile(99.9));
        row("P99.99",  percentile(99.99));
        row("max",     max());

        std::cout << "  " << CLR_DIM << std::left << std::setw(12) << "stddev"
                  << CLR_RESET << " │ " << std::right << std::setw(12)
                  << fmt_ns(static_cast<int64_t>(stddev())) << "\n";
        std::cout << "  " << CLR_DIM << std::left << std::setw(12) << "samples"
                  << CLR_RESET << " │ " << std::right << std::setw(12)
                  << samples_.size() << "\n";
    }

    // Machine-readable CSV line for comparison tables
    std::string to_csv(const std::string& label) const {
        std::ostringstream oss;
        oss << label << ","
            << min() << ","
            << static_cast<int64_t>(mean()) << ","
            << percentile(50) << ","
            << percentile(99) << ","
            << percentile(99.9) << ","
            << max();
        return oss.str();
    }

private:
    std::vector<int64_t> samples_;
};
