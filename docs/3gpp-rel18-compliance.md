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
| AnalyticsInfo request | `NwdafSbiService::getAnalytics` in `src/nwdaf_sbi.cpp` · tests: `tests/test_3gpp_sbi.cpp` | TS 29.520 V18.14.0; OpenAPI `TS29520_Nnwdaf_AnalyticsInfo.yaml` 1.3.5: `GET /nnwdaf-analyticsinfo/v1/analytics` | Partially compliant | The spec `GET` is served. Each query parameter is validated against its official schema, and failures use the TS 29.500 causes (A.4). No analytics ID is advertised yet, so every `event-id` gets `400 MANDATORY_QUERY_PARAM_INCORRECT` with `supportedFeatures` (I-1). The non-standard `POST` still works and is marked deprecated. | H1.7 | Add the per-ID Rel-18 mappings (Step 3). |
| AnalyticsInfo response body | `handleGetAnalytics`, `handlePostAnalytics` | `AnalyticsData` (`start`, `expiry`, `timeStampGen`, `anaMetaInfo`, per-event info lists, `suppFeat`) | Non-compliant | Custom envelope: `validity` instead of `expiry`, `confidence` at envelope level, and `analData` instead of the per-event info lists. | H1.7 | Add a 3GPP adapter that maps the engine output to `AnalyticsData`. |
| Query parameter encoding | `getAnalytics` · test `tests/test_3gpp_sbi.cpp` "H1.7: malformed or schema-invalid optional query parameters are rejected" | Official artifact: `ana-req`, `event-filter` and `tgt-ue` are declared `content: application/json` | Compliant | — | H1.7 | — |
| Target UE (`tgt-ue`) | `supi` query / body field | `TargetUeInformation` (`anyUe`, `supis`, `gpsis`, `intGroupIds`) | Non-compliant | Only a single custom `supi` parameter. GPSI and group targets aren't supported. | H1.7 | Implement `TargetUeInformation`. Handle unsupported target forms with the operation-specific failure semantics (§5). |
| Event filter (`event-filter`) | `dnn`/`snssai` body check | `EventFilter` (`anySlice`, `snssais`, `dnns`, `networkArea`, `nfInstanceIds`, …) | Partially compliant | Only a single served DNN and S-NSSAI are checked. The other filters aren't recognised. | H1.7, H1.2 | Validate the whole `EventFilter` against the schema. Apply the filters we support; handle the rest with the failure semantics. |
| Analytics context retrieval | — | `GET /nnwdaf-analyticsinfo/v1/context` | Optional | Not implemented. | — | Deferred until after M1. |
| No-data response | — | TS 29.520 V18.14.0 §4.3.2.2 (verify clause): "If the requested NWDAF Analytics data does not exist, the NWDAF shall respond with 204 No Content" | Not implemented | — | H1.7 | Return `204` when there is no analytics data for the request. This is distinct from requested *statistics* whose input data is unavailable (§5, A.2). |

## 2. Nnwdaf_EventsSubscription (TS 29.520)

