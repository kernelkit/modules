KDIR ?= /lib/modules/$(shell uname -r)/build
MODCONF ?= $(if $(MO),$(MO),.)/.config

-include $(MODCONF)
export $(filter MOD_%,$(.VARIABLES))

obj-y := drivers/

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

install: all
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

menuconfig:
	CONFIG_=MOD_ \
	KCONFIG_CONFIG=$(MODCONF) \
		kconfig-mconf Kconfig
