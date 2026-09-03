# Audio transport handoff

Last updated: 2026-09-03

Implementation baseline at handoff: `8e2e867` (`--allow-messages to allow apple push + imessage hopefully`)

## Purpose and source-of-truth notes

This document records the operational knowledge, physical-test results, failure analysis, and design constraints that are easy to lose because they are not obvious from the code. It is intentionally not another chronological implementation log.

- [`Progress.md`](Progress.md) is useful history, but it is append-only. Earlier entries often describe designs that were later removed or superseded. Read it from the bottom upward and confirm claims against the current code.
- [`README.md`](README.md) still contains aspirational language from the original project. In particular, `--light` is not implemented, and audio mode no longer requires an explicit `--audio` flag.
- The current command-line help and tests are authoritative for supported flags and wire behavior.
- Files under `logs/` are investigation artifacts from several incompatible protocol revisions. They are evidence, not golden-output fixtures.

## Current state in one page

The audio system is a real bidirectional IP link over speakers and microphones. The tested arrangement is a macOS client connected acoustically to a Linux Internet gateway. It has successfully carried DNS, HTTP, HTTPS with `curl`, and a small page in Firefox. The important pieces now working together are:

- audio discovery, handshake, asymmetric calibration, and calibration caching;
- raw TUN on Linux and raw utun on macOS;
- macOS route and DNS service-state publication;
- half-duplex token-controlled acoustic transfer;
- packet aggregation, bounded multi-cell windows, and selective repair;
- a quiet-by-default traffic policy to keep operating-system background traffic from consuming the link;
- a Linux transparent split-TCP relay, which is essential for making ordinary Internet TCP tolerate the acoustic RTT;
- scheduling that preserves TCP flow ordering and prevents parallel browser flows from creating deadline-sized head-of-line stalls;
- useful-IP goodput telemetry that is not inflated by acoustic retries.

What is **not** yet established:

- There is no final long-duration performance or reliability characterization on the current head revision.
- The latest Firefox fix was reported working by the operator, but there is no saved, comprehensive success log from that exact revision.
- `--allow-messages` is covered by policy tests, but a real end-to-end Messages send/receive was not physically confirmed before this handoff.
- The target of at least 4 kbps sustained useful Internet traffic has not been demonstrated. Historical browser runs reached roughly 2.0–2.4 kbps aggregate useful IP goodput; other runs were closer to 1.2 kbps.
- The Internet path is effectively IPv4-only.
- The transparent TCP relay is Linux-only, so a macOS gateway does not get the most important long-RTT workaround.
- There is no video/light transport yet.

## Known-good physical topology

The physical setup used during successful tests was:

- **Gateway:** Linux, with Internet egress observed as `wlo1`.
- **Gateway receive device:** JLAB TALK GO USB microphone.
- **Gateway transmit device:** Audioengine 2+ USB speakers.
- **Client:** MacBook/macOS using a dynamically allocated raw utun interface.
- **Isolation during honest tests:** Wi-Fi was disabled on the Mac so traffic could not bypass the acoustic link.

The PulseAudio/PipeWire-compatible endpoints observed on the Linux machine were:

```text
source: alsa_input.usb-18072020_JLAB_TALK_GO_MICROPHONE-00.analog-stereo
sink:   alsa_output.usb-Audioengine_Ltd._Audioengine_2__ABCDEFB1180003-00.analog-stereo
```

Direct ALSA access to these devices is normally the wrong choice while the desktop audio server owns them. This invocation:

```sh
sudo ./universal-modem --gateway \
  --input-device 'plughw:CARD=MICROPHONE,DEV=0' \
  --output-device 'plughw:CARD=A2,DEV=0'
```

failed with `Device or resource busy`. That was expected ownership contention, not a modem defect.

### Recommended launch sequence

On Linux, preserve the calling user's audio environment:

```sh
sudo -E ./universal-modem --gateway
```

On the Mac:

```sh
sudo ./universal-modem --client
```

To opt into the minimal Messages/APNs policy exception on the client:

```sh
sudo ./universal-modem --client --allow-messages
```

Start the gateway first. It waits in `LISTENING`; the client sends discovery approximately every two seconds.

