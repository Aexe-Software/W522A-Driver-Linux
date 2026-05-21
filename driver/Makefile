# ============================================================
# w1-aml WiFi driver — Armbian / mainline kernel 6.6+ port
# Chip: Amlogic W155S1 / Fn-Link K255B-SR on X96 Max Plus
#
# Usage:
#   make                       # build with running kernel
#   make KDIR=/path/to/kernel  # build against specific kernel
#   sudo make install          # install modules + firmware
# ============================================================

KDIR ?= /lib/modules/$(shell uname -r)/build

# --- Architecture ---
ARCH     ?= arm64
CROSS_COMPILE ?=
BUILD_CC ?= $(shell if command -v gcc-15 >/dev/null 2>&1; then echo gcc-15; elif command -v gcc-14 >/dev/null 2>&1; then echo gcc-14; else echo gcc; fi)

# --- Module names ---
TARGET_SDIO  = aml_sdio
TARGET_VMAC  = vlsicomm

# ============================================================
# Autodetect kernel API variations
# These checks grep the actual installed kernel headers so the
# driver builds correctly on ANY kernel version without manual
# editing.
# ============================================================

# ============================================================
# kbuild context (KERNELRELEASE is set by kbuild)
# Use $(src) — absolute path to module source dir, correctly
# set by kbuild regardless of where make was invoked.
# $(shell pwd) DOES NOT WORK here because kbuild re-invokes
# this Makefile from the kernel source directory.
# ============================================================
ifneq ($(KERNELRELEASE),)

VMAC_DIR := $(src)/vmac
COMM_DIR := $(src)/common

ccflags-y += -I$(VMAC_DIR) -I$(VMAC_DIR)/w1_sdio -I$(COMM_DIR)
subdir-ccflags-y += -I$(VMAC_DIR) -I$(VMAC_DIR)/w1_sdio -I$(COMM_DIR)
EXTRA_CFLAGS += -I$(VMAC_DIR) -I$(VMAC_DIR)/w1_sdio -I$(COMM_DIR)
# ---- Kernel API autodetect (runs in kbuild context with correct headers) ----
# $(src) points to our module dir; KDIR is the kernel headers dir.
# These shell commands grep the actual installed cfg80211.h to detect
# which function signature this specific kernel provides.
CFG80211_H_KB := $(srctree)/include/net/cfg80211.h

# cfg80211_ch_switch_notify: has the puncturing argument (upstream 6.3+,
# NOT in some vendor builds). The exact argument name has varied across
# kernel releases:
#   - 6.3..6.4:  u16 punctured
#   - 6.5+:      u16 punct_bitmap
# Match on the common "punct" prefix so both spellings are detected.
# v15p: pre-fix the grep only checked for "punctured" and missed
# punct_bitmap (6.5+), which caused the compat wrapper in
# wifi_cfg80211_compat.h to call cfg80211_ch_switch_notify() with one
# argument too few and the build failed on Armbian 6.5/6.6 kernels.
CH_SWITCH_NOTIFY_PROTO_KB := $(shell grep -A4 '^void cfg80211_ch_switch_notify' $(CFG80211_H_KB) 2>/dev/null)
CH_SWITCH_PUNCTURED_KB := $(shell printf '%s\n' '$(CH_SWITCH_NOTIFY_PROTO_KB)' | grep -q 'punct' && echo 1 || echo 0)
ifneq ($(CH_SWITCH_PUNCTURED_KB),0)
EXTRA_CFLAGS += -DAML_CFG80211_CSN_HAS_PUNCTURED
endif

CH_SWITCH_LINKID_KB := $(shell printf '%s\n' '$(CH_SWITCH_NOTIFY_PROTO_KB)' | grep -q 'link_id' && echo 1 || echo 0)
ifneq ($(CH_SWITCH_LINKID_KB),0)
EXTRA_CFLAGS += -DAML_CFG80211_CSN_HAS_LINK_ID
endif

CH_SWITCH_STARTED_PROTO_KB := $(shell grep -A5 '^void cfg80211_ch_switch_started_notify' $(CFG80211_H_KB) 2>/dev/null)
CH_SWITCH_ST_PUNCTURED_KB := $(shell printf '%s\n' '$(CH_SWITCH_STARTED_PROTO_KB)' | grep -q 'punct' && echo 1 || echo 0)
ifneq ($(CH_SWITCH_ST_PUNCTURED_KB),0)
EXTRA_CFLAGS += -DAML_CFG80211_CSSN_HAS_PUNCTURED
endif

CH_SWITCH_ST_LINKID_KB := $(shell printf '%s\n' '$(CH_SWITCH_STARTED_PROTO_KB)' | grep -q 'link_id' && echo 1 || echo 0)
ifneq ($(CH_SWITCH_ST_LINKID_KB),0)
EXTRA_CFLAGS += -DAML_CFG80211_CSSN_HAS_LINK_ID
endif

