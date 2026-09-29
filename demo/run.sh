#!/bin/bash
# Build and start the live demo: Open5GS v2.8.0 + UERANSIM + this NWDAF in one
# systemd container. Dashboard: http://localhost:${DEMO_PORT:-8080}
#   demo/run.sh           build (if needed) and start
#   demo/run.sh rebuild   rebuild the image, then start
#   demo/run.sh stop      stop and remove the container
set -euo pipefail
cd "$(dirname "$0")/.."
NAME=nwdaf-demo
PORT="${DEMO_PORT:-8080}"

if [ "${1:-}" = stop ]; then docker rm -f "$NAME" >/dev/null && echo "stopped"; exit 0; fi
if [ "${1:-}" = rebuild ] || ! docker image inspect "$NAME" >/dev/null 2>&1; then
    docker build -f demo/Dockerfile -t "$NAME" .
fi

docker rm -f "$NAME" >/dev/null 2>&1 || true
# --privileged: systemd as PID 1, the UPF's TUN device (ogstun), UE tunnels and NAT.
docker run -d --name "$NAME" --hostname nwdaf-demo --privileged --cgroupns=host \
    -v /sys/fs/cgroup:/sys/fs/cgroup:rw --tmpfs /run --tmpfs /run/lock \
    -p "$PORT:80" "$NAME" >/dev/null

echo -n "waiting for the core and the NWDAF"
for _ in $(seq 1 90); do
    if docker exec "$NAME" sh -c 'systemctl is-active -q open5gs-nwdafd ueransim-gnb && \
         curl -sf http://127.0.0.1:7779/nwdaf-analytics/v1/health >/dev/null' 2>/dev/null; then
        echo " — ready"
        echo "Dashboard:  http://localhost:$PORT"
        echo "Walkthrough: docker exec -it $NAME nwdaf-demo        (or: nwdaf-demo status|ues|traffic|analytics|subscribe|nf-outage)"
        exit 0
    fi
    echo -n "."; sleep 2
done
echo " — not ready after 3 minutes; see: docker exec $NAME systemctl --failed"
exit 1