`sudo -E` matters on the Linux desktop. The program separates privileges: network setup runs with saved root authority while the audio/modem worker runs as the invoking user from `SUDO_UID`/`SUDO_GID`. Preserving the user session environment lets ALSA `default` reach the intended PulseAudio/PipeWire routing. Without it, `default` previously resolved to the onboard PCH/ALC897 device even though the intended USB devices appeared in the enumeration.

Always inspect these startup lines, not merely the enumerated device list:

```text
Selected audio input:  ...
Selected audio output: ...
Opened audio input:  ...
Opened audio output: ...
```

The `Opened` lines are the best available confirmation of what the process actually obtained. When they say `pcm=default`, ALSA plugin routing still hides the final physical device, so also verify the desktop sound settings or `pactl` state when there is doubt.

`--allow-messages` only affects the client's outbound policy. Supplying it on the gateway is harmless but has no useful effect.

### Link-only testing

Use this before involving routes, NAT, DNS, or the firewall:

```sh
./universal-modem --client --link-test
./universal-modem --gateway --link-test
```

Root is unnecessary when the selected audio devices are accessible to the user. `--link-test` exercises the acoustic state machine but does not create TUN/utun interfaces or alter host networking.

## Architecture and invariants

The current path is conceptually:

```text
application TCP/UDP
  -> macOS utun
  -> traffic policy and packet queue
  -> serialized packet batch
  -> one or more FEC/CRC-protected acoustic cells
  -> half-duplex token transfer
  -> Linux TUN
  -> DNS/NAT or transparent split-TCP relay
  -> Internet
```

Return traffic traverses the reverse path. The main invariants worth preserving are:

- An acoustic message transports typed bytes; it is not equivalent to one IP packet. Small IP packets may be coalesced into one acoustic body, and a large serialized batch may span multiple acoustic cells.
- Packets belonging to one TCP flow retain capture order.
- A payload batch does not mix unrelated TCP flows. This prevents one missing cell from delaying every connection in a browser burst.
- Once a flow triggers a response, reverse-flow affinity gives that response a chance before unrelated queued flows.
- Pure cumulative TCP ACKs may replace older cumulative ACKs only when it is provably safe. Packets with SACK or unknown options are not casually coalesced.
- The link is half-duplex and token-controlled. This is not merely a scheduler implementation detail: timeouts, turnarounds, acknowledgements, and goodput all assume that only one endpoint emits acoustic data at a time.
- A receiver can decline a token offer when it has no queued return work, avoiding an otherwise empty round trip.
- Multi-cell windows use per-cell protection and a bitmap acknowledgement so only missing cells are repaired. The window is deliberately bounded to limit latency and failure blast radius.

Current format identifiers are live protocol `7`, configuration format `2`, and proxy format `7`. The physical audio constants remain 48 kHz sampling and a 2048-point FFT. Supported live acoustic bodies range from 128 to 2048 bytes; normal CLI `--chunk-bytes` defaults to 2048, but calibration and duration guards often select less.

The most important architectural debt is [`src/live.c`](src/live.c). It is over 5,000 lines and currently owns audio framing, discovery, calibration, cache exchange, link testing, proxy scheduling, telemetry, and token state. [`src/audio.h`](src/audio.h) is a narrow sample-I/O interface, but swapping that backend is not enough to create a video implementation because modulation, calibration capabilities, and live state are still audio-specific inside `live.c`.

## Networking details that were hard-won

### macOS client

The client installs split default routes through the peer:

```text
0.0.0.0/1   -> 10.77.0.1 on utunN
128.0.0.0/1 -> 10.77.0.1 on utunN
```

The explicit gateway is important. An interface-only utun route appeared valid but did not reliably deliver ordinary traffic to the userspace fd. During a session, these checks should resolve to `10.77.0.1` and the active utun:

```sh
route -n get 1.1.1.1
route -n get 8.8.8.8
scutil --nwi
scutil --dns
```

The program also publishes temporary IPv4 and DNS state through a held `SCDynamicStore` session. That makes macOS regard the utun as an IPv4/DNS-capable service and gives the resolver `1.1.1.1` with a 60-second timeout. `scutil --dns` should show an A-record-capable scoped resolver on the active utun, and `scutil --nwi` should report that interface as IPv4/DNS reachable.

