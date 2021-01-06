ifneq ($(KERNELRELEASE),)
include Kbuild
else

# KDIR ?= /lib/modules/`uname -r`/build
KDIR ?= ~/dev/hpd2/linux-xlnx
export ARCH = arm
export CROSS_COMPILE ?= arm-none-eabi-

default:
	$(MAKE) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KDIR) M=$$PWD

clean:
	$(MAKE) -C $(KDIR) M=$$PWD $@

endif