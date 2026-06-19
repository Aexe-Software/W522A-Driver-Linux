#!/bin/bash
#
# Install W522A (Amlogic W155S1) Bluetooth UART stack + BlueALSA-AAC audio.
#
# Two install modes:
#   binary  — drop precompiled .ko, .bin, bluealsa-binaries; install runtime
#             dependencies via apt (no compiler needed). Fast.
#   source  — apt-install gcc-15 + kernel-headers + build deps, then compile
#             everything from source. Use if your kernel != 6.12.81-ophub or
#             if you want to verify the build.
#
# Usage:
#   sudo ./install.sh                       # interactive menu
#   sudo ./install.sh binary
#   sudo ./install.sh source
#   sudo ./install.sh --help
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
KVER="$(uname -r)"
ARCH="$(uname -m)"

bold()  { echo -e "\033[1m$*\033[0m"; }
green() { echo -e "\033[1;32m$*\033[0m"; }
red()   { echo -e "\033[1;31m$*\033[0m" >&2; }
warn()  { echo -e "\033[1;33m$*\033[0m"; }

[ "$(id -u)" -eq 0 ] || { red "Run as root (sudo $0)"; exit 1; }
[ "$ARCH" = "aarch64" ] || warn "WARNING: not aarch64 ($ARCH), proceeding anyway"

MODE="${1:-}"
case "${MODE,,}" in
  --help|-h|help)
    sed -n '2,18p' "$0" | sed 's/^#//'
    exit 0
    ;;
  binary|bin|prebuilt)
    MODE=binary
    ;;
  source|src|compile|build)
    MODE=source
    ;;
  "")
    echo
    bold "=== W522A Bluetooth installer ==="
    echo
    echo "Выбери режим установки:"
    echo "  1) BINARY — поставить готовые .ko/.bin/bluealsa из архива + runtime deps (быстро, без gcc)"
    echo "  2) SOURCE — установить gcc-15 и зависимости, собрать всё из исходников (медленнее)"
    echo "  3) EXIT"
    echo
    read -rp "Введи 1 / 2 / 3 [1]: " choice
    case "$choice" in
      ""|1) MODE=binary ;;
      2) MODE=source ;;
      3) exit 0 ;;
      *) red "Неизвестный выбор: $choice"; exit 1 ;;
    esac
    ;;
  *)
    red "Unknown mode: '$MODE'. Use 'binary', 'source', or omit for menu."
    exit 1
    ;;
esac

bold ">>> Mode  : $MODE"
bold ">>> Kernel: $KVER"
bold ">>> Arch  : $ARCH"
bold ">>> Dir   : $SCRIPT_DIR"
echo

export DEBIAN_FRONTEND=noninteractive
NEED_REBOOT=0

# -----------------------------------------------------------------------------
# Common: ensure multiverse, apt update
# -----------------------------------------------------------------------------
ensure_multiverse() {
  if ! grep -hRE '^[^#]*multiverse' /etc/apt/sources.list /etc/apt/sources.list.d/ \
         2>/dev/null | grep -q .; then
    apt-get install -y software-properties-common >/dev/null
    add-apt-repository -y multiverse >/dev/null
  fi
}

# -----------------------------------------------------------------------------
# Common: install firmware + DTB
# -----------------------------------------------------------------------------
install_firmware_and_dtb() {
  bold "[*] Installing Bluetooth firmware (aml_w155s2_bt_uart.bin)…"
  install -d /lib/firmware/amlogic
  install -m 0644 "$SCRIPT_DIR/firmware/aml_w155s2_bt_uart.bin" /lib/firmware/amlogic/

  bold "[*] Installing DTB (BT on serial@24000 / UART_A)…"
  local DTB_NAME="meson-sm1-x96-max-plus-w100-v2.1.dtb"
  local DTB_SRC="$SCRIPT_DIR/dtb/$DTB_NAME"
  if [ -f "$DTB_SRC" ] && [ -d /boot/dtb/amlogic ]; then
    if ! cmp -s "$DTB_SRC" "/boot/dtb/amlogic/$DTB_NAME" 2>/dev/null; then
      cp -f "/boot/dtb/amlogic/$DTB_NAME" \
            "/boot/dtb/amlogic/$DTB_NAME.bak.$(date +%s)" 2>/dev/null || true
      install -m 0644 "$DTB_SRC" "/boot/dtb/amlogic/$DTB_NAME"
      warn ">>> DTB updated — REBOOT required to activate the new device tree."
      NEED_REBOOT=1
    else
      green ">>> DTB already up-to-date."
    fi
  else
    warn ">>> DTB or /boot/dtb/amlogic missing — apply manually if needed."
  fi
}