The utun kernel pending-packet limit is raised from its one-packet default to 512, and the fd is drained non-blockingly. This fixed a severe head-of-line condition where a pending DNS or `curl` packet could remain in the kernel for minutes while the acoustic loop appeared idle. Extending DNS timeouts alone could never fix that because the query was not reaching the userspace queue.

The current implementation intentionally installs only `1.1.1.1`. Older logs that show both `1.1.1.1` and `8.8.8.8` came from older revisions and should not be used to infer current behavior.

There is no IPv6 default-route/NAT design. Disable alternate interfaces during controlled tests both to prevent bypass and to avoid misleading dual-stack behavior.

### Linux gateway

The gateway creates `/dev/net/tun` interface `umN`, assigns `10.77.0.1`, discovers the host's default egress, enables IPv4 forwarding, and configures NAT with iptables or nftables. The client is normally `10.77.0.2` on a `/30` subnet.

All client TCP is transparently terminated and re-originated by the Linux split-TCP relay using the original destination. UDP, including DNS, continues through ordinary routing/NAT. The relay does **not** intercept or decrypt TLS; it only splits one long-RTT TCP control loop into a local acoustic-side connection and an Internet-side connection while copying the byte stream end to end.

The origin-side connection is delayed until the relay receives the client's first application bytes, with a 30-second server-first fallback. This prevents a remote server from starting retransmission and timeout clocks several acoustic turns before the client request can arrive.

This relay currently depends on Linux transparent-proxy facilities (`SO_ORIGINAL_DST`). Running the gateway on macOS falls back to PF NAT without split TCP and can reproduce the old long-RTT/TLS failures.

### Cleanup after interruption

Normal `SIGINT`/Ctrl-C and `SIGTERM` trigger route, DNS, interface, NAT, and firewall cleanup. `SIGKILL`, a crash, or power loss can leave state behind.

- Linux firewall state is tagged with `universal-modem-<pid>` or uses an nftables table named `um_<pid>`.
- The macOS PF anchor is named `com.apple/universal-modem-<pid>`.
- If the process was killed immediately after enabling Linux forwarding, forwarding can remain enabled because the old value was never restored.

Inspect exact stale objects before removing them; do not blindly flush a host firewall or all routes.

## Calibration and cache behavior

Calibration is directional and intentionally asymmetric. Each endpoint determines how well it can receive the other direction, and each machine stores only its own receive-direction result in a local `calibration.config`. The identical filename on the two machines is therefore expected. Do not copy one endpoint's file onto the other.

The cache is atomically written and validated against role, protocol, and format. It is **not** keyed by:

- chosen input/output devices;
- the current PulseAudio/PipeWire route;
- mixer volume or automatic gain behavior;
- speaker/microphone positions;
- room geometry, fan noise, or other acoustic conditions.

Delete `calibration.config` on **both** machines after any meaningful device or room change, or as the first response to unexplained sustained fragility:

```sh
rm calibration.config
```

A link-stage failure discards cached modes in memory and forces calibration on the next connection attempt in that process. It does not erase the on-disk cache. If the process is restarted before it saves a good recalibration, the same stale mode can be loaded again; manual deletion is then required.

The untracked `calibration_to_laptop_with_fan.config` artifact uses protocol 4/config format 1. Current protocol validation will reject it. It should not be renamed to `calibration.config` for a current run.

The public library options contain a calibration path, but the main CLI has no `--calibration-path`. Keeping multiple physical profiles currently requires manually moving/renaming `calibration.config`, or adding a CLI option later.

### Reliability policy

Early calibration selected the fastest candidate that survived only a few short probes. In excellent-looking conditions it could choose a frontier mode such as 16-QAM, FEC 3/4, a very short prefix/training/sync, and a 21 kHz band. That mode then failed repeatedly during sustained proxy traffic. Requiring ten identical successful trials would be slow and still would not reserve environmental margin.

The current solution is deterministic structural backoff after search and verification:

