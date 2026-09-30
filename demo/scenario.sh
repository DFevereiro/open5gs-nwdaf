#!/bin/bash
# Live demo walkthrough, run inside the demo container:
#   docker exec -it nwdaf-demo nwdaf-demo <step>
# Steps: status | ues [N] | traffic [SECONDS] | analytics | subscribe |
#        nf-outage | stop-ues | all
set -u

H2="curl -s --http2-prior-knowledge"
SBI="http://127.0.0.1:7780"
OPS="http://127.0.0.1:7779/nwdaf-analytics/v1"
NRF="http://127.0.0.10:7777"

say()  { printf '\n\033[1;36m== %s\033[0m\n' "$*"; }
urlq() { jq -rn --arg v "$1" '$v|@uri'; }
pause() { [ "${DEMO_NONINTERACTIVE:-0}" = 1 ] || read -rp "   (press Enter) " _; }

status() {
    say "Open5GS network functions (systemd)"
    for u in nrfd scpd amfd smfd upfd ausfd udmd udrd pcfd nssfd bsfd; do
        printf '  %-8s %s\n' "$u" "$(systemctl is-active open5gs-$u)"
    done
    printf '  %-8s %s\n' "gNB" "$(systemctl is-active ueransim-gnb)"
    printf '  %-8s %s\n' "NWDAF" "$(systemctl is-active open5gs-nwdafd)"
    say "NFs registered in the NRF (NFListRetrieval)"
    for t in AMF SMF AUSF UDM UDR PCF NSSF BSF SCP NWDAF; do
        # Open5GS nests totalItemCount inside _links (TS 29.510 has it at the top level).
        n=$($H2 "$NRF/nnrf-nfm/v1/nf-instances?nf-type=$t" | jq '._links.item | length')
        printf '  %-6s %s\n' "$t" "$n"
    done
    say "NWDAF health (operator API)"
    curl -s "$OPS/health" | jq '{status, sbiTransportProfile, oamSources: [.oamSources[] | {nfType, up, samples}]}'
}

ues() {
    local n="${1:-3}"
    say "Starting $n UERANSIM UE(s): registration + PDU session on slice SST 1"
    systemctl stop 'ueransim-ue@*' 2>/dev/null
    systemctl start "ueransim-ue@$n"
    for _ in $(seq 1 30); do
        [ "$(ip -o link show | grep -c uesimtun)" -ge "$n" ] && break
        sleep 1
    done
    ip -4 -o addr show | awk '/uesimtun/ {print "  " $2 "  " $4}'
    say "AMF / SMF view (journald)"
    journalctl -u open5gs-amfd -n 300 --no-pager | grep 'Registration complete' | tail -"$n" | sed 's/^/  /' 
    journalctl -u open5gs-smfd -n 300 --no-pager | grep -o 'UE SUPI\[[^]]*\] DNN\[[^]]*\] IPv4\[[^]]*\]' | tail -"$n" | sed 's/^/  /' 
}

traffic() {
    local secs="${1:-30}"
    say "UE traffic through the UPF for ${secs}s (iperf3 to 10.45.0.1 over each UE tunnel)"
    local i=0
    for ip in $(ip -4 -o addr show | awk '/uesimtun/ {split($4, a, "/"); print a[1]}'); do
        local port=$((5201 + i)) rate=$((10 + 10 * i))
        pgrep -f "iperf3 -s .* -p $port" >/dev/null || iperf3 -s -D -B 10.45.0.1 -p "$port" >/dev/null
        iperf3 -c 10.45.0.1 -p "$port" -B "$ip" -t "$secs" -b "${rate}M" >/dev/null 2>&1 &
        echo "  UE $ip → ${rate} Mbit/s"
        i=$((i + 1))
    done
    [ "$i" = 0 ] && { echo "  no UE tunnels: run 'nwdaf-demo ues' first"; return; }
    wait
    echo "  done"
}

