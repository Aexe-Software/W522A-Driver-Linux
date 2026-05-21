#!/bin/sh
# Connect w522a as a client and run a simple speedtest.
# Usage:
#   W522A_STA_SSID='YourSSID' W522A_STA_PSK='YourPassword' ./w522a-sta-test.sh
#   W522A_STA_SSID='YourSSID_5G' W522A_STA_PSK='YourPassword' W522A_STA_BSSID='aa:bb:cc:dd:ee:ff' ./w522a-sta-test.sh

set -eu

IFACE="${W522A_IFACE:-w522a}"
SSID="${W522A_STA_SSID:-}"
PSK="${W522A_STA_PSK:-}"
BSSID="${W522A_STA_BSSID:-}"
NAME="${W522A_STA_NAME:-w522a-sta-test}"

if [ -z "$SSID" ] || [ -z "$PSK" ]; then
    echo "Set W522A_STA_SSID and W522A_STA_PSK" >&2
    exit 2
fi

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "Required command not found: $1" >&2
        exit 1
    }
}

need_cmd nmcli
need_cmd iw
need_cmd speedtest-cli

systemctl stop w522a-ap.service >/dev/null 2>&1 || true
nmcli con delete "$NAME" >/dev/null 2>&1 || true
nmcli dev disconnect "$IFACE" >/dev/null 2>&1 || true
ip link set "$IFACE" up >/dev/null 2>&1 || true
nmcli dev wifi rescan ifname "$IFACE" >/dev/null 2>&1 || true
sleep 5

if [ -n "$BSSID" ]; then
    nmcli dev wifi connect "$SSID" password "$PSK" ifname "$IFACE" bssid "$BSSID" name "$NAME"
else
    nmcli dev wifi connect "$SSID" password "$PSK" ifname "$IFACE" name "$NAME"
fi

sleep 8
src="$(ip -4 -o addr show dev "$IFACE" | sed -n 's/.*inet \([0-9.]*\)\/.*/\1/p' | head -1)"
echo "source=${src}"
iw dev "$IFACE" link || true
speedtest-cli --source "$src" --simple