- modulation above QPSK steps down one tier;
- FEC stronger than 1/2 receives another stronger-code tier;
- already-low modulation falls back toward repeated QPSK;
- cyclic prefix is at least 1024 samples (about 21.3 ms);
- equalizer window is at least 64;
- training is at least 3;
- sync is at least 1536 samples (32 ms);
- inter-frame gap is at least 2048 samples (about 42.7 ms);
- spectrum edges are stepped inward and the upper edge is capped at 18 kHz;
- deployed body size is one full confirmed tier below the largest body that passed.

Candidate bodies are 256, 384, 512, 768, 1024, 1536, and 2048 bytes, but a body whose modeled acoustic duration exceeds roughly 5.5 seconds is not used. Default calibration tries up to 12 probes per direction; `--calib-high` expands the search to 64.

Increasing `--retries` is not a substitute for PHY margin. It can turn corruption into long stalls and extra airtime. For recurring CRC/header failures, first verify the actual devices, clear both caches, and recalibrate. If needed, impose the same conservative `--chunk-bytes` limit on both endpoints while diagnosing.

There is no continuous rate adaptation once a session is established. Repeated failures eventually reconnect and recalibrate rather than stepping down smoothly in-session.

## Quiet firewall and Messages exception

The consolidated packet policy is [`src/traffic_policy.c`](src/traffic_policy.c). It exists because a normal macOS desktop can produce enough discovery, Apple-service, browser-maintenance, QUIC, and retry traffic to starve the one deliberate `curl` or page load.

Default behavior includes:

- dropping multicast, broadcast, stale DNS ICMP, NTP, and DNS-over-TLS traffic that is not useful to the intended narrow test;
- rejecting UDP/443 locally so applications fall back from QUIC to TCP instead of repeatedly timing out over an unsuitable path;
- returning local DNS NXDOMAIN responses for known blocked background names, rather than silently dropping and encouraging resolver retries;
- semantic suppression of duplicate queued, in-flight, and recently completed DNS work;
- DNS queue caps and deprioritization of tunnel discovery lookups.

Unknown non-Apple domains remain allowed. Broad Apple and Mozilla background zones are temporarily blocked. This is a DNS/packet policy, not a process firewall, so it has unavoidable limitations:

- cached DNS or direct-IP connections can bypass a hostname deny;
- it cannot reliably distinguish a Firefox preload from a user-requested visit to the same host;
- domain labels shown beside later IP packets are best-effort annotations learned from observed A/AAAA answers, not authoritative attribution.

`--allow-background` bypasses background DNS and port policy, but the fundamental QUIC/multicast/broadcast safety filters execute earlier and remain active.

`--allow-messages` permits the current minimum observed set for an already signed-in Mac sending/receiving plain-text iMessages:

```text
push.apple.com
push-apple.com.akadns.net
ess.apple.com
ess-apple.com.akadns.net
ess.g.aaplimg.com
```

APNs is shared by many Apple push services. Once `*.push.apple.com` is allowed, the modem cannot cryptographically or semantically isolate only Messages notifications from other Apple push traffic. Attachments, account setup, activation, broad iCloud synchronization, and every possible Messages edge case are intentionally not allowed by this minimal list.

The same suffixes currently appear in comments, the opt-in allow table, and the temporary block table. That repetition is not required for matching correctness: allow precedence makes the behavior work. It is policy-representation debt that should eventually become one categorized table. Also note that `ocsp2.apple.com` appears in both an always-allow certificate group and a temporary block group; always-allow precedence means it is effectively allowed.

Do not casually broaden or reorganize the temporary Apple firewall while starting video work. These rules were deliberately kept untouched during the last transport fixes. A dedicated Firefox profile may use [`config/firefox-quiet.user.js`](config/firefox-quiet.user.js), but that file disables update/security-list/background features and should never be copied into a person's normal browsing profile.

One testing caveat is intentional: after an explicit request during development, there is no direct assertion that APNs is blocked by default. Tests assert that the opt-in permits APNs/ESS and that unrelated Apple News traffic remains blocked.

## What the important failures actually were

### DNS failed instantly or appeared minutes late

This had two distinct macOS causes, neither of which was acoustic DNS RTT:

1. The raw utun did not initially publish enough IPv4/DNS service state for macOS to select it normally.
2. Even after routes looked correct, the utun's default one-packet pending queue caused kernel head-of-line blocking, so application queries never reached the modem's userspace queue.

