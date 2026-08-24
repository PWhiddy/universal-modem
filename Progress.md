# Universal Modem Progress

This file is an append-oriented engineering log. New work should add dated entries; earlier measurements and decisions should remain available for comparison.

## 2026-08-24 — Portable coded-OFDM foundation

Implemented the first required incremental layer as C11 with no third-party code dependencies:

- A 48 kHz, real-valued OFDM signal path with a fixed 256-point FFT (187.5 Hz carrier spacing), configurable carrier bounds, cyclic prefix, and overlap window.
- Gray-coded, unit-average-power QAM-4, QAM-16, and QAM-64 constellations with soft demodulation.
- Constraint-length-7 convolutional coding using the standard octal 171/133 polynomials, six termination bits, soft-decision Viterbi decoding, and punctured 1/2, 2/3, and 3/4 rates.
- Deterministic whole-block time/frequency interleaving so a contiguous acoustic cutout becomes dispersed erasures at the convolutional decoder.
- Self-synchronizing frames: an 8 ms swept-frequency sync waveform, quiet guard, two repeated channel-estimation symbols, per-symbol pilots, a fixed QAM-4/rate-1/2 protected header, and CRC-16/CRC-32 integrity checks.
- Channel normalization from the repeated training symbols. This supplies automatic gain handling and per-carrier complex equalization for quiet/loud signals and multipath within the cyclic prefix.
- A deterministic acoustic simulator supporting leading delay, attenuation/amplification, additive Gaussian noise, a delayed echo, clipping, and sample cutouts.

Default working configuration: bins 16–72 (3.0–13.5 kHz), 52 data carriers plus five pilots, QAM-16, rate-1/2 FEC, a 32-sample (0.667 ms) prefix, and an 8-sample (0.167 ms) overlap window. The conservative prefix and coding are discovery/data defaults only; calibration may select different values by direction.

Boundary leakage was measured using an oversampled FFT over a multi-symbol frame. With the same payload, rectangular symbol boundaries measured -20.96 dB out-of-band power and the 8-sample overlap window measured -23.66 dB. The window therefore remains enabled by default. High-quality calibration applies a small spectral-quality penalty to a zero-window candidate so an unrealistically flat simulated channel cannot select it solely for its few trailing samples.

## 2026-08-24 — Calibration

Implemented a real encode/channel/decode sweep rather than a formula-only selector. Every candidate logs its frequency range, QAM order, prefix duration, FEC rate, window, decode result, estimated SNR, EVM, payload rate, and score.

The default sweep tests 81 combinations: three useful bands, all three QAM modes, prefixes of 16/32/64 samples, and all three coding rates. With 32-byte probes and explicit 20 ms response/turnaround allowance, measured emitted-time estimates are 6.8 seconds per direction and 13.5 seconds for both directions. Handshake/configuration messages bring the intended live budget close to the roughly 15-second requirement.

`--calib-high` also sweeps ten bands, seven prefix lengths from 8–96 samples, and four overlap-window lengths. It tests 2,430 valid combinations and currently estimates 390 seconds for both directions, within the requested several-minute exploratory budget.

## 2026-08-24 — Asynchronous session simulation

Implemented a two-endpoint virtual acoustic session using the production modem frames for every wire message:

- The client periodically sends `DISCOVER`; the listening gateway sends `OFFER`; the client returns `CONFIRM`.
- Each direction calibrates independently and retains its own selected modem configuration.
- Data uses sequenced stop-and-wait delivery with acknowledgements, duplicate suppression, bounded retries, and direction-specific channel/configuration state.
- The simulator inserts a 2.8-second full acoustic blackout during transfer. After three timeouts both endpoints return to discovery, tolerate discovery attempts made during the outage, reconnect, and resume without duplicating delivered bytes.
- Transmit/listen turnarounds are explicitly scheduled (60 ms negotiation and 25 ms data/ACK), so endpoints do not listen to their own output or transmit simultaneously in the validated protocol flow.

The default simulation transferred 3,072 bytes client-to-gateway and 1,536 bytes gateway-to-client, incurred three data retries, re-established the connection once, and finished connected in 20.92 seconds of virtual time.

## Verification

`make test` passes 11 test groups covering known CRC vectors, FFT numerical round trips, every QAM point, all FEC rates, interleaved burst correction, all nine QAM/FEC frame combinations, zero-length frames, combined gain/noise/echo/clipping/cutout impairments, the bounded calibration budget, spectral leakage, false-sync rejection, and the complete loss/reconnection session.

