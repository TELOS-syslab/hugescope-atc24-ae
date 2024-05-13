CONFIG_MODULE_SIG=n
obj-m := switch.o
CURRENT_PATH := $(shell pwd)
LINUX_KERNEL_PATH := ../linux-5.4.142-host/
KBUILD_EXTRA_SYMBOLS = ../linux-5.4.142-host/arch/x86/kvm/Module.symvers

all:
	$(MAKE) -C $(LINUX_KERNEL_PATH) M=$(CURRENT_PATH) $(KBUILD_EXTRA_SYMBOLS) modules

clean:
	rm *.ko
	rm *.o
	rm *.a
	rm *.mod*
	rm modules.order
	rm Module.symvers