The SCDynamicStore service publication, explicit peer routes, 512-packet pending limit, and nonblocking drain fixed those causes. Merely extending the resolver's timeout changed how long callers waited but did not make a missing query enter utun.

### “Checksum mismatch” was misdiagnosed as IP rewriting

Modem `payload checksum mismatch` means the acoustic frame failed its post-demodulation/FEC CRC. It is not evidence that macOS, TUN, or NAT emitted a bad UDP/IP checksum. Experimental IP/UDP checksum rewriting/materialization did not solve the problem and was removed. Do not restore checksum rewriting without a packet capture that proves the IP packet itself is malformed.

### Higher apparent DNS throughput made the link less useful

Delayed DNS responses caused macOS to retry the same semantic lookup, sometimes against multiple resolvers. Blindly prioritizing all DNS then transported the retries ahead of TCP and produced impressive-looking byte rates that were largely redundant work. Silent dropping was also harmful because it encouraged more retries.

The useful solution was local negative responses for known background domains plus semantic duplicate suppression across queued, in-flight, and recently completed requests. Current timers are approximately 15 seconds for in-flight suppression and 30 seconds for recently completed work, with queue limits to prevent DNS from owning all memory/airtime.

### Remote sites reset HTTP/TLS over the acoustic RTT

The repeated `Recv failure: Connection reset by peer` was not fixed by TLS-specific buffering, longer application deadlines, or packet-header tricks. It was reproduced locally against the real `justinjackson.ca` origin by adding about 2.5 seconds of delay in each direction. Rate limiting alone did not reproduce the same behavior. At roughly five seconds of end-to-end RTT, the origin's TCP retransmission/state timing could advance far enough that later delayed segments elicited a reset.

The proven workaround is the Linux transparent split-TCP relay. The remote origin sees a normal low-latency Linux TCP endpoint, while the Mac's slow TCP control loop ends locally at the gateway. TLS bytes remain end-to-end between the Mac application and the origin. Buffering “TLS packets” at the gateway is the wrong model because TLS rides a TCP byte stream and TCP timing/state was the failing layer.

### `curl` worked while Firefox did not

Firefox created multiple parallel TCP flows and attempted QUIC. Older proxy batching mixed data from independent flows into a long multi-cell window and released nothing until every cell completed. A single missing cell could therefore hold several connections for approximately 10–13 seconds. Meanwhile SYN/FIN/ACK chatter competed ahead of the response payload that would let the page progress.

The general solution, not a Firefox special case, was:

- reject QUIC locally so clients use the supported TCP path;
- give TCP payload precedence over pure control chatter;
- preserve order within each flow;
- keep payload batches flow-local;
- prefer the reverse flow after transmitting data likely to trigger a response;
- limit a turn by a latency/deadline budget as well as byte capacity.

Speculative SYN pruning, retransmission guessing, payload-over-SYN exceptions, arbitrary longer TLS deadlines, and a protocol-8/6144-byte-body experiment were non-fixes and were removed. They should not be resurrected without a new, measured failure model.

### Calibration looked excellent but sustained traffic collapsed

A short clean probe did not predict long-run robustness at the edge of the search envelope. The sustained link experienced invalid headers, checksum failures, selective-repair timeouts, and reconnection even with nominal SNR above 20 dB. The structural reliability guard and body-tier backoff described above are the current fix. The lesson is to optimize expected goodput after loss, not the raw bitrate of a single passing frame.

## Telemetry: what each number means

Normal full-proxy state progression is:

```text
LISTENING/DISCOVERING
  -> NEGOTIATING
  -> CONNECTED
  -> CALIBRATING or CALIBRATION_SKIPPED
  -> NETWORK_CONFIGURING
  -> NETWORK_READY
  -> PROXYING
```

Interpret common records as follows:

