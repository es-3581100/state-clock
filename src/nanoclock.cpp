#include "nanoclock.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace nanoclock {
namespace {
constexpr std::uint64_t NS_PER_SEC = 1'000'000'000ULL;

std::int64_t signed_delta(std::uint64_t a, std::uint64_t b) noexcept {
    if (a >= b) {
        const auto d = a - b;
        return d > static_cast<std::uint64_t>(INT64_MAX) ? INT64_MAX : static_cast<std::int64_t>(d);
    }
    const auto d = b - a;
    return d > static_cast<std::uint64_t>(INT64_MAX) ? INT64_MIN : -static_cast<std::int64_t>(d);
}
} // namespace

std::uint64_t NanoClock::clock_ns(clockid_t id) noexcept {
    timespec ts{};
    if (::clock_gettime(id, &ts) != 0) {
        std::terminate();
    }
    return static_cast<std::uint64_t>(ts.tv_sec) * NS_PER_SEC + static_cast<std::uint64_t>(ts.tv_nsec);
}

NanoClock::Anchor NanoClock::capture_anchor(unsigned trials) noexcept {
    if (trials == 0) trials = 1;

    Anchor best{};
    std::uint64_t best_width = std::numeric_limits<std::uint64_t>::max();

    // Sandwich CLOCK_REALTIME between two CLOCK_BOOTTIME reads. Choosing the
    // narrowest sandwich minimizes uncertainty in which monotonic instant the
    // wall sample corresponds to. CLOCK_BOOTTIME keeps advancing through suspend.
    for (unsigned i = 0; i < trials; ++i) {
        const auto m0 = clock_ns(CLOCK_BOOTTIME);
        const auto wall = clock_ns(CLOCK_REALTIME);
        const auto m1 = clock_ns(CLOCK_BOOTTIME);
        const auto width = m1 - m0;

        if (width < best_width) {
            best_width = width;
            best.monotonic_ns = m0 + width / 2;
            best.epoch_ns = wall;
            // Worst-case temporal placement of the wall sample inside sandwich.
            best.uncertainty_ns = (width + 1) / 2;
            best.publication_floor_ns = 0;
            if (width == 0) break;
        }
    }
    return best;
}

NanoClock::NanoClock(unsigned anchor_trials) {
    const auto a = capture_anchor(anchor_trials);
    anchor_epoch_ns_.store(a.epoch_ns, std::memory_order_relaxed);
    anchor_monotonic_ns_.store(a.monotonic_ns, std::memory_order_relaxed);
    anchor_uncertainty_ns_.store(a.uncertainty_ns, std::memory_order_relaxed);
    publication_floor_ns_.store(0, std::memory_order_relaxed);
}

NanoClock::Anchor NanoClock::read_anchor() const noexcept {
    for (;;) {
        const auto begin = seq_.load(std::memory_order_acquire);
        if (begin & 1U) continue;

        Anchor a{
            anchor_epoch_ns_.load(std::memory_order_relaxed),
            anchor_monotonic_ns_.load(std::memory_order_relaxed),
            anchor_uncertainty_ns_.load(std::memory_order_relaxed),
            publication_floor_ns_.load(std::memory_order_relaxed),
        };

        std::atomic_thread_fence(std::memory_order_acq_rel);
        const auto end = seq_.load(std::memory_order_acquire);
        if (begin == end) return a;
    }
}

std::uint64_t NanoClock::derive_raw(std::uint64_t monotonic_now_ns, const Anchor& a) const noexcept {
    const auto elapsed = monotonic_now_ns >= a.monotonic_ns ? monotonic_now_ns - a.monotonic_ns : 0ULL;
    if (elapsed > std::numeric_limits<std::uint64_t>::max() - a.epoch_ns) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return a.epoch_ns + elapsed;
}

std::uint64_t NanoClock::now_ns() const noexcept {
    for (;;) {
        const auto begin = seq_.load(std::memory_order_acquire);
        if (begin & 1U) continue;

        const Anchor a{
            anchor_epoch_ns_.load(std::memory_order_relaxed),
            anchor_monotonic_ns_.load(std::memory_order_relaxed),
            anchor_uncertainty_ns_.load(std::memory_order_relaxed),
            publication_floor_ns_.load(std::memory_order_relaxed),
        };
        const auto mono = clock_ns(CLOCK_BOOTTIME);

        // The second sequence read makes the clock sample part of the coherent
        // snapshot. If resync crossed this read, retry against the new anchor.
        std::atomic_thread_fence(std::memory_order_acq_rel);
        const auto end = seq_.load(std::memory_order_acquire);
        if (begin != end) continue;

        const auto raw = derive_raw(mono, a);
        return std::max(raw, a.publication_floor_ns);
    }
}

