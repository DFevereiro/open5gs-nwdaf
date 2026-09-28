# 3GPP Release 18 Compliance Tracking

**Baseline:** [`frozen-standards.md`](frozen-standards.md). All spec versions and
official OpenAPI artifacts cited here come from that baseline. That means the
artifacts at commit `d05657604fa1` of the official 3GPP API repository. Item
**B-1** is closed: the pinned commit is the TS 29.520 V18.14.0 OpenAPI. Normative
prose is cited from the ETSI publication of the same version. Rules taken from the
prose are pinned in [Appendix A](#appendix-a--pinned-prose-facts-ts-29520-v18140).

**Current claim: none.** This NWDAF does **not** yet claim Release 18 compliance.
The target claim is **"Release 18 compliant for the supported scope"**, which may
be made only when milestone **M1** passes. The M1 criteria are listed
[at the end of this document](#m1--rel-18-supported-scope-compliance-gate).

**Roadmap:** work items refer to
[`ENHANCEMENT_PLAN_5G_6G.md`](ENHANCEMENT_PLAN_5G_6G.md). H1.1–H1.6 are the
existing items (issues #23–#28). H1.7–H1.10 were added for Rel-18 compliance.

## How to read this document

**Status values:**

| Status | Meaning |
|---|---|
| Compliant | Meets the requirement at the baseline. A row may be marked Compliant **only if** its Code cell names a passing test. |
| Partially compliant | Some of the requirement is met. The Rel-18 gap cell says what is missing. |
| Non-compliant | The capability exists, but it contradicts the requirement. |
| Not implemented | The capability doesn't exist. |
| Optional | Rel-18 optional functionality that is outside the targeted scope. |
| Out of scope | Deliberately not planned for this implementation. |
| Requires verification | Can't be decided from the pinned artifacts alone: it needs the spec prose, an interop test, or closing a baseline item. |

**Scope of this document.** Only the 3GPP-facing interfaces are assessed. The
operator API (`/nwdaf-analytics/v1/*`, used by the dashboard, Prometheus and
Grafana) is an Open5GS-specific surface. It is **not** a 3GPP interface and is not
assessed here.

**Clause numbers.** "(verify)" after a clause number means the number is carried
over from earlier project documentation and hasn't yet been checked against the
baseline version.

## 1. Nnwdaf_AnalyticsInfo (TS 29.520)

| Capability | Code | 3GPP reference | Current status | Rel-18 gap | Roadmap item | Required action |
|---|---|---|---|---|---|---|
| AnalyticsInfo request | `NwdafSbiService::getAnalytics` in `src/nwdaf_sbi.cpp` · tests: `tests/test_3gpp_sbi.cpp` | TS 29.520 V18.14.0; OpenAPI `TS29520_Nnwdaf_AnalyticsInfo.yaml` 1.3.5: `GET /nnwdaf-analyticsinfo/v1/analytics` | Partially compliant | The spec `GET` is served. Its query parameters are validated against the official schemas, and failures use the TS 29.500 causes (A.4). Only NF_LOAD is advertised; other IDs get `400 MANDATORY_QUERY_PARAM_INCORRECT` with `supportedFeatures` (I-1). The transport is HTTP/1.1 (§6). The non-standard `POST` still works, and is marked deprecated. | H1.7, H1.8 | Map further IDs; HTTP/2. |
| AnalyticsInfo response body | `nfLoadInfo` in `src/nwdaf_sbi.cpp` · test `tests/test_3gpp_sbi.cpp` "H1.7: NF_LOAD is served when nf_instance_ids is configured", which validates the body against the official `AnalyticsData` schema | `AnalyticsData` (`start`, `expiry`, `timeStampGen`, per-event info lists, `suppFeat`) | Partially compliant | Correct for NF_LOAD (`nfLoadLevelInfos`, `timeStampGen`, `expiry` = the next collection refresh, `suppFeat`). The other IDs aren't mapped yet. | H1.7 | Map further IDs. |
| Query parameter encoding | `getAnalytics` · test `tests/test_3gpp_sbi.cpp` "H1.7: malformed or schema-invalid optional query parameters are rejected" | Official artifact: `ana-req`, `event-filter` and `tgt-ue` are declared `content: application/json` | Compliant | — | H1.7 | — |
| Target UE (`tgt-ue`) | `interpretNfLoad` · test `tests/test_3gpp_sbi.cpp` "H1.7: NF_LOAD target UE rules" | `TargetUeInformation` (`anyUe`, `supis`, `gpsis`, `intGroupIds`) | Partially compliant | NF_LOAD: `anyUe` is served. `supis` → `400 MANDATORY_QUERY_PARAM_INCORRECT`, because the collectors don't observe which AMF or SMF instance serves a given UE. A missing target → `MANDATORY_QUERY_PARAM_MISSING`. | H1.7, H1.1 | Per-UE targets once H1.1 supplies serving-NF data. |
| Event filter (`event-filter`) | `interpretNfLoad` · tests `tests/test_3gpp_sbi.cpp` | `EventFilter` | Partially compliant | NF_LOAD: `nfInstanceIds`, `nfTypes` and `maxObjectNbr` are honoured. `nfSetIds` and `snssais` → `400 OPTIONAL_QUERY_PARAM_INCORRECT`. `networkArea` (NfLoadExt) and `listOfAnaSubsets` (EneNA) are ignored under I-2. | H1.7, H1.2 | Per ID as each is mapped. |
| Analytics context retrieval | — | `GET /nnwdaf-analyticsinfo/v1/context` | Optional | Not implemented. | — | Deferred until after M1. |
| No-data response | `nfLoadInfo` · test `tests/test_3gpp_sbi.cpp` "H1.7: NF_LOAD with no matching data is 204 No Content" | TS 29.520 V18.14.0 §4.3.2.2: "If the requested NWDAF Analytics data does not exist, the NWDAF shall respond with 204 No Content" | Compliant | — | H1.7 | — |

## 2. Nnwdaf_EventsSubscription (TS 29.520)

| Capability | Code | 3GPP reference | Current status | Rel-18 gap | Roadmap item | Required action |
|---|---|---|---|---|---|---|
| Subscribe | `createSubscription` / `evaluateSubscription` · tests `tests/test_3gpp_sbi.cpp` "H1.7: Subscribe to NF_LOAD → 201 with Location and the negotiated features", "… failEventReports" | OpenAPI `TS29520_Nnwdaf_EventsSubscription.yaml` 1.3.3: `POST …/subscriptions` → `201` + `Location`; §4.2.2.2.2 | Compliant | The operation's behaviour is compliant. The transport is tracked in §6, and the catalogue of analytics in §4. | H1.7 | — |
| Subscription body | `representation()` in `src/nwdaf_sbi.cpp`, `NwdafSubscriptionStore::createRel18` · tests `tests/test_3gpp_sbi.cpp` (every body is validated against the official schema) | `NnwdafEventsSubscription` | Compliant | The representation holds the accepted events, the negotiated `supportedFeatures` and `failEventReports`. EneNA-only attributes (`notifCorrId`, `evtReq.notifFlag`) are not echoed (I-2). | H1.7 | — |
| Modify subscription | `modifySubscription` · test `tests/test_3gpp_sbi.cpp` "H1.7: modify and delete an NF_LOAD subscription" | `PUT …/subscriptions/{subscriptionId}` → `200` with the representation | Compliant | — | H1.7 | — |
| Unsubscribe | `deleteSubscription` · test `tests/test_3gpp_sbi.cpp` "H1.7: modify and delete an NF_LOAD subscription" | `DELETE …/subscriptions/{subscriptionId}` → `204` | Compliant | Operator-API subscriptions aren't Rel-18 resources, so they get `404` here (tested). | H1.7 | — |
| Immediate report | `createSubscription`, `NwdafSbiService::eventReport` · test `tests/test_3gpp_sbi.cpp` "H1.7: immRep returns the available report in the 201 body" | `evtReq.immRep` → `eventNotifications` in the `201` body, "if available" | Compliant | — | H1.7 | — |
| Reporting control | `NwdafNotifier::deliverRel18` in `src/nwdaf_notifier.cpp` · tests `tests/test_3gpp_sbi.cpp` "H1.7: PERIODIC notifications repeat and stop at maxReportNbr", "H1.7: a subscription ends when monDur elapses", and the ONE_TIME notification test | `ReportingInformation`: `notifMethod`, `maxReportNbr`, `monDur`, `repPeriod` | Compliant | Implemented and tested: PERIODIC (per-event periods, with `evtReq` taking precedence), ONE_TIME, `maxReportNbr` and `monDur`. ON_EVENT_DETECTION and THRESHOLD are rejected (I-3). | H1.7 | — |
| Threshold reporting | — | `EventSubscription.notificationMethod = THRESHOLD`, `loadLevelThreshold`, `nfLoadLvlThds` | Optional | Not implemented. | after M1 | Handle through the failure semantics until implemented. |
| Subscription and analytics transfer | — | `/transfers` resource | Optional | Not implemented. | — | Deferred. |

## 3. Notifications

| Capability | Code | 3GPP reference | Current status | Rel-18 gap | Roadmap item | Required action |
|---|---|---|---|---|---|---|
| Notification body | `deliverRel18` · test `tests/test_3gpp_sbi.cpp` "H1.7: NF_LOAD notifications are Rel-18 NnwdafEventsSubscriptionNotification", which validates against the official schema | `NnwdafEventsSubscriptionNotification` | Compliant | Operator-API subscriptions keep their own format. | H1.7 | — |
| Callback header | `deliverRel18` · same test | TS 29.500 V18.10.0 §5.2.3.2.3: `3gpp-Sbi-Callback: Nnwdaf_EventsSubscription_Notify` | Compliant | — | H1.7 | — |
| Target and filters applied to reports | `NwdafSbiService::eventReport` · test `tests/test_3gpp_sbi.cpp` "H1.7: notifications apply the subscription's filters and omit events without data" | EventSubscription targeting and filters | Compliant | — | H1.7 | — |
| Per-event reporting failure | `deliverRel18` · same test | Table 5.1.6.2.5 NOTE 2 | Compliant | StatisticsFailure isn't supported, so `failNotifyCode` `UNAVAILABLE_DATA` doesn't apply. An event with no data is left out of that round, and this is tested. | H1.7 | — |

## 4. Analytics IDs and output data types

The Rel-18 `NwdafEvent` enum (OpenAPI at the pinned commit) has 21 values:
SLICE_LOAD_LEVEL, NETWORK_PERFORMANCE, NF_LOAD, SERVICE_EXPERIENCE, UE_MOBILITY,
UE_COMMUNICATION, QOS_SUSTAINABILITY, ABNORMAL_BEHAVIOUR, USER_DATA_CONGESTION,
NSI_LOAD_LEVEL, DN_PERFORMANCE, DISPERSION, RED_TRANS_EXP, WLAN_PERFORMANCE,
SM_CONGESTION, PFD_DETERMINATION, PDU_SESSION_TRAFFIC, E2E_DATA_VOL_TRANS_TIME,
MOVEMENT_BEHAVIOUR, LOC_ACCURACY, RELATIVE_PROXIMITY.

**Advertisement policy.** The NRF advertises an analytics ID only when two things
hold: its Rel-18 output is implemented, and the mandatory inputs are backed by
capability that is implemented **and configured** in the deployment. Advertisement
does not follow transient data availability. A temporary data-source or NRF
failure produces the operation-specific analytics failure (§5) instead. **Currently advertised through the 3GPP
interfaces: NF_LOAD, when `nf_instance_ids` is configured.**

| Capability | Code | 3GPP reference | Current status | Rel-18 gap | Roadmap item | Required action |
|---|---|---|---|---|---|---|
| ID spelling: QoS sustainability | `NwdafAnalyticsCatalogue` in `src/nwdaf_analytics_catalogue.cpp` · tests: `tests/test_analytics_catalogue.cpp` "H1.7: every operator-API ID is a Rel-18 NwdafEvent value" and "H1.7: legacy spellings map to Rel-18 IDs on the operator API" | `NwdafEvent` `QOS_SUSTAINABILITY` | Compliant | None for the spelling. The 3GPP interface itself is tracked in §1–§3. | H1.4 (adjusted) | Done. `QOS_SUSTAINABILITY` is canonical. `QoS_SUSTAINABILITY` is accepted only as an operator-API input alias, and persisted subscriptions are migrated. |
| ID spelling: redundant transmission experience | `NwdafAnalyticsCatalogue` · tests: as above, plus `tests/test_server_integration.cpp` "Integration: H1.4 REDUNDANT_TRANSMISSION still accepted, answered as RED_TRANS_EXP" | `NwdafEvent` `RED_TRANS_EXP` | Compliant | None for the spelling. | H1.4 (adjusted) | Done. `RED_TRANS_EXP` is canonical, and `REDUNDANT_TRANSMISSION` is an operator-API alias. |
| Governance of the known, implemented and advertised ID sets | `NwdafAnalyticsCatalogue` · tests: `tests/test_analytics_catalogue.cpp` "H1.7: advertised ⊆ implemented ⊆ known", "H1.7: advertisement follows configured capability, not runtime data" | TS 29.510 V18.11.0 `NwdafInfo.nwdafEvents` | Partially compliant | `REL18_IMPLEMENTED` = {NF_LOAD}. NF_LOAD is advertised only when `nf_instance_ids` is configured. The NRF profile doesn't yet use `rel18Advertised` (H1.9). | H1.7, H1.9 | Build `nwdafInfo` from `rel18Advertised` (H1.9). |
| NF_LOAD | `Nwdaf3gppAdapter::nfLoadLevelInfos`, `NwdafSbiService::interpretNfLoad` · tests `tests/test_3gpp_sbi.cpp` | TS 23.288 V18.13.0 §6.5 (verify); `NfLoadLevelInformation` requires `nfType`, `nfInstanceId`, and at least one load measure | Partially compliant | **Advertised when `nf_instance_ids` is configured or `nrf_nf_discovery` is enabled.** It reports `nfCpuUsage`: the measured CPU percentage of one core (I-5), for running NFs that have a configured ID. It does not report `nfStatus` (NRF registration status isn't observed) or memory, storage or peak load. Not supported: per-UE (`supis`), `nfSetIds`, `snssais`, predictions, and past statistics (`500 UNAVAILABLE_DATA`); thresholds belong to THRESHOLD reporting. | H1.7, H1.9 | Memory usage. |
| SERVICE_EXPERIENCE | `serviceExperience()` (E-model, H1.5) | §6.4 (verify); `ServiceExperienceInfo` requires `svcExprc` | Non-compliant | Custom output. The E-model supports it only at network level. | H1.7 | Map `mosScore` to `SvcExperience.mos`. Per-application and per-UE experience needs H1.3. |
| SM_CONGESTION | `smCongestion()` | §6.16 (verify); `SmcceInfo` requires `smcceUeList` with at least one of `highLevel`, `mediumLevel`, `lowLevel` | Non-compliant | Custom field names such as `highLevelCongestion`. | H1.7 | Rename to the spec fields through the adapter. |
| ABNORMAL_BEHAVIOUR | `abnormalBehaviour()` | §6.7.5 (verify); `AbnormalBehaviour` requires `excep` (an `Exception` with `excepId`) | Non-compliant | No `Exception` or `excepId` mapping. | H1.7; H2.7 improves attribution later | Map only the `excepId`s that can actually be detected. Handle the others through the failure semantics. |
| DISPERSION | `dispersion()` | §6.10 (verify); `DispersionInfo` requires `tsStart`, `tsDuration`, `disperCollects`, `disperType` | Non-compliant | Custom output. Data-volume dispersion (DVDA) needs per-UE volumes. | H1.7; H1.3 for DVDA | Map transaction dispersion (TDA). Handle DVDA through the failure semantics until H1.3. |
| NETWORK_PERFORMANCE | `networkPerformance()` | §6.6 (verify); `NetworkPerfInfo` requires `networkArea`, `nwPerfType`, and one of `relativeRatio` / `absoluteNum` | Non-compliant | The composite score isn't a Rel-18 `nwPerfType`, and there's no network area. | H1.7 | Map `SESS_SUCC_RATIO` and `NUM_OF_UE`. Advertise only when a served TAI list is configured. The composite score stays on the operator API. |
| QOS_SUSTAINABILITY | `qosSustainability()` | §6.9 (verify); `QosSustainabilityInfo` requires `qosFlowRetThd` or `ranUeThrouThd` | Non-compliant | A network-wide trend, not threshold-based. | H1.7; H1.3 for per-flow data | Support `ranUeThrouThd` crossing prediction when the consumer supplies `ranUeThrouThds`. |
| UE_MOBILITY | `ueMobility()` | §6.7.2 (verify); `UeMobility` requires `duration`, `locInfos`, and `ts` or `recurringTime` | Not implemented (in Rel-18 form) | No location data is collected. | H1.1 (adjusted: Namf_EventExposure `LOCATION_REPORT`) | Don't advertise it. Handle requests through the failure semantics until H1.1. |
| UE_COMMUNICATION | `ueCommunication()` | §6.7.3 (verify); `UeCommunication` requires `commDur`, `trafChar`, and `ts` or `recurringTime` | Not implemented (in Rel-18 form) | No per-UE traffic data. | H1.3 | Don't advertise it until H1.3. |
| RED_TRANS_EXP | `redundantTransmission()` | §6.12 (verify); `RedundantTransmissionExpInfo` requires `redTransExps` | Not implemented (in Rel-18 form) | The rate-stability proxy doesn't measure redundant transmission. There are no redundant-transmission sessions to observe. | H1.4 | Don't advertise it. The proxy stays on the operator API. |
| SLICE_LOAD_LEVEL | — | §6.3 (verify) | Optional | Not implemented. | H1.2 | After M1. |
| USER_DATA_CONGESTION, DN_PERFORMANCE | — | §6.8, §6.14 (verify) | Optional | Not implemented. Blocked on inputs. | H1.4, H1.1 | After M1. |
| PDU_SESSION_TRAFFIC, E2E_DATA_VOL_TRANS_TIME, MOVEMENT_BEHAVIOUR, LOC_ACCURACY, RELATIVE_PROXIMITY, PFD_DETERMINATION | — | Rel-18 `NwdafEvent` additions | Optional | Not implemented. | H1.4 (extended) | After M1. |
| WLAN_PERFORMANCE, NSI_LOAD_LEVEL | — | — | Out of scope | Depend on N3IWF / network slice instances, which aren't in the target deployment. | H1.4 | None. |

## 5. Errors, failure semantics and supported features

| Capability | Code | 3GPP reference | Current status | Rel-18 gap | Roadmap item | Required action |
|---|---|---|---|---|---|---|
| ProblemDetails format (3GPP interfaces) | `problem()` in `src/nwdaf_sbi.cpp` · test `tests/test_3gpp_sbi.cpp`, which validates every error body against the official TS 29.571 `ProblemDetails` schema | TS 29.571 V18.12.0 `ProblemDetails`; TS 29.500 V18.10.0 Table 5.2.7.2-1 causes | Compliant | The operator API keeps its own error format, which is not a 3GPP interface. | H1.7 | — |
| AnalyticsInfo failure semantics | `nfLoadInfo`, `getAnalytics` · tests `tests/test_3gpp_sbi.cpp` | TS 29.520 V18.14.0 Table 5.2.7.3-1 and §4.3.2.2 (A.2) | Partially compliant | Implemented and tested: `204`, `BOTH_STAT_PRED_NOT_ALLOWED` and `UNAVAILABLE_DATA`. `PREDICTION_NOT_ALLOWED` and `UNSATISFIED_REQUESTED_ANALYTICS_TIME` depend on features this NWDAF doesn't support (PredictionError, EneNA). `NO_ROAMING_SUPPORT` and `USER_CONSENT_NOT_GRANTED` are not implemented. | H1.7 | Roaming and user-consent handling if those scopes are taken on. |
| Subscription failure semantics | `evaluateSubscription` · tests `tests/test_3gpp_sbi.cpp` (including "H1.7: Subscribe asking for past NF_LOAD statistics is UNAVAILABLE_DATA") | TS 29.520 V18.14.0 Table 5.1.7.3-1 and §4.2.2.2.2 (A.2) | Partially compliant | Implemented and tested: `failEventReports`, `BOTH_STAT_PRED_NOT_ALLOWED` and `UNAVAILABLE_DATA`. `NO_ROAMING_SUPPORT` and `USER_CONSENT_NOT_GRANTED` are not implemented. | H1.7 | Roaming and user-consent handling if those scopes are taken on. |
| Rejection of unsupported functionality | `evaluateSubscription` · test `tests/test_3gpp_sbi.cpp` "H1.7: unsupported reporting requirements are rejected, not ignored" | Official schema and prose first, then operation-specific failure semantics (A.2, A.4) | Partially compliant | Implemented for the subscription-wide `evtReq` and for events. The per-event attribute capability tables arrive with each mapping in Step 3. | H1.7 | Step 3. |
| Features that weren't negotiated | `evaluateSubscription` · test `tests/test_3gpp_sbi.cpp` "H1.7: an EneNA-only attribute is ignored when EneNA is not supported (I-2)" | TS 29.500 V18.10.0 §6.6.2 and §5.2.7.2 (A.4) | Compliant | — (I-2) | H1.7 | Every attribute ignored under I-2 is listed in A.5. |
| Supported-features negotiation | `NwdafFeatureSet`, `NwdafSupportedFeatures` · tests: `tests/test_supported_features.cpp`, `tests/test_3gpp_sbi.cpp` (`suppFeat` and `supportedFeatures` in the `201` body) | TS 29.571 V18.12.0 `SupportedFeatures`; TS 29.520 V18.14.0 Tables 5.1.8-1 and 5.2.8-1; TS 29.500 §6.6.2 | Partially compliant | Negotiation is implemented and tested on both APIs. The local bitmask is NfLoad when `nf_instance_ids` is configured (`80` on AnalyticsInfo, `40` on EventsSubscription). Registering the features in the NRF profile is pending (H1.9). S-1 remains open. | H1.7, H1.9 | NRF profile. |

## 6. Transport — TS 29.500

| Capability | Code | 3GPP reference | Current status | Rel-18 gap | Roadmap item | Required action |
|---|---|---|---|---|---|---|
| HTTP/2 on the SBI | `NwdafH2Server` (`src/nwdaf_h2_server.cpp`, nghttp2) on `sbi_h2_port`; `NwdafHttpClient` (`src/nwdaf_http_client.cpp`, libcurl) for the NRF and Rel-18 notifications · tests: the whole `tests/test_3gpp_sbi.cpp` suite runs over HTTP/2 and asserts the protocol version on every response; "H1.8: the HTTP/2 listener does not accept HTTP/1.1"; `tests/test_sbi_security.cpp` "H1.8: h2 over TLS (ALPN) …" | TS 29.500 V18.10.0 §5.2 | Compliant (rel18-sbi profile) | h2c with prior knowledge, and h2 over TLS negotiated by ALPN. The operator API (port 7779) stays HTTP/1.1; it is not a 3GPP interface. For development, the 3GPP routes are also reachable over HTTP/1.1 on that port, but the NRF advertises the HTTP/2 endpoint only. | H1.8 | — |
| Compliance profile | `NwdafServer::transportProfile`; CMake `NWDAF_USE_HTTP2`, `NWDAF_REQUIRE_REL18_PROFILE`; CI 22.04 full job · test `tests/test_3gpp_sbi.cpp` "H1.8: /health reports the SBI transport profile" | — | Compliant | `rel18-sbi` (HTTP/2) or `dev-legacy` (HTTP/1.1, explicitly transport non-compliant). The profile is logged at startup and reported on `/health`. The main CI job fails to configure without HTTP/2 and TLS. | H1.8 | — |
| Interoperability with the Open5GS NRF | `registerWithNrf` and the heartbeat in `src/main.cpp`, over `NwdafHttpClient` | TS 29.510 V18.11.0 NFRegister, NFUpdate (heartbeat) | Compliant (transport) | Validated against Open5GS v2.8.0; see the interoperability records. The *content* of the NF profile is H1.9. | H1.8, H1.9 | — |

## 7. NRF interaction — TS 29.510 V18.11.0

| Capability | Code | 3GPP reference | Current status | Rel-18 gap | Roadmap item | Required action |
|---|---|---|---|---|---|---|
| NF profile service list | `NwdafNrfClient::profile` (`src/nwdaf_nrf_client.cpp`) · test `tests/test_nrf_client.cpp` "H1.9: the NF profile is a valid Rel-18 NFProfile with both Nnwdaf services", which validates it against the official `NFProfile` schema | `NFProfile.nfServiceList`; `nfServices` is deprecated | Compliant | — | H1.9 | — |
| Advertised services | `profile()` · same test | `ServiceName` values `nnwdaf-analyticsinfo` (API 1.3.5) and `nnwdaf-eventssubscription` (API 1.3.3) | Compliant | Both services are registered on the HTTP/2 endpoint. Each carries its own local supported-features bitmask when that bitmask is non-empty. | H1.9 | — |
| Service scheme | `profile()` · test `tests/test_nrf_client.cpp` "H1.9: scheme and oauth2Required follow the configuration" | `NFService.scheme` | Compliant | — | H1.9 | — |
| `nwdafInfo` | `profile()` · test `tests/test_nrf_client.cpp` "H1.9: the profile claims nothing that is not supported" | `NwdafInfo` | Compliant | Truthful: `nwdafEvents` and `eventIds` hold the advertised analytics only, and `nwdafInfo` is left out entirely when nothing is advertised. `taiList`, `nwdafCapability`, `analyticsDelay`, the serving-NF lists and `mlAnalyticsList` are omitted because nothing backs them. **Interop note:** the Open5GS v2.8.0 NRF doesn't store `nwdafInfo` (see the interoperability records). | H1.9 | — |
| Heartbeat | `NwdafNrfClient::heartbeat` · test `tests/test_nrf_client.cpp` "H1.9: heartbeat is an NFUpdate PATCH; 404 re-registers at once" | NFUpdate `PATCH` (`application/json-patch+json`) | Compliant | `404` → immediate NFRegister. Other failures retry with a capped backoff and re-register after three misses. | H1.9 | — |
| Deregistration | `NwdafNrfClient::deregister`, called on shutdown in `src/main.cpp` · test `tests/test_nrf_client.cpp` "H1.9: NFDeregister DELETEs the instance"; validated against Open5GS | NFDeregister (`DELETE`) | Compliant | — | H1.9 | — |
| Update on capability change | `profile()` depends on configuration only · test `tests/test_nrf_client.cpp` "H1.9: a transient NRF failure does not change the profile" | NFRegister (`PUT`) replaces the profile; NFUpdate | Compliant | Capability-affecting settings (`nf_instance_ids`, ports, TLS, OAuth) take effect only on restart, like the rest of the configuration. The restart registers again with `PUT`, which replaces the profile the NRF holds. Transient failures never change the advertisement. | H1.9 | — |
| NF discovery | `NwdafNfIdResolver` (`src/nwdaf_nf_id_resolver.cpp`), refreshed every `nrf_nf_discovery_interval_seconds` · tests `tests/test_nrf_client.cpp` "H1.9: discovery resolves an NF type only when exactly one instance is registered" and three related tests; validated against Open5GS | `Nnrf_NFDiscovery` 1.3.4: `GET /nnrf-disc/v1/nf-instances` with `target-nf-type` and `requester-nf-type` | Compliant | Opt-in with `nrf_nf_discovery`. Configured `nf_instance_ids` take precedence. A type is resolved only when exactly one instance is registered; with several, the locally measured one can't be told apart, so the type stays unresolved. A failed query keeps the last known ID. | H1.9 | — |

## 8. Security — TS 33.501 V18.12.0

| Capability | Code | 3GPP reference | Current status | Rel-18 gap | Roadmap item | Required action |
|---|---|---|---|---|---|---|
| TLS on the SBI | `NwdafServer` constructor (`httplib::SSLServer`) | §13.1 (verify) | Partially compliant | Server-side TLS works. Mutual authentication doesn't (next row). | H1.10 | — |
| Mutual TLS | `tls_ca_file` → `httplib::SSLServer` (operator port) and `NwdafH2Server` (HTTP/2 listener) · tests: `tests/test_sbi_security.cpp` ("H1.10: …" for HTTP/1.1; "H1.8: h2 over TLS (ALPN) with a client certificate from the CA" and "… refuses a client without a certificate" for HTTP/2) | §13.1 (verify) | Compliant | Opt-in: a non-empty `tls_ca_file` enforces client certificates on both listeners, and an unreadable CA stops startup. | H1.10 | — |
| OAuth2 access-token validation | `NwdafAccessTokenValidator` (`src/nwdaf_oauth.cpp`), applied in `NwdafServer::serve3gpp` to both listeners · tests: `tests/test_oauth.cpp` (11 cases), `tests/test_sbi_security.cpp` "H1.10: …" (7 end-to-end cases) | TS 33.501 V18.12.0 §13.4.1.0 (*"mandatory to support for NRF and NF"*) and §13.4.1.1.2 step 2; TS 29.510 V18.11.0 `AccessTokenClaims`; TS 29.500 V18.10.0 §6.7.3 | Compliant | Integrity is checked with RS256 or ES256 (NRF public key) or HS256 (shared secret), rejecting unsigned tokens. The claims are validated against the official schema. Then `sub` is checked against the client certificate's NF ID, `aud` against this NF instance or type, and `producerSnssaiList`, `producerNsiList`, `producerNfSetId` and `producerNfServiceSetId`, followed by `exp`, the per-service `scope`, and `analyticsIdList` (I-6). Responses: `401 Bearer realm` (no token); `401 … error="invalid_token"`, with `ACCESS_TOKEN_CLAIM_MISSING` when claims are missing; `403 … error="insufficient_scope", scope=…`. Notifications carry no token, as specified. The operator API keeps its presence check; it isn't a 3GPP interface. | H1.10 | — |
| Source of the token verification key | `NwdafTokenKeyProvider`: `NwdafPemKeyProvider` (`oauth_nrf_public_key_file`), `NwdafSharedSecretProvider` (`oauth_shared_secret_file`) · test `tests/test_oauth.cpp` "H1.10: OAuth without a verification key fails closed" | Implementation-specific; this is **not** a 3GPP mechanism | Compliant | This is a deployment mechanism behind an interface; other key sources can be added without touching the standards logic. Enabling OAuth without a key source, or in a build without TLS, stops startup. | H1.10 | — |
| NRF client server-certificate verification | `NwdafHttpClient` (libcurl `SSL_VERIFYPEER`/`VERIFYHOST`, `tls_ca_file` as CA, the NF certificate presented when `tls_enabled`) | §13.1 (verify) | Partially compliant | Implemented. There's no automated test against an https NRF yet. | H1.10, H1.9 | Cover it in `test_nrf_client` (H1.9). |

## 9. Data collection — TS 23.288 V18.13.0

| Capability | Code | 3GPP reference | Current status | Rel-18 gap | Roadmap item | Required action |
|---|---|---|---|---|---|---|
| Input data sources | `src/nwdaf_collector.cpp` | TS 23.288 data collection from NFs (`Nnf_EventExposure`) and OAM (clause: verify) | Partially compliant | Data comes from OAM-style, implementation-specific sources (journald, procfs, sysfs, MongoDB). There's no location, per-UE volume or S-NSSAI data. | H1.1 (adjusted), H1.2, H1.3 | Adjust H1.1 to name the Rel-18 input events each unblocked analytics ID needs. |
| DCCF, ADRF, historical analytics | — | TS 23.288 | Optional | Not implemented. | H2.5 | After M1. |
| ML model training, provisioning and monitoring; federated learning; accuracy | — | `nnwdaf-mlmodelprovision`, `nnwdaf-mlmodeltraining`, `nnwdaf-mlmodelmonitor`, `accuReq`/`accuInfo` | Optional | Not implemented. Not needed for an NWDAF that only does analytics (AnLF). | H2.1, H2.3, H2.6, H3.3 | After M1. |
| Roaming analytics | — | `nnwdaf-roaminganalytics`, `roamingInfo` | Out of scope | — | H3.6 | Handle `roamingInfo` requests through the failure semantics (for example `NO_ROAMING_SUPPORT` where that code applies). |
| User consent | — | TS 23.288 user consent for UE-level analytics (verify) | Optional | Not implemented. | — | Deferred. Document the deployment assumption. |

## 10. OpenAPI publication

| Capability | Code | 3GPP reference | Current status | Rel-18 gap | Roadmap item | Required action |
|---|---|---|---|---|---|---|
| Contract validated against the official artifacts | `NwdafSchemaValidator` (`src/nwdaf_schema_validator.cpp`); artifacts fetched and hash-verified by `cmake/Nwdaf3gppOpenApi.cmake` · tests: `tests/test_schema_validator.cpp`, `tests/test_3gpp_sbi.cpp` | Official artifacts at `d05657604fa1` (106 files) | Partially compliant | 3GPP requests and error bodies are validated against the official schemas, and `KNOWN_REL18` is tested against the official enum. Success responses will be validated as each ID is mapped (Step 3). The operator document is now checked with the same validator. | H1.6 (adjusted) | Step 3: an official-schema test for every advertised ID. |

## M1 — Rel-18 supported-scope compliance gate

M1 passes only when every criterion below is met. Each criterion needs a CI test
or a recorded manual check as evidence.

| Criterion | Evidence | Status |
|---|---|---|
| Conformant AnalyticsInfo and EventsSubscription APIs | `test_3gpp_sbi`, run over HTTP/2 | Open |
| Every advertised analytics ID produces output that validates against the official schemas | Official-schema suite in `test_openapi_conformance` | Open |
| Unsupported IDs aren't advertised; advertised ⊆ implemented ⊆ known; advertisement doesn't flap on transient failures; a real capability change is reflected in the NRF profile | `test_analytics_catalogue`, `test_nrf_client` | **Met** (capability changes are restart-only, and the restart re-registers with NFRegister) |
| Requests are validated against the official schema first. A schema-valid request we don't support gets the operation-specific failure semantics. | `test_3gpp_sbi`, `test_schema_validator` | Open |
| HTTP/2 conformance, over both h2c and TLS h2 | `tests/test_3gpp_sbi.cpp` over HTTP/2 in the Ubuntu 22.04 full CI job (`NWDAF_REQUIRE_REL18_PROFILE`); `tests/test_sbi_security.cpp` h2/TLS tests | **Met** |
| Correct NRF registration, a truthful profile, and the full lifecycle | `test_nrf_client` (a mock NRF over HTTP/2); Open5GS interop records | **Met**: register, heartbeat, `404` re-registration, deregister, and NF discovery |
| ProblemDetails, application errors and the per-operation failure semantics | `test_3gpp_sbi` | Open |
| Supported-features negotiation | `test_supported_features`, `test_3gpp_sbi`, `test_nrf_client` | Open |
| mTLS and OAuth2 access-token validation | `test_sbi_security`, `test_oauth` | **Met** |
| Every Compliant row in this document names a passing test | Cross-check of the tests this document cites | Open |
| Manual interoperability with Open5GS, either validated or marked Requires verification | A dated record below | NRF: **validated** (2026-09-28). Notification consumers: Requires verification (no Open5GS NF subscribes to NWDAF analytics). |
| Baseline items B-1 and B-2 resolved or explicitly carried forward | [`frozen-standards.md`](frozen-standards.md) | **Met**: both closed |

### Interoperability records

| Date | Peer | Topology | Result |
|---|---|---|---|
| 2026-09-28 | Open5GS NRF **v2.8.0** (`open5gs-nrf`, ppa:open5gs/latest, Ubuntu 22.04) | NRF and NWDAF in containers sharing one network namespace; NRF on `127.0.0.10:7777`, NWDAF 3GPP interfaces on `127.0.0.1:7780` (h2c) | **HTTP/1.1 NFRegister rejected** by the NRF (`nghttp2_session_mem_recv() failed (-903: Received bad client magic byte string)`): pre-H1.8 builds could not register. **With H1.8:** NFRegister `201 Created` over HTTP/2; NFUpdate heartbeat `204` every 3 s; the NRF lists the NWDAF with its HTTP/2 endpoint (port 7780); `GET /nnwdaf-analyticsinfo/v1/analytics` over h2c answered as specified. The profile the NRF stored shows the H1.9 gaps: only `nnwdaf-analyticsinfo`, `apiFullVersion` "1.0.0", no `nwdafInfo`, no supported features. |
| 2026-09-28 | Open5GS NRF **v2.8.0**, same topology | H1.9 client; `nf_instance_ids` set to a test value so that NF_LOAD is advertised | NFRegister `201` over HTTP/2. The NRF stores both services, `nnwdaf-analyticsinfo` (`apiFullVersion` 1.3.5) and `nnwdaf-eventssubscription` (1.3.3), each on the HTTP/2 endpoint (port 7780). Heartbeats `204` every 3 s. On SIGTERM the NWDAF sends NFDeregister: the NRF logs `NF de-registered`, and a later GET of the profile returns `404`. **Open5GS limitation:** the NRF does not store the sent `nwdafInfo` or the per-service `supportedFeatures`; the stored profile omits them. Consumers therefore can't discover this NWDAF by analytics ID through an Open5GS v2.8.0 NRF. |
| 2026-09-28 | Open5GS **v2.8.0** (NRF, and the SBI library shared by all Open5GS NFs) | Binary inspection and a standard `POST /oauth2/token` (Nnrf_AccessToken) to the NRF | **Open5GS does not implement OAuth 2.0.** Only its OpenAPI-generated data model (`libogssbi-openapi`) contains the AccessToken types. The NRF has no token endpoint (the request fails in its generic JSON parser with `400`), and Open5GS NFs never send tokens. So in Open5GS deployments `oauth_enabled` must stay `false`, which is the default. Token validation interop remains **Requires verification** with a 3GPP NRF that issues tokens. |
| 2026-09-28 | Open5GS NRF **v2.8.0**, same topology | NF discovery: a stand-in AMF profile registered with the NRF; NWDAF started with `nrf_nf_discovery: true` and no `nf_instance_ids` | The first refresh resolved the AMF's instance ID through `GET /nnrf-disc/v1/nf-instances?target-nf-type=AMF&requester-nf-type=NWDAF` over HTTP/2. The NRF's own instance was resolved too. |

## Appendix A — Pinned prose facts (TS 29.520 V18.14.0)

Taken from ETSI TS 129 520 V18.14.0 (2026-07) on 2026-09-28. These are
the rules H1.7 implements. Each rule's clause is given so it can be re-checked.

### A.1 Open specification items

| Item | Finding | Handling |
|---|---|---|
| **S-1** | The Nnwdaf_EventsSubscription data types give `"SMCCE"` as the applicability of the SM_CONGESTION data (`SmcceInfo`, `smccExps`, …; Table 5.1.6.1-1 and related). The Subscribe prose also says *"if the feature "SMCCE" is supported and the event is "SM_CONGESTION""*. However, **Table 5.1.8-1 defines no `SMCCE` feature bit**: bits 1–55 were checked. Nnwdaf_AnalyticsInfo does define `SMCCE` (Table 5.2.8-1, bit 15). | Treated as a specification inconsistency. SM_CONGESTION may be **advertised and served on AnalyticsInfo**. On EventsSubscription it can't be negotiated, so it is neither advertised nor subscribable there until the inconsistency is resolved. A schema-valid SM_CONGESTION subscription gets a `failEventReports` entry with `failureCode` `OTHER` (I-3). |

### A.2 Failure semantics

**Nnwdaf_AnalyticsInfo** (Table 5.2.7.3-1 and the request-handling prose):

| Condition | Response |
|---|---|
| The requested analytics data does not exist | `204 No Content` |
| Analytics target period has its start in the past and its end in the future | `400`, cause `BOTH_STAT_PRED_NOT_ALLOWED` |
| Prediction requested for `SM_CONGESTION` or `PDU_SESSION_TRAFFIC`, when `PredictionError` is supported | `400`, cause `PREDICTION_NOT_ALLOWED` |
| Past statistics requested but the necessary data is unavailable | `500`, cause `UNAVAILABLE_DATA` |
| `timeAnaNeeded` reached before the analytics are ready | `500` (*may*). With `EneNA`: `ProblemDetailsAnalyticsInfoRequest` with cause `UNSATISFIED_REQUESTED_ANALYTICS_TIME` and `rvWaitTime` |
| Roaming analytics required but not supported | `403`, cause `NO_ROAMING_SUPPORT` (feature `RoamingAnalytics`) |
| Required user consent not granted | `403`, cause `USER_CONSENT_NOT_GRANTED` |

Per the note to the table, including `ProblemDetails` with `cause` is optional
unless the operation's clause mandates it. Every row above except the 204 and the
`timeAnaNeeded` 500 is written as a mandate ("shall … including the cause").

**Nnwdaf_EventsSubscription** (Table 5.1.7.3-1 and the Subscribe/Update prose):

| Condition | Response |
|---|---|
| Created; not all events accepted | `201 Created` + `Location`; *may* include `failEventReports` |
| Updated; not all events modified | `200 OK`; *may* include `failEventReports`. Or `204 No Content` |
| Analytics target period has its start in the past and its end in the future | `400`, cause `BOTH_STAT_PRED_NOT_ALLOWED` |
| Prediction requested for `SM_CONGESTION`, `PFD_DETERMINATION` or `PDU_SESSION_TRAFFIC`, when `PredictionError` is supported | `400`, cause `PREDICTION_NOT_ALLOWED` |
| Past statistics requested but the necessary data is unavailable | `500`, cause `UNAVAILABLE_DATA` |
| UE location aggregation not allowed (UE Mobility) | `400`, cause `UE_AGGREGATION_NOT_ALLOWED` |
| Roaming not supported | `403`, cause `NO_ROAMING_SUPPORT` |
| User consent not granted | `403`, cause `USER_CONSENT_NOT_GRANTED` |
| `immRep` = true | Reports of the subscribed events, *if available*, in the `201` body |

**Notifications** (Table 5.1.6.2.5, NOTE 2): `failNotifyCode` may carry
`UNAVAILABLE_DATA` **only when `StatisticsFailure` is supported**, which in turn
requires `EneNA`. `PREDICTION_NOT_ALLOWED` and `BOTH_STAT_PRED_NOT_ALLOWED` never
apply to `failNotifyCode`.

### A.3 Supported-feature bits

Features are negotiated per TS 29.500 §6.6. Only the bits that relate to this
NWDAF's analytics and failure handling are listed. The full tables are in the
specification.

| Feature | Nnwdaf_EventsSubscription (Table 5.1.8-1) bit | Nnwdaf_AnalyticsInfo (Table 5.2.8-1) bit |
|---|---|---|
| ServiceExperience | 1 | 4 |
| UeMobility | 2 | 1 |
| UeCommunication | 3 | 2 |
| QoSSustainability | 4 | 5 |
| AbnormalBehaviour | 5 | 6 |
| NfLoad | 7 | 8 |
| NetworkPerformance | 8 | 3 |
| EneNA | 11 | 10 |
| SMCCE | **— (S-1)** | 15 |
| Dispersion | 18 | 17 |
| RedundantTransmissionExp | 19 | 18 |
| UserConsent | 25 | 23 |
| StatisticsFailure | 53 (requires EneNA) | — |
| RoamingAnalytics | 54 | 47 |
| PredictionError | 55 | 48 |

AnalyticsInfo bits 39 and 42 both came out of text extraction as
`QoSSustainExt_eNA`. This is **Requires verification** against the original
document. Neither bit is used by this NWDAF.

### A.4 TS 29.500 V18.10.0 rules applied

| Rule | Clause | Applied as |
|---|---|---|
| Unknown attributes and values shall be ignored. Attributes tied to a feature *"shall only be sent … if the corresponding feature is supported"* | §6.6.2 | I-2 |
| Unknown IEs *"may"* be discarded; the rest of the request shall be processed | §5.2.7.2 | I-2 |
| IEs or query parameters that don't comply with the schema *"should"* be rejected with 400 (e.g. `INVALID_MSG_FORMAT`), *"even when the failed IEs are defined as optional"* | §5.2.7.2 | Schema stage: `MANDATORY_IE_MISSING` / `INVALID_MSG_FORMAT` for bodies; `MANDATORY_` / `OPTIONAL_QUERY_PARAM_INCORRECT` for query parameters |
| Unsupported query parameters on a safe method (GET): ignore them, or reject | §5.2.9 a) | Ignored and logged |
| A method a resource doesn't support: `405` with an `Allow` header | §5.2.7.2 | All three 3GPP resources |
| `3gpp-Sbi-Callback` value is *"typically the name of the notify service operation"*. Omit `apiversion` for major version 1 | §5.2.3.2.3, Annex B | `Nnwdaf_EventsSubscription_Notify` (Step 3) |
| Supported features are the intersection of client and server, *"shall"* be included in the resource representation returned on creation, and apply to notifications | §6.6.2 | `NwdafFeatureSet::intersect` |
| Excessive request rate | Table 5.2.7.2-1 | `429`, cause `NF_CONGESTION_RISK`, plus `Retry-After` |

Also pinned from TS 29.520 V18.14.0:
- `notificationURI` *"shall be supplied … in the HTTP POST … and … PUT"* (Table 5.1.6.2.2-1).
- `repetitionPeriod` *"shall be supplied for notification method PERIODIC"* (Table 5.1.6.2.3-1).
- When `notificationMethod` isn't supplied, *"the default value is THRESHOLD"* (Table 5.1.6.2.3-1, NOTE 2).
- `evtReq.notifMethod` and `repPeriod` supersede the event's own method and period (§4.2.2.2.2, NOTE 1).

### A.5 Interpretations

Each of these is a reading of the specification, not a literal rule. Each one is
**Requires verification** through interoperability testing or clarification.

| Id | Interpretation | Basis |
|---|---|---|
| **I-1** | An analytics ID is *accepted* when the **NWDAF's own** feature set contains its gating feature. The consumer may know the NWDAF's features from the NRF, so it doesn't have to list them in `supported-features`. Requesting an event puts that event's own feature in effect for the resource, so its data isn't stripped. `suppFeat` / `supportedFeatures` still report the exact intersection, and the intersection gates the *other* optional behaviour (EneNA, StatisticsFailure, PredictionError, …). An analytics ID the NWDAF doesn't support gets `400 MANDATORY_QUERY_PARAM_INCORRECT` (AnalyticsInfo) or a `failEventReports` entry (subscriptions), and the error carries the NWDAF's `supportedFeatures`. | TS 29.500 §6.6.2; TS 29.520 V18.14.0 NwdafEvent applicability column |
| **I-2** | An attribute tied only to optional features this NWDAF doesn't support is **ignored and logged**, as TS 29.500 requires for unknown content. An attribute relevant to a feature the NWDAF *does* support, but that isn't implemented, gets the operation-specific failure. Nothing is silently accepted. Attributes currently ignored under I-2: `evtReq.notifFlag`, `notifCorrId`, `listOfAnaSubsets` and the EneNA members of `EventReportingRequirement` (`accPerSubset`, `offsetPeriod`, `timeAnaNeeded`, `histAnaTimePeriod`); `anaMeta` and `anaMetaInd` (Aggregation); and NF_LOAD `networkArea` (NfLoadExt). | TS 29.500 §6.6.2, §5.2.7.2 |
| **I-3** | Schema-valid but unsupported *subscription-wide* reporting requirements (`evtReq` members other than `immRep`, `notifMethod`, `maxReportNbr`, `monDur` and `repPeriod`; the `notifMethod` value `ON_EVENT_DETECTION`) → `400 OPTIONAL_IE_INCORRECT` with `invalidParams`. An unsupported *event* → a `failEventReports` entry with `failureCode` `OTHER`. The notification method `THRESHOLD`, which is the default, is also treated as an unsupported event until THRESHOLD reporting is implemented. When **no** event can be served → `400 MANDATORY_IE_INCORRECT` with `invalidParams`, rather than creating an empty subscription. | TS 29.500 Table 5.2.7.2-1; TS 29.520 §4.2.2.2.2 |
| **I-4** | `EventReportingRequirement.accuracy` is a *preferred* level of accuracy. This NWDAF produces one level, which it serves whatever preference is stated. That is best effort, as "preferred" permits. | TS 29.520 V18.14.0 Table 5.1.6.2.7-1 |
| **I-5** | `nfCpuUsage` is the measured CPU time of the NF's process as a percentage of one core, clamped to 0–100. Open5GS NFs are single-threaded, so this approximates the NF's CPU usage. `nfStatus`, which is the share of time the NF was *registered*, *unregistered* or *undiscoverable* in the NRF, is not reported, because the collectors observe systemd state and not NRF registration state. | TS 29.520 V18.14.0 Table 5.1.6.2.26-1 (verify clause) |
| **I-6** | When an access token carries `analyticsIdList`, a request for any analytics ID outside that list gets `403 insufficient_scope`: AnalyticsInfo for its `event-id`, subscriptions if any requested event is outside it. The TS 29.510 table requires the NRF to provide the list when the producer is an NWDAF AnLF, but the prose defines no producer-side check for AnLF analytics exposure (TS 33.501 Annex X.10 covers MTLF model sharing). | TS 29.510 V18.11.0 `AccessTokenClaims.analyticsIdList`; TS 33.501 Annex X.10 |
| **I-7** | The consumer's NF Instance ID is read from a `subjectAltName` URI of the form `urn:uuid:<id>` in its TLS client certificate. That follows the TS 33.310 NF certificate profile, which is outside the frozen baseline: **Requires verification**. With no such URI (or no client certificate) the `sub` check is skipped. | TS 33.501 §13.4.1.1.2 step 2 |