# -----------------------------------------------------------------------------
# Common: install autoload + systemd overrides
# -----------------------------------------------------------------------------
install_autoload() {
  bold "[*] Installing modules-load.d + bluealsa systemd override…"
  install -d /etc/modules-load.d
  install -m 0644 "$SCRIPT_DIR/etc/modules-load.d/w522a-bt.conf" \
                  /etc/modules-load.d/w522a-bt.conf

  install -d /etc/systemd/system/bluealsa.service.d
  install -m 0644 "$SCRIPT_DIR/etc/systemd/system/bluealsa.service.d/override.conf" \
                  /etc/systemd/system/bluealsa.service.d/override.conf

  if [ -f "$SCRIPT_DIR/etc/asound.conf" ]; then
    bold "[*] Installing safe ALSA default (prevents accidental HDMI/noise playback)..."
    install -m 0644 "$SCRIPT_DIR/etc/asound.conf" /etc/asound.conf
  fi

  if [ -d "$SCRIPT_DIR/scripts" ]; then
    bold "[*] Installing Bluetooth safety helpers..."
    install -d /usr/local/sbin
    [ -f "$SCRIPT_DIR/scripts/w522a-bt-connect-m45" ] && \
      install -m 0755 "$SCRIPT_DIR/scripts/w522a-bt-connect-m45" /usr/local/sbin/
    [ -f "$SCRIPT_DIR/scripts/w522a-bt-safe-play" ] && \
      install -m 0755 "$SCRIPT_DIR/scripts/w522a-bt-safe-play" /usr/local/sbin/
    [ -f "$SCRIPT_DIR/scripts/bt-maxsignal.sh" ] && \
      install -m 0755 "$SCRIPT_DIR/scripts/bt-maxsignal.sh" /usr/local/sbin/
  fi

  if [ -f "$SCRIPT_DIR/etc/wireplumber/wireplumber.conf.d/51-bluez-stable.conf" ]; then
    bold "[*] Installing WirePlumber stable Bluetooth codec policy…"
    install -d /etc/wireplumber/wireplumber.conf.d
    install -m 0644 "$SCRIPT_DIR/etc/wireplumber/wireplumber.conf.d/51-bluez-stable.conf" \
                    /etc/wireplumber/wireplumber.conf.d/51-bluez-stable.conf
  fi

  depmod -a "${KVER}"
  systemctl daemon-reload
  systemctl enable bluetooth bluealsa >/dev/null 2>&1 || true
}

# -----------------------------------------------------------------------------
# Common: live load + sanity check
# -----------------------------------------------------------------------------
live_load() {
  bold "[*] Loading modules + restarting services…"
  modprobe aml_sdio              2>/dev/null || warn "(aml_sdio loadable at boot)"
  rmmod hci_uart                 2>/dev/null || true
  rmmod w522a_bt_reset           2>/dev/null || true

  modprobe w522a_bt_reset || red "WARNING: w522a_bt_reset failed (check dmesg)"
  sleep 1
  modprobe btintel btrtl btbcm   2>/dev/null || true
  modprobe hci_uart              || red "WARNING: hci_uart failed (check dmesg)"
  sleep 2

  systemctl restart bluealsa 2>/dev/null || true
  sleep 2

  bold ">>> Live status:"
  hciconfig -a hci0 2>/dev/null | head -8 || warn "hci0 not up yet — check dmesg"
}

