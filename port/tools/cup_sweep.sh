#!/bin/bash
# Load every cup's courses in the attract demo of the no-graphics build and
# report the runs that crash (docs/port/census.md section 33).
#   MKGP2_ISO=/path/to.iso port/tools/cup_sweep.sh [seconds per run] [jobs]
# Each run forces one cup / round length / course with force_cup.gdb and
# plays the demo loop for the given time (default 60 s, several loads).
# Courses the game does not have crash too (cup 3 course 3, cup 8 long
# course 3): the list of cups and courses is not read from the game.
# Logs: build/pc-census/cup_sweep/<cup>_<long>_<variant>_r<reverse>.log
cd "$(dirname "$0")/../.." || exit 1
secs=${1:-60}; jobs=${2:-6}
out=build/pc-census/cup_sweep
mkdir -p "$out"
for rev in 0 1; do for cup in 1 2 3 4 5 6 7 8; do for long in 0 1; do for variant in 0 1 2 3; do
  echo "$cup $long $variant $rev"
done; done; done; done | xargs -P "$jobs" -L 1 bash -c '
  MKGP2PC_NOLIMIT=1 MKGP2PC_CARD=0 MKGP2PC_LINK=0 timeout '"$secs"' gdb -q -batch \
    -ex "set \$cup=$0" -ex "set \$long=$1" -ex "set \$variant=$2" -ex "set \$rev=$3" \
    -x port/tools/force_cup.gdb build/pc-census/mkgp2-nogfx > '"$out"'/$0_$1_$2_r$3.log 2>&1'
bad=0
for f in "$out"/*.log; do
  if grep -q "received signal" "$f"; then
    bad=1
    echo "CRASH $(basename "$f" .log): $(grep -m4 '^#[0-3] ' "$f" | cut -c1-120 | tr '\n' ' ')"
  else
    echo "ok    $(basename "$f" .log): $(grep -c force_cup: "$f") loads"
  fi
done
exit $bad