AnchorReport NanoClock::resync(unsigned anchor_trials) noexcept {
    auto fresh = capture_anchor(anchor_trials);

    // Capture can be relatively expensive, so do it before entering the tiny
    // writer critical section. Once seq becomes odd, new readers retry.
    // Reserve the odd sequence value exclusively. fetch_add would let two
    // concurrent resync writers temporarily make seq_ even during a write.
    auto sequence = seq_.load(std::memory_order_acquire);
    for (;;) {
        if ((sequence & 1U) == 0 &&
            seq_.compare_exchange_weak(sequence, sequence + 1,
                                       std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
            break;
        }
        sequence = seq_.load(std::memory_order_acquire);
    }

    const Anchor old{
        anchor_epoch_ns_.load(std::memory_order_relaxed),
        anchor_monotonic_ns_.load(std::memory_order_relaxed),
        anchor_uncertainty_ns_.load(std::memory_order_relaxed),
        publication_floor_ns_.load(std::memory_order_relaxed),
    };
    const auto commit_mono = clock_ns(CLOCK_BOOTTIME);
    const auto old_published = std::max(derive_raw(commit_mono, old), old.publication_floor_ns);
    const auto fresh_raw = derive_raw(commit_mono, fresh);

    // If corrected wall time is behind, hold only the old published value. As
    // fresh raw time advances it naturally overtakes this floor. No hot-path
    // atomic write and no synthetic +1 ns increments are needed.
    fresh.publication_floor_ns = fresh_raw < old_published ? old_published : 0;

    anchor_epoch_ns_.store(fresh.epoch_ns, std::memory_order_relaxed);
    anchor_monotonic_ns_.store(fresh.monotonic_ns, std::memory_order_relaxed);
    anchor_uncertainty_ns_.store(fresh.uncertainty_ns, std::memory_order_relaxed);
    publication_floor_ns_.store(fresh.publication_floor_ns, std::memory_order_relaxed);
    seq_.store(sequence + 2, std::memory_order_release);

    return AnchorReport{
        fresh.epoch_ns,
        fresh.monotonic_ns,
        fresh.uncertainty_ns,
        signed_delta(fresh_raw, old_published),
    };
}

Diagnostics NanoClock::diagnostics(unsigned samples) const noexcept {
    if (samples < 2) samples = 2;

    std::uint64_t min_step = std::numeric_limits<std::uint64_t>::max();
    auto prev = clock_ns(CLOCK_BOOTTIME);
    for (unsigned i = 1; i < samples; ++i) {
        const auto cur = clock_ns(CLOCK_BOOTTIME);
        if (cur > prev) min_step = std::min(min_step, cur - prev);
        prev = cur;
    }
    if (min_step == std::numeric_limits<std::uint64_t>::max()) min_step = 0;

    std::uint64_t best_window = std::numeric_limits<std::uint64_t>::max();
    std::int64_t best_skew = 0;
    for (unsigned i = 0; i < 256; ++i) {
        const auto d0 = now_ns();
        const auto wall = clock_ns(CLOCK_REALTIME);
        const auto d1 = now_ns();
        const auto window = d1 - d0;
        if (window < best_window) {
            best_window = window;
            const auto midpoint = d0 + window / 2;
            best_skew = signed_delta(wall, midpoint);
        }
    }

    const auto a = read_anchor();
    return Diagnostics{
        a.uncertainty_ns,
        min_step,
        best_window == std::numeric_limits<std::uint64_t>::max() ? 0 : best_window,
        best_skew,
    };
}

std::string NanoClock::iso8601_utc(std::uint64_t epoch_ns) {
    const std::time_t seconds = static_cast<std::time_t>(epoch_ns / NS_PER_SEC);
    const auto nanos = static_cast<unsigned>(epoch_ns % NS_PER_SEC);
    std::tm tm{};
    gmtime_r(&seconds, &tm);

    char base[32]{};
    std::strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &tm);

    std::ostringstream out;
    out << base << '.' << std::setw(9) << std::setfill('0') << nanos << 'Z';
    return out.str();
}

std::string NanoClock::u64_hex(std::uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex << std::setw(16) << std::setfill('0') << value;
    return out.str();
}

} // namespace nanoclock
