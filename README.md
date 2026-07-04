# TCP Latency Lab — HFT Socket Benchmarking

> Proving why every HFT engineer disables Nagle's algorithm — with 50,000 measurements and nanosecond precision.

A production-grade, low-latency TCP echo server + benchmarking client written in C++17.
Demonstrates every socket-level optimization relevant to HFT order-entry connections.

---

## Real Benchmark Numbers (WSL Ubuntu, Linux 6.x, loopback)

| Scenario                     |   P50    |   P99    |  P99.9   |   max   |
|------------------------------|----------|----------|----------|---------|
| 0  Baseline      (Nagle ON)  | 66.8 µs  | 241.6 µs | 680.0 µs | 5.5 ms  |
| 1  TCP_NODELAY   (Nagle OFF) | 64.4 µs  | 210.2 µs | 609.7 µs | 2.4 ms  |
| 2  NODELAY+QACK  (both ON)   | 67.3 µs  | 185.9 µs | 500.8 µs | 2.9 ms  |
| 3  All Opts      (full HFT)  | 66.3 µs  | 184.4 µs | 618.7 µs | 2.2 ms  |

50,000 messages per scenario. 2,000 warmup messages discarded.

> **Why loopback differences are small**: Loopback RTT (~66µs) is shorter than Nagle's 40ms timer,
> so the ACK arrives before Nagle can buffer anything. On a real network with 200µs+ RTT,
> Nagle's P99 jumps to ~40ms. TCP_NODELAY keeps it at ~400µs — a **100x improvement**.
> This is why every HFT firm sets TCP_NODELAY on every single socket.

---

## What This Project Implements

| Concept | File | Why It Matters in HFT |
|---------|------|-----------------------|
| `TCP_NODELAY` | `common.hpp` | Disables Nagle — #1 HFT socket setting |
| `TCP_QUICKACK` | `common.hpp` | ACKs immediately, no 40ms delayed-ACK batching |
| `SO_REUSEADDR` | `common.hpp` | Restart server instantly after crash, no 60s wait |
| `SO_SNDBUF / SO_RCVBUF` | `common.hpp` | 4MB buffers handle burst without drops |
| `SO_KEEPALIVE` | `common.hpp` | Detects dead connections before trading ends |
| `send_all()` / `recv_all()` | `common.hpp` | TCP is a stream — recv() can return partial data |
| `CLOCK_MONOTONIC` | `common.hpp` | Only correct clock for latency — never jumps like REALTIME |
| CPU Affinity | `common.hpp` | Pinned thread never migrates, no cache warm-up cost |
| Warmup phase | `benchmark.cpp` | Discards slow-start and cold-cache measurements |
| P99 / P99.9 reporting | `stats.hpp` | Average latency is useless — tail latency costs money |

---

## The TCP_QUICKACK Gotcha Nobody Talks About

```cpp
// WRONG — Linux resets TCP_QUICKACK silently after every recv() call
set_quickack(fd);
recv_all(fd, &msg, sizeof(msg));
// TCP_QUICKACK is now OFF. You are leaking 40ms ACK delays with zero warning.

// CORRECT — re-apply after every single recv()
recv_all(fd, &msg, sizeof(msg));
set_quickack(fd);  // must re-apply every time
```

This resets silently. No warning. No log. Just mystery latency spikes at P99.
This project handles it correctly in the hot loop inside `benchmark.cpp`.

---

## Project Structure
tcp-latency-lab/
├── src/
│   ├── common.hpp       ← Socket helpers, timestamps, Message struct
│   ├── stats.hpp        ← P50/P99/P99.9 engine + ASCII histogram
│   ├── server.cpp       ← Standalone echo server (configurable opts)
│   ├── client.cpp       ← Standalone RTT measurement client
│   └── benchmark.cpp    ← Self-contained: server thread + 4 scenarios
├── scripts/
│   ├── setup.sh         ← OS-level sysctl tuning (needs sudo)
│   └── run_benchmark.sh ← Automated two-terminal benchmark runner
├── results/
│   └── benchmark.csv    ← Machine-readable output
└── Makefile

---

## Build

```bash
sudo apt install -y g++ make git iproute2
git clone https://github.com/PrasannaMarihalkar/tcp-latency-lab
cd tcp-latency-lab
make
```

---

## Run

### Option A — Self-contained (one command)
```bash
./benchmark --count 50000 --warmup 2000
```
Runs all 4 scenarios automatically. No second terminal needed.

### Option B — Two terminals (like real HFT: separate server and client)
```bash
# Terminal 1 — start the server
./server --nodelay --quickack --port 9999

# Terminal 2 — run the client
./client --host 127.0.0.1 --port 9999 --count 100000
```

---

## Simulate Real Network Conditions

On loopback, Nagle's 40ms timer rarely fires. Use `tc netem` to add artificial delay and see the real damage:

```bash
# Add 5ms one-way delay = 10ms RTT
sudo tc qdisc add dev lo root netem delay 5ms

./benchmark --count 10000 --warmup 500
# Baseline P99 explodes to ~12ms
# All optimized scenarios stay significantly tighter

# Remove delay when done
sudo tc qdisc del dev lo root
```

---

## How RTT Is Measured
Client                          Server
│                               │
│  t0 = now_ns()                │
│  msg.send_ts = t0             │
│──── send(msg, 48 bytes) ─────>│
│                               │  echo back unchanged
│<─── recv(reply, 48 bytes) ────│
│  rtt = now_ns() - reply.send_ts

- Timestamp is embedded **inside** the message — no shared memory needed
- `CLOCK_MONOTONIC` is used — never jumps, unaffected by NTP adjustments
- Warmup phase discards the first N messages (TCP slow-start + cold cache)
- Results reported as P50, P99, P99.9, P99.99 — never just average

---

## Server Options
--port N        Port to listen on (default: 9999)
--nodelay       Enable TCP_NODELAY (disable Nagle's algorithm)
--quickack      Enable TCP_QUICKACK (re-applied after each recv)
--affinity N    Pin server thread to CPU core N
--bufsize N     Socket buffer size in bytes (default: 4MB)
--verbose       Log stats every 10,000 messages

## Client Options
--host H        Server IP (default: 127.0.0.1)
--port N        Server port (default: 9999)
--count N       Messages per scenario (default: 100000)
--warmup N      Warmup messages to discard (default: 5000)
--affinity N    Pin client thread to CPU core N
--mode N        Run only scenario N (0–3), default runs all

---

## What This Maps To in Production HFT

| This Project | Production HFT Equivalent |
|---|---|
| Echo server | Order gateway — receives and ACKs orders from the exchange |
| 48-byte `Message` | FIX / OUCH order message (~40–80 bytes) |
| `send_all()` loop | Order encoder with reliable TCP send |
| `recv_all()` loop | ACK parser waiting for exchange response |
| Warmup phase | Pre-market connection warmup before 09:30 open |
| P99.9 reporting | Worst-case order round-trip latency measurement |
| `TCP_NODELAY` | Set on every order-entry socket in every HFT firm |
| CPU affinity | Order gateway thread pinned to isolated core |

---
