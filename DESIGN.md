# Design notes

## Semantic contract carried forward

1. **No callback-driven state.** No interval/frame/counter advances canonical time.
2. **Absolute derivation.** Every read is anchor plus current monotonic elapsed time.
3. **Observation is separate from state.** CLI/UI cadence only controls observation.
4. **Non-regression.** A monotonic floor prevents published time from moving backward.
5. **No fabricated resolution.** Equal reads are valid; no synthetic `+1 ns` increments.
6. **Precision honesty.** Diagnostics measure observed source step and anchor uncertainty.

## Native refinement over the browser version

The browser prototype derives elapsed nanoseconds by multiplying a floating-point millisecond value before converting to `BigInt`. Native `clock_gettime` supplies integer seconds + nanoseconds directly, removing the floating-point conversion from the canonical path.

The native resync also differs deliberately: when wall time is corrected backward, the fresh wall anchor is still accepted internally. The publication floor holds output steady until the corrected wall catches up. This preserves non-regression without locking in an old forward offset indefinitely.

## Hot path

A normal `now_ns()` performs:

1. coherent atomic anchor snapshot;
2. one `clock_gettime(CLOCK_BOOTTIME)` read;
3. one integer subtraction/addition;
4. a second sequence read to prove the anchor did not change mid-sample;
5. one integer `max(raw, correction_floor)`.

The hot path performs **no atomic write**. The correction floor changes only during resync. No allocation, formatting, mutex, sleep, timer callback, or wall-clock syscall occurs on the hot path after initialization.

## Concurrent writer protection

The resync writer acquires the odd seqlock sequence with compare-and-exchange, then publishes the next even sequence only after the anchor fields are stored. Without exclusive writer admission, simultaneous resync calls could incorrectly expose an even sequence while an anchor update is in progress. The self-test includes concurrent resync writers and readers, in addition to ordinary multi-reader tests.
