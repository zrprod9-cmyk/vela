#!/bin/sh
# Replays of a recorded series (the series recorder's output; sgsr_replay reads it as a fork dump), on the PC:
#   Vela AI: L6acw with the Params resolve, amax 0.6, one run per mk ("8 12 16" -> tags mk8 mk12 mk16)
#   "sr": Vela SR (v1f with the lock and hp 8, the scale's upscale build as in the plugin checks) -> tag sr
# sh tools/series_replay.sh <series dir> <out dir> ["8 12 16 sr"]
S=$1; O=$2; RUNS=${3:-"8 12 16 sr"}
# Inputs from the environment: SGSR_REPLAY (sgsr_replay.exe of the SGSR bench), SPV (its SPIR-V folder), STUDENT_PLAN (.plan).
R=${SGSR_REPLAY:?}; SPV=${SPV:?}; P=${STUDENT_PLAN:?}
mkdir -p $O
for r in $RUNS; do
  if [ "$r" = sr ]; then
    sc=$(python -I -c "import json,glob,sys;m=json.load(open(sorted(glob.glob(sys.argv[1]+'/f*.json'))[0]));s=m['display'][0]/m['render'][0];print('a' if s<1.75 else 'b' if s<2.5 else 'c')" $S)
    $R $B/spv $S --out $O --no-seed --only sgsr3 --s3-tag sr --s3-up vf_up1_base_$sc --s3-conv vf_conv1_l2 \
      --s3-fast --s3-hsplit 10 --s3-work-fmt 10 --s3-nrg --s3-hp 8 > $O/sr.log 2>&1 || { tail -5 $O/sr.log; exit 1; }
    echo "Vela SR ($sc): $(ls $O/*_replay_sr.bin | wc -l) frames"
  else
    ST_AMAX=0.6 ST_MK=$r $R $SPV $S --out $O --no-seed --only student --student-plan $P --st-tag mk$r > $O/mk$r.log 2>&1 || { tail -5 $O/mk$r.log; exit 1; }
    echo "Vela AI mk $r: $(ls $O/*_replay_mk$r.bin | wc -l) frames"
  fi
done
