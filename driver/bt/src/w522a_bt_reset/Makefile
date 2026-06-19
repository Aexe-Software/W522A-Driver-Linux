obj-m += w522a_bt_reset.o
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
KBUILD_EXTRA_SYMBOLS := /tmp/aml-sdio.symvers

all:
	$(MAKE) -C $(KDIR) M=$(PWD) CC=gcc-15 KCFLAGS=" -Wno-attributes" KBUILD_EXTRA_SYMBOLS=$(KBUILD_EXTRA_SYMBOLS) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
