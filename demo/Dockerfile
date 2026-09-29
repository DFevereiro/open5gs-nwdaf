# Live demo: an Open5GS v2.8.0 core, UERANSIM gNB + UEs and this NWDAF in one
# Ubuntu 22.04 container running systemd — the layout the NWDAF targets on a
# real host (systemd units, journald, /proc, /sys, MongoDB).
#
# Build from the repository root:  docker build -f demo/Dockerfile -t nwdaf-demo .
# Run with demo/run.sh (needs --privileged: systemd, the UPF's TUN device, NAT).

# ── NWDAF ────────────────────────────────────────────────────────────────────
FROM ubuntu:22.04 AS nwdaf
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake pkg-config git ca-certificates \
        libssl-dev libsystemd-dev libsqlite3-dev libnghttp2-dev libcurl4-openssl-dev \
    && rm -rf /var/lib/apt/lists/*
COPY . /src
RUN cmake -S /src -B /build -DCMAKE_BUILD_TYPE=Release -DNWDAF_BUILD_TESTS=OFF \
        -DNWDAF_REQUIRE_REL18_PROFILE=ON && \
    cmake --build /build -j"$(nproc)"

# ── UERANSIM ─────────────────────────────────────────────────────────────────
FROM ubuntu:22.04 AS ueransim
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git ca-certificates libsctp-dev lksctp-tools \
    && rm -rf /var/lib/apt/lists/*
RUN git clone --depth 1 --branch v3.2.7 https://github.com/aligungr/UERANSIM /ueransim && \
    cd /ueransim && make -j"$(nproc)"

# ── Demo host ────────────────────────────────────────────────────────────────
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive container=docker

RUN apt-get update && apt-get install -y --no-install-recommends \
        systemd systemd-sysv dbus iproute2 iptables iputils-ping curl ca-certificates \
        gnupg software-properties-common nginx iperf3 libsctp1 lksctp-tools \
        libssl3 libsqlite3-0 libnghttp2-14 libcurl4 nghttp2-client jq \
    && rm -rf /var/lib/apt/lists/*

# MongoDB (Open5GS UDR/PCF store; the NWDAF reads the subscriber count).
RUN curl -fsSL https://pgp.mongodb.com/server-7.0.asc | gpg --dearmor -o /usr/share/keyrings/mongodb-7.gpg && \
    echo "deb [signed-by=/usr/share/keyrings/mongodb-7.gpg] https://repo.mongodb.org/apt/ubuntu jammy/mongodb-org/7.0 multiverse" \
        > /etc/apt/sources.list.d/mongodb-org-7.0.list && \
    apt-get update && apt-get install -y --no-install-recommends mongodb-org && \
    rm -rf /var/lib/apt/lists/*

# Open5GS v2.8.0 from the official PPA.
RUN add-apt-repository -y ppa:open5gs/latest && apt-get update && \
    apt-get install -y --no-install-recommends open5gs && rm -rf /var/lib/apt/lists/* && \
    curl -fsSL -o /usr/local/bin/open5gs-dbctl \
        https://raw.githubusercontent.com/open5gs/open5gs/v2.8.0/misc/db/open5gs-dbctl && \
    chmod +x /usr/local/bin/open5gs-dbctl

COPY --from=ueransim /ueransim/build/nr-gnb /ueransim/build/nr-ue /ueransim/build/nr-cli \
     /ueransim/build/nr-binder /ueransim/build/libdevbnd.so /usr/local/bin/

# NWDAF, installed as the systemd unit shipped in the repository.
COPY --from=nwdaf /build/open5gs-nwdafd /usr/local/bin/open5gs-nwdafd
COPY --from=nwdaf /build/3gpp-openapi /etc/open5gs/openapi/3gpp
COPY docs/openapi/nwdaf-analytics-v1.yaml /etc/open5gs/openapi/nwdaf-analytics-v1.yaml
COPY systemd/open5gs-nwdafd.service /etc/systemd/system/open5gs-nwdafd.service
COPY demo/nwdaf.yaml /etc/open5gs/nwdaf.yaml
RUN mkdir -p /opt/nwdaf/models /var/log/open5gs && chown -R open5gs:open5gs /opt/nwdaf /var/log/open5gs && \
    usermod -aG systemd-journal open5gs

# Dashboard behind nginx (proxies /nwdaf-analytics/ to the operator API).
COPY dashboard/index.html dashboard/nwdaf_dashboard.jsx /var/www/html/
COPY dashboard/default /etc/nginx/sites-available/default

# RAN simulator and demo plumbing.
COPY demo/gnb.yaml demo/ue.yaml /etc/ueransim/
COPY demo/units/ /etc/systemd/system/
COPY demo/scenario.sh /usr/local/bin/nwdaf-demo
COPY demo/provision-subscribers.sh /usr/local/bin/demo-provision-subscribers
RUN chmod +x /usr/local/bin/nwdaf-demo /usr/local/bin/demo-provision-subscribers && \
    systemctl enable mongod open5gs-nwdafd nginx demo-net demo-subscribers ueransim-gnb && \
    systemctl mask systemd-networkd.service systemd-networkd-wait-online.service \
        systemd-resolved.service getty@tty1.service

STOPSIGNAL SIGRTMIN+3
CMD ["/sbin/init"]
