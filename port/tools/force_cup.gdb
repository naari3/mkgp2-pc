# Make the attract mode's demo race load another cup's course, to reach a
# course without playing through the menus (docs/port/census.md section 33).
#
#   MKGP2_ISO=... MKGP2PC_NOLIMIT=1 gdb -q -batch \
#       -ex 'set $cup=1' -ex 'set $long=0' -ex 'set $variant=0' \
#       -x port/tools/force_cup.gdb build/pc-census/mkgp2-nogfx
#
# $cup: 1 is the Mario cup (tables `MRA_*`), 8 is the demo's own; the others
# are the cases of clModelMgr's constructor (src/game/mapobject/fn_8017B1C0.cpp).
# $long: 0 short round, 1 long. $variant: the course of the cup, 0..3 where
# the cup has several. $rev (optional, default 0): 1 loads the reverse
# direction of the course, which the game uses for rounds 5 to 8 ("ura").
# The demo's recorded steering belongs to its own course, so the kart drives
# into walls; course loading, the decoration models and drawing still run.
# The breakpoint is the statement after RaceSceneC's constructor has set the
# three globals; if the source moved, look for `g_cupId = 8;`.
set debuginfod enabled off
set pagination off
set confirm off
handle SIGUSR1 SIGUSR2 SIGPIPE SIGALRM nostop noprint pass
break src/game/Scene_8014E0C4.cpp:4964
commands
  silent
  set {int}&g_cupId = $cup
  set {int}&g_longRoundFlag = $long
  set {int}&g_courseVariantIdx = $variant
  if $_isvoid($rev)
    set $rev = 0
  end
  set {int}&g_reverseRoundFlag = $rev
  printf "force_cup: cup %d long %d variant %d reverse %d\n", $cup, $long, $variant, $rev
  continue
end
run
bt 16
