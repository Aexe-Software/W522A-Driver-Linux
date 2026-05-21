#!/bin/sh
# w1-aml WiFi driver — one-shot installer for Armbian / mainline kernels
#
# After running this once you can reboot and the `w522a` interface
# should come up automatically — same UX as mt7601u / brcmfmac etc.,
# but with a dedicated chip-specific name (no fight over wlan0/wlan1).
#
# Usage:  sudo ./install.sh

set -eu

KVER="${KVER:-$(uname -r)}"
MOD_DIR="/lib/modules/${KVER}/kernel/drivers/net/wireless/aml-w1"

if [ "$(id -u)" -ne 0 ]; then
    echo "This script must be run as root (try: sudo $0)" >&2
    exit 1
fi

if [ ! -d "/lib/modules/${KVER}/build" ]; then
    echo "Kernel headers for ${KVER} not found in /lib/modules/${KVER}/build" >&2
    echo "Install them first (e.g. apt install linux-headers-${KVER} or your" >&2
    echo "Armbian-specific headers package), then re-run this script." >&2
    exit 1
fi

SRC_DIR="$(cd "$(dirname "$0")"/.. && pwd)"

echo "==> Building modules against kernel ${KVER}..."
cd "${SRC_DIR}"
make clean >/dev/null 2>&1 || true
make KDIR="/lib/modules/${KVER}/build"

echo "==> Installing modules to ${MOD_DIR}..."
install -d "${MOD_DIR}"
install -m 644 aml_sdio.ko vlsicomm.ko "${MOD_DIR}/"

echo "==> Installing firmware / RF config..."
install -d /lib/firmware/aml /etc/aml-wifi
install -m 644 vmac/aml_wifi_rf_fn_link.txt /lib/firmware/aml/aml_wifi_rf_fn_link.txt
install -m 644 vmac/aml_wifi_rf_0333.txt    /lib/firmware/aml/aml_wifi_rf_0333.txt
install -m 644 vmac/aml_wifi_rf_fn_link.txt /lib/firmware/aml/aml_wifi_rf.txt
install -m 644 vmac/aml_wifi_rf_fn_link.txt /etc/aml-wifi/aml_wifi_rf.txt
install -m 644 vmac/aml_wifi_drv_cfg_0.conf /etc/aml-wifi/

echo "==> Installing modprobe.d / modules-load.d auto-load config..."
install -m 644 install_files/aml-w1.conf    /etc/modprobe.d/aml-w1.conf
install -m 644 install_files/aml-w1.modules /etc/modules-load.d/aml-w1.conf

echo "==> Refreshing module dependency cache..."
depmod -a "${KVER}"

# Best-effort blacklist of conflicting in-tree experimental drivers
# (none known to date; placeholder for future).

cat <<EOF

============================================================
 Driver installed for kernel ${KVER}.

 What happens next:
  1. Reboot. The SDIO hotplug event will modprobe aml_sdio,
     which will pull in vlsicomm, which will register the
     dedicated 'w522a' interface (the chip-specific name
     keeps it from colliding with wlan0/wlan1 owned by other
     in-tree wifi drivers like mt7601u).
  2. NetworkManager / iwd / wpa_supplicant should pick the
     interface up the same way they do for mt7601u.

 To verify after reboot:
     lsmod | grep -E 'aml_sdio|vlsicomm'
     ip link show w522a
     iw dev
     dmesg | grep -E 'aml_|vlsicomm|w522a'

 To uninstall later:
     sudo rm -rf ${MOD_DIR}
     sudo rm -f /etc/modprobe.d/aml-w1.conf /etc/modules-load.d/aml-w1.conf
     sudo depmod -a
============================================================
EOF