analytics() {
    local any; any=$(urlq '{"anyUe":true}')
    say "3GPP Nnwdaf_AnalyticsInfo over HTTP/2 — NF_LOAD (NF status from the NRF, CPU from /proc)"
    $H2 "$SBI/nnwdaf-analyticsinfo/v1/analytics?event-id=NF_LOAD&tgt-ue=$any" | jq .
    say "Slice load level (LOAD_LEVEL_INFORMATION), I-9: max(UEs/5, PDU sessions/5) for SST 1"
    $H2 "$SBI/nnwdaf-analyticsinfo/v1/analytics?event-id=LOAD_LEVEL_INFORMATION&event-filter=$(urlq '{"anySlice":true}')" | jq .
    say "NSI_LOAD_LEVEL for S-NSSAI {sst:1}"
    $H2 "$SBI/nnwdaf-analyticsinfo/v1/analytics?event-id=NSI_LOAD_LEVEL&event-filter=$(urlq '{"nsiIdInfos":[{"snssai":{"sst":1}}]}')" | jq .
    say "NETWORK_PERFORMANCE for the served area (TAC 1): registered UEs and PDU session success ratio (I-11)"
    local area='{"tais":[{"plmnId":{"mcc":"999","mnc":"70"},"tac":"000001"}]}'
    $H2 "$SBI/nnwdaf-analyticsinfo/v1/analytics?event-id=NETWORK_PERFORMANCE&tgt-ue=$any&event-filter=$(urlq "{\"nwPerfTypes\":[\"NUM_OF_UE\",\"SESS_SUCC_RATIO\"],\"networkArea\":$area}")" | jq .
    say "UE_MOBILITY for the first UE: its TA and cell stays, from the AMF's UE list (I-12)"
    local supi; supi=$(curl -s "http://127.0.0.5:9090/ue-info" | jq -r '.items[0].supi // empty')
    [ -n "$supi" ] && $H2 "$SBI/nnwdaf-analyticsinfo/v1/analytics?event-id=UE_MOBILITY&tgt-ue=$(urlq "{\"supis\":[\"$supi\"]}")" | jq .
    say "Operator API (dashboard): UE_COMMUNICATION and NETWORK_PERFORMANCE"
    curl -s "$OPS/analytics?analyticsId=UE_COMMUNICATION" | jq '{analyticsId, confidence, analData}'
    curl -s "$OPS/analytics?analyticsId=NETWORK_PERFORMANCE" | jq '{analyticsId, confidence, analData}'
}

subscribe() {
    say "Nnwdaf_EventsSubscription: NF_LOAD + SLICE_LOAD_LEVEL, periodic, immediate report"
    local body='{"notificationURI":"http://127.0.0.1:9999/notify","evtReq":{"immRep":true},
      "eventSubscriptions":[
        {"event":"NF_LOAD","tgtUe":{"anyUe":true},"nfTypes":["AMF","SMF"],"notificationMethod":"PERIODIC","repetitionPeriod":30},
        {"event":"SLICE_LOAD_LEVEL","anySlice":true,"notificationMethod":"PERIODIC","repetitionPeriod":30},
        {"event":"SERVICE_EXPERIENCE","notificationMethod":"PERIODIC","repetitionPeriod":30}]}'
    local out; out=$($H2 -D - -H 'Content-Type: application/json' -d "$body" "$SBI/nnwdaf-eventssubscription/v1/subscriptions")
    echo "$out" | grep -i '^location:' | sed 's/^/  /'
    echo "$out" | sed -n '/^{/,$p' | jq '{failEventReports, eventNotifications}'
    local loc; loc=$(echo "$out" | awk -F': ' 'tolower($1)=="location" {print $2}' | tr -d '\r')
    [ -n "$loc" ] && { $H2 -X DELETE -o /dev/null -w "  DELETE → %{http_code}\n" "$loc"; }
}

nf_outage() {
    say "Stopping the SMF: the NRF drops it, NF_LOAD nfStatus follows"
    systemctl stop open5gs-smfd
    sleep 25
    $H2 "$SBI/nnwdaf-analyticsinfo/v1/analytics?event-id=NF_LOAD&tgt-ue=$(urlq '{"anyUe":true}')&event-filter=$(urlq '{"nfTypes":["SMF"]}')" | jq '.nfLoadLevelInfos'
    say "Restarting the SMF"
    systemctl start open5gs-smfd
}

stop_ues() { systemctl stop 'ueransim-ue@*'; say "UEs stopped"; }

case "${1:-all}" in
    status)    status ;;
    ues)       ues "${2:-3}" ;;
    traffic)   traffic "${2:-30}" ;;
    analytics) analytics ;;
    subscribe) subscribe ;;
    nf-outage) nf_outage ;;
    stop-ues)  stop_ues ;;
    all)
        status; pause
        ues 3; pause
        traffic 20; pause
        analytics; pause
        subscribe; pause
        nf_outage
        ;;
    *) echo "usage: nwdaf-demo {status|ues [N]|traffic [S]|analytics|subscribe|nf-outage|stop-ues|all}"; exit 2 ;;
esac
