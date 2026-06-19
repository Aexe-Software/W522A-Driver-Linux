#!/usr/bin/env bash
# bt-maxsignal.sh — apply safe BT TX-power / link-robustness tweaks for W522A.
# Hardware TX power on the AML W155S2 is firmware-capped at ~+7 dBm (Class 2-ish).
# This script applies the SAFE tweaks that don't risk corrupting RF state.
# See docs/W522A-BT-TX-power.md for findings & what does NOT work.

set -e
HCI=${HCI:-hci0}
MAC=${1:-}

log()  { echo "[bt-maxsignal] $*"; }
die()  { echo "[bt-maxsignal] ERROR: $*" >&2; exit 1; }

command -v hcitool   >/dev/null || die "hcitool not installed (apt install bluez)"
command -v bluetoothctl >/dev/null || die "bluetoothctl missing"
hciconfig "$HCI" >/dev/null 2>&1 || die "interface $HCI not present"

log "1) Write Inquiry TX Power Level = +20 dBm (firmware may clamp to +7)"
hcitool -i "$HCI" cmd 0x03 0x59 0x14 >/dev/null 2>&1 || true
RESP=$(hcitool -i "$HCI" cmd 0x03 0x58 2>&1 | tail -1 | awk '{print $NF}')
log "   chip reports inquiry TX power: 0x$RESP dBm"

log "2) Tune BlueALSA AAC for distance (bitrate 192k VBR + afterburner)"
if systemctl is-active --quiet bluealsa; then
    OVERRIDE=/etc/systemd/system/bluealsa.service.d/override.conf
    if [ ! -f "$OVERRIDE" ] || ! grep -q 'aac-bitrate=192000' "$OVERRIDE" 2>/dev/null; then
        mkdir -p /etc/systemd/system/bluealsa.service.d
        cat > "$OVERRIDE" <<'EOF'
[Service]
ExecStart=
ExecStart=/usr/bin/bluealsa -S -p a2dp-source -p a2dp-sink \
    -c sbc -c aac \
    --aac-afterburner --aac-vbr --aac-bitrate=192000 \
    --keep-alive=10 --sbc-quality=high --io-rt-priority=8
EOF
        systemctl daemon-reload
        systemctl restart bluealsa
        log "   bluealsa restarted with AAC bitrate=192k VBR afterburner"
    else
        log "   bluealsa override already present"
    fi
fi

log "3) If a device MAC is given, prefer AAC codec on its sink"
if [ -n "$MAC" ]; then
    if hcitool con 2>/dev/null | grep -q "$MAC"; then
        PCM="/org/bluealsa/$HCI/dev_${MAC//:/_}/a2dpsrc/sink"
        bluealsa-cli codec "$PCM" AAC 2>/dev/null && \
            log "   $MAC sink codec → AAC" || \
            log "   $MAC AAC codec set failed (peer may only do SBC)"
        log "   RSSI=$(hcitool rssi $MAC 2>&1 | awk '{print $NF}')  TX=$(hcitool tpl $MAC 0 2>&1 | awk '{print $NF}') dBm"
    else
        log "   $MAC not connected; codec switch skipped"
    fi
fi

log "Done. NOTE: hardware TX is firmware-capped at +7 dBm; physical antenna"
log "      position has more impact than any software setting here."
