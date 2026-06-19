#!/usr/bin/env bash
# =============================================================================
#  W522A Bluetooth A2DP audio helper
#
#  WiFi and BT share ONE radio on this chip — they cannot run at the same time.
#  This script frees the radio from WiFi, brings up the BT controller, starts
#  bluez-alsa (AAC), and (optionally) pairs/connects a headset and plays a file.
#
#  Usage:
#     sudo ./bt-audio.sh up                  # free radio + bring BT up + bluealsa
#     sudo ./bt-audio.sh pair <MAC>          # pair+trust+connect a headset
#     sudo ./bt-audio.sh play <MAC> <file>   # play an mp3 to the headset (AAC)
#     sudo ./bt-audio.sh wifi-back           # re-enable WiFi (needs reboot)
#
#  NOTE: "up" requires the WiFi driver NOT to be loaded (it holds the radio).
#        If WiFi is loaded, this script disables its autoload and asks you to
#        reboot once; after reboot run "up" again.
# =============================================================================
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }
SERDEV="serial0-0"
DRV=/sys/bus/serial/drivers/hci_uart_aml

cmd_up(){
    if lsmod | grep -q vlsicomm; then
        echo "[bt] WiFi driver (vlsicomm) is loaded and holds the shared radio."
        echo "[bt] Disabling WiFi autoload. REBOOT once, then run: sudo ./bt-audio.sh up"
        systemctl disable w522a-ap 2>/dev/null || true
        [ -f /etc/modules-load.d/w522a.conf ] && \
            mv /etc/modules-load.d/w522a.conf /etc/modules-load.d/w522a.conf.disabled
        echo "[bt] now: sudo reboot"
        exit 0
    fi
    echo "[bt] binding BT serdev..."
    echo "$SERDEV" > "$DRV/bind" 2>/dev/null || true
    sleep 3
    rfkill unblock bluetooth 2>/dev/null || true
    hciconfig hci0 up 2>/dev/null || true
    systemctl start bluetooth 2>/dev/null || true
    sleep 1
    pgrep -x bluealsa >/dev/null || \
        (bluealsa -S -p a2dp-source -c aac --aac-bitrate=256000 >/dev/null 2>&1 &)
    sleep 2
    hciconfig hci0 2>/dev/null | grep -E 'BD Address|UP RUNNING' || echo "[bt] hci0 not up — check dmesg"
    echo "[bt] ready. Pair:  sudo ./bt-audio.sh pair <MAC>"
}

cmd_pair(){
    M="${1:?usage: pair <MAC>}"
    bluetoothctl --timeout 3 power on >/dev/null 2>&1 || true
    echo "[bt] put the headset in pairing mode, scanning 15s..."
    bluetoothctl --timeout 15 scan on >/dev/null 2>&1 || true
    bluetoothctl --timeout 10 pair "$M"    2>&1 | tail -1 || true
    bluetoothctl trust "$M"                2>&1 | tail -1 || true
    bluetoothctl --timeout 10 connect "$M" 2>&1 | tail -1 || true
    bluetoothctl info "$M" 2>/dev/null | grep -E 'Connected|Paired|Name'
}

cmd_play(){
    M="${1:?usage: play <MAC> <file.mp3>}"; F="${2:?need an mp3 file}"
    [ -f "$F" ] || { echo "no such file: $F"; exit 1; }
    pkill mpg123 2>/dev/null || true; sleep 1
    echo "[bt] playing $F -> $M (AAC). Ctrl-C to stop."
    mpg123 -o alsa -a "bluealsa:DEV=$M,PROFILE=a2dp" "$F"
}

cmd_wifi_back(){
    echo "$SERDEV" > "$DRV/unbind" 2>/dev/null || true
    [ -f /etc/modules-load.d/w522a.conf.disabled ] && \
        mv /etc/modules-load.d/w522a.conf.disabled /etc/modules-load.d/w522a.conf
    systemctl enable w522a-ap 2>/dev/null || true
    echo "[bt] WiFi autoload restored. Reboot to load WiFi (radio handed back)."
}

case "${1:-}" in
    up)        cmd_up ;;
    pair)      shift; cmd_pair "$@" ;;
    play)      shift; cmd_play "$@" ;;
    wifi-back) cmd_wifi_back ;;
    *) echo "usage: $0 {up|pair <MAC>|play <MAC> <file.mp3>|wifi-back}"; exit 1 ;;
esac
