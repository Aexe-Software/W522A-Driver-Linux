#!/usr/bin/env bash
# w155-v11 ONE-SHOT: build → install → reload → connect → speedtest
#
# Run on the Armbian target device (the X96 Max Plus). It will:
#   1. compile aml_sdio.ko + vlsicomm.ko against /lib/modules/$(uname -r)/build
#   2. wipe any old version under /lib/modules/.../aml-w1/
#   3. copy in the new modules + auto-load configs (modprobe.d / modules-load.d)
#   4. rmmod old modules and modprobe the new aml_sdio (vlsicomm pulled via softdep)
#   5. wait up to 20 s for the `w522a` interface to appear
#   6. connect to the SSID/PSK passed on the command line via nmcli
#   7. run a tiny throughput/ping test and dump full dmesg into a result log
#
# Usage:
#   sudo ./install_files/oneshot.sh <SSID> <PSK> [--reboot]
#
# Example:
#   sudo ./install_files/oneshot.sh your-ssid your-password
#
# If something doesn't work, send the resulting /tmp/w155-oneshot.log
# back so it can be diagnosed.

set -u

SSID="${1:-}"
PSK="${2:-}"
REBOOT_FLAG="${3:-}"

LOG=/tmp/w155-oneshot.log
exec > >(tee -a "${LOG}") 2>&1

echo "=== w155 ONE-SHOT: $(date) ==="
echo "Kernel: $(uname -r)  Arch: $(uname -m)"
echo "Args: SSID='${SSID}'  REBOOT_FLAG='${REBOOT_FLAG}'"
echo

if [[ "$(id -u)" -ne 0 ]]; then
    echo "ERROR: must run as root.  Try:  sudo $0 $*" >&2
    exit 1
fi

if [[ -z "${SSID}" || -z "${PSK}" ]]; then
    cat >&2 <<EOF
Usage:
  sudo $0 <SSID> <PSK>           # build, install, hot-reload, connect, speedtest
  sudo $0 <SSID> <PSK> --reboot  # ... but reboot after install instead of hot-reloading
EOF
    exit 2
fi

KVER="$(uname -r)"
KBUILD="/lib/modules/${KVER}/build"
SRC_DIR="$(cd "$(dirname "$0")"/.. && pwd)"
MOD_DIR="/lib/modules/${KVER}/kernel/drivers/net/wireless/aml-w1"

if [[ ! -d "${KBUILD}" ]]; then
    echo "ERROR: kernel headers not found at ${KBUILD}" >&2
    echo "Install matching headers first, e.g. apt install linux-headers-${KVER}" >&2
    exit 3
fi

# ===========================================================================
# 1. Stop NetworkManager from owning the aml interface while we reload
# ===========================================================================
echo "--- stopping wpa_supplicant on aml interface (best effort) ---"
# Cover the current name (w522a) plus legacy names that older
# vendor / patched builds used so a fresh hot-reload works
# regardless of which version is currently loaded.
for IFACE in w522a w522a_amlogic wlan0 wlan1 wlan2; do
    nmcli dev disconnect "${IFACE}" 2>/dev/null || true
done

# ===========================================================================
# 2. Build
# ===========================================================================
echo "--- building modules ---"
cd "${SRC_DIR}"
make clean >/dev/null 2>&1 || true
if ! make KDIR="${KBUILD}"; then
    echo "ERROR: build failed.  See output above." >&2
    exit 4
fi
ls -la aml_sdio.ko vlsicomm.ko || { echo "ERROR: .ko files missing"; exit 5; }

# ===========================================================================
# 3. Wipe any old aml-w1 modules and install the new ones
# ===========================================================================
echo "--- wiping old install ---"
rm -rf "${MOD_DIR}"
install -d "${MOD_DIR}"
install -m 644 aml_sdio.ko vlsicomm.ko "${MOD_DIR}/"

echo "--- installing firmware / RF config ---"
install -d /lib/firmware/aml /etc/aml-wifi
install -m 644 vmac/aml_wifi_rf_fn_link.txt /lib/firmware/aml/aml_wifi_rf_fn_link.txt
install -m 644 vmac/aml_wifi_rf_0333.txt    /lib/firmware/aml/aml_wifi_rf_0333.txt
install -m 644 vmac/aml_wifi_rf_fn_link.txt /lib/firmware/aml/aml_wifi_rf.txt
install -m 644 vmac/aml_wifi_rf_fn_link.txt /etc/aml-wifi/aml_wifi_rf.txt
install -m 644 vmac/aml_wifi_drv_cfg_0.conf /etc/aml-wifi/

