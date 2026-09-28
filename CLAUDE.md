# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A standalone NWDAF (Network Data Analytics Function) daemon, Release 18 compliant for a documented supported scope (see below), `open5gs-nwdafd`, written in C++17. It sits beside an Open5GS 5G core and needs no core patches: it scrapes journald, `/proc`, `/sys` and MongoDB, runs native C++ ML, and serves analytics over a TS 29.520 SBI (HTTP/JSON on port 7779, which avoids Open5GS's 7777). Spec references: TS 23.288 (analytics IDs), TS 29.520 (Nnwdaf SBI), TS 29.510 (NRF), TS 33.501 (security).

## Build and test

CMake ≥ 3.22. cpp-httplib, nlohmann/json, yaml-cpp, spdlog and Catch2 are pulled by `FetchContent` at configure time, so the first configure needs network access. Don't install them from apt.

```bash
# Full build (needs libsystemd-dev, libssl-dev with OpenSSL >= 3.0, libsqlite3-dev)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DNWDAF_BUILD_TESTS=ON
cmake --build build --parallel $(nproc)

# Minimal build: no systemd or OpenSSL headers needed (use on non-systemd hosts or Ubuntu 20.04)
cmake -S . -B build -DNWDAF_USE_SD_JOURNAL=OFF -DNWDAF_USE_TLS=OFF

# All tests
cd build && ctest --output-on-failure

# Single test (ctest names = Catch2 TEST_CASE names, discovered via catch_discover_tests)
cd build && ctest -R "GET /health" --output-on-failure
# or run the Catch2 binaries directly with a name filter (wildcards allowed)
./build/tests/nwdaf_tests "H1.5:*"
./build/tests/nwdaf_integration_tests "Integration: GET /health returns 200 UP"

# Run locally
./build/open5gs-nwdafd --config config/nwdaf.yaml
curl http://127.0.0.1:7779/nwdaf-analytics/v1/health
```

**Newer toolchains fail on the pinned dependencies.** With GCC ≥ 16 or CMake ≥ 4, the pinned yaml-cpp 0.8.0 and spdlog (bundled fmt) fail under this project's flags: a missing `<cstdint>`, and `-Werror=dangling-reference`. CMake 4 also needs `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`. Verify on the CI platform instead, for example `docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/src:ro,Z ubuntu:22.04 …` (as your user, so build output is not root-owned), following the steps in `ci.yml`.

The build uses `-Wall -Wextra -Werror`, so any warning fails the build. Fix warnings; don't suppress them. CI (`.github/workflows/ci.yml`) builds the full and minimal profiles on Ubuntu 22.04 and 20.04 (20.04 always has TLS off), then builds the Docker image. A change must compile under both the full and the minimal flag sets.

## Architecture

Everything except `src/main.cpp` compiles into the static library `nwdaf_lib`, which the daemon and the tests both link. `main.cpp` wires the components together, then runs the heartbeat thread (TS 29.510 §5.3.2.4) and the SIGHUP hot-reload. NRF NFRegister, heartbeat and NFDeregister live in `NwdafNrfClient` (`src/nwdaf_nrf_client.cpp`), whose `profile()` is built from configuration only. It is tested against a mock NRF in `tests/test_nrf_client.cpp`.

Data flows through these components in order:

1. **`NwdafCollector`** (`src/nwdaf_collector.cpp`): a background thread (`bgLoop`) samples on each `collection_interval_seconds` tick:
   - NF health, from systemd units named through the `nf_service_names` map.
   - UPF throughput, read directly from `/sys/class/net/<iface>/statistics`. The gtp5g module bypasses user-space capture, so tcpdump and eBPF can't see this traffic.
   - AMF/SMF events, parsed from journald with `supi_regex`.
   - Subscriber count from MongoDB.

   It holds ring buffers of recent events and throughput, a stateful PDU-session set, and the EWMA predictors. The EWMA predictors live here, not in the engine, and are updated only by `bgLoop`. Raw OS access goes through **protected virtual** methods (`readJournalLines`, `readProcStat`, `readNetStats`, `querySubscriberCountFromMongo`, `collectNfLoad`, `getCpuNow`). That virtual layer is the test seam.
