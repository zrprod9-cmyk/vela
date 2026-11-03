#!/bin/sh
# Vela NPU system part, package 12: lets Vela run FSR 4 / FSR 4 Fast on the Hexagon NPU (Snapdragon 8 Gen 2, SM8550).
# Needs sudo ONCE (it adds a system extension, a udev rule and system services). Run in Desktop mode -> Konsole, game closed:
#   curl -fsSL https://github.com/zrprod9-cmyk/vela/releases/download/v0.8.16/install-vela-npu.sh | sudo sh
# What it does:
#   1. downloads vela-npu-sysext-12.raw (NPU daemon, 7 FSR 4 graphs, quic/fastrpc, and the Qualcomm AI Engine Direct (QNN)
#      runtime 2.47.0.260601, unmodified) and checks its sha256;
#   2. reads the DSP shell and C++ runtime (3 files) read-only from your device's own dsp partition;
#   3. installs both as systemd system extensions in /var/lib/extensions (nothing in armadaOS is overwritten), enables the
#      7 NPU daemons, and checks each one with a fixed input. Prints "Vela NPU: OK" or "Vela NPU: FAILED" + the reason.
# Remove: uninstall-vela-npu.sh (same release). Options: --image <local vela-npu-sysext-12.raw>, --force.
# The image carries the QNN runtime (no SDK download); library search paths: /usr/lib64/vela-npu only; root:root, umask 022.
set -u
umask 022
VER=12
TAG=v0.8.16
BASE_URL="https://github.com/zrprod9-cmyk/vela/releases/download/$TAG"
IMAGE=vela-npu-sysext-$VER.raw
IMAGE_SHA256=392ba27e276d985221d1d695b782628cffa68ae4165bcb2ec0e7676cfd49a52a
EXT=/var/lib/extensions
QDIR=/var/lib/extensions/armada-vela-npu-qnn
UNITS="vela-npu@nf4_exact.service vela-npu@nf4_k1h.service vela-npu@nf4_k1h_360.service vela-npu@nf4_k1h_450.service vela-npu@nf4up_k1h.service vela-npu@nf4up_k1h_450.service vela-npu@nf4up_k1h_360.service"
# DSP files from the device's dsp partition (/cdsp/<name>): the hashes of the tested Retroid Pocket 6; another hash = a warning
DSP_FILES="cb2a0a8ed448bd6e0fea0a81bd0c3892b93373f3eff5c43ca577c7a0628d7251 fastrpc_shell_unsigned_3
6050f67f1416a69a81d3b7f34fa4ccdca139d8c752a24698a7fd679bf418b90f libc++.so.1
934f8039214f15b1a692a2278909a9468f1b5561affafbf60efb96e158583563 libc++abi.so.1"
# fsr4npu-check output hash per daemon (fixed input) = the hash of the tested graphs
WANT="fsr4npud_nf4up_k1h 60a3d9ad466fca76
fsr4npud_nf4up_k1h_450 90a028a11df7fc3e
fsr4npud_nf4up_k1h_360 f27ed0e09ef0d7b8
fsr4npud_nf4_k1h 8a735e657feee21f
fsr4npud_nf4_k1h_450 2b9d68691c111dbe
fsr4npud_nf4_k1h_360 2606832826ab4c91
fsr4npud_nf4_exact a83aff6f6a4633e8"

say() { printf '%s\n' "$*"; }
fail() { say ""; say "Vela NPU: FAILED - $*"; say "Vela keeps running FSR 4 on the GPU."; exit 1; }

LOCAL_IMAGE=""; FORCE=0
while [ $# -gt 0 ]; do case "$1" in
  --qairt) say "(--qairt is not needed any more: the image carries the QNN runtime)"; shift 2;; --image) LOCAL_IMAGE=${2:-}; shift 2;; --force) FORCE=1; shift;;
  -h|--help) sed -n 2,13p "$0" 2>/dev/null; exit 0;; *) fail "unknown option $1";; esac; done

