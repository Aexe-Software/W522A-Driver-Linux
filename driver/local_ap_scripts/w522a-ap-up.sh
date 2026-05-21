#!/bin/sh
# Bring up/down a W522A AP with persistent hostapd/dnsmasq configs.

set -eu

DEFAULTS=/etc/default/w522a-ap
[ -r "$DEFAULTS" ] && . "$DEFAULTS"

CMD="${1:-up}"
IFACE="${W522A_IFACE:-w522a}"
MODE="${W522A_AP_MODE:-2g-ht40-ch1}"
[ "$#" -lt 2 ] || MODE="$2"
COUNTRY="${W522A_COUNTRY:-UA}"
SSID="${W522A_SSID:-w522a-test-v16m}"
SECURITY="${W522A_SECURITY:-open}"
PASSPHRASE="${W522A_PASSPHRASE:-}"
ADDR="${W522A_ADDR:-192.168.77.1/24}"
DHCP_START="${W522A_DHCP_START:-192.168.77.10}"
DHCP_END="${W522A_DHCP_END:-192.168.77.100}"
DHCP_LEASE="${W522A_DHCP_LEASE:-12h}"
DNS="${W522A_DNS:-1.1.1.1,8.8.8.8}"
GATEWAY="${W522A_GATEWAY:-${ADDR%/*}}"
NAT="${W522A_NAT:-1}"
UPLINK="${W522A_UPLINK:-}"
TX_AMPDU="${W522A_TX_AMPDU:-off}"
TX_AMSDU="${W522A_TX_AMSDU:-off}"
UAPSD="${W522A_UAPSD:-0}"
BEACON_INT="${W522A_BEACON_INT:-50}"
DTIM_PERIOD="${W522A_DTIM_PERIOD:-1}"
TXQUEUELEN="${W522A_TXQUEUELEN:-100}"
QDISC="${W522A_QDISC:-fq_codel}"

RUN_DIR=/run/w522a
CFG_DIR=/etc/aml-wifi
HOSTAPD_CONF="${CFG_DIR}/w522a-hostapd.conf"
DNSMASQ_CONF="${CFG_DIR}/w522a-dnsmasq.conf"
HOSTAPD_PID="${RUN_DIR}/hostapd.pid"
DNSMASQ_PID="${RUN_DIR}/dnsmasq.pid"

need_root() {
    if [ "$(id -u)" -ne 0 ]; then
        echo "Run as root (try: sudo $0 $CMD)" >&2
        exit 1
    fi
}

need_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "Required command not found: $1" >&2
        exit 1
    fi
}

apply_low_latency_queue() {
    ip link set dev "$IFACE" txqueuelen "$TXQUEUELEN" >/dev/null 2>&1 || true
    case "$QDISC" in
        fq_codel)
            tc qdisc replace dev "$IFACE" root fq_codel \
                limit 256 target 5ms interval 100ms quantum 300 noecn \
                >/dev/null 2>&1 || true
            ;;
        none|"")
            tc qdisc del dev "$IFACE" root >/dev/null 2>&1 || true
            ;;
    esac
}

detect_uplink() {
    if [ -n "$UPLINK" ]; then
        echo "$UPLINK"
        return 0
    fi
    ip route show default 0.0.0.0/0 | awk '{print $5; exit}'
}

nat_up() {
    [ "$NAT" = "1" ] || return 0
    need_cmd iptables

    uplink="$(detect_uplink)"
    [ -n "$uplink" ] || return 0

    sysctl -w net.ipv4.ip_forward=1 >/dev/null 2>&1 || true
    iptables -C FORWARD -i "$IFACE" -o "$uplink" -j ACCEPT 2>/dev/null ||
        iptables -A FORWARD -i "$IFACE" -o "$uplink" -j ACCEPT
    iptables -C FORWARD -i "$uplink" -o "$IFACE" -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT 2>/dev/null ||
        iptables -A FORWARD -i "$uplink" -o "$IFACE" -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT
    iptables -t nat -C POSTROUTING -s "${ADDR%.*}.0/24" -o "$uplink" -j MASQUERADE 2>/dev/null ||
        iptables -t nat -A POSTROUTING -s "${ADDR%.*}.0/24" -o "$uplink" -j MASQUERADE
}

nat_down() {
    [ "$NAT" = "1" ] || return 0
    need_cmd iptables

    uplink="$(detect_uplink)"
    [ -n "$uplink" ] || return 0

    iptables -D FORWARD -i "$IFACE" -o "$uplink" -j ACCEPT 2>/dev/null || true
    iptables -D FORWARD -i "$uplink" -o "$IFACE" -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT 2>/dev/null || true
    iptables -t nat -D POSTROUTING -s "${ADDR%.*}.0/24" -o "$uplink" -j MASQUERADE 2>/dev/null || true
}

