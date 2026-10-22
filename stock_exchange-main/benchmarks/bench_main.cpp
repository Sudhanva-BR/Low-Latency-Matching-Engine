// benchmarks/bench_main.cpp
//
// Self-contained benchmark suite for the Low-Latency Matching Engine.
// Uses only the C++ standard library (std::chrono) – no external dependencies.
//
// What is measured
// ─────────────────
//   1. Order submission latency    – single LIMIT order add, nanoseconds/op
//   2. Limit order throughput      – sustained resting-order insertion rate (M ops/s)
//   3. Matching throughput         – alternating buy/sell market order pairs (M trades/s)
//   4. Cancel latency              – single cancel round-trip, nanoseconds/op
//   5. SPSC queue round-trip       – push + pop latency, nanoseconds/op
//   6. SPSC queue throughput       – sustained push/pop bandwidth (M ops/s)
//   7. Mixed workload throughput   – add / match / cancel mix (M ops/s)
//
// Build (Release, Linux/macOS):
//   cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_BENCHMARKS=ON
//   cmake --build build --config Release -j$(nproc)
//   ./build/benchmarks/bench_main
//
// Build (Release, Windows):
//   cmake -B build -G "Visual Studio 17 2022" -A x64 -DBUILD_BENCHMARKS=ON
//   cmake --build build --config Release
//   .\build\benchmarks\Release\bench_main.exe

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include "core/spsc_queue.h"
#include "core/types.h"
#include "matching/matching_engine.h"

// ─── helpers ──────────────────────────────────────────────────────────────────

using Clock     = std::chrono::steady_clock;
using Nanos     = std::chrono::nanoseconds;
using TimePoint = Clock::time_point;

static inline TimePoint now() noexcept { return Clock::now(); }

static inline std::int64_t elapsed_ns(TimePoint t0, TimePoint t1) noexcept {
    return std::chrono::duration_cast<Nanos>(t1 - t0).count();
}

// Prevent the compiler from optimising away result values.
static volatile std::uint64_t sink = 0;
template <typename T>
static inline void do_not_optimise(T const& val) noexcept {
    sink += static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&val));
}

// ─── pretty printer ───────────────────────────────────────────────────────────

struct Result {
    std::string name;
    std::int64_t median_ns{0};   // for latency benchmarks
    double throughput_mops{0.0}; // for throughput benchmarks
    bool is_latency{true};
};

static void print_header() {
    std::cout << "\n";
    std::cout << "==========================================================================\n";
    std::cout << "          Low-Latency Matching Engine -- Benchmark Results                \n";
    std::cout << "==========================================================================\n";
    std::cout << "\n";
    std::cout << std::left
              << std::setw(42) << "Benchmark"
              << std::setw(20) << "Median Latency"
              << std::setw(20) << "Throughput"
              << "\n";
    std::cout << std::string(82, '-') << "\n";
}

static void print_result(const Result& r) {
    std::cout << std::left << std::setw(42) << r.name;

    if (r.is_latency) {
        std::string lat = std::to_string(r.median_ns) + " ns";
        std::cout << std::setw(20) << lat;
        std::cout << std::setw(20) << "--";
    } else {
        std::cout << std::setw(20) << "--";
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << r.throughput_mops << " M ops/s";
        std::cout << std::setw(20) << oss.str();
    }
    std::cout << "\n";
}

// ─── fixture helpers ──────────────────────────────────────────────────────────

static exchange::core::Symbol make_sym(const char* s) {
    return exchange::core::make_symbol(s);
}

using Engine = exchange::matching::MatchingEngine<>;
using Order  = exchange::matching::InboundOrder;
using Action = exchange::matching::RequestAction;
namespace T  = exchange::core;

static Order make_limit(std::uint64_t id, T::Symbol sym,
                        T::Side side, T::Price px, T::Quantity qty) {
    Order o{};
    o.action     = Action::ADD;
    o.symbol     = sym;
    o.order_id   = id;
    o.side       = side;
    o.price      = px;
    o.qty        = qty;
    o.order_type = T::OrderType::LIMIT;
    o.timestamp  = static_cast<T::Timestamp>(id);
    o.participant_id = 1;
    return o;
}

static Order make_cancel(std::uint64_t id, T::Symbol sym) {
    Order o{};
    o.action   = Action::CANCEL;
    o.symbol   = sym;
    o.order_id = id;
    o.timestamp = static_cast<T::Timestamp>(id + 1'000'000);
    return o;
}

// ─── 1. Order submission latency ──────────────────────────────────────────────

