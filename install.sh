#!/usr/bin/env bash
# =============================================================================
#  W522A / W155S1 driver installer  (X96 Max Plus W100, kernel 6.12.81-ophub)
#
#  Usage:
#     sudo ./install.sh                 # interactive menu
#     sudo ./install.sh prebuilt        # install the prebuilt .ko (no compiler)
#     sudo ./install.sh build           # install gcc-15, compile, install
#     sudo ./install.sh uninstall       # remove driver + autoload
#
#  After install the WiFi driver auto-loads on every boot as module "vlsicomm".
#  (Bluetooth shares the radio — see README; BT setup is a separate step.)
# =============================================================================
set -euo pipefail

KREL="$(uname -r)"
KMODDIR="/lib/modules/${KREL}/kernel/drivers/net/wireless/aml-w1"
HERE="$(cd "$(dirname "$0")" && pwd)"
GREEN='\033[0;92m'; YEL='\033[0;93m'; RED='\033[0;91m'; NC='\033[0m'
say(){ echo -e "${GREEN}[w522a]${NC} $*"; }
warn(){ echo -e "${YEL}[w522a]${NC} $*"; }
die(){ echo -e "${RED}[w522a] ERROR:${NC} $*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "run as root (sudo)."

install_configs(){
    say "Installing configs, RF tables and module params..."
    mkdir -p /etc/aml-wifi /lib/firmware/aml /etc/modprobe.d /etc/modules-load.d
    cp -f "$HERE"/config/aml_wifi_drv_cfg_*.conf /etc/aml-wifi/         2>/dev/null || true
    cp -f "$HERE"/config/ap-*.conf               /etc/aml-wifi/         2>/dev/null || true
    cp -f "$HERE"/firmware/aml_wifi_rf*.txt       /lib/firmware/aml/     2>/dev/null || true
    cp -f "$HERE"/config/modprobe.d/*.conf        /etc/modprobe.d/       2>/dev/null || true
    # autoload the module on boot
    echo "vlsicomm" > /etc/modules-load.d/w522a.conf
    # optional helper script
    if [ -f "$HERE/scripts/start_ap.sh" ]; then
        cp -f "$HERE/scripts/start_ap.sh" /root/start_ap.sh && chmod +x /root/start_ap.sh
    fi
}

load_now(){
    say "Loading driver now..."
    depmod -a "${KREL}"
    modprobe aml_sdio 2>/dev/null || true
    modprobe vlsicomm 2>/dev/null || true
    sleep 2
    if lsmod | grep -q vlsicomm; then
        say "vlsicomm loaded. Interface:"
        ip -br link show 2>/dev/null | grep -i w522a || warn "w522a interface not up yet (check dmesg)."
    else
        warn "vlsicomm did not load — check 'dmesg | tail'."
    fi
}

do_prebuilt(){
    [ -f "$HERE/prebuilt-modules/vlsicomm.ko" ] || die "prebuilt-modules/vlsicomm.ko missing."
    say "Installing PREBUILT modules for ${KREL}..."
    # sanity: prebuilt was built for 6.12.81-ophub; warn on mismatch
    if ! modinfo "$HERE/prebuilt-modules/vlsicomm.ko" 2>/dev/null | grep -q "${KREL}"; then
        warn "prebuilt module vermagic != running kernel ${KREL}. It may fail to load."
        warn "If it does, run:  sudo ./install.sh build"
    fi
    mkdir -p "$KMODDIR"
    cp -f "$HERE/prebuilt-modules/vlsicomm.ko" "$HERE/prebuilt-modules/aml_sdio.ko" "$KMODDIR/"
    install_configs
    load_now
    say "Done (prebuilt)."
}

ensure_gcc15(){
    if command -v gcc-15 >/dev/null 2>&1; then say "gcc-15 present."; return; fi
    say "Installing gcc-15 (kernel 6.12.81-ophub was built with GCC 15)..."
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -y || warn "apt-get update had errors (continuing)."
    apt-get install -y --no-install-recommends gcc-15 build-essential bc || \
        die "could not install gcc-15. Add a toolchain repo or install it manually, then re-run."
    command -v gcc-15 >/dev/null 2>&1 || die "gcc-15 still not found after install."
}

do_build(){
    [ -d "$HERE/source" ] || die "source/ directory missing."
    [ -d "/lib/modules/${KREL}/build" ] || die "kernel headers for ${KREL} missing (install linux-headers-${KREL})."
    ensure_gcc15
    say "Building driver with gcc-15 ..."
    make -C "$HERE/source" clean >/dev/null 2>&1 || true
    make -C "$HERE/source" -j"$(nproc)" CC=gcc-15 KBUILD_BUILD_TIMESTAMP='' \
        || die "build failed (see output above)."
    [ -f "$HERE/source/vlsicomm.ko" ] || die "build produced no vlsicomm.ko."
    say "Build OK. Installing..."
    mkdir -p "$KMODDIR"
    cp -f "$HERE/source/vlsicomm.ko" "$HERE/source/aml_sdio.ko" "$KMODDIR/"
    install_configs
    load_now
    say "Done (built from source)."
}

do_uninstall(){
    warn "Uninstalling. NOTE: do NOT rmmod vlsicomm on a running system (kernel Oops). A reboot is cleaner."
    rm -f /etc/modules-load.d/w522a.conf
    rm -f "$KMODDIR/vlsicomm.ko" "$KMODDIR/aml_sdio.ko"
    depmod -a "${KREL}" || true
    say "Removed module files + autoload. Reboot to fully unload."
}

menu(){
    echo "==============================================="
    echo "  W522A / W155S1 driver installer  (k=${KREL})"
    echo "==============================================="
    echo "  1) Install PREBUILT module (fastest, no compiler)"
    echo "  2) BUILD from source (installs gcc-15) + install"
    echo "  3) Uninstall"
    echo "  q) quit"
    read -rp "choose: " a
    case "$a" in
        1) do_prebuilt ;;
        2) do_build ;;
        3) do_uninstall ;;
        q|Q) exit 0 ;;
        *) die "invalid choice." ;;
    esac
}

case "${1:-menu}" in
    prebuilt)   do_prebuilt ;;
    build)      do_build ;;
    uninstall)  do_uninstall ;;
    menu|"")    menu ;;
    *)          die "unknown command '$1' (use: prebuilt | build | uninstall)" ;;
esac