CH_SWITCH_QUIET_KB := $(shell printf '%s\n' '$(CH_SWITCH_STARTED_PROTO_KB)' | grep -q 'quiet' && echo 1 || echo 0)
ifneq ($(CH_SWITCH_QUIET_KB),0)
EXTRA_CFLAGS += -DAML_CFG80211_CSSN_HAS_QUIET
endif

# ---- End autodetect ----

# Non-Amlogic platform: generic kernel, no Amlogic BSP
EXTRA_CFLAGS += -DNOT_AMLOGIC_PLATFORM
EXTRA_CFLAGS += -DLINUX_PLATFORM
EXTRA_CFLAGS += -DSDIO_BUILD_IN
EXTRA_CFLAGS += -DCONFIG_SDIO_IF
EXTRA_CFLAGS += -DHAL_FPGA_VER
EXTRA_CFLAGS += -DPROJECT_W1
EXTRA_CFLAGS += -DCONFIG_AML_CFG80211
EXTRA_CFLAGS += -DCONFIG_MAC_SUPPORT
EXTRA_CFLAGS += -DWIFIMAC_PATH=\"/etc/aml-wifi/wifimac.txt\"
EXTRA_CFLAGS += -DINTER_BEACON
EXTRA_CFLAGS += -DSRAM_CONCURRENT
EXTRA_CFLAGS += -DFW_RF_CALIBRATION
EXTRA_CFLAGS += -DAML_WPA3
EXTRA_CFLAGS += -DDYNAMIC_BW
EXTRA_CFLAGS += -DCONFIG_AML_USE_STATIC_BUF
EXTRA_CFLAGS += -DFPGA
EXTRA_CFLAGS += -DWITH_RF
EXTRA_CFLAGS += -DRF_T9026
EXTRA_CFLAGS += -DUSE_T902X_RF
EXTRA_CFLAGS += -DSYSTEM64
# IRQ mode: use SDIO interrupt (not GPIO) on Armbian — suppresses -Wundef
EXTRA_CFLAGS += -DUSE_SDIO_IRQ=1
EXTRA_CFLAGS += -DUSE_GPIO_IRQ=0

# Suppress warnings from this out-of-tree driver
EXTRA_CFLAGS += -Wno-enum-conversion -Wno-strict-prototypes
EXTRA_CFLAGS += -Wno-misleading-indentation
EXTRA_CFLAGS += -Wno-unused-but-set-variable
EXTRA_CFLAGS += -Wno-unused-label
# Note: -Wno-macro-redefined and -Wno-extra-tokens are clang-only, removed for GCC
EXTRA_CFLAGS += -Wno-unused-function -Wno-unused-variable
EXTRA_CFLAGS += -Wno-attributes
EXTRA_CFLAGS += -Wno-format
EXTRA_CFLAGS += -Wno-empty-body
EXTRA_CFLAGS += -Wno-missing-prototypes
EXTRA_CFLAGS += -Wno-missing-declarations
EXTRA_CFLAGS += -Wno-cast-function-type
EXTRA_CFLAGS += -Wno-undef
KBUILD_CFLAGS_MODULE += -Wframe-larger-than=2048
KBUILD_CFLAGS_MODULE += -Wlarger-than=1048576
KCFLAGS += -Wno-attributes
ccflags-y += $(EXTRA_CFLAGS)
subdir-ccflags-y += $(EXTRA_CFLAGS)

# ---- Module 1: aml_sdio (low-level SDIO bus layer) --------
obj-m += $(TARGET_SDIO).o
$(TARGET_SDIO)-objs := vmac/w1_sdio/w1_sdio.o

# ---- Module 2: vlsicomm (full WiFi stack) -----------------
obj-m += $(TARGET_VMAC).o