static Result bench_order_submission_latency() {
    constexpr int RUNS = 10'000;

    Engine engine;
    auto sym = make_sym("AAPL");
    engine.prepare_symbols(std::span(&sym, 1));

    std::vector<std::int64_t> samples;
    samples.reserve(RUNS);

    for (int i = 0; i < RUNS; ++i) {
        Order o = make_limit(
            static_cast<std::uint64_t>(i + 1),
            sym,
            T::Side::BUY,
            100'0000LL * T::PRICE_SCALE,
            100);

        auto t0 = now();
        auto events = engine.process_order(o);
        auto t1 = now();

        do_not_optimise(events);
        samples.push_back(elapsed_ns(t0, t1));
    }

    std::sort(samples.begin(), samples.end());
    return Result{
        "Order Submission Latency (resting LIMIT)",
        samples[samples.size() / 2],
        0.0,
        true
    };
}

// ─── 2. Limit order insertion throughput ──────────────────────────────────────

static Result bench_limit_order_throughput() {
    constexpr int N = 200'000;

    Engine engine;
    auto sym = make_sym("NVDA");
    engine.prepare_symbols(std::span(&sym, 1));

    std::vector<Order> orders;
    orders.reserve(N);
    for (int i = 0; i < N; ++i) {
        orders.push_back(make_limit(
            static_cast<std::uint64_t>(i + 1),
            sym,
            T::Side::BUY,
            (1000LL + i) * T::PRICE_SCALE,
            10));
    }

    auto t0 = now();
    for (auto& o : orders) {
        auto events = engine.process_order(o);
        do_not_optimise(events);
    }
    auto t1 = now();

    const double seconds = static_cast<double>(elapsed_ns(t0, t1)) * 1e-9;
    return Result{
        "Limit Order Insertion Throughput",
        0,
        static_cast<double>(N) / seconds / 1e6,
        false
    };
}

// ─── 3. Matching throughput ────────────────────────────────────────────────────

static Result bench_matching_throughput() {
    constexpr int N = 200'000;

    Engine engine;
    auto sym = make_sym("MSFT");
    engine.prepare_symbols(std::span(&sym, 1));

    std::vector<Order> asks, buys;
    asks.reserve(N);
    buys.reserve(N);

    for (int i = 0; i < N; ++i) {
        std::uint64_t ask_id = static_cast<std::uint64_t>(i) * 2 + 1;
        std::uint64_t buy_id = ask_id + 1;

        Order ask = make_limit(ask_id, sym, T::Side::SELL, 200LL * T::PRICE_SCALE, 1);
        asks.push_back(ask);

        Order buy{};
        buy.action     = Action::ADD;
        buy.symbol     = sym;
        buy.order_id   = buy_id;
        buy.side       = T::Side::BUY;
        buy.price      = 0;
        buy.qty        = 1;
        buy.order_type = T::OrderType::MARKET;
        buy.timestamp  = static_cast<T::Timestamp>(buy_id);
        buy.participant_id = 2;
        buys.push_back(buy);
    }

    auto t0 = now();
    for (int i = 0; i < N; ++i) {
        auto e1 = engine.process_order(asks[i]);
        do_not_optimise(e1);
        auto e2 = engine.process_order(buys[i]);
        do_not_optimise(e2);
    }
    auto t1 = now();

    const double seconds = static_cast<double>(elapsed_ns(t0, t1)) * 1e-9;
    return Result{
        "Matching Throughput (LIMIT+MARKET pairs)",
        0,
        static_cast<double>(N) / seconds / 1e6,
        false
    };
}

// ─── 4. Cancel latency ────────────────────────────────────────────────────────

static Result bench_cancel_latency() {
    constexpr int N = 10'000;

    Engine engine;
    auto sym = make_sym("GOOG");
    engine.prepare_symbols(std::span(&sym, 1));

    for (int i = 0; i < N; ++i) {
        auto o = make_limit(
            static_cast<std::uint64_t>(i + 1),
            sym, T::Side::BUY,
            (50000LL + i) * T::PRICE_SCALE, 10);
        engine.process_order(o);
    }

    std::vector<std::int64_t> samples;
    samples.reserve(N);

    for (int i = 0; i < N; ++i) {
        Order cancel = make_cancel(static_cast<std::uint64_t>(i + 1), sym);
        auto t0 = now();
        auto events = engine.process_order(cancel);
        auto t1 = now();
        do_not_optimise(events);
        samples.push_back(elapsed_ns(t0, t1));
    }

    std::sort(samples.begin(), samples.end());
    return Result{
        "Cancel Order Latency",
        samples[samples.size() / 2],
        0.0,
        true
    };
}

// ─── 5. SPSC queue round-trip latency ─────────────────────────────────────────

