# Standalone rebuild of hci_uart with Amlogic AML protocol enabled.
# hci_aml.c uses w1_wifi_bt_try_claim/release exported by aml_sdio.ko,
# so KBUILD_EXTRA_SYMBOLS must point to the aml_sdio Module.symvers.
W1_SYMVERS ?= /tmp/aml-sdio.symvers
KBUILD_EXTRA_SYMBOLS := $(W1_SYMVERS)

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

obj-m += hci_uart.o
hci_uart-y := hci_ldisc.o hci_serdev.o hci_h4.o hci_bcsp.o hci_ll.o hci_ath.o hci_h5.o hci_ag6xx.o hci_mrvl.o hci_aml.o
ccflags-y += -DCONFIG_BT_HCIUART_SERDEV -DCONFIG_BT_HCIUART_H4 -DCONFIG_BT_HCIUART_BCSP -DCONFIG_BT_HCIUART_LL -DCONFIG_BT_HCIUART_ATH3K -DCONFIG_BT_HCIUART_3WIRE -DCONFIG_BT_HCIUART_AG6XX -DCONFIG_BT_HCIUART_MRVL -DCONFIG_BT_HCIUART_AML -UCONFIG_BT_HCIUART_QCA -UCONFIG_BT_HCIUART_NOKIA

all:
	$(MAKE) -C $(KDIR) M=$(PWD) CC=gcc-15 KCFLAGS=" -Wno-attributes" KBUILD_EXTRA_SYMBOLS=$(KBUILD_EXTRA_SYMBOLS) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