say "== Vela NPU package $VER installer"
DRY=${VELA_NPU_DRY:-0}   # 1 = test of steps 1-3 only (no root, nothing installed; VELA_NPU_PART = a dsp image file)
[ $DRY = 1 ] && { ID=armada; VERSION_ID=44; } || . /etc/os-release
[ $DRY = 1 ] || [ "$(id -u)" = 0 ] || fail "run it with sudo (it asks for your password once)"
[ "${ID:-}" = armada ] || fail "this is for armadaOS (found ID=${ID:-?})"
[ $DRY = 1 ] || [ "$(uname -m)" = aarch64 ] || fail "this is for ARM64 devices"
[ $DRY = 1 ] || tr '\0' '\n' < /proc/device-tree/compatible 2>/dev/null | grep -qx "qcom,sm8550" || \
  fail "the NPU graphs are built for Snapdragon 8 Gen 2 (SM8550) only; on this chip Vela runs FSR 4 on the GPU"
if [ "${VERSION_ID:-}" != 44 ] && [ $FORCE = 0 ]; then
  fail "the image is made for armadaOS VERSION_ID=44, this system has ${VERSION_ID:-?} (systemd-sysext would not load it). --force tries anyway"
fi
USER_NAME=${SUDO_USER:-armada}; USER_HOME=$(getent passwd "$USER_NAME" | cut -d: -f6); USER_UID=$(id -u "$USER_NAME" 2>/dev/null || echo 1000)
[ "$USER_UID" = 1000 ] || say "WARNING: the NPU daemons accept only uid 1000 (the default armadaOS user); $USER_NAME has uid $USER_UID"
for t in sha256sum systemd-sysext systemctl udevadm debugfs runuser; do command -v $t >/dev/null 2>&1 || fail "missing tool: $t"; done
W=$(mktemp -d /tmp/vela-npu-install.XXXXXX) || fail "cannot make a temp folder"

say "== 1. our image ($IMAGE)"
if [ -n "$LOCAL_IMAGE" ]; then cp "$LOCAL_IMAGE" "$W/img" || fail "cannot read $LOCAL_IMAGE"
else command -v curl >/dev/null 2>&1 || fail "missing tool: curl"
  curl -fL --retry 3 -o "$W/img" "$BASE_URL/$IMAGE" || fail "download failed: $BASE_URL/$IMAGE"; fi
echo "$IMAGE_SHA256  $W/img" | sha256sum -c --status || fail "$IMAGE: sha256 does not match (broken download?)"
say "  sha256 ok"

mkdir -p "$W/stage/usr/lib/rfsa/adsp" "$W/stage/usr/lib/extension-release.d"

say "== 2. DSP shell and C++ runtime from this device's dsp partition (read-only)"
SLOT=$(tr ' ' '\n' < /proc/cmdline | sed -n 's/^androidboot.slot_suffix=//p' | head -1); SLOT=${SLOT:-_a}
PART=${VELA_NPU_PART:-/dev/disk/by-partlabel/dsp$SLOT}; [ -e "$PART" ] || fail "no dsp partition $PART"
echo "$DSP_FILES" | while read -r sha name; do
  debugfs -R "dump /cdsp/$name $W/stage/usr/lib/rfsa/adsp/$name" "$PART" >/dev/null 2>&1
  [ -s "$W/stage/usr/lib/rfsa/adsp/$name" ] || { echo x; break; }
  if echo "$sha  $W/stage/usr/lib/rfsa/adsp/$name" | sha256sum -c --status; then echo "  ok $name" >&2
  else echo "  WARNING: $name differs from the tested device (Retroid Pocket 6); using this device's own copy" >&2; fi