echo "--- installing auto-load configs ---"
install -m 644 install_files/aml-w1.conf    /etc/modprobe.d/aml-w1.conf
install -m 644 install_files/aml-w1.modules /etc/modules-load.d/aml-w1.conf

depmod -a "${KVER}"

# ===========================================================================
# 4. Either reboot, or hot-reload the modules now
# ===========================================================================
if [[ "${REBOOT_FLAG}" == "--reboot" ]]; then
    cat <<EOF

============================================================
 Modules installed.  Rebooting now (--reboot was requested).
 After the box comes back, run the connect+speedtest part:
     sudo $0 ${SSID} ${PSK}
 (without --reboot, with the same SSID/PSK).
============================================================
EOF
    sync
    reboot
    exit 0
fi

echo "--- hot-reloading modules ---"
# 4a. delete stale NM profiles tied to old interface names from
#     earlier vendor / patched builds.
for OLD in $(nmcli -t -f UUID,DEVICE con show 2>/dev/null \
             | awk -F: '$2=="w522a_amlogic"{print $1}'); do
    nmcli con delete "${OLD}" 2>/dev/null || true
done

# 4b. unload in reverse-dependency order
rmmod vlsicomm 2>/dev/null || true
sleep 1
rmmod aml_sdio 2>/dev/null || true
sleep 2

# 4c. load the new ones
modprobe aml_sdio || { echo "ERROR: modprobe aml_sdio failed"; lsmod | grep aml; exit 6; }
sleep 3   # give vlsicomm time to come up via softdep + register the netdev

# ===========================================================================
# 5. Wait for the new w522a interface
# ===========================================================================
echo "--- waiting for new w522a interface ---"
WLAN_IF=""
for i in $(seq 1 20); do
    # Preferred match: the exact 'w522a' name registered by this driver.
    if [[ -d "/sys/class/net/w522a/wireless" ]]; then
        WLAN_IF="w522a"
        break
    fi
    # Fall back: any wireless netdev whose driver is NOT mt7601u and
    # is NOT one of the usual mainline drivers. This keeps the script
    # useful if someone insmod's vlsicomm with `vmac0=...` override.
    for cand in $(ls /sys/class/net/ 2>/dev/null); do
        [[ -d "/sys/class/net/${cand}/wireless" ]] || continue
        DRIVER=$(readlink "/sys/class/net/${cand}/device/driver" 2>/dev/null | sed 's:.*/::')
        if [[ "${DRIVER}" == "mt7601u" ]]; then continue; fi
        # accept anything else (w522a*, wlanX, w522a_amlogic, ...)
        WLAN_IF="${cand}"
        break
    done
    [[ -n "${WLAN_IF}" ]] && break
    echo "[wait ${i}/20] interface not ready yet…"
    sleep 1
done

if [[ -z "${WLAN_IF}" ]]; then
    echo "ERROR: no aml-w1 wireless interface appeared after 20s" >&2
    echo "--- ip link --- "; ip link
    echo "--- iw dev ---"; iw dev
    echo "--- lsmod ---"; lsmod | grep -E 'aml_sdio|vlsicomm'
    echo "--- dmesg (last 80 lines) ---"; dmesg | tail -80
    exit 7
fi

echo "Found w1-aml interface: ${WLAN_IF}"
ip link set "${WLAN_IF}" up || true

# ===========================================================================
# 6. Connect via nmcli
# ===========================================================================
echo "--- connecting ${WLAN_IF} → ${SSID} ---"
PROFILE="w1_${SSID}"
# kill stale profile of same name
nmcli con delete "${PROFILE}" 2>/dev/null || true
# build a fresh one bound to our specific interface
if ! nmcli con add type wifi \
        ifname "${WLAN_IF}" \
        con-name "${PROFILE}" \
        ssid "${SSID}"; then
    echo "ERROR: nmcli con add failed"
    exit 8
