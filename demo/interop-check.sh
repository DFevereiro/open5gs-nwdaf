#!/bin/bash
# COMPAT-03: interoperability check against the live demo core (Open5GS +
# UERANSIM + this NWDAF, demo/run.sh). Two parts:
#  - format probes: the Open5GS outputs each workaround (O5GS-NN,
#    src/nwdaf_open5gs_compat.cpp) relies on are still there, so an Open5GS
#    change is caught before the analytics silently go wrong;
#  - end to end: the NWDAF's 3GPP answers match the core's own ground truth.
# Run from the host after demo/run.sh; exits 1 on a failure. CI: interop.yml.
set -uo pipefail

if [[ "${NWDAF_IN_CONTAINER:-0}" != 1 ]]; then
    exec docker exec -i -e NWDAF_IN_CONTAINER=1 "${DEMO_CONTAINER:-nwdaf-demo}" bash -s < "$0"
fi

AMF=http://127.0.0.5:9090
SMF=http://127.0.0.4:9090
NRF=http://127.0.0.10:7777
OPS=http://127.0.0.1:7779/nwdaf-analytics/v1
FAILED=0
ok()   { echo "ok    $1"; }
fail() { echo "FAIL  $1"; FAILED=1; }
check() {   # NAME JQ-FILTER JSON
    if jq -e "$2" >/dev/null 2>&1 <<<"$3"; then ok "$1"; else fail "$1"; echo "$3" | head -c 600 | sed 's/^/      /'; echo; fi
}
at() { date -u -d "$1" +%Y-%m-%dT%H:%M:%SZ; }

T0=$(at now)
echo "== starting 3 UEs"
nwdaf-demo ues 3 >/dev/null 2>&1
sleep 35   # a few collection intervals and one slice_load_window_seconds

echo "== format probes (Open5GS)"
check "O5GS-01 NFListRetrieval lists instances under _links.item" \
      '._links.item | length > 0' "$(curl -s --http2-prior-knowledge "$NRF/nnrf-nfm/v1/nf-instances?nf-type=AMF")"
metrics_smf=$(curl -s "$SMF/metrics")
grep -q '^fivegs_smffunction_sm_pdusessioncreationreq{plmnid="",snssai=""}' <<<"$metrics_smf" \
    && ok "O5GS-04 the SMF counts every session request once without labels" \
    || fail "O5GS-04 the SMF counts every session request once without labels"
grep -q '^fivegs_smffunction_sm_sessionnbr{plmnid="99970",snssai=' <<<"$metrics_smf" \
    && ok "O5GS-05 the SMF counts sessions per S-NSSAI" || fail "O5GS-05 the SMF counts sessions per S-NSSAI"
grep -q '^fivegs_amffunction_rm_registeredsubnbr{plmnid="99970",snssai=' <<<"$(curl -s "$AMF/metrics")" \
    && ok "O5GS-05 the AMF counts registered UEs per S-NSSAI" || fail "O5GS-05 the AMF counts registered UEs per S-NSSAI"
ue_info=$(curl -s "$AMF/ue-info")
check "O5GS-07 /ue-info lists the UEs with TAI, cell and timestamp" \
      '(.items | length) == 3 and all(.items[]; .location.timestamp > 0 and .location.nr_tai.tac_hex != null and .location.nr_cgi.nci != null)' \
      "$ue_info"
SUPI=$(jq -r '.items[0].supi' <<<"$ue_info")
UES=$(jq '[.items[] | select(.location.nr_tai.tac_hex == "000001")] | length' <<<"$ue_info")

echo "== the NWDAF"
health=$(curl -s "$OPS/health")
installed=$(dpkg-query -W -f='${Version}' open5gs 2>/dev/null | grep -oE '^[0-9]+\.[0-9]+\.[0-9]+')
check "the declared open5gs_version is the installed Open5GS ($installed)" \
      ".open5gs.version == \"$installed\"" "$health"
check "health: verified Open5GS, all sources up" '.open5gs.verified and all(.oamSources[]; .up)' "$health"
check "health: the demo's analytics are advertised" \
      '.rel18Analytics.advertised as $a | (["NF_LOAD","NETWORK_PERFORMANCE","NSI_LOAD_LEVEL","SLICE_LOAD_LEVEL","UE_MOBILITY"] - $a) == []' \
      "$health"
check "the NRF holds the NWDAF's registration" \
      '._links.item | length == 1' "$(curl -s --http2-prior-knowledge "$NRF/nnrf-nfm/v1/nf-instances?nf-type=NWDAF")"
check "NF_LOAD: the AMF's instance ID and NRF status" \
      'any(.nfLoadLevelInfos[]; .nfType == "AMF" and .nfInstanceId != null and .nfStatus.statusRegistered == 100)' \
      "$(nwdaf-cli get NF_LOAD --nf-type AMF 2>/dev/null)"
check "NUM_OF_UE for TA 1 equals the UEs the AMF lists there ($UES)" \
      ".nwPerfs[0].absoluteNum == $UES" \
      "$(nwdaf-cli get NETWORK_PERFORMANCE --nw-perf NUM_OF_UE --tai 999-70-1 --start "$(at '-10 sec')" 2>/dev/null)"
check "SESS_SUCC_RATIO is 100 % for the UEs' sessions" \
      '.nwPerfs[0].relativeRatio == 100' \
      "$(nwdaf-cli get NETWORK_PERFORMANCE --nw-perf SESS_SUCC_RATIO --tai 999-70-1 --start "$T0" 2>/dev/null)"
check "SLICE_LOAD_LEVEL is 3 of 5 UEs (60)" \
      '.sliceLoadLevelInfos[0].loadLevelInformation == 60' \
      "$(nwdaf-cli get SLICE_LOAD_LEVEL --snssai 1 --start "$(at '-10 sec')" 2>/dev/null)"
check "UE_MOBILITY for $SUPI: stays in TA 1" \
      '(.ueMobs | length) >= 1 and .ueMobs[-1].locInfos[0].loc.nrLocation.tai.tac == "000001"' \
      "$(nwdaf-cli get UE_MOBILITY --supi "$SUPI" 2>/dev/null)"
check "NSI_LOAD_LEVEL prediction carries a confidence" \
      '.nsiLoadLevelInfos[0].confidence != null' \
      "$(nwdaf-cli get NSI_LOAD_LEVEL --snssai 1 --start "$(at '+60 sec')" --end "$(at '+120 sec')" 2>/dev/null)"

[[ $FAILED == 0 ]] && echo "all interoperability checks passed" || { echo "--- NWDAF log"; journalctl -u open5gs-nwdafd -n 30 --no-pager; }
exit $FAILED
