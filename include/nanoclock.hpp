#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <time.h>

namespace nanoclock {

struct AnchorReport {
    std::uint64_t epoch_ns{};
    std::uint64_t monotonic_ns{};
    std::uint64_t uncertainty_ns{};
    std::int64_t wall_minus_clock_ns{};
};

struct Diagnostics {
    std::uint64_t anchor_uncertainty_ns{};
    std::uint64_t observed_monotonic_step_ns{};
    std::uint64_t best_wall_comparison_window_ns{};
    std::int64_t best_wall_minus_clock_ns{};
};

class NanoClock final {
public:
    explicit NanoClock(unsigned anchor_trials = 64);

    // Canonical UTC Unix-epoch nanoseconds. State is derived from an absolute
    // wall/monotonic anchor; calls do not advance a software tick counter.
    [[nodiscard]] std::uint64_t now_ns() const noexcept;

    // Refresh the wall anchor. A backward wall correction is adopted internally,
    // while the published value is held at the monotonic floor until wall time
    // catches back up. A forward correction becomes visible immediately.
    [[nodiscard]] AnchorReport resync(unsigned anchor_trials = 64) noexcept;

    [[nodiscard]] Diagnostics diagnostics(unsigned samples = 32768) const noexcept;

    [[nodiscard]] static std::string iso8601_utc(std::uint64_t epoch_ns);
    [[nodiscard]] static std::string u64_hex(std::uint64_t value);

private:
    struct Anchor {
        std::uint64_t epoch_ns;
        std::uint64_t monotonic_ns;
        std::uint64_t uncertainty_ns;
        std::uint64_t publication_floor_ns;
    };

    [[nodiscard]] static std::uint64_t clock_ns(clockid_t id) noexcept;
    [[nodiscard]] static Anchor capture_anchor(unsigned trials) noexcept;
    [[nodiscard]] Anchor read_anchor() const noexcept;
    [[nodiscard]] std::uint64_t derive_raw(std::uint64_t monotonic_now_ns, const Anchor& a) const noexcept;

    mutable std::atomic<std::uint64_t> seq_{0};
    mutable std::atomic<std::uint64_t> anchor_epoch_ns_{0};
    mutable std::atomic<std::uint64_t> anchor_monotonic_ns_{0};
    mutable std::atomic<std::uint64_t> anchor_uncertainty_ns_{0};
    mutable std::atomic<std::uint64_t> publication_floor_ns_{0};
};

} // namespace nanoclock
