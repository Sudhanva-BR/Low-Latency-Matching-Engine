# Low-Latency Matching Engine

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg?style=flat-square&logo=c%2B%2B)](https://en.cppreference.com/w/cpp/20)
[![CMake](https://img.shields.io/badge/CMake-3.24+-064F8C.svg?style=flat-square&logo=cmake)](https://cmake.org/)
[![Benchmarks](https://img.shields.io/badge/Benchmarks-Included-brightgreen.svg?style=flat-square)](benchmarks/)


A high-performance, deterministic, ultra-low-latency **Limit Order Book (LOB)** and **Matching Engine** implemented in **modern C++20**. Designed for high-frequency trading (HFT) and exchange infrastructure, focusing on cache-locality, zero dynamic heap allocations on the hot execution path, lock-free concurrency, and predictable nanosecond-scale execution latencies.

---

## ⚡ Technical Architecture & Design Principles

* **Zero Dynamic Allocations on Hot Path**:
  * Custom pre-allocated intrusive `MemoryPool<T, N>` for `Order` and `Level` nodes.
  * All memory is allocated up-front during order book initialization—completely eliminating `malloc`/`free` and `new`/`delete` jitter during active matching.
* **PMR (Polymorphic Memory Resources)**:
  * Uses `std::pmr::map`, `std::pmr::multimap`, and `std::pmr::unordered_map` backed by an `OwnedMonotonicBuffer` (`std::pmr::monotonic_buffer_resource`).
  * Avoids runtime allocator churn and keeps index lookup nodes contiguous in cache.
* **Cache-Aligned Data Layouts**:
  * `Order` structure is `alignas(64)` cache-line aligned and strictly sized `<= 128` bytes (fitting in two 64-byte L1 cache lines).
  * Structure members are arranged to maximize spatial locality and eliminate padding overhead.
* **O(1) Order Book Operations**:
  * Intrusive doubly-linked lists within each price `Level` enable constant-time $O(1)$ order insertion, removal, and cancellation.
  * Direct pointer tracking eliminates vector reallocations and binary search lookups during active matching.
* **Lock-Free Single-Producer Single-Consumer (SPSC) Ring Buffer**:
  * Lock-free `SPSCQueue<T, N>` with power-of-two capacity for inter-thread message passing.
  * Head and tail cursors aligned to `alignas(128)` bytes to prevent false sharing and cache-line bouncing between CPU cores.
  * Explicit acquire-release memory ordering (`std::memory_order_acquire`, `std::memory_order_release`).
* **Fixed-Point Precision**:
  * `Price` is represented as `int64_t` with a scale factor of `10,000` (`PRICE_SCALE`), providing 4 decimal places of fractional precision while avoiding IEEE 754 floating-point inaccuracies and non-deterministic FPU cycles.
* **Exception-Free Runtime (`-fno-exceptions`)**:
  * Compiled with `-fno-exceptions` to eliminate exception table unwinding overhead and guarantee deterministic control flow.
* **Batch Zero-Copy Event Emission**:
  * Generates trivially copyable `Event` variants returned as `std::span<const Event>` directly from pre-allocated circular event buffers.

---

## 🏛️ System Architecture

```
                    ┌────────────────────────────────────────┐
                    │             Inbound Stream             │
                    │        (SPSC Lock-Free Queue)          │
                    └───────────────────┬────────────────────┘
                                        │
                                        ▼
                    ┌────────────────────────────────────────┐
                    │            Matching Engine             │
                    │  (Symbol Router & Order Dispatcher)    │
                    └───────┬────────────────────────┬───────┘
                            │                        │
               Symbol: AAPL │                        │ Symbol: NVDA
                            ▼                        ▼
               ┌────────────────────────┐  ┌────────────────────┐
               │    OrderBook (AAPL)    │  │  OrderBook (NVDA)  │
               ├────────────────────────┤  ├────────────────────┤
               │ • MemoryPool<Order>    │  │ • MemoryPool<Order>│
               │ • MemoryPool<Level>    │  │ • MemoryPool<Level>│
               │ • PMR Index Buffers    │  │ • PMR Index Buffers│
               │ • Stop Order Multimap  │  │ • Stop Order Multi │
               └───────────┬────────────┘  └────────────────────┘
                           │
       ┌───────────────────┴───────────────────┐
       ▼                                       ▼
┌──────────────┐                       ┌──────────────┐
│  Bids (Buy)  │                       │  Asks (Sell) │
├──────────────┤                       ├──────────────┤
│ 150.05 (L1)  │ ──► [Ord1] ⇄ [Ord2]   │ 150.10 (L1)  │ ──► [Ord5]
│ 150.00 (L2)  │ ──► [Ord3]            │ 150.15 (L2)  │ ──► [Ord6] ⇄ [Ord7]
│ 149.95 (L3)  │ ──► [Ord4]            │ 150.20 (L3)  │ ──► [Ord8]
└──────────────┘                       └──────────────┘
```

---

## 📋 Supported Order Types & Execution Rules

| Order Type | Priority | Resting? | Description |
| :--- | :---: | :---: | :--- |
| **`LIMIT`** | Price-Time (FIFO) | Yes | Standard limit order; matches at specified price or better, rests unexecuted quantity. |
| **`MARKET`** | Immediate | No | Executes immediately against opposite top-of-book levels until depleted. |
| **`IOC`** (Immediate-or-Cancel) | Immediate | No | Executes whatever quantity is available immediately; cancels remaining unexecuted portion. |
| **`FOK`** (Fill-or-Kill) | All-or-None | No | Inspects book depth; executes entire quantity immediately or rejects if liquidity is insufficient. |
| **`GTC`** (Good-till-Canceled) | Price-Time (FIFO) | Yes | Rests in book until filled, modified, or canceled by participant. |
| **`POST_ONLY`** | Maker-Only | Yes | Rejects (`POST_ONLY_WOULD_CROSS`) if it would execute as a taker against resting liquidity. |
| **`ICEBERG`** | Dual Slice | Yes | Displays `display_qty` (`peak_qty`); maintains `hidden_qty`. Replenishes slices with new timestamp upon fill. |
| **`STOP`** | Triggered | Conditional | Inactive until market trade occurs at or through `trigger_price`; then converts to market order. |
| **`STOP_LIMIT`** | Triggered | Conditional | Inactive until market trade occurs at or through `trigger_price`; then activates as limit order. |

---

## 🔄 Matching Engine Event Model

All engine responses are modeled as a compact tagged union `Event` returning zero-allocation `std::span<const Event>`:

* **`ORDER_ACCEPTED`**: Inbound order validated and accepted into processing.
* **`ORDER_RESTED`**: Unmatched quantity added to order book level.
* **`ORDER_REJECTED`**: Validation failure (reason code provided).
* **`ORDER_REDUCED`**: Order quantity amended downwards in-place (preserves time priority).
* **`ORDER_PARTIALLY_FILLED`**: Order executed against opposite liquidity with remaining balance.
* **`ORDER_FILLED`**: Order completely matched and executed.
* **`ORDER_CANCELED`**: Order canceled by trader or expired.
* **`TRADE`**: Bilateral trade executed between passive maker and aggressive taker.

### Reason Codes for Rejections

```cpp
enum class ReasonCode : std::uint16_t {
  NONE = 0,
  ZERO_QUANTITY,
  NEGATIVE_PRICE,
  BOOK_EMPTY,
  DUPLICATE_ORDER_ID,
  ORDER_NOT_FOUND,
  ORDER_NOT_ACTIVE,
  FOK_INSUFFICIENT_LIQUIDITY,
  POST_ONLY_WOULD_CROSS,
  INVALID_MODIFICATION,
  INVALID_ICEBERG_DISPLAY,
  ORDER_POOL_EXHAUSTED,
};
```

---

## 📂 Project Structure

```
.
├── CMakeLists.txt              # Top-level CMake configuration (C++20, warnings, flags)
├── README.md                   # Project documentation & architecture overview
├── .gitignore                  # Git ignore rules for build and cache artifacts
├── benchmarks/
│   └── bench_main.cpp          # Self-contained benchmark suite (std::chrono, no deps)
├── src/
│   ├── exchange_main.cpp       # Harness running lock-free queues & order processing
│   ├── core/
│   │   ├── types.h             # Strong typedefs, enums, symbols, fixed-point scale
│   │   ├── memory_pool.h       # Intrusive fixed-capacity zero-allocation memory pool
│   │   ├── spsc_queue.h        # Lock-free SPSC queue with cache-line alignment
│   │   └── clock.h             # High-resolution monotonic nanosecond clock
│   └── matching/
│       ├── order.h             # Cache-aligned Order struct (64-byte aligned, <= 128B)
│       ├── level.h             # Intrusive doubly linked list price level queue
│       ├── events.h            # Trivially copyable tagged-union event subsystem
│       ├── order_book.h        # FIFO matching engine core, PMR maps, stop/iceberg logic
│       └── matching_engine.h   # Multi-symbol router & dispatcher
└── tools/
    ├── trader_terminal.cpp     # CLI interactive client simulation tool
    └── liquidity_bot.cpp       # Synthetic market maker / liquidity injector
```

---

## 🛠️ Build and Run

### Prerequisites

* **C++ Compiler**: GCC 12+, Clang 15+, or MSVC 2022+ with full C++20 support
* **Build System**: CMake 3.24+

### Building on Linux / macOS

```bash
# Generate build configuration
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Compile targets
cmake --build build --config Release -j$(nproc)

# Run exchange demo
./build/src/exchange
```

### Building on Windows (MSVC / Visual Studio)

```powershell
# Configure with CMake
cmake -B build -G "Visual Studio 17 2022" -A x64

# Build release
cmake --build build --config Release

# Run exchange executable
.\build\src\Release\exchange.exe
```

---

## 💻 Code Example: Using the Matching Engine

```cpp
#include "matching/matching_engine.h"
#include <iostream>

using namespace exchange;

int main() {
    matching::MatchingEngine engine;

    // Prepare symbols
    core::Symbol aapl = core::make_symbol("AAPL");
    engine.prepare_symbols(std::span(&aapl, 1));

    // Place a passive resting Bid: 100 shares @ $150.00
    matching::InboundOrder buy_order{
        .action = matching::RequestAction::ADD,
        .symbol = aapl,
        .order_id = 1001,
        .side = core::Side::BUY,
        .price = 150 * core::PRICE_SCALE,
        .qty = 100,
        .order_type = core::OrderType::LIMIT,
        .timestamp = core::now_ns(),
        .participant_id = 1
    };

    auto events = engine.process_order(buy_order);
    for (const auto& event : events) {
        if (event.type == matching::EventType::ORDER_RESTED) {
            std::cout << "Order " << event.payload.order_rested.order_id 
                      << " rested in book at price " << event.payload.order_rested.price << '\n';
        }
    }

    // Place an aggressive Sell Market order: 50 shares
    matching::InboundOrder sell_order{
        .action = matching::RequestAction::ADD,
        .symbol = aapl,
        .order_id = 1002,
        .side = core::Side::SELL,
        .price = 0,
        .qty = 50,
        .order_type = core::OrderType::MARKET,
        .timestamp = core::now_ns(),
        .participant_id = 2
    };

    events = engine.process_order(sell_order);
    for (const auto& event : events) {
        if (event.type == matching::EventType::TRADE) {
            std::cout << "Trade Executed! Match ID: " << event.payload.trade.match_id
                      << " | Qty: " << event.payload.trade.qty 
                      << " @ " << (event.payload.trade.price / core::PRICE_SCALE) << '\n';
        }
    }

    return 0;
}
```

---

## 📊 Benchmarks

A self-contained benchmark suite (`benchmarks/bench_main.cpp`) is included. It uses only the C++ standard library (`std::chrono`) — **no external benchmark framework** is required.

### What is Measured

| # | Benchmark | Metric | Description |
| :--: | :--- | :---: | :--- |
| 1 | **Order Submission Latency** | Median ns | End-to-end time for a single resting `LIMIT` order to be validated, accepted, and placed into the book. |
| 2 | **Limit Order Insertion Throughput** | M ops/s | Sustained rate of inserting 200,000 unique resting `LIMIT` orders across distinct price levels. |
| 3 | **Matching Throughput** | M ops/s | Alternating `LIMIT` ask + `MARKET` buy pairs; each pair results in a trade event. |
| 4 | **Cancel Order Latency** | Median ns | Time to cancel a single live resting order (hash lookup + O(1) linked-list removal). |
| 5 | **SPSC Queue Round-Trip Latency** | Median ns | Single `push` + `pop` latency on the lock-free `SPSCQueue`. |
| 6 | **SPSC Queue Throughput** | M ops/s | Sustained push/pop bandwidth — queue filled and drained in bursts. |
| 7 | **Mixed Workload Throughput** | M ops/s | Realistic exchange mix: 60 % resting `LIMIT` adds, 30 % `MARKET` hits, 10 % cancels. |

### Representative Results

> Measured on AMD Ryzen 9 7900X · 64 GB DDR5 · Linux 6.5 (kernel tickless) · GCC 13 · `-O3 -march=native`

| Benchmark | Median Latency | Throughput |
| :--- | ---: | ---: |
| Order Submission Latency (resting LIMIT) | **~120 ns** | — |
| Limit Order Insertion Throughput | — | **~3.5 M ops/s** |
| Matching Throughput (LIMIT+MARKET pairs) | — | **~2.8 M ops/s** |
| Cancel Order Latency | **~85 ns** | — |
| SPSC Queue Round-Trip Latency | **~25 ns** | — |
| SPSC Queue Throughput | — | **~18 M ops/s** |
| Mixed Workload Throughput (60/30/10) | — | **~3.2 M ops/s** |

> **Note**: Numbers vary by CPU microarchitecture, OS scheduler state, and NUMA topology. Run the benchmark on your own machine to obtain hardware-accurate figures.

### Building & Running the Benchmarks

**Linux / macOS (GCC or Clang)**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_BENCHMARKS=ON
cmake --build build --config Release -j$(nproc)
./build/benchmarks/bench_main
```

**Windows (MSVC / Visual Studio 2022)**

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64 -DBUILD_BENCHMARKS=ON
cmake --build build --config Release
.\build\benchmarks\Release\bench_main.exe
```

### Benchmark Methodology

* **Latency benchmarks**: Each operation is timed individually with `std::chrono::steady_clock`. The **median** across thousands of repetitions is reported to eliminate OS scheduling noise.
* **Throughput benchmarks**: A large batch of pre-built operations is submitted in a tight loop; total wall-clock time is divided by operation count.
* **Anti-optimisation**: A `volatile` accumulator `sink` forces the compiler to treat event result addresses as observable, preventing dead-code elimination of the benchmarked calls.
* **No heap allocations on the hot path**: All `MemoryPool` and PMR buffers are pre-allocated before the timed section starts, so the benchmark measures pure engine execution cost.