An AddressSanitizer + UndefinedBehaviorSanitizer build also passes with leak detection disabled because LeakSanitizer is unavailable under the execution environment's ptrace wrapper. The normal build is warning-free with `-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes`.

## Next hardware-facing increment

Live microphone/speaker backends, streaming frame acquisition, TUN/utun bindings, and gateway OS forwarding/NAT are not yet connected. They should follow only after capturing representative two-machine audio and adding those measured clock drift, frequency response, and nonlinear distortion profiles to the simulator. The current `--audio --gateway` and `--audio --client` paths fail explicitly instead of changing routes or claiming a usable network prematurely.

## 2026-08-24 — Development status and build commands

The dependency-free signal path, channel simulator, calibration sweep, and asynchronous end-to-end session simulator are implemented. Live audio-device and TUN/utun integration remain intentionally gated on the tested modem layers.

Build and run the verified stages with:

```sh
make
make test
./universal-modem --simulate
./universal-modem --calibrate
./universal-modem --session-sim
```

## 2026-08-24 — Progressive distortion-to-failure validation

Moved development status and build commands out of `README.md` so the README remains the untouched requirements document. Operational status continues here in the append-oriented log.

Replaced the calibration and asynchronous-session single-point channel checks with a shared eight-level distortion ladder. Each runner now starts with a genuinely clean channel and cumulatively introduces:

1. asymmetric quiet/loud gain and capture delay;
2. multipath echo and increasing echo delay;
3. additive Gaussian noise;
4. nonlinear clipping, an in-frame sample cutout, and a timed total-link blackout;
5. increasingly severe combinations of all those impairments;
6. a terminal no-usable-signal profile that guarantees a measured failure boundary.

The two directions deliberately use different levels to model asymmetric microphones, speakers, and room paths. Every pass logs all exact channel values. A passing combined-impairment level must exercise gain extremes, leading delay, echo, noise, clipping, sample loss, and—in the session runner—a full outage before the ladder is allowed to count the coverage as complete.

Calibration now performs one full candidate sweep, ranks successful candidates, and then requires three independently seeded 128-byte verification frames (five under `--calib-high`) before selecting a configuration. It falls through to slower/more robust candidates when a fast candidate is not repeatable. This fixed the observed case where one 32-byte probe selected a QAM/FEC combination that later failed every data packet. A clean default calibration still measures 14.04 seconds for both directions, preserving the roughly 15-second requirement.

Measured default calibration ladder result: levels 0–4 passed, including the all-acoustic-impairments level; level 5 (`strong-combined`) was the first failure. Across the six attempted levels it ran 12 directional calibrations, 972 primary candidate probes, 43 ranked verification frames, and 85.8 seconds of simulated audio.

Session ACKs and control traffic now remain on the robust QAM-4/rate-1/2 bootstrap configuration rather than inheriting an aggressively calibrated data mode. The distortion validation uses 64-byte data chunks, while payload traffic still uses the independently calibrated direction-specific modes. Level 4 passed with all channel impairments plus a 2.2-second blackout, three timeouts, discovery fallback, one successful reconnection, duplicate-safe resume, and complete 3,072/1,536-byte bidirectional transfer. Level 5 was the first session failure. The six attempted session levels represented 173.1 seconds of virtual time.

`make test` now passes 13 test groups. New assertions verify that profile zero is clean, impairment masks grow cumulatively, all impairment classes are exercised before failure, calibration performs repeated verification frames, both ladders stop at exactly their first expected signal failure, and the last passing session level includes successful outage recovery.

## 2026-08-24 — Native real-audio connection and transfer phase

Implemented the hardware-facing audio phase without enabling TUN/utun, route, forwarding, or NAT changes. The executable now has native system backends for ALSA on Linux and CoreAudio/Audio Queue Services on macOS. Both use 48 kHz mono signed 16-bit PCM at the device boundary and convert to the modem's floating-point sample representation internally. Linux links only the system ALSA library; macOS links only the bundled CoreAudio, AudioToolbox, and CoreFoundation frameworks.

Every real-audio startup enumerates input and output devices before opening either one, prints stable device IDs and descriptions, and then prints the exact selected input/output IDs. `--list-audio` performs enumeration without starting a session. Devices can be selected independently with `--input-device ID` and `--output-device ID`; `default` remains the default for each. CLI logs are explicitly flushed so state remains visible immediately even when redirected to a file.

