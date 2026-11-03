#!/bin/sh
# Vela AI replays with a lower current-frame weight: amax (cap) and alphaK (scale), mk 8
# Inputs from the environment: SGSR_REPLAY (sgsr_replay.exe of the SGSR bench), SPV (its SPIR-V folder), STUDENT_PLAN
# (the .plan file of the Vela AI recipe), CAPTURES (RE4 capture folders), REPLAYS (output, default ./re4-rep).
R=${SGSR_REPLAY:?}; SPV=${SPV:?}; P=${STUDENT_PLAN:?}
for s in 130905 131039 131119 131245; do
  S=${CAPTURES:?}/Resident-Evil-4_20261026-$s; O=${REPLAYS:-./re4-rep}/$s
  for v in "a40 0.4 1" "a25 0.25 1" "a15 0.15 1" "k50 0.6 0.5" "k25 0.6 0.25"; do
    set -- $v
    ST_AMAX=$2 ST_ALPHAK=$3 ST_MK=8 $R $SPV $S --out $O --no-seed --only student --student-plan $P --st-tag $1 > $O/$1.log 2>&1 || { echo "FAILED $s $1"; tail -3 $O/$1.log; }
  done
  echo "$s done"
done