2. **`NwdafAnalyticsEngine`** (`src/nwdaf_analytics.cpp`): `compute(analytics_id, supi, start_ts, end_ts)` dispatches through an if-chain to one private method per analytics ID. It owns the 5-feature `IsolationForestN<5>` behind a `shared_mutex`, so `/train` and reads can run concurrently. It persists the model to `model_dir` with write-then-rename.
3. **`NwdafServer`** (`src/nwdaf_server.cpp`): routes are registered in `setupRoutes()`. The server is an `SSLServer` when `tls_enabled` is set. The same handler also has to guard OAuth2 auth and token-bucket rate limiting (`nwdaf_ratelimit.hpp`). On the 3GPP interfaces, OAuth2 access tokens are fully validated by `NwdafAccessTokenValidator` (`src/nwdaf_oauth.cpp`, TLS builds only), with the key supplied by a pluggable `NwdafTokenKeyProvider`. The operator API keeps a simple Bearer-presence check. Open5GS doesn't implement OAuth 2.0, so `oauth_enabled` stays off there. The server also serves Prometheus `/metrics` from a cached last-computed-value struct and the OpenAPI YAML from `openapi_spec_path`. The `/traffic/{start,stop,status}` simulator endpoints exist only for the dashboard and aren't in the OpenAPI spec.
4. **`NwdafSubscriptionStore`** (`src/nwdaf_subscription.cpp`) stores subscriptions, with an optional SQLite backend. **`NwdafNotifier`** is a push-delivery thread, compiled in only with `NWDAF_ENABLE_PUSH_DELIVERY`.
5. **ML** (`src/ml/`): Isolation Forest, the EWMA predictor, and the ITU-T G.107 E-model MOS estimator. All are dependency-free C++.

### Optional features are compile-time guarded

Code touching optional dependencies must be wrapped in the matching define and must degrade gracefully when the dependency is absent. For example, with no MongoDB the subscriber count is 0, and with no SQLite history stays in memory.

| Define | Guards | Set by |
|---|---|---|
| `NWDAF_USE_SD_JOURNAL` | journald reading and `sd_notify` | CMake option |
| `NWDAF_USE_TLS` / `CPPHTTPLIB_OPENSSL_SUPPORT` | TLS/mTLS | CMake option |
| `NWDAF_ENABLE_PUSH_DELIVERY` | `nwdaf_notifier.cpp` (only added to the sources when ON) | CMake option |
| `NWDAF_HAS_SQLITE` | history and subscription persistence | auto-detected |
| `NWDAF_HAS_MONGODB` | subscriber count | auto-detected |

### Adding or changing an analytics ID touches several places

- `src/nwdaf_analytics_catalogue.cpp` (`NwdafAnalyticsCatalogue`) holds the ID sets:
  - `KNOWN_REL18` is the official Rel-18 enum.
  - `OPERATOR_IDS` is what the operator API serves.
  - `REL18_IMPLEMENTED` and `rel18Advertised(cfg)` cover the 3GPP interfaces.

  An ID joins `REL18_IMPLEMENTED` only together with its official-schema conformance test. Advertisement depends on code and configuration only. `tests/test_analytics_catalogue.cpp` enforces `advertised ⊆ implemented ⊆ known` and `OPERATOR_IDS ⊆ KNOWN_REL18`.
- The `compute()` dispatch chain in `src/nwdaf_analytics.cpp`, and a private method in `include/nwdaf_analytics.hpp`.
- `src/nwdaf_server.cpp`: the GET and POST analytics handlers share `resolveOperatorId()`, but each has its own metrics-cache update. Change both.
- `docs/openapi/nwdaf-analytics-v1.yaml`: add the ID to the `AnalyticsId` enum, and add an `x-analyticsDataSchemas` entry mapping it to a response schema. `tests/test_openapi_conformance.cpp` requires the enum to equal `OPERATOR_IDS` exactly and requires every mapped schema to exist. It also validates each live GET and POST response against that schema, so if the spec and the code drift apart, CI fails.
- The README's 3GPP compliance table, and `docs/3gpp-rel18-compliance.md`.