stop_pid() {
    pidfile="$1"
    if [ -s "$pidfile" ]; then
        kill "$(cat "$pidfile")" >/dev/null 2>&1 || true
        rm -f "$pidfile"
    fi
}

wait_iface() {
    i=0
    while [ "$i" -lt 20 ]; do
        ip link show "$IFACE" >/dev/null 2>&1 && return 0
        sleep 1
        i=$((i + 1))
    done
    echo "Interface ${IFACE} did not appear" >&2
    return 1
}

security_block() {
    case "$SECURITY" in
        open|"")
            ;;
        wpa2|sae|mixed)
            if [ "${#PASSPHRASE}" -lt 8 ]; then
                echo "W522A_PASSPHRASE must be at least 8 chars for ${SECURITY}" >&2
                exit 1
            fi
            echo "wpa=2"
            echo "wpa_passphrase=${PASSPHRASE}"
            echo "rsn_pairwise=CCMP"
            case "$SECURITY" in
                wpa2)
                    echo "wpa_key_mgmt=WPA-PSK"
                    ;;
                sae)
                    echo "wpa_key_mgmt=SAE"
                    echo "ieee80211w=2"
                    echo "sae_require_mfp=1"
                    ;;
                mixed)
                    echo "wpa_key_mgmt=WPA-PSK SAE"
                    echo "ieee80211w=1"
                    ;;
            esac
            ;;
        *)
            echo "Unsupported W522A_SECURITY=${SECURITY}" >&2
            exit 1
            ;;
    esac
}