- `invalid frame header`: acquisition/header decode failed at the physical link.
- `payload checksum mismatch`: the frame was acquired but failed the payload CRC after demodulation/FEC.
- isolated receive misses or bitmap repairs: expected on a real acoustic channel.
- repeated misses followed by `RECONNECTING`: the mode or current environment is not sustainable.
- `proxy ingress queue ...`: the best snapshot of backlog, queue loss, duplicates, DNS suppression, ACK coalescing, and firewall rejection counters.
- TCP relay `open`, `first-byte`, and close-reason telemetry: separates origin-connect behavior from local/acoustic delay.
- a per-packet `rate=`: cumulative raw IP bytes observed in that direction since the session counter started; useful for trend, not instantaneous link capacity.
- calibration `rate@...`: modeled physical payload rate for that candidate and body size; it excludes most real proxy costs.
- `proxy internet-goodput wall=... upload=... download=... total=...`: the authoritative end-user rate. It counts complete client-perspective IP traffic only after it has been accepted/written across the proxy path, so framing, retries, and retransmitted acoustic cells do not inflate it.

The 10 kbps-class values seen in calibration and a 1–2.4 kbps Internet-goodput result are therefore not contradictory. Half-duplex turns, control frames, TCP dynamics, small packets, acoustic headers/training, conservative PHY backoff, and repair idle time consume the gap.

Domain annotations are learned opportunistically from observed DNS A/AAAA results. Missing names, stale names, shared-CDN ambiguity, and CNAME loss are expected. They are diagnostic convenience, not a security boundary.

Logs currently lack synchronized wall-clock timestamps across the two machines. Correlate them by handshake session ID, direction, token sequence, batch/window ID, and packet details rather than adjacent line position alone.

## Physical evidence and log map

The following facts were proven at least once with real hardware:

- discovery and handshake complete in both directions;
- calibration/cache exchange works;
- DNS queries and responses cross the acoustic link;
- macOS routes ordinary traffic into utun;
- Linux forwards/NATs UDP;
- `curl google.com` completed after the utun fixes;
- `curl https://justinjackson.ca/words.html` completed after split TCP;
- the post-scheduling-fix Firefox path was reported working.

Useful historical files under `logs/`:

- `log_example_traffic.txt`: pre-firewall Apple/APNs/ESS burst and queue saturation; source material for the Messages allow list.
- `dns_attempt.txt`, `dns_activity.txt`, `log_more_dns.txt`, `log_dns_route.txt`: progression from resolver failure to the macOS route/utun-queue diagnosis.
- `log_checksum.txt`: old protocol-4 checksum investigation; useful mainly as a warning against conflating acoustic CRC with IP checksums.
- `tls_succ.txt`: ordinary-network comparison for a successful TLS request.
- `tls_fail.txt`, `tls_still_fail.txt`, `tls_another_fail.txt`, `tls_not_work.txt`, `tls_timeout.txt`: the reset investigation across multiple now-obsolete revisions.
- `am_fail.txt`: Amazon passed TLS but stalled later, helping disprove the idea that the defect was TLS-record-specific.
- `log_curl_work_firefox_fail.txt`: `curl` success followed by the original parallel Firefox failure; also contains roughly 2 kbps useful-goodput evidence.
- `log_firefox_still_fail.txt`: strongest evidence for the final deadline-safe, per-flow scheduling work.

These logs sometimes concatenate client and gateway output and use different wire formats. Confirm the header's protocol/proxy versions before comparing behavior. There is no saved final Firefox success trace from current head; the final evidence is the operator's report, not a log file.

## Test coverage and its limits

At handoff, the normal, warning-as-error, AddressSanitizer, and UndefinedBehaviorSanitizer suites had passed. The suite covers 24 modem/policy groups, the TCP relay integration test, and four paired-live scenarios. `make test` is the canonical entry point.

The paired-live harness uses the real state machine with fake audio and fake TUN endpoints. It exercises recorded-v2/worse impairments, startup loss, ACK/commit/window-cell loss, DNS saturation, cache behavior, and reconnection paths. The Linux relay also has a real endpoint integration test.

It does **not** prove:

- CoreAudio or ALSA device behavior;
- real desktop scheduling and audio-server jitter;
- macOS route/PF/SCDynamicStore behavior on every OS version;
- Linux iptables/nftables interaction on every distribution;
- actual APNs/Messages behavior;
- long acoustic soak reliability.

