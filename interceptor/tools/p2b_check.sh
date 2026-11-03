#!/bin/sh
# Check: recipe resolve (a) vs the game's Params resolve (b) with amax 0 must be bit-identical; then RE4 series amax 0 / 0.25
# Inputs from the environment: SHADERS (the SGSR bench shaders folder; generated student shaders in $SHADERS/student/gen),
# SGSR_REPLAY (sgsr_replay.exe), SPV (its SPIR-V folder), FORK_TOOLS (the fork's tools folder with gen_student_spv.py),
# DUMPS (fork dump sets), CAPTURES (RE4 capture folders); GLSLANG (default glslangValidator), WORK (default ./p2b).
GL=${GLSLANG:-glslangValidator}; SH=${SHADERS:?}; GD=$SH/student/gen
W=${WORK:-./p2b}; M=S4s4P2b
R=${SGSR_REPLAY:?}; P=$GD/$M/$M.plan
rm -rf $W; mkdir -p $W/a $W/b $W/out
for d in a b; do $GL -V $SH/fullscreen.vert -o $W/$d/fullscreen.vert.spv >/dev/null; for f in $GD/$M/*.comp; do $GL -V $f -o $W/$d/gen_${M}_$(basename $f).spv >/dev/null || exit 1; done; done
$GL -V $GD/$M/resolve.frag -o $W/a/gen_${M}_resolve.frag.spv >/dev/null || exit 1
python -I -c "
import sys; sys.path.insert(0, '${FORK_TOOLS:?}'); import gen_student_spv as g
open('$W/resolve_p.frag', 'w', newline='\n').write(g.params_patch(open('$GD/$M/resolve.frag').read()))"
cp $GD/$M/*.glsl $W/ 2>/dev/null
$GL -V $W/resolve_p.frag -o $W/b/gen_${M}_resolve.frag.spv >/dev/null || exit 1
for c in c17-bmi-540p-174637816 g-ctl-540p-63294580; do
  set=${DUMPS:?}/$c; mkdir -p $W/out/$c
  $R $W/a $set --out $W/out/$c --no-seed --only student --student-plan $P --st-tag A > $W/a.log 2>&1 || { tail -3 $W/a.log; exit 1; }
  ST_AMAX=0 $R $W/b $set --out $W/out/$c --no-seed --only student --student-plan $P --st-tag B > $W/b.log 2>&1 || { tail -3 $W/b.log; exit 1; }
  ST_AMAX=0.25 $R $W/b $set --out $W/out/$c --no-seed --only student --student-plan $P --st-tag C > $W/c.log 2>&1 || { tail -3 $W/c.log; exit 1; }
  n=0; s=0; d=0; for f in $W/out/$c/*_replay_A.bin; do n=$((n+1)); cmp -s $f ${f%_A.bin}_B.bin && s=$((s+1)); cmp -s $f ${f%_A.bin}_C.bin || d=$((d+1)); done
  echo "$c: amax 0 == recipe: $s / $n; amax 0.25 differs: $d / $n"
  rm -rf $W/out/$c
done
for s in 130905 131039 131119 131245; do
  S=${CAPTURES:?}/Resident-Evil-4_20261026-$s; O=${REPLAYS:-./re4-rep}/$s; mkdir -p $O
  for v in "p2b0 0" "p2b25 0.25" "p2b15 0.15"; do set -- $v
    ST_AMAX=$2 $R $W/b $S --out $O --no-seed --only student --student-plan $P --st-tag $1 > $O/$1.log 2>&1 || { echo "FAILED $s $1"; tail -3 $O/$1.log; }
  done
  ST_AMAX=0.6 ST_MK=8 $R ${SPV:?} $S --out $O --no-seed --only student --student-plan $GD/S4s4L6acw/S4s4L6acw.plan --st-tag l6a60 > $O/l6.log 2>&1
  ST_AMAX=0.25 ST_MK=8 $R ${SPV:?} $S --out $O --no-seed --only student --student-plan $GD/S4s4L6acw/S4s4L6acw.plan --st-tag l6a25 > $O/l6b.log 2>&1
  echo "$s replays done"
done