| Capability | Code | 3GPP reference | Current status | Rel-18 gap | Roadmap item | Required action |
|---|---|---|---|---|---|---|
| Subscribe | `NwdafSbiService::createSubscription` / `evaluateSubscription` · tests: `tests/test_3gpp_sbi.cpp` | OpenAPI `TS29520_Nnwdaf_EventsSubscription.yaml` 1.3.3: `POST /nnwdaf-eventssubscription/v1/subscriptions`, returning `201` with a required `Location` header | Partially compliant | Served on the spec path. The body is checked in order: the official schema; the prose rules (`notificationURI`, `BOTH_STAT_PRED_NOT_ALLOWED`, the notification-method default and supersession, `repetitionPeriod`); then capability, with `failEventReports` and I-3. The `201` path (storage, `Location`, immediate report) arrives with the first subscribable analytics ID. | H1.7 | Step 3: store Rel-18 subscriptions and return `201`. The operator resource is unchanged. |
| Subscription body | `src/nwdaf_subscription.cpp` | `NnwdafEventsSubscription` (required `eventSubscriptions`; also `evtReq`, `notificationURI`, `notifCorrId`, `supportedFeatures`, `eventNotifications`, `failEventReports`, `prevSub`, `consNfInfo`) | Non-compliant | There's a single event, and only `repPeriod` and `maxReportNbr` are kept. | H1.7 | Store and handle the full request. Validate it schema-first. |
| Modify subscription | `modifySubscription` · test `tests/test_3gpp_sbi.cpp` | `PUT …/subscriptions/{subscriptionId}` → `200` or `204` | Partially compliant | The route exists. It answers `404 SUBSCRIPTION_NOT_FOUND` because no Rel-18 subscription can exist yet. | H1.7 | Step 3. |
| Unsubscribe | `deleteSubscription` · test `tests/test_3gpp_sbi.cpp` | `DELETE …/subscriptions/{subscriptionId}` → `204` | Partially compliant | As for Modify. | H1.7 | Step 3. |
| Immediate report | — | `evtReq.immRep` → `eventNotifications` in the `201` body | Not implemented | — | H1.7 | Implement. |
| Reporting control | `src/nwdaf_notifier.cpp` | `ReportingInformation` in `evtReq`: `notifMethod`, `maxReportNbr`, `monDur`, `repPeriod`, … | Partially compliant | Only periodic reporting with a count limit. No `monDur` and no one-time reporting. | H1.7 | Implement `monDur` and one-time reporting. Handle other schema-valid options through the failure semantics. |
| Threshold reporting | — | `EventSubscription.notificationMethod = THRESHOLD`, `loadLevelThreshold`, `nfLoadLvlThds` | Optional | Not implemented. | after M1 | Handle through the failure semantics until implemented. |
| Subscription and analytics transfer | — | `/transfers` resource | Optional | Not implemented. | — | Deferred. |

## 3. Notifications

| Capability | Code | 3GPP reference | Current status | Rel-18 gap | Roadmap item | Required action |
|---|---|---|---|---|---|---|
| Notification body | `NwdafNotifier::deliver` | `NnwdafEventsSubscriptionNotification` (required `subscriptionId`, plus `eventNotifications[]` whose `EventNotification` requires `event`), `notifCorrId` | Non-compliant | Custom body `{subId, notifId, analyticsId, ts, analData}`. | H1.7 | Emit the spec body for 3GPP subscriptions. Leave operator subscriptions unchanged. |
| Callback header | — | TS 29.500 V18.10.0: `3gpp-Sbi-Callback` header on notification requests (clause: Requires verification) | Not implemented | — | H1.7 | Send the header. |
| Target and filters applied to reports | `deliver` calls `compute(id)` | EventSubscription targeting and filters | Non-compliant | Notifications ignore the subscription's target and filters. | H1.7 | Pass the target and filters into the analytics computation. |
| Per-event reporting failure | — | `EventNotification.failNotifyCode` (`NwdafFailureCode`), `rvWaitTime`. TS 29.520 V18.14.0 Table 5.1.6.2.5 NOTE 2: `UNAVAILABLE_DATA` is allowed in `failNotifyCode` only when feature `StatisticsFailure` is supported, and `PREDICTION_NOT_ALLOWED` / `BOTH_STAT_PRED_NOT_ALLOWED` never apply there | Not implemented | — | H1.7 | Implement, gated on the negotiated `StatisticsFailure` feature (A.3). |

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
failure produces the operation-specific analytics failure (§5) instead. **No ID is
currently advertised through a 3GPP interface.**

