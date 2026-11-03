#!/bin/sh
# Dry run of install-vela-npu.sh steps 1-2 in a fedora:44 container (x86-64, no root actions): a local image and a stand-in
# for the device's dsp partition. Checks that the installer needs no Qualcomm SDK, stages the DSP files, and refuses a wrong
# image (sha256).
# Inputs: IMG (vela-npu-sysext-12.raw), DSP_TREE (the extracted file tree of the device's dsp partition),
# optional PAYLOAD_MD5 (md5 list of the tested NPU package, "<md5>  ./usr/..."): the merged file set is compared with it.
#   docker run --rm -v <repo>/npu:/npu -v <inputs>:/in -e IMG=/in/vela-npu-sysext-12.raw -e DSP_TREE=/in/dsp \
#     registry.fedoraproject.org/fedora:44 sh /npu/dry_test.sh
HERE=$(cd "$(dirname "$0")" && pwd)
IMG=${IMG:?path to vela-npu-sysext-12.raw}
DSP_TREE=${DSP_TREE:?extracted dsp partition tree}
dnf -y -q install e2fsprogs util-linux systemd udev squashfs-tools >/dev/null 2>&1
mkdir -p /home/armada/Downloads
useradd -M -d /home/armada armada 2>/dev/null
# an ext4 image of the extracted dsp tree stands in for the dsp partition
mkfs.ext4 -q -d "$DSP_TREE" /tmp/dsp_a.img 160M
export VELA_NPU_DRY=1 VELA_NPU_PART=/tmp/dsp_a.img SUDO_USER=armada
echo "--- run A: image only, no SDK on the machine"
sh "$HERE/install-vela-npu.sh" --image "$IMG"; echo "rc $?"
S=$(ls -d /tmp/vela-npu-install.* | head -1); (cd $S/stage && find . -type f | sort | xargs md5sum)
if [ -n "${PAYLOAD_MD5:-}" ]; then
  echo "--- merged file set (image + staged dsp dir) vs the tested NPU package ($PAYLOAD_MD5)"
  unsquashfs -q -d /tmp/m "$IMG" >/dev/null; cp -r $S/stage/usr/lib/rfsa/adsp/. /tmp/m/usr/lib/rfsa/adsp/
  (cd /tmp/m && find usr -type f ! -path "usr/share/*" ! -name "extension-release.*" ! -name MANIFEST.md5 | sort | while read f; do
    w=$(grep "  ./$f\$" "$PAYLOAD_MD5" | cut -d' ' -f1); g=$(md5sum $f | cut -d' ' -f1)
    [ "$w" = "$g" ] && echo "same $f" || echo "DIFF $f"; done)
fi
echo "--- run C: wrong image (must fail on sha256)"
sh "$HERE/install-vela-npu.sh" --image "$HERE/install-vela-npu.sh"; echo "rc $?"
