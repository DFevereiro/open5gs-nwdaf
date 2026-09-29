#!/bin/sh
# Demo: provision the UERANSIM test subscribers (IMSI 999700000000001…005,
# public Open5GS/UERANSIM test K and OPc — not real credentials).
set -e
until mongosh --quiet --eval 'db.runCommand({ping: 1})' >/dev/null 2>&1; do sleep 1; done
for i in 1 2 3 4 5; do
    imsi="99970000000000$i"
    n=$(mongosh --quiet open5gs --eval "db.subscribers.countDocuments({imsi: '$imsi'})")
    [ "$n" = "0" ] && open5gs-dbctl add "$imsi" 465B5CE8B199B49FAA5F0A2EE238A6BC E8ED289DEBA952E4283B54E88E6183CA >/dev/null
done
echo "subscribers: $(mongosh --quiet open5gs --eval 'db.subscribers.countDocuments({})')"
