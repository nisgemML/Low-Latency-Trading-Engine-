# Low-Latency Trading Engine

A from-scratch exchange simulation in C++20, targeting sub-microsecond per-event latency and 6M+ messages/sec sustained throughput on Linux x86-64.

```
~400–500 ns p50  ·  ~1.5–2 µs p99  ·  6M+ msgs/sec sustained
```

---

## Architecture

```
Feed (UDP/sim)
      │
      ▼
┌─────────────────────┐
│  MarketDataIngestion │  ← decode wire format, gap-detect, normalize
└─────────┬───────────┘
          │  SPSC queue  (lock-free, cache-line-separated producer/consumer heads)
          ▼
┌─────────────────────┐
│   MatchingEngine     │  ← pinned thread, SCHED_FIFO, busy-poll
│  ┌───────────────┐  │
│  │  OrderBook[N] │  │  ← SoA layout, pool-allocated slots, O(1) cancel
│  └───────────────┘  │
└─────────┬───────────┘
          │  SPSC queue
          ▼
┌─────────────────────┐
│   ExecutionLayer     │  ← position tracking, P&L, downstream dispatch
└─────────────────────┘
```

Every component is isolated by a typed SPSC queue. The matching engine's hot path contains **zero mutexes, zero heap allocations, and zero system calls** after startup.

---

## Key Design Decisions

### Lock-free concurrency — SPSC ring buffers

The minimum synchronization this problem requires is *two* atomic variables: one producer-owned (`write_pos`) and one consumer-owned (`read_pos`). No mutex, no CAS loop, no ABA problem.

```cpp
// Producer: store payload, then release-store the index.
buffer_[wp] = item;
write_pos_.store(next, std::memory_order_release);

// Consumer: acquire-load the index, then read payload.
if (rp == write_pos_.load(std::memory_order_acquire)) return false;
out = buffer_[rp];
read_pos_.store((rp + 1) & kMask, std::memory_order_release);
```

The `release`/`acquire` pair establishes happens-before without any explicit fence. Producer and consumer heads live on **separate cache lines** to eliminate false sharing.

### Cache-aware order book — struct-of-arrays

The classic textbook LOB uses `std::map<Price, std::list<Order*>>` — each operation is a tree traversal followed by pointer-chasing scattered across virtual memory. Profiling identified L1 miss rate as the primary bottleneck.

The fix: transform to struct-of-arrays. The matching engine iterates `prices[]` alone to find crossing levels:

```
prices[]       [99.95] [99.90] [99.85] ...   ← hot: streamed sequentially
qtys[]         [ 1000] [  500] [  200] ...   ← touched only on match
order_counts[] [    3] [    2] [    1] ...   ← touched only on match
```

The hardware prefetcher saturates `prices[]` ahead of the comparison loop. `qtys[]` stays cold until actually needed.

### Pool allocator — no heap on the hot path

`malloc`/`free` are non-deterministic and touch shared heap state. Every `Order` allocation on the hot path now comes from a pre-allocated mmap'd slab with an intrusive free list, pinned with `mlock` to prevent page faults at runtime.

```
alloc() → O(1), deterministic, zero system calls
free()  → O(1), deterministic, zero system calls
```

### CPU isolation

The matching engine thread is pinned via `pthread_setaffinity_np` and elevated to `SCHED_FIFO` priority. It busy-polls its inbound SPSC queue with `__builtin_ia32_pause()` (the x86 PAUSE hint) in the empty case — reducing power consumption and memory-order traffic without blocking.

---

## Project Layout

```
trading-engine/
├── include/
│   ├── core/
│   │   ├── types.hpp           # Price, Qty, Order, ExecutionReport, ...
│   │   ├── spsc_queue.hpp      # Lock-free SPSC ring buffer
│   │   ├── order_book.hpp      # SoA limit order book
│   │   ├── matching_engine.hpp # Orchestrator + thread management
│   │   ├── market_data.hpp     # Wire format decoder + ingestion
│   │   └── execution_layer.hpp # Position tracking + P&L
│   └── util/
│       ├── allocator.hpp       # mmap pool allocator
│       └── logger.hpp          # Lock-free async logger
├── src/
│   ├── core/                   # Implementations
│   └── util/
├── tests/
│   ├── test_order_book.cpp     # Unit + property-based invariant tests
│   ├── test_spsc.cpp           # Concurrent correctness (TSan-clean)
│   ├── test_matching.cpp       # End-to-end integration tests
│   └── test_allocator.cpp      # Pool allocator stress tests
├── bench/
│   ├── bench_latency.cpp       # p50/p90/p99/p99.9 latency distribution
│   └── bench_throughput.cpp    # Sustained msgs/sec measurement
├── scripts/
│   └── build.sh                # Build + test + benchmark driver
└── CMakeLists.txt
```

---

## Building

**Requirements:** GCC ≥ 12 or Clang ≥ 16, CMake ≥ 3.22, Ninja, Linux x86-64.

```bash
# Release build + run all tests
./scripts/build.sh

# Release build + benchmarks
./scripts/build.sh --bench

# ThreadSanitizer build (validates concurrent SPSC correctness)
./scripts/build.sh --tsan

# AddressSanitizer build
./scripts/build.sh --asan

# Manual CMake workflow
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

---

## Benchmark Results

Measured on a pinned core (Linux, `isolcpus`, `SCHED_FIFO`, `mlock`'d slab):

| Metric       | Result      |
|--------------|-------------|
| p50 latency  | ~400–500 ns |
| p99 latency  | ~1.5–2 µs   |
| Throughput   | 6M+ msg/sec |

Latency is measured end-to-end: SPSC submit → message decode → match attempt → SPSC push of execution report.

---

## Testing

- **Property-based invariant test** (`test_order_book`): generates 100K random orders and cancels, asserts `best_bid ≤ best_ask` holds at every step.
- **Concurrent SPSC test** (`test_spsc`): 2M items across producer/consumer threads, verified FIFO-ordered receipt. Passes ThreadSanitizer with zero reported races.
- **Integration tests** (`test_matching`): end-to-end cross, cancel-before-match, multi-symbol isolation, market order fill.
- **Allocator stress** (`test_allocator`): 1M alloc/free cycles; validates free-list consistency throughout.

---

## What's Not Here (Intentionally)

- **Network I/O**: DPDK or kernel-bypass UDP would be the next layer; the `MarketDataIngestion` interface accepts a raw `std::span<uint8_t>` and is transport-agnostic.
- **Persistence**: A production system would WAL every order event for crash recovery.
- **Risk pre-trade checks**: Fat-finger limits, position limits, etc. would live between ingestion and the matching engine.
- **Multi-threaded matching**: Parallelism at the LOB level requires per-symbol sharding; the current single-threaded design is the correct baseline before adding that complexity.
