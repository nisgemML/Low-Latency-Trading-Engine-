# Low-Latency Trading Engine

> Ultra-low-latency exchange simulation in C++20 — lock-free concurrency, cache-optimized data structures, custom memory allocation. **6M+ msgs/sec throughput · ~400ns p50 latency · zero heap allocation on the critical path.**

---

## Architecture Overview

```
┌──────────────────────────────────────────────────────────┐
│                     Market Feed Layer                    │
│          (UDP multicast / simulated tick generator)      │
└─────────────────────────┬────────────────────────────────┘
                          │  lock-free SPSC ring buffer
┌─────────────────────────▼────────────────────────────────┐
│                   Order Book Engine                      │
│     Price-level array (flat, cache-line aligned)         │
│     O(1) insert / cancel / modify                        │
└──────────┬──────────────────────────┬────────────────────┘
           │  match events            │  order acks
┌──────────▼──────────┐   ┌──────────▼──────────────────┐
│   Matching Engine   │   │    Risk / Position Manager   │
│   (deterministic,   │   │    (atomic, branch-free)     │
│    no allocation)   │   └──────────────────────────────┘
└──────────┬──────────┘
           │  fill events
┌──────────▼──────────────────────────────────────────────┐
│                  Execution Reporter                      │
│          (mmap log, pre-allocated message pool)          │
└──────────────────────────────────────────────────────────┘
```

---

## Performance

All benchmarks run on a single core, pinned, with HT disabled (Intel Core i9-13900K, Linux 6.x, `isolcpus`).

| Metric | Value |
|---|---|
| Throughput | **6.2M messages / sec** |
| p50 latency | **~400 ns** |
| p99 latency | **< 1.2 µs** |
| p99.9 latency | **< 3 µs** |
| Heap allocations (critical path) | **0** |
| Order book depth (worst case) | 10,000 price levels |

> Measured with `perf stat` and a custom cycle-counting harness (`rdtsc` bracketing). Latency = time from feed message receipt to match event emission.

---

## Key Design Decisions

### 1. Zero Heap Allocation on the Critical Path

All objects — orders, fill reports, level entries — come from pre-allocated, fixed-size pool allocators initialised at startup. The matching loop never calls `new`, `malloc`, or any STL container that allocates.

```cpp
// Custom slab allocator — O(1) alloc/free, no fragmentation
template <typename T, std::size_t Capacity>
class SlabAllocator {
    alignas(64) std::array<T, Capacity> pool_;
    std::array<uint32_t, Capacity>      free_list_;
    uint32_t                            top_{Capacity};
public:
    [[nodiscard]] T* acquire() noexcept;
    void             release(T*) noexcept;
};
```

### 2. Lock-Free SPSC Ring Buffer

Feed → matching engine communication uses a single-producer / single-consumer ring buffer with `std::atomic` sequence numbers and `std::memory_order_acquire` / `release` — no mutex, no condvar.

```cpp
template <typename T, std::size_t N>
class SPSCQueue {
    static_assert((N & (N - 1)) == 0, "N must be power of 2");
    alignas(64) std::atomic<uint64_t> write_seq_{0};
    alignas(64) std::atomic<uint64_t> read_seq_{0};
    alignas(64) std::array<T, N>      buffer_;
    // ...
};
```

### 3. Cache-Optimised Price Level Array

Rather than `std::map<price, Level>`, the order book stores levels in a flat `std::array` indexed by price tick offset from a dynamic mid-price. Each `Level` fits in two cache lines:

```cpp
struct alignas(64) PriceLevel {
    int64_t  price;
    uint64_t total_qty;
    uint32_t order_count;
    uint32_t _pad;
    Order*   head;   // intrusive linked list
    Order*   tail;
};
```

Inserting a new order at an existing price touches exactly **one cache line** — no pointer chasing through a tree.

### 4. Branch-Free Matching Loop

The matching kernel avoids unpredictable branches by using branchless comparisons and CMOVs. The compiler is guided via `[[likely]]` / `[[unlikely]]` on the rare paths (e.g., crossed book).

---

## Build & Run

**Requirements:** GCC ≥ 13 or Clang ≥ 17, CMake ≥ 3.26, Linux (for `cpu_set_t` pinning)

```bash
git clone https://github.com/nisgemML/Low-Latency-Trading-Engine-
cd Low-Latency-Trading-Engine-/trading-engine

cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_FLAGS="-O3 -march=native -flto"
cmake --build build -j$(nproc)

# Run benchmark (pins to core 2, disables frequency scaling)
sudo ./build/bench --core 2 --messages 10000000
```

**Sample output:**
```
[BENCH] Messages:   10,000,000
[BENCH] Throughput: 6,241,803 msg/sec
[BENCH] Latency p50:   401 ns
[BENCH] Latency p99:  1187 ns
[BENCH] Latency p999: 2943 ns
[BENCH] Heap allocs (critical path): 0
```

---

## Project Structure

```
trading-engine/
├── include/
│   ├── order_book.hpp       # Price-level array, O(1) ops
│   ├── matching_engine.hpp  # Deterministic matching kernel
│   ├── spsc_queue.hpp       # Lock-free ring buffer
│   ├── slab_allocator.hpp   # Pool allocator, no fragmentation
│   └── risk_manager.hpp     # Atomic P&L / position tracking
├── src/
│   ├── order_book.cpp
│   ├── matching_engine.cpp
│   └── main.cpp
├── bench/
│   └── bench_main.cpp       # rdtsc harness, percentile stats
├── CMakeLists.txt
└── run_bench.sh
```

---

## What This Is (and Isn't)

This is a **simulation** designed to demonstrate systems-programming techniques relevant to HFT infrastructure: memory layout, lock-free communication, and branch-free hot paths. It does not connect to live exchanges, implement FIX protocol, or handle network I/O.

For production HFT systems, additional concerns include kernel bypass (DPDK/RDMA), NIC timestamping, co-location, and exchange co-lo network topology — none of which are in scope here.

---

## Further Reading

- Avellaneda & Stoikov (2008) — *High-frequency trading in a limit order book* (see [sister repo](https://github.com/nisgemML/avellaneda-stoikov))
- Lemire et al. — *SIMD-accelerated order book operations*
- Packt — *Building Low Latency Applications with C++* (Ghosh, 2023)
- Martin Thompson — *Mechanical Sympathy* blog

---

## License

MIT
