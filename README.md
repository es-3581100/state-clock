# NanoClock Native

A Linux-native implementation of the **observer-independent derived-state clock** model from the Nano-Sec State Clock HTML prototype.

The core invariant is:

```text
canonical_ns = epoch_anchor_ns + monotonic_elapsed_ns
```

There is no software tick accumulator. Reading the clock samples current state; it does not advance state. A delayed observer simply reads the current derived value when it wakes.

## Why C++20 for this reference build

The target behavior is native Linux timing, and this build environment has GCC/Clang but no Rust toolchain. C++20 gives direct, zero-framework access to `clock_gettime`, atomics, and the Linux vDSO, so the implementation can be compiled, benchmarked, and stress-tested here rather than shipped as unverified source. The core API is intentionally small enough to wrap behind a C ABI or port directly to Rust.

## Clock sources

- `CLOCK_REALTIME` — wall/UTC anchor, disciplined by the operating system/NTP/PTP configuration.
- `CLOCK_BOOTTIME` — monotonic elapsed source that continues across system suspend.
- Anchor capture uses a **monotonic → realtime → monotonic sandwich** and keeps the narrowest of multiple trials. Half the sandwich width is reported as anchor placement uncertainty.
- Published values use a resync-boundary monotonic floor. The normal read path performs no shared-state write. Equal consecutive reads are allowed; the clock never invents `+1 ns` merely because code executed.
- Concurrent resync writers are serialized using an atomic sequence compare-and-exchange; readers retry if a resync overlaps their sample.
- Resync adopts a corrected wall anchor even if it moved backward, but output remains on the non-regression floor until corrected wall time catches up. This avoids making a backward correction into a permanent forward offset.

## Accuracy claim

`nanosecond` means **integer nanosecond address/representation**, not guaranteed 1 ns physical accuracy. Actual accuracy is bounded by the host clock discipline, kernel clocksource, hardware, and anchor sampling uncertainty. On Linux, `clock_gettime` is normally served through the vDSO, so reads can be extremely cheap without a kernel context switch.

For genuinely accurate wall time, discipline the host with a good NTP implementation or PTP/hardware timestamping. A user-space clock cannot manufacture accuracy that the operating system/hardware does not possess.

## Build

```bash
make
./build/nanoclock --self-test
./build/nanoclock --diag
./build/nanoclock --bench
```

For a machine-local optimized binary:

```bash
make native
```

## CLI

```text
--once       one timestamp
--json       one timestamp + diagnostics as JSON
--watch      ~60 Hz sampled display; sampling cadence does not drive the clock
--diag       source-step and wall-skew diagnostics
--bench      10,000,000-call hot-path benchmark
--self-test  end-to-end invariant/stress checks
```

## Why not hand-roll RDTSC/RDTSCP?

An invariant TSC can be faster as a raw cycle counter, but converting it into correct civil/UTC time across CPUs, frequency policy, suspend, virtualization, clocksource changes, NTP/PTP discipline, and migration means reimplementing pieces the kernel already maintains. Linux's vDSO clock path may itself use the TSC while preserving kernel timekeeping semantics. For the combined goal of **speed + wall-time correctness**, the kernel clock path is the stronger default.

## Scope and limitations

This repository contains the **native Linux C++20 reference implementation**, not the earlier HTML/browser prototype. The API is process-local; it is not a clock synchronization protocol or a hardware timing instrument. `now_ns()` does not periodically re-anchor itself, so an application that needs to track later wall-clock corrections must call `resync()` at an appropriate interval. Published values are non-decreasing **per observed call sequence**; there is no claim of globally increasing values across independent processes or strictly increasing values between simultaneous callers.