# =============================================================================
# MODE: binary — drop prebuilt artefacts + apt-install runtime deps only.
# =============================================================================
do_binary() {
  bold "===== Binary install ====="
  ensure_multiverse
  apt-get update -qq

  RUNTIME_PKGS=(
    # BT stack + audio + tools
    bluez bluetooth alsa-utils
    mpg123
    # runtime libs the prebuilt bluealsa was linked against
    libasound2t64 libbluetooth3 libdbus-1-3 libglib2.0-0t64 libsbc1 libfdk-aac2
    # for module loading / hciconfig
    kmod
    wget
  )
  bold "[1/5] Installing runtime packages…"
  apt-get install -y "${RUNTIME_PKGS[@]}"

  bold "[2/5] Checking prebuilt kernel modules…"
  PREBUILT_KVER="$(cat "$SCRIPT_DIR/prebuilt/KERNEL_VERSION" 2>/dev/null || echo unknown)"
  if [ "$PREBUILT_KVER" != "$KVER" ]; then
    warn "Prebuilt .ko built for kernel '$PREBUILT_KVER', but running '$KVER'."
    warn "If 'modprobe' fails below, re-run installer with 'source' mode:"
    warn "    sudo $0 source"
  fi

  # Drop kernel modules
  install -d "/lib/modules/${KVER}/extra"
  install -m 0644 "$SCRIPT_DIR/src/w522a_bt_reset/w522a_bt_reset.ko" \
                  "/lib/modules/${KVER}/extra/"
  install -d "/lib/modules/${KVER}/updates/bluetooth"
  install -m 0644 "$SCRIPT_DIR/src/hci_uart/hci_uart.ko" \
                  "/lib/modules/${KVER}/updates/bluetooth/"

  bold "[3/5] Installing prebuilt BlueALSA (with AAC)…"
  # remove distro version (would shadow ours)
  apt-get remove -y bluez-alsa-utils >/dev/null 2>&1 || true

  P="$SCRIPT_DIR/prebuilt"
  install -d /usr/bin /usr/lib/aarch64-linux-gnu/alsa-lib \
             /usr/lib/systemd/system /usr/share/man/man7 \
             /usr/share/dbus-1/system.d /etc/alsa/conf.d
  install -m 0755 "$P/usr/bin/bluealsa"       /usr/bin/
  install -m 0755 "$P/usr/bin/bluealsa-aplay" /usr/bin/
  install -m 0755 "$P/usr/bin/bluealsa-cli"   /usr/bin/
  [ -f "$P/usr/lib/aarch64-linux-gnu/alsa-lib/libasound_module_pcm_bluealsa.so" ] && \
    install -m 0644 "$P/usr/lib/aarch64-linux-gnu/alsa-lib/libasound_module_pcm_bluealsa.so" \
                    /usr/lib/aarch64-linux-gnu/alsa-lib/
  [ -f "$P/usr/lib/aarch64-linux-gnu/alsa-lib/libasound_module_ctl_bluealsa.so" ] && \
    install -m 0644 "$P/usr/lib/aarch64-linux-gnu/alsa-lib/libasound_module_ctl_bluealsa.so" \
                    /usr/lib/aarch64-linux-gnu/alsa-lib/
  install -m 0644 "$P/usr/lib/systemd/system/bluealsa.service"       /usr/lib/systemd/system/
  install -m 0644 "$P/usr/lib/systemd/system/bluealsa-aplay.service" /usr/lib/systemd/system/
  install -m 0644 "$P/usr/share/dbus-1/system.d/bluealsa.conf"       /usr/share/dbus-1/system.d/
  install -m 0644 "$P/etc/alsa/conf.d/20-bluealsa.conf"              /etc/alsa/conf.d/
  [ -f "$P/usr/share/man/man7/bluealsa-plugins.7.gz" ] && \
    install -m 0644 "$P/usr/share/man/man7/bluealsa-plugins.7.gz"    /usr/share/man/man7/

  bold "[4/5] Installing firmware + DTB…"
  install_firmware_and_dtb

  bold "[5/5] Autoload + live load…"
  install_autoload
  live_load
}

