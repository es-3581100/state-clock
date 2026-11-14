#include "nanoclock.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace {
using nanoclock::NanoClock;

void print_usage(const char* argv0) {
    std::cout
        << "NanoClock Native — observer-independent derived-state nanosecond clock\n\n"
        << "Usage:\n"
        << "  " << argv0 << " --once        Print one timestamp (default)\n"
        << "  " << argv0 << " --json        Print one machine-readable timestamp\n"
        << "  " << argv0 << " --watch       Continuously display sampled state\n"
        << "  " << argv0 << " --diag        Measure source step / wall skew\n"
        << "  " << argv0 << " --bench       Benchmark now_ns() hot path\n"
        << "  " << argv0 << " --self-test   Run end-to-end invariants\n";
}

int self_test() {
    NanoClock clock;
    int passed = 0;
    int failed = 0;
    auto check = [&](const char* name, bool ok) {
        std::cout << (ok ? "PASS  " : "FAIL  ") << name << '\n';
        ok ? ++passed : ++failed;
    };

    const auto a = clock.now_ns();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto b = clock.now_ns();
    check("observer stall does not create missed ticks", b > a && (b - a) >= 10'000'000ULL);

    bool regressed = false;
    auto prev = clock.now_ns();
    for (std::uint32_t i = 0; i < 1'000'000; ++i) {
        const auto cur = clock.now_ns();
        if (cur < prev) { regressed = true; break; }
        prev = cur;
    }
    check("1,000,000 hot-path reads never regress", !regressed);

    const auto before_sync = clock.now_ns();
    const auto report = clock.resync();
    const auto after_sync = clock.now_ns();
    check("resync preserves publication non-regression", after_sync >= before_sync);
    check("anchor capture has bounded uncertainty", report.uncertainty_ns < 1'000'000ULL);

    constexpr int THREADS = 4;
    constexpr int READS = 250'000;
    std::atomic<bool> thread_regression{false};
    std::vector<std::thread> workers;
    workers.reserve(THREADS);
    for (int t = 0; t < THREADS; ++t) {
        workers.emplace_back([&] {
            auto local_prev = clock.now_ns();
            for (int i = 0; i < READS; ++i) {
                const auto cur = clock.now_ns();
                if (cur < local_prev) {
                    thread_regression.store(true, std::memory_order_relaxed);
                    return;
                }
                local_prev = cur;
            }
        });
    }
    for (auto& w : workers) w.join();
    check("4-thread concurrent readers preserve per-reader order", !thread_regression.load());

    // Concurrent anchor writers must not make the sequence appear even while
    // one writer is updating it. Check per-reader ordering under resync load.
    std::atomic<bool> resync_regression{false};
    std::vector<std::thread> race_workers;
    for (int t = 0; t < 2; ++t) {
        race_workers.emplace_back([&] {
            for (int i = 0; i < 300; ++i) (void)clock.resync(2);
        });
    }
    for (int t = 0; t < 4; ++t) {
        race_workers.emplace_back([&] {
            auto local_prev = clock.now_ns();
            for (int i = 0; i < 100'000; ++i) {
                const auto cur = clock.now_ns();
                if (cur < local_prev) {
                    resync_regression.store(true, std::memory_order_relaxed);
                    return;
                }
                local_prev = cur;
            }
        });
    }
    for (auto& w : race_workers) w.join();
    check("2 concurrent resync writers and 4 readers preserve order", !resync_regression.load());

    const auto d = clock.diagnostics(8192);
    check("monotonic source exposes a positive observed step", d.observed_monotonic_step_ns > 0);
    check("derived/wall comparison is within 5 ms", std::llabs(d.best_wall_minus_clock_ns) < 5'000'000LL);

    std::cout << "\nRESULT " << (failed == 0 ? "PASS" : "FAIL")
              << " // " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}

void benchmark() {
    NanoClock clock;
    constexpr std::uint64_t N = 10'000'000ULL;
    volatile std::uint64_t sink = 0;

    for (int i = 0; i < 10000; ++i) sink = clock.now_ns();
    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < N; ++i) sink = clock.now_ns();
    const auto end = std::chrono::steady_clock::now();

    const auto total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    const double ns_per_call = static_cast<double>(total_ns) / static_cast<double>(N);
    const double calls_per_sec = 1e9 / ns_per_call;

    std::cout << std::fixed << std::setprecision(2)
              << "calls=" << N << '\n'
              << "total_ms=" << (static_cast<double>(total_ns) / 1e6) << '\n'
              << "ns_per_now=" << ns_per_call << '\n'
              << "calls_per_sec=" << calls_per_sec << '\n'
              << "sink=" << sink << '\n';
}

void diag() {
    NanoClock clock;
    const auto d = clock.diagnostics();
    std::cout << "anchor_uncertainty_ns=" << d.anchor_uncertainty_ns << '\n'
              << "observed_monotonic_step_ns=" << d.observed_monotonic_step_ns << '\n'
              << "best_wall_comparison_window_ns=" << d.best_wall_comparison_window_ns << '\n'
              << "best_wall_minus_clock_ns=" << d.best_wall_minus_clock_ns << '\n';
}

void once(bool json) {
    NanoClock clock;
    const auto ns = clock.now_ns();
    if (json) {
        const auto d = clock.diagnostics(4096);
        std::cout << "{"
                  << "\"canonical_ns\":" << ns << ','
                  << "\"u64\":\"" << NanoClock::u64_hex(ns) << "\","
                  << "\"iso_utc\":\"" << NanoClock::iso8601_utc(ns) << "\","
                  << "\"wall_source\":\"CLOCK_REALTIME\","
                  << "\"elapsed_source\":\"CLOCK_BOOTTIME\","
                  << "\"model\":\"observer-independent-derived-state\","
                  << "\"anchor_uncertainty_ns\":" << d.anchor_uncertainty_ns << ','
                  << "\"observed_source_step_ns\":" << d.observed_monotonic_step_ns << ','
                  << "\"best_wall_minus_clock_ns\":" << d.best_wall_minus_clock_ns
                  << "}\n";
    } else {
        std::cout << "canonical_ns=" << ns << '\n'
                  << "u64=" << NanoClock::u64_hex(ns) << '\n'
                  << "iso_utc=" << NanoClock::iso8601_utc(ns) << '\n'
                  << "source=CLOCK_REALTIME anchor + CLOCK_BOOTTIME elapsed\n"
                  << "model=observer-independent-derived-state\n";
    }
}

void watch() {
    NanoClock clock;
    for (;;) {
        const auto ns = clock.now_ns();
        std::cout << '\r' << NanoClock::iso8601_utc(ns)
                  << "  ns=" << ns << "  " << std::flush;
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}
} // namespace

int main(int argc, char** argv) {
    const std::string arg = argc > 1 ? argv[1] : "--once";
    if (arg == "--once") once(false);
    else if (arg == "--json") once(true);
    else if (arg == "--watch") watch();
    else if (arg == "--diag") diag();
    else if (arg == "--bench") benchmark();
    else if (arg == "--self-test") return self_test();
    else if (arg == "--help" || arg == "-h") print_usage(argv[0]);
    else { print_usage(argv[0]); return 2; }
    return 0;
}