The real link uses a versioned binary wire envelope inside the existing CRC-protected coded-OFDM frame. It implements `DISCOVER`, `OFFER`, `CONFIRM`, and `CONNECTED` negotiation; typed calibration control/probe/report/verification messages; test-data frames and robust bootstrap-mode ACKs; transfer turns; and completion. Session IDs reject unrelated traffic. The client repeats discovery every two seconds by default, while the gateway listens indefinitely. A signal loss, failed verification, exhausted data retry, or malformed link frame returns both roles to discovery/listening and starts a fresh session rather than continuing with stale configuration.

Audio operation is explicitly half duplex. Capture is dropped/paused before every playback operation, output is fully drained, a 50 ms quiet tail is included for acoustic decay, and capture is restarted and flushed afterward. An additional 80 ms protocol turnaround is reserved before replies so the peer has time to drain its speaker and resume its microphone. CoreAudio waits for the queue's actual stopped state rather than treating a reusable playback buffer as proof that the speaker has finished.

Real calibration is independent in each direction. The default live design uses a balanced 27-point fractional sweep across all three frequency ranges, QAM-4/16/64, 16/32/64-sample prefixes, and all three FEC rates. At 220 ms per deterministic slot this schedules 5.94 seconds of probes per direction, or 11.88 seconds total; handshakes, selection reports, and three independent 128-byte verification frames put the intended wall time near the roughly 15-second target. `--calib-high` uses 810 valid balanced candidates across ten ranges, seven prefix lengths, four overlap windows, all QAM modes, and all FEC rates: 178.2 seconds of probes per direction and roughly six minutes total before control overhead. Every attempted configuration logs its complete parameters and its decode result; successful receives also log SNR, EVM, and throughput score. The highest-scoring candidate must then pass all three independent verification frames. Each direction communicates its own calibration mode, so peers with different local `--calib-high` choices can still interoperate.

After calibration, the client initiates a deterministic test transfer in both directions. Defaults are 1,024 bytes each way in 128-byte chunks. Data runs on the independently selected directional mode; ACK and control messages remain on the robust QAM-4/rate-1/2 bootstrap mode. Stop-and-wait delivery provides bounded retransmission and duplicate suppression. The receiver verifies every generated byte, while progress logs report bytes, effective bit rate, sequence, sync correlation, estimated SNR, and EVM. `--test-bytes`, `--chunk-bytes` (up to 512), and `--retries` make the physical test easy to shorten or expand.

Build and inspect devices with:

```sh
make
./universal-modem --list-audio
```

For this phase, run without `sudo` unless the local audio setup specifically requires it; no privileged network state is touched. On the two systems, use IDs printed at startup if `default` is not correct:

```sh
./universal-modem --audio --gateway --input-device INPUT_ID --output-device OUTPUT_ID
./universal-modem --audio --client  --input-device INPUT_ID --output-device OUTPUT_ID
```

The hardware-independent suite now passes 15 test groups. New coverage round-trips maximum-sized live wire messages, rejects invalid wire headers and capacities, validates all 27/810 live calibration candidates, proves coverage of every QAM/FEC/prefix/window class, and confirms even the slowest emitted probe fits its assigned 220 ms slot. The default and high worst-case modem frames measured 80.8 ms and 108.3 ms respectively before their audio guards. AddressSanitizer and UndefinedBehaviorSanitizer also pass all 15 groups with leak detection disabled for the ptrace-constrained test environment.

ALSA enumeration and a full native open/listen/interrupt lifecycle were exercised locally using its `null` PCM. The sandbox exposes no real `/dev/snd` nodes, so acoustic transfer between independent physical devices remains the deliberate next manual test. The CoreAudio implementation was checked against Apple's Audio Queue device-selection, callback, pause/start, and synchronous-disposal semantics but still needs its first compile/run on a macOS host. TUN/utun proxying remains explicitly deferred until the two-system acoustic test succeeds.

## 2026-08-24 — macOS SDK format-flag correction

The first macOS build exposed an incorrect CoreAudio constant name in the signed 16-bit PCM stream description. Replaced `kLinearPCMFormatFlagsNativeEndian` with the SDK-defined `kAudioFormatFlagsNativeEndian`. The Linux warning-free build and all 15 modem tests continue to pass; the macOS build should be rerun to validate the complete native branch.
