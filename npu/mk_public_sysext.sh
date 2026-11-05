#!/bin/sh
# Builds the Vela NPU system part, package 14: the systemd system extension vela-npu-sysext-14.raw.
# Contents: the NPU daemon fsr4npud and its checker, the 17 FSR 4 graphs for HTP v73 (Snapdragon 8 Gen 2, SM8550) and the same
# 17 graphs built for HTP v79 (Snapdragon 8 Elite, SM8750; in v79/), quic/fastrpc (commit a56e9d4, BSD-3-Clause, built from
# source), the systemd units (units/ next to this script), two drop-ins that pick the v79 graphs on SM8750 (units/ next to this script), the udev rule,
# modules-load, extension-release, notices, and the Qualcomm AI Engine Direct (QNN) runtime 2.47.0.260601 (10 files: host part,
# HTP v73 and HTP v79 parts, unmodified, each sha256-checked) with Qualcomm's licence and Notice File (unchanged).
# NOT in the image: the 3 DSP-side files of the device (fastrpc_shell_unsigned_3, libc++.so.1, libc++abi.so.1);
# install-vela-npu.sh reads those from the device's own dsp partition into a second, directory extension armada-vela-npu-qnn.
# Library search paths: libcdsp_default_listener RUNPATH and fsr4npud RPATH are set to /usr/lib64/vela-npu (DT_RPATH kept on
# fsr4npud); the Qualcomm files stay byte-identical (their RPATH $ORIGIN = /usr/lib64/vela-npu, root-owned, read-only).
# A security gate fails the build on any RPATH/RUNPATH outside /usr/lib64/vela-npu (or $ORIGIN on an unmodified Qualcomm file
# there), any /var/home, /home/, /tmp/, /run/user string (plus EXTRA_BAD_PATHS, a regex) in any file, a setuid/setgid/sticky or
# group/world-writable file, or a DSP partition file in the image.
# Every payload file is md5-checked against the payload list BEFORE the search-path change.
# Reproducible: fixed SOURCE_DATE_EPOCH, all-root, no xattrs, 1 thread (two builds give the same sha256).
#
# Inputs (folders mounted into the container; all required):
#   FASTRPC_STAGE  quic/fastrpc install tree: lib/libcdsprpc.so.1.0.0, lib/libcdsp_default_listener.so.1.0.0, sbin/cdsprpcd
#   FASTRPC_SRC    quic/fastrpc source checkout (LICENSE.txt);  FASTRPC_COMMIT_FILE: a file "<repo> <commit>"
#   NPUD_BIN       fsr4npud.stripped and fsr4npu-check
#   GRAPHS         the 17 graphs nf4_exact.bin ... nf4up_k1h_270.bin (HTP v73)
#   GRAPHS_V79     the same 17 file names, built for HTP v79
#   (not an input: the units, the udev rule, modules-load and the two drop-ins are units/ next to this script)
#   QAIRT          the unpacked Qualcomm AI Engine Direct SDK 2.47.0.260601 (lib/..., LICENSE.pdf, QNN_NOTICE.txt)
#   PAYLOAD_MD5    md5 list of the payload ("<md5>  ./usr/..."): the files tested on the SM8550 device, the v79 graphs,
#                  the V79 runtime files and the two drop-ins
#   OUT            output folder
# Example (Fedora 44 container):
#   docker run --rm -v <inputs>:/in -v <out>:/out -v <repo>/npu:/npu -e FASTRPC_STAGE=/in/... ... -e OUT=/out \
#     registry.fedoraproject.org/fedora:44 sh /npu/mk_public_sysext.sh
set -e
umask 022
VER=14
EPOCH=1761955200   # 2025-11-01 00:00 UTC
SYSLIB=/usr/lib64/vela-npu
dnf -y -q install squashfs-tools patchelf binutils >/dev/null 2>&1
echo "tools: $(patchelf --version) / $(mksquashfs -version 2>&1 | head -1) / $(readelf --version | head -1)"
FR=${FASTRPC_STAGE:?}; FSRC=${FASTRPC_SRC:?}; FCOMMIT=${FASTRPC_COMMIT_FILE:?}; NB=${NPUD_BIN:?}; G=${GRAPHS:?}; P=$(cd "$(dirname "$0")" && pwd)/units
PAY=${PAYLOAD_MD5:?}; OUT=${OUT:?}; XBAD=${EXTRA_BAD_PATHS:-}
QAIRT_VER=2.47.0.260601
Q=${QAIRT:?}
# Qualcomm files: <sha256> <path in the image> <path in the SDK>
QNN_FILES="9e1a73ed4f3e7cf3ef3199a23dfb3885b12a64edea30e607b0687b25b28e94f7 usr/lib64/vela-npu/libQnnHtp.so lib/aarch64-oe-linux-gcc11.2/libQnnHtp.so
71253e59ef34bc1b41bd7a54c0a1bd780e65aa47f4e2d542305106eb02af6e32 usr/lib64/vela-npu/libQnnHtpV73Stub.so lib/aarch64-oe-linux-gcc11.2/libQnnHtpV73Stub.so
1dcc4e535f94b7d921d5c294eafc06bc0c87968077e5ee36bb14538820ef6990 usr/lib64/vela-npu/libQnnSystem.so lib/aarch64-oe-linux-gcc11.2/libQnnSystem.so
b5084469a693b05e372eedf8861e29e92c6e012c1cf543144c894f6141a5348f usr/lib/rfsa/adsp/libQnnHtpV73Skel.so lib/hexagon-v73/unsigned/libQnnHtpV73Skel.so
472a7e75763a83dca3463b14f543f54bb4cd5b392a615343375ea1738159cafc usr/lib/rfsa/adsp/libQairtHtpV73Skel.so lib/hexagon-v73/unsigned/libQairtHtpV73Skel.so
9be81519a066a7beee544d1c1a5fe7947df04b53e087fd8e55160b846db0c614 usr/lib/rfsa/adsp/libQnnHtpV73.so lib/hexagon-v73/unsigned/libQnnHtpV73.so
e8b7712f1df3e05e08b61f1d9ebde5b966cca0f598c55fd96c85d6bf37c41dcc usr/lib64/vela-npu/libQnnHtpV79Stub.so lib/aarch64-oe-linux-gcc11.2/libQnnHtpV79Stub.so
dd8987c17928e53877e51ce5739c550d3e74393bd5b1b5ccf311326eddb08612 usr/lib/rfsa/adsp/libQnnHtpV79Skel.so lib/hexagon-v79/unsigned/libQnnHtpV79Skel.so
bea936b4ab0d5d8a3d172a634cbeaba38ab6b59c103c23b4580191f95717707c usr/lib/rfsa/adsp/libQairtHtpV79Skel.so lib/hexagon-v79/unsigned/libQairtHtpV79Skel.so
998a3f868c906db88969b11a328ef9adefa4269017f5216b33780574f06d2e0c usr/lib/rfsa/adsp/libQnnHtpV79.so lib/hexagon-v79/unsigned/libQnnHtpV79.so"
QNN_NAMES="libQnnHtp.so libQnnHtpV73Stub.so libQnnSystem.so libQnnHtpV73Skel.so libQairtHtpV73Skel.so libQnnHtpV73.so libQnnHtpV79Stub.so libQnnHtpV79Skel.so libQairtHtpV79Skel.so libQnnHtpV79.so"
G79=${GRAPHS_V79:?}; DROPIN=$P
T=$(mktemp -d /tmp/velapub.XXXXXX); chmod 0755 $T
L=$T$SYSLIB; D=$T/usr/share/licenses/armada-vela-npu
mkdir -p $L $L/v79 $T/usr/bin $T/usr/lib/udev/rules.d $T/usr/lib/systemd/system/vela-npu-setup.service.d $T/usr/lib/systemd/system/vela-npu.service.d $T/usr/lib/extension-release.d $T/usr/lib/modules-load.d $T/usr/lib/rfsa/adsp $D
install -m 0755 $FR/lib/libcdsprpc.so.1.0.0 $FR/lib/libcdsp_default_listener.so.1.0.0 $L/
ln -s libcdsprpc.so.1.0.0 $L/libcdsprpc.so.1; ln -s libcdsprpc.so.1 $L/libcdsprpc.so
ln -s libcdsp_default_listener.so.1.0.0 $L/libcdsp_default_listener.so.1; ln -s libcdsp_default_listener.so.1 $L/libcdsp_default_listener.so
install -m 0755 $FR/sbin/cdsprpcd $L/cdsprpcd
install -m 0755 $NB/fsr4npud.stripped $L/fsr4npud
install -m 0755 $NB/fsr4npu-check $L/fsr4npu-check
ln -s ../lib64/vela-npu/fsr4npu-check $T/usr/bin/fsr4npu-check
GRAPH_LIST="nf4_k1h nf4_k1h_360 nf4_k1h_450 nf4up_k1h nf4up_k1h_450 nf4up_k1h_360 nf4_exact nf4_k1h_452 nf4up_k1h_452 nf4_k1h_405 nf4up_k1h_405 nf4_k1h_400 nf4up_k1h_400 nf4_k1h_324 nf4up_k1h_324 nf4_k1h_270 nf4up_k1h_270"
for u in $GRAPH_LIST; do install -m 0644 $G/$u.bin $L/$u.bin; done
for u in $GRAPH_LIST; do install -m 0644 $G79/$u.bin $L/v79/$u.bin; done
# the Qualcomm runtime: host libraries 0755 (as on the tested device), DSP-side skels 0644; each sha256-checked
echo "$QNN_FILES" | while read -r sha dst src; do
  case $dst in usr/lib64/*) m=0755;; *) m=0644;; esac
  install -m $m "$Q/$src" "$T/$dst"
  echo "$sha  $T/$dst" | sha256sum -c --status || { echo "QNN SHA256 MISMATCH $src"; exit 1; }
  echo "qnn sha256 ok $dst"
done
install -m 0644 $P/70-vela-npu.rules $T/usr/lib/udev/rules.d/70-vela-npu.rules
install -m 0644 $P/vela-npu.service $P/vela-npu-listener.service $P/vela-npu-setup.service $T/usr/lib/systemd/system/
install -m 0644 $P/vela-npu.conf $T/usr/lib/modules-load.d/vela-npu.conf
# SM8750: the setup unit links /run/vela-npu/graphs to v79/, the daemon gets each v79 graph mounted over its path in --graph-dir
install -m 0644 $DROPIN/vela-npu-setup-chip.conf $T/usr/lib/systemd/system/vela-npu-setup.service.d/50-vela-npu-chip.conf
install -m 0644 $DROPIN/vela-npu-chip.conf $T/usr/lib/systemd/system/vela-npu.service.d/50-vela-npu-chip.conf
install -m 0644 $FSRC/LICENSE.txt $D/fastrpc-BSD-3-Clause.txt
# AI Stack License s2(b) keep notices, s10(h) "must not remove or alter any such Notice File": the SDK's
# LICENSE.pdf and QNN_NOTICE.txt ship UNCHANGED next to the runtime (sha256-checked; name of the PDF as ORT's Qualcomm_LICENSE.pdf)
QLIC_FILES="ec1dccfdcba5c6e64126e84199b8362bf4999107bfa567ebe831dbb4c461692b LICENSE.pdf Qualcomm-AI-Stack-LICENSE.pdf
c8a22fa8b9ed3e9266c4a3366965474cef5cb147ee2a1f33e488bbb24a071c09 QNN_NOTICE.txt QNN_NOTICE.txt"
echo "$QLIC_FILES" | while read -r sha src dst; do
  install -m 0644 "$Q/$src" "$D/$dst"
  echo "$sha  $D/$dst" | sha256sum -c --status || { echo "QUALCOMM LICENCE SHA256 MISMATCH $src"; exit 1; }
  echo "licence sha256 ok $dst"
done
cat > $D/README.txt <<R
Vela NPU system part, package $VER (public image).
Ours: fsr4npud, fsr4npu-check (the NPU daemon and its checker), the 17 FSR 4 graphs nf4*.bin (HTP v73, Snapdragon 8 Gen 2 SM8550)
and v79/nf4*.bin (the same graphs for HTP v79, Snapdragon 8 Elite SM8750), the systemd units and drop-ins, the udev rule.
quic/fastrpc (BSD-3-Clause, fastrpc-BSD-3-Clause.txt): cdsprpcd, libcdsprpc, libcdsp_default_listener, built from source.
Qualcomm AI Engine Direct SDK runtime $QAIRT_VER, redistributed by the Vela author, unmodified:
  usr/lib64/vela-npu/libQnnHtp.so, libQnnHtpV73Stub.so, libQnnSystem.so;
  usr/lib/rfsa/adsp/libQnnHtpV73Skel.so, libQairtHtpV73Skel.so, libQnnHtpV73.so;
  usr/lib64/vela-npu/libQnnHtpV79Stub.so; usr/lib/rfsa/adsp/libQnnHtpV79Skel.so, libQairtHtpV79Skel.so, libQnnHtpV79.so.
  Licence: Qualcomm AI Stack License (Qualcomm-AI-Stack-LICENSE.pdf, the SDK's LICENSE.pdf unchanged) and its Notice File
  (QNN_NOTICE.txt, unchanged). Distributed only as part of Vela, never on a standalone basis (licence section 1).
  Export: see the licence's export-control section (10(f)).
fsr4npud: GPL-3.0-or-later with an additional permission (GPL-3.0 section 7) to link with and load the Qualcomm AI Engine Direct runtime.
NOT in this image: the DSP-side shell and C++ runtime (fastrpc_shell_unsigned_3, libc++.so.1, libc++abi.so.1).
install-vela-npu.sh reads them from your device's own dsp partition (extension armada-vela-npu-qnn).
R
cat > $T/usr/lib/extension-release.d/extension-release.armada-vela-npu <<R
ID=armada
VERSION_ID=44
ARCHITECTURE=arm64
# Not used by systemd-sysext: what is inside.
X_VELA_NPU_PACKAGE=$VER
X_VELA_NPU_PART=base + Qualcomm QNN runtime (public r2; the device's DSP shell: extension armada-vela-npu-qnn)
X_VELA_NPU_GRAPHS=nf4_k1h nf4_k1h_360 nf4_k1h_450 nf4up_k1h nf4up_k1h_450 nf4up_k1h_360 nf4_exact nf4_k1h_452 nf4up_k1h_452 nf4_k1h_405 nf4up_k1h_405 nf4_k1h_400 nf4up_k1h_400 nf4_k1h_324 nf4up_k1h_324 nf4_k1h_270 nf4up_k1h_270
X_VELA_NPU_UNIT=vela-npu.service
X_VELA_NPU_CHIPS=SM8550 (HTP v73) SM8750 (HTP v79, graphs in v79/)
X_VELA_NPU_QAIRT=$QAIRT_VER
X_VELA_NPU_FASTRPC=$(cut -d' ' -f2 $FCOMMIT)
X_VELA_NPU_BUILT_ON_IMAGE=20261010.c3d178c
R
cd $T; fail=0
# every payload file must be byte-identical to its line in $PAYLOAD_MD5 (SM8550: the tested device's files)
for f in $(find usr -type f ! -path "usr/share/*" ! -name "extension-release.*" | sort); do
  want=$(grep "  ./$f\$" $PAY | cut -d' ' -f1); got=$(md5sum $f | cut -d' ' -f1)
  if [ -n "$want" ] && [ "$want" = "$got" ]; then echo "md5 ok   $f"; else echo "md5 DIFF $f ($got vs tested ${want:-none})"; fail=1; fi
done
for q in fastrpc_shell_unsigned_3 libc++.so.1 libc++abi.so.1; do
  [ -z "$(find . -name $q)" ] || { echo "DSP PARTITION FILE IN IMAGE: $q"; fail=1; }; done
[ $fail = 0 ] || { echo "FAIL"; exit 1; }
# system paths only (after the md5 check: these two files differ from the tested package by the search path alone)
patchelf --set-rpath $SYSLIB .$SYSLIB/libcdsp_default_listener.so.1.0.0
patchelf --force-rpath --set-rpath $SYSLIB .$SYSLIB/fsr4npud
# security gate over every file in the image
for f in $(find usr -type f | sort); do
  if [ "$(head -c 4 $f | od -An -tx1 | tr -d ' \n')" = 7f454c46 ]; then
    for p in $(readelf -d $f 2>/dev/null | sed -n 's/.*(R\(UN\)\{0,1\}PATH).*\[\(.*\)\]/\2/p' | tr ':' ' '); do
      b=$(basename $f); ok=0
      [ "$p" = $SYSLIB ] && ok=1
      [ "$p" = '$ORIGIN' ] && [ "$(dirname /$f)" = $SYSLIB ] && case " $QNN_NAMES " in *" $b "*) ok=1;; esac
      if [ $ok = 1 ]; then echo "path ok  $f: $p"; else echo "BAD SEARCH PATH $f: $p"; fail=1; fi
    done
  fi
  # the Qualcomm runtime names a mkstemp template /tmp/tmp.XXXXXX (a temp file it creates; the daemons run with PrivateTmp=yes):
  # allowed on those 6 files only, by its exact prefix
  h=$(LC_ALL=C grep -a -o -E "(/var/home|/home/|/tmp/|/run/user${XBAD:+|$XBAD})[[:graph:]]{0,40}" $f | sort -u || true)
  case " $QNN_NAMES " in *" $(basename $f) "*) h=$(echo "$h" | grep -v '^/tmp/tmp\.XXXXXX' || true);; esac
  if [ -n "$h" ]; then echo "USER PATH STRING IN $f: $(echo "$h" | head -3 | tr '\n' ' ')"; fail=1; fi
done
[ -z "$(find usr ! -type l -perm /7022)" ] || { echo "BAD MODE:"; find usr ! -type l -perm /7022 -exec ls -ld {} +; fail=1; }
[ $fail = 0 ] || { echo "FAIL (security gate)"; exit 1; }
echo "security gate ok: search paths only $SYSLIB (or \$ORIGIN on the unmodified Qualcomm files there), no user-writable path strings, no setuid/sgid/sticky, no group/world-writable file; mksquashfs -all-root = root:root"
find usr -type f | sort | xargs md5sum > $L/MANIFEST.md5
find $T -exec touch -h -d @$EPOCH {} +
mkdir -p $OUT; R=$OUT/vela-npu-sysext-$VER.raw
mksquashfs $T $R -all-root -no-xattrs -noappend -comp zstd -processors 1 -quiet -mkfs-time $EPOCH -all-time $EPOCH
cd $OUT && sha256sum vela-npu-sysext-$VER.raw > vela-npu-sysext-$VER.raw.sha256 && unsquashfs -lls vela-npu-sysext-$VER.raw > vela-npu-sysext-$VER.contents.txt
cat vela-npu-sysext-$VER.raw.sha256; ls -la $R
