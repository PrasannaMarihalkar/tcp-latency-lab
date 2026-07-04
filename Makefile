# ──────────────────────────────────────────────────────────────────────────────
# Makefile — tcp-latency-lab
# ──────────────────────────────────────────────────────────────────────────────

CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic
INCLUDES := -Isrc

# Release: maximum optimization for accurate latency numbers
# -O3          : aggressive optimization
# -march=native: use all CPU features available (AVX2, etc.)
# -mtune=native: tune for this specific CPU model
# -flto        : link-time optimization across translation units
RFLAGS   := -O3 -march=native -mtune=native -flto

# Debug: symbols and sanitizers for development
DFLAGS   := -O0 -g3 -fsanitize=address,undefined

LDFLAGS  := -lpthread

SRCDIR   := src
BINDIR   := .

.PHONY: all release debug clean help

all: release

# ── Release build (use this for benchmarks) ───────────────────────────────────
release: server client benchmark

server: $(SRCDIR)/server.cpp $(SRCDIR)/common.hpp
	@echo "  [CXX] server (release)"
	$(CXX) $(CXXFLAGS) $(RFLAGS) $(INCLUDES) $< -o $(BINDIR)/server $(LDFLAGS)

client: $(SRCDIR)/client.cpp $(SRCDIR)/common.hpp $(SRCDIR)/stats.hpp
	@echo "  [CXX] client (release)"
	$(CXX) $(CXXFLAGS) $(RFLAGS) $(INCLUDES) $< -o $(BINDIR)/client $(LDFLAGS)

benchmark: $(SRCDIR)/benchmark.cpp $(SRCDIR)/common.hpp $(SRCDIR)/stats.hpp
	@echo "  [CXX] benchmark (release)"
	$(CXX) $(CXXFLAGS) $(RFLAGS) $(INCLUDES) $< -o $(BINDIR)/benchmark $(LDFLAGS)

# ── Debug build ───────────────────────────────────────────────────────────────
debug: server_dbg client_dbg

server_dbg: $(SRCDIR)/server.cpp $(SRCDIR)/common.hpp
	@echo "  [CXX] server (debug)"
	$(CXX) $(CXXFLAGS) $(DFLAGS) $(INCLUDES) $< -o $(BINDIR)/server_dbg $(LDFLAGS)

client_dbg: $(SRCDIR)/client.cpp $(SRCDIR)/common.hpp $(SRCDIR)/stats.hpp
	@echo "  [CXX] client (debug)"
	$(CXX) $(CXXFLAGS) $(DFLAGS) $(INCLUDES) $< -o $(BINDIR)/client_dbg $(LDFLAGS)

clean:
	rm -f server client server_dbg client_dbg
	rm -f results/benchmark.csv

help:
	@echo ""
	@echo "  Usage:"
	@echo "    make              → release build"
	@echo "    make debug        → debug build with ASan/UBSan"
	@echo "    make clean        → remove binaries"
	@echo ""
	@echo "  Quick start:"
	@echo "    Terminal 1:  ./server --nodelay --quickack"
	@echo "    Terminal 2:  ./client --count 100000"
	@echo ""
	@echo "  Full benchmark:"
	@echo "    bash scripts/run_benchmark.sh"
	@echo ""