Sanitizer runs intentionally disable leak detection because the integration harness uses tracing/ptrace behavior that otherwise conflicts with LeakSanitizer. Preserve the existing documented test invocation rather than interpreting that setting as a product leak exemption.

## Current performance and next audio work

Historical wall-time useful-goodput observations include roughly 1.2 kbps in an earlier long run and about 2.0–2.4 kbps during later browser investigations. They came from different revisions, acoustic modes, traffic mixes, and session lengths. No honest current-head number should be quoted until a new controlled run is saved.

When audio work resumes, the highest-value sequence is:

1. Save synchronized gateway/client logs for a current-head successful DNS lookup, HTTPS `curl`, and one tiny Firefox page.
2. Run a 10–15 minute idle-plus-active soak and record useful goodput, header/CRC misses, selective repairs, cumulative-ACK retries, and reconnect count.
3. Physically test `--allow-messages` with an already signed-in Mac, first plain text in both directions, then explicitly decide whether attachments are in scope.
4. Profile acoustic wall time by category before changing the PHY: payload airtime, training/sync/gap, control/token time, empty waits, retry/repair time, and TCP relay idle time.
5. Improve expected goodput while holding the current reliability margin. Large FFTs, compression, and still-larger bodies were ideas, not validated solutions. TLS/application payload is usually already compressed or incompressible, and large bodies worsen latency and loss blast radius.
6. Add conservative live rate adaptation and cache fingerprinting for physical device/route changes.
7. Decide whether Linux is a required gateway or implement an equivalent split-TCP facility elsewhere.
8. Add IPv6 only after the IPv4 path has measurable regression targets.

Do not optimize against calibration bitrate alone. The product metric is sustained `internet-goodput` with bounded request latency and no reconnects.

## Boundary for starting video work

There is no existing `--light`/video path despite the old README language. The safest way to start video without destabilizing this working audio baseline is to reuse the medium-independent pieces and isolate the medium-specific ones.

Good candidates to reuse:

- Linux TUN/NAT and macOS utun route/DNS setup;
- the Linux split-TCP relay;
- traffic policy and DNS de-amplification;
- IP packet serialization and queueing concepts;
- per-flow batching, reverse-flow affinity, goodput accounting, and deadline budgeting;
- the typed live-wire envelope idea.

Do not directly reuse as if they were medium-neutral:

- `um_modem_config` and its cache schema;
- OFDM/FEC candidate search and audio SNR/EVM gates;
- acoustic turnaround constants and receive timeouts;
- the assumption that self-hearing forces half-duplex token ownership;
- any current protocol version without advertising the medium/capabilities.

A practical extraction boundary is a small byte-oriented medium interface that can:

```text
send a protected typed byte frame
receive a protected typed byte frame with a deadline
report negotiated maximum body and latency/reliability capabilities
flush/cancel medium state
```

Keep the current audio runner unchanged while proving the video implementation behind that boundary. Give video its own cache path, format, and version. Either add a medium/capability field to discovery or bump the live protocol so an audio endpoint and a video endpoint cannot accidentally complete discovery and then fail mysteriously.

Choose video duplex semantics explicitly. If video can genuinely send and receive concurrently, carrying over the acoustic token protocol unchanged would unnecessarily preserve half-duplex latency. If it is also half-duplex, reuse the scheduling concepts but retune all turn, ACK, and failure deadlines from measurements.

Finally, add audio regression coverage before extracting large pieces from `src/live.c`. That file's coupling makes a broad cleanup and a new medium at the same time unusually risky; a narrow adapter followed by incremental extraction is safer and easier to verify.

## Minimal resumption checklist

If audio work is picked up months later:

1. Build current head and run `make test`.
2. Confirm both machines run the same protocol version.
3. Delete both `calibration.config` files if hardware, routing, or room conditions changed.
4. Start Linux with `sudo -E` and verify the **opened** audio endpoints.
5. Start the Mac client, disable bypass interfaces for a controlled test, and verify utun route/DNS state.
6. Test DNS, one HTTPS `curl`, then a dedicated quiet Firefox profile.
7. Judge success by wall-time Internet goodput and reconnect/repair counts, not calibration bitrate or per-packet cumulative rates.
8. Preserve both endpoint logs with session IDs and exact commands whenever reporting a regression.
