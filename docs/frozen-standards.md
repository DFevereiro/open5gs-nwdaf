# ONEmNWDAF — Frozen 3GPP Release 18 Baseline

Baseline date: 2026-09-28

```text
TS 23.288  V18.13.0  — NWDAF architecture, procedures and analytics
TS 29.500  V18.10.0  — 5GC Service-Based Architecture
TS 29.510  V18.11.0  — NRF services and NF profiles
TS 29.520  V18.14.0  — NWDAF services / Stage 3 APIs
TS 29.571  V18.12.0  — Common SBI data types / ProblemDetails
TS 33.501  V18.12.0  — 5GS security
TS 28.554  V18.9.0   — 5G performance measurements/KPIs where applicable
```

Every Release 18 compliance statement in this repository is measured against this
baseline. The main user of it is
[`3gpp-rel18-compliance.md`](3gpp-rel18-compliance.md). Official 3GPP/ETSI
specifications and their official OpenAPI artifacts are authoritative. Where this
repository's own `docs/openapi/*.yaml` disagrees with an official artifact, the
official artifact wins.

## Pinned official OpenAPI artifacts

| | |
|---|---|
| Repository | <https://forge.3gpp.org/rep/all/5G_APIs> |
| Tag | `TSG112-Rel18` |
| Commit | `d05657604fa16603ff5e0b6190220da2cdd2bf60` |
| Commit date | 2026-08-13 (the head of branch `REL-18` when the baseline was taken) |

| Artifact | `info.version` | Spec version stamped in the artifact | SHA-256 at the pinned commit |
|---|---|---|---|
| `TS29520_Nnwdaf_AnalyticsInfo.yaml` | 1.3.5 | TS 29.520 V18.13.0 (unchanged in V18.14.0; see B-1) | `1b8d480a45721f7bfef6db7733c189528480dd33a6e31bcab64abcf6ded2f239` |
| `TS29520_Nnwdaf_EventsSubscription.yaml` | 1.3.3 | TS 29.520 V18.13.0 (unchanged in V18.14.0; see B-1) | `6603c6af385accee815a5bbc76152fb52afbed66167ced9f0ac96f1a9ebe7dee` |
| `TS29510_Nnrf_NFManagement.yaml` | 1.3.4 | TS 29.510 V18.11.0 | `1f0aba0b0234637a0ab876d690cdb30384b11f0fbaf44d9f7447f1fe27acfbbe` |
| `TS29510_Nnrf_NFDiscovery.yaml` | 1.3.4 | TS 29.510 V18.11.0 | `cb55d9118634dfa888e6b2574daf80bbfb70ee0048ca77ecda68a999a25bce96` |
| `TS29510_Nnrf_AccessToken.yaml` | 1.3.1 | TS 29.510 V18.8.0 (the artifact hasn't changed since then) | `6878a7dc76a0cb8f14d18b98b6727412c0bf68f60fe94ec7ed9b22daaeafd36c` |
| `TS29571_CommonData.yaml` | 1.5.5 | TS 29.571 V18.12.0 | `627c6b93149ef59ee4bb70a634aee21efaca5ec8cb3ed5f123d62b90d8428328` |

TS 23.288, TS 29.500, TS 33.501 and TS 28.554 are prose specifications with no
OpenAPI artifact. Requirements taken from them are cited by clause against the
versions above.

The checksums were taken on 2026-09-28. The build fetches these files at the
pinned commit, and the build and tests use only these files.

## Open baseline items

### B-1 — TS 29.520 version: **Closed 2026-09-28**

The baseline names **TS 29.520 V18.14.0**. The pinned artifacts stamp the
Nnwdaf_AnalyticsInfo and Nnwdaf_EventsSubscription APIs as **V18.13.0**.

**Resolution.** This was checked against the published document, ETSI TS 129 520
V18.14.0 (2026-07), `https://www.etsi.org/deliver/etsi_ts/129500_129599/129520/18.14.00_60/`:

- Annex A of V18.14.0 itself declares the same versions as the pinned artifacts:
  - Nnwdaf_EventsSubscription 1.3.3, stamped "V18.13.0";
  - Nnwdaf_AnalyticsInfo 1.3.5, stamped "V18.13.0";
  - Nnwdaf_DataManagement 1.1.5, stamped "V18.13.0";
  - Nnwdaf_MLModelMonitor 1.0.2, stamped "V18.13.0";
  - Nnwdaf_MLModelTraining 1.0.5, the only one stamped "V18.14.0".
- The pinned commit carries exactly these stamps, including the MLModelTraining
  1.0.5 / V18.14.0 artifact.
- The V18.14.0 change history (CT#112) lists four CRs:
  - CR 1216, "Wrong attribute names for DN performance analytics";
  - CR 1218, "Location Granularity corrections";
  - CR 1228, "Correction to Nwdaf ML Model Training notifications";
  - CR 1235, "Update of info and externalDocs fields".

  Only the MLModelTraining artifact was restamped.

The pinned commit is therefore the **V18.14.0 OpenAPI**. The AnalyticsInfo and
EventsSubscription artifacts are unchanged since V18.13.0. **No amendment is
needed**, and compliance statements cite TS 29.520 V18.14.0 for both the prose and
the OpenAPI.

**Source of the normative prose.** The 3GPP archive (`3gpp.org/ftp`) refuses
automated access (HTTP 403). The reference text for this repository's prose citations is therefore the ETSI
publication of the same version:
- ETSI TS 129 520 for TS 29.520.
- The same numbering scheme for the other baseline specifications, for example
  ETSI TS 129 500 for TS 29.500 and ETSI TS 133 501 for TS 33.501.

### B-2 — TS 29.510 AccessToken artifact: **Closed 2026-09-28**

`TS29510_Nnrf_AccessToken.yaml` is stamped V18.8.0, which is older than the
V18.11.0 baseline. Annex A of ETSI TS 129 510 V18.11.0 (2026-03) itself declares
the "NRF OAuth2" API as version `1.3.1`, stamped "3GPP TS 29.510 V18.8.0". That
matches the pinned artifact: the AccessToken API has not changed since V18.8.0.
No amendment is needed.

## Change policy

This baseline is **immutable**:

- Newer revisions of these specifications or artifacts do **not** move the baseline
  automatically.
- A baseline change needs a dated **Amendment** section below. The amendment gives
  the new versions and the new commit, the reason for the change, and a full re-run
  of the compliance table in `3gpp-rel18-compliance.md`.
- CI and the build pin the exact commit above. Changing the pin without an amendment
  is a defect.

## Amendments

_None._