static Result bench_spsc_latency() {
    constexpr int RUNS = 100'000;
    constexpr std::size_t CAPACITY = 1024;

    using Q = exchange::core::SPSCQueue<exchange::matching::InboundOrder, CAPACITY>;
    Q queue;

    Order o = make_limit(1, make_sym("SPY"), T::Side::BUY, 400LL * T::PRICE_SCALE, 1);

    std::vector<std::int64_t> samples;
    samples.reserve(RUNS);

    for (int i = 0; i < RUNS; ++i) {
        Order out{};
        auto t0 = now();
        queue.push(o);
        queue.pop(out);
        auto t1 = now();
        do_not_optimise(out);
        samples.push_back(elapsed_ns(t0, t1));
    }

    std::sort(samples.begin(), samples.end());
    return Result{
        "SPSC Queue Round-Trip Latency",
        samples[samples.size() / 2],
        0.0,
        true
    };
}

// ─── 6. SPSC queue throughput ─────────────────────────────────────────────────

static Result bench_spsc_throughput() {
    constexpr int N = 1'000'000;
    constexpr std::size_t CAPACITY = 1024;

    using Q = exchange::core::SPSCQueue<exchange::matching::InboundOrder, CAPACITY>;
    Q queue;

    Order tmpl = make_limit(1, make_sym("QQQ"), T::Side::SELL, 350LL * T::PRICE_SCALE, 5);

    int remaining = N;

    auto t0 = now();
    while (remaining > 0) {
        while (remaining > 0 && queue.push(tmpl)) {
            --remaining;
        }
        Order out{};
        while (queue.pop(out)) {
            do_not_optimise(out);
        }
    }
    auto t1 = now();

    const double seconds = static_cast<double>(elapsed_ns(t0, t1)) * 1e-9;
    return Result{
        "SPSC Queue Throughput",
        0,
        static_cast<double>(N) / seconds / 1e6,
        false
    };
}

// ─── 7. Mixed workload throughput ─────────────────────────────────────────────

static Result bench_mixed_workload() {
    constexpr int N = 300'000;

    Engine engine;
    auto sym = make_sym("AMD");
    engine.prepare_symbols(std::span(&sym, 1));

    // Seed passive ASKs
    for (int i = 0; i < 500; ++i) {
        auto ask = make_limit(
            static_cast<std::uint64_t>(9'000'000 + i),
            sym, T::Side::SELL,
            (300LL + i) * T::PRICE_SCALE, 1'000);
        engine.process_order(ask);
    }

    std::vector<std::uint64_t> live_ids;
    live_ids.reserve(N);
    std::uint64_t next_id = 1;
    std::uint64_t cancel_cursor = 0;

    auto t0 = now();
    for (int i = 0; i < N; ++i) {
        const int bucket = i % 10;

        if (bucket <= 5) {
            Order o = make_limit(next_id++, sym, T::Side::BUY,
                                 (200LL + (i % 50)) * T::PRICE_SCALE, 10);
            auto ev = engine.process_order(o);
            do_not_optimise(ev);
            for (const auto& e : ev) {
                if (e.type == exchange::matching::EventType::ORDER_RESTED) {
                    live_ids.push_back(o.order_id);
                    break;
                }
            }
        } else if (bucket <= 8) {
            Order o{};
            o.action     = Action::ADD;
            o.symbol     = sym;
            o.order_id   = next_id++;
            o.side       = T::Side::BUY;
            o.price      = 0;
            o.qty        = 1;
            o.order_type = T::OrderType::MARKET;
            o.timestamp  = next_id;
            o.participant_id = 2;
            auto ev = engine.process_order(o);
            do_not_optimise(ev);
        } else {
            if (!live_ids.empty() && cancel_cursor < live_ids.size()) {
                Order c = make_cancel(live_ids[cancel_cursor++], sym);
                auto ev = engine.process_order(c);
                do_not_optimise(ev);
            }
        }
    }
    auto t1 = now();

    const double seconds = static_cast<double>(elapsed_ns(t0, t1)) * 1e-9;
    return Result{
        "Mixed Workload Throughput (60/30/10)",
        0,
        static_cast<double>(N) / seconds / 1e6,
        false
    };
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main() {
    print_header();

    std::vector<Result> results;
    results.push_back(bench_order_submission_latency());
    results.push_back(bench_limit_order_throughput());
    results.push_back(bench_matching_throughput());
    results.push_back(bench_cancel_latency());
    results.push_back(bench_spsc_latency());
    results.push_back(bench_spsc_throughput());
    results.push_back(bench_mixed_workload());

    for (const auto& r : results) {
        print_result(r);
    }

    std::cout << std::string(82, '-') << "\n";
    std::cout << "\nNotes:\n"
              << "  * Compiled with -O2/-O3 Release optimisations.\n"
              << "  * Latency figures are median over repeated single-op measurements.\n"
              << "  * Throughput figures are batch sustained rates.\n"
              << "  * Results vary by CPU, OS scheduler, and memory sub-system.\n\n";

    return 0;
}