IDs use the Rel-18 `NwdafEvent` spelling (`QOS_SUSTAINABILITY`, `RED_TRANS_EXP`). The legacy spellings `QoS_SUSTAINABILITY` and `REDUNDANT_TRANSMISSION` are accepted **only on the operator API**. `canonicalOperatorId()` rewrites them there, and the subscription store also canonicalizes on create and when it loads persisted rows.

## Testing

- `tests/mock_open5gs.*` defines `MockNwdafCollector`, which overrides the virtual OS-access methods to inject journal lines, net counters, `/proc` stats, NF metrics and a controllable CPU clock. It also re-exposes `appendThroughputSample` so window-based analytics (DISPERSION, REDUNDANT_TRANSMISSION) can be tested without running `bgLoop` in real time. To test anything new that touches the OS, add a virtual seam plus a mock override.
- Two binaries:
  - `nwdaf_tests`: unit tests, safe to run in parallel.
  - `nwdaf_integration_tests`: starts a real `NwdafServer` on port **17779**, serialized through a CTest `RESOURCE_LOCK`. Server and SBI tests belong here.
- The conformance test loads the OpenAPI spec from the source tree through the compile definition `NWDAF_OPENAPI_SPEC`.

## Conventions

- Naming:
  - Classes: `NwdafPascalCase`
  - Methods: `camelCase`
  - Members: trailing underscore (`config_`)
  - Constants: `UPPER_SNAKE`
- Headers go in `include/` (ML code in `include/ml/`); sources go in `src/`.
- Comments carry tracking tags such as `BUG-02`, `PROD-03`, `ARCH-05`, `COMP-01` and `H1.4`. The `H*` tags refer to the horizons in `docs/ENHANCEMENT_PLAN_5G_6G.md`, which is the roadmap and should be kept up to date as items land. Follow this tagging style, and cite the 3GPP clause when touching spec-defined behaviour.
- Deployment-specific values go in `config/nwdaf.yaml`: parse them in `src/nwdaf_config.cpp`, give them a default in `include/nwdaf_config.hpp`, and document them in the README config table. Never hard-code them. Only log level, collection interval, `ewma_alpha` and `anomaly_contamination` reload on SIGHUP; any other setting needs a restart.
- Keep the Open5GS-specific collection choices (the `nf_service_names` map, `supi_regex`, `/sys` throughput reads, validation that the network-performance weights sum to 1.0). Each one exists for a documented reason.
- Commits follow lightweight Conventional Commits (`feat(analytics): …`, `fix(collector): …`). Never commit real IMSI/SUPI values or credentials.

## 3GPP Release 18 work

- `docs/frozen-standards.md` is the **immutable** spec baseline. It pins the official 3GPP OpenAPI artifacts (forge.3gpp.org `5G_APIs`, commit `d05657604fa1`) and records open baseline items B-1 and B-2. Never change the versions or the pin silently. A baseline change needs a dated amendment in that file.
- `docs/3gpp-rel18-compliance.md` is the gap and status tracker.
  - A row may be marked **Compliant** only if it cites a passing test. `tests/test_compliance_doc.cpp` enforces this: every Compliant row must name existing `tests/*.cpp` files and `TEST_CASE` names (a trailing "…" is a prefix).
  - **M1 passed on 2026-09-28.** The claim "Release 18 compliant for the supported scope" covers exactly the scope table at the top of the compliance doc: both Nnwdaf services with NF_LOAD, HTTP/2, NRF lifecycle and discovery, mTLS and OAuth2, in the `rel18-sbi` build profile. Don't widen the claim without widening that table and its tests.