write_hostapd_conf() {
    mkdir -p "$CFG_DIR"
    {
        echo "interface=${IFACE}"
        echo "driver=nl80211"
        echo "ssid=${SSID}"
        echo "country_code=${COUNTRY}"
        echo "ieee80211d=1"
        if [ "$MODE" = "2g-legacy" ] || [ "$MODE" = "2g-g" ] || [ "$MODE" = "2g-legacy-ch1" ]; then
            echo "wmm_enabled=0"
        else
            echo "wmm_enabled=1"
        fi
        echo "ignore_broadcast_ssid=0"
        echo "logger_syslog=-1"
        echo "logger_syslog_level=2"
        echo "beacon_int=${BEACON_INT}"
        echo "dtim_period=${DTIM_PERIOD}"
        echo "uapsd_advertisement_enabled=${UAPSD}"
        case "$MODE" in
            5g-ht20)
                # 20 MHz, MCS0-7 SGI = up to ~72 Mbps PHY.  Most conservative,
                # best range, lowest chance of co-channel interference.
                echo "hw_mode=a"
                echo "channel=36"
                echo "ieee80211n=1"
                echo "ht_capab=[LDPC][SHORT-GI-20][TX-STBC][RX-STBC1][MAX-AMSDU-7935]"
                ;;
            5g-ht40)
                # 40 MHz, MCS0-7 SGI = up to ~150 Mbps PHY.  Default — good
                # balance of speed and stability on a SISO 1T1R chip.
                echo "hw_mode=a"
                echo "channel=36"
                echo "ieee80211n=1"
                echo "ht_capab=[HT40+][LDPC][SHORT-GI-20][SHORT-GI-40][TX-STBC][RX-STBC1][MAX-AMSDU-7935]"
                ;;
            5g-vht80|5g-ht80)
                # 80 MHz, VHT MCS0-9 SGI = up to ~390 Mbps PHY.  Maximum speed
                # but most sensitive to interference / DFS / weak signal.
                echo "hw_mode=a"
                echo "channel=36"
                echo "ieee80211n=1"
                echo "ieee80211ac=1"
                echo "ht_capab=[HT40+][LDPC][SHORT-GI-20][SHORT-GI-40][TX-STBC][RX-STBC1][MAX-AMSDU-7935]"
                echo "vht_capab=[RXLDPC][SHORT-GI-80][TX-STBC-2BY1][RX-STBC-1][SU-BEAMFORMEE][MAX-MPDU-11454][MAX-A-MPDU-LEN-EXP7]"
                echo "vht_oper_chwidth=1"
                echo "vht_oper_centr_freq_seg0_idx=42"
                ;;
            2g-ht20)
                echo "hw_mode=g"
                echo "channel=6"
                echo "supported_rates=60 90 120 180 240 360 480 540"
                echo "basic_rates=60 120 240"
                echo "ieee80211n=1"
                echo "ht_capab=[LDPC][SHORT-GI-20][TX-STBC][RX-STBC1][MAX-AMSDU-7935]"
                ;;
            2g-n20-safe)
                echo "hw_mode=g"
                echo "channel=6"
                echo "supported_rates=60 90 120 180 240 360 480 540"
                echo "basic_rates=60 120 240"
                echo "ieee80211n=1"
                echo "ht_capab=[SHORT-GI-20]"
                ;;
            2g-n20-safe-ch1)
                echo "hw_mode=g"
                echo "channel=1"
                echo "supported_rates=60 90 120 180 240 360 480 540"
                echo "basic_rates=60 120 240"
                echo "ieee80211n=1"
                echo "ht_capab=[SHORT-GI-20]"
                ;;
            2g-legacy|2g-g)
                echo "hw_mode=g"
                echo "channel=6"
                echo "supported_rates=60 90 120 180 240 360 480 540"
                echo "basic_rates=60 120 240"
                ;;
            2g-legacy-ch1)
                echo "hw_mode=g"
                echo "channel=1"
                echo "supported_rates=60 90 120 180 240 360 480 540"
                echo "basic_rates=60 120 240"
                ;;
            2g-ht40)
                echo "hw_mode=g"
                echo "channel=6"
                echo "ieee80211n=1"
                echo "ht_capab=[HT40+][LDPC][SHORT-GI-20][SHORT-GI-40][TX-STBC][RX-STBC1][MAX-AMSDU-7935][DSSS_CCK-40]"
                ;;
            2g-ht40-ch1)
                # 2.4 GHz ch1 HT40+ (ch1+ch5).  Avoids conflict with home APs
                # on the typical ch6/ch11.  PHY rate up to ~150 Mbps.
                echo "hw_mode=g"
                echo "channel=1"
                echo "ieee80211n=1"
                echo "ht_capab=[HT40+][LDPC][SHORT-GI-20][SHORT-GI-40][TX-STBC][RX-STBC1][MAX-AMSDU-7935][DSSS_CCK-40]"
                ;;
            2g-ht20-ch1)
                # 2.4 GHz ch1 HT20.  Most conservative, max compatibility.
                echo "hw_mode=g"
                echo "channel=1"
                echo "ieee80211n=1"
                echo "ht_capab=[LDPC][SHORT-GI-20][TX-STBC][RX-STBC1][MAX-AMSDU-7935]"
                ;;
            5g-ht40-ch149)
                # 5 GHz ch149 HT40+ (U-NII-3, no DFS).  Clean band, avoids
                # typical home networks on ch36.  PHY rate up to ~150 Mbps.
                echo "hw_mode=a"
                echo "channel=149"
                echo "ieee80211n=1"
                echo "ht_capab=[HT40+][LDPC][SHORT-GI-20][SHORT-GI-40][TX-STBC][RX-STBC1][MAX-AMSDU-7935]"
                ;;
            5g-vht80-ch48)
                # 5 GHz ch48 primary, VHT80 centered on ch42. This avoids
                # ch36/ch149 while using a hostapd-accepted HT40 pair.
                echo "hw_mode=a"
                echo "channel=48"
                echo "ieee80211n=1"
                echo "ieee80211ac=1"
                echo "ht_capab=[HT40-][LDPC][SHORT-GI-20][SHORT-GI-40][TX-STBC][RX-STBC1][MAX-AMSDU-7935]"
                echo "vht_capab=[RXLDPC][SHORT-GI-80][TX-STBC-2BY1][RX-STBC-1][SU-BEAMFORMEE][MAX-MPDU-11454][MAX-A-MPDU-LEN-EXP7]"
                echo "vht_oper_chwidth=1"
                echo "vht_oper_centr_freq_seg0_idx=42"
                ;;
            5g-vht80-ch40)
                # 5 GHz ch40 primary, VHT80 centered on ch42. Avoids ch36
                # primary while staying in the lower non-DFS 80 MHz block.
                echo "hw_mode=a"
                echo "channel=40"
                echo "ieee80211n=1"
                echo "ieee80211ac=1"
                echo "ht_capab=[HT40+][LDPC][SHORT-GI-20][SHORT-GI-40][TX-STBC][RX-STBC1][MAX-AMSDU-7935]"
                echo "vht_capab=[RXLDPC][SHORT-GI-80][TX-STBC-2BY1][RX-STBC-1][SU-BEAMFORMEE][MAX-MPDU-11454][MAX-A-MPDU-LEN-EXP7]"
                echo "vht_oper_chwidth=1"
                echo "vht_oper_centr_freq_seg0_idx=42"
                ;;
            5g-vht80-ch44)
                # 5 GHz ch44 primary, VHT80 centered on ch42. Same 80 MHz
                # block as ch36, but a different primary channel for clients
                # that behave poorly on ch36 or ch149.
                echo "hw_mode=a"
                echo "channel=44"
                echo "ieee80211n=1"
                echo "ieee80211ac=1"
                echo "ht_capab=[HT40+][LDPC][SHORT-GI-20][SHORT-GI-40][TX-STBC][RX-STBC1][MAX-AMSDU-7935]"
                echo "vht_capab=[RXLDPC][SHORT-GI-80][TX-STBC-2BY1][RX-STBC-1][SU-BEAMFORMEE][MAX-MPDU-11454][MAX-A-MPDU-LEN-EXP7]"
                echo "vht_oper_chwidth=1"
                echo "vht_oper_centr_freq_seg0_idx=42"
                ;;
            5g-vht80-ch149)
                # 5 GHz ch149 VHT80 (U-NII-3, no DFS).  PHY rate up to ~390 Mbps.
                echo "hw_mode=a"
                echo "channel=149"
                echo "ieee80211n=1"
                echo "ieee80211ac=1"
                echo "ht_capab=[HT40+][LDPC][SHORT-GI-20][SHORT-GI-40][TX-STBC][RX-STBC1][MAX-AMSDU-7935]"
                echo "vht_capab=[RXLDPC][SHORT-GI-80][TX-STBC-2BY1][RX-STBC-1][SU-BEAMFORMEE][MAX-MPDU-11454][MAX-A-MPDU-LEN-EXP7]"
                echo "vht_oper_chwidth=1"
                echo "vht_oper_centr_freq_seg0_idx=155"
                ;;
            *)
                echo "Unsupported W522A_AP_MODE=${MODE}" >&2
                exit 1
                ;;
        esac
        echo "obss_interval=0"
        security_block
    } > "$HOSTAPD_CONF"
}