done | grep -qx x && fail "cannot read the /cdsp/ files from $PART"
chmod 0644 "$W"/stage/usr/lib/rfsa/adsp/*
cat > "$W/stage/usr/lib/extension-release.d/extension-release.armada-vela-npu-qnn" <<R
ID=armada
VERSION_ID=${VERSION_ID:-44}
ARCHITECTURE=arm64
# Not used by systemd-sysext: the device's DSP shell + C++ runtime for Vela NPU package $VER, from $PART (name kept from the first installer)
X_VELA_NPU_PACKAGE=$VER
X_VELA_NPU_PART=dsp (the device's DSP shell; the QNN runtime is in armada-vela-npu.raw)
R
[ $DRY = 1 ] && { say "DRY RUN: staged in $W/stage, nothing installed"; exit 0; }

say "== 3. stop the Vela NPU daemons, install the two extensions"
systemctl stop $UNITS vela-npu.service vela-npu-listener.service 2>/dev/null || true
systemctl disable vela-npu.service 2>/dev/null || true
mkdir -p "$EXT" "$QDIR/usr/lib/rfsa/adsp" "$QDIR/usr/lib/extension-release.d"
# The first installer put the QNN host libraries here too; the image carries them now: move the old copies out of the extension
[ -d "$QDIR/usr/lib64" ] && { mv "$QDIR/usr/lib64" "$W/old-r1-lib64" || fail "cannot move the old $QDIR/usr/lib64 aside"; }
cp "$W/img" "$EXT/armada-vela-npu.raw.new" && mv -f "$EXT/armada-vela-npu.raw.new" "$EXT/armada-vela-npu.raw" || fail "cannot write $EXT"
for d in usr/lib/rfsa/adsp usr/lib/extension-release.d; do cp -p "$W/stage/$d"/* "$QDIR/$d/" || fail "cannot write $QDIR"; done
chown -R root:root "$QDIR" && chmod -R go-w "$QDIR" || fail "cannot set root:root on $QDIR"
systemd-sysext refresh || fail "systemd-sysext refresh failed"
systemctl daemon-reload
systemd-sysext status | grep -q armada-vela-npu-qnn || fail "systemd-sysext did not merge the extensions (see: systemd-sysext status)"

say "== 4. device access (udev rule: group render may open /dev/fastrpc-cdsp and /dev/dma_heap/system)"
modprobe system_heap 2>/dev/null || true
udevadm control --reload
udevadm trigger --action=change --settle --subsystem-match=misc --sysname-match=fastrpc-cdsp
udevadm trigger --action=change --settle --subsystem-match=dma_heap --sysname-match=system

say "== 5. services: 7 NPU daemons + a boot unit that starts them after the extension merge"
B=/etc/systemd/system/vela-npu-boot.service
cat > "$B.tmp" <<UNIT
[Unit]
Description=Vela NPU: start the NPU daemons after the sysext merge (sysext units miss the boot transaction)
After=systemd-sysext.service
Wants=systemd-sysext.service

[Service]
Type=oneshot
ExecStart=/usr/bin/systemctl start --no-block $UNITS

[Install]
WantedBy=multi-user.target
UNIT
mv -f "$B.tmp" "$B"
systemctl daemon-reload
systemctl enable $UNITS vela-npu-boot.service >/dev/null 2>&1 || fail "systemctl enable failed"
systemctl restart vela-npu-setup.service || fail "vela-npu-setup.service failed (journalctl -u vela-npu-setup)"
for u in $UNITS; do systemctl restart "$u" || say "  $u did not start"; done
sleep 8

say "== 6. check every daemon with a fixed input"
bad=0
for u in $UNITS; do s=${u#vela-npu@}; s=fsr4npud_${s%.service}; w=$(echo "$WANT" | sed -n "s/^$s //p")
  o=$(runuser -u "$USER_NAME" -- /usr/lib64/vela-npu/fsr4npu-check --name "$s" --runs 20 2>&1) || true
  h=$(echo "$o" | grep -oE 'output hash [0-9a-f]+' | cut -d' ' -f3)
  if [ "$(systemctl is-active "$u")" = active ] && echo "$o" | grep -q "RESULT OK" && [ "$h" = "$w" ]; then say "  ok $u"
  else say "  FAILED $u (state $(systemctl is-active "$u"), hash ${h:-none}, expected $w)"; bad=$((bad + 1)); fi
done
if [ $bad = 0 ]; then
  say ""; say "Vela NPU: OK - 7 of 7 NPU daemons work. Start your game again; the Vela panel shows \"on the NPU\"."
  exit 0
fi
say ""; say "Vela NPU: FAILED - $bad of 7 NPU daemons do not work. Logs: journalctl -b -u 'vela-npu@*' -u vela-npu-listener"
say "Reboot once and look at the Vela panel. To remove: uninstall-vela-npu.sh (with sudo)"
exit 2
