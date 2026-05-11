# Test fixtures (optional real-world images)

The test suite is **fully self-contained by default** — every `integration_*`
test generates the FITS/XISF bytes it needs at runtime (see
`tests/helpers/synth_fits.{h,cpp}` and `tests/helpers/synth_xisf.{h,cpp}`).
That means a fresh `git clone` runs all tests green without any extra files.

This directory is reserved for **opt-in real images** that, when present,
unlock additional realism checks (header diversity, true sensor noise,
real star fields, etc.). Tests that depend on these files use
`GTEST_SKIP()` when the file is missing — they never fail the build.

## Expected files (drop them in here)

| Filename               | Type | Notes                                            |
| ---------------------- | ---- | ------------------------------------------------ |
| `sample_mono.fit`      | FITS | small mono frame (a few MB), real headers        |
| `sample_color.fit`     | FITS | OSC / bayered frame, exposes color metadata      |
| `sample_master.xisf`   | XISF | calibrated master (any size), tests XISF parser  |
| `corrupted.fit`        | FITS | hand-truncated to exercise the error path        |

Keep individual files **under ~10 MB** so checking them into git stays cheap.
For anything larger, store outside the repo and point the test at it via an
environment variable (a future test can opt into that pattern when needed).

## How to provide images

If you're contributing back, attach the files to a PR and add a short note
about the capture (telescope / camera / filter / exposure) so future readers
have provenance. Anonymized strip of acquisition headers is fine if you'd
rather not share the full instrument metadata.
