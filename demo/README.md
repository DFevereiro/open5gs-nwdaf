# Live demo: NWDAF on an Open5GS core

One container runs a complete 5G standalone core and radio simulator, with the
NWDAF beside it the way it runs on a real host:

- **Open5GS v2.8.0** (official PPA): NRF, SCP, AMF, SMF, UPF, AUSF, UDM, UDR,
  PCF, NSSF and BSF as systemd units on their default loopback addresses, plus
  MongoDB.
- **UERANSIM v3.2.7**: a gNB and up to five UEs, which register, open PDU
  sessions on slice SST 1 and send traffic through the UPF.
- **This NWDAF** as the `open5gs-nwdafd` systemd unit (`rel18-sbi` build).
  It reads journald, `/proc`, `/sys`, MongoDB, the NFs' metrics endpoints and
  the NRF.
- **The dashboard**, served by nginx.

The demo subscribers use the test PLMN 999-70 and the public Open5GS/UERANSIM
test keys. They are not real identities.

## Run

Needs Docker and network access for the first build (about 10 minutes).

```bash
demo/run.sh                     # build if needed, start, wait until ready
docker exec -it nwdaf-demo nwdaf-demo      # guided walkthrough
```

Open the dashboard at <http://localhost:8080> (`DEMO_PORT` changes the port).
It loads React from a CDN, so the browser needs internet access.

`demo/run.sh stop` removes the container, and `demo/run.sh rebuild` rebuilds
the image after code changes.

The container runs **privileged**, because it needs systemd as PID 1, the
UPF's TUN device, the UE tunnels and NAT. Run it only on a machine where that
is acceptable.

## Walkthrough steps

Each step can also be run on its own with `docker exec -it nwdaf-demo nwdaf-demo <step>`.

| Step | What it shows |
|---|---|
| `status` | The NF units, what the NRF has registered (including the NWDAF), and the NWDAF's metrics sources. |
| `ues [N]` | N UEs (default 3) register and get PDU sessions, as shown by their tunnel interfaces and the AMF and SMF logs. |
| `traffic [S]` | Each UE sends iperf3 traffic through the UPF for S seconds (default 30), at 10, 20, 30… Mbit/s. |
| `analytics` | The Rel-18 interfaces over HTTP/2: NF_LOAD (NRF status and CPU), slice load level and NSI_LOAD_LEVEL for SST 1, then the operator API. |
| `subscribe` | A Rel-18 subscription with an immediate report. SERVICE_EXPERIENCE is refused in `failEventReports` because it isn't served in Rel-18 form. The subscription is deleted afterwards. |
| `nf-outage` | Stops the SMF. The NRF drops it and NF_LOAD `nfStatus` changes; then the SMF is restarted. |
| `stop-ues` | Detaches the UEs. |

With 3 UEs and the demo's slice capacity (5 UEs, 5 PDU sessions), the slice
load level reads 60 once the UEs have been attached for the demo's 30-second
statistics window (`slice_load_window_seconds`). See interpretation I-9 in
[`docs/3gpp-rel18-compliance.md`](../docs/3gpp-rel18-compliance.md).

## Troubleshooting

```bash
docker exec nwdaf-demo systemctl --failed
docker exec nwdaf-demo journalctl -u open5gs-nwdafd -n 50
docker exec nwdaf-demo journalctl -u ueransim-gnb -n 50
```
