#!/bin/sh
# Removes the Vela NPU system part (install-vela-npu.sh). Needs sudo once. Vela then runs FSR 4 on the GPU.
#   curl -fsSL https://github.com/zrprod9-cmyk/vela/releases/download/v0.8.16/uninstall-vela-npu.sh | sudo sh
set -u
UNITS="vela-npu@nf4_exact.service vela-npu@nf4_k1h.service vela-npu@nf4_k1h_360.service vela-npu@nf4_k1h_450.service vela-npu@nf4up_k1h.service vela-npu@nf4up_k1h_450.service vela-npu@nf4up_k1h_360.service"
[ "$(id -u)" = 0 ] || { echo "Vela NPU uninstall: FAILED - run it with sudo"; exit 1; }
echo "== stop and disable the Vela NPU services"
systemctl stop $UNITS vela-npu-boot.service vela-npu-listener.service vela-npu-setup.service vela-npu.service 2>/dev/null || true
systemctl disable $UNITS vela-npu-boot.service vela-npu.service >/dev/null 2>&1 || true
rm -f /etc/systemd/system/vela-npu-boot.service
echo "== remove the two extensions (only the files the installer wrote)"
rm -f /var/lib/extensions/armada-vela-npu.raw /var/lib/extensions/armada-vela-npu.raw.new
cd /var/lib/extensions/armada-vela-npu-qnn 2>/dev/null && {
  rm -f usr/lib64/vela-npu/libQnnHtp.so usr/lib64/vela-npu/libQnnHtpV73Stub.so usr/lib64/vela-npu/libQnnSystem.so \
        usr/lib/rfsa/adsp/libQnnHtpV73Skel.so usr/lib/rfsa/adsp/libQairtHtpV73Skel.so usr/lib/rfsa/adsp/libQnnHtpV73.so \
        usr/lib/rfsa/adsp/fastrpc_shell_unsigned_3 usr/lib/rfsa/adsp/libc++.so.1 usr/lib/rfsa/adsp/libc++abi.so.1 \
        usr/lib/extension-release.d/extension-release.armada-vela-npu-qnn
  rmdir usr/lib64/vela-npu usr/lib64 usr/lib/rfsa/adsp usr/lib/rfsa usr/lib/extension-release.d usr/lib usr 2>/dev/null
  cd / && rmdir /var/lib/extensions/armada-vela-npu-qnn 2>/dev/null; }
cd /
systemd-sysext refresh
systemctl daemon-reload
udevadm control --reload
if [ -e /var/lib/extensions/armada-vela-npu.raw ] || [ -e /var/lib/extensions/armada-vela-npu-qnn ] || systemd-sysext status | grep -q armada-vela-npu; then
  echo "Vela NPU uninstall: FAILED - something is left (ls /var/lib/extensions; systemd-sysext status)"; exit 2
fi
echo "Vela NPU uninstall: OK - removed. /dev/fastrpc-cdsp goes back to root-only at the next reboot."