| Capability | Code | 3GPP reference | Current status | Rel-18 gap | Roadmap item | Required action |
|---|---|---|---|---|---|---|
| ID spelling: QoS sustainability | `NwdafAnalyticsCatalogue` in `src/nwdaf_analytics_catalogue.cpp` · tests: `tests/test_analytics_catalogue.cpp` "H1.7: every operator-API ID is a Rel-18 NwdafEvent value" and "H1.7: legacy spellings map to Rel-18 IDs on the operator API" | `NwdafEvent` `QOS_SUSTAINABILITY` | Compliant | None for the spelling. The 3GPP interface itself is tracked in §1–§3. | H1.4 (adjusted) | Done. `QOS_SUSTAINABILITY` is canonical. `QoS_SUSTAINABILITY` is accepted only as an operator-API input alias, and persisted subscriptions are migrated. |
| ID spelling: redundant transmission experience | `NwdafAnalyticsCatalogue` · tests: as above, plus `tests/test_server_integration.cpp` "Integration: H1.4 REDUNDANT_TRANSMISSION still accepted, answered as RED_TRANS_EXP" | `NwdafEvent` `RED_TRANS_EXP` | Compliant | None for the spelling. | H1.4 (adjusted) | Done. `RED_TRANS_EXP` is canonical, and `REDUNDANT_TRANSMISSION` is an operator-API alias. |
| Governance of the known, implemented and advertised ID sets | `NwdafAnalyticsCatalogue` (`KNOWN_REL18`, `OPERATOR_IDS`, `REL18_IMPLEMENTED`, `rel18Advertised(cfg)`) · test: `tests/test_analytics_catalogue.cpp` "H1.7: advertised ⊆ implemented ⊆ known" | TS 29.510 V18.11.0 `NwdafInfo.nwdafEvents` | Partially compliant | The catalogue and its invariants are in place. `REL18_IMPLEMENTED` is still empty, and the NRF profile doesn't yet use `rel18Advertised`. | H1.7, H1.9 | Add each ID to `REL18_IMPLEMENTED` together with its conformance test. Build `nwdafInfo` from `rel18Advertised` (H1.9). |
| NF_LOAD | `nfLoad()` | TS 23.288 V18.13.0 §6.5 (verify); `NfLoadLevelInformation` requires `nfType` and `nfInstanceId`, plus at least one load measure | Non-compliant | Custom output, and no NF instance IDs. | H1.7, H1.9 | Map to `NfLoadLevelInformation`. Resolve instance IDs through NRF discovery, or through a configured map (an Open5GS deployment mechanism). |
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
| AnalyticsInfo failure semantics | — | TS 29.520 V18.14.0 Table 5.2.7.3-1 and the AnalyticsInfo request prose (A.2) | Not implemented | Today an unknown ID returns 422, and free-text causes are used. | H1.7 | Implement exactly A.2: `204` for no data; `400` `BOTH_STAT_PRED_NOT_ALLOWED`; `400` `PREDICTION_NOT_ALLOWED` (only with feature `PredictionError`); `500` `UNAVAILABLE_DATA` for unavailable past statistics; `500` with `ProblemDetailsAnalyticsInfoRequest` + `UNSATISFIED_REQUESTED_ANALYTICS_TIME` + `rvWaitTime` (only with `EneNA`); `403` `NO_ROAMING_SUPPORT` / `USER_CONSENT_NOT_GRANTED`. |
| Subscription failure semantics | — | TS 29.520 V18.14.0 Table 5.1.7.3-1 and the Subscribe/Update prose (A.2) | Not implemented | — | H1.7 | Implement exactly A.2. Partial acceptance: `201`/`200` carrying `failEventReports`. Whole request: `400` `BOTH_STAT_PRED_NOT_ALLOWED`; `400` `PREDICTION_NOT_ALLOWED` (with `PredictionError`); `500` `UNAVAILABLE_DATA`; `403` `NO_ROAMING_SUPPORT` / `USER_CONSENT_NOT_GRANTED`. |
| Rejection of unsupported functionality | `evaluateSubscription` · test `tests/test_3gpp_sbi.cpp` "H1.7: unsupported reporting requirements are rejected, not ignored" | Official schema and prose first, then operation-specific failure semantics (A.2, A.4) | Partially compliant | Implemented for the subscription-wide `evtReq` and for events. The per-event attribute capability tables arrive with each mapping in Step 3. | H1.7 | Step 3. |
| Features that weren't negotiated | `evaluateSubscription` · test `tests/test_3gpp_sbi.cpp` "H1.7: an EneNA-only attribute is ignored when EneNA is not supported (I-2)" | TS 29.500 V18.10.0 §6.6.2 and §5.2.7.2 (A.4) | Compliant | — (I-2) | H1.7 | Every attribute ignored under I-2 is listed in A.5. |
| Supported-features negotiation | `NwdafFeatureSet`, `NwdafSupportedFeatures` in `src/nwdaf_supported_features.cpp` · tests: `tests/test_supported_features.cpp`, `tests/test_3gpp_sbi.cpp` | TS 29.571 V18.12.0 `SupportedFeatures`; TS 29.520 V18.14.0 Tables 5.1.8-1 and 5.2.8-1; TS 29.500 §6.6.2 | Partially compliant | Implemented and tested: the encoding (against the TS 29.571 examples), the per-API tables (A.3), the local bitmask and the intersection. The local bitmask is derived from `rel18Advertised` and is currently `0`. `supportedFeatures` is returned in rejections. Returning `suppFeat` in `AnalyticsData` and `supportedFeatures` in `201` bodies arrives with the first advertised ID. S-1 remains open. | H1.7 | Step 3. |