# =============================================================================
# MODE: source — apt-install build deps, compile, install.
# =============================================================================
do_source() {
  bold "===== Source build & install ====="
  ensure_multiverse

  # gcc-15: prefer distro package if available, else ubuntu-toolchain-r PPA
  if ! apt-cache show gcc-15 >/dev/null 2>&1; then
    warn "gcc-15 not in repos, adding ubuntu-toolchain-r/test PPA"
    add-apt-repository -y ppa:ubuntu-toolchain-r/test
  fi
  apt-get update -qq

  BUILD_PKGS=(
    build-essential
    gcc-15 g++-15
    "linux-headers-${KVER}"
    bc bison flex kmod
    autoconf automake libtool pkg-config
    libasound2-dev libbluetooth-dev libdbus-1-dev libglib2.0-dev
    libsbc-dev libfdk-aac-dev libreadline-dev libncurses-dev
    bluez bluetooth alsa-utils mpg123
    git wget rsync
  )
  bold "[1/8] Installing build prerequisites…"
  apt-get install -y "${BUILD_PKGS[@]}"

  update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-15 60 \
                      --slave   /usr/bin/g++ g++ /usr/bin/g++-15 \
                      >/dev/null 2>&1 || true

  bold "[2/8] Building w522a_bt_reset kernel module…"
  WRESET_SRC="$SCRIPT_DIR/src/w522a_bt_reset"
  cp -f "$WRESET_SRC/aml-sdio.symvers" /tmp/aml-sdio.symvers
  ( cd "$WRESET_SRC" && make clean 2>/dev/null || true; cd "$WRESET_SRC" && make )
  install -d "/lib/modules/${KVER}/extra"
  install -m 0644 "$WRESET_SRC/w522a_bt_reset.ko" "/lib/modules/${KVER}/extra/"

  bold "[3/8] Building hci_uart kernel module…"
  HCI_SRC="$SCRIPT_DIR/src/hci_uart"
  (
    cd "$HCI_SRC"
    rm -f *.o *.ko *.mod *.mod.c .*.cmd modules.order Module.symvers
    make -C "/lib/modules/${KVER}/build" M="$HCI_SRC" \
         CC=gcc-15 KCFLAGS="-Wno-attributes" modules
  )
  install -d "/lib/modules/${KVER}/updates/bluetooth"
  install -m 0644 "$HCI_SRC/hci_uart.ko" "/lib/modules/${KVER}/updates/bluetooth/"

  bold "[4/8] Building BlueALSA from source (SBC + AAC)…"
  BA_SRC="$SCRIPT_DIR/src/bluez-alsa"
  apt-get remove -y bluez-alsa-utils >/dev/null 2>&1 || true
  (
    cd "$BA_SRC"
    [ -f configure ] || autoreconf --install --force
    ./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var \
                --enable-aac --enable-systemd --enable-cli --enable-aplay \
                --with-systemdsystemunitdir=/usr/lib/systemd/system
    make -j"$(nproc)"
    make install
  )

  bold "[5/8] Installing firmware + DTB…"
  install_firmware_and_dtb

  bold "[6/8] Autoload + systemd overrides…"
  install_autoload

  bold "[7/8] Live load…"
  live_load

  bold "[8/8] Done."
}

case "$MODE" in
  binary) do_binary ;;
  source) do_source ;;
esac

echo
green ">>> Installed."
green ">>> Usage:"
echo  "    bluetoothctl                # then: scan on / pair MAC / trust MAC / connect MAC"
echo  "    bluealsa-cli list-pcms"
echo  "    bluealsa-cli codec /org/bluealsa/hci0/dev_<MAC>/a2dpsrc/sink AAC"
echo  "    mpg123 -o alsa -a \"bluealsa:DEV=<MAC>\" /path/to/track.mp3"
if [ "$NEED_REBOOT" = "1" ]; then
  echo
  warn "*** DTB was updated — REBOOT to activate the new device tree. ***"
fi
