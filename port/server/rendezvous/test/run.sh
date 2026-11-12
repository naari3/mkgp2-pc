#!/bin/sh
# Two copies of the no-graphics build, each behind its own NAT router
# (net.sh, run it first, as root), meet through a room.
#   ISO=/path/to/disc.iso port/server/rendezvous/test/run.sh SECONDS
# Logs: $OUT/h1.log, h2.log (default /tmp/mkgp2pc-room). Look for
# "room: cabinet N is at", "connect N: connected" and the traffic lines.
# EXTRA="NAME=VALUE" adds to both copies' environment.
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../.." && pwd)
OUT=${OUT:-/tmp/mkgp2pc-room}
mkdir -p "$OUT/h1" "$OUT/h2"
pkill -f fakestun.py; pkill -f rendezvous/src/local.js; sleep 0.3
ip netns exec inet python3 "$HERE/fakestun.py" 203.0.113.1 3478 53 > "$OUT/stun.log" 2>&1 &
ip netns exec inet node "$HERE/../src/local.js" 8787 203.0.113.1 > "$OUT/server.log" 2>&1 &
sleep 0.5
for i in 1 2; do
  ( cd "$OUT/h$i" && ip netns exec h$i env MKGP2_ISO="$ISO" MKGP2PC_HZ=60 MKGP2PC_TRACE_LINK=1 \
    MKGP2PC_LINK_ROOM="てすと room" MKGP2PC_LINK_SERVER=http://203.0.113.1:8787 \
    MKGP2PC_LINK_STUN=203.0.113.1:3478,203.0.113.1:53 $EXTRA \
    timeout "$1" "$ROOT/build/pc-census/mkgp2-nogfx" 2>&1 | head -c 300000 > "$OUT/h$i.log" ) &
  sleep 1
done
wait %3 %4 2>/dev/null
pkill -f fakestun.py; pkill -f rendezvous/src/local.js
grep -h "room:\|connected\|accept on" "$OUT/h1.log" "$OUT/h2.log"