write_dnsmasq_conf() {
    mkdir -p "$CFG_DIR"
    {
        echo "interface=${IFACE}"
        echo "bind-interfaces"
        echo "port=0"
        echo "no-resolv"
        echo "no-poll"
        echo "dhcp-authoritative"
        echo "dhcp-range=${DHCP_START},${DHCP_END},${DHCP_LEASE}"
        echo "dhcp-option=3,${GATEWAY}"
        echo "dhcp-option=6,${DNS}"
    } > "$DNSMASQ_CONF"
}

up() {
    mkdir -p "$RUN_DIR"

    need_cmd ip
    need_cmd modprobe
    need_cmd hostapd
    need_cmd dnsmasq

    modprobe aml_sdio || true
    sleep 3
    modprobe vlsicomm || true
    wait_iface

    ip link set "$IFACE" up
    ip addr replace "$ADDR" dev "$IFACE"
    apply_low_latency_queue
    iw dev "$IFACE" set power_save off >/dev/null 2>&1 || true

    write_hostapd_conf
    write_dnsmasq_conf

    stop_pid "$DNSMASQ_PID"
    stop_pid "$HOSTAPD_PID"

    hostapd -B "$HOSTAPD_CONF" -P "$HOSTAPD_PID"
    if command -v iwpriv >/dev/null 2>&1; then
        [ "$TX_AMPDU" = "keep" ] || iwpriv "$IFACE" set_ampdu "$TX_AMPDU" >/dev/null 2>&1 || true
        [ "$TX_AMSDU" = "keep" ] || iwpriv "$IFACE" set_amsdu "$TX_AMSDU" >/dev/null 2>&1 || true
        iwpriv "$IFACE" set_uapsd "$UAPSD" >/dev/null 2>&1 || true
        iwpriv "$IFACE" set_pwr_save 0 >/dev/null 2>&1 || true
    fi
    dnsmasq -C "$DNSMASQ_CONF" -x "$DNSMASQ_PID"
    nat_up

    echo "W522A AP up: iface=${IFACE} mode=${MODE} ssid=${SSID} addr=${ADDR}"
}

down() {
    nat_down
    stop_pid "$DNSMASQ_PID"
    stop_pid "$HOSTAPD_PID"
    ip addr flush dev "$IFACE" >/dev/null 2>&1 || true
    echo "W522A AP down: iface=${IFACE}"
}

need_root
case "$CMD" in
    up) up ;;
    down|stop) down ;;
    restart)
        down
        up
        ;;
    *)
        echo "Usage: $0 {up|down|restart} [5g-ht20|5g-ht40|5g-vht80|5g-ht40-ch149|5g-vht80-ch149|2g-legacy|2g-legacy-ch1|2g-n20-safe|2g-n20-safe-ch1|2g-ht20|2g-ht40|2g-ht40-ch1|2g-ht20-ch1]" >&2
        exit 2
        ;;
esac
