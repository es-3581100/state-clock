# Local validation

The source included in this repository was checked on Linux with GCC 14.2.0.

- `make -j2 all`: pass
- `make test`: 8 / 8 self-tests passed in three repeated optimized runs
- AddressSanitizer + UndefinedBehaviorSanitizer build and self-tests: 8 / 8 passed
- GitHub-hosted CI: **not yet verified** at the time of preparation

The archived benchmark logs from the uploaded ZIP were **not** copied into this
source release, because their performance readings and binary hashes are tied
to the earlier pre-patch build/environment. See the original ZIP for those
historical measurements. No physical nanosecond-accuracy claim is made.
