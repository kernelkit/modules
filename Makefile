KDIR ?= /lib/modules/$(shell uname -r)/build
MODCONF ?= $(if $(MO),$(MO),.)/.config

-include $(MODCONF)
export $(filter MOD_%,$(.VARIABLES))

obj-y := drivers/

.PHONY: all install clean menuconfig allmodconfig

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

install: all
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

KCONFIG_ENV := CONFIG_=MOD_ KCONFIG_CONFIG=$(MODCONF)

menuconfig:
	$(KCONFIG_ENV) kconfig-mconf Kconfig

allmodconfig:
	$(KCONFIG_ENV) kconfig-conf --allmodconfig Kconfig