## 6. Transport — TS 29.500

| Capability | Code | 3GPP reference | Current status | Rel-18 gap | Roadmap item | Required action |
|---|---|---|---|---|---|---|
| HTTP/2 on the SBI | cpp-httplib (HTTP/1.1) | TS 29.500 V18.10.0 §5.2 (verify) | Non-compliant | HTTP/1.1 only. | H1.8 | Add an nghttp2 server and a libcurl HTTP/2 client behind `NWDAF_USE_HTTP2`. |
| Compliance profile | — | — | Not implemented | — | H1.8 | With HTTP/2 enabled, the build is the `rel18-sbi` profile, which is eligible for the claim. With HTTP/2 disabled, it is the `dev-legacy` profile, which is explicitly transport non-compliant. The main CI conformance jobs must run over HTTP/2. |
| Interoperability with the Open5GS NRF | `registerWithNrf` in `src/main.cpp` | — | Requires verification | Open5GS SBI is HTTP/2-based, so registration over HTTP/1.1 is expected to fail against a real Open5GS NRF. | H1.8, H1.9 | Test manually and record the result under M1. |

## 7. NRF interaction — TS 29.510 V18.11.0

| Capability | Code | 3GPP reference | Current status | Rel-18 gap | Roadmap item | Required action |
|---|---|---|---|---|---|---|
| NF profile service list | `registerWithNrf` | `NFProfile.nfServiceList` (a map). `nfServices` is **deprecated**. | Non-compliant | Uses the deprecated `nfServices`. | H1.9 | Use `nfServiceList`. |
| Advertised services | `registerWithNrf` | `ServiceName` values `nnwdaf-analyticsinfo` and `nnwdaf-eventssubscription` | Partially compliant | Only `nnwdaf-analyticsinfo` is advertised. | H1.9 | Advertise both services. Each entry carries the local supported-features bitmask. |
| Service scheme | `registerWithNrf` | `NFService.scheme` | Non-compliant | Hard-coded to `http`, even when TLS is enabled. | H1.9 | Derive the scheme from the TLS setting. |
| `nwdafInfo` | — | `NwdafInfo` (`eventIds`, `nwdafEvents`, `taiList`, `taiRangeList`, `nwdafCapability`, `analyticsDelay`, `servingNfSetIdList`, `servingNfTypeList`, `mlAnalyticsList`) | Not implemented | Absent from the registered profile. `/health` shows a non-standard `analyticsIds` field. | H1.9 | Populate it **truthfully**: only the advertised events, and TAIs only when configured. Omit the other fields unless a real capability backs them. |
| Heartbeat | NRF heartbeat thread in `main.cpp` | NFUpdate by `PATCH` with `application/json-patch+json` | Partially compliant | Re-registers only after 3 misses. It should re-register as soon as the NRF says the profile is gone. | H1.9 | Re-register immediately on 404. |
| Deregistration | — | NFDeregister (`DELETE`) | Not implemented | — | H1.9 | Deregister on shutdown. |
| Update on capability change | — | NFUpdate | Not implemented | — | H1.9 | Send an explicit NFUpdate when a capability-affecting configuration change alters the advertised set. |
| NF discovery | — | `Nnrf_NFDiscovery` 1.3.4 | Not implemented | NF instance IDs are needed for NF_LOAD. | H1.9 | Look them up by NF type. A configured map is the Open5GS fallback. |

## 8. Security — TS 33.501 V18.12.0