VMAC_SRCS := \
	vmac/aml_static_buf.o \
	vmac/rc80211_minstrel.o \
	vmac/rc80211_minstrel_ht.o \
	vmac/rc80211_minstrel_init.o \
	vmac/wifi_cfg80211.o \
	vmac/wifi_clock_measure.o \
	vmac/wifi_cmd_func.o \
	vmac/wifi_common.o \
	vmac/wifi_debug.o \
	vmac/wifi_drv_capture.o \
	vmac/wifi_drv_config.o \
	vmac/wifi_drv_if.o \
	vmac/wifi_drv_main.o \
	vmac/wifi_drv_power.o \
	vmac/wifi_drv_recv.o \
	vmac/wifi_drv_reg_ops.o \
	vmac/wifi_drv_statistic.o \
	vmac/wifi_drv_timer.o \
	vmac/wifi_drv_uapsd.o \
	vmac/wifi_drv_xmit.o \
	vmac/wifi_hal.o \
	vmac/wifi_hal_cmd.o \
	vmac/wifi_hal_platform.o \
	vmac/wifi_hal_txdesc.o \
	vmac/wifi_hif.o \
	vmac/wifi_iwpriv_cmd.o \
	vmac/wifi_mac_acl.o \
	vmac/wifi_mac_action.o \
	vmac/wifi_mac_amsdu.o \
	vmac/wifi_mac_beacon.o \
	vmac/wifi_mac_chan.o \
	vmac/wifi_mac_encrypt.o \
	vmac/wifi_mac_if.o \
	vmac/wifi_mac_monitor.o \
	vmac/wifi_mac_p2p.o \
	vmac/wifi_mac_pmf.o \
	vmac/wifi_mac_power.o \
	vmac/wifi_mac_rate.o \
	vmac/wifi_mac_recv.o \
	vmac/wifi_mac_sae.o \
	vmac/wifi_mac_scan.o \
	vmac/wifi_mac_sta.o \
	vmac/wifi_mac_xmit.o \
	vmac/wifi_phy.o \
	vmac/wifi_pkt_desc.o \
	vmac/wifi_pt_if.o \
	vmac/wifi_pt_init.o \
	vmac/wifi_pt_network.o \
	vmac/wifi_rate_ctrl.o \
	vmac/wifi_sdio.o \
	vmac/wifi_share_fifo.o \
	vmac/wifi_skbbuf.o \
	vmac/crypto/aes-ccm.o \
	vmac/crypto/aes-ctr.o \
	vmac/crypto/aes-gcm.o \
	vmac/crypto/aes-internal-enc.o \
	vmac/crypto/aes-internal.o \
	vmac/crypto/aes-omac1.o \
	vmac/crypto/aes-siv.o \
	vmac/crypto/aml_crypto_wrap.o \
	vmac/crypto/ccmp.o \
	vmac/crypto/sha256-internal.o \
	vmac/crypto/sha256-prf.o \
	vmac/crypto/sha256.o

$(TARGET_VMAC)-objs := $(VMAC_SRCS)

else
# ============================================================
# Host (non-kbuild) targets
# ============================================================
M_DIR := $(CURDIR)

all:
	@echo "Building w1-aml WiFi driver for kernel $(shell uname -r)..."
	$(MAKE) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) \
		-C $(KDIR) M=$(M_DIR) CC=$(BUILD_CC) KCFLAGS="$(KCFLAGS) -Wno-attributes" modules

clean:
	$(MAKE) -C $(KDIR) M=$(M_DIR) clean
	find $(M_DIR) \( -name "*.o" -o -name "*.ko" -o -name "*.mod.c" \
		-o -name "*.mod" -o -name "modules.order" \
		-o -name "Module.symvers" -o -name ".*.cmd" \) \
		-delete 2>/dev/null; true

install: all
	@echo "Installing modules..."
	install -d /lib/modules/$(shell uname -r)/kernel/drivers/net/wireless/aml-w1/
	install -m 644 aml_sdio.ko vlsicomm.ko \
		/lib/modules/$(shell uname -r)/kernel/drivers/net/wireless/aml-w1/
	@echo ""
	@echo "Installing firmware / RF config..."
	install -d /lib/firmware/aml/
	install -d /etc/aml-wifi/
	install -m 644 vmac/aml_wifi_rf_fn_link.txt /lib/firmware/aml/aml_wifi_rf_fn_link.txt
	install -m 644 vmac/aml_wifi_rf_0333.txt /lib/firmware/aml/aml_wifi_rf_0333.txt
	install -m 644 vmac/aml_wifi_rf_fn_link.txt /lib/firmware/aml/aml_wifi_rf.txt
	install -m 644 vmac/aml_wifi_rf_fn_link.txt /etc/aml-wifi/aml_wifi_rf.txt
	install -m 644 vmac/aml_wifi_drv_cfg_0.conf /etc/aml-wifi/
	@echo ""
	@echo "Installing auto-load config (modprobe.d / modules-load.d)..."
	install -d /etc/modprobe.d/ /etc/modules-load.d/
	install -m 644 install_files/aml-w1.conf    /etc/modprobe.d/aml-w1.conf
	install -m 644 install_files/aml-w1.modules /etc/modules-load.d/aml-w1.conf
	@echo ""
	@echo "Refreshing module dependencies..."
	depmod -a $(shell uname -r)
	@echo ""
	@echo "Done! Reboot — the 'w522a' interface will come up automatically"
	@echo "  on SDIO hotplug (no wlan0/wlan1 collision with other drivers)."
	@echo "Or to load now without reboot:"
	@echo "  sudo modprobe aml_sdio"

uninstall:
	@echo "Removing modules..."
	rm -rf /lib/modules/$(shell uname -r)/kernel/drivers/net/wireless/aml-w1/
	rm -f /etc/modprobe.d/aml-w1.conf /etc/modules-load.d/aml-w1.conf
	rm -rf /etc/aml-wifi/
	rm -f /lib/firmware/aml/aml_wifi_rf_fn_link.txt
	rm -f /lib/firmware/aml/aml_wifi_rf_0333.txt
	rm -f /lib/firmware/aml/aml_wifi_rf.txt
	depmod -a $(shell uname -r)
	@echo "Done. Reboot to fully unload."

endif
