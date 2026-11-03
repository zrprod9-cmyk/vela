#!/bin/bash
# AMD module -> provider module: OpSDot -> OpSUDot (sudot.py) and constant Z (zconst.py).   usage: xf.sh in.spv out.spv TOOLS-dir
set -e
spirv-dis "$1" -o /tmp/xf.0
V=$(grep -m1 'Version:' /tmp/xf.0 | sed 's/.*Version: //')
python3 "$3/sudot.py" /tmp/xf.0 /tmp/xf.1 | tail -1
python3 "$3/zconst.py" /tmp/xf.1 /tmp/xf.2 | tail -1
spirv-as --target-env spv$V /tmp/xf.2 -o "$2"
spirv-val --target-env vulkan1.3 "$2"
