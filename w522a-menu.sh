#!/bin/sh
# Minimal W522A helper menu.
# Run as root from the driver source directory or from a ready bundle.

set -eu

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
KVER="$(uname -r)"
MOD_DIR="/lib/modules/${KVER}/kernel/drivers/net/wireless/aml-w1"

need_root() {
    if [ "$(id -u)" -ne 0 ]; then
        echo "Run as root" >&2
        exit 1
    fi
}

install_deps() {
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    apt-get install -y make perl kmod bc bison flex libssl-dev libelf-dev \
        dwarves hostapd dnsmasq iptables iw wireless-tools network-manager \
        curl speedtest-cli
    if ! command -v gcc-15 >/dev/null 2>&1; then
        apt-get install -y software-properties-common
        add-apt-repository -y ppa:ubuntu-toolchain-r/test || true
        apt-get update
        apt-get install -y gcc-15 g++-15 || apt-get install -y gcc g++
    fi
}

install_ready() {
    need_root
    install_deps
    install -d "$MOD_DIR" /etc/aml-wifi /etc/modprobe.d /etc/modules-load.d \
        /usr/local/sbin /etc/default /etc/systemd/system

    if [ -f "$ROOT_DIR/aml_sdio.ko" ] && [ -f "$ROOT_DIR/vlsicomm.ko" ]; then
        install -m 644 "$ROOT_DIR/aml_sdio.ko" "$ROOT_DIR/vlsicomm.ko" "$MOD_DIR/"
    elif [ -f "$ROOT_DIR/built/aml_sdio.ko" ] && [ -f "$ROOT_DIR/built/vlsicomm.ko" ]; then
        install -m 644 "$ROOT_DIR/built/aml_sdio.ko" "$ROOT_DIR/built/vlsicomm.ko" "$MOD_DIR/"
    else
        echo "Ready .ko files not found; use option 2 to build." >&2
        exit 1
    fi

    [ -f "$ROOT_DIR/vmac/aml_wifi_rf_fn_link.txt" ] &&
        install -m 644 "$ROOT_DIR/vmac/aml_wifi_rf_fn_link.txt" /etc/aml-wifi/aml_wifi_rf.txt || true
    [ -f "$ROOT_DIR/vmac/aml_wifi_drv_cfg_0.conf" ] &&
        install -m 644 "$ROOT_DIR/vmac/aml_wifi_drv_cfg_0.conf" /etc/aml-wifi/ || true
    [ -f "$ROOT_DIR/install_files/aml-w1.conf" ] &&
        install -m 644 "$ROOT_DIR/install_files/aml-w1.conf" /etc/modprobe.d/aml-w1.conf || true
    [ -f "$ROOT_DIR/install_files/aml-w1.modules" ] &&
        install -m 644 "$ROOT_DIR/install_files/aml-w1.modules" /etc/modules-load.d/aml-w1.conf || true
    [ -f "$ROOT_DIR/local_ap_scripts/w522a-ap-up.sh" ] &&
        install -m 755 "$ROOT_DIR/local_ap_scripts/w522a-ap-up.sh" /usr/local/sbin/w522a-ap-up.sh || true
    [ -f "$ROOT_DIR/local_ap_scripts/w522a-sta-test.sh" ] &&
        install -m 755 "$ROOT_DIR/local_ap_scripts/w522a-sta-test.sh" /usr/local/sbin/w522a-sta-test.sh || true
    [ -f "$ROOT_DIR/local_ap_scripts/w522a-ap.default" ] &&
        install -m 600 "$ROOT_DIR/local_ap_scripts/w522a-ap.default" /etc/default/w522a-ap || true
    [ -f "$ROOT_DIR/local_ap_scripts/w522a-ap.service" ] &&
        install -m 644 "$ROOT_DIR/local_ap_scripts/w522a-ap.service" /etc/systemd/system/w522a-ap.service || true

    depmod -a "$KVER"
    systemctl daemon-reload
    systemctl enable w522a-ap.service >/dev/null 2>&1 || true
    modprobe aml_sdio || true
    modprobe vlsicomm || true
    systemctl restart w522a-ap.service || true
    echo "Installed ready W522A AP profile."
}

build_install() {
    need_root
    install_deps
    make -C "$ROOT_DIR" BUILD_CC="${BUILD_CC:-gcc-15}"
    make -C "$ROOT_DIR" BUILD_CC="${BUILD_CC:-gcc-15}" install
    install_ready
}

case "${1:-menu}" in
    ready|install-ready|1)
        install_ready
        ;;
    build|build-install|2)
        build_install
        ;;
    menu|*)
        echo "W522A helper"
        echo "1) install ready modules + AP configs"
        echo "2) build with gcc-15, install dependencies, modules and AP configs"
        printf "Select: "
        read ans
        case "$ans" in
            1) install_ready ;;
            2) build_install ;;
            *) echo "Nothing selected" ;;
        esac
        ;;
esac
