#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────────────────────
# setup.sh — OS-level network tuning for low-latency TCP
#
# Run this ONCE before benchmarking (needs sudo).
# These settings mimic what HFT firms apply to trading servers.
#
# What each setting does is explained inline.
# ──────────────────────────────────────────────────────────────────────────────

set -euo pipefail

echo ""
echo "  ══════════════════════════════════════"
echo "   TCP Latency Lab — System Setup"
echo "  ══════════════════════════════════════"
echo ""

if [[ $EUID -ne 0 ]]; then
    echo "  [WARN] Not running as root. Some settings may fail."
    echo "  Run: sudo bash scripts/setup.sh"
    echo ""
fi

apply() {
    local key="$1"
    local val="$2"
    local desc="$3"
    if sysctl -w "$key=$val" > /dev/null 2>&1; then
        printf "  ✓ %-45s = %s\n" "$key" "$val"
    else
        printf "  ✗ %-45s   [FAILED - need root]\n" "$key"
    fi
}

echo "  ── Socket Buffer Sizes ──────────────────────"
# Default socket buffers are 128KB. We allow up to 128MB.
# A feed handler receiving burst multicast needs large buffers or it drops packets.
apply net.core.rmem_default    131072        "default recv buffer"
apply net.core.wmem_default    131072        "default send buffer"
apply net.core.rmem_max        134217728     "max recv buffer (128MB)"
apply net.core.wmem_max        134217728     "max send buffer (128MB)"
apply net.ipv4.tcp_rmem        "4096 87380 134217728"  "tcp recv [min/default/max]"
apply net.ipv4.tcp_wmem        "4096 65536 134217728"  "tcp send [min/default/max]"

echo ""
echo "  ── TCP Behavior ─────────────────────────────"
# Disable Nagle at the system level (TCP_NODELAY does it per-socket,
# but this ensures even legacy code without TCP_NODELAY gets it)
# NOTE: In HFT you set TCP_NODELAY per socket. Don't rely on the global.
apply net.ipv4.tcp_low_latency  1            "prefer low latency over throughput"

# Fast socket reuse after crash/restart
apply net.ipv4.tcp_tw_reuse     1            "reuse TIME_WAIT sockets"
apply net.ipv4.tcp_fin_timeout  15           "reduce FIN_WAIT2 timeout (default 60s)"

# Allow more ports for outbound connections
apply net.ipv4.ip_local_port_range "1024 65535" "increase available port range"

# TCP Fast Open: send data with the SYN (saves 1 RTT on reconnect)
# 3 = enable both client and server
apply net.ipv4.tcp_fastopen     3            "TCP Fast Open (client+server)"

# Use BBR congestion control (better on low-RTT, uncongested colo links)
# Falls back to CUBIC if BBR module not loaded
if sysctl -w net.ipv4.tcp_congestion_control=bbr > /dev/null 2>&1; then
    echo "  ✓ Congestion control = BBR"
else
    echo "  ~ Congestion control = CUBIC (BBR not available)"
fi

echo ""
echo "  ── Interrupt & CPU Tuning ───────────────────"
# Disable CPU frequency scaling — a CPU running at 800MHz during idle
# can take microseconds to ramp up to 3.5GHz when a packet arrives.
# In HFT this is unacceptable jitter.
if [[ -f /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]]; then
    for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        echo performance > "$cpu" 2>/dev/null || true
    done
    echo "  ✓ CPU governor set to 'performance'"
else
    echo "  ~ CPU governor: cpufreq not available in this environment"
fi

# Move NIC interrupts away from CPU 0 (our benchmark core).
# In production you'd move them to a non-trading core.
# Here we just show the mechanism.
echo ""
echo "  ── IRQ Affinity (informational) ─────────────"
echo "  To pin NIC interrupts away from your trading core:"
echo "    cat /proc/interrupts | grep <interface>"
echo "    echo 2 > /proc/irq/<N>/smp_affinity  # CPU 1 only (bitmask)"
echo ""

# Disable transparent huge pages (THP).
# THP sounds good but its background compaction causes random latency spikes.
# HFT uses explicit hugepages (2MB), not transparent ones.
if [[ -f /sys/kernel/mm/transparent_hugepage/enabled ]]; then
    echo never > /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || true
    echo "  ✓ Transparent hugepages disabled (use explicit hugepages)"
fi

echo ""
echo "  ── Verify Settings ──────────────────────────"
echo "  Current socket buffer max:"
sysctl net.core.rmem_max net.core.wmem_max 2>/dev/null || true
echo ""
echo "  ✓ Setup complete. Run: make && bash scripts/run_benchmark.sh"
echo ""