fi
nmcli con modify "${PROFILE}" \
    wifi-sec.key-mgmt wpa-psk \
    wifi-sec.psk "${PSK}" \
    ipv4.method auto \
    ipv6.method auto \
    connection.autoconnect yes

# trigger one explicit scan first (sometimes nmcli needs it)
echo "--- explicit scan ---"
nmcli dev wifi rescan ifname "${WLAN_IF}" 2>/dev/null || true
sleep 6
nmcli dev wifi list ifname "${WLAN_IF}" | head -30

echo "--- nmcli con up ${PROFILE} ---"
nmcli --wait 30 con up "${PROFILE}" || {
    echo "WARN: first attempt failed, retrying once after 5 s…"
    sleep 5
    nmcli dev wifi rescan ifname "${WLAN_IF}" 2>/dev/null || true
    sleep 5
    nmcli --wait 30 con up "${PROFILE}" || {
        echo "ERROR: nmcli con up ${PROFILE} failed"
        echo "--- nmcli dev status ---"; nmcli dev status
        echo "--- ip a show ${WLAN_IF} ---"; ip a show "${WLAN_IF}"
        echo "--- iw dev ${WLAN_IF} link ---"; iw dev "${WLAN_IF}" link 2>/dev/null
        echo "--- dmesg (last 80 lines) ---"; dmesg | tail -80
        echo "--- journalctl wpa_supplicant (last 40) ---"
        journalctl -u wpa_supplicant -n 40 --no-pager 2>/dev/null
        echo "--- journalctl NetworkManager (last 40) ---"
        journalctl -u NetworkManager -n 40 --no-pager 2>/dev/null
        exit 9
    }
}

# ===========================================================================
# 7. Verify and benchmark
# ===========================================================================
echo "--- ip a show ${WLAN_IF} ---"
ip a show "${WLAN_IF}"

echo "--- iw dev ${WLAN_IF} link ---"
iw dev "${WLAN_IF}" link 2>/dev/null || true

echo "--- ping gateway ---"
GW=$(ip route show dev "${WLAN_IF}" | awk '/default/{print $3; exit}')
[[ -n "${GW}" ]] && ping -c 5 -I "${WLAN_IF}" "${GW}"

echo "--- ping 8.8.8.8 ---"
ping -c 5 -I "${WLAN_IF}" 8.8.8.8 || true

echo "--- 5 s curl throughput test ---"
if command -v curl >/dev/null; then
    curl --interface "${WLAN_IF}" -fsSL -o /dev/null -w \
        '  speed_download=%{speed_download} bytes/s\n  time_total=%{time_total} s\n' \
        --max-time 15 \
        http://speedtest.tele2.net/10MB.zip 2>&1 || true
fi

echo "--- speedtest-cli (if available) ---"
if command -v speedtest-cli >/dev/null; then
    speedtest-cli --simple --source "$(ip -4 -o addr show "${WLAN_IF}" | awk '{print $4}' | cut -d/ -f1)" 2>&1 || true
elif command -v speedtest >/dev/null; then
    speedtest --accept-license --accept-gdpr -i "$(ip -4 -o addr show "${WLAN_IF}" | awk '{print $4}' | cut -d/ -f1)" 2>&1 || true
else
    echo "speedtest-cli not installed.  Install with: sudo apt install speedtest-cli"
fi

# ===========================================================================
# 8. Snapshot dmesg / journals for the bug report
# ===========================================================================
{
    echo
    echo "=========================================================="
    echo "FULL DMESG (T-format)"
    echo "=========================================================="
    dmesg --color=never -T
    echo
    echo "=========================================================="
    echo "journalctl NetworkManager last 200"
    echo "=========================================================="
    journalctl -u NetworkManager -n 200 --no-pager 2>/dev/null
    echo
    echo "=========================================================="
    echo "journalctl wpa_supplicant last 200"
    echo "=========================================================="
    journalctl -u wpa_supplicant -n 200 --no-pager 2>/dev/null
    echo
    echo "=========================================================="
    echo "lsmod / iw dev / ip a"
    echo "=========================================================="
    lsmod | grep -E 'aml_sdio|vlsicomm|cfg80211|mac80211|mt7601u'
    iw dev
    ip a
} >> "${LOG}"

echo
echo "============================================================"
echo " DONE.  Result log: ${LOG}"
echo " Send this file back so it can be diagnosed."
echo "============================================================"