- The work is sequenced as H1.7–H1.10, then M1, in `docs/ENHANCEMENT_PLAN_5G_6G.md`.
- **Two SBI surfaces:**
  - `/nwdaf-analytics/v1/*` is the Open5GS **operator API** (dashboard, Prometheus). It is not a 3GPP interface; keep its behaviour stable.
  - The 3GPP interfaces (`/nnwdaf-analyticsinfo/v1`, `/nnwdaf-eventssubscription/v1`) must follow the official schemas.
- **Official artifacts are authoritative** over this repo's `docs/openapi/*.yaml`.
- **3GPP interface code:**
  - `src/nwdaf_sbi.cpp` (`NwdafSbiService`) handles the 3GPP interfaces independently of the transport. Requests are checked in order: official schema, then the prose rules, then the capability table.
  - `src/nwdaf_schema_validator.cpp` validates against the official artifacts in `openapi_3gpp_dir`.
  - `src/nwdaf_supported_features.cpp` holds the TS 29.571 bitmask and the per-API TS 29.520 feature numbers. The same feature has different bit numbers in each API.
  - Rules taken from the spec prose, and interpretations I-1..I-3, are pinned in the compliance doc's Appendix A. Cite them there instead of re-deriving them.
- **Official YAML files:** `cmake/Nwdaf3gppOpenApi.cmake` downloads the 106 official YAML files at configure time from the pinned commit and verifies them against `cmake/3gpp_openapi_manifest.cmake`, so configure needs network access. Set `-DNWDAF_3GPP_OPENAPI_SOURCE_DIR=<dir>` for an offline copy; it is still hash-checked. The files are "All rights reserved", so never commit them.
- **HTTP/2 (H1.8):**
  - The 3GPP interfaces are served by `NwdafH2Server` (nghttp2) on `sbi_h2_port` (7780). Port 7779 stays HTTP/1.1 for the operator API.
  - Outbound NRF traffic and Rel-18 notifications go through `NwdafHttpClient` (libcurl h2c/h2). Operator-API notifications stay on httplib.
  - `tests/test_3gpp_sbi.cpp` talks to the HTTP/2 listener when the build has `NWDAF_USE_HTTP2`, and asserts the protocol version. Test servers that don't need HTTP/2 set `sbi_h2_port = 0`.
  - The Open5GS NRF rejects HTTP/1.1, so a `NWDAF_USE_HTTP2=OFF` build cannot register with it.
- **Open5GS v2.8.0 limits:** no NF event-exposure services (`namf-evts` and `nsmf-event-exposure` are "Not implemented", and the UPF has no SBI), no OAuth 2.0, the NRF doesn't store `nwdafInfo`, the SMF applies no SM congestion control (so no §6.12 SMCCE data), there's no AF service data (so no §6.4 observed service experience), and PFCP usage reports carry only downlink volume per 100 MiB, sent to the SMF. These are verified; see the interoperability records. Data collection therefore stays on scraping for Open5GS.
- **Spec texts:** the 3GPP FTP archive refuses automated access. Use the ETSI publications (`etsi.org/deliver/etsi_ts/1295xx_…`, ETSI TS 1xx xxx = TS xx.xxx).
- **Advertisement** (NRF `nwdafInfo`) reflects implemented and configured capability only, never transient data availability.

## Other pieces

- `dashboard/`: a React + Recharts single-page app (`nwdaf_dashboard.jsx`) that is compiled in the browser by Babel standalone from `index.html`, with no build step. `dashboard/default` is an nginx site config that serves it and proxies `/nwdaf-analytics/` to port 7779.
- `grafana/nwdaf_dashboard.json`: a Grafana dashboard fed by `/metrics`.
- `systemd/open5gs-nwdafd.service`, plus `cmake --install`. The install puts the binary in `/usr/local/bin`, the config in `/etc/open5gs/`, and the OpenAPI YAML in `/etc/open5gs/openapi/`, which is the default `openapi_spec_path`.
- `nwdaf_cpp_setup_guide.md`: a long-form setup walkthrough for Ubuntu 20.04 and VS Code.
