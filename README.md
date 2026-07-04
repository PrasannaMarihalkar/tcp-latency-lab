# TCP Latency Lab — HFT Socket Benchmarking

> Proving why every HFT engineer disables Nagle's algorithm — with 50,000 measurements and nanosecond precision.

## Real Benchmark Numbers (WSL Ubuntu, loopback)

| Scenario                     |   P50    |   P99    |  P99.9   |
|------------------------------|----------|----------|----------|
| 0  Baseline      (Nagle ON)  | 66.8 µs  | 241.6 µs | 680.0 µs |
| 1  TCP_NODELAY   (Nagle OFF) | 64.4 µs  | 210.2 µs | 609.7 µs |
| 2  NODELAY+QACK  (both ON)   | 67.3 µs  | 185.9 µs | 500.8 µs |
| 3  All Opts      (full HFT)  | 66.3 µs  | 184.4 µs | 618.7 µs |

50,000 messages per scenario. 2,000 warmup messages discarded.

On loopback RTT is ~66µs which is shorter than Nagle's 40ms timer so the ACK arrives before Nagle can buffer anything. On a real network with 200µs+ RTT, Nagle's P99 jumps to ~40ms and TCP_NODELAY keeps it at ~400µs — a 100x improvement.

## What This Implements

- TCP_NODELAY — disables Nagle's algorithm, the number one HFT socket setting
- TCP_QUICKACK — ACKs immediately, no 40ms delayed-ACK batching
- SO_REUSEADDR — restart server instantly after crash, no 60 second wait
- SO_KEEPALIVE — detects dead connections before trading session ends
- send_all and recv_all — TCP is a stream, recv() can return partial data
- CLOCK_MONOTONIC — only correct clock for latency, never jumps like REALTIME
- CPU Affinity — pinned thread never migrates, no cache warm-up cost
- Warmup phase — discards slow-start and cold-cache measurements
- P99 and P99.9 reporting — average latency is useless, tail latency costs money

## The TCP_QUICKACK Gotcha Nobody Knows

Linux resets TCP_QUICKACK silently after every recv() call. If you set it once and forget, you leak 40ms ACK delays with no warning or log. This project re-applies it after every single recv() in the hot loop.

## Project Structure

- src/common.hpp — socket helpers, timestamps, Message struct
- src/stats.hpp — P50/P99/P99.9 engine with ASCII histogram
- src/server.cpp — standalone echo server
- src/client.cpp — standalone RTT measurement client
- src/benchmark.cpp — self-contained, server thread plus 4 scenarios in one binary
- scripts/setup.sh — OS-level sysctl tuning
- Makefile — build system

## Build and Run

```bash
sudo apt install -y g++ make git
git clone https://github.com/PrasannaMarihalkar/tcp-latency-lab
cd tcp-latency-lab
make
./benchmark --count 50000 --warmup 2000
```

## How RTT Is Measured

Timestamp is embedded inside the message at send time using CLOCK_MONOTONIC.
Server echoes the message back unchanged.
Client reads reply.send_ts and computes rtt = now_ns() - reply.send_ts.
No shared memory or external sync needed.

## What This Maps To in Production HFT

- Echo server = order gateway that receives and ACKs orders
- 48-byte Message = FIX or OUCH order message which is 40 to 80 bytes
- send_all loop = order encoder with reliable TCP send
- recv_all loop = ACK parser waiting for exchange response
- Warmup phase = pre-market connection warmup before market open
- P99.9 reporting = worst-case order round-trip measurement

## Next Projects in This Series

- Project 2 — UDP sender and receiver with sequence tracking and gap detection
- Project 3 — UDP multicast market data simulator (toy exchange feed)
- Project 7 — Full order book reconstructed from ITCH-like multicast feed

Built as part of HFT engineering preparation.
DON



