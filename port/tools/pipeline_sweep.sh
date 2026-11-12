#!/bin/bash
# Record the pipelines the game uses without playing it: run the rendering
# build's attract demo once per cup and course (forced with force_cup.gdb,
# as cup_sweep.sh does for the no-graphics build), each run with a user
# directory of its own, then merge the lists Aurora recorded into one file
# (docs/port/census.md section 36).
#
#   MKGP2_ISO=/path/to.iso port/tools/pipeline_sweep.sh OUT_DIR [frames] [jobs] [more lists...]
#
# OUT_DIR/initial_pipeline_cache.db is the result: next to the executable it
# makes the game compile all of these before it starts. Further lists (a
# pipeline_cache.db from a PC that has played) are merged in. Needs a
# display (Xvfb will do; software rendering manages about 25 frames a
# second, so 7000 frames, which reaches the demo race and its first half
# minute, take four to five minutes a run). A run that has finished is not
# repeated: delete OUT_DIR/runs to start over.
cd "$(dirname "$0")/../.." || exit 1
out=${1:?output directory}; frames=${2:-7000}; jobs=${3:-4}; shift 3 2>/dev/null || shift $#
mkdir -p "$out/runs"
export out frames
for cup in 1 2 3 4 5 6 7 8; do for variant in 0 1 2 3; do for long in 0 1; do
  echo "$cup $long $variant 0"
done; done; done | xargs -P "$jobs" -L 1 bash -c '
  d=$out/runs/$0_$1_$2_$3; mkdir -p "$d"
  [ -f "$d/done" ] && exit 0
  XDG_DATA_HOME=$d MKGP2PC_NOLIMIT=1 MKGP2PC_LINK=0 MKGP2PC_CARD=0 MKGP2PC_SPIKE_MS=0 MKGP2PC_NO_SHADER_WAIT=1 \
  MKGP2PC_FRAMES=$frames timeout 1200 gdb -q -batch \
    -ex "set \$cup=$0" -ex "set \$long=$1" -ex "set \$variant=$2" -ex "set \$rev=$3" \
    -x port/tools/force_cup.gdb build/pc-gfx/cmake/mkgp2-gfx > "$d/run.log" 2>&1
  touch "$d/done"'
# shellcheck disable=SC2046
python3 port/tools/pipeline_list.py -o "$out/initial_pipeline_cache.db" \
  $(ls "$out"/runs/*/mkgp2-pc/pipeline_cache.db) "$@"