| Capability | Code | 3GPP reference | Current status | Rel-18 gap | Roadmap item | Required action |
|---|---|---|---|---|---|---|
| TLS on the SBI | `NwdafServer` constructor (`httplib::SSLServer`) | §13.1 (verify) | Partially compliant | Server-side TLS works. Mutual authentication doesn't (next row). | H1.10 | — |
| Mutual TLS | `NwdafServer` constructor passes `tls_ca_file` to `httplib::SSLServer` · tests: `tests/test_sbi_security.cpp` ("H1.10: …", 5 cases) | §13.1 (verify) | Partially compliant | mTLS is implemented and tested on the HTTP/1.1 listener. It's opt-in: a non-empty `tls_ca_file` turns it on, and an unreadable CA stops startup. The HTTP/2 SBI listener doesn't exist yet. | H1.10, H1.8 | Apply the same client-CA verification to the HTTP/2 listener. |
| OAuth2 access-token validation | pre-routing handler in `src/nwdaf_server.cpp` | §13.4.1 (verify); TS 29.510 `AccessTokenClaims`; TS 29.520 OAuth2 scopes `nnwdaf-analyticsinfo` and `nnwdaf-eventssubscription` | Non-compliant | Only checks that a Bearer token is present. There's no signature, claim or scope validation. | H1.10 | Validate the signature and the `iss`, `sub`, `aud`, `exp` and per-service `scope` claims. Answer 401 or 403 with `WWW-Authenticate`. |
| Source of the token verification key | — | Implementation-specific; this is **not** a 3GPP mechanism | Not implemented | — | H1.10 | Add a key-provider interface. The initial Open5GS implementation reads a configured NRF public-key file, documented as a deployment mechanism. |
| NRF client server-certificate verification | `createHttpClient` in `src/main.cpp` | §13.1 (verify) | Partially compliant | Verification is on: against `tls_ca_file` when set, else the system trust store. When TLS is enabled the client also presents the NF certificate. No automated test yet. | H1.10, H1.9 | Cover it in `test_nrf_client` once the NRF client is extracted (H1.9). |

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
| Unsupported IDs aren't advertised; advertised ⊆ implemented ⊆ known; advertisement doesn't flap on transient failures; a real capability change sends an NFUpdate | `test_analytics_catalogue`, `test_nrf_client` | Open |
| Requests are validated against the official schema first. A schema-valid request we don't support gets the operation-specific failure semantics. | `test_3gpp_sbi`, `test_schema_validator` | Open |
| HTTP/2 conformance, over both h2c and TLS h2 | HTTP/2 suite in the main CI jobs | Open |
| Correct NRF registration, a truthful profile, and the full lifecycle | `test_nrf_client`, with a mock NRF over HTTP/2 | Open |
| ProblemDetails, application errors and the per-operation failure semantics | `test_3gpp_sbi` | Open |
| Supported-features negotiation | `test_supported_features`, `test_3gpp_sbi`, `test_nrf_client` | Open |
| mTLS and OAuth2 access-token validation | `test_sbi_security` | Open |
| Every Compliant row in this document names a passing test | Cross-check of the tests this document cites | Open |
| Manual interoperability with Open5GS, either validated or marked Requires verification | A dated record below | Open |
| Baseline items B-1 and B-2 resolved or explicitly carried forward | [`frozen-standards.md`](frozen-standards.md) | B-1 closed; B-2 open |

### Interoperability records

_None yet._

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
| **I-2** | An attribute tied only to optional features this NWDAF doesn't support is **ignored and logged**, as TS 29.500 requires for unknown content. An attribute relevant to a feature the NWDAF *does* support, but that isn't implemented, gets the operation-specific failure. Nothing is silently accepted. Attributes currently ignored under I-2: `evtReq.notifFlag` (EneNA). | TS 29.500 §6.6.2, §5.2.7.2 |
| **I-3** | Schema-valid but unsupported *subscription-wide* reporting requirements (`evtReq` members other than `immRep`, `notifMethod`, `maxReportNbr`, `monDur` and `repPeriod`; the `notifMethod` value `ON_EVENT_DETECTION`) → `400 OPTIONAL_IE_INCORRECT` with `invalidParams`. An unsupported *event* → a `failEventReports` entry with `failureCode` `OTHER`. The notification method `THRESHOLD`, which is the default, is also treated as an unsupported event until THRESHOLD reporting is implemented. When **no** event can be served → `400 MANDATORY_IE_INCORRECT` with `invalidParams`, rather than creating an empty subscription. | TS 29.500 Table 5.2.7.2-1; TS 29.520 §4.2.2.2.2 